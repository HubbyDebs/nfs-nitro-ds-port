/* Tiempo e interrupciones.
 *
 * En la consola una interrupcion corta el programa en cualquier instruccion. Aqui solo se
 * entregan en puntos seguros: cuando se agota el presupuesto de saltos hacia atras
 * (irq_tick) y cuando el juego se para a esperar (irq_halt).
 *
 * Entrega: se ejecuta el manejador original del SDK (OS_IrqHandler, 0x01FF8000 en la ITCM),
 * que llama a la rutina que el juego registro. Lo que en la consola hace despues
 * OS_IrqHandler_ThreadSwitch (despertar hilos que esperaban y cambiar de hilo) se hace aqui
 * a mano, porque usa instrucciones de cambio de contexto que no tienen sentido traducidas.
 * Direcciones y desplazamientos leidos del desensamblado de 0x01FF8000-0x01FF81E3. */
#include <stdio.h>
#include <stdlib.h>

#include "recomp_funcs.h"
#include "sys.h"

#define OS_IRQ_HANDLER 0x01FF8000u
#define LIT_IRQ_THREAD_QUEUE 0x01FF81D4u /* literal: &OSi_IrqThreadQueue */
#define LIT_THREAD_INFO 0x01FF81D8u      /* literal: &OSi_ThreadInfo (primer campo: hay que replanificar) */
#define RETURN_MAGIC 0xFFFF0020u

#define TICKS_PER_FRAME 4
#define BUDGET_PER_TICK 60000

uint32_t g_frame;
static uint32_t tick_in_frame;
static uint32_t frame_limit;
static int in_irq;

static void advance_time(void) {
    tick_in_frame++;
    io_advance(560190 / TICKS_PER_FRAME); /* ciclos del procesador en este tramo */
    if (tick_in_frame == TICKS_PER_FRAME - 1) {
        io_set_vblank(1, 192);
        {   /* Botones X e Y, que en la consola lee el ARM7 y deja aqui: bit 10 = X, bit 11 = Y
             * (1 = sin pulsar); el bit 15 es "tapa cerrada". El SDK los junta con KEYINPUT. */
            uint32_t k = win_extra_keys();
            mem_w16(0x027FFFA8u, ((k & 1) ? 0x400 : 0) | ((k & 2) ? 0x800 : 0));
        }
        win_frame();
        if (io_reg(0x004) & 8) io_raise_irq(IRQ_VBLANK); /* DISPSTAT bit 3: avisar del vblank */
    } else if (tick_in_frame >= TICKS_PER_FRAME) {
        tick_in_frame = 0;
        io_set_vblank(0, 0);
        g_frame++;
        snd_vblank(); /* sonido: secuencias, alarmas y mezcla de este fotograma */
        if (g_frame % 100 == 0) fprintf(stderr, "[tiempo] fotograma %u%c", g_frame, 10);
        if (!frame_limit) {
            const char *e = getenv("NFS_FRAMES");
            frame_limit = e ? (uint32_t)atoi(e) : 0xFFFFFFFFu; /* sin limite salvo que se pida */
        }
        if (g_frame >= frame_limit) rt_die(0, "limite de %u fotogramas alcanzado (NFS_FRAMES)", frame_limit);
    }
}

static void deliver(Cpu *c) {
    if (in_irq || (c->cpsr_ctl & 0x80) || !io_irq_enabled_pending()) return;
    in_irq = 1;
    Cpu saved = *c;

    /* Entrada en modo IRQ, como haria la BIOS */
    uint32_t irq_sp = c->bank_sp[2];
    cpsr_write(c, 0x92, 1, 0);
    c->r[13] = irq_sp;
    c->r[14] = RETURN_MAGIC;
    call_indirect(c, OS_IRQ_HANDLER);

    /* Parte de OS_IrqHandler_ThreadSwitch: despertar a los hilos que esperaban una interrupcion */
    uint32_t queue = mem_r32(LIT_IRQ_THREAD_QUEUE), info = mem_r32(LIT_THREAD_INFO);
    uint32_t t = mem_r32(queue);
    if (t) {
        while (t) {
            uint32_t next = mem_r32(t + 0x80);
            mem_w32(t + 0x64, 1); /* estado: listo */
            mem_w32(t + 0x78, 0);
            mem_w32(t + 0x7C, 0);
            mem_w32(t + 0x80, 0);
            t = next;
        }
        mem_w32(queue, 0);
        mem_w32(queue + 4, 0);
        mem_w16(info, 1);
    }

    /* Vuelta al modo y a los registros de antes */
    c->r[13] = irq_sp;
    cpsr_write(c, saved.cpsr_ctl, 1, 0);
    memcpy(c->r, saved.r, sizeof c->r);
    c->fn = saved.fn; c->fz = saved.fz; c->fc = saved.fc; c->fv = saved.fv;
    c->bank_sp[2] = irq_sp;
    in_irq = 0;

    /* Si hay que cambiar de hilo, se llama al replanificador del juego. Esta fibra puede quedar
     * dormida aqui dentro y seguir mas tarde. */
    if (mem_r16(info)) {
        mem_w16(info, 0);
        saved = *c;
        c->r[14] = RETURN_MAGIC;
        ram_02007de8(c);
        memcpy(c->r, saved.r, sizeof c->r);
        c->fn = saved.fn; c->fz = saved.fz; c->fc = saved.fc; c->fv = saved.fv;
    }
}

/* Una interrupcion recien pedida (fin de DMA, respuesta del ARM7...) no espera al siguiente tramo
 * de tiempo: se entrega en el proximo salto hacia atras, sin que avance el reloj. Si no, el juego
 * pasaria casi todo el tiempo esperando en bucles. */
static int64_t budget_left;
static int soon;

void irq_request_soon(void) {
    if (soon || in_irq) return;
    soon = 1;
    budget_left = g_budget;
    g_budget = 1;
}

void irq_tick(Cpu *c) {
    if (soon) {
        soon = 0;
        g_budget = budget_left > 0 ? budget_left : 1;
        deliver(c);
        return;
    }
    g_budget = BUDGET_PER_TICK;
    if (in_irq) return;
    advance_time();
    deliver(c);
}

void irq_halt(Cpu *c) {
    /* Nadie tiene nada que hacer: el tiempo salta hasta que haya una interrupcion que entregar */
    for (int i = 0; i < TICKS_PER_FRAME * 120; i++) {
        advance_time();
        if (io_irq_enabled_pending()) {
            if (c->cpsr_ctl & 0x80) rt_die(c, "el juego espera una interrupcion con las interrupciones apagadas");
            deliver(c);
            return;
        }
    }
    rt_die(c, "el juego espera una interrupcion que no llega (IE=%08x, IME=%u, DISPSTAT=%04x)",
           io_reg(0x210), io_reg(0x208) & 1, io_reg(0x004) & 0xFFFF);
}
