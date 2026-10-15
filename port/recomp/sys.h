/* Declaraciones internas de la capa de ejecucion (no las ve el codigo traducido, asi que
 * cambiar este archivo no obliga a recompilar el juego entero). */
#ifndef SYS_H
#define SYS_H

#include "rt.h"

/* io.c */
void io_raise_irq(uint32_t mask);   /* marca interrupciones como pendientes (registro IF) */
uint32_t io_irq_enabled_pending(void); /* IE & IF si IME esta activo; si no, 0 */
uint32_t io_reg(uint32_t offset);   /* lee un registro de 0x04000000 sin efectos */
void io_set_vblank(int on, uint32_t line);
void io_advance(uint32_t cycles); /* pasa el tiempo para temporizadores y avisos pendientes */
uint8_t *io_mem(int which); /* 0 registros A, 1 registros B, 2 paletas, 3 VRAM, 4 OAM */

/* pool.c: grupo de hilos del PC para el dibujado */
int pool_threads(void);
void pool_run(int count, void (*fn)(void *ctx, int item), void *ctx);

/* gx.c: chip 3D */
void gx_write(uint32_t offset, uint32_t value);
uint32_t gx_read(uint32_t offset);
int gx_has_replacements(void);   /* hay modelos sustitutos cargados */
int gx_try_replace(uint64_t dl_hash); /* dibuja el sustituto de esa lista de visualizacion, si lo hay */
void gx_vram_written(uint32_t flat_offset); /* el juego ha escrito en la VRAM (desplazamiento en el bloque plano) */

/* Una pantalla para que la grafica la mezcle con el 3D y la pinte en la ventana (gpu.inc.c) */
typedef struct GpuScreen {
    const uint32_t *back, *front, *raw; /* capas 2D juntadas (256x192): detras del 3D, delante, delante sin mezclar */
    const uint32_t *ids;                /* 256x192: capa de detras, capa de delante (0xFF ninguna), mezcla pendiente */
    int use3d, fx3d, cnt, eva, evb, bright_mode, bright_factor;
    int x, y, w, h;                     /* donde va en la ventana (pixeles, desde arriba a la izquierda); w = 0: no se pinta */
} GpuScreen;
int gpu_present(const GpuScreen *top, const GpuScreen *bottom, void *hwnd, int win_w, int win_h);
int video_present_gpu(const int top[4], const int bot[4], void *hwnd, int win_w, int win_h);

/* win.c: ventana, teclado y raton */
void win_frame(void);
uint32_t win_keys(void);
uint32_t win_extra_keys(void);
int win_touch(int *x, int *y);
void win_save_screenshot(const char *path);

/* arm7.c: el segundo procesador, simulado a nivel de mensajes */
void arm7_message(uint32_t word);
int arm7_reply_pending(void);
uint32_t arm7_reply_pop(void);

#define IRQ_IPC_RECV 0x00040000u

/* snd.c: sonido */
void snd_command(uint32_t id, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
void snd_vblank(void);

/* interp.c: interprete de reserva para codigo sin traducir */
void interp_run(Cpu *c, uint32_t target);

/* irq.c */
void irq_request_soon(void); /* hay una interrupcion nueva: entregarla en el proximo punto seguro */
void irq_tick(Cpu *c);  /* ha pasado un rato de ejecucion: avanza el tiempo y entrega interrupciones */
void irq_halt(Cpu *c);  /* el juego se queda quieto hasta la proxima interrupcion */
extern uint32_t g_frame;

#define IRQ_VBLANK 0x00000001u

#endif
