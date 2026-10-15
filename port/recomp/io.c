/* Registros de hardware de la DS y memorias de video. De momento lo minimo para arrancar:
 * casi todo se guarda y se devuelve tal cual, y se avisa la primera vez que se toca un
 * registro que no esta tratado. */
#include "sys.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static uint8_t io_a[0x1000];     /* 0x04000000: pantalla principal y sistema */
static uint8_t io_b[0x1000];     /* 0x04001000: pantalla secundaria */
static uint8_t seen_a[0x1000], seen_b[0x1000];
static uint8_t *palette, *vram, *oam;
static uint32_t vcount;
static uint32_t ipc_remote;
static int in_vblank;

static uint8_t *video_ptr(uint32_t a);
static uint32_t get(uint8_t *base, uint32_t off, int size);

uint8_t *io_mem(int which) {
    video_ptr(0x05000000u); /* asegura que las memorias de video existen */
    switch (which) {
    case 0: return io_a;
    case 1: return io_b;
    case 2: return palette;
    case 3: return vram;
    default: return oam;
    }
}

/* Temporizadores (0x04000100-0x0400010F): 4 contadores de 16 bits que avisan al desbordar.
 * El SDK los usa para su reloj (OS_GetTick) y para las alarmas (OS_Sleep). */
static uint32_t timer_counter[4], timer_frac[4];

void io_advance(uint32_t cycles) {
    static const uint32_t presc[4] = {1, 64, 256, 1024};
    uint32_t overflows_prev = 0;
    for (int n = 0; n < 4; n++) {
        uint32_t reload = get(io_a, 0x100 + n * 4, 2), ctl = get(io_a, 0x102 + n * 4, 2), counts, overflows = 0;
        if (!(ctl & 0x80)) { overflows_prev = 0; continue; }
        if ((ctl & 4) && n > 0) {
            counts = overflows_prev; /* en cascada: cuenta los desbordes del anterior */
        } else {
            uint32_t total = timer_frac[n] + cycles, p = presc[ctl & 3];
            counts = total / p;
            timer_frac[n] = total % p;
        }
        uint32_t period = 0x10000 - reload, c = timer_counter[n];
        if (c < reload || c > 0xFFFF) c = reload;
        uint32_t left = 0x10000 - c;
        if (counts >= left) {
            counts -= left;
            overflows = 1 + counts / period;
            c = reload + counts % period;
            if (ctl & 0x40) io_raise_irq(8u << n);
        } else {
            c += counts;
        }
        timer_counter[n] = c;
        overflows_prev = overflows;
    }
    /* Cola del chip 3D: siempre esta vacia, asi que si el juego pide aviso (GXSTAT bits 30-31) se da */
    if (get(io_a, 0x600, 4) >> 30) io_raise_irq(0x00200000u);
}

void io_raise_irq(uint32_t mask) {
    uint32_t v;
    memcpy(&v, io_a + 0x214, 4);
    v |= mask;
    memcpy(io_a + 0x214, &v, 4);
    irq_request_soon();
}

uint32_t io_reg(uint32_t offset) {
    uint32_t v;
    memcpy(&v, io_a + (offset & 0xFFC), 4);
    return v;
}

uint32_t io_irq_enabled_pending(void) {
    if (!(io_reg(0x208) & 1)) return 0;
    return io_reg(0x210) & io_reg(0x214);
}

void io_set_vblank(int on, uint32_t line) {
    in_vblank = on;
    vcount = line;
}

static uint8_t *video_ptr(uint32_t a) {
    if (!palette) {
        palette = calloc(1, 0x800);
        vram = calloc(1, 0x900000);
        oam = calloc(1, 0x800);
    }
    switch (a >> 24) {
    case 5: return palette + (a & 0x7FF);
    case 6: return vram + (a & 0xFFFFFF) % 0x900000;
    case 7: return oam + (a & 0x7FF);
    }
    return 0;
}

static uint32_t get(uint8_t *base, uint32_t off, int size) {
    uint32_t v = 0;
    memcpy(&v, base + off, size);
    return v;
}

static void put(uint8_t *base, uint32_t off, uint32_t v, int size) { memcpy(base + off, &v, size); }

static uint64_t get64(uint32_t off) {
    uint64_t v;
    memcpy(&v, io_a + off, 8);
    return v;
}

