/* Sonido: lo que en la consola hace el programa del ARM7 (la parte de sonido del SDK de Nintendo),
 * hecho aqui directamente, y la salida por los altavoces del PC.
 *
 * El juego (ARM9) manda listas de ordenes de sonido (snd_command_list, desde arm7.c). Con ellas:
 *  - Reproductor de secuencias: partituras SSEQ (tipo MIDI) con los instrumentos del banco SBNK y las
 *    muestras de los archivos de ondas SWAR, que el juego ya ha cargado en la memoria. Envolventes de
 *    volumen (ataque, caida, sostenido, liberacion), panorama, tono, vibrato, bucles, llamadas y variables.
 *  - Canales sueltos que el juego maneja directamente (musica en "streaming"): PCM de 8 y 16 bits y ADPCM,
 *    cuadradas y ruido, con sus temporizadores, y alarmas que avisan al juego para que rellene el bufer.
 *  - Los 16 canales de la consola se mezclan a 32.728 Hz en estereo y salen por waveOut en otro hilo.
 *  - Zona compartida con el juego (orden 29): estado de reproductores y canales, variables y contadores.
 *
 * El formato de las ordenes y de los datos es el de la libreria SND del SDK (ver notes/sound.md). Lo que
 * no se hace: efectos con captura (reverberacion): el sonido sale sin ese efecto.
 * NFS_SOUND=0 quita el sonido; NFS_VOLUME=0..100 (por defecto 100). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <mmsystem.h>

#include "sys.h"

#define OUT_RATE 32728
#define SND_FRAMES_PER_SEC 192.0 /* el ARM7 procesa el sonido cada ~5,2 ms */
#define ARM7_CLOCK 33513982.0
#define CH_FREQ_BASE 16756991.0  /* frecuencia de un canal = esto / periodo */
#define OS_TICK_HZ (ARM7_CLOCK / 64.0)
#define ENV_MIN (-92544)

void arm7_send(uint32_t tag, uint32_t data); /* arm7.c: mensaje al ARM9 */

static int rd8(uint32_t a) { return (int)mem_r8(a); }
static int rd16(uint32_t a) { return (int)mem_r16(a); }
static uint32_t rd32(uint32_t a) { return mem_r32(a); }

/* ---- tablas ------------------------------------------------------------------------------------ */
/* Volumenes del SDK: en DECIMAS de decibelio (-723 = -72,3 dB = silencio). La envolvente va en esas mismas
 * unidades por 128. (Antes se tomaban como decibelios: envolventes 10 veces mas rapidas y sostenidos mal.) */
static int db_square(int v) { /* 10 * 40*log10(v/127), como SNDi_DecibelSquareTable */
    if (v <= 0) return -723;
    if (v >= 127) return 0;
    int d = (int)floor(400.0 * log10(v / 127.0) + 0.5);
    return d < -723 ? -723 : d;
}
static int attack_coeff(int a) {
    static const int t[19] = {0, 1, 5, 14, 26, 38, 51, 63, 73, 84, 92, 100, 109, 116, 123, 127, 132, 137, 143};
    if (a < 109) return 255 - a;
    return t[127 - a < 19 ? 127 - a : 18];
}
static int decay_coeff(int v) {
    if (v >= 127) return 0xFFFF;
    if (v == 126) return 0x3C00;
    if (v < 50) return v * 2 + 1;
    return 0x1E00 / (126 - v);
}

/* ---- canales ------------------------------------------------------------------------------------ */
typedef struct Channel {
    int active, kind;        /* kind: 0 PCM, 1 PSG (cuadrada), 2 ruido */
    int fmt, loop;           /* fmt: 0 PCM8, 1 PCM16, 2 ADPCM; loop: 1 bucle, 2 una vez */
    uint32_t data;           /* direccion de las muestras */
    uint32_t loop_start, loop_len; /* en muestras */
    uint32_t total;          /* muestras totales (loop_start + loop_len) */
    double period;           /* periodo del temporizador (16,756991 MHz / periodo = muestras por segundo) */
    double pos;              /* posicion en muestras */
    int vol, shift, pan;     /* registro: 0-127, divisor, 0-127 */
    int duty;                /* PSG */
    uint32_t lfsr;           /* ruido */
    /* ADPCM: estado en la posicion actual y en el inicio del bucle */
    int ad_sample, ad_index, ad_pos, ad_ls_sample, ad_ls_index, ad_ls_saved;
    double cur, prev;        /* muestras decodificadas para interpolar */
    int cur_pos;
    /* dirigido por una secuencia */
    int seq;                 /* 1 = lo maneja el reproductor */
    int player, track, prio, key, base_key, velocity;
    int length;              /* ticks que le quedan a la nota; -1 sin limite */
    int env_state;           /* 0 ataque, 1 caida, 2 sostenido, 3 liberacion */
    int env, attack, decay, sustain_lvl, release;
    double base_period;
    double amp_l, amp_r;     /* ganancia ya calculada para la mezcla */
    int age;
} Channel;

static Channel ch[16];
static uint32_t locked_mask, capture_active;
static int master_volume = 127;
static CRITICAL_SECTION snd_lock;

