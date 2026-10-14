/* Sustitutos nativos de funciones del SDK de Nintendo: en vez de ejecutar el codigo
 * original (que hablaria con el cartucho), hacen lo mismo con archivos del PC.
 * Los datos salen siempre de la ROM extraida del usuario (g_rom_dir). */
#include <stdio.h>
#include <stdlib.h>

#include "recomp_funcs.h"

/* ---- archivos de la ROM (FS_*) --------------------------------------------------- */
/* Campos del FSFile del SDK que usa el juego (comprobados en FS_SeekFile, 0x0200DB28):
 * +0x08 archivo, +0x0C estado, +0x24 inicio, +0x28 fin, +0x2C posicion. */
/* El juego copia estructuras FSFile enteras (minizip clona su manejador), asi que el archivo
 * del PC no se puede asociar a la direccion del FSFile. Se guarda un numero de ranura en el
 * propio FSFile (+0x30, campo de argumentos que ya no se usa tras abrir) y los archivos del PC
 * se abren una vez por ruta y no se cierran. */
#define MAX_OPEN 64
#define SLOT_MAGIC 0x4E460000u
static struct { char path[512]; FILE *f; } host_files[MAX_OPEN];

static FILE *file_of(Cpu *c, uint32_t file) {
    uint32_t v = mem_r32(file + 0x30);
    if ((v & 0xFFFF0000u) != SLOT_MAGIC || (v & 0xFFFF) >= MAX_OPEN || !host_files[v & 0xFFFF].f)
        rt_die(c, "operacion sobre un FSFile que no ha abierto el sustituto (%08x)", file);
    return host_files[v & 0xFFFF].f;
}

static void guest_string(uint32_t addr, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) {
        out[i] = (char)mem_r8(addr + i);
        if (!out[i]) break;
    }
    out[i] = 0;
}

static void open_by_name(Cpu *c, uint32_t file, const char *name);

/* FS_OpenFile(FSFile *file, const char *path) */
void hle_ram_0200d97c(Cpu *c) {
    char name[256];
    guest_string(c->r[1], name, sizeof name);
    open_by_name(c, c->r[0], name);
}

/* FS_ConvertPathToFileID(FSFileID *id, const char *path): el "identificador" es un indice en una
 * tabla propia de nombres. FSFileID = { archivo, numero }. */
#define MAX_IDS 256
static char id_names[MAX_IDS][128];
static int id_count;

void hle_ram_0200d884(Cpu *c) {
    char name[128];
    guest_string(c->r[1], name, sizeof name);
    int i = 0;
    while (i < id_count && strcmp(id_names[i], name)) i++;
    if (i == id_count) {
        if (id_count == MAX_IDS) rt_die(c, "demasiados identificadores de archivo");
        strcpy(id_names[id_count++], name);
    }
    mem_w32(c->r[0], 1);
    mem_w32(c->r[0] + 4, (uint32_t)i);
    c->r[0] = 1;
}

/* FS_OpenFileFast(FSFile *file, FSFileID id)  (r1 = archivo, r2 = numero) */
void hle_ram_0200d90c(Cpu *c) {
    if (c->r[1] == 0 || c->r[2] >= (uint32_t)id_count) { c->r[0] = 0; return; }
    open_by_name(c, c->r[0], id_names[c->r[2]]);
}