/* Divisor y raiz cuadrada por hardware (0x04000280-0x040002BF) */
static void math_update(void) {
    uint32_t mode = get(io_a, 0x280, 4) & 3;
    int64_t num, den, quo = 0, rem = 0;
    if (mode == 0) {
        num = (int32_t)get(io_a, 0x290, 4);
        den = (int32_t)get(io_a, 0x298, 4);
    } else if (mode == 1) {
        num = (int64_t)get64(0x290);
        den = (int32_t)get(io_a, 0x298, 4);
    } else {
        num = (int64_t)get64(0x290);
        den = (int64_t)get64(0x298);
    }
    if (den == 0) {
        quo = num < 0 ? 1 : -1;
        rem = num;
    } else if (!(num == INT64_MIN && den == -1)) {
        quo = num / den;
        rem = num % den;
    } else {
        quo = num;
    }
    memcpy(io_a + 0x2A0, &quo, 8);
    memcpy(io_a + 0x2A8, &rem, 8);

    uint64_t param = (get(io_a, 0x2B0, 4) & 1) ? get64(0x2B8) : get(io_a, 0x2B8, 4);
    uint64_t root = (uint64_t)sqrtl((long double)param);
    while (root * root > param) root--;
    while ((root + 1) * (root + 1) <= param) root++;
    put(io_a, 0x2B4, (uint32_t)root, 4);
}

/* DMA: copia de memoria por hardware. Se hace entera en el momento en que se activa. */
int g_dl_log = -1; /* NFS_DL_LOG: apuntar las huellas de las listas de visualizacion que se envian */

static void dma_check(int ch) {
    if (g_dl_log < 0) g_dl_log = getenv("NFS_DL_LOG") != 0;
    uint32_t base = 0xB0 + ch * 12;
    uint32_t cnt = get(io_a, base + 8, 4);
    if (!(cnt & 0x80000000u)) return;
    uint32_t src = get(io_a, base, 4), dst = get(io_a, base + 4, 4);
    uint32_t n = cnt & 0x1FFFFF;
    int wide = (cnt >> 26) & 1, step = wide ? 4 : 2;
    int dmode = (cnt >> 21) & 3, smode = (cnt >> 23) & 3;
    uint64_t dl_hash = 0;
    if (dst == 0x04000400u && wide && (gx_has_replacements() || g_dl_log)) {
        /* lista de visualizacion para el chip 3D: su huella identifica la pieza del modelo */
        dl_hash = 1469598103934665603ull;
        uint32_t bytes = (n ? n : 0x200000) * 4;
        const uint8_t *p = mem_ptr(src);
        if (p && mem_ptr(src + bytes - 1) == p + bytes - 1) {
            for (uint32_t k = 0; k < bytes; k++) { dl_hash ^= p[k]; dl_hash *= 1099511628211ull; }
        } else {
            for (uint32_t k = 0; k < bytes; k++) { dl_hash ^= mem_r8(src + k); dl_hash *= 1099511628211ull; }
        }
        if (g_dl_log) {
            static uint64_t seen[4096];
            int dup = 0;
            for (int k = 0; k < 4096 && seen[k]; k++) dup |= seen[k] == dl_hash;
            if (!dup) {
                for (int k = 0; k < 4096; k++) if (!seen[k]) { seen[k] = dl_hash; break; }
                fprintf(stderr, "[dl] fotograma %u: %08x %u bytes huella %016llx%c", g_frame, src, bytes,
                        (unsigned long long)dl_hash, 10);
            }
        }
    }
    put(io_a, base + 8, cnt & 0x7FFFFFFFu, 4); /* terminado: se apaga el bit de "ocupado" */
    if (cnt & 0x40000000u) io_raise_irq(0x100u << ch); /* aviso de fin de DMA */
    if (dl_hash && gx_try_replace(dl_hash)) return; /* se ha dibujado un modelo sustituto en su lugar */
    for (uint32_t i = 0; i < n; i++) {
        if (wide) mem_w32(dst, mem_r32(src)); else mem_w16(dst, mem_r16(src));
        if (smode == 0) src += step; else if (smode == 1) src -= step;
        if (dmode == 0 || dmode == 3) dst += step; else if (dmode == 1) dst -= step;
    }
}

static void note(uint8_t *seen, uint32_t addr, const char *what, uint32_t value) {
    uint32_t off = addr & 0xFFF;
    if (seen[off]) return;
    seen[off] = 1;
    fprintf(stderr, "[io] %s %08x (%08x)\n", what, addr, value);
}

