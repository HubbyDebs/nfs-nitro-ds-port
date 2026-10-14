/* Prueba del hito 2: ejecutar en PC la funcion del juego VfsIndex_FindFile (0x0209D758),
 * traducida a C, y comprobar que encuentra cada archivo de vfs.zip en el indice vfs.bin.
 *
 * Uso: test_vfs CARPETA_ROM LISTA
 *   LISTA: archivo de texto con lineas "numero_esperado ruta" (lo genera tools/recomp/make_vfs_cases.py)
 * Los datos del juego se leen de la ROM extraida del usuario en tiempo de ejecucion. */
#include <stdio.h>
#include <stdlib.h>

#include "recomp_funcs.h"

#define OBJ_ADDR 0x02300000u   /* objeto VfsIndex */
#define DATA_ADDR 0x02301000u  /* contenido de vfs.bin */
#define PATH_ADDR 0x023E0000u  /* ruta que se busca */
#define STACK_TOP 0x023F0000u
#define RETURN_MAGIC 0xDEAD0000u

static uint8_t *read_file(const char *dir, const char *name, long *size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no se puede abrir %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size + 1);
    fread(buf, 1, *size, f);
    buf[*size] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: test_vfs CARPETA_ROM LISTA\n"); return 1; }
    rt_init();

    long size;
    uint8_t *arm9 = read_file(argv[1], "arm9.bin", &size);
    memcpy(g_ram, arm9, size);

    unsigned n_nodes, n_files, n_names;
    uint8_t *dat = read_file(argv[1], "data/vfs.dat", &size);
    sscanf((char *)dat, "%u, %u, %u", &n_nodes, &n_files, &n_names);

    uint8_t *bin = read_file(argv[1], "data/vfs.bin", &size);
    memcpy(mem_ptr(DATA_ADDR), bin, size);
    uint32_t nodes = DATA_ADDR + 8, files = nodes + n_nodes * 8, names = files + n_files * 4;

    /* Mismo reparto que hace Vfs_Open: [0] nodos, [3] tabla de archivos, [6] nombres, +0x24 cabecera */
    mem_w32(OBJ_ADDR + 0 * 4, nodes);
    mem_w32(OBJ_ADDR + 3 * 4, files);
    mem_w32(OBJ_ADDR + 6 * 4, names);
    memcpy(mem_ptr(OBJ_ADDR + 0x24), bin, 8);

    FILE *list = fopen(argv[2], "r");
    if (!list) { fprintf(stderr, "no se puede abrir %s\n", argv[2]); return 1; }
    char line[600];
    int ok = 0, bad = 0;
    while (fgets(line, sizeof line, list)) {
        unsigned expected;
        char path[512];
        if (sscanf(line, "%u %511[^\r\n]", &expected, path) != 2) continue;
        strcpy((char *)mem_ptr(PATH_ADDR), path);

        Cpu cpu;
        memset(&cpu, 0, sizeof cpu);
        cpu.r[0] = OBJ_ADDR;
        cpu.r[1] = PATH_ADDR;
        cpu.r[13] = STACK_TOP;
        cpu.r[14] = RETURN_MAGIC;
        ram_0209d758(&cpu);

        uint32_t got = cpu.r[0] ? (cpu.r[0] - files) / 4 : 0xFFFFFFFFu;
        if (got == expected && cpu.r[13] == STACK_TOP) {
            ok++;
        } else {
            if (bad++ < 10) printf("MAL: %s -> %u (esperado %u), sp=%08x\n", path, got, expected, cpu.r[13]);
        }
    }
    printf("correctos: %d, incorrectos: %d\n", ok, bad);
    return bad != 0;
}
