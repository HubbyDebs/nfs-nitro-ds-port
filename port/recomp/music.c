/* Musica sustituta: las canciones del juego en la calidad de los archivos del usuario.
 *
 * En la DS la musica son 16 canciones (STRM dentro de audio/sound_data.sdat) en ADPCM de 4 bits, mono, a
 * 22.767 Hz: por eso suena con siseo y sin agudos. Si el usuario pone sus propios archivos de esas canciones
 * (MP3, M4A/AAC, FLAC, WAV o WMA; los lee Windows con Media Foundation) en %USERPROFILE%\nfs-nitro-music, el
 * port los toca en su lugar, en estereo, sincronizados con el juego:
 *
 *  - Que cancion suena: el juego lee la cancion del archivo de sonido a trozos (FS_ReadFile, ver hle.c), la
 *    descomprime en el ARM9 y la toca por un canal PCM16 suelto (el 6, VERIFICADO en menu y carrera). Por la
 *    posicion de lectura se sabe que cancion es y por que muestra va.
 *  - En que punto exacto: el port descomprime tambien la cancion original y compara (correlacion) lo que sale
 *    por el canal con la original alrededor de la posicion estimada. Asi la posicion es exacta a la muestra.
 *  - Con que volumen: la relacion entre lo que sale por el canal y la original (el juego baja la musica en la
 *    pausa, con las opciones de volumen, etc.) se aplica a la sustituta.
 *  - Donde cae en el archivo del usuario: al cargarla se busca, por trozos de 5 s, donde esta cada parte de la
 *    original en el archivo (comparando la energia en el tiempo). Si el archivo es otra version y una parte no
 *    se encuentra, en esa parte suena la original.
 * Los archivos se buscan por nombre (ver music_name_matches) y nunca forman parte del proyecto.
 * NFS_MUSIC=0 la desactiva; NFS_MUSIC_DIR cambia la carpeta; NFS_MUS_LOG=1 escribe lo que hace. */
#define COBJMACROS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include "sys.h"

extern const char *g_rom_dir;

#define MAX_SONGS 32
#define SEG_SEC 5.0     /* trozos para buscar cada parte de la original en el archivo del usuario */
#define ENV_RATE 100    /* puntos por segundo de la energia que se compara */

typedef struct Song {
    char name[64];             /* STRM_... */
    uint32_t file_off, size;   /* dentro del .sdat */
    uint32_t data_off;         /* datos de muestras, desde el inicio del STRM */
    uint32_t block_size, block_samples, nblocks;
    int fmt, chans, rate, loop;
    uint32_t loop_start, samples;
    char repl_path[MAX_PATH];  /* archivo del usuario ("" = no hay) */
    volatile LONG state;       /* 0 sin cargar, 1 cargandose, 2 lista, 3 no se pudo */
    int16_t *orig;             /* la original descomprimida (mono) */
    int16_t *repl;             /* la del usuario (estereo, intercalada) */
    uint32_t repl_frames;
    int repl_rate;
    double norm;               /* iguala el volumen de la sustituta al de la original */
    int nseg;
    double *seg_off;           /* por trozo de SEG_SEC: segundos que hay que sumar para caer en el archivo; NAN = no esta */
} Song;

static Song songs[MAX_SONGS];
static int nsongs, enabled = -1, nrepl;
static char sdat_path[MAX_PATH];
static uint32_t sdat_size;

