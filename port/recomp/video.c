/* Dibujado de las dos pantallas 2D de la DS a partir de los registros y la memoria de video.
 *
 * Cada pantalla la pinta un "motor" 2D: el A (registros en 0x04000000) y el B (0x04001000).
 * Cada motor tiene hasta 4 fondos (BG0-BG3) y 128 sprites (OBJ). Se dibuja el fotograma
 * entero de una vez, al llegar el vblank.
 *
 * Hecho: fondos de teselas (4 y 8 bits), fondos afines y extendidos (teselas de 16 bits,
 * mapas de bits de 8 bits y de color directo), sprites normales (1D y 2D), prioridades,
 * sprites rotados/escalados, sprites semitransparentes, mezcla alfa y de brillo entre capas
 * (tambien con el 3D, usando su alfa por pixel), ventanas 0 y 1, brillo maestro, modo "pantalla
 * desde VRAM", ventana de sprites, mosaico (fondos de teselas y sprites), sprites de mapa de bits.
 * Sin hacer: paletas extendidas de sprites, mosaico en fondos afines.
 *
 * La memoria de video se trata como un bloque plano por direccion (no se simula el reparto
 * de bancos de VRAM): fondos del motor A en 0x06000000, del B en 0x06200000, sprites de A
 * en 0x06400000, de B en 0x06600000, LCDC en 0x06800000. */
#include <stdio.h>
#include <stdlib.h>

#include "sys.h"

#define W 256
#define H 192

typedef struct Engine {
    uint8_t *reg;      /* registros del motor */
    uint8_t *bg_vram;  /* memoria de fondos */
    uint8_t *obj_vram; /* memoria de sprites */
    uint8_t *bg_pal;   /* paleta de fondos (256 colores) */
    uint8_t *obj_pal;  /* paleta de sprites */
    uint8_t *oam;      /* tabla de sprites */
    uint8_t *bg_extpal; /* paletas extendidas de fondos: 4 ranuras de 16 paletas de 256 colores */
    int is_main;
} Engine;

static uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t color(uint32_t c) { /* BGR555 -> 0xFFRRGGBB (el byte alto marca "pixel pintado") */
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

/* Resolucion interna (win.c): cada pixel de la consola son S x S pixeles de salida. El 2D se
 * dibuja a tamano original y se amplia; el 3D se dibuja directamente a la resolucion interna. */
int video_scale(void);

const uint32_t *gx_frame(int *w, int *h);
static uint32_t layer[W * H]; /* capa 2D en curso, a tamano original */

/* Efecto de brillo por capas (registros BLDCNT/BLDY): fx = 0 nada, 1..16 hacia blanco,
 * -1..-16 hacia negro. El juego lo usa para tapar la escena entre la carga y el fundido. */
static uint32_t fade(uint32_t c, int fx) {
    if (!fx) return c;
    uint32_t out = c & 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        int v = (c >> s) & 255;
        v = fx > 0 ? v + (255 - v) * fx / 16 : v + v * fx / 16;
        out |= (uint32_t)v << s;
    }
    return out;
}

/* Mezcla de dos colores con los pesos de la consola (BLDALPHA: eva y evb sobre 16) */
static uint32_t mix(uint32_t a, uint32_t b, int eva, int evb) {
    uint32_t out = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) {
        int v = ((int)((a >> s) & 255) * eva + (int)((b >> s) & 255) * evb) >> 4;
        out |= (uint32_t)(v > 255 ? 255 : v) << s;
    }
    return out;
}

/* Las capas 2D se juntan a tamano original en dos imagenes: lo que queda detras del 3D y lo que
 * queda delante. Asi la ampliacion a la resolucion interna se hace una sola vez por pantalla.
 *
 * Para la mezcla alfa (BLDCNT modo 1 y sprites semitransparentes) hace falta saber, en cada
 * pixel, que capa hay justo debajo y su color sin mezclar: *_raw y *_id (0-3 fondos, 4 sprites,
 * 5 color de fondo, NO_LAYER nada). Si lo que hay debajo de una capa delantera es el 3D, la
 * mezcla se deja pendiente (front_pend) y se hace al ampliar, con el pixel 3D a alta resolucion. */
#define NO_LAYER 0xFF
static uint32_t back2d[W * H], front2d[W * H], back_raw[W * H], front_raw[W * H];
static uint8_t back_id[W * H], front_id[W * H], front_pend[W * H];

typedef struct BlendRegs {
    uint32_t cnt;
    int mode, eva, evb, evy;
} BlendRegs;

