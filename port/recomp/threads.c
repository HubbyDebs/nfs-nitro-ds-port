/* Hilos del sistema operativo del juego (SDK de Nintendo) sobre fibras de Windows.
 *
 * El SDK cambia de hilo con dos funciones: OS_SaveContext (guarda los registros y devuelve 0;
 * "devuelve 1" cuando alguien restaura ese contexto) y OS_LoadContext (restaura un contexto
 * y no vuelve). Es el mismo esquema que setjmp/longjmp.
 *
 * En el codigo recompilado la pila de llamadas es la del PC, asi que no basta con cambiar
 * los registros del juego. Solucion:
 *  - cada hilo del juego corre en su propia fibra, con su propia estructura Cpu;
 *  - en cada llamada a OS_SaveContext el traductor pone un setjmp en la funcion
 *    que llama (rt_ctx_slot devuelve donde guardarlo);
 *  - OS_LoadContext (sustituido aqui) cambia a la fibra del contexto pedido, y esa fibra,
 *    al despertar, hace longjmp a su setjmp: el juego ve que OS_SaveContext
 *    "devuelve 1".
 *
 * Estructura OSContext (leida en el desensamblado de 0x02008504 / 0x02008550):
 *  +0x00 cpsr, +0x04..+0x3C r0..r14, +0x40 pc+4, +0x44 sp del modo SVC. */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp_funcs.h"

#define MAX_CTX 64

typedef struct Ctx {
    uint32_t guest;   /* direccion del OSContext en la memoria del juego */
    void *fiber;      /* fibra en la que quedo guardado */
    Cpu *cpu;         /* Cpu de esa fibra */
    jmp_buf jb;       /* destino del longjmp */
    int saved;        /* 1 si hay un setjmp pendiente de reanudar */
} Ctx;

static Ctx ctxs[MAX_CTX];
static Ctx *resume_target;
Cpu *fiber_cpu; /* Cpu de la fibra en ejecucion */
int g_thread_log = 0; /* (los __builtin_setjmp de clang fallan en Windows: se usa setjmp normal) */

void threads_init(Cpu *main_cpu) {
    g_thread_log = getenv("NFS_THREADLOG") != 0;
    ConvertThreadToFiber(0);
    fiber_cpu = main_cpu;
}

static Ctx *find_ctx(uint32_t guest, int create) {
    Ctx *free_slot = 0;
    for (int i = 0; i < MAX_CTX; i++) {
        if (ctxs[i].guest == guest) return &ctxs[i];
        if (!ctxs[i].guest && !free_slot) free_slot = &ctxs[i];
    }
    if (!create) return 0;
    if (!free_slot) rt_die(0, "demasiados hilos del juego");
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->guest = guest;
    return free_slot;
}

/* La llama el codigo traducido justo antes de OS_SaveContext(r0 = contexto). */
jmp_buf *rt_ctx_slot(Cpu *c, uint32_t guest) {
    Ctx *x = find_ctx(guest, 1);
    if (g_thread_log) printf("[hilos] guardar contexto %08x (sp=%08x lr=%08x)%c", guest, c->r[13], c->r[14], 10);
    x->fiber = GetCurrentFiber();
    x->cpu = c;
    x->saved = 1;
    return &x->jb;
}

static void load_regs(Cpu *c, uint32_t guest) {
    uint32_t cpsr = mem_r32(guest);
    for (int i = 0; i < 15; i++) c->r[i] = mem_r32(guest + 4 + i * 4);
    c->fn = cpsr >> 31;
    c->fz = (cpsr >> 30) & 1;
    c->fc = (cpsr >> 29) & 1;
    c->fv = (cpsr >> 28) & 1;
    c->cpsr_ctl = cpsr & 0xFF;
    c->bank_sp[3] = mem_r32(guest + 0x44);
}

/* Reanuda, en la fibra actual, el contexto apuntado por resume_target. */
static void resume_here(void) {
    Ctx *x = resume_target;
    resume_target = 0;
    fiber_cpu = x->cpu;
    load_regs(x->cpu, x->guest); /* incluye r0 = 1: lo que "devuelve" OS_SaveContext */
    x->saved = 0;
    longjmp(x->jb, 1);
}