/* Canciones del juego: nombre del STRM, artista y titulo (de los creditos del juego) */
static const struct { const char *strm, *artist, *title; } catalog[] = {
    {"STRM_BloodyBeetroots", "Bloody Beetroots", "Awesome"},
    {"STRM_CodeRoad", "Danko Jones", "Code Of The Road"},
    {"STRM_CrystalMethod", "Crystal Method", "Sine Language"},
    {"STRM_DizzieRascal", "Dizzee Rascal", "Bonkers"},
    {"STRM_Drumagik", "Drumagick", "Make It Rock"},
    {"STRM_Evil9", "Evil 9", "All The Cash"},
    {"STRM_GayBlades", "Gay Blades", "O Shot"},
    {"STRM_KOS", "k-os", "Fun"},
    {"STRM_MajorLazar", "Major Lazer", "Hold The Line"},
    {"STRM_NotThinkingStraight", "Pint Shot Riot", "Not Thinking Straight"},
    {"STRM_OyeVaya", "Earl Greyhound", "Oye Vaya"},
    {"STRM_Riase", "Rise Against", "Kotov Syndrome"},
    {"STRM_RootsManuva", "Roots Manuva", "Buff Nuff"},
    {"STRM_TakingBackSunday", "Taking Back Sunday", "Lonely Lonely"},
    {"STRM_TwoFingers", "Two Fingers", "Jewels And Gems"},
    {"STRM_WhatNow", "Alex Metric", "What Now"},
};

static void norm_text(const char *s, char *out, int max) {
    int n = 0;
    for (; *s && n < max - 1; s++)
        if (isalnum((unsigned char)*s)) out[n++] = (char)tolower((unsigned char)*s);
    out[n] = 0;
}

/* El archivo vale para la cancion si su nombre (sin extension) es el corto del STRM ("MajorLazar") o lleva
 * el artista y el titulo ("Major Lazer - Hold The Line") */
