/* Capa de ejecucion para el codigo recompilado de NFS Nitro (DS).
 * Define el estado del procesador ARM9 y el acceso a la memoria de la consola. */
#ifndef RT_H
#define RT_H

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

typedef struct Cpu {
    uint32_t r[16];
    uint32_t fn, fz, fc, fv; /* banderas N, Z, C, V (0 o 1) */
    uint32_t cpsr_ctl;       /* bits de control del CPSR (modo, IRQ) */
    /* r13/r14/SPSR propios de cada modo: 0 usr/sys, 1 fiq, 2 irq, 3 svc, 4 abt, 5 und */
    uint32_t bank_sp[6], bank_lr[6], spsr[6];
} Cpu;

typedef void (*GuestFn)(Cpu *c);

typedef struct FnEntry {
    uint32_t key;      /* direccion | 1 si es Thumb */
    const char *block; /* "ram", "itcm", "ov0"... */
    GuestFn fn;
} FnEntry;

extern const FnEntry g_fn_table[];
extern const int g_fn_count;

/* Depuracion: ultimas funciones en las que se ha entrado y presupuesto de saltos hacia atras */
extern uint32_t g_ring[256];
extern uint32_t g_ring_i;
extern int64_t g_budget;
extern int g_overlay_loaded[8];
extern const char *g_rom_dir;

/* Memoria de la consola */
#define RAM_SIZE 0x400000u
#define ITCM_SIZE 0x8000u
#define DTCM_BASE 0x027E0000u
#define DTCM_SIZE 0x4000u
extern uint8_t *g_ram, *g_itcm, *g_dtcm;

uint32_t io_read(uint32_t addr, int size);
void io_write(uint32_t addr, uint32_t value, int size);

static inline uint8_t *mem_ptr(uint32_t a) {
    if (a - 0x02000000u < 0x01000000u) {
        if (a - DTCM_BASE < DTCM_SIZE) return g_dtcm + (a - DTCM_BASE);
        return g_ram + (a & (RAM_SIZE - 1));
    }
    if (a < 0x02000000u) return g_itcm + (a & (ITCM_SIZE - 1));
    return 0;
}

static inline uint32_t mem_r32(uint32_t a) {
    uint8_t *p = mem_ptr(a & ~3u);
    uint32_t v;
    if (!p) return io_read(a & ~3u, 4);
    memcpy(&v, p, 4);
    return v;
}
static inline uint32_t mem_r16(uint32_t a) {
    uint8_t *p = mem_ptr(a & ~1u);
    uint16_t v;
    if (!p) return io_read(a & ~1u, 2);
    memcpy(&v, p, 2);
    return v;
}
static inline uint32_t mem_r8(uint32_t a) {
    uint8_t *p = mem_ptr(a);
    return p ? *p : io_read(a, 1);
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    uint8_t *p = mem_ptr(a & ~3u);
    if (p) memcpy(p, &v, 4); else io_write(a & ~3u, v, 4);
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    uint8_t *p = mem_ptr(a & ~1u);
    uint16_t h = (uint16_t)v;
    if (p) memcpy(p, &h, 2); else io_write(a & ~1u, h, 2);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    uint8_t *p = mem_ptr(a);
    if (p) *p = (uint8_t)v; else io_write(a, v & 0xFF, 1);
}

/* Suma con acarreo que actualiza las cuatro banderas. La resta es a + ~b + 1. */
static inline uint32_t add_f(Cpu *c, uint32_t a, uint32_t b, uint32_t cin) {
    uint64_t s = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)s;
    c->fc = (uint32_t)(s >> 32);
    c->fv = (~(a ^ b) & (a ^ res)) >> 31;
    c->fn = res >> 31;
    c->fz = (res == 0);
    return res;
}

/* Desplazamiento por registro: tipo 0 LSL, 1 LSR, 2 ASR, 3 ROR. Actualiza *co solo si desplaza. */
static inline uint32_t sh_reg(int type, uint32_t v, uint32_t s, uint32_t *co) {
    s &= 0xFF;
    if (!s) return v;
    switch (type) {
    case 0:
        if (s < 32) { *co = (v >> (32 - s)) & 1; return v << s; }
        *co = (s == 32) ? (v & 1) : 0;
        return 0;
    case 1:
        if (s < 32) { *co = (v >> (s - 1)) & 1; return v >> s; }
        *co = (s == 32) ? (v >> 31) : 0;
        return 0;
    case 2:
        if (s < 32) { *co = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
        *co = v >> 31;
        return (uint32_t)((int32_t)v >> 31);
    default:
        s &= 31;
        if (!s) { *co = v >> 31; return v; }
        *co = (v >> (s - 1)) & 1;
        return (v >> s) | (v << (32 - s));
    }
}

static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }

/* Hilos del juego sobre fibras (threads.c) */
void threads_init(Cpu *main_cpu);
jmp_buf *rt_ctx_slot(Cpu *c, uint32_t guest_ctx);
jmp_buf *rt_jmp_slot(Cpu *c, uint32_t guest_buf); /* para el setjmp del propio juego */

/* Implementadas en rt.c */
void rt_init(void);
void rt_die(Cpu *c, const char *fmt, ...);
void call_indirect(Cpu *c, uint32_t target);
int rt_is_function(uint32_t target); /* hay una funcion traducida (y cargada) que empieza ahi? */
void trap(Cpu *c, uint32_t addr, const char *what);
void abi_violation(Cpu *c, uint32_t fn, int reg, uint32_t before); /* solo en compilaciones de depuracion */
void watchdog(Cpu *c, uint32_t addr);
void swi_handler(Cpu *c, uint32_t num);
uint32_t cp15_read(Cpu *c, int crn, int crm, int op2);
void cp15_write(Cpu *c, int crn, int crm, int op2, uint32_t value);
uint32_t cpsr_read(Cpu *c, int spsr);
void cpsr_write(Cpu *c, uint32_t value, int fields, int spsr);

#endif
