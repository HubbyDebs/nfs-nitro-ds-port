/* Capa de ejecucion: memoria, llamadas indirectas, modos del procesador y BIOS. */
#include "sys.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t *g_ram, *g_itcm, *g_dtcm;
uint32_t g_ring[256];
uint32_t g_ring_i;
int64_t g_budget = 60000;
int g_overlay_loaded[8];
const char *g_rom_dir;

void rt_init(void) {
    g_ram = calloc(1, RAM_SIZE);
    g_itcm = calloc(1, ITCM_SIZE);
    g_dtcm = calloc(1, DTCM_SIZE);
}

void rt_die(Cpu *c, const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "\nPARADA: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    extern Cpu *fiber_cpu;
    if (!c) c = fiber_cpu;
    if (c) {
        for (int i = 0; i < 16; i++)
            fprintf(stderr, "r%-2d=%08x%s", i, c->r[i], (i & 3) == 3 ? "\n" : "  ");
        fprintf(stderr, "cpsr_ctl=%02x  pila:", c->cpsr_ctl & 0xFF);
        for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", mem_r32(c->r[13] + i * 4));
        fprintf(stderr, "\n");
    }
    if (getenv("NFS_SHOT")) win_save_screenshot(getenv("NFS_SHOT")); /* captura de las dos pantallas */
    if (getenv("NFS_DUMP")) { /* volcado de la memoria principal para analizarla (no compartir) */
        FILE *f = fopen("ram_dump.bin", "wb");
        if (f) {
            fwrite(g_ram, 1, RAM_SIZE, f);
            fclose(f);
        }
    }
    fprintf(stderr, "ultimas funciones (la mas reciente al final):\n");
    for (int i = 24; i >= 1; i--) {
        uint32_t idx = g_ring_i - i;
        if (idx > g_ring_i) continue;
        fprintf(stderr, " %08x", g_ring[idx & 0xFF]);
    }
    fprintf(stderr, "\n");
    exit(2);
}

static void call_indirect_missing(Cpu *c, uint32_t target);

static int block_active(const char *block) {
    if (block[0] == 'o' && block[1] == 'v') return g_overlay_loaded[block[2] - '0'];
    return 1;
}

static GuestFn find_function(uint32_t target) {
    int lo = 0, hi = g_fn_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_fn_table[mid].key == target) {
            int i = mid;
            while (i > 0 && g_fn_table[i - 1].key == target) i--;
            for (; i < g_fn_count && g_fn_table[i].key == target; i++)
                if (block_active(g_fn_table[i].block)) return g_fn_table[i].fn;
            return 0;
        }
        if (g_fn_table[mid].key < target) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

int rt_is_function(uint32_t target) { return find_function(target) != 0; }

void call_indirect(Cpu *c, uint32_t target) {
    GuestFn fn = find_function(target);
    if (fn) {
        fn(c);
        return;
    }
    call_indirect_missing(c, target);
}

static void call_indirect_missing(Cpu *c, uint32_t target) {
    /* Codigo del juego sin traducir: lo ejecuta el interprete de reserva (interp.c), que ademas
     * apunta la direccion para traducirla en la proxima compilacion. */
    uint32_t a = target & ~1u;
    if ((a >= 0x02000000u && a < 0x02400000u) || (a >= 0x01FF8000u && a < 0x02000000u)) {
        interp_run(c, target);
        return;
    }
    /* Se apunta la direccion: tools/recomp/discover.py la usa para traducirla en la siguiente compilacion */
    FILE *f = fopen("missing_calls.txt", "a");
    if (f) {
        fprintf(f, "%08x%c", target, 10);
        fclose(f);
    }
    rt_die(c, "llamada indirecta a %08x: no hay funcion traducida ahi", target);
}

void abi_violation(Cpu *c, uint32_t fn, int reg, uint32_t before) {
    /* Cada funcion se avisa una sola vez. Se excluyen las que no siguen el convenio a proposito:
     * el arranque y el manejador de interrupciones. */
    static uint32_t seen[128];
    static int count;
    if (fn == 0x01FF8000u || fn == 0x02000A1Cu || fn == 0x02000800u) return;
    for (int i = 0; i < count; i++)
        if (seen[i] == fn) return;
    if (count == 128) return;
    seen[count++] = fn;
    fprintf(stderr, "[abi] la funcion %08x devuelve r%d cambiado: %08x -> %08x. Ultimas funciones:", fn, reg, before,
            c->r[reg]);
    for (int i = 10; i >= 1; i--) fprintf(stderr, " %08x", g_ring[(g_ring_i - i) & 0xFF]);
    fprintf(stderr, "%c", 10);
}

void trap(Cpu *c, uint32_t addr, const char *what) {
    rt_die(c, "instruccion sin traducir en %08x: %s", addr, what);
}