static int music_name_matches(const char *file, const char *strm) {
    char base[MAX_PATH], f[MAX_PATH], k[128], a[128], t[128];
    snprintf(base, sizeof base, "%s", file);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    norm_text(base, f, sizeof f);
    norm_text(strm + 5, k, sizeof k);
    if (!strcmp(f, k)) return 1;
    for (size_t i = 0; i < sizeof catalog / sizeof catalog[0]; i++) {
        if (strcmp(catalog[i].strm, strm)) continue;
        norm_text(catalog[i].artist, a, sizeof a);
        norm_text(catalog[i].title, t, sizeof t);
        return strstr(f, a) && strstr(f, t);
    }
    return 0;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16le(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static int read_at(FILE *f, uint32_t off, void *dst, uint32_t len) {
    return fseek(f, (long)off, SEEK_SET) == 0 && fread(dst, 1, len, f) == len;
}

/* Tabla de canciones del .sdat (bloques SYMB, INFO y FAT del formato SDAT) */
static void parse_sdat(void) {
    FILE *f = fopen(sdat_path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sdat_size = (uint32_t)ftell(f);
    uint8_t h[0x40];
    if (!read_at(f, 0, h, sizeof h) || memcmp(h, "SDAT", 4)) { fclose(f); return; }
    uint32_t symb = rd32le(h + 0x10), info = rd32le(h + 0x18), fat = rd32le(h + 0x20);
    uint8_t b[8];
    if (!read_at(f, info + 8 + 7 * 4, b, 4)) { fclose(f); return; }
    uint32_t rec = info + rd32le(b);
    if (!read_at(f, rec, b, 4)) { fclose(f); return; }
    uint32_t n = rd32le(b), srec = 0;
    if (symb && read_at(f, symb + 8 + 7 * 4, b, 4)) srec = symb + rd32le(b);
    for (uint32_t i = 0; i < n && nsongs < MAX_SONGS; i++) {
        if (!read_at(f, rec + 4 + i * 4, b, 4)) break;
        uint32_t e = rd32le(b);
        if (!e || !read_at(f, info + e, b, 2)) continue;
        uint32_t fid = rd16le(b);
        uint8_t fe[8], sh[0x40];
        if (!read_at(f, fat + 12 + fid * 16, fe, 8)) continue;
        Song *s = &songs[nsongs];
        memset(s, 0, sizeof *s);
        s->file_off = rd32le(fe);
        s->size = rd32le(fe + 4);
        if (!read_at(f, s->file_off, sh, sizeof sh) || memcmp(sh, "STRM", 4) || memcmp(sh + 0x10, "HEAD", 4)) continue;
        const uint8_t *hd = sh + 0x18;
        s->fmt = hd[0];
        s->loop = hd[1];
        s->chans = hd[2];
        s->rate = rd16le(hd + 4);
        s->loop_start = rd32le(hd + 8);
        s->samples = rd32le(hd + 12);
        s->data_off = rd32le(hd + 16);
        s->nblocks = rd32le(hd + 20);
        s->block_size = rd32le(hd + 24);
        s->block_samples = rd32le(hd + 28);
        snprintf(s->name, sizeof s->name, "STRM_%u", i);
        if (srec && read_at(f, srec + 4 + i * 4, b, 4) && rd32le(b)) {
            char nm[64] = {0};
            if (read_at(f, symb + rd32le(b), nm, 63)) snprintf(s->name, sizeof s->name, "%s", nm);
        }
        nsongs++;
    }
    fclose(f);
}

static void find_files(void) {
    char dir[MAX_PATH];
    const char *e = getenv("NFS_MUSIC_DIR");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else snprintf(dir, sizeof dir, "%s\\nfs-nitro-music", getenv("USERPROFILE") ? getenv("USERPROFILE") : ".");
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        for (int i = 0; i < nsongs; i++)
            if (!songs[i].repl_path[0] && music_name_matches(fd.cFileName, songs[i].name)) {
                snprintf(songs[i].repl_path, sizeof songs[i].repl_path, "%s\\%s", dir, fd.cFileName);
                nrepl++;
            }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    fprintf(stderr, "[musica] %d canciones sustitutas en %s%c", nrepl, dir, 10);
}

static int music_init(void) {
    if (enabled >= 0) return enabled;
    enabled = 0;
    const char *e = getenv("NFS_MUSIC");
    if (e && !strcmp(e, "0")) return 0;
    snprintf(sdat_path, sizeof sdat_path, "%s/data/audio/sound_data.sdat", g_rom_dir);
    parse_sdat();
    if (!nsongs) return 0;
    find_files();
    enabled = nrepl > 0;
    return enabled;
}

/* ---- carga (en otro hilo) --------------------------------------------------------------------------- */
static const int8_t ima_index_tab[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
static int ima_step_tab(int i) {
    static int16_t t[89];
    if (!t[0]) { /* la misma tabla que snd.c, generada: 7 * 1.1^i redondeado como el estandar IMA */
        static const int16_t s[89] = {
            7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
            118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
            1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
            6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
            32767};
        memcpy(t, s, sizeof s);
    }
    return t[i];
}

/* La original, descomprimida a PCM16 mono (primer canal si hubiera mas) */
static int decode_orig(Song *s) {
    FILE *f = fopen(sdat_path, "rb");
    if (!f) return 0;
    uint8_t *d = malloc(s->size);
    int ok = d && read_at(f, s->file_off, d, s->size);
    fclose(f);
    if (!ok) { free(d); return 0; }
    s->orig = malloc(sizeof(int16_t) * (s->samples + 1));
    uint32_t out = 0;
    for (uint32_t blk = 0; blk < s->nblocks && out < s->samples; blk++) {
        const uint8_t *p = d + s->data_off + (size_t)blk * s->block_size * s->chans; /* bloques intercalados por canal */
        if ((size_t)(p - d) + s->block_size > s->size) break;
        uint32_t n = s->block_samples;
        if (out + n > s->samples) n = s->samples - out;
        if (s->fmt == 2) {
            int smp = (int16_t)rd16le(p), idx = p[2] > 88 ? 88 : p[2];
            for (uint32_t k = 0; k < n; k++) {
                int b = p[4 + (k >> 1)], nib = (k & 1) ? b >> 4 : b & 15;
                int st = ima_step_tab(idx), diff = st >> 3;
                if (nib & 1) diff += st >> 2;
                if (nib & 2) diff += st >> 1;
                if (nib & 4) diff += st;
                if (nib & 8) { smp -= diff; if (smp < -32767) smp = -32767; }
                else { smp += diff; if (smp > 32767) smp = 32767; }
                idx += ima_index_tab[nib];
                if (idx < 0) idx = 0;
                if (idx > 88) idx = 88;
                s->orig[out++] = (int16_t)smp;
            }
        } else if (s->fmt == 1) {
            for (uint32_t k = 0; k < n; k++) s->orig[out++] = (int16_t)rd16le(p + k * 2);
        } else {
            for (uint32_t k = 0; k < n; k++) s->orig[out++] = (int16_t)((int8_t)p[k] * 256);
        }
    }
    while (out < s->samples) s->orig[out++] = 0;
    free(d);
    return 1;
}

/* El archivo del usuario con Media Foundation: PCM 16 bits, estereo */
static int decode_repl(Song *s) {
    WCHAR wpath[MAX_PATH];
    MultiByteToWideChar(CP_ACP, 0, s->repl_path, -1, wpath, MAX_PATH);
    IMFSourceReader *rd = 0;
    IMFMediaType *mt = 0, *cur = 0;
    int ok = 0;
    if (FAILED(MFCreateSourceReaderFromURL(wpath, 0, &rd))) return 0;
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&mt);
    IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFAudioFormat_PCM);
    IMFMediaType_SetUINT32(mt, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, mt))) goto done;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) goto done;
    UINT32 chans = 0, rate = 0;
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_NUM_CHANNELS, &chans);
    IMFMediaType_GetUINT32(cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    if (!chans || !rate) goto done;
    size_t cap = (size_t)rate * 60 * 2, n = 0;
    int16_t *buf = malloc(cap * sizeof(int16_t));
    for (;;) {
        DWORD flags = 0;
        IMFSample *smp = 0;
        if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, 0, &flags, 0, &smp))) break;
        if (smp) {
            IMFMediaBuffer *mb = 0;
            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(smp, &mb))) {
                BYTE *p;
                DWORD len;
                if (SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, 0, &len))) {
                    size_t frames = len / (2 * chans);
                    if (n + frames * 2 > cap) {
                        cap = (n + frames * 2) * 2;
                        buf = realloc(buf, cap * sizeof(int16_t));
                    }
                    const int16_t *in = (const int16_t *)p;
                    for (size_t k = 0; k < frames; k++) { /* mono: a los dos lados; mas de dos: los dos primeros */
                        buf[n++] = in[k * chans];
                        buf[n++] = in[k * chans + (chans > 1)];
                    }
                    IMFMediaBuffer_Unlock(mb);
                }
                IMFMediaBuffer_Release(mb);
            }
            IMFSample_Release(smp);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    s->repl = buf;
    s->repl_frames = (uint32_t)(n / 2);
    s->repl_rate = (int)rate;
    ok = s->repl_frames > (uint32_t)rate; /* al menos un segundo */