static BlendRegs blend_regs(const Engine *e) {
    BlendRegs b;
    uint32_t alpha = rd16(e->reg + 0x52);
    b.cnt = rd16(e->reg + 0x50);
    b.mode = (b.cnt >> 6) & 3;
    b.eva = alpha & 31;
    b.evb = (alpha >> 8) & 31;
    b.evy = rd16(e->reg + 0x54) & 31;
    if (b.eva > 16) b.eva = 16;
    if (b.evb > 16) b.evb = 16;
    if (b.evy > 16) b.evy = 16;
    return b;
}

/* Ventanas (WIN0/WIN1): rectangulos que dicen que capas se ven dentro y fuera, y si hay efectos de
 * mezcla. wmask guarda por pixel los bits de WININ/WINOUT que tocan: 0-4 capas, 5 efectos. */
static uint8_t wmask[W * H];

static int in_range(int v, int lo, int hi) { return lo <= hi ? (v >= lo && v < hi) : (v >= lo || v < hi); }

static void draw_objs(Engine *e, int prio, uint32_t *buf, int win_only);

static void build_windows(Engine *e, uint32_t dispcnt) {
    if (!(dispcnt & 0xE000)) { memset(wmask, 0x3F, sizeof wmask); return; }
    uint32_t winin = rd16(e->reg + 0x48), winout = rd16(e->reg + 0x4A);
    int objwin = (dispcnt & 0x8000) && (dispcnt & 0x1000);
    if (objwin) { /* ventana de sprites: los pixeles de los sprites en modo "ventana" */
        memset(layer, 0, sizeof layer);
        draw_objs(e, 0, layer, 1);
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t m = winout & 0x3F; /* fuera de todas */
            if (objwin && (layer[y * W + x] >> 24)) m = (winout >> 8) & 0x3F; /* dentro de la de sprites */
            for (int n = 1; n >= 0; n--) { /* la 0 tiene prioridad sobre la 1 */
                if (!(dispcnt & (0x2000u << n))) continue;
                uint32_t h = rd16(e->reg + 0x40 + n * 2), v = rd16(e->reg + 0x44 + n * 2);
                int x1 = h >> 8, x2 = h & 255, y1 = v >> 8, y2 = v & 255;
                if (in_range(x, x1, x2) && in_range(y, y1, y2)) m = (winin >> (n * 8)) & 0x3F;
            }
            wmask[y * W + x] = m;
        }
}

/* Pone la capa en curso (layer) encima de lo ya juntado. Los pixeles de sprites semitransparentes
 * llegan marcados con alfa 0xFE. */
static void merge_layer(const BlendRegs *br, int id, int front) {
    uint32_t *disp = front ? front2d : back2d, *raw = front ? front_raw : back_raw;
    uint8_t *ids = front ? front_id : back_id;
    int first = (br->cnt >> id) & 1;
    int fx = (first && br->mode >= 2) ? (br->mode == 2 ? br->evy : -br->evy) : 0;
    for (int i = 0; i < W * H; i++) {
        uint32_t c = layer[i];
        if (!(c >> 24) || !((wmask[i] >> id) & 1)) continue;
        int semi = (c >> 24) == 0xFE, effects = (wmask[i] >> 5) & 1;
        c |= 0xFF000000u;
        uint32_t out = effects ? fade(c, fx) : c;
        int pend = 0;
        if (effects && (semi || (first && br->mode == 1))) {
            if (ids[i] == NO_LAYER) pend = 1;
            else if ((br->cnt >> 8 >> ids[i]) & 1) out = mix(c, raw[i], br->eva, br->evb);
        }
        disp[i] = out;
        raw[i] = c;
        ids[i] = (uint8_t)id;
        if (front) front_pend[i] = (uint8_t)pend;
    }
}