void watchdog(Cpu *c, uint32_t addr) {
    /* Presupuesto de saltos hacia atras agotado: ha pasado "un rato" de ejecucion */
    (void)addr;
    irq_tick(c);
}

/* ---- modos del procesador ------------------------------------------------------ */
static int mode_bank(uint32_t mode) {
    switch (mode & 0x1F) {
    case 0x11: return 1;
    case 0x12: return 2;
    case 0x13: return 3;
    case 0x17: return 4;
    case 0x1B: return 5;
    default: return 0;
    }
}

uint32_t cpsr_read(Cpu *c, int spsr) {
    if (spsr) return c->spsr[mode_bank(c->cpsr_ctl)];
    return (c->fn << 31) | (c->fz << 30) | (c->fc << 29) | (c->fv << 28) | (c->cpsr_ctl & 0xFF);
}

void cpsr_write(Cpu *c, uint32_t value, int fields, int spsr) {
    if (spsr) {
        c->spsr[mode_bank(c->cpsr_ctl)] = value;
        return;
    }
    if (fields & 8) {
        c->fn = value >> 31;
        c->fz = (value >> 30) & 1;
        c->fc = (value >> 29) & 1;
        c->fv = (value >> 28) & 1;
    }
    if (fields & 1) {
        int from = mode_bank(c->cpsr_ctl), to = mode_bank(value);
        if (from != to) {
            c->bank_sp[from] = c->r[13];
            c->bank_lr[from] = c->r[14];
            c->r[13] = c->bank_sp[to];
            c->r[14] = c->bank_lr[to];
        }
        c->cpsr_ctl = (c->cpsr_ctl & ~0xFFu) | (value & 0xFF);
    }
}

uint32_t cp15_read(Cpu *c, int crn, int crm, int op2) {
    (void)c;
    if (crn == 0 && crm == 0 && op2 == 0) return 0x41059461; /* identificador del ARM946E-S */
    if (crn == 9 && crm == 1 && op2 == 0) return 0x027E000A; /* DTCM: base y tamano */
    if (crn == 9 && crm == 1 && op2 == 1) return 0x00000020; /* ITCM */
    if (crn == 1) return 0x00052078;
    return 0;
}

void cp15_write(Cpu *c, int crn, int crm, int op2, uint32_t value) {
    /* Cache, proteccion de memoria y "esperar interrupcion": no hacen falta en PC */
    if (crn == 7 && crm == 0 && op2 == 4) irq_halt(c); /* "esperar interrupcion" */
    (void)value;
}

/* ---- BIOS ---------------------------------------------------------------------- */
static uint16_t crc16(uint16_t crc, uint32_t addr, uint32_t len) {
    static const uint16_t tab[8] = {0xC0C1, 0xC181, 0xC301, 0xC601, 0xCC01, 0xD801, 0xF001, 0xA001};
    for (uint32_t i = 0; i < len; i++) {
        crc ^= mem_r8(addr + i);
        for (int j = 0; j < 8; j++) {
            int carry = crc & 1;
            crc >>= 1;
            if (carry) crc ^= (uint16_t)(tab[j] << (7 - j));
        }
    }
    return crc;
}

void swi_handler(Cpu *c, uint32_t num) {
    uint32_t *r = c->r;
    switch (num) {
    case 0x03: /* WaitByLoop */
        return;
    case 0x09: { /* Div */
        int32_t n = (int32_t)r[0], d = (int32_t)r[1];
        if (d == 0) rt_die(c, "BIOS Div entre cero");
        r[0] = (uint32_t)(n / d);
        r[1] = (uint32_t)(n % d);
        r[3] = (uint32_t)abs(n / d);
        return;
    }
    case 0x0B: { /* CpuSet */
        uint32_t src = r[0], dst = r[1], cnt = r[2] & 0x1FFFFF;
        int fill = (r[2] >> 24) & 1, wide = (r[2] >> 26) & 1;
        for (uint32_t i = 0; i < cnt; i++) {
            if (wide) mem_w32(dst + i * 4, mem_r32(fill ? src : src + i * 4));
            else mem_w16(dst + i * 2, mem_r16(fill ? src : src + i * 2));
        }
        return;
    }
    case 0x0C: { /* CpuFastSet */
        uint32_t src = r[0], dst = r[1], cnt = r[2] & 0x1FFFFF;
        int fill = (r[2] >> 24) & 1;
        for (uint32_t i = 0; i < cnt; i++) mem_w32(dst + i * 4, mem_r32(fill ? src : src + i * 4));
        return;
    }
    case 0x0D: /* Sqrt */
        r[0] = (uint32_t)sqrt((double)r[0]);
        return;
    case 0x0E: /* GetCRC16 */
        r[0] = crc16((uint16_t)r[0], r[1], r[2]);
        return;
    case 0x0F: /* IsDebugger */
        r[0] = 0;
        return;
    default:
        rt_die(c, "llamada a la BIOS (swi %02x) sin implementar", num);
    }
}
