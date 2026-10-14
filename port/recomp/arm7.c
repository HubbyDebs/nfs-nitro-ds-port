/* El segundo procesador de la DS (ARM7), simulado a nivel de mensajes.
 *
 * En la consola el ARM7 ejecuta su propio programa (arm7.bin) y lleva el sonido, la pantalla
 * tactil, el reloj, el encendido, la memoria de guardado... El ARM9 le habla por una cola de
 * mensajes (PXI). Cada mensaje es una palabra: bits 0-4 = servicio (tag), bit 5 = error,
 * bits 6-31 = dato.
 *
 * Aqui no se ejecuta arm7.bin: se contesta a cada servicio lo que el juego necesita. De momento
 * solo se apuntan los mensajes para saber que pide. */
#include <stdio.h>
#include <stdlib.h>

#include "sys.h"

static const char *TAG_NAMES[32] = {
    "EX", "USER_0", "USER_1", "SYSTEM", "NVRAM", "RTC", "TOUCHPANEL", "SOUND", "PM", "MIC", "WM", "FS",
    "OS", "CTRDG", "CARD", "WVR", "CTRDG_Ex", "CTRDG_PHI"};

#define QUEUE 64
static uint32_t replies[QUEUE];
static int head, count;
static uint32_t logged;

static void reply(uint32_t tag, uint32_t data, int err) {
    if (count == QUEUE) rt_die(0, "cola de respuestas del ARM7 llena");
    replies[(head + count++) % QUEUE] = (tag & 0x1F) | (err ? 0x20 : 0) | (data << 6);
    if (io_reg(0x184) & 0x0400) io_raise_irq(IRQ_IPC_RECV);
}

/* Mensaje del ARM7 al juego por iniciativa propia (alarmas de sonido). Si la cola va llena, se pierde. */
void arm7_send(uint32_t tag, uint32_t data) {
    if (count < QUEUE - 8) reply(tag, data, 0);
}

int arm7_reply_pending(void) { return count > 0; }

uint32_t arm7_reply_pop(void) {
    if (!count) return 0;
    uint32_t v = replies[head];
    head = (head + 1) % QUEUE;
    count--;
    if (count && (io_reg(0x184) & 0x0400)) io_raise_irq(IRQ_IPC_RECV);
    return v;
}

/* Memoria de guardado del cartucho: un archivo en la carpeta desde la que se ejecuta */
#define SAVE_FILE "nfs_nitro.sav"
static uint8_t *save_data;
static uint32_t save_size;

static void save_load(uint32_t size_log2) {
    if (save_data) return;
    if (size_log2 < 9 || size_log2 > 23) size_log2 = 13;
    save_size = 1u << size_log2;
    save_data = malloc(save_size);
    memset(save_data, 0xFF, save_size); /* memoria borrada */
    FILE *f = fopen(SAVE_FILE, "rb");
    if (f) {
        fread(save_data, 1, save_size, f);
        fclose(f);
    }
    printf("[arm7] memoria de guardado: %u bytes (%s)\n", save_size, f ? "cargada de " SAVE_FILE : "vacia");
}

static void save_flush(void) {
    FILE *f = fopen(SAVE_FILE, "wb");
    if (!f) return;
    fwrite(save_data, 1, save_size, f);
    fclose(f);
}

