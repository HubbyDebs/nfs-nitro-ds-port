/* Ejecuta los casos de tools/recomp/fuzz.py sobre el C traducido y compara con lo que dio
 * el emulador de referencia. Uso: fuzz CARPETA (con fuzz_image.bin, fuzz_window.bin, fuzz_tests.bin) */
#include <stdio.h>
#include <stdlib.h>

#include "rt.h"

#define WIN_LO 0x021F8000u
#define WIN_SIZE 0x10000u

uint8_t *g_ram, *g_itcm, *g_dtcm;
uint32_t g_ring[256];
uint32_t g_ring_i;
int64_t g_budget = 1000000000;
int g_overlay_loaded[8];
const char *g_rom_dir;
static int trapped, io_touched;

void call_indirect(Cpu *c, uint32_t target) { (void)c; (void)target; }
int rt_is_function(uint32_t target) { (void)target; return 0; }
void trap(Cpu *c, uint32_t addr, const char *what) { (void)c; (void)addr; (void)what; trapped = 1; }
void watchdog(Cpu *c, uint32_t addr) { (void)c; (void)addr; }
void swi_handler(Cpu *c, uint32_t num) { (void)c; (void)num; trapped = 1; }
uint32_t cp15_read(Cpu *c, int a, int b, int d) { (void)c; (void)a; (void)b; (void)d; trapped = 1; return 0; }
void cp15_write(Cpu *c, int a, int b, int d, uint32_t v) { (void)c; (void)a; (void)b; (void)d; (void)v; trapped = 1; }
uint32_t cpsr_read(Cpu *c, int s) { (void)c; (void)s; trapped = 1; return 0; }
void cpsr_write(Cpu *c, uint32_t v, int f, int s) { (void)c; (void)v; (void)f; (void)s; trapped = 1; }
uint32_t io_read(uint32_t addr, int size) { (void)addr; (void)size; io_touched = 1; return 0; }
void io_write(uint32_t addr, uint32_t value, int size) { (void)addr; (void)value; (void)size; io_touched = 1; }
void abi_violation(Cpu *c, uint32_t fn, int reg, uint32_t before) { (void)c; (void)fn; (void)reg; (void)before; }
void rt_die(Cpu *c, const char *fmt, ...) { (void)c; (void)fmt; trapped = 1; }
void interp_one(Cpu *c, uint32_t pc, int thumb);
jmp_buf *rt_ctx_slot(Cpu *c, uint32_t g) { (void)c; (void)g; return 0; }
jmp_buf *rt_jmp_slot(Cpu *c, uint32_t g) { (void)c; (void)g; return 0; }

static uint8_t *load(const char *dir, const char *name, long *size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no se puede abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size + 1);
    fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

static GuestFn find(uint32_t key) {
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].key == key) return g_fn_table[mid].fn;
        if (g_fn_table[mid].key < key) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    long isize, wsize, tsize;
    g_ram = calloc(1, RAM_SIZE);
    g_itcm = calloc(1, ITCM_SIZE);
    g_dtcm = calloc(1, DTCM_SIZE);
    uint8_t *image = load(argv[1], "fuzz_image.bin", &isize);
    uint8_t *window = load(argv[1], "fuzz_window.bin", &wsize);
    uint8_t *tests = load(argv[1], "fuzz_tests.bin", &tsize);
    memcpy(g_ram, image, isize);
    uint8_t *win = g_ram + (WIN_LO - 0x02000000u);

    long pos = 0;
    int total = 0, bad = 0, untranslated = 0;
    while (pos < tsize) {
        uint32_t *t = (uint32_t *)(tests + pos);
        uint32_t key = t[0], *in = t + 1, in_flags = t[16], *out = t + 17, out_flags = t[32], nw = t[33];
        uint32_t *w = t + 34;
        pos += (34 + nw * 3) * 4;
        total++;

        memcpy(win, window, WIN_SIZE);
        Cpu c;
        memset(&c, 0, sizeof c);
        memcpy(c.r, in, 15 * 4);
        c.fn = (in_flags >> 3) & 1; c.fz = (in_flags >> 2) & 1; c.fc = (in_flags >> 1) & 1; c.fv = in_flags & 1;
        trapped = io_touched = 0;
        if (argc > 2) { /* modo "interp": se prueba el interprete de reserva en vez del C traducido */
            interp_one(&c, key & ~1u, key & 1);
        } else {
            GuestFn fn = find(key);
            if (!fn) { printf("MAL %08x sin funcion\n", key); bad++; continue; }
            fn(&c);
        }
        if (trapped) { untranslated++; continue; }

        char what[160] = "";
        for (int i = 0; i < 15 && !what[0]; i++)
            if (c.r[i] != out[i]) snprintf(what, sizeof what, "r%d: esperado %08x, sale %08x", i, out[i], c.r[i]);
        uint32_t flags = (c.fn << 3) | (c.fz << 2) | (c.fc << 1) | c.fv;
        if (!what[0] && flags != out_flags)
            snprintf(what, sizeof what, "banderas NZCV: esperado %x, sale %x (entrada %x)", out_flags, flags, in_flags);
        if (!what[0] && io_touched) snprintf(what, sizeof what, "acceso a memoria fuera de la ventana");
        for (uint32_t i = 0; i < nw && !what[0]; i++) {
            uint32_t a = w[i * 3], s = w[i * 3 + 1], v = w[i * 3 + 2], got = 0;
            memcpy(&got, win + (a - WIN_LO), s);
            if (s < 4) v &= (1u << (8 * s)) - 1;
            if (got != v) snprintf(what, sizeof what, "memoria %08x: esperado %x, sale %x", a, v, got);
            memcpy(win + (a - WIN_LO), window + (a - WIN_LO), s);
        }
        if (!what[0] && memcmp(win, window, WIN_SIZE)) snprintf(what, sizeof what, "escritura de memoria de mas");
        if (what[0]) {
            printf("MAL %08x %s\n", key, what);
            bad++;
        }
    }
    printf("casos: %d, incorrectos: %d, sin traducir: %d\n", total, bad, untranslated);
    return 0;
}