/* Fondo de teselas ("texto") */
static void draw_text_bg(Engine *e, int n, uint32_t *line_buf) {
    uint32_t dispcnt = rd32(e->reg), cnt = rd16(e->reg + 8 + n * 2);
    uint32_t char_base = ((cnt >> 2) & 15) * 0x4000, screen_base = ((cnt >> 8) & 31) * 0x800;
    if (e->is_main) {
        char_base += ((dispcnt >> 24) & 7) * 0x10000;
        screen_base += ((dispcnt >> 27) & 7) * 0x10000;
    }
    int bpp8 = (cnt >> 7) & 1, size = (cnt >> 14) & 3;
    int map_w = (size & 1) ? 512 : 256, map_h = (size & 2) ? 512 : 256;
    uint32_t hofs = rd16(e->reg + 0x10 + n * 4) & 0x1FF, vofs = rd16(e->reg + 0x12 + n * 4) & 0x1FF;
    int mh = (cnt & 0x40) ? (int)(rd16(e->reg + 0x4C) & 15) + 1 : 1, mv = (cnt & 0x40) ? (int)((rd16(e->reg + 0x4C) >> 4) & 15) + 1 : 1; /* mosaico */
    /* Con paletas extendidas (DISPCNT bit 30) los fondos de 256 colores usan su ranura: la del
     * numero de fondo, o esa + 2 para BG0/BG1 si lo pide el bit 13 de su registro. */
    int ext = bpp8 && (dispcnt & 0x40000000u);
    int slot = (n < 2 && (cnt & 0x2000)) ? n + 2 : n;
    const uint8_t *pal8 = ext ? e->bg_extpal + slot * 0x2000 : e->bg_pal;

    for (int y = 0; y < H; y++) {
        uint32_t py = (y - y % mv + vofs) & (map_h - 1);
        for (int x = 0; x < W; x++) {
            uint32_t px = (x - x % mh + hofs) & (map_w - 1);
            /* los mapas grandes son varios bloques de 32x32 teselas seguidos */
            uint32_t block = 0;
            if (px >= 256) block += 1;
            if (py >= 256) block += (map_w == 512) ? 2 : 1;
            uint32_t entry = rd16(e->bg_vram + screen_base + block * 0x800 + (((py & 255) >> 3) * 32 + ((px & 255) >> 3)) * 2);
            uint32_t tile = entry & 0x3FF, tx = px & 7, ty = py & 7;
            if (entry & 0x400) tx = 7 - tx;
            if (entry & 0x800) ty = 7 - ty;
            uint32_t idx;
            if (bpp8) {
                idx = e->bg_vram[char_base + tile * 64 + ty * 8 + tx];
                if (idx) line_buf[y * W + x] = color(rd16(pal8 + (ext ? (entry >> 12) * 0x200 : 0) + idx * 2));
            } else {
                uint8_t b = e->bg_vram[char_base + tile * 32 + ty * 4 + (tx >> 1)];
                idx = (tx & 1) ? b >> 4 : b & 15;
                if (idx) line_buf[y * W + x] = color(rd16(e->bg_pal + ((entry >> 12) * 16 + idx) * 2));
            }
        }
    }
}

/* Fondo afin o extendido (BG2/BG3): se recorre con la matriz de transformacion */
static void draw_affine_bg(Engine *e, int n, int extended, uint32_t *buf) {
    uint32_t dispcnt = rd32(e->reg), cnt = rd16(e->reg + 8 + n * 2);
    const uint8_t *p = e->reg + 0x20 + (n - 2) * 0x10;
    int32_t pa = (int16_t)rd16(p), pb = (int16_t)rd16(p + 2), pc = (int16_t)rd16(p + 4), pd = (int16_t)rd16(p + 6);
    int32_t ox = (int32_t)(rd32(p + 8) << 4) >> 4, oy = (int32_t)(rd32(p + 12) << 4) >> 4;
    int size = (cnt >> 14) & 3, wrap = (cnt >> 13) & 1;
    int bitmap = extended && (cnt & 0x80);
    int direct = bitmap && (cnt & 4);
    int bw, bh;
    if (bitmap) {
        static const int ws[4] = {128, 256, 512, 512}, hs[4] = {128, 256, 256, 512};
        bw = ws[size];
        bh = hs[size];
    } else {
        bw = bh = 128 << size;
    }
    uint32_t screen_base = ((cnt >> 8) & 31) * (bitmap ? 0x4000 : 0x800);
    uint32_t char_base = ((cnt >> 2) & 15) * 0x4000;
    if (e->is_main && !bitmap) {
        char_base += ((dispcnt >> 24) & 7) * 0x10000;
        screen_base += ((dispcnt >> 27) & 7) * 0x10000;
    }

    for (int y = 0; y < H; y++) {
        int32_t fx = ox + pb * y, fy = oy + pd * y;
        for (int x = 0; x < W; x++, fx += pa, fy += pc) {
            int32_t sx = fx >> 8, sy = fy >> 8;
            if (wrap) {
                sx &= bw - 1;
                sy &= bh - 1;
            } else if (sx < 0 || sy < 0 || sx >= bw || sy >= bh) {
                continue;
            }
            if (direct) {
                uint32_t c = rd16(e->bg_vram + screen_base + (sy * bw + sx) * 2);
                if (c & 0x8000) buf[y * W + x] = color(c);
            } else if (bitmap) {
                uint32_t idx = e->bg_vram[screen_base + sy * bw + sx];
                if (idx) buf[y * W + x] = color(rd16(e->bg_pal + idx * 2));
            } else {
                uint32_t tile, tx = sx & 7, ty = sy & 7;
                const uint8_t *tpal = e->bg_pal;
                if (extended) {
                    uint32_t entry = rd16(e->bg_vram + screen_base + ((sy >> 3) * (bw >> 3) + (sx >> 3)) * 2);
                    tile = entry & 0x3FF;
                    if (dispcnt & 0x40000000u) tpal = e->bg_extpal + n * 0x2000 + (entry >> 12) * 0x200;
                    if (entry & 0x400) tx = 7 - tx;
                    if (entry & 0x800) ty = 7 - ty;
                } else {
                    tile = e->bg_vram[screen_base + (sy >> 3) * (bw >> 3) + (sx >> 3)];
                }
                uint32_t idx = e->bg_vram[char_base + tile * 64 + ty * 8 + tx];
                if (idx) buf[y * W + x] = color(rd16(tpal + idx * 2));
            }
        }
    }
}