void arm7_message(uint32_t word) {
    uint32_t tag = word & 0x1F, data = word >> 6;
    if (tag != 11 && logged < 200) {
        logged++;
        printf("[arm7] mensaje del juego: servicio %u (%s), dato %07x%s\n", tag,
               TAG_NAMES[tag] ? TAG_NAMES[tag] : "?", data, (word & 0x20) ? " [error]" : "");
    }
    switch (tag) {
    case 11: { /* FS: en realidad lo usa la libreria CARD para la memoria de guardado */
        /* HIPOTESIS (protocolo CARDi_Request del SDK): la orden 0 (iniciar) va seguida de una segunda
         * palabra con la direccion de la estructura de ordenes; las demas son una sola palabra.
         * Respuesta: misma etiqueta, bit de error puesto, dato = resultado (0 = bien). */
        static int expect_pointer;
        static uint32_t card_cmd;
        if (expect_pointer) {
            expect_pointer = 0;
            card_cmd = data;
            printf("[arm7] CARD: estructura de ordenes en %08x\n", card_cmd);
            reply(11, 0, 1);
        } else if (data == 0) {
            expect_pointer = 1;
        } else {
            /* Estructura de ordenes (HIPOTESIS, por los valores vistos): +0 resultado, +4 tipo de memoria
             * (byte bajo: 1 = EEPROM; byte siguiente: log2 del tamano), +0xC origen, +0x10 destino, +0x14 longitud.
             * Ordenes: 6 leer, 7 escribir, 8 programar, 9 verificar. */
            uint32_t type = mem_r32(card_cmd + 4), src = mem_r32(card_cmd + 12), dst = mem_r32(card_cmd + 16),
                     len = mem_r32(card_cmd + 20);
            static uint32_t shown;
            if (shown++ < 12)
                printf("[arm7] CARD: orden %u tipo %04x origen %08x destino %08x longitud %x\n", data, type, src,
                       dst, len);
            save_load((type >> 8) & 0xFF);
            if (data == 6 && src < save_size) {
                for (uint32_t i = 0; i < len && src + i < save_size; i++) mem_w8(dst + i, save_data[src + i]);
            } else if ((data == 7 || data == 8) && dst < save_size) {
                for (uint32_t i = 0; i < len && dst + i < save_size; i++) save_data[dst + i] = (uint8_t)mem_r8(src + i);
                save_flush();
            }
            mem_w32(card_cmd, 0);
            reply(11, 0, 1);
        }
        break;
    }
    case 7: { /* SOUND */
        /* Libreria SND del SDK (VERIFICADO: la orden 29 da la zona compartida): el dato es la direccion
         * de una lista de ordenes { siguiente, numero, 4 argumentos }. Se ejecutan (snd.c) y se suma 1
         * al contador de listas terminadas, el primer campo de la zona compartida de sonido. */
        static uint32_t shared, lists;
        uint32_t cmd = data;
        for (int guard = 0; cmd >= 0x02000000u && cmd < 0x02400000u && guard < 512; guard++) {
            uint32_t id = mem_r32(cmd + 4);
            if (getenv("NFS_SND_LOG")) {
                static int counts[64], shown;
                if (id < 64 && counts[id]++ < 6 && shown++ < 400)
                    printf("[snd] fotograma %u orden %u: %08x %08x %08x %08x%c", g_frame, id, mem_r32(cmd + 8),
                           mem_r32(cmd + 12), mem_r32(cmd + 16), mem_r32(cmd + 20), 10);
                if (id < 64 && g_frame % 600 == 0 && !(counts[63]++ % 50)) {
                    printf("[snd] cuenta:");
                    for (int k = 0; k < 40; k++) if (counts[k]) printf(" %d:%d", k, counts[k]);
                    printf("%c", 10);
                }
            }
            snd_command(id, mem_r32(cmd + 8), mem_r32(cmd + 12), mem_r32(cmd + 16), mem_r32(cmd + 20));
            if (id == 29) {
                shared = mem_r32(cmd + 8);
                printf("[arm7] sonido: zona compartida en %08x%c", shared, 10);
            }
            cmd = mem_r32(cmd);
        }
        lists++;
        if (shared) mem_w32(shared, lists);
        break;
    }
    case 4:   /* NVRAM */
    case 6:   /* TOUCHPANEL */
    case 8:   /* PM (encendido) */
    case 9: { /* MIC */
        /* HIPOTESIS (protocolo "SPI" del SDK): dato = inicio(bit 25) | fin(bit 24) | indice(bits 16-19) |
         * orden(bits 8-14) | parametro. Respuesta: lo mismo con el bit 15 puesto y resultado 0 = bien.
         * Solo se contesta al ultimo trozo de una orden (bit de fin).
         * Pantalla tactil: la muestra se deja en la zona compartida (0x027FFFAA); todo a cero = sin tocar. */
        uint32_t cmd = (data >> 8) & 0x7F;
        if (tag == 6) {
            /* HIPOTESIS del formato: x (12 bits), y (12 bits), tocando (1 bit), en unidades de 1/16 de pixel */
            int tx, ty, down = win_touch(&tx, &ty);
            uint32_t sample = down ? ((uint32_t)(tx << 4) | ((uint32_t)(ty << 4) << 12) | (1u << 24)) : 0;
            mem_w16(0x027FFFAAu, sample & 0xFFFF);
            mem_w16(0x027FFFACu, sample >> 16);
        }
        if (data & 0x01000000u) reply(tag, 0x03000000u | ((cmd | 0x80) << 8), 0);
        break;
    }
    default:
        break;
    }
}
