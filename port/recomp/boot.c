/* Arranque del juego recompilado: prepara la memoria como la dejaria la consola y salta
 * al punto de entrada original (0x02000800).
 *
 * Uso: nfs_boot CARPETA_ROM   (la carpeta extraida con ndstool: arm9.bin, header.bin, data/, overlay/) */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "recomp_funcs.h"

static long load_into(const char *name, uint32_t addr, long max) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_rom_dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no se puede abrir %s\n", path); exit(1); }
    long n = (long)fread(mem_ptr(addr), 1, max, f);
    fclose(f);
    return n;
}

/* Si el programa del PC falla (acceso a memoria invalido, pila agotada...), decir donde */
static LONG CALLBACK on_crash(EXCEPTION_POINTERS *e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    static char alt_stack_note[] = "fallo del programa del PC";
    (void)alt_stack_note;
    fprintf(stderr, "%cFALLO DEL PC: codigo %08lx en %p, direccion de memoria %p%c", 10, code,
            e->ExceptionRecord->ExceptionAddress,
            e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : 0, 10);
    if (code == EXCEPTION_STACK_OVERFLOW) {
        fprintf(stderr, "pila del PC agotada (recursion demasiado profunda)%c", 10);
        fflush(stderr);
        fprintf(stderr, "ultimas funciones:");
        for (int i = 40; i >= 1; i--) fprintf(stderr, " %08x", g_ring[(g_ring_i - i) & 0xFF]);
        fprintf(stderr, "%c", 10);
        fflush(stderr);
        _exit(3);
    }
    rt_die(0, "fallo del programa del PC (ver arriba)");
    return EXCEPTION_CONTINUE_SEARCH;
}

int launcher_menu(void); /* menu.c */

int main(int argc, char **argv) {
    /* Carpeta del juego: el argumento (si lo hay) manda; el menu la deja en NFS_ROM (y la recuerda) */
    if (argc >= 2) _putenv_s("NFS_ROM", argv[1]);
    /* Menu de opciones antes de empezar (no en las pruebas automaticas) */
    if (!getenv("NFS_HEADLESS") && !getenv("NFS_NOMENU") && !launcher_menu()) return 0;
    g_rom_dir = getenv("NFS_ROM") && *getenv("NFS_ROM") ? getenv("NFS_ROM") : "C:\\nfs";
    {
        char probe[1100];
        snprintf(probe, sizeof probe, "%s/arm9.bin", g_rom_dir);
        if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
            fprintf(stderr, "no se encuentra el juego en %s (falta arm9.bin)\n", g_rom_dir);
            if (!getenv("NFS_HEADLESS"))
                MessageBoxA(0, "No se encuentra el juego extraido (falta arm9.bin).\nAbre el juego otra vez y elige la carpeta en el menu.",
                            "NFS Nitro", MB_ICONERROR);
            return 1;
        }
    }
    setvbuf(stdout, 0, _IONBF, 0);
    AddVectoredExceptionHandler(1, on_crash);
    rt_init();

    long n = load_into("arm9.bin", 0x02000000u, 0x3BFE00);
    printf("[boot] arm9.bin: %ld bytes en 02000000\n", n);
    load_into("header.bin", 0x027FFE00u, 0x200); /* copia de la cabecera que deja la consola */
    mem_w32(0x027FFC40u, 1);                     /* tipo de arranque: cartucho normal */
    /* Zona compartida con el ARM7 (0x027FFC00): el ARM7 real marca aqui que servicios tiene
     * listos (PXI_IsCallbackReady). Aqui el ARM7 es simulado: todos listos. */
    mem_w32(0x027FFC00u + 0x388 + 4, 0xFFFFFFFFu);
    /* Ajustes de usuario de la consola (copia en 0x027FFC80). HIPOTESIS de desplazamientos (formato de
     * la memoria de configuracion de la DS): calibracion de la pantalla tactil en +0x58 e idioma en +0x64.
     * Calibracion elegida: 16 unidades por pixel, igual que las muestras que da arm7.c. */
    {
        uint32_t u = 0x027FFC80u;
        mem_w8(u + 0x00, 5);          /* version */
        mem_w16(u + 0x06, 'P'); mem_w16(u + 0x08, 'C'); mem_w16(u + 0x1A, 2); /* apodo "PC" */
        mem_w16(u + 0x58, 32 * 16); mem_w16(u + 0x5A, 32 * 16); mem_w8(u + 0x5C, 32); mem_w8(u + 0x5D, 32);
        mem_w16(u + 0x5E, 224 * 16); mem_w16(u + 0x60, 160 * 16); mem_w8(u + 0x62, 224); mem_w8(u + 0x63, 160);
        mem_w16(u + 0x64, 1);         /* idioma: ingles */
    }

    Cpu cpu;
    memset(&cpu, 0, sizeof cpu);
    cpu.cpsr_ctl = 0xDF; /* modo sistema, interrupciones apagadas */
    cpu.r[13] = 0x027E3FC0u;
    cpu.r[14] = 0xFFFF0000u;
    threads_init(&cpu);
    printf("[boot] entrando en el punto de entrada 02000800\n");
    ram_02000800(&cpu);
    printf("[boot] el punto de entrada ha vuelto (no deberia)\n");
    return 0;
}