static void open_by_name(Cpu *c, uint32_t file, const char *name) {
    char path[512];
    const char *rel = name;
    /* Videos Mobiclip: de momento se hace como si no existieran (el plan permite saltarlos).
     * Con NFS_VIDEO=1 se dejan abrir, para depurar el descodificador. */
    size_t n = strlen(name);
    if (n > 5 && !strcmp(name + n - 5, ".mods") && !getenv("NFS_VIDEO")) {
        printf("[fs] abrir %c%s%c -> video saltado%c", 34, name, 34, 10);
        if (getenv("NFS_STACK")) { /* depuracion: quien pide el video (direcciones de codigo en la pila) */
            printf("[fs]   lr=%08x pila:", c->r[14]);
            for (int i = 0; i < 96; i++) {
                uint32_t v = mem_r32(c->r[13] + i * 4);
                if ((v >= 0x02000000u && v < 0x021A5000u) && (v & 1 || !(v & 3))) printf(" %08x", v);
            }
            printf("%c", 10);
        }
        c->r[0] = 0;
        return;
    }
    if (!strncmp(rel, "rom:", 4)) rel += 4;
    while (*rel == '/' || *rel == 92) rel++;
    snprintf(path, sizeof path, "%s/data/%s", g_rom_dir, rel);

    int slot = 0;
    while (slot < MAX_OPEN && host_files[slot].f && strcmp(host_files[slot].path, path)) slot++;
    if (slot == MAX_OPEN) rt_die(c, "demasiados archivos distintos abiertos");
    if (!host_files[slot].f) {
        host_files[slot].f = fopen(path, "rb");
        strcpy(host_files[slot].path, path);
    }
    FILE *f = host_files[slot].f;
    printf("[fs] abrir %c%s%c -> %s%c", 34, name, 34, f ? "ok" : "NO EXISTE", 10);
    if (!f) { c->r[0] = 0; return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    mem_w32(file + 0x08, 1);
    mem_w32(file + 0x0C, mem_r32(file + 0x0C) | 0x10);
    mem_w32(file + 0x24, 0);
    mem_w32(file + 0x28, (uint32_t)size);
    mem_w32(file + 0x2C, 0);
    mem_w32(file + 0x30, SLOT_MAGIC | (uint32_t)slot);
    c->r[0] = 1;
}

/* FS_ReadFile(FSFile *file, void *dst, s32 len) -> bytes leidos */
void hle_ram_0200db18(Cpu *c) {
    uint32_t file = c->r[0], dst = c->r[1];
    int32_t len = (int32_t)c->r[2];
    FILE *f = file_of(c, file);
    uint32_t pos = mem_r32(file + 0x2C), end = mem_r32(file + 0x28);
    if (len < 0) len = 0;
    if ((uint32_t)len > end - pos) len = (int32_t)(end - pos);
    uint8_t *buf = malloc(len ? len : 1);
    fseek(f, (long)pos, SEEK_SET);
    size_t got = fread(buf, 1, len, f);
    { /* musica sustituta (music.c): que cancion lee el juego y por donde va */
        void music_on_read(const char *path, uint32_t pos, uint32_t len);
        music_on_read(host_files[mem_r32(file + 0x30) & 0xFFFF].path, pos, (uint32_t)got);
    }
    for (size_t i = 0; i < got; i++) mem_w8(dst + (uint32_t)i, buf[i]);
    free(buf);
    mem_w32(file + 0x2C, pos + (uint32_t)got);
    c->r[0] = (uint32_t)got;
}

/* FSi_ReadFileCore(FSFile *file, void *dst, s32 len, BOOL async): la usan FS_ReadFile y
 * FS_ReadFileAsync. Aqui la lectura siempre termina en el momento. */
void hle_ram_0200d808(Cpu *c) { hle_ram_0200db18(c); }

/* FS_CloseFile(FSFile *file) */
void hle_ram_0200d9c4(Cpu *c) {
    uint32_t file = c->r[0];
    mem_w32(file + 0x08, 0);
    mem_w32(file + 0x0C, mem_r32(file + 0x0C) & ~0x30u);
    c->r[0] = 1;
}

/* FSi_SendCommand: si se llega aqui, el juego usa una funcion de archivos sin sustituto */
void hle_ram_0200d218(Cpu *c) {
    rt_die(c, "FSi_SendCommand(archivo %08x, orden %u): funcion de archivos del SDK sin sustituto", c->r[0], c->r[1]);
}

/* ---- overlays (FS_LoadOverlay / FS_UnloadOverlay) --------------------------------- */
static uint8_t *load_file(const char *rel, long *size) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_rom_dir, rel);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(*size + 1);
    fread(buf, 1, *size, f);
    fclose(f);
    return buf;
}