static void draw_bg(Engine *e, int n, uint32_t *buf) {
    uint32_t dispcnt = rd32(e->reg);
    int mode = dispcnt & 7;
    int kind = 0; /* 0 teselas, 1 afin, 2 extendido */
    if (n == 3) kind = (mode == 0) ? 0 : (mode == 1 || mode == 2) ? 1 : 2;
    if (n == 2) kind = (mode <= 1 || mode == 3) ? 0 : (mode == 2 || mode == 4) ? 1 : 2;
    if (kind == 0) draw_text_bg(e, n, buf);
    else draw_affine_bg(e, n, kind == 2, buf);
}

/* Sprites de una prioridad dada */
/* win_only = 1: solo los sprites de "ventana" (modo 2), de cualquier prioridad, para la ventana de sprites */
static void draw_objs(Engine *e, int prio, uint32_t *buf, int win_only) {
    static const int sizes[3][4][2] = {
        {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
        {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
        {{8, 16}, {8, 32}, {16, 32}, {32, 64}}};
    uint32_t dispcnt = rd32(e->reg);
    int map_1d = (dispcnt >> 4) & 1;
    uint32_t boundary = map_1d ? (32u << ((dispcnt >> 20) & 3)) : 32;

    for (int i = 127; i >= 0; i--) {
        const uint8_t *o = e->oam + i * 8;
        uint32_t a0 = rd16(o), a1 = rd16(o + 2), a2 = rd16(o + 4);
        int affine = (a0 >> 8) & 1;
        if (!affine && (a0 & 0x200)) continue; /* oculto */
        int mode = (a0 >> 10) & 3, shape = a0 >> 14;
        if (shape == 3 || (win_only ? mode != 2 : mode == 2)) continue;
        if (!win_only && (int)((a2 >> 10) & 3) != prio) continue;
        uint32_t mos = rd16(e->reg + 0x4C);
        int mos_w = (a0 & 0x1000) ? (int)((mos >> 8) & 15) + 1 : 1, mos_h = (a0 & 0x1000) ? (int)((mos >> 12) & 15) + 1 : 1;
        int w = sizes[shape][a1 >> 14][0], h = sizes[shape][a1 >> 14][1];
        int x0 = a1 & 0x1FF, y0 = a0 & 0xFF;
        if (x0 >= 256) x0 -= 512;
        if (y0 >= 192) y0 -= 256;
        int bpp8 = (a0 >> 13) & 1, hflip = !affine && (a1 & 0x1000), vflip = !affine && (a1 & 0x2000);
        uint32_t tile = a2 & 0x3FF, pal = a2 >> 12;
        uint32_t mark = mode == 1 ? 0xFE000000u : 0xFF000000u; /* semitransparente: se mezcla al juntar capas */
        /* Sprites rotados/escalados: la matriz va repartida en 4 entradas de la tabla; con "doble
         * tamano" el recuadro en pantalla es el doble para que quepa el sprite girado */
        int bw = w, bh = h;
        int32_t pa = 256, pb = 0, pc = 0, pd = 256;
        if (affine) {
            const uint8_t *pp = e->oam + ((a1 >> 9) & 31) * 32 + 6;
            pa = (int16_t)rd16(pp); pb = (int16_t)rd16(pp + 8); pc = (int16_t)rd16(pp + 16); pd = (int16_t)rd16(pp + 24);
            if (a0 & 0x200) { bw *= 2; bh *= 2; }
        }

        for (int y = 0; y < bh; y++) {
            int sy = y0 + y;
            if (sy < 0 || sy >= H) continue;
            int ty0 = vflip ? h - 1 - y : y;
            for (int x = 0; x < bw; x++) {
                int ty = ty0;
                int sx = x0 + x;
                if (sx < 0 || sx >= W) continue;
                int tx = hflip ? w - 1 - x : x;
                int ty_px = ty;
                if (affine) {
                    int rx = x - bw / 2, ry = y - bh / 2;
                    tx = ((pa * rx + pb * ry) >> 8) + w / 2;
                    ty_px = ((pc * rx + pd * ry) >> 8) + h / 2;
                    if (tx < 0 || ty_px < 0 || tx >= w || ty_px >= h) continue;
                }
                ty = ty_px;
                if (mos_w > 1) tx -= tx % mos_w; /* mosaico */
                if (mos_h > 1) ty -= ty % mos_h;
                if (mode == 3) { /* mapa de bits de color directo */
                    uint32_t addr;
                    if (dispcnt & 0x40) { /* 1D: bloques de 128 o 256 bytes */
                        addr = tile * ((dispcnt & 0x400000) ? 256 : 128) + (ty * w + tx) * 2;
                    } else { /* 2D: imagen de 128 o 256 pixeles de ancho */
                        int bmw = (dispcnt & 0x20) ? 256 : 128, cols = bmw / 8;
                        addr = ((tile / cols) * 8 * bmw + (tile % cols) * 8 + ty * bmw + tx) * 2;
                    }
                    uint32_t c = e->obj_vram[addr & 0x3FFFF] | (e->obj_vram[(addr + 1) & 0x3FFFF] << 8);
                    if (c & 0x8000) buf[sy * W + sx] = color(c);
                    continue;
                }
                uint32_t idx, addr;
                if (map_1d) {
                    uint32_t t = (ty >> 3) * (w >> 3) + (tx >> 3);
                    addr = tile * boundary + t * (bpp8 ? 64 : 32);
                } else {
                    addr = (tile + (ty >> 3) * 32 + (tx >> 3) * (bpp8 ? 2 : 1)) * 32;
                }
                if (bpp8) {
                    idx = e->obj_vram[(addr + (ty & 7) * 8 + (tx & 7)) & 0x3FFFF];
                    if (idx) buf[sy * W + sx] = (color(rd16(e->obj_pal + idx * 2)) & 0xFFFFFFu) | mark;
                } else {
                    uint8_t b = e->obj_vram[(addr + (ty & 7) * 4 + ((tx & 7) >> 1)) & 0x3FFFF];
                    idx = (tx & 1) ? b >> 4 : b & 15;
                    if (idx) buf[sy * W + sx] = (color(rd16(e->obj_pal + (pal * 16 + idx) * 2)) & 0xFFFFFFu) | mark;
                }
            }
        }
    }
}

typedef struct UpscaleJob {
    uint32_t *buf;
    const uint32_t *g3d;
    int S, has_3d, fx_3d;
    BlendRegs br;
} UpscaleJob;

/* Un pixel 3D encima de lo de detras: si es translucido (o la niebla le ha bajado el alfa) se mezcla
 * con su propio alfa con la capa de debajo, si esa capa es "segundo objetivo" de la mezcla */
static uint32_t over_3d(uint32_t g, uint32_t bc, int back_layer, const UpscaleJob *j, int idx) {
    uint32_t ga = g >> 24, c = g | 0xFF000000u;
    if (j->fx_3d && ((wmask[idx] >> 5) & 1)) c = fade(c, j->fx_3d); /* el efecto, solo donde las ventanas lo permiten */
    if (ga < 255 && ((j->br.cnt >> 8 >> back_layer) & 1)) {
        uint32_t out = 0xFF000000u;
        for (int s = 0; s < 24; s += 8)
            out |= ((((c >> s) & 255) * ga + ((bc >> s) & 255) * (255 - ga)) / 255) << s;
        return out;
    }
    return c;
}

/* Un pixel 2D delantero con la mezcla pendiente: se mezcla con lo que haya debajo (3D o fondo) */
static uint32_t over_front(int idx, uint32_t below, int below_layer, const UpscaleJob *j) {
    if (front_pend[idx] && ((j->br.cnt >> 8 >> below_layer) & 1)) return mix(front_raw[idx], below, j->br.eva, j->br.evb);
    return front2d[idx];
}

static void upscale_row(void *ctx, int y) {
    UpscaleJob *j = ctx;
    int S = j->S, ow = W * S;
    uint32_t *row = j->buf + (size_t)y * S * ow;
    const uint32_t *b = back2d + y * W;
    const uint8_t *fid = front_id + y * W, *bid = back_id + y * W;
    if (!j->g3d) {
        for (int x = 0; x < W; x++) {
            uint32_t c = (j->has_3d && fid[x] != NO_LAYER) ? over_front(y * W + x, b[x], bid[x], j) : b[x];
            for (int i = 0; i < S; i++) row[x * S + i] = c;
        }
        for (int k = 1; k < S; k++) memcpy(row + (size_t)k * ow, row, sizeof(uint32_t) * ow);
        return;
    }
    for (int k = 0; k < S; k++) {
        uint32_t *out = row + (size_t)k * ow;
        const uint32_t *g = j->g3d + (size_t)(y * S + k) * ow;
        for (int x = 0; x < W; x++) {
            uint32_t bc = b[x];
            int idx = y * W + x;
            if (fid[x] != NO_LAYER && !front_pend[idx]) { /* lo normal: 2D opaco delante */
                uint32_t fc = front2d[idx];
                for (int i = 0; i < S; i++) out[x * S + i] = fc;
                continue;
            }
            for (int i = 0; i < S; i++) {
                uint32_t c = (wmask[idx] & 1) ? g[x * S + i] : 0; /* el 3D (BG0) tapado por las ventanas */
                if (c >> 24) {
                    c = (c >> 24) == 255 && !j->fx_3d ? c : over_3d(c, bc, bid[x], j, idx);
                    if (fid[x] != NO_LAYER) c = over_front(idx, c, 0, j);
                } else {
                    c = fid[x] != NO_LAYER ? over_front(idx, bc, bid[x], j) : bc;
                }
                out[x * S + i] = c;
            }
        }
    }
}

static int layer_fx(const Engine *e, int plane) { /* plano: 0-3 fondos, 4 sprites, 5 fondo de pantalla */
    uint32_t cnt = rd16(e->reg + 0x50), evy = rd16(e->reg + 0x54) & 31;
    int mode = (cnt >> 6) & 3;
    if (mode < 2 || !(cnt & (1u << plane))) return 0;
    if (evy > 16) evy = 16;
    return mode == 2 ? (int)evy : -(int)evy;
}

static void fx_log(const Engine *e) { /* que efectos 2D pide el juego de verdad (NFS_FX_LOG) */
    static uint32_t calls;
    if (!getenv("NFS_FX_LOG") || (calls++ / 2) % (getenv("NFS_FX_LOG")[0] == '2' ? 1 : 120)) return;
    int affine = 0, semi = 0, winobj = 0, bitmap = 0, mosaic = 0;
    for (int i = 0; i < 128; i++) {
        uint32_t a0 = rd16(e->oam + i * 8);
        if (!(a0 & 0x100) && (a0 & 0x200)) continue;
        if (a0 & 0x100) affine++;
        int m = (a0 >> 10) & 3;
        if (m == 1) semi++;
        if (m == 2) winobj++;
        if (m == 3) bitmap++;
        if (a0 & 0x1000) mosaic++;
    }
    uint32_t dispcnt = rd32(e->reg);
    fprintf(stderr, "[fx] motor %c: dispcnt %08x bldcnt %04x bldalpha %04x bldy %02x ventanas %s winin %04x winout %04x "
                    "mosaico %04x | sprites afines %d semitransp %d ventana %d mapa de bits %d mosaico %d%c",
            e->is_main ? 'A' : 'B', dispcnt, rd16(e->reg + 0x50), rd16(e->reg + 0x52), rd16(e->reg + 0x54) & 31,
            (dispcnt & 0xE000) ? "SI" : "no", rd16(e->reg + 0x48), rd16(e->reg + 0x4A), rd16(e->reg + 0x4C), affine, semi,
            winobj, bitmap, mosaic, 10);
}

/* Junta las capas 2D de un motor en back2d/front2d/... (a 256x192). Dice si el 3D va en medio. */
static void build_engine(Engine *e, int *has_3d_out, int *fx_3d_out, BlendRegs *br_out) {
    uint32_t dispcnt = rd32(e->reg);
    fx_log(e);
    int display = (dispcnt >> 16) & 3;
    int has_3d = 0, fx_3d = 0;
    BlendRegs br = blend_regs(e);
    memset(back_id, 5, sizeof back_id);
    memset(front_id, NO_LAYER, sizeof front_id);
    build_windows(e, dispcnt);
    if (display == 0 || (dispcnt & 0x80)) { /* pantalla apagada: blanco */
        for (int i = 0; i < W * H; i++) back2d[i] = 0xFFFFFFFFu;
    } else if (display == 2 && e->is_main) { /* imagen directa desde un banco de VRAM */
        const uint8_t *src = io_mem(3) + 0x800000 + ((dispcnt >> 18) & 3) * 0x20000;
        for (int i = 0; i < W * H; i++) back2d[i] = color(rd16(src + i * 2));
    } else {
        uint32_t raw_backdrop = color(rd16(e->bg_pal)), backdrop = fade(raw_backdrop, layer_fx(e, 5));
        int front = 0;
        for (int i = 0; i < W * H; i++) {
            back2d[i] = backdrop;
            back_raw[i] = raw_backdrop;
        }
        for (int prio = 3; prio >= 0; prio--) {
            for (int n = 3; n >= 0; n--) {
                if (!(dispcnt & (0x100u << n)) || (int)(rd16(e->reg + 8 + n * 2) & 3) != prio) continue;
                if (n == 0 && e->is_main && (dispcnt & 8)) { /* BG0 del motor A = salida del 3D */
                    has_3d = 1;
                    fx_3d = layer_fx(e, 0);
                    front = 1;
                    continue;
                }
                memset(layer, 0, sizeof layer);
                draw_bg(e, n, layer);
                merge_layer(&br, n, front);
            }
            if (dispcnt & 0x1000) {
                memset(layer, 0, sizeof layer);
                draw_objs(e, prio, layer, 0);
                merge_layer(&br, 4, front);
            }
        }
    }
    *has_3d_out = has_3d;
    *fx_3d_out = fx_3d;
    *br_out = br;
}

static void render_engine(Engine *e, uint32_t *buf, int S) {
    int total = W * S * H * S, ow = W * S, has_3d, fx_3d;
    BlendRegs br;
    build_engine(e, &has_3d, &fx_3d, &br);

    /* Ampliacion a la resolucion interna, mezclando con el 3D si lo hay (por filas, en paralelo) */
    int gw, gh;
    const uint32_t *g3d = has_3d ? gx_frame(&gw, &gh) : 0;
    if (g3d && gw != ow) g3d = 0;
    UpscaleJob job = {buf, g3d, S, has_3d, fx_3d, br};
    pool_run(H, upscale_row, &job);

    /* Brillo maestro: fundidos a blanco o a negro */
    uint32_t bright = rd16(e->reg + 0x6C);
    int mode = bright >> 14, factor = bright & 31;
    if (factor > 16) factor = 16;
    if ((mode == 1 || mode == 2) && factor) {
        for (int i = 0; i < total; i++) {
            uint32_t c = buf[i], out = 0;
            for (int s = 0; s < 24; s += 8) {
                int v = (c >> s) & 255;
                v = mode == 1 ? v + (255 - v) * factor / 16 : v - v * factor / 16;
                out |= (uint32_t)v << s;
            }
            buf[i] = out;
        }
    }
}

/* Con la grafica: se juntan las capas 2D aqui (a 256x192) y la grafica las mezcla con el 3D, a la
 * resolucion que toque, y las pinta en la ventana. Asi no hay que leer el 3D de vuelta. 0 = no se pudo. */
static uint32_t scr_back[2][W * H], scr_front[2][W * H], scr_raw[2][W * H], scr_ids[2][W * H];

static void pack_screen(int k, Engine *e, GpuScreen *g) {
    int has_3d, fx_3d;
    BlendRegs br;
    build_engine(e, &has_3d, &fx_3d, &br);
    memcpy(scr_back[k], back2d, sizeof back2d);
    memcpy(scr_front[k], front2d, sizeof front2d);
    memcpy(scr_raw[k], front_raw, sizeof front_raw);
    /* alfa: 0xF0 | 3D visible en este pixel (ventanas, bit 0 de WININ/WINOUT) | efectos permitidos (bit 5) << 1.
     * El juego hace la X blanca encima del coche con un sprite "ventana" con forma de X y el brillo del 3D solo
     * dentro de esa ventana; antes el brillo se aplicaba a todo el 3D y la escena entera salia blanqueada. */
    for (int i = 0; i < W * H; i++)
        scr_ids[k][i] = back_id[i] | (front_id[i] << 8) | (front_pend[i] << 16) |
                        (uint32_t)(0xF0 | (wmask[i] & 1) | (((wmask[i] >> 5) & 1) << 1)) << 24;
    uint32_t bright = rd16(e->reg + 0x6C);
    g->back = scr_back[k]; g->front = scr_front[k]; g->raw = scr_raw[k]; g->ids = scr_ids[k];
    g->use3d = has_3d; g->fx3d = fx_3d; g->cnt = (int)br.cnt; g->eva = br.eva; g->evb = br.evb;
    g->bright_mode = bright >> 14; g->bright_factor = (bright & 31) > 16 ? 16 : (bright & 31);
}

static GpuScreen last_ga, last_gb; /* el 2D de la ultima vez (con 120 fps se pinta dos veces el mismo) */
static int last_main_on_top, have_last;
static int present_packed(const int top_r[4], const int bot_r[4], void *hwnd, int win_w, int win_h) {
    GpuScreen ga = last_ga, gb = last_gb;
    GpuScreen *top = last_main_on_top ? &ga : &gb, *bot = last_main_on_top ? &gb : &ga;
    top->x = top_r[0]; top->y = top_r[1]; top->w = top_r[2]; top->h = top_r[3];
    bot->x = bot_r[0]; bot->y = bot_r[1]; bot->w = bot_r[2]; bot->h = bot_r[3];
    return gpu_present(top, bot, hwnd, win_w, win_h);
}
int video_present_gpu(const int top_r[4], const int bot_r[4], void *hwnd, int win_w, int win_h) {
    uint8_t *vram = io_mem(3), *pal = io_mem(2), *oam = io_mem(4);
    Engine a = {io_mem(0), vram, vram + 0x400000, pal, pal + 0x200, oam, vram + 0x880000, 1};
    Engine b = {io_mem(1), vram + 0x200000, vram + 0x600000, pal + 0x400, pal + 0x600, oam + 0x400, vram + 0x898000, 0};
    last_main_on_top = (io_reg(0x304) >> 15) & 1;
    pack_screen(0, &a, &last_ga);
    pack_screen(1, &b, &last_gb);
    have_last = 1;
    return present_packed(top_r, bot_r, hwnd, win_w, win_h);
}
/* Lo mismo sin volver a juntar las capas 2D (no han cambiado desde la ultima vez) */
int video_present_gpu_again(const int top_r[4], const int bot_r[4], void *hwnd, int win_w, int win_h) {
    if (!have_last) return video_present_gpu(top_r, bot_r, hwnd, win_w, win_h);
    return present_packed(top_r, bot_r, hwnd, win_w, win_h);
}

/* Dibuja las dos pantallas en out: (256*S) x (384*S) pixeles, la de arriba primero. */
void video_render(uint32_t *out) {
    int S = video_scale(), screen = W * S * H * S;
    uint8_t *vram = io_mem(3), *pal = io_mem(2), *oam = io_mem(4);
    /* Paletas extendidas: el juego las escribe por la direccion LCDC de su banco (E para el motor A,
     * H para el B) y luego cambia el banco a "paleta extendida". Se leen de esa direccion. */
    Engine a = {io_mem(0), vram, vram + 0x400000, pal, pal + 0x200, oam, vram + 0x880000, 1};
    Engine b = {io_mem(1), vram + 0x200000, vram + 0x600000, pal + 0x400, pal + 0x600, oam + 0x400, vram + 0x898000, 0};
    int main_on_top = (io_reg(0x304) >> 15) & 1; /* POWCNT1 bit 15 */
    render_engine(&a, out + (main_on_top ? 0 : screen), S);
    render_engine(&b, out + (main_on_top ? screen : 0), S);
}

/* ---- capturas en PNG (sin comprimir) para poder comprobar lo que se dibuja ------------- */
static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return crc;
}

static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t head[8] = {n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3]};
    uint32_t crc = crc32_update(0xFFFFFFFFu, head + 4, 4);
    crc = ~crc32_update(crc, data, n);
    uint8_t tail[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(head, 1, 8, f);
    fwrite(data, 1, n, f);
    fwrite(tail, 1, 4, f);
}