done:
    if (cur) IMFMediaType_Release(cur);
    if (mt) IMFMediaType_Release(mt);
    IMFSourceReader_Release(rd);
    return ok;
}

/* Energia (raiz del cuadrado medio) a ENV_RATE puntos por segundo, de un canal de 16 bits con paso 'stride' */
static float *envelope(const int16_t *x, uint32_t frames, int rate, int stride, int *count) {
    int n = (int)((double)frames * ENV_RATE / rate);
    float *e = malloc(sizeof(float) * (n + 1));
    for (int i = 0; i < n; i++) { /* limites exactos en el tiempo: con un numero entero de muestras por punto, */
        size_t k0 = (size_t)((double)i * rate / ENV_RATE), k1 = (size_t)((double)(i + 1) * rate / ENV_RATE); /* se desfasaban */
        double acc = 0;
        for (size_t k = k0; k < k1; k++) /* energia de cada canal por separado (sumarlos a mono puede anularlos) */
            for (int c = 0; c < stride; c++) {
                double v = x[k * stride + c];
                acc += v * v / stride;
            }
        e[i] = (float)sqrt(acc / (double)(k1 - k0));
    }
    *count = n;
    return e;
}

/* Correlacion normalizada de a[ia..ia+len) con b[ib..ib+len), quitando la media */
static double ncc(const float *a, int ia, const float *b, int ib, int len) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (int i = 0; i < len; i++) {
        double x = a[ia + i], y = b[ib + i];
        sa += x; sb += y; saa += x * x; sbb += y * y; sab += x * y;
    }
    double va = saa - sa * sa / len, vb = sbb - sb * sb / len;
    if (va <= 1e-9 || vb <= 1e-9) return 0;
    return (sab - sa * sb / len) / sqrt(va * vb);
}