/* FS_LoadOverlay(MIProcessor target, FSOverlayID id) */
void hle_ram_0200e3b0(Cpu *c) {
    uint32_t id = c->r[1], saved_lr = c->r[14];
    long ysize, osize;
    char rel[64];
    uint8_t *y9 = load_file("y9.bin", &ysize);
    if (!y9 || (long)(id * 32 + 32) > ysize) rt_die(c, "FS_LoadOverlay: overlay %u desconocido", id);
    uint32_t e[8];
    memcpy(e, y9 + id * 32, 32);
    free(y9);
    uint32_t ram = e[1], size = e[2], bss = e[3], init_start = e[4], init_end = e[5];

    snprintf(rel, sizeof rel, "overlay/overlay_%04u.bin", id);
    uint8_t *data = load_file(rel, &osize);
    if (!data) rt_die(c, "FS_LoadOverlay: no se encuentra %s", rel);
    memcpy(mem_ptr(ram), data, size);
    memset(mem_ptr(ram + size), 0, bss);
    free(data);
    g_overlay_loaded[id] = 1;
    printf("[overlay] cargado %u en %08x (%u bytes)\n", id, ram, size);

    /* Constructores estaticos del overlay: tabla de punteros a funcion */
    for (uint32_t p = init_start; p < init_end; p += 4) {
        uint32_t fn = mem_r32(p);
        if (!fn) continue;
        c->r[14] = 0xFFFFFFF0u;
        call_indirect(c, fn);
    }
    c->r[14] = saved_lr;
    c->r[0] = 1;
}

/* FS_UnloadOverlay(MIProcessor target, FSOverlayID id) */
void hle_ram_0200e404(Cpu *c) {
    uint32_t id = c->r[1];
    if (id < 8) g_overlay_loaded[id] = 0;
    printf("[overlay] descargado %u\n", id);
    c->r[0] = 1;
}

/* ---- puntos de control ------------------------------------------------------------- */
/* unzOpen: se deja correr el codigo original y se anuncia el resultado (hito 3). */
void hle_ram_0203c150(Cpu *c) {
    char name[128];
    guest_string(c->r[0], name, sizeof name);
    ram_0203c150_orig(c);
    printf("[hito 3] unzOpen(\"%s\") del juego devuelve %08x -> %s\n", name, c->r[0],
           c->r[0] ? "ZIP ABIERTO" : "FALLO");
    fflush(stdout);
}

/* FileSys_OpenFile del juego (0x02085670): se deja correr; solo se apunta que archivo pide,
 * para saber por donde va el juego. */
void hle_ram_02085670(Cpu *c) {
    char name[160];
    guest_string(c->r[1], name, sizeof name);
    ram_02085670_orig(c);
    printf("[juego] archivo %c%s%c -> %s%c", 34, name, 34, c->r[0] ? "ok" : "no se pudo abrir", 10);
}

/* Secuencia de pantallas de presentacion (overlay 1). 0x0212C3C4 arranca una entrada de la lista;
 * cada entrada son 16 bytes: +0 nombre, +0xC "es un video". El video del editor no se reproduce
 * (decision del usuario): si la entrada es un video, se pasa directamente a la siguiente con la
 * funcion del juego que avanza la lista (0x0212C4E0). */
void hle_ov1_0212c3c4(Cpu *c) {
    uint32_t entry = c->r[1];
    if (mem_r8(entry + 0xC) && mem_r32(entry)) {
        char name[96];
        guest_string(mem_r32(entry), name, sizeof name);
        printf("[juego] presentacion: se salta el video %c%s%c%c", 34, name, 34, 10);
        c->r[1] = 0;
        ov1_0212c4e0(c);
        return;
    }
    ov1_0212c3c4_orig(c);
}