static void save_png(const char *path, const uint32_t *pixels, int w, int h, int with_alpha);
void video_save_png(const char *path, const uint32_t *pixels, int w, int h) { save_png(path, pixels, w, h, 0); }
/* Con canal alfa (pixeles 0xAARRGGBB): para volcar texturas */
void video_save_png_rgba(const char *path, const uint32_t *pixels, int w, int h) { save_png(path, pixels, w, h, 1); }

static void save_png(const char *path, const uint32_t *pixels, int w, int h, int with_alpha) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    int bpp = with_alpha ? 4 : 3;
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, with_alpha ? 6 : 2, 0, 0, 0};
    png_chunk(f, "IHDR", ihdr, 13);

    uint32_t row = 1 + w * bpp, raw = row * h;
    uint8_t *z = malloc(raw + (raw / 65535 + 1) * 5 + 6), *p = z;
    uint8_t *img = malloc(raw);
    for (int y = 0; y < h; y++) {
        img[y * row] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t c = pixels[y * w + x];
            img[y * row + 1 + x * bpp] = c >> 16;
            img[y * row + 2 + x * bpp] = c >> 8;
            img[y * row + 3 + x * bpp] = c;
            if (with_alpha) img[y * row + 4 + x * bpp] = c >> 24;
        }
    }
    *p++ = 0x78;
    *p++ = 0x01;
    uint32_t a1 = 1, a2 = 0;
    for (uint32_t i = 0; i < raw; i++) {
        a1 = (a1 + img[i]) % 65521;
        a2 = (a2 + a1) % 65521;
    }
    for (uint32_t pos = 0; pos < raw;) {
        uint32_t n = raw - pos > 65535 ? 65535 : raw - pos;
        *p++ = (pos + n == raw);
        *p++ = n & 255;
        *p++ = n >> 8;
        *p++ = ~n & 255;
        *p++ = (~n >> 8) & 255;
        memcpy(p, img + pos, n);
        p += n;
        pos += n;
    }
    *p++ = a2 >> 8; *p++ = a2; *p++ = a1 >> 8; *p++ = a1;
    png_chunk(f, "IDAT", z, (uint32_t)(p - z));
    png_chunk(f, "IEND", z, 0);
    fclose(f);
    free(z);
    free(img);
}