static void CALLBACK fiber_main(void *param) {
    Ctx *x = param;
    Cpu *c = x->cpu;
    fiber_cpu = c;
    resume_target = 0;
    uint32_t cpsr = mem_r32(x->guest);
    uint32_t pc = mem_r32(x->guest + 0x40) - 4;
    load_regs(c, x->guest);
    if (cpsr & 0x20) pc |= 1; /* el hilo empieza en modo Thumb */
    printf("[hilos] arranca el hilo con contexto %08x en %08x\n", x->guest, pc);
    call_indirect(c, pc);
    /* La funcion del hilo ha terminado: se salta a su direccion de retorno (OS_ExitThread) */
    call_indirect(c, c->r[14]);
    rt_die(c, "un hilo del juego ha terminado y OS_ExitThread ha vuelto");
}

/* OS_LoadContext(OSContext *ctx): no vuelve nunca a quien la llama. */
void hle_ram_02008550(Cpu *c) {
    uint32_t guest = c->r[0];
    Ctx *x = find_ctx(guest, 1);
    if (g_thread_log) printf("[hilos] cargar contexto %08x (guardado=%d) sp=%08x pc+4=%08x%c", guest, x->saved,
                             mem_r32(guest + 0x38), mem_r32(guest + 0x40), 10);
    if (!x->saved) {
        /* Contexto nuevo, creado por OS_CreateThread: fibra y Cpu propios */
        x->cpu = malloc(sizeof(Cpu));
        *x->cpu = *c;
        x->fiber = CreateFiber(0x100000, fiber_main, x);
        if (!x->fiber) rt_die(c, "no se pudo crear la fibra de un hilo");
        SwitchToFiber(x->fiber);
    } else {
        resume_target = x;
        if (x->fiber == GetCurrentFiber()) resume_here();
        SwitchToFiber(x->fiber);
    }
    /* Esta fibra habia quedado dormida aqui y alguien acaba de restaurar su contexto */
    if (!resume_target || resume_target->fiber != GetCurrentFiber())
        rt_die(c, "se ha despertado una fibra sin contexto que reanudar");
    resume_here();
}

/* ---- setjmp / longjmp del propio juego ---------------------------------------------
 * La libreria de C del juego tiene su setjmp (0x0202A2A4) y longjmp (0x0202A2B0); Lua los usa
 * para sus errores. Igual que con los hilos: el setjmp del PC se pone en la funcion que llama
 * al setjmp del juego, y el longjmp del juego acaba en un longjmp del PC. */
#define MAX_JMP 256
static struct { uint32_t guest; void *fiber; jmp_buf jb; } jmps[MAX_JMP];
static int jmp_next;

jmp_buf *rt_jmp_slot(Cpu *c, uint32_t guest) {
    (void)c;
    void *fiber = GetCurrentFiber();
    int i;
    for (i = 0; i < MAX_JMP; i++)
        if (jmps[i].guest == guest && jmps[i].fiber == fiber) break;
    if (i == MAX_JMP) {
        i = jmp_next;
        jmp_next = (jmp_next + 1) % MAX_JMP;
    }
    jmps[i].guest = guest;
    jmps[i].fiber = fiber;
    return &jmps[i].jb;
}

/* longjmp(jmp_buf buf, int valor) */
void hle_ram_0202a2b0(Cpu *c) {
    uint32_t guest = c->r[0];
    void *fiber = GetCurrentFiber();
    for (int i = 0; i < MAX_JMP; i++) {
        if (jmps[i].guest == guest && jmps[i].fiber == fiber) {
            ram_0202a2b0_orig(c); /* restaura r4-r11, sp y lr del juego y deja el valor en r0 */
            longjmp(jmps[i].jb, 1);
        }
    }
    rt_die(c, "longjmp del juego a un setjmp desconocido (%08x)", guest);
}

/* luaG_errormsg(lua_State *L): el mensaje de error esta en la cima de la pila de Lua.
 * Desplazamientos de Lua 5.0 (HIPOTESIS): L->top en +8; cada valor son 8 bytes (tipo, dato);
 * tipo 4 = cadena, con el texto 16 bytes despues del inicio del objeto. */
void hle_ram_02034ee0(Cpu *c) {
    uint32_t top = mem_r32(c->r[0] + 8);
    uint32_t type = mem_r32(top - 8), value = mem_r32(top - 4);
    char text[200];
    int n = 0;
    if (type == 4)
        for (; n < 199; n++) {
            text[n] = (char)mem_r8(value + 16 + n);
            if (!text[n]) break;
        }
    text[n] = 0;
    printf("[lua] ERROR de Lua (tipo de valor %u): %s%c", type, text, 10);
    ram_02034ee0_orig(c);
}