/* Donde cae cada trozo de la original en el archivo del usuario */
static void align(Song *s) {
    int na, nb;
    float *ea = envelope(s->orig, s->samples, s->rate, 1, &na);
    float *eb = envelope(s->repl, s->repl_frames, s->repl_rate, 2, &nb);
    int seg = (int)(SEG_SEC * ENV_RATE), win = seg * 2, search = 30 * ENV_RATE;
    s->nseg = (int)(s->samples / (s->rate * SEG_SEC)) + 1;
    s->seg_off = malloc(sizeof(double) * s->nseg);
    /* primero una posicion global con los primeros 30 s */
    int glen = na < 30 * ENV_RATE ? na : 30 * ENV_RATE, best_lag = 0;
    double best = -2;
    for (int lag = -search; lag <= search; lag++) {
        int ia = lag < 0 ? -lag : 0, ib = lag > 0 ? lag : 0, len = glen - ia;
        if (ib + len > nb) len = nb - ib;
        if (len < 5 * ENV_RATE) continue;
        double c = ncc(ea, ia, eb, ib, len);
        if (c > best) { best = c; best_lag = lag; }
    }
    int prev = best_lag, found = 0;
    for (int k = 0; k < s->nseg; k++) {
        int ia = k * seg, len = win;
        if (ia + len > na) len = na - ia;
        s->seg_off[k] = NAN;
        if (len < ENV_RATE) continue;
        double bc = -2;
        int bl = prev;
        for (int lag = prev - 10 * ENV_RATE; lag <= prev + 10 * ENV_RATE; lag++) {
            int ib = ia + lag;
            if (ib < 0 || ib + len > nb) continue;
            double c = ncc(ea, ia, eb, ib, len);
            if (c > bc) { bc = c; bl = lag; }
        }
        if (bc > 0.8) { s->seg_off[k] = (double)bl / ENV_RATE; prev = bl; found++; }
    }
    /* volumen: energia de la original / de la sustituta en las partes encontradas */
    double eo = 0, er = 0;
    for (int k = 0; k < s->nseg; k++) {
        if (isnan(s->seg_off[k])) continue;
        int ia = k * seg, ib = ia + (int)lround(s->seg_off[k] * ENV_RATE);
        for (int i = 0; i < seg && ia + i < na && ib + i < nb && ib + i >= 0; i++) {
            eo += (double)ea[ia + i] * ea[ia + i];
            er += (double)eb[ib + i] * eb[ib + i];
        }
    }
    s->norm = er > 0 ? sqrt(eo / er) : 1;
    if (s->norm > 4) s->norm = 4;
    fprintf(stderr, "[musica] %s: %s (%u Hz) -> desfase %.2f s, %d de %d trozos encontrados (parecido %.2f), volumen x%.2f%c",
            s->name, s->repl_path, s->repl_rate, (double)best_lag / ENV_RATE, found, s->nseg, best, s->norm, 10);
    free(ea);
    free(eb);
}