static const int8_t ima_index[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t ima_step[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130,
    143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282,
    1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630,
    9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

static void channel_start(Channel *c) {
    c->active = 1;
    c->pos = 0;
    c->cur_pos = -1;
    c->cur = c->prev = 0;
    c->lfsr = 0x7FFF;
    if (c->kind == 0 && c->fmt == 2) { /* ADPCM: cabecera de 4 bytes */
        uint32_t h = rd32(c->data);
        c->ad_sample = (int16_t)(h & 0xFFFF);
        c->ad_index = (h >> 16) & 0x7F;
        if (c->ad_index > 88) c->ad_index = 88;
        c->ad_pos = 0;
        c->ad_ls_saved = 0;
    }
}

/* Muestra n del canal (entero), -1..1 */
static double channel_sample(Channel *c, int n) {
    if (c->fmt == 0) return (int8_t)rd8(c->data + n) / 128.0;
    if (c->fmt == 1) return (int16_t)rd16(c->data + n * 2) / 32768.0;
    /* ADPCM: se decodifica hacia delante desde donde se quedo (o desde el inicio del bucle) */
    if (n < c->ad_pos) {
        if (c->ad_ls_saved && n >= (int)c->loop_start) {
            c->ad_pos = c->loop_start; c->ad_sample = c->ad_ls_sample; c->ad_index = c->ad_ls_index;
        } else {
            uint32_t h = rd32(c->data);
            c->ad_sample = (int16_t)(h & 0xFFFF); c->ad_index = (h >> 16) & 0x7F; c->ad_pos = 0;
            if (c->ad_index > 88) c->ad_index = 88;
        }
    }
    while (c->ad_pos < n) {
        if (c->ad_pos == (int)c->loop_start && !c->ad_ls_saved) {
            c->ad_ls_sample = c->ad_sample; c->ad_ls_index = c->ad_index; c->ad_ls_saved = 1;
        }
        int b = rd8(c->data + 4 + (c->ad_pos >> 1));
        int nib = (c->ad_pos & 1) ? b >> 4 : b & 15;
        int step = ima_step[c->ad_index], diff = step >> 3;
        if (nib & 1) diff += step >> 2;
        if (nib & 2) diff += step >> 1;
        if (nib & 4) diff += step;
        if (nib & 8) { c->ad_sample -= diff; if (c->ad_sample < -32767) c->ad_sample = -32767; }
        else { c->ad_sample += diff; if (c->ad_sample > 32767) c->ad_sample = 32767; }
        c->ad_index += ima_index[nib];
        if (c->ad_index < 0) c->ad_index = 0;
        if (c->ad_index > 88) c->ad_index = 88;
        c->ad_pos++;
    }
    return c->ad_sample / 32768.0;
}

/* Siguiente muestra mezclable del canal; avanza su posicion. 0 si acaba */
static double channel_next(Channel *c, double step) {
    if (c->kind == 1) { /* cuadrada: 8 pasos, el ciclo de trabajo dice cuantos estan arriba */
        double v = ((int)c->pos & 7) <= c->duty ? 1.0 : -1.0;
        c->pos += step;
        if (c->pos >= 8) c->pos -= 8 * floor(c->pos / 8);
        return v * 0.5;
    }
    if (c->kind == 2) { /* ruido */
        double v = (c->lfsr & 1) ? -0.5 : 0.5;
        c->pos += step;
        while (c->pos >= 1) {
            c->pos -= 1;
            int carry = c->lfsr & 1;
            c->lfsr >>= 1;
            if (carry) c->lfsr ^= 0x6000;
        }
        return v;
    }
    int n = (int)c->pos;
    if (n >= (int)c->total) {
        if (c->loop == 1 && c->loop_len) {
            while (n >= (int)c->total) { c->pos -= c->loop_len; n = (int)c->pos; }
        } else {
            c->active = 0;
            return 0;
        }
    }
    if (n != c->cur_pos) {
        c->prev = c->cur_pos == n - 1 ? c->cur : channel_sample(c, n > 0 ? n - 1 : 0);
        c->cur = channel_sample(c, n);
        c->cur_pos = n;
    }
    double f = c->pos - n, v = c->prev + (c->cur - c->prev) * f; /* interpolacion lineal */
    c->pos += step;
    return v;
}

/* ---- reproductor de secuencias ------------------------------------------------------------------- */
typedef struct Track {
    int active, pos, wait, prg, vol, expr, pan, ext_pan, transpose, bend, bend_range, prio;
    int note_wait, tie, porta, porta_key, porta_time, mute;
    int attack, decay, sustain, release; /* 0xFF = los del instrumento */
    int mod_depth, mod_speed, mod_type, mod_range, mod_delay, mod_counter, mod_delay_left;
    int ext_fader, ext_pitch;
    int stack[3], loop_count[3], sp;
    int cmp; /* resultado de la ultima comparacion (orden A2) */
    int tie_channel;
} Track;

typedef struct Player {
    int active, prepared, paused, prio, volume, ext_fader;
    int finishing;          /* la partitura ha terminado: se espera a que acaben sus notas */
    uint32_t seq_base, bank;
    int tempo, tempo_ratio, tempo_counter;
    int track_no[16];       /* pistas del reproductor (indices en tracks[]), -1 libre */
    uint32_t alloc_mask;    /* canales que puede usar */
    int16_t local_var[16];
    uint32_t ticks;
} Player;

static Player players[16];
static Track tracks[32];
static int16_t global_var[16];
static uint32_t shared_work;

static void stop_player(int p);

static int track_alloc(void) {
    for (int i = 0; i < 32; i++)
        if (!tracks[i].active) return i;
    return -1;
}

static void track_init(Track *t, int pos) {
    memset(t, 0, sizeof *t);
    t->active = 1;
    t->pos = pos;
    t->vol = 127; t->expr = 127; t->bend_range = 2; t->prio = 64; t->note_wait = 1;
    t->attack = t->decay = t->sustain = t->release = 0xFF;
    t->mod_speed = 16; t->mod_range = 1;
    t->tie_channel = -1;
}

/* Instrumento (SBNK) para un programa y una tecla: devuelve el tipo y la direccion de sus parametros */
static int find_instrument(uint32_t bank, int prg, int key, uint32_t *param) {
    if (!bank) return 0;
    static int dumped;
    if (getenv("NFS_SND_LOG") && dumped++ < 2) {
        printf("[sonido] banco %08x prg %d tecla %d:", bank, prg, key);
        for (int i = 0; i < 0x60; i += 4) printf(" %08x", rd32(bank + i));
        printf("%c", 10);
    }
    uint32_t count = rd32(bank + 0x38); /* VERIFICADO con el banco en memoria */
    if ((uint32_t)prg >= count) return 0;
    uint32_t off = rd32(bank + 0x3C + prg * 4);
    int type = off & 0xFF;
    uint32_t a = bank + (off >> 8);
    if (type == 16) { /* bateria: un instrumento por tecla */
        int lo = rd8(a), hi = rd8(a + 1);
        if (key < lo || key > hi) return 0;
        uint32_t e = a + 2 + (key - lo) * 12;
        *param = e + 2;
        return rd16(e) & 0xFF;
    }
    if (type == 17) { /* zonas por tecla */
        int i = 0;
        while (i < 8 && rd8(a + i) && key > rd8(a + i)) i++;
        if (i >= 8) return 0;
        uint32_t e = a + 8 + i * 12;
        *param = e + 2;
        return rd16(e) & 0xFF;
    }
    *param = a;
    return type;
}

static Channel *alloc_channel(Player *pl, int kind, int prio) {
    uint32_t allowed = pl->alloc_mask & ~locked_mask;
    if (kind == 1) allowed &= 0x3F00;
    if (kind == 2) allowed &= 0xC000;
    Channel *best = 0;
    for (int i = 0; i < 16; i++) { /* libre primero; si no, el de menos prioridad (o el que se esta apagando) */
        if (!(allowed & (1u << i))) continue;
        Channel *c = &ch[i];
        if (!c->active) return c;
        if (!c->seq) continue;
        int score = c->prio - (c->env_state == 3 ? 256 : 0);
        if (!best || score < best->prio - (best->env_state == 3 ? 256 : 0) || (score == best->prio && c->age > best->age)) best = c;
    }
    if (best && (best->prio <= prio || best->env_state == 3)) return best;
    return 0;
}

static int dbg_notes, dbg_fail, dbg_nochan, dbg_nowave, dbg_ok;
static uint32_t dbg_wa;
static void note_on(Player *pl, int pi, int ti, Track *t, int key, int vel, int len) {
    if (t->mute) return;
    dbg_notes++;
    int k = key + t->transpose;
    if (k < 0) k = 0;
    if (k > 127) k = 127;
    if (t->tie && t->tie_channel >= 0 && ch[t->tie_channel].active && ch[t->tie_channel].seq) { /* ligado: cambia el tono */
        Channel *c = &ch[t->tie_channel];
        c->key = k; c->velocity = vel; c->length = len > 0 ? len : -1;
        return;
    }
    uint32_t param = 0;
    int type = find_instrument(pl->bank, t->prg, k, &param);
    if (!type || type > 4) { dbg_fail++; return; }
    int kind = type == 2 ? 1 : type == 3 ? 2 : 0;
    Channel *c = alloc_channel(pl, kind, pl->prio + t->prio);
    if (!c) { dbg_nochan++; return; }
    memset(c, 0, sizeof *c);
    c->kind = kind;
    int wave = rd16(param), arc = rd16(param + 2);
    c->base_key = rd8(param + 4);
    int a = t->attack != 0xFF ? t->attack : rd8(param + 5), d = t->decay != 0xFF ? t->decay : rd8(param + 6);
    int s = t->sustain != 0xFF ? t->sustain : rd8(param + 7), r = t->release != 0xFF ? t->release : rd8(param + 8);
    int ipan = rd8(param + 9);
    if (kind == 0) {
        uint32_t wa = arc < 4 ? rd32(pl->bank + 0x18 + arc * 4) : 0; /* archivo de ondas (lo enlaza el juego) */
        dbg_wa = wa;
        if (wa < 0x02000000u || wa >= 0x02400000u) { dbg_nowave++; return; }
        if ((uint32_t)wave >= rd32(wa + 0x38)) { dbg_nowave++; return; }
        uint32_t w = wa + rd32(wa + 0x3C + wave * 4);
        c->fmt = rd8(w);
        c->loop = rd8(w + 1) ? 1 : 2;
        c->base_period = rd16(w + 4);
        int unit = c->fmt == 0 ? 4 : c->fmt == 1 ? 2 : 8; /* muestras por palabra */
        c->loop_start = rd16(w + 6) * unit - (c->fmt == 2 ? 8 : 0);
        c->loop_len = rd32(w + 8) * unit;
        c->total = c->loop_start + c->loop_len;
        c->data = w + 12;
        if (!c->base_period) return;
    } else if (kind == 1) {
        c->duty = wave;
        c->base_period = CH_FREQ_BASE / (261.6256 * 8); /* do central en la tecla 60 */
        c->base_key = 60;
    } else {
        c->base_period = CH_FREQ_BASE / 8000.0;
        c->base_key = 60;
    }
    c->seq = 1; c->player = pi; c->track = ti; c->prio = pl->prio + t->prio;
    c->key = k; c->velocity = vel; c->length = len > 0 ? len : -1;
    c->attack = attack_coeff(a); c->decay = decay_coeff(d); c->release = decay_coeff(r);
    c->sustain_lvl = db_square(s) << 7;
    c->env = ENV_MIN; c->env_state = 0;
    c->pan = ipan; /* desplazamiento del instrumento; se suma al de la pista al actualizar */
    channel_start(c);
    dbg_ok++;
    if (t->tie) t->tie_channel = (int)(c - ch);
}

static int read_var(uint32_t *p) { /* longitud variable tipo MIDI */
    int v = 0;
    for (int i = 0; i < 4; i++) {
        int b = rd8((*p)++);
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static int16_t *var_ref(Player *pl, int n) { return n < 16 ? &pl->local_var[n] : &global_var[n & 15]; }

/* Un tick de una pista. Devuelve 0 si la pista termina */
static int track_tick(int pi, int ti) {
    Player *pl = &players[pi];
    Track *t = &tracks[ti];
    if (t->wait > 0) { t->wait--; if (t->wait > 0) return 1; }
    for (int guard = 0; guard < 1000; guard++) {
        uint32_t p = pl->seq_base + t->pos;
        int cmd = rd8(p++);
        int arg_mode = 0, rnd_lo = 0, rnd_hi = 0, var_no = 0, cond = 1;
        if (cmd == 0xA2) { cond = t->cmp; cmd = rd8(p++); }
        if (cmd == 0xA0) { arg_mode = 1; cmd = rd8(p++); }
        else if (cmd == 0xA1) { arg_mode = 2; cmd = rd8(p++); }
#define LASTARG(normal)                                                                                 \
    (arg_mode == 1 ? (rnd_lo = (int16_t)rd16(p), rnd_hi = (int16_t)rd16(p + 2), p += 4,                \
                      rnd_lo + rand() % (rnd_hi - rnd_lo + 1 > 0 ? rnd_hi - rnd_lo + 1 : 1))              \
     : arg_mode == 2 ? (var_no = rd8(p), p += 1, *var_ref(pl, var_no))                                   \
                     : (normal))
        if (cmd < 0x80) { /* nota */
            int vel = rd8(p++);
            int len = LASTARG(read_var(&p));
            t->pos = p - pl->seq_base;
            if (cond) {
                note_on(pl, pi, ti, t, cmd, vel, len);
                if (t->note_wait) { t->wait = len; if (len > 0) return 1; }
            }
            continue;
        }
        int a;
        switch (cmd) {
        case 0x80: a = LASTARG(read_var(&p)); t->pos = p - pl->seq_base; if (cond && a > 0) { t->wait = a; return 1; } continue;
        case 0x81: a = LASTARG(read_var(&p)); if (cond) t->prg = a; break;
        case 0x93: { int n = rd8(p); uint32_t off = rd32(p + 1) & 0xFFFFFF; p += 4;
            if (cond && n < 16 && pl->track_no[n] < 0) { int nt = track_alloc(); if (nt >= 0) { track_init(&tracks[nt], off); pl->track_no[n] = nt; } }
            break; }
        case 0x94: { uint32_t off = rd32(p) & 0xFFFFFF; p += 3; if (cond) { t->pos = off; continue; } break; }
        case 0x95: { uint32_t off = rd32(p) & 0xFFFFFF; p += 3;
            if (cond && t->sp < 3) { t->stack[t->sp] = p - pl->seq_base; t->loop_count[t->sp] = 0; t->sp++; t->pos = off; continue; } break; }
        case 0xFD: if (cond && t->sp > 0) { t->sp--; t->pos = t->stack[t->sp]; continue; } break;
        case 0xD4: a = LASTARG(rd8(p++)); if (cond && t->sp < 3) { t->stack[t->sp] = p - pl->seq_base; t->loop_count[t->sp] = a; t->sp++; } break;
        case 0xFC: if (cond && t->sp > 0) {
                int c2 = t->loop_count[t->sp - 1];
                if (c2 == 0 || --t->loop_count[t->sp - 1] > 0) { t->pos = t->stack[t->sp - 1]; continue; }
                t->sp--;
            } break;
        case 0xFE: p += 2; break; /* pistas que usara la secuencia: no hace falta */
        case 0xFF: return 0;
        case 0xC0: a = LASTARG(rd8(p++)); if (cond) t->pan = a - 64; break;
        case 0xC1: a = LASTARG(rd8(p++)); if (cond) t->vol = a; break;
        case 0xC2: a = LASTARG(rd8(p++)); if (cond) pl->volume = a; break;
        case 0xC3: a = LASTARG((int8_t)rd8(p++)); if (cond) t->transpose = a; break;
        case 0xC4: a = LASTARG((int8_t)rd8(p++)); if (cond) t->bend = a; break;
        case 0xC5: a = LASTARG(rd8(p++)); if (cond) t->bend_range = a; break;
        case 0xC6: a = LASTARG(rd8(p++)); if (cond) t->prio = a; break;
        case 0xC7: a = LASTARG(rd8(p++)); if (cond) t->note_wait = a; break;
        case 0xC8: a = LASTARG(rd8(p++)); if (cond) { t->tie = a; t->tie_channel = -1; } break;
        case 0xC9: a = LASTARG(rd8(p++)); if (cond) { t->porta_key = a; t->porta = 1; } break;
        case 0xCA: a = LASTARG(rd8(p++)); if (cond) t->mod_depth = a; break;
        case 0xCB: a = LASTARG(rd8(p++)); if (cond) t->mod_speed = a; break;
        case 0xCC: a = LASTARG(rd8(p++)); if (cond) t->mod_type = a; break;
        case 0xCD: a = LASTARG(rd8(p++)); if (cond) t->mod_range = a; break;
        case 0xCE: a = LASTARG(rd8(p++)); if (cond) t->porta = a; break;
        case 0xCF: a = LASTARG(rd8(p++)); if (cond) t->porta_time = a; break;
        case 0xD0: a = LASTARG(rd8(p++)); if (cond) t->attack = a; break;
        case 0xD1: a = LASTARG(rd8(p++)); if (cond) t->decay = a; break;
        case 0xD2: a = LASTARG(rd8(p++)); if (cond) t->sustain = a; break;
        case 0xD3: a = LASTARG(rd8(p++)); if (cond) t->release = a; break;
        case 0xD5: a = LASTARG(rd8(p++)); if (cond) t->expr = a; break;
        case 0xD6: p += 1; break; /* imprimir variable (depuracion) */
        case 0xE0: a = LASTARG((int16_t)rd16(p)); if (arg_mode == 0) p += 2; if (cond) t->mod_delay = a; break;
        case 0xE1: a = LASTARG(rd16(p)); if (arg_mode == 0) p += 2; if (cond) pl->tempo = a; break;
        case 0xE3: a = LASTARG((int16_t)rd16(p)); if (arg_mode == 0) p += 2; break; /* barrido de tono: sin hacer */
        default:
            if (cmd >= 0xB0 && cmd <= 0xBD) { /* operaciones con variables */
                int n = rd8(p++);
                int v = LASTARG((int16_t)rd16(p));
                if (arg_mode == 0) p += 2;
                if (!cond) break;
                int16_t *x = var_ref(pl, n);
                switch (cmd) {
                case 0xB0: *x = (int16_t)v; break;
                case 0xB1: *x += v; break;
                case 0xB2: *x -= v; break;
                case 0xB3: *x *= v; break;
                case 0xB4: if (v) *x /= v; break;
                case 0xB5: *x = (int16_t)(v >= 0 ? *x << v : *x >> -v); break;
                case 0xB6: *x = (int16_t)(v >= 0 ? rand() % (v + 1) : -(rand() % (-v + 1))); break;
                case 0xB8: t->cmp = *x == v; break;
                case 0xB9: t->cmp = *x >= v; break;
                case 0xBA: t->cmp = *x > v; break;
                case 0xBB: t->cmp = *x <= v; break;
                case 0xBC: t->cmp = *x < v; break;
                case 0xBD: t->cmp = *x != v; break;
                }
                break;
            }
            return 0; /* orden desconocida: se termina la pista */
        }
        t->pos = p - pl->seq_base;
    }
    return 1;
}

static void stop_player(int pi) {
    Player *pl = &players[pi];
    for (int i = 0; i < 16; i++) {
        if (pl->track_no[i] >= 0) tracks[pl->track_no[i]].active = 0;
        pl->track_no[i] = -1;
    }
    for (int i = 0; i < 16; i++)
        if (ch[i].active && ch[i].seq && ch[i].player == pi) { ch[i].env_state = 3; ch[i].release = 0xFFFF; }
    pl->active = pl->prepared = pl->finishing = 0;
}

static void prepare_player(int pi, uint32_t base, uint32_t offset, uint32_t bank) {
    if (pi < 0 || pi > 15) return;
    if (getenv("NFS_SND_LOG")) {
        static uint32_t seen[64];
        int dup = 0;
        for (int i = 0; i < 64 && seen[i]; i++) dup |= seen[i] == base + offset;
        if (!dup) {
            for (int i = 0; i < 64; i++) if (!seen[i]) { seen[i] = base + offset; break; }
            printf("[sonido] secuencia %08x+%x en reproductor %d:", base, offset, pi);
            for (int i = 0; i < 48; i++) printf(" %02x", rd8(base + offset + i));
            printf("%c", 10);
        }
    }
    stop_player(pi);
    Player *pl = &players[pi];
    pl->seq_base = base;
    pl->bank = bank;
    pl->tempo = 120; pl->tempo_ratio = 256; pl->tempo_counter = 240;
    pl->volume = 127; pl->prio = 64; pl->ext_fader = 0; pl->paused = 0; pl->finishing = 0;
    if (!pl->alloc_mask) pl->alloc_mask = 0xFFFF;
    int t = track_alloc();
    if (t < 0) return;
    track_init(&tracks[t], offset);
    pl->track_no[0] = t;
    pl->prepared = 1;
    pl->ticks = 0;
}

/* ---- cada "fotograma de sonido" (192 por segundo) ------------------------------------------------ */
static void seq_frame(void) {
    for (int pi = 0; pi < 16; pi++) {
        Player *pl = &players[pi];
        if (!pl->active || pl->paused) continue;
        if (pl->finishing) {
            int playing = 0;
            for (int c = 0; c < 16; c++) playing |= ch[c].active && ch[c].seq && ch[c].player == pi;
            if (!playing) { pl->active = pl->prepared = pl->finishing = 0; }
            continue;
        }
        while (pl->tempo_counter >= 240) {
            pl->tempo_counter -= 240;
            int alive = 0;
            for (int i = 0; i < 16; i++) {
                int ti = pl->track_no[i];
                if (ti < 0) continue;
                if (!track_tick(pi, ti)) { tracks[ti].active = 0; pl->track_no[i] = -1; continue; }
                alive = 1;
            }
            /* notas: cuenta atras de su duracion */
            for (int c = 0; c < 16; c++)
                if (ch[c].active && ch[c].seq && ch[c].player == pi && ch[c].length > 0 && --ch[c].length == 0)
                    ch[c].env_state = 3;
            pl->ticks++;
            if (!alive) { pl->finishing = 1; break; } /* como en la consola: las notas siguen hasta acabar */
        }
        pl->tempo_counter += pl->tempo * pl->tempo_ratio / 256;
    }
    /* envolventes y volumen/tono de los canales de secuencia */
    for (int i = 0; i < 16; i++) {
        Channel *c = &ch[i];
        if (!c->active || !c->seq) continue;
        c->age++;
        switch (c->env_state) {
        case 0: c->env = c->env * c->attack / 256; if (c->env >= -64 || c->attack == 0) { c->env = 0; c->env_state = 1; } break;
        case 1: c->env -= c->decay; if (c->env <= c->sustain_lvl) { c->env = c->sustain_lvl; c->env_state = 2; } break;
        case 2: break;
        case 3: c->env -= c->release; if (c->env <= ENV_MIN) { c->active = 0; continue; } break;
        }
        Player *pl = &players[c->player];
        Track *t = &tracks[c->track];
        int pitch = (c->key - c->base_key) * 64 + t->bend * t->bend_range * 64 / 128 + t->ext_pitch;
        double vol_db = db_square(c->velocity) + db_square(t->vol) + db_square(t->expr) + db_square(pl->volume) +
                        t->ext_fader + pl->ext_fader + c->env / 128.0; /* decimas de dB */
        int pan = 64 + t->pan + t->ext_pan + (c->pan - 64);
        if (t->mod_depth) { /* vibrato / tremolo / paneo */
            if (t->mod_delay_left < t->mod_delay) t->mod_delay_left++;
            else {
                t->mod_counter = (t->mod_counter + t->mod_speed * 2) & 0xFFFF;
                double lfo = sin(t->mod_counter / 65536.0 * 2 * 3.14159265) * t->mod_depth * t->mod_range;
                if (t->mod_type == 0) pitch += (int)(lfo * 64 / 128);
                else if (t->mod_type == 1) vol_db += lfo * 10 / 64.0;
                else pan += (int)(lfo / 2);
            }
        }
        if (pan < 0) pan = 0;
        if (pan > 127) pan = 127;
        c->period = c->base_period * pow(2.0, -pitch / 768.0);
        double amp = vol_db <= -720 ? 0 : pow(10.0, vol_db / 200.0);
        c->amp_l = amp * (127 - pan) / 127.0;
        c->amp_r = amp * pan / 127.0;
    }
}

/* ---- alarmas (el juego rellena el bufer de la musica cuando le avisan) ---------------------------- */
typedef struct Alarm {
    int active, armed, id, no;
    double next, period; /* en segundos de juego */
} Alarm;
static Alarm alarms[8];
static double snd_time; /* segundos de juego desde que empezo el sonido */

static void alarms_run(void) {
    for (int i = 0; i < 8; i++) {
        Alarm *al = &alarms[i];
        while (al->active && al->next <= snd_time) {
            arm7_send(7, (uint32_t)(al->no | (al->id << 8)));
            if (al->period <= 0) { al->active = 0; break; }
            al->next += al->period;
        }
    }
}

/* ---- mezcla y salida ------------------------------------------------------------------------------ */
#define RING 16384
static int16_t ring[RING * 2];
static volatile LONG ring_w, ring_r;
static int out_on = -1;
static FILE *wav; /* NFS_WAV=archivo.wav: graba lo que suena (para comprobarlo sin oirlo) */
static uint32_t wav_frames;

static void wav_close(void) {
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
    fclose(wav);
    wav = 0;
}
static double out_gain = 1.0;

static void mix(int frames) {
    for (int f = 0; f < frames; f++) {
        double l = 0, r = 0;
        for (int i = 0; i < 16; i++) {
            Channel *c = &ch[i];
            if (!c->active || c->period <= 0) continue;
            if (capture_active && (i == 1 || i == 3)) { /* repiten lo capturado (efecto): no se oye doble */
                c->pos += CH_FREQ_BASE / c->period / OUT_RATE;
                continue;
            }
            double step = CH_FREQ_BASE / c->period / OUT_RATE;
            double v = channel_next(c, step);
            static int only = -1; /* NFS_SND_ONLY=seq|stream: depuracion, solo una clase de canales */
            if (only < 0) only = !getenv("NFS_SND_ONLY") ? 0 : getenv("NFS_SND_ONLY")[0] == 's' && getenv("NFS_SND_ONLY")[1] == 'e' ? 1 : 2;
            if ((only == 1 && !c->seq) || (only == 2 && c->seq)) continue;
            if (c->seq) { l += v * c->amp_l; r += v * c->amp_r; }
            else {
                static const int div[4] = {1, 2, 4, 16};
                double amp = c->vol / 127.0 / div[c->shift & 3];
                l += v * amp * (127 - c->pan) / 127.0;
                r += v * amp * c->pan / 127.0;
            }
        }
        double mv = master_volume / 127.0 * out_gain * 0.5;
        l *= mv; r *= mv;
        /* limitador suave: con musica, motores y efectos a la vez la suma pasaba de 1 y saturaba */
        l = l > 0.7 ? 0.7 + 0.3 * tanh((l - 0.7) / 0.3) : l < -0.7 ? -0.7 + 0.3 * tanh((l + 0.7) / 0.3) : l;
        r = r > 0.7 ? 0.7 + 0.3 * tanh((r - 0.7) / 0.3) : r < -0.7 ? -0.7 + 0.3 * tanh((r + 0.7) / 0.3) : r;
        int il = (int)(l * 32767), ir = (int)(r * 32767);
        if (il > 32767) il = 32767;
        if (il < -32768) il = -32768;
        if (ir > 32767) ir = 32767;
        if (ir < -32768) ir = -32768;
        if (wav) {
            int16_t smp[2] = {(int16_t)il, (int16_t)ir};
            fwrite(smp, 2, 2, wav);
            wav_frames++;
        }
        if (!out_on) continue;
        LONG w = ring_w;
        if (((w + 1) & (RING - 1)) == (ring_r & (RING - 1))) continue; /* lleno: se descarta */
        ring[(w & (RING - 1)) * 2] = (int16_t)il;
        ring[(w & (RING - 1)) * 2 + 1] = (int16_t)ir;
        InterlockedExchange(&ring_w, (w + 1) & (RING - 1));
    }
}

#define WAVE_BUFS 4
#define WAVE_LEN 1024
static DWORD WINAPI audio_thread(void *arg) {
    (void)arg;
    WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, OUT_RATE, OUT_RATE * 4, 4, 16, 0};
    HANDLE ev = CreateEventA(0, FALSE, FALSE, 0);
    HWAVEOUT wo;
    if (waveOutOpen(&wo, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        fprintf(stderr, "[sonido] no se puede abrir la salida de audio%c", 10);
        return 0;
    }
    static int16_t bufs[WAVE_BUFS][WAVE_LEN * 2];
    WAVEHDR hdr[WAVE_BUFS];
    memset(hdr, 0, sizeof hdr);
    for (int i = 0; i < WAVE_BUFS; i++) {
        hdr[i].lpData = (LPSTR)bufs[i];
        hdr[i].dwBufferLength = WAVE_LEN * 4;
        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
        hdr[i].dwFlags |= WHDR_DONE;
    }
    for (;;) {
        for (int i = 0; i < WAVE_BUFS; i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            for (int k = 0; k < WAVE_LEN; k++) {
                LONG r = ring_r;
                if ((r & (RING - 1)) == (ring_w & (RING - 1))) { bufs[i][k * 2] = bufs[i][k * 2 + 1] = 0; continue; }
                bufs[i][k * 2] = ring[(r & (RING - 1)) * 2];
                bufs[i][k * 2 + 1] = ring[(r & (RING - 1)) * 2 + 1];
                InterlockedExchange(&ring_r, (r + 1) & (RING - 1));
            }
            hdr[i].dwFlags &= ~WHDR_DONE;
            waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
        }
        WaitForSingleObject(ev, 50);
    }
    return 0;
}

static void snd_init(void) {
    if (out_on >= 0) return;
    InitializeCriticalSection(&snd_lock);
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 16; k++) players[i].track_no[k] = -1;
    const char *e = getenv("NFS_SOUND");
    out_on = !(e && !strcmp(e, "0")) && !getenv("NFS_HEADLESS");
    if (getenv("NFS_VOLUME")) out_gain = atoi(getenv("NFS_VOLUME")) / 100.0;
    if (out_on) CreateThread(0, 0, audio_thread, 0, 0, 0);
    if (getenv("NFS_WAV") && (wav = fopen(getenv("NFS_WAV"), "wb"))) {
        static const uint8_t head[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                                         0xD8, 0x7F, 0, 0, 0x60, 0xFF, 1, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
        fwrite(head, 1, 44, wav); /* 32.728 Hz, estereo, 16 bits */
        atexit(wav_close);
    }
}

/* Estado para el juego en la zona compartida */
static void update_shared(void) {
    if (!shared_work) return;
    uint32_t ps = 0, cs = 0;
    for (int i = 0; i < 16; i++) {
        if (players[i].active || players[i].prepared) ps |= 1u << i;
        if (ch[i].active) cs |= 1u << i;
    }
    mem_w32(shared_work + 4, ps);
    mem_w16(shared_work + 8, (uint16_t)cs);
    for (int i = 0; i < 16; i++) {
        uint32_t base = shared_work + 32 + i * 36;
        for (int v = 0; v < 16; v++) mem_w16(base + v * 2, (uint16_t)players[i].local_var[v]);
        mem_w32(base + 32, players[i].ticks);
    }
    for (int v = 0; v < 16; v++) mem_w16(shared_work + 32 + 16 * 36 + v * 2, (uint16_t)global_var[v]);
}

/* Una vez por fotograma de video (60 por segundo), desde irq.c */
void snd_vblank(void) {
    snd_init();
    static double frames_acc, samples_acc;
    snd_time += 1.0 / 59.8261;
    alarms_run();
    frames_acc += SND_FRAMES_PER_SEC / 59.8261;
    samples_acc += OUT_RATE / 59.8261;
    while (frames_acc >= 1) {
        frames_acc -= 1;
        seq_frame();
        int n = (int)(samples_acc / (frames_acc + 1)); /* reparto de las muestras entre los fotogramas de sonido */
        if (n > (int)samples_acc) n = (int)samples_acc;
        samples_acc -= n;
        if (out_on || wav) mix(n);
        else for (int i = 0; i < 16; i++) /* sin salida (pruebas): solo se avanza la posicion */
            if (ch[i].active && ch[i].period > 0) channel_next(&ch[i], CH_FREQ_BASE / ch[i].period / OUT_RATE * n);
    }
    update_shared();
    if (getenv("NFS_SND_LOG") && g_frame % 600 == 0) {
        int act = 0, seqc = 0, pl = 0;
        for (int i = 0; i < 16; i++) { act += ch[i].active; seqc += ch[i].active && ch[i].seq; pl += players[i].active; }
        printf("[sonido] fotograma %u: %d notas (%d sin instrumento, %d sin canal, %d sin onda [%08x], %d suenan), %d reproductores, %d canales (%d de secuencia)%c",
               g_frame, dbg_notes, dbg_fail, dbg_nochan, dbg_nowave, dbg_wa, dbg_ok, pl, act, seqc, 10);
        dbg_notes = dbg_fail = dbg_nochan = dbg_nowave = dbg_ok = 0;
    }
}

/* ---- ordenes del juego ------------------------------------------------------------------------- */
static void set_param(uint8_t *base, int size, int offset, uint32_t value, int base_size) {
    if (offset < 0 || offset + size > base_size) return;
    memcpy(base + offset, &value, size);
}

void snd_command(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    snd_init();
    switch (id) {
    case 0: prepare_player((int)a0, a1, a2, a3); if (a0 < 16) players[a0].active = 1; break;   /* START_SEQ */
    case 1: if (a0 < 16) stop_player((int)a0); break;                                            /* STOP_SEQ */
    case 2: prepare_player((int)a0, a1, a2, a3); break;                                          /* PREPARE_SEQ */
    case 3: if (a0 < 16 && players[a0].prepared) players[a0].active = 1; break;                  /* START_PREPARED_SEQ */
    case 4: if (a0 < 16) players[a0].paused = (int)a1; break;                                    /* PAUSE_SEQ */
    case 5: break;                                                                               /* SKIP_SEQ: sin hacer */
    case 6: { /* PLAYER_PARAM: campo de la estructura del reproductor del SDK */
        if (a0 >= 16) break;
        Player *pl = &players[a0];
        int v = (int)a2;
        if (a1 == 4) pl->prio = v & 0xFF;
        else if (a1 == 5) pl->volume = v & 0xFF;
        else if (a1 == 6) pl->ext_fader = (int16_t)v;
        else if (a1 == 24) pl->tempo = v & 0xFFFF;
        else if (a1 == 26) pl->tempo_ratio = v & 0xFFFF;
        break;
    }
    case 7: { /* TRACK_PARAM: jugador | tamano<<24, mascara de pistas, desplazamiento, valor */
        int pi = a0 & 0xFFFFFF;
        if (pi >= 16) break;
        int size = (a0 >> 24) & 0xFF;
        for (int i = 0; i < 16; i++) {
            if (!(a1 & (1u << i)) || players[pi].track_no[i] < 0) continue;
            Track *t = &tracks[players[pi].track_no[i]];
            int v = size == 1 ? (int8_t)a3 : size == 2 ? (int16_t)a3 : (int)a3;
            if (a2 == 4) t->vol = v & 0xFF;
            else if (a2 == 5) t->expr = v & 0xFF;
            else if (a2 == 6) t->bend = (int8_t)v;
            else if (a2 == 9) t->ext_pan = (int8_t)v;
            else if (a2 == 10) t->ext_fader = (int16_t)v;
            else if (a2 == 12) t->ext_pitch = (int16_t)v;
        }
        break;
    }
    case 8: { /* MUTE_TRACK */
        if (a0 >= 16) break;
        for (int i = 0; i < 16; i++)
            if ((a1 & (1u << i)) && players[a0].track_no[i] >= 0) tracks[players[a0].track_no[i]].mute = a2 != 0;
        break;
    }
    case 9: if (a0 < 16) players[a0].alloc_mask = a1 & 0xFFFF; break;                             /* ALLOCATABLE_CHANNEL */
    case 10: if (a0 < 16 && a1 < 16) players[a0].local_var[a1] = (int16_t)a2; break;               /* PLAYER_LOCAL_VAR */
    case 11: if (a1 < 16) global_var[a1] = (int16_t)a2; break;                                    /* PLAYER_GLOBAL_VAR */
    case 12: { /* START_TIMER: canales, capturas, alarmas */
        for (int i = 0; i < 16; i++) if (a0 & (1u << i)) channel_start(&ch[i]);
        if (a1) capture_active |= a1;
        for (int i = 0; i < 8; i++)
            if ((a2 & (1u << i)) && alarms[i].armed) { alarms[i].active = 1; alarms[i].next = snd_time + alarms[i].next; }
        break;
    }
    case 13: { /* STOP_TIMER */
        for (int i = 0; i < 16; i++) if (a0 & (1u << i)) ch[i].active = 0;
        capture_active &= ~a1;
        for (int i = 0; i < 8; i++) if (a2 & (1u << i)) alarms[i].active = 0;
        break;
    }
    case 14: { /* SETUP_CHANNEL_PCM */
        int n = a0 & 0xFFFF;
        if (n > 15) break;
        Channel *c = &ch[n];
        memset(c, 0, sizeof *c);
        c->kind = 0;
        c->data = a1 & 0x07FFFFFF;
        c->fmt = (a3 >> 24) & 3;
        c->loop = (a3 >> 26) & 3;
        c->pan = (a3 >> 16) & 0x7F;
        int unit = c->fmt == 0 ? 4 : c->fmt == 1 ? 2 : 8;
        c->loop_start = (a3 & 0xFFFF) * unit - (c->fmt == 2 && (a3 & 0xFFFF) ? 8 : 0);
        c->loop_len = (a2 & 0x3FFFFF) * unit;
        c->total = c->loop_start + c->loop_len;
        c->vol = (a2 >> 24) & 0x7F;
        c->shift = (a2 >> 22) & 3;
        c->period = (a0 >> 16) & 0xFFFF;
        break;
    }
    case 15: case 16: { /* SETUP_CHANNEL_PSG / NOISE: HIPOTESIS del formato (el juego no las usa en lo visto) */
        int n = a0 & 0xFFFF;
        if (n > 15) break;
        Channel *c = &ch[n];
        memset(c, 0, sizeof *c);
        c->kind = id == 15 ? 1 : 2;
        c->duty = (int)(a1 & 7);
        c->vol = (a2 >> 24) & 0x7F;
        c->shift = (a2 >> 22) & 3;
        c->period = (a0 >> 16) & 0xFFFF;
        c->pan = (a3 >> 16) & 0x7F;
        break;
    }
    case 17: break; /* SETUP_CAPTURE: el efecto no se hace */
    case 18: { /* SETUP_ALARM: numero, primer aviso y periodo (ticks del sistema), identificador */
        if (a0 >= 8) break;
        Alarm *al = &alarms[a0];
        al->no = (int)a0;
        al->id = (int)(a3 & 0xFF);
        al->next = a1 / OS_TICK_HZ;
        al->period = a2 / OS_TICK_HZ;
        al->armed = 1;
        al->active = 0;
        break;
    }
    case 19: for (int i = 0; i < 16; i++) if (a0 & (1u << i)) ch[i].period = a1 & 0xFFFF; break; /* CHANNEL_TIMER */
    case 20: for (int i = 0; i < 16; i++) if (a0 & (1u << i)) { ch[i].vol = a1 & 0x7F; ch[i].shift = a2 & 3; } break; /* CHANNEL_VOLUME */
    case 21: for (int i = 0; i < 16; i++) if (a0 & (1u << i)) ch[i].pan = a1 & 0x7F; break; /* CHANNEL_PAN */
    case 23: master_volume = a0 & 0x7F; break; /* MASTER_VOLUME */
    case 26: locked_mask |= a0 & 0xFFFF; break; /* LOCK_CHANNEL */
    case 27: locked_mask &= ~a0; break; /* UNLOCK_CHANNEL */
    case 28: for (int i = 0; i < 16; i++) if ((a0 & (1u << i)) && !(locked_mask & (1u << i))) ch[i].active = 0; break;
    case 29: shared_work = a0; break; /* SHARED_WORK */
    default: break; /* 22 surround, 24 master pan, 25 output selector, 30-33 invalidar / informacion: nada */
    }
    (void)set_param;
}