uint32_t io_read(uint32_t addr, int size) {
    uint8_t *p = video_ptr(addr);
    if (p) return get(p, 0, size);

    if ((addr >> 12) == 0x04000) {
        uint32_t off = addr & 0xFFF;
        if (off == 0x006) { /* VCOUNT: linea de pantalla. Avanza en cada lectura dentro de su mitad */
            vcount++;
            if (in_vblank) { if (vcount >= 263 || vcount < 192) vcount = 192; }
            else if (vcount >= 192) vcount = 0;
            return vcount;
        }
        if (off == 0x004) /* DISPSTAT: bit 0 = estamos en vblank */
            return (get(io_a, off, 2) & ~7u) | (in_vblank ? 1 : 0);
        if (off == 0x130) return win_keys(); /* botones */
        if (off >= 0x600 && off < 0x6A4) return gx_read(off & ~3u) >> ((off & 3) * 8); /* chip 3D */
        if (off >= 0x100 && off < 0x110 && !(off & 2)) /* contador actual de un temporizador */
            return (timer_counter[(off - 0x100) / 4] & 0xFFFF) | (size == 4 ? get(io_a, off + 2, 2) << 16 : 0);
        if (off == 0x184) /* IPCFIFOCNT: cola de envio siempre vacia; cola de recepcion segun haya respuestas */
            return (get(io_a, off, 2) & 0x8404) | 0x0001 | (arm7_reply_pending() ? 0 : 0x0100);
        if (off == 0x180) /* IPCSYNC: los 4 bits bajos son lo que "dice" el otro procesador (ARM7) */
            return (get(io_a, off, 2) & 0x4F00) | ipc_remote;
        if (off >= 0x2A0 && off < 0x2C0) math_update();
        if (off >= 0x280 && off < 0x2C0) return get(io_a, off, size) & (off == 0x280 || off == 0x2B0 ? 0x7FFF : 0xFFFFFFFF);
        note(seen_a, addr, "lectura ", get(io_a, off, size));
        return get(io_a, off, size);
    }
    if ((addr >> 12) == 0x04001) {
        note(seen_b, addr, "lectura ", get(io_b, addr & 0xFFF, size));
        return get(io_b, addr & 0xFFF, size);
    }
    if (addr == 0x04100000u) return arm7_reply_pop(); /* IPCFIFORECV: respuesta del ARM7 */
    if ((addr >> 24) == 3) rt_die(0, "lectura en %08x (memoria del ARM7): seguramente un puntero malo", addr);
    fprintf(stderr, "[io] lectura fuera de mapa %08x\n", addr);
    return 0;
}

void io_write(uint32_t addr, uint32_t value, int size) {
    { /* depuracion: escrituras del brillo general (MASTER_BRIGHT) y de los efectos (NFS_BRIGHT_LOG) */
        static int on = -1;
        if (on < 0) on = getenv("NFS_BRIGHT_LOG") != 0;
        uint32_t o = addr & 0xFFFF;
        if (on && (addr >> 16) == 0x0400 && (((o & 0xEFFF) >= 0x50 && (o & 0xEFFF) < 0x70) || (o >= 0x330 && o < 0x3C0))) {
            extern uint32_t g_frame;
            fprintf(stderr, "[brillo] f%u %08x = %08x (%d bytes)%c", g_frame, addr, value, size, 10);
        }
    }
    uint8_t *p = video_ptr(addr);
    if (p) {
        put(p, 0, value, size);
        if ((addr >> 24) == 6) gx_vram_written((addr & 0xFFFFFF) % 0x900000); /* memoria de texturas y paletas */
        return;
    }
    if ((addr >> 12) == 0x04000) {
        uint32_t off = addr & 0xFFF;
        if (!(off >= 0x280 && off < 0x2C0)) note(seen_a, addr, "escritura", value);
        if (off == 0x180) {
            /* Saludo inicial del SDK (PXI_Init): el ARM7 simulado contesta cambiando su valor
             * cada vez que el ARM9 escribe, hasta dar la vuelta a cero. */
            ipc_remote = (ipc_remote + 1) & 15;
            put(io_a, off, value & 0x4F00, 2);
            return;
        }
        if (off >= 0x400 && off < 0x600) { /* ordenes para el chip 3D */
            gx_write(off & ~3u, value);
            return;
        }
        if (off == 0x188) { /* IPCFIFOSEND: mensaje para el ARM7 */
            arm7_message(value);
            return;
        }
        if (off == 0x214) { /* IF: escribir 1 borra el aviso */
            put(io_a, off, get(io_a, off, 4) & ~value, 4);
            return;
        }
        if (off >= 0x100 && off < 0x110) { /* temporizadores: al encenderlos, el contador arranca en la recarga */
            int n = (off - 0x100) / 4;
            uint32_t old_ctl = get(io_a, 0x102 + n * 4, 2);
            put(io_a, off, value, size);
            uint32_t new_ctl = get(io_a, 0x102 + n * 4, 2);
            if (!(old_ctl & 0x80) && (new_ctl & 0x80)) { timer_counter[n] = get(io_a, 0x100 + n * 4, 2); timer_frac[n] = 0; }
            return;
        }
        put(io_a, off, value, size);
        if (off == 0x600 && (value >> 30)) io_raise_irq(0x00200000u);
        if (off >= 0xB0 && off < 0xE0) dma_check((off - 0xB0) / 12);
        return;
    }
    if ((addr >> 12) == 0x04001) {
        note(seen_b, addr, "escritura", value);
        put(io_b, addr & 0xFFF, value, size);
        return;
    }
    fprintf(stderr, "[io] escritura fuera de mapa %08x = %08x\n", addr, value);
}