static DWORD WINAPI load_thread(void *arg) {
    Song *s = arg;
    static LONG mf_started;
    CoInitializeEx(0, COINIT_MULTITHREADED);
    if (!InterlockedExchange(&mf_started, 1)) MFStartup(MF_VERSION, MFSTARTUP_LITE);
    int ok = decode_orig(s);
    if (ok && !decode_repl(s)) {
        fprintf(stderr, "[musica] no se pudo leer %s (formato que Windows no sabe abrir?)%c", s->repl_path, 10);
        ok = 0;
    }
    if (ok) align(s);
    InterlockedExchange(&s->state, ok ? 2 : 3);
    return 0;
}

/* ---- seguimiento de lo que suena (hilo del juego) ------------------------------------------------- */
static int cur_song = -1;
static double playhead;        /* muestra de la original que suena ahora en el canal */
static double read_sample;     /* hasta donde ha leido el juego */
static uint32_t last_read_frame;
static int synced;             /* la posicion se ha confirmado comparando */
static double gain = 1, num, den, weight;
#define HIST 4096
static float hist_v[HIST];
static double hist_p[HIST];
static int hist_n, hist_i, since_sync;

/* hle.c: el juego ha leido [pos, pos+len) del archivo 'path' */
void music_on_read(const char *path, uint32_t pos, uint32_t len) {
    if (!strstr(path, "sound_data.sdat") || !music_init()) return;
    for (int i = 0; i < nsongs; i++) {
        Song *s = &songs[i];
        uint32_t d0 = s->file_off + s->data_off;
        if (pos + len <= d0 || pos >= s->file_off + s->size) continue;
        uint32_t o = pos + len > d0 ? pos + len - d0 : 0, stride = s->block_size * s->chans;
        uint32_t blk = o / stride, in = o % stride;
        if (in > s->block_size) in = s->block_size;
        double smp = (double)blk * s->block_samples + (s->fmt == 2 ? (in > 4 ? (in - 4) * 2 : 0) : s->fmt == 1 ? in / 2 : in);
        extern uint32_t g_frame;
        last_read_frame = g_frame;
        double lead = smp - playhead;
        if (i != cur_song || lead < -1024 || lead > 16384) { /* otra cancion, o ha saltado (bucle): volver a buscar */
            if (getenv("NFS_MUS_LOG"))
                fprintf(stderr, "[musica] %s en la muestra %.0f (antes %d, adelanto %.0f)%c", s->name, smp, cur_song, lead, 10);
            cur_song = i;
            playhead = smp - 2048 > 0 ? smp - 2048 : 0;
            synced = 0;
            hist_n = 0;
            since_sync = 0;
            if (s->repl_path[0] && s->state == 0) {
                s->state = 1;
                CloseHandle(CreateThread(0, 0, load_thread, s, 0, 0));
            }
            for (int k = 0; k < nsongs; k++) /* solo se guarda la que suena */
                if (k != i && songs[k].state == 2) {
                    free(songs[k].orig); free(songs[k].repl); free(songs[k].seg_off);
                    songs[k].orig = songs[k].repl = 0; songs[k].seg_off = 0;
                    songs[k].state = 0;
                }
        }
        read_sample = smp;
        return;
    }
}

static double orig_at(const Song *s, double p) {
    if (p < 0 || p >= s->samples - 1) return 0;
    int n = (int)p;
    double f = p - n;
    return (s->orig[n] + (s->orig[n + 1] - s->orig[n]) * f) / 32768.0;
}

/* Busca la posicion exacta: compara lo ultimo que ha sonado con la original alrededor de lo estimado */
static void resync(const Song *s) {
    int n = hist_n < 2048 ? hist_n : 2048, range = synced ? 64 : 3000; /* ya sincronizada: solo se afina */
    if (n < 2048) return;
    double best = 0, bl = 0, e_v = 0;
    for (int k = 0; k < n; k += 2) e_v += (double)hist_v[(hist_i - 1 - k + HIST * 2) % HIST] * hist_v[(hist_i - 1 - k + HIST * 2) % HIST];
    if (e_v < 1e-4) return; /* silencio: no hay con que comparar */
    for (int lag = -range; lag <= range; lag++) {
        double c = 0, e_o = 0;
        for (int k = 0; k < n; k += 2) {
            int j = (hist_i - 1 - k + HIST * 2) % HIST;
            int p = (int)hist_p[j] + lag;
            double o = p >= 0 && p < (int)s->samples ? s->orig[p] / 32768.0 : 0;
            c += hist_v[j] * o;
            e_o += o * o;
        }
        if (e_o < 1e-6) continue;
        double nc = c / sqrt(e_o * e_v);
        if (nc > best) { best = nc; bl = lag; }
    }
    if (getenv("NFS_MUS_LOG"))
        fprintf(stderr, "[musica] comparacion: parecido %.3f, ajuste %+.0f muestras%c", best, bl, 10);
    if (best > 0.7) {
        playhead += bl;
        for (int k = 0; k < HIST; k++) hist_p[k] += bl;
        synced = 1;
    } else if (best < 0.3) {
        synced = 0;
    }
}

/* snd.c, por cada muestra de salida de un canal PCM suelto: v es lo que da el juego, step lo que avanza el
 * canal (muestras de la original por muestra de salida). Si el canal es el de la musica y hay sustituta,
 * devuelve 1 y en l/r la muestra que debe sonar en su lugar (antes del volumen y del pan del canal). */
int music_mix(int fmt, double rate, double v, double step, int out_rate, double *l, double *r) {
    if (enabled <= 0 || cur_song < 0 || fmt != 1) return 0;
    Song *s = &songs[cur_song];
    if (fabs(rate - s->rate) > s->rate * 0.02) return 0;
    extern uint32_t g_frame;
    if (g_frame - last_read_frame > 120) return 0; /* hace 2 s que el juego no lee: ya no es la musica */
    hist_v[hist_i] = (float)v;
    hist_p[hist_i] = playhead;
    hist_i = (hist_i + 1) % HIST;
    hist_n++;
    if (++since_sync >= (synced ? 2 * out_rate : 2048)) { /* cada 2 s, o enseguida si no se sabe la posicion */
        since_sync = 0;
        if (s->state == 2) resync(s);
    }
    double o = orig_at(s, playhead);
    /* volumen que aplica el juego: v = gain * o (minimos cuadrados con memoria de ~50 ms) */
    double decay = exp(-1.0 / (0.05 * out_rate));
    num = num * decay + v * o;
    den = den * decay + o * o;
    if (den > 1e-5) gain = num / den;
    if (gain < 0) gain = 0;
    if (gain > 2) gain = 2;
    double rl = 0, rr = 0;
    int ok = 0;
    if (s->state == 2 && synced) {
        int seg = (int)(playhead / (s->rate * SEG_SEC));
        if (seg >= 0 && seg < s->nseg && !isnan(s->seg_off[seg])) {
            double t = playhead / s->rate + s->seg_off[seg], p = t * s->repl_rate;
            if (p >= 0 && p < s->repl_frames - 1) {
                uint32_t n = (uint32_t)p;
                double f = p - n;
                const int16_t *a = s->repl + n * 2;
                rl = (a[0] + (a[2] - a[0]) * f) / 32768.0 * s->norm * gain;
                rr = (a[1] + (a[3] - a[1]) * f) / 32768.0 * s->norm * gain;
                ok = 1;
            }
        }
    }
    weight += (ok ? 20.0 : -20.0) / out_rate; /* fundido de 50 ms al cambiar entre la original y la sustituta */
    if (weight < 0) weight = 0;
    if (weight > 1) weight = 1;
    playhead += step;
    if (s->loop && playhead >= s->samples) playhead -= s->samples - s->loop_start;
    if (weight <= 0) return 0;
    *l = v * (1 - weight) + rl * weight;
    *r = v * (1 - weight) + rr * weight;
    return 1;
}
