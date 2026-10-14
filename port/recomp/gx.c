/* El chip 3D de la DS, emulado: recibe las ordenes de geometria del juego (matrices, vertices,
 * materiales...), monta los poligonos y los dibuja por software.
 *
 * El dibujado se hace a una resolucion interna configurable (NFS_3D_SCALE, por defecto 1 =
 * 256x192 como la consola): es la base de las mejoras graficas, porque el mismo codigo dibuja
 * a 2x, 4x... con correccion de perspectiva por pixel.
 *
 * El dibujado se reparte por franjas entre varios hilos del PC (NFS_THREADS), las texturas se
 * descodifican una vez por fotograma y se filtran (bilineal) salvo NFS_FILTER=0.
 *
 * Hecho: pila de matrices, transformacion y recorte, luces por vertice, generacion de
 * coordenadas de textura, los 7 formatos de textura, modos modulate/decal/toon/brillo,
 * transparencia (el alfa se guarda por pixel para mezclarlo con el 2D), prueba de alfa, bufer de
 * profundidad Z/W, niebla, sombras por volumen, lectura de matrices y pruebas de posicion.
 * Sin hacer: bordes, antialiasing de la consola, orden por Y de los translucidos, prueba de caja
 * real (siempre "visible"). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "sys.h"

int video_scale(void);
int video_filter(void);

typedef int32_t Mtx[16]; /* 4x4, coma fija 20.12, por filas; los vectores multiplican por la izquierda */

typedef struct Vtx { /* todo en float y seguido: el recorte interpola los VTX_FLOATS campos de golpe */
    float x, y, z, w;   /* espacio de recorte; despues, x e y de pantalla */
    float r, g, b;      /* 0..1 */
    float s, t;         /* en texeles */
    float nx, ny, nz;   /* normal, en el espacio de la camara (solo vertices iluminados) */
    float vx, vy, vz;   /* posicion en el espacio de la camara */
    float mx, my, mz;   /* normal tal como la mando el juego (espacio del modelo): de ella sale el reflejo */
} Vtx;
#define VTX_FLOATS 18

/* Luces y material vigentes cuando se envio un poligono: hacen falta para iluminar por pixel */
typedef struct LightState {
    uint32_t mask;
    float dir[4][3], half[4][3], col[4][3];
    float dif[3], amb[3], spe[3], emi[3];
    int shin_on;
    uint8_t shin[128];
} LightState;

typedef struct Poly {
    Vtx v[10];
    int n;
    uint32_t attr, tex, pal;
    int translucent;
    int lit;  /* todos sus vertices llevan normal: se puede iluminar por pixel */
    int ls;   /* indice en la tabla de estados de luz */
    int env;  /* textura de reflejo: coordenadas generadas a partir de la normal (pintura, cristales) */
    int glow; /* luz del coche (1 freno, 2 marcha atras): se dibuja sumando luz (gpu.inc.c) */
    float envp[8]; /* para recalcularlas: s y t de partida y las dos filas de la matriz de textura */
} Poly;

#define MAX_POLYS 65536
#define MAX_LSTATES 4096
static LightState *lstates;
static int ls_count, ls_dirty = 1;
static float cur_nx, cur_ny, cur_nz, cur_mx, cur_my, cur_mz;
static int cur_lit;
int video_pixel_light(void);
int video_tess(void);
int video_fog(void);
int video_shadows(void);
int video_shiny(void);
int video_bloom(void);
static Poly *polys;
static int poly_count;

static Mtx proj, proj_stack, pos, vec, tex_mtx, tex_stack, clip;
static Mtx pos_stack[32], vec_stack[32];
static int stack_ptr, proj_ptr, mtx_mode, clip_dirty = 1;

static uint32_t cur_attr, pending_attr, cur_tex, cur_pal;
static float cur_r = 1, cur_g = 1, cur_b = 1;
static int32_t raw_s, raw_t, tc_s, tc_t;
static int32_t last_x, last_y, last_z;
static float light_dir[4][3], light_half[4][3], light_col[4][3];
static float mat_dif[3], mat_amb[3], mat_spe[3], mat_emi[3];
static int shininess_on;
static uint8_t shininess[128];
static int prim_type = -1, prim_count;
static Vtx prim[4];
static int poly_lit; /* todos los vertices del poligono (o de la tira) en curso llevan normal */
static int vp_x1, vp_y1 = 0, vp_x2 = 255, vp_y2 = 191;
static uint32_t swap_flags;
static uint32_t vertex_total;

/* Resultado */
static int scale = 0, fb_w, fb_h;
static uint32_t *fb;   /* 0xAARRGGBB; alfa 0 = no hay 3D en ese pixel */
static uint32_t *fb_bufs[2]; /* se alternan: el del fotograma anterior sirve para los reflejos de la pintura */
static int fb_cur;
static float *zbuf;
/* Atributos por pixel, como el bufer de atributos de la consola: bits 0-5 = identificador del
 * poligono opaco que lo pinto (lo usan las sombras), bit 6 = lleva niebla, bit 7 = marca de sombra */
static uint8_t *abuf;
/* Identificador del ultimo poligono translucido que pinto cada pixel (0xFF = ninguno): la consola
 * no deja que un translucido pinte dos veces el mismo pixel con el mismo identificador (asi una
 * sombra o un humo de varias caras no se oscurece el doble donde se solapan) */
static uint8_t *tbuf;
#define A_FOG 0x40
#define A_STENCIL 0x80
static uint32_t frames_drawn;

/* ---- ordenes y sus parametros ---------------------------------------------------------- */
static const uint8_t PARAMS[256] = {
    [0x10] = 1, [0x11] = 0, [0x12] = 1, [0x13] = 1, [0x14] = 1, [0x15] = 0, [0x16] = 16, [0x17] = 12, [0x18] = 16,
    [0x19] = 12, [0x1A] = 9, [0x1B] = 3, [0x1C] = 3, [0x20] = 1, [0x21] = 1, [0x22] = 1, [0x23] = 2, [0x24] = 1,
    [0x25] = 1, [0x26] = 1, [0x27] = 1, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = 1, [0x30] = 1, [0x31] = 1,
    [0x32] = 1, [0x33] = 1, [0x34] = 32, [0x40] = 1, [0x41] = 0, [0x50] = 1, [0x60] = 1, [0x70] = 3, [0x71] = 2,
    [0x72] = 1};

static void identity(Mtx m) {
    memset(m, 0, sizeof(Mtx));
    m[0] = m[5] = m[10] = m[15] = 4096;
}

/* cur = m * cur */
static void mult(Mtx cur, const Mtx m) {
    Mtx t;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            int64_t a = 0;
            for (int k = 0; k < 4; k++) a += (int64_t)m[i * 4 + k] * cur[k * 4 + j];
            t[i * 4 + j] = (int32_t)(a >> 12);
        }
    memcpy(cur, t, sizeof t);
}

static void update_clip(void) {
    if (!clip_dirty) return;
    memcpy(clip, proj, sizeof clip);
    mult(clip, pos);
    clip_dirty = 0;
}

static void load_matrix(const Mtx m) {
    if (mtx_mode == 0) memcpy(proj, m, sizeof(Mtx));
    else if (mtx_mode == 3) memcpy(tex_mtx, m, sizeof(Mtx));
    else {
        memcpy(pos, m, sizeof(Mtx));
        if (mtx_mode == 2) memcpy(vec, m, sizeof(Mtx));
    }
    clip_dirty = 1;
}

static void mult_matrix(const Mtx m) {
    if (mtx_mode == 0) mult(proj, m);
    else if (mtx_mode == 3) mult(tex_mtx, m);
    else {
        mult(pos, m);
        if (mtx_mode == 2) mult(vec, m);
    }
    clip_dirty = 1;
}

/* ---- luces -------------------------------------------------------------------------------- */
static void rgb15(uint32_t c, float *out) {
    out[0] = (c & 31) / 31.0f;
    out[1] = ((c >> 5) & 31) / 31.0f;
    out[2] = ((c >> 10) & 31) / 31.0f;
}

static int32_t s10(uint32_t v) { return (int32_t)(v << 22) >> 22; }

static void do_normal(uint32_t p) {
    float n[3] = {s10(p) / 512.0f, s10(p >> 10) / 512.0f, s10(p >> 20) / 512.0f}, tn[3];
    cur_mx = n[0]; cur_my = n[1]; cur_mz = n[2];
    if ((cur_tex >> 30) == 2) { /* coordenadas de textura a partir de la normal (mapa de entorno) */
        int32_t nx = s10(p), ny = s10(p >> 10), nz = s10(p >> 20);
        tc_s = raw_s + (int32_t)(((int64_t)nx * tex_mtx[0] + (int64_t)ny * tex_mtx[4] + (int64_t)nz * tex_mtx[8]) >> 21);
        tc_t = raw_t + (int32_t)(((int64_t)nx * tex_mtx[1] + (int64_t)ny * tex_mtx[5] + (int64_t)nz * tex_mtx[9]) >> 21);
    }
    for (int j = 0; j < 3; j++) tn[j] = (n[0] * vec[j] + n[1] * vec[4 + j] + n[2] * vec[8 + j]) / 4096.0f;
    float len = sqrtf(tn[0] * tn[0] + tn[1] * tn[1] + tn[2] * tn[2]);
    if (len > 0) { cur_nx = tn[0] / len; cur_ny = tn[1] / len; cur_nz = tn[2] / len; cur_lit = 1; }
    float c[3] = {mat_emi[0], mat_emi[1], mat_emi[2]};
    for (int i = 0; i < 4; i++) {
        if (!(cur_attr & (1u << i))) continue;
        float d = -(light_dir[i][0] * tn[0] + light_dir[i][1] * tn[1] + light_dir[i][2] * tn[2]);
        if (d < 0) d = 0;
        float h = -(light_half[i][0] * tn[0] + light_half[i][1] * tn[1] + light_half[i][2] * tn[2]);
        if (h < 0) h = 0;
        h = 2 * h * h - 1;
        if (h < 0) h = 0;
        if (shininess_on) h = shininess[(int)(h * 127)] / 255.0f;
        for (int k = 0; k < 3; k++)
            c[k] += mat_spe[k] * light_col[i][k] * h + mat_dif[k] * light_col[i][k] * d + mat_amb[k] * light_col[i][k];
    }
    cur_r = c[0] > 1 ? 1 : c[0];
    cur_g = c[1] > 1 ? 1 : c[1];
    cur_b = c[2] > 1 ? 1 : c[2];
}

/* ---- poligonos ----------------------------------------------------------------------------- */
static int clip_plane(Vtx *in, int n, Vtx *out, int axis, float sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        Vtx *a = &in[i], *b = &in[(i + 1) % n];
        float ca = (&a->x)[axis] * sign, cb = (&b->x)[axis] * sign;
        float da = a->w - ca, db = b->w - cb; /* dentro si >= 0 */
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) {
            float t = da / (da - db);
            Vtx v;
            float *pa = &a->x, *pb = &b->x, *pv = &v.x;
            for (int k = 0; k < VTX_FLOATS; k++) pv[k] = pa[k] + (pb[k] - pa[k]) * t;
            out[m++] = v;
        }
    }
    return m;
}

/* Estado de luz actual, guardado en la tabla (se reutiliza mientras no cambie nada) */
static int current_light_state(void) {
    if (!ls_dirty && ls_count && lstates[ls_count - 1].mask == (cur_attr & 15)) return ls_count - 1;
    if (ls_count >= MAX_LSTATES) return ls_count - 1;
    LightState *l = &lstates[ls_count];
    l->mask = cur_attr & 15;
    memcpy(l->dir, light_dir, sizeof l->dir);
    memcpy(l->half, light_half, sizeof l->half);
    memcpy(l->col, light_col, sizeof l->col);
    memcpy(l->dif, mat_dif, sizeof l->dif);
    memcpy(l->amb, mat_amb, sizeof l->amb);
    memcpy(l->spe, mat_spe, sizeof l->spe);
    memcpy(l->emi, mat_emi, sizeof l->emi);
    l->shin_on = shininess_on;
    memcpy(l->shin, shininess, sizeof l->shin);
    ls_dirty = 0;
    return ls_count++;
}

/* Los poligonos se apuntan tal como llegan y se montan todos juntos al final del fotograma
 * (finish_polygons): asi la subdivision puede mirar que vertices comparten varios poligonos. */
double perf_3d_ms; /* tiempo acumulado en montar y dibujar el 3D (lo lee win.c con NFS_PERF) */
int perf_tex_flushes; /* veces que se han tirado las texturas descodificadas */
double perf_tex_ms;   /* tiempo descodificando texturas (NFS_SPIKES) */
int perf_tex_decoded, perf_tex_disk;
int perf_polys, perf_trans, perf_raw; /* poligonos del ultimo fotograma: dibujados, translucidos, enviados */ /* texturas descodificadas; archivos de texturas buscados en disco */

typedef struct RawPoly {
    Vtx v[4];
    int n, lit, ls;
    int notess;               /* viene de un modelo sustituto: ya trae sus poligonos, no se subdivide */
    int glow;
    int env;
    float envp[8];
    uint32_t attr, tex, pal;
    float proj[16];
    int vp[4];
} RawPoly;

#define MAX_RAW 32768
static RawPoly *raws;
static int raw_count;
static uint32_t raw_polys; /* poligonos tal como los envio el juego (sin subdividir) */
static float tess_round = 0.6f; /* fuerza del redondeo (NFS_TESS_ROUND, en tanto por ciento) */

static int cur_notess, cur_glow;
static void add_polygon(Vtx *src, int n, int lit) {
    raw_polys++;
    if (raw_count >= MAX_RAW) return;
    RawPoly *r = &raws[raw_count++];
    memcpy(r->v, src, sizeof(Vtx) * n);
    r->n = n;
    r->lit = lit;
    r->notess = cur_notess;
    r->glow = cur_glow;
    r->env = (cur_tex >> 30) == 2;
    if (r->env) { /* tc = raw + n * matriz (ver do_normal), en 1/16 de texel */
        r->envp[0] = (float)raw_s; r->envp[1] = (float)raw_t;
        r->envp[2] = tex_mtx[0] / 4096.0f; r->envp[3] = tex_mtx[4] / 4096.0f; r->envp[4] = tex_mtx[8] / 4096.0f;
        r->envp[5] = tex_mtx[1] / 4096.0f; r->envp[6] = tex_mtx[5] / 4096.0f; r->envp[7] = tex_mtx[9] / 4096.0f;
    }
    r->ls = lit ? current_light_state() : 0;
    r->attr = cur_attr;
    r->tex = cur_tex;
    r->pal = cur_pal;
    for (int i = 0; i < 16; i++) r->proj[i] = proj[i] / 4096.0f;
    r->vp[0] = vp_x1; r->vp[1] = vp_y1; r->vp[2] = vp_x2; r->vp[3] = vp_y2;
}

/* Recorta un poligono, lo pasa a pantalla y lo deja en la lista de dibujado */
static void emit_polygon(const RawPoly *st, Vtx *src, int n) {
    if (poly_count >= MAX_POLYS) return;
    Vtx a[10], b[10];
    memcpy(a, src, sizeof(Vtx) * n);
    /* Lo normal es que el poligono este entero dentro de la vista (no hay que recortar) o entero
     * fuera por un mismo lado (se descarta): solo el resto pasa por el recorte, que es lo caro. */
    int all_in = 1, out_mask = 63;
    for (int i = 0; i < n; i++) {
        int m = (a[i].x > a[i].w) | ((a[i].x < -a[i].w) << 1) | ((a[i].y > a[i].w) << 2) | ((a[i].y < -a[i].w) << 3) |
                ((a[i].z > a[i].w) << 4) | ((a[i].z < -a[i].w) << 5);
        if (m) all_in = 0;
        out_mask &= m;
    }
    if (out_mask) return;
    if (!all_in)
        for (int axis = 0; axis < 3 && n; axis++) {
            n = clip_plane(a, n, b, axis, 1);
            if (!n) break;
            n = clip_plane(b, n, a, axis, -1);
        }
    if (n < 3) return;
    float vw = (st->vp[2] - st->vp[0] + 1) * 0.5f, vh = (st->vp[3] - st->vp[1] + 1) * 0.5f;
    for (int i = 0; i < n; i++) {
        float iw = a[i].w != 0 ? 1.0f / a[i].w : 0;
        a[i].x = (a[i].x * iw + 1) * vw + st->vp[0];
        a[i].y = 192 - ((a[i].y * iw + 1) * vh + st->vp[1]);
        a[i].z = a[i].z * iw;
    }
    /* Orientacion: en pantalla (y hacia abajo), las caras delanteras salen con area negativa */
    float area = 0;
    for (int i = 0; i < n; i++) {
        Vtx *p = &a[i], *q = &a[(i + 1) % n];
        area += p->x * q->y - q->x * p->y;
    }
    int front = area < 0;
    if (area == 0) return;
    if (front ? !(st->attr & 0x80) : !(st->attr & 0x40)) return;

    Poly *poly = &polys[poly_count++];
    memcpy(poly->v, a, sizeof(Vtx) * n);
    poly->n = n;
    poly->attr = st->attr;
    poly->tex = st->tex;
    poly->pal = st->pal;
    poly->lit = st->lit;
    poly->ls = st->ls;
    poly->env = st->env;
    poly->glow = st->glow;
    memcpy(poly->envp, st->envp, sizeof poly->envp);
    int fmt = (st->tex >> 26) & 7, alpha = (st->attr >> 16) & 31;
    /* los poligonos de sombra van siempre con los translucidos, despues de los opacos */
    poly->translucent = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6 || ((st->attr >> 4) & 3) == 3;
}

/* ---- subdivision de los modelos iluminados ("teselado de Phong") --------------------------------
 * Cada triangulo se parte en 4^nivel y los puntos nuevos se empujan hacia la superficie curva que
 * definen las normales de los vertices, asi los modelos se redondean.
 *
 * Para que no se abran grietas entre poligonos vecinos, el empuje en un vertice tiene que ser el
 * mismo en todos los poligonos que lo comparten. Por eso no se usa la normal que trae cada poligono
 * (en una arista marcada del modelo, cada lado trae la suya), sino una por POSICION: la media de
 * todas las normales que llegan a ese punto. Y si esas normales discrepan mucho (arista viva), ese
 * vertice no empuja: la arista se queda recta. Para sombrear se siguen usando las normales reales. */
typedef struct Weld {
    int32_t key[3];
    float n[3];
    int count, used;
} Weld;

#define WELD_SLOTS 32768
static Weld *welds;

static Weld *weld_at(const Vtx *v, int create) {
    int32_t k[3] = {(int32_t)floorf(v->vx * 2048.0f + 0.5f), (int32_t)floorf(v->vy * 2048.0f + 0.5f),
                    (int32_t)floorf(v->vz * 2048.0f + 0.5f)};
    uint32_t h = (uint32_t)k[0] * 73856093u ^ (uint32_t)k[1] * 19349663u ^ (uint32_t)k[2] * 83492791u;
    for (int probe = 0; probe < 64; probe++) {
        Weld *w = &welds[(h + probe) % WELD_SLOTS];
        if (w->used && w->key[0] == k[0] && w->key[1] == k[1] && w->key[2] == k[2]) return w;
        if (!w->used) {
            if (!create) return 0;
            w->used = 1;
            w->key[0] = k[0]; w->key[1] = k[1]; w->key[2] = k[2];
            w->n[0] = w->n[1] = w->n[2] = 0;
            w->count = 0;
            return w;
        }
    }
    return 0;
}

static void tessellate(const RawPoly *st, const Vtx *t0, const Vtx *t1, const Vtx *t2, int level) {
    const Vtx *T[3] = {t0, t1, t2};
    float wn[3][3], wgt[3];
    for (int m = 0; m < 3; m++) { /* normal y peso de empuje de cada esquina, por posicion */
        Weld *w = weld_at(T[m], 0);
        wgt[m] = 0;
        wn[m][0] = T[m]->nx; wn[m][1] = T[m]->ny; wn[m][2] = T[m]->nz;
        if (!w || !w->count) continue;
        float len = sqrtf(w->n[0] * w->n[0] + w->n[1] * w->n[1] + w->n[2] * w->n[2]);
        float agree = len / (float)w->count; /* 1 = todas las normales coinciden; menos = arista viva */
        if (len <= 0) continue;
        wn[m][0] = w->n[0] / len; wn[m][1] = w->n[1] / len; wn[m][2] = w->n[2] / len;
        wgt[m] = (agree - 0.90f) / 0.08f;
        if (wgt[m] < 0) wgt[m] = 0;
        if (wgt[m] > 1) wgt[m] = 1;
    }
    int n = 1 << level;
    static Vtx grid[17][17];
    const float *pm = st->proj;
    for (int i = 0; i <= n; i++)
        for (int j = 0; j <= n - i; j++) {
            float b[3] = {(float)(n - i - j) / n, (float)i / n, (float)j / n};
            Vtx *v = &grid[i][j];
            float *pv = &v->x;
            for (int k = 0; k < VTX_FLOATS; k++) pv[k] = b[0] * (&t0->x)[k] + b[1] * (&t1->x)[k] + b[2] * (&t2->x)[k];
            float px = v->vx, py = v->vy, pz = v->vz, ox = 0, oy = 0, oz = 0;
            for (int m = 0; m < 3; m++) { /* empuje hacia el plano tangente de cada esquina */
                float d = (px - T[m]->vx) * wn[m][0] + (py - T[m]->vy) * wn[m][1] + (pz - T[m]->vz) * wn[m][2];
                float f = b[m] * wgt[m] * d;
                ox -= f * wn[m][0];
                oy -= f * wn[m][1];
                oz -= f * wn[m][2];
            }
            v->vx = px + ox * tess_round;
            v->vy = py + oy * tess_round;
            v->vz = pz + oz * tess_round;
            float len = sqrtf(v->nx * v->nx + v->ny * v->ny + v->nz * v->nz);
            if (len > 0) { v->nx /= len; v->ny /= len; v->nz /= len; }
            v->x = v->vx * pm[0] + v->vy * pm[4] + v->vz * pm[8] + pm[12];
            v->y = v->vx * pm[1] + v->vy * pm[5] + v->vz * pm[9] + pm[13];
            v->z = v->vx * pm[2] + v->vy * pm[6] + v->vz * pm[10] + pm[14];
            v->w = v->vx * pm[3] + v->vy * pm[7] + v->vz * pm[11] + pm[15];
        }
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n - i; j++) {
            Vtx q[3] = {grid[i][j], grid[i + 1][j], grid[i][j + 1]};
            emit_polygon(st, q, 3);
            if (j < n - i - 1) {
                Vtx q2[3] = {grid[i + 1][j], grid[i + 1][j + 1], grid[i][j + 1]};
                emit_polygon(st, q2, 3);
            }
        }
}

/* Al final del fotograma: monta la lista de dibujado a partir de los poligonos apuntados */
static void finish_polygons(void) {
    int level = video_tess();
    if (level > 0) { /* una normal media por posicion, con todos los poligonos iluminados */
        memset(welds, 0, sizeof(Weld) * WELD_SLOTS);
        for (int i = 0; i < raw_count; i++) {
            RawPoly *r = &raws[i];
            if (!r->lit) continue;
            for (int k = 0; k < r->n; k++) {
                Weld *w = weld_at(&r->v[k], 1);
                if (!w) continue;
                w->n[0] += r->v[k].nx; w->n[1] += r->v[k].ny; w->n[2] += r->v[k].nz;
                w->count++;
            }
        }
    }
    for (int i = 0; i < raw_count; i++) {
        RawPoly *r = &raws[i];
        if (level > 0 && r->lit && !r->notess && (r->n == 3 || r->n == 4)) {
            tessellate(r, &r->v[0], &r->v[1], &r->v[2], level);
            if (r->n == 4) tessellate(r, &r->v[0], &r->v[2], &r->v[3], level);
        } else {
            emit_polygon(r, r->v, r->n);
        }
    }
    raw_count = 0;
}

static void submit_vertex(int32_t x, int32_t y, int32_t z) {
    last_x = x; last_y = y; last_z = z;
    update_clip();
    vertex_total++;
    if (prim_type < 0) return;
    if ((cur_tex >> 30) == 3) { /* coordenadas de textura a partir de la posicion */
        tc_s = raw_s + (int32_t)(((int64_t)x * tex_mtx[0] + (int64_t)y * tex_mtx[4] + (int64_t)z * tex_mtx[8]) >> 24);
        tc_t = raw_t + (int32_t)(((int64_t)x * tex_mtx[1] + (int64_t)y * tex_mtx[5] + (int64_t)z * tex_mtx[9]) >> 24);
    }
    Vtx v;
    float c[4];
    for (int j = 0; j < 4; j++)
        c[j] = (float)(((int64_t)x * clip[j] + (int64_t)y * clip[4 + j] + (int64_t)z * clip[8 + j] +
                        (int64_t)4096 * clip[12 + j]) >> 12) / 4096.0f;
    v.x = c[0]; v.y = c[1]; v.z = c[2]; v.w = c[3];
    v.r = cur_r; v.g = cur_g; v.b = cur_b;
    v.s = tc_s / 16.0f;
    v.t = tc_t / 16.0f;
    v.nx = cur_nx; v.ny = cur_ny; v.nz = cur_nz;
    v.mx = cur_mx; v.my = cur_my; v.mz = cur_mz;
    v.vx = (float)(((int64_t)x * pos[0] + (int64_t)y * pos[4] + (int64_t)z * pos[8] + (int64_t)4096 * pos[12]) >> 12) / 4096.0f;
    v.vy = (float)(((int64_t)x * pos[1] + (int64_t)y * pos[5] + (int64_t)z * pos[9] + (int64_t)4096 * pos[13]) >> 12) / 4096.0f;
    v.vz = (float)(((int64_t)x * pos[2] + (int64_t)y * pos[6] + (int64_t)z * pos[10] + (int64_t)4096 * pos[14]) >> 12) / 4096.0f;
    if (prim_count == 0) poly_lit = 1;
    poly_lit = poly_lit && cur_lit;

    Vtx q[4];
    switch (prim_type) {
    case 0: /* triangulos */
        prim[prim_count++] = v;
        if (prim_count == 3) { add_polygon(prim, 3, poly_lit); prim_count = 0; }
        break;
    case 1: /* cuadrilateros */
        prim[prim_count++] = v;
        if (prim_count == 4) { add_polygon(prim, 4, poly_lit); prim_count = 0; }
        break;
    case 2: /* tira de triangulos */
        if (prim_count < 2) { prim[prim_count++] = v; break; }
        q[0] = prim[0]; q[1] = prim[1]; q[2] = v;
        if (prim_count & 1) { q[0] = prim[1]; q[1] = prim[0]; }
        add_polygon(q, 3, poly_lit);
        prim[0] = prim[1];
        prim[1] = v;
        prim_count++;
        break;
    default: /* tira de cuadrilateros: llegan de dos en dos */
        if (prim_count < 2) { prim[prim_count++] = v; break; }
        prim[prim_count++] = v;
        if (prim_count == 4) {
            q[0] = prim[0]; q[1] = prim[1]; q[2] = prim[3]; q[3] = prim[2];
            add_polygon(q, 4, poly_lit);
            prim[0] = prim[2];
            prim[1] = prim[3];
            prim_count = 2;
        }
        break;
    }
}

/* ---- texturas ------------------------------------------------------------------------------ */
static uint8_t *tex_slot[4], *pal_slot[8];

static void map_texture_memory(void) {
    uint8_t *vram = io_mem(3);
    uint32_t cnt_ad = io_reg(0x240), cnt_eg = io_reg(0x244);
    memset(tex_slot, 0, sizeof tex_slot);
    memset(pal_slot, 0, sizeof pal_slot);
    for (int bank = 0; bank < 4; bank++) { /* bancos A-D como textura: MST = 3, OFS = ranura */
        uint32_t c = (cnt_ad >> (bank * 8)) & 0xFF;
        if ((c & 0x80) && (c & 7) == 3) tex_slot[(c >> 3) & 3] = vram + 0x800000 + bank * 0x20000;
    }
    uint32_t e = cnt_eg & 0xFF, f = (cnt_eg >> 8) & 0xFF, g = (cnt_eg >> 16) & 0xFF;
    if ((e & 0x80) && (e & 7) == 3)
        for (int i = 0; i < 4; i++) pal_slot[i] = vram + 0x880000 + i * 0x4000;
    if ((f & 0x80) && (f & 7) == 3) pal_slot[((f >> 3) & 1) * 4 + ((f >> 4) & 1)] = vram + 0x890000;
    if ((g & 0x80) && (g & 7) == 3) pal_slot[((g >> 3) & 1) * 4 + ((g >> 4) & 1)] = vram + 0x894000;
}

static uint32_t tex8(uint32_t addr) {
    uint8_t *s = tex_slot[(addr >> 17) & 3];
    return s ? s[addr & 0x1FFFF] : 0;
}
static uint32_t tex16(uint32_t addr) { return tex8(addr) | (tex8(addr + 1) << 8); }
static uint32_t pal16(uint32_t addr) {
    uint8_t *s = pal_slot[(addr >> 14) & 7];
    return s ? s[addr & 0x3FFF] | (s[(addr & 0x3FFF) + 1] << 8) : 0x7C1F;
}

/* Color de un texel, como 0xAARRGGBB */
static uint32_t decode_texel(uint32_t tp, uint32_t palbase, int w, int s, int t) {
    uint32_t base = (tp & 0xFFFF) * 8, fmt = (tp >> 26) & 7;
    uint32_t pal = palbase * (fmt == 2 ? 8 : 16), c = 0, i, a = 255;
    switch (fmt) {
    case 1: i = tex8(base + t * w + s); c = pal16(pal + (i & 31) * 2); a = (i >> 5) * 255 / 7; break;
    case 6: i = tex8(base + t * w + s); c = pal16(pal + (i & 7) * 2); a = (i >> 3) * 255 / 31; break;
    case 2: i = (tex8(base + (t * w + s) / 4) >> ((s & 3) * 2)) & 3; c = pal16(pal + i * 2); if (!i && (tp & 0x20000000)) a = 0; break;
    case 3: i = (tex8(base + (t * w + s) / 2) >> ((s & 1) * 4)) & 15; c = pal16(pal + i * 2); if (!i && (tp & 0x20000000)) a = 0; break;
    case 4: i = tex8(base + t * w + s); c = pal16(pal + i * 2); if (!i && (tp & 0x20000000)) a = 0; break;
    case 7: c = tex16(base + (t * w + s) * 2); a = (c & 0x8000) ? 255 : 0; break;
    case 5: { /* comprimida en bloques de 4x4 */
        uint32_t block = base + ((t / 4) * (w / 4) + s / 4) * 4;
        uint32_t bits = (tex8(block + (t & 3)) >> ((s & 3) * 2)) & 3;
        uint32_t info_addr = 0x20000 + ((block & 0x1FFFF) / 2) + ((block >> 17) == 2 ? 0x10000 : 0);
        uint32_t info = tex16(info_addr), pofs = pal + (info & 0x3FFF) * 4, mode = info >> 14;
        uint32_t c0 = pal16(pofs), c1 = pal16(pofs + 2);
        if (bits == 0) c = c0;
        else if (bits == 1) c = c1;
        else if (mode == 2) c = pal16(pofs + bits * 2);
        else if (mode == 0) { if (bits == 2) c = pal16(pofs + 4); else a = 0; }
        else {
            int r0 = c0 & 31, g0 = (c0 >> 5) & 31, b0 = (c0 >> 10) & 31, r1 = c1 & 31, g1 = (c1 >> 5) & 31, b1 = (c1 >> 10) & 31;
            if (mode == 1) {
                if (bits == 2) c = ((r0 + r1) / 2) | (((g0 + g1) / 2) << 5) | (((b0 + b1) / 2) << 10); else a = 0;
            } else if (bits == 2) c = ((r0 * 5 + r1 * 3) / 8) | (((g0 * 5 + g1 * 3) / 8) << 5) | (((b0 * 5 + b1 * 3) / 8) << 10);
            else c = ((r0 * 3 + r1 * 5) / 8) | (((g0 * 3 + g1 * 5) / 8) << 5) | (((b0 * 3 + b1 * 5) / 8) << 10);
        }
        break;
    }
    default: return 0xFFFFFFFFu;
    }
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (a << 24) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

/* Cada textura se descodifica una vez a una imagen normal de 32 bits, con sus reducciones
 * ("mipmaps") para que de lejos no parpadee. Si existe una version ampliada de esa textura en la
 * carpeta de texturas del usuario, se usa esa en su lugar. */
#define MAX_MIPS 12
typedef struct TexImage {
    uint32_t tex, pal, stamp;
    int w, h;                 /* tamano original, en texeles */
    int k;                    /* cuantas veces mas grande es la imagen que se usa (1 = la original) */
    int levels;
    const uint32_t *mip[MAX_MIPS];
    int mw[MAX_MIPS], mh[MAX_MIPS];
    uint32_t *own;            /* memoria propia (cuando no es una textura sustituta) */
    int has_alpha;            /* tiene texeles no opacos (recortes): no puede ir en la pasada de profundidad */
    int own_size;
    uint32_t decoded_at, checked_at; /* fotograma (de VRAM) en que se descodifico / se comprobo */
    uint32_t rng[8][2];       /* memoria de VRAM que lee (texeles, bloques 4x4, paleta) */
    int nranges;
    struct Repl *pending;     /* sustituta que se esta cargando */
    uint32_t version, gl_version; /* cambia cada vez que cambia la imagen; la grafica la vuelve a subir */
    unsigned int gl;          /* textura en la grafica (gpu.inc.c) */
    uint64_t hash;            /* huella del contenido (el nombre de su archivo en la carpeta de texturas) */
} TexImage;

/* Las imagenes descodificadas valen mientras el juego no escriba en la memoria que leen (ver
 * gx_vram_written mas abajo). Si cambia el reparto de bancos de VRAM, se tiran todas. */
#define TEX_CACHE 1024
static TexImage tex_cache[TEX_CACHE];
static uint32_t tex_generation = 1;

/* Reducciones sucesivas a la mitad; los colores se pesan por su alfa. Devuelve cuantos niveles hay. */
static int build_mips(uint32_t *base, int w, int h, const uint32_t **mip, int *mw, int *mh) {
    int levels = 1;
    uint32_t *src = base;
    mip[0] = base; mw[0] = w; mh[0] = h;
    while ((w > 1 || h > 1) && levels < MAX_MIPS) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        uint32_t *dst = src + w * h;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                uint32_t a = 0, r = 0, g = 0, b = 0;
                for (int j = 0; j < 2; j++)
                    for (int i = 0; i < 2; i++) {
                        int sx = x * 2 + i < w ? x * 2 + i : w - 1, sy = y * 2 + j < h ? y * 2 + j : h - 1;
                        uint32_t c = src[sy * w + sx], ca = c >> 24;
                        a += ca; r += ((c >> 16) & 255) * ca; g += ((c >> 8) & 255) * ca; b += (c & 255) * ca;
                    }
                dst[y * nw + x] = a ? ((a / 4) << 24) | ((r / a) << 16) | ((g / a) << 8) | (b / a) : 0;
            }
        src = dst; w = nw; h = nh;
        mip[levels] = dst; mw[levels] = w; mh[levels] = h;
        levels++;
    }
    return levels;
}

/* ---- texturas sustitutas (carpeta del usuario, fuera del proyecto) -------------------------------
 * Cada textura se identifica por una huella de su contenido ya descodificado. En
 * <carpeta>\original se vuelcan como PNG (con NFS_TEXDUMP=1) y de <carpeta>\pack se cargan las
 * ampliadas (archivos .tex que genera tools/textures/upscale.py).
 *
 * Para no dar tirones: al empezar se hace una lista de las que hay en pack\ (asi no se busca en
 * el disco cada textura nueva, que con las animadas son muchas por segundo) y cada sustituta se
 * carga en un hilo aparte la primera vez que hace falta; mientras tanto se usa la original. */
void video_save_png_rgba(const char *path, const uint32_t *pixels, int w, int h);

typedef struct Repl {
    uint64_t hash;
    int w, h;
    volatile LONG state;      /* 0 libre, 1 en el disco, 3 cargandose, 2 cargada, 4 no se pudo cargar */
    int k, levels;
    const uint32_t *mip[MAX_MIPS];
    int mw[MAX_MIPS], mh[MAX_MIPS];
} Repl;

static Repl *repl;            /* tabla con las sustitutas que hay en pack\ (huella -> datos) */
static int repl_slots;
static const char *tex_dir;
static int tex_dump, tex_use = -1, tex_dumped;
static volatile LONG tex_loaded;
static uint64_t *dumped;      /* huellas ya volcadas en esta partida (NFS_TEXDUMP) */
#define DUMPED_SLOTS 65536

static uint64_t tex_hash(const uint32_t *px, int w, int h) {
    uint64_t hsh = 1469598103934665603ull ^ (uint64_t)(w * 4099 + h);
    for (int i = 0; i < w * h; i++) {
        hsh ^= px[i];
        hsh *= 1099511628211ull;
    }
    return hsh;
}

static Repl *repl_slot(uint64_t hsh, int create) {
    for (int probe = 0; probe < repl_slots; probe++) {
        Repl *r = &repl[(hsh + probe) % (uint64_t)repl_slots];
        if (r->state && r->hash == hsh) return r;
        if (!r->state) {
            if (!create) return 0;
            r->hash = hsh;
            return r;
        }
    }
    return 0;
}

/* Hilo que carga las sustitutas pedidas */
static CRITICAL_SECTION load_lock;
static HANDLE load_event;
#define LOAD_QUEUE 4096
static Repl *load_queue[LOAD_QUEUE];
static int load_head, load_tail;

static void load_one(Repl *r) {
    char path[600];
    snprintf(path, sizeof path, "%s/pack/%016llx_%dx%d.tex", tex_dir, (unsigned long long)r->hash, r->w, r->h);
    FILE *f = fopen(path, "rb");
    LONG result = 4;
    if (f) {
        uint32_t head[3];
        if (fread(head, 4, 3, f) == 3 && head[0] == 0x3158544Eu && head[1] % (uint32_t)r->w == 0 &&
            head[2] % (uint32_t)r->h == 0) {
            int rw = (int)head[1], rh = (int)head[2];
            uint32_t *data = malloc(sizeof(uint32_t) * rw * rh * 2);
            if (data && fread(data, 4, (size_t)rw * rh, f) == (size_t)rw * rh) {
                r->k = rw / r->w;
                r->levels = build_mips(data, rw, rh, r->mip, r->mw, r->mh);
                result = 2;
            } else {
                free(data);
            }
        }
        fclose(f);
    }
    MemoryBarrier();
    InterlockedExchange(&r->state, result); /* el dibujado la ve entera o no la ve */
    if (result == 2) InterlockedIncrement(&tex_loaded);
}

static DWORD WINAPI loader_thread(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(load_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&load_lock);
            Repl *r = load_head != load_tail ? load_queue[load_head] : 0;
            if (r) load_head = (load_head + 1) % LOAD_QUEUE;
            LeaveCriticalSection(&load_lock);
            if (!r) break;
            load_one(r);
        }
    }
    return 0;
}

static void request_load(Repl *r) {
    EnterCriticalSection(&load_lock);
    if ((load_tail + 1) % LOAD_QUEUE != load_head) {
        load_queue[load_tail] = r;
        load_tail = (load_tail + 1) % LOAD_QUEUE;
        r->state = 3;
    }
    LeaveCriticalSection(&load_lock);
    SetEvent(load_event);
}

static void textures_init(void) {
    static char def_tex[512];
    snprintf(def_tex, sizeof def_tex, "%s/nfs-nitro-textures", getenv("USERPROFILE") ? getenv("USERPROFILE") : ".");
    tex_dir = getenv("NFS_TEXTURES") ? getenv("NFS_TEXTURES") : def_tex;
    tex_dump = getenv("NFS_TEXDUMP") != 0;
    tex_use = !getenv("NFS_TEXPACK") || atoi(getenv("NFS_TEXPACK")) != 0;
    if (tex_dump) dumped = calloc(DUMPED_SLOTS, sizeof(uint64_t));
    if (!tex_use) return;
    /* Lista de las sustitutas: los nombres son <huella>_<ancho>x<alto>.tex */
    char pattern[600];
    snprintf(pattern, sizeof pattern, "%s/pack/*.tex", tex_dir);
    WIN32_FIND_DATAA fd;
    int count = 0;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do count++; while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    repl_slots = count * 2 + 64;
    repl = calloc(repl_slots, sizeof(Repl));
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long hsh;
            int w, hh;
            if (sscanf(fd.cFileName, "%16llx_%dx%d.tex", &hsh, &w, &hh) != 3 || w <= 0 || hh <= 0) continue;
            Repl *r = repl_slot(hsh, 1);
            if (!r || r->state) continue;
            r->w = w;
            r->h = hh;
            r->state = 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    InitializeCriticalSection(&load_lock);
    load_event = CreateEventA(0, FALSE, FALSE, 0);
    CreateThread(0, 0, loader_thread, 0, 0, 0);
    fprintf(stderr, "[texturas] %d sustitutas en %s/pack%c", count, tex_dir, 10);
}

/* Devuelve la sustituta de esta textura (quiza aun cargandose: mirar state), o 0 si no hay */
static Repl *find_replacement(const uint32_t *px, int w, int h, uint64_t hsh) {
    if (tex_use < 0) textures_init();
    if (!tex_use && !tex_dump) return 0;
    if (tex_dump && dumped) {
        for (int probe = 0; probe < 64; probe++) {
            uint64_t *d = &dumped[(hsh + probe) % DUMPED_SLOTS];
            if (*d == hsh) break;
            if (!*d) {
                *d = hsh;
                char path[600];
                snprintf(path, sizeof path, "%s/original/%016llx_%dx%d.png", tex_dir, (unsigned long long)hsh, w, h);
                FILE *probe_file = fopen(path, "rb");
                if (probe_file) fclose(probe_file);
                else { video_save_png_rgba(path, px, w, h); tex_dumped++; }
                break;
            }
        }
    }
    if (!tex_use || !repl) return 0;
    Repl *r = repl_slot(hsh, 0);
    if (!r || r->w != w || r->h != h || r->state == 4) return 0;
    if (r->state == 1) { perf_tex_disk++; request_load(r); }
    return r;
}

/* Lista de texturas de coches: se apuntan (una vez cada una) las que se dibujan sobre poligonos
 * iluminados, que en este juego son casi solo los coches. Va a <carpeta de texturas>\coches.txt y
 * la usa tools/textures/upscale.py para ampliar esas con un modelo de IA mejor para coches.
 * NFS_TEXLIST=archivo la manda a otro sitio; NFS_TEXLIST=0 la apaga. */
static void seen_add(uint64_t *seen, uint64_t h, int *added) {
    *added = 0;
    for (int probe = 0; probe < 64; probe++) {
        uint64_t *s = &seen[(h + probe) & 65535];
        if (*s == h) return;
        if (!*s) { *s = h; *added = 1; return; }
    }
}

static void note_lit_texture(const TexImage *t) {
    static FILE *f;
    static uint64_t *seen;
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("NFS_TEXLIST");
        char path[600];
        off = e && !strcmp(e, "0");
        if (!off) {
            if (tex_use < 0) textures_init();
            if (e) snprintf(path, sizeof path, "%s", e);
            else snprintf(path, sizeof path, "%s/coches.txt", tex_dir);
            seen = calloc(65536, sizeof(uint64_t));
            FILE *old = fopen(path, "r"); /* las ya apuntadas en otras partidas no se repiten */
            if (old && seen) {
                char line[64];
                unsigned long long h;
                int added;
                while (fgets(line, sizeof line, old))
                    if (sscanf(line, "%16llx", &h) == 1) seen_add(seen, h, &added);
            }
            if (old) fclose(old);
            f = fopen(path, "ab");
            if (!f || !seen) off = 1;
        }
    }
    if (off) return;
    int added;
    seen_add(seen, t->hash, &added);
    if (added) {
        fprintf(f, "%016llx_%dx%d\n", (unsigned long long)t->hash, t->w, t->h);
        fflush(f);
    }
}

/* ---- validez de las texturas descodificadas ---------------------------------------------------
 * io.c avisa de cada escritura en la memoria de texturas y paletas (gx_vram_written): se apunta en
 * que "pagina" de 4 KB fue y en que fotograma. Una textura descodificada sigue valiendo mientras
 * no se haya escrito en ninguna de las paginas que lee (sus texeles, sus datos de bloques 4x4 y su
 * paleta). Antes se tiraban TODAS las texturas con cualquier escritura: con una textura animada,
 * todas se volvian a descodificar en cada fotograma. */
#define VRAM_TEX_BASE 0x800000u
#define VRAM_PAGES (0x100000 >> 12)
static uint32_t page_stamp[VRAM_PAGES];
static uint32_t vram_epoch = 2, vram_last_write = 1;

void gx_vram_written(uint32_t flat) {
    if (flat - VRAM_TEX_BASE < 0x100000u) {
        page_stamp[(flat - VRAM_TEX_BASE) >> 12] = vram_epoch;
        vram_last_write = vram_epoch;
    }
}

static void add_range(TexImage *t, uint8_t *const *slots, int slot_bits, int nslots, uint32_t addr, uint32_t len) {
    uint8_t *vram = io_mem(3);
    while (len && t->nranges < 8) {
        uint32_t slot_size = 1u << slot_bits, off = addr & (slot_size - 1);
        uint32_t n = slot_size - off < len ? slot_size - off : len;
        uint8_t *s = slots[(addr >> slot_bits) & (nslots - 1)];
        if (s) {
            uint32_t flat = (uint32_t)(s - vram) + off;
            t->rng[t->nranges][0] = flat;
            t->rng[t->nranges][1] = flat + n;
            t->nranges++;
        }
        addr += n;
        len -= n;
    }
}

/* Que memoria lee una textura: para saber cuando hay que volver a descodificarla */
static void texture_ranges(TexImage *t, uint32_t tp, uint32_t pal) {
    static const int bits[8] = {0, 8, 2, 4, 8, 2, 8, 16};
    static const uint32_t pal_bytes[8] = {0, 64, 8, 32, 512, 0x10000, 16, 0};
    uint32_t fmt = (tp >> 26) & 7, base = (tp & 0xFFFF) * 8, texels = (uint32_t)(t->w * t->h);
    t->nranges = 0;
    add_range(t, tex_slot, 17, 4, base, texels * bits[fmt] / 8);
    if (fmt == 5) add_range(t, tex_slot, 17, 4, 0x20000 + ((base & 0x1FFFF) / 2) + ((base >> 17) == 2 ? 0x10000 : 0), texels / 8);
    if (pal_bytes[fmt]) add_range(t, pal_slot, 14, 8, pal * (fmt == 2 ? 8 : 16), pal_bytes[fmt]);
}

static int texture_still_valid(const TexImage *t) {
    for (int i = 0; i < t->nranges; i++) {
        uint32_t lo = t->rng[i][0], hi = t->rng[i][1];
        if (hi <= VRAM_TEX_BASE || lo >= VRAM_TEX_BASE + 0x100000) continue;
        if (lo < VRAM_TEX_BASE) lo = VRAM_TEX_BASE;
        for (uint32_t p = (lo - VRAM_TEX_BASE) >> 12; p <= (hi - 1 - VRAM_TEX_BASE) >> 12 && p < VRAM_PAGES; p++)
            if (page_stamp[p] > t->decoded_at) return 0;
    }
    return 1;
}

static void use_replacement(TexImage *t, const Repl *r) {
    t->version++;
    t->k = r->k;
    t->levels = r->levels;
    memcpy(t->mip, r->mip, sizeof t->mip);
    memcpy(t->mw, r->mw, sizeof t->mw);
    memcpy(t->mh, r->mh, sizeof t->mh);
}

static void decode_texture(TexImage *t, uint32_t tp, uint32_t pal) {
    LARGE_INTEGER q0, q1, qf;
    QueryPerformanceCounter(&q0);
    perf_tex_decoded++;
    t->version++;
    int w = 8 << ((tp >> 20) & 7), h = 8 << ((tp >> 23) & 7);
    if (t->own_size < w * h * 2) {
        t->own = realloc(t->own, sizeof(uint32_t) * w * h * 2);
        t->own_size = w * h * 2;
    }
    t->tex = tp & 0x3FFFFFFFu;
    t->pal = pal;
    t->stamp = tex_generation;
    t->w = w;
    t->h = h;
    t->decoded_at = t->checked_at = vram_epoch;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) t->own[y * w + x] = decode_texel(tp, pal, w, x, y);
    t->has_alpha = 0;
    for (int i = 0; i < w * h; i++)
        if ((t->own[i] >> 24) != 255) { t->has_alpha = 1; break; }
    texture_ranges(t, tp, pal);
    t->hash = tex_hash(t->own, w, h);
    Repl *r = find_replacement(t->own, w, h, t->hash);
    t->pending = 0;
    if (r && r->state == 2) {
        use_replacement(t, r);
    } else {
        t->pending = r; /* se esta cargando: de momento, la original */
        t->k = 1;
        t->levels = build_mips(t->own, w, h, t->mip, t->mw, t->mh);
    }
    QueryPerformanceCounter(&q1);
    QueryPerformanceFrequency(&qf);
    perf_tex_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000 / (double)qf.QuadPart;
}

static const TexImage *get_texture(uint32_t tp, uint32_t pal) {
    if (((tp >> 26) & 7) == 0) return 0;
    uint32_t key = (tp & 0x3FFFFFFFu) * 2654435761u + pal * 40503u;
    for (int probe = 0; probe < 32; probe++) {
        TexImage *t = &tex_cache[(key + probe) % TEX_CACHE];
        if (t->stamp == tex_generation && t->tex == (tp & 0x3FFFFFFFu) && t->pal == pal) {
            if (t->checked_at < vram_last_write) { /* se ha escrito algo desde la ultima vez: mirar si le toca */
                if (!texture_still_valid(t)) { decode_texture(t, tp, pal); return t; }
                t->checked_at = vram_epoch;
            }
            if (t->pending && t->pending->state == 2) { use_replacement(t, t->pending); t->pending = 0; }
            return t;
        }
        if (t->stamp == tex_generation) continue; /* ocupada por otra textura */
        decode_texture(t, tp, pal);
        return t;
    }
    return 0;
}

static inline int wrap_coord(int v, int size, int repeat, int flip) {
    if (repeat) {
        if (flip && (v & size)) return size - 1 - (v & (size - 1));
        return v & (size - 1);
    }
    return v < 0 ? 0 : v >= size ? size - 1 : v;
}

/* ---- dibujado ------------------------------------------------------------------------------- */
typedef struct RenderState {
    int wbuffer, alpha_test, blend, filter, pixel_light;
    int shadows;            /* dibujar las sombras (NFS_SHADOWS=0 las quita) */
    int env_pixel;          /* reflejo calculado en cada pixel (va con la luz por pixel) */
    int shiny;              /* barniz, reflejo del cielo y Fresnel en los coches (NFS_SHINY) */
    float sky_h[3], sky_z[3]; /* color del cielo en el horizonte y arriba, para el reflejo de la pintura */
    const uint32_t *prev;   /* imagen 3D del fotograma anterior: lo que se refleja en la pintura */
    int bloom;              /* resplandor de las zonas muy brillantes (NFS_BLOOM) */
    int highlight;          /* DISP3DCNT bit 1: el modo 2 de los poligonos es "brillo" (si no, "toon") */
    float toon[32][3];      /* tabla de tonos (0x04000380), indexada por el rojo del color de vertice */
    int fog, fog_alpha_only, fog_step, fog_offset; /* niebla (DISP3DCNT bits 6-11, FOG_OFFSET) */
    float fog_rgb[3], fog_a;
    uint8_t fog_table[32];
    float clear_depth15;    /* profundidad del fondo (CLEAR_DEPTH), en unidades de 15 bits */
} RenderState;

/* how: 0 = solo profundidad; 1 = pintar solo donde gano la pasada de profundidad (sin escribirla);
 *      2 = normal (prueba y escribe profundidad) */
static void draw_triangle(const Poly *p, const TexImage *ti, const Vtx *v0, const Vtx *v1, const Vtx *v2,
                          const RenderState *ds, int band_lo, int band_hi, int how) {
    float S = (float)scale;
    float x0 = v0->x * S, y0 = v0->y * S, x1 = v1->x * S, y1 = v1->y * S, x2 = v2->x * S, y2 = v2->y * S;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area == 0) return;
    int minx = (int)floorf(fminf(x0, fminf(x1, x2))), maxx = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
    int miny = (int)floorf(fminf(y0, fminf(y1, y2))), maxy = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
    if (minx < 0) minx = 0;
    if (miny < band_lo) miny = band_lo;
    if (maxx > fb_w - 1) maxx = fb_w - 1;
    if (maxy > band_hi - 1) maxy = band_hi - 1;
    if (minx > maxx || miny > maxy) return;
    float iw0 = 1 / v0->w, iw1 = 1 / v1->w, iw2 = 1 / v2->w, inv_area = 1 / area;
    int mode = (p->attr >> 4) & 3;
    float poly_alpha = ((p->attr >> 16) & 31) / 31.0f;
    int depth_equal = (p->attr >> 14) & 1, write_depth = !p->translucent || (p->attr & 0x800);
    int rep_s = (p->tex >> 16) & 1, rep_t = (p->tex >> 17) & 1, flip_s = (p->tex >> 18) & 1, flip_t = (p->tex >> 19) & 1;
    /* Las funciones de borde son lineales: se calculan al principio de cada fila y se van sumando */
    float step0 = (y1 - y2) * inv_area, step1 = (y2 - y0) * inv_area;
    /* Derivadas (constantes en el triangulo) de s/w, t/w y 1/w respecto a x e y de pantalla: con ellas
     * se sabe en cada pixel cuantos texeles abarca, para elegir el nivel de reduccion de la textura. */
    float ys0 = (x2 - x1) * inv_area, ys1 = (x0 - x2) * inv_area;
    float a0s = v0->s * iw0, a1s = v1->s * iw1, a2s = v2->s * iw2, a0t = v0->t * iw0, a1t = v1->t * iw1, a2t = v2->t * iw2;
    float dsdx = step0 * (a0s - a2s) + step1 * (a1s - a2s), dsdy = ys0 * (a0s - a2s) + ys1 * (a1s - a2s);
    float dtdx = step0 * (a0t - a2t) + step1 * (a1t - a2t), dtdy = ys0 * (a0t - a2t) + ys1 * (a1t - a2t);
    float dwdx = step0 * (iw0 - iw2) + step1 * (iw1 - iw2), dwdy = ys0 * (iw0 - iw2) + ys1 * (iw1 - iw2);
    float tex_k = ti ? (float)ti->k : 1;
    float alpha_ref = ds->alpha_test >= 0 ? ds->alpha_test / 31.0f : -1;
    const LightState *ls = (p->lit && ds->pixel_light) ? &lstates[p->ls] : 0;
    /* Sombras de la consola (volumenes): un poligono de sombra con identificador 0 es la "mascara"
     * (marca los pixeles donde falla la prueba de profundidad: el suelo queda dentro del volumen);
     * los demas pintan la sombra solo en los pixeles marcados que no sean de su mismo objeto. */
    int shadow = mode == 3 ? (((p->attr >> 24) & 63) ? 2 : 1) : 0;
    uint8_t poly_id = (p->attr >> 24) & 63, poly_fog = (p->attr & 0x8000) ? A_FOG : 0;
    int toon_mode = mode == 2;

    for (int y = miny; y <= maxy; y++) {
        float py = y + 0.5f, px = minx + 0.5f;
        float e0 = ((x1 - px) * (y2 - py) - (x2 - px) * (y1 - py)) * inv_area;
        float e1 = ((x2 - px) * (y0 - py) - (x0 - px) * (y2 - py)) * inv_area;
        float *zrow = &zbuf[y * fb_w];
        uint32_t *frow = &fb[y * fb_w];
        uint8_t *arow = &abuf[y * fb_w], *trow = &tbuf[y * fb_w];
        for (int x = minx; x <= maxx; x++, e0 += step0, e1 += step1) {
            float e2 = 1 - e0 - e1;
            if (e0 < 0 || e1 < 0 || e2 < 0) continue;
            float iw = e0 * iw0 + e1 * iw1 + e2 * iw2;
            float w = 1 / iw;
            float depth = ds->wbuffer ? w : (e0 * v0->z + e1 * v1->z + e2 * v2->z);
            if (how == 0) {
                if (depth < zrow[x]) zrow[x] = depth;
                continue;
            }
            if (shadow == 1) { /* mascara: solo marca */
                if (depth >= zrow[x]) arow[x] |= A_STENCIL;
                continue;
            }
            /* sombra: solo donde marco la mascara, y no sobre el propio objeto (mismo identificador) */
            if (shadow == 2 && (!(arow[x] & A_STENCIL) || (arow[x] & 63) == poly_id)) continue;
            if (how == 1) {
                if (depth > zrow[x]) continue;
            } else if (depth_equal ? fabsf(depth - zrow[x]) > 0.002f * (ds->wbuffer ? w : 1) : depth >= zrow[x]) {
                continue;
            }
            float b0 = e0 * iw0 * w, b1 = e1 * iw1 * w, b2 = e2 * iw2 * w; /* con correccion de perspectiva */
            float r, g, b, a = poly_alpha;
            float nx = 0, ny = 0, nz = 1;
            if (ls) { /* luz por pixel: se interpola la normal y se evalua aqui la formula de la consola */
                nx = b0 * v0->nx + b1 * v1->nx + b2 * v2->nx; ny = b0 * v0->ny + b1 * v1->ny + b2 * v2->ny;
                nz = b0 * v0->nz + b1 * v1->nz + b2 * v2->nz;
                float il = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz + 1e-12f);
                nx *= il; ny *= il; nz *= il;
                r = ls->emi[0]; g = ls->emi[1]; b = ls->emi[2];
                for (int i = 0; i < 4; i++) {
                    if (!(ls->mask & (1u << i))) continue;
                    float d = -(ls->dir[i][0] * nx + ls->dir[i][1] * ny + ls->dir[i][2] * nz);
                    if (d < 0) d = 0;
                    float h = -(ls->half[i][0] * nx + ls->half[i][1] * ny + ls->half[i][2] * nz);
                    if (h < 0) h = 0;
                    h = 2 * h * h - 1;
                    if (h < 0) h = 0;
                    if (ls->shin_on) h = ls->shin[(int)(h * 127)] * (1.0f / 255.0f);
                    r += (ls->spe[0] * h + ls->dif[0] * d + ls->amb[0]) * ls->col[i][0];
                    g += (ls->spe[1] * h + ls->dif[1] * d + ls->amb[1]) * ls->col[i][1];
                    b += (ls->spe[2] * h + ls->dif[2] * d + ls->amb[2]) * ls->col[i][2];
                }
                if (r > 1) r = 1;
                if (g > 1) g = 1;
                if (b > 1) b = 1;
            } else {
                r = b0 * v0->r + b1 * v1->r + b2 * v2->r;
                g = b0 * v0->g + b1 * v1->g + b2 * v2->g;
                b = b0 * v0->b + b1 * v1->b + b2 * v2->b;
            }
            float tone[3] = {0, 0, 0}; /* modo toon/brillo: el rojo del color de vertice elige un tono */
            if (toon_mode) {
                int k = (int)(r * 31 + 0.5f);
                if (k > 31) k = 31;
                if (ds->highlight) { /* brillo: el tono se suma despues de la textura */
                    tone[0] = ds->toon[k][0]; tone[1] = ds->toon[k][1]; tone[2] = ds->toon[k][2];
                } else { /* toon: el tono sustituye al color de vertice */
                    r = ds->toon[k][0]; g = ds->toon[k][1]; b = ds->toon[k][2];
                }
            }
            if (ti) {
                float fs = b0 * v0->s + b1 * v1->s + b2 * v2->s, ft = b0 * v0->t + b1 * v1->t + b2 * v2->t;
                if (p->env && ds->env_pixel) {
                    /* Reflejo: la consola calcula sus coordenadas solo en los vertices y las estira por el
                     * triangulo (de ahi las vetas en formas con detalle). Aqui se calculan en cada pixel con
                     * la normal interpolada, con la misma formula. */
                    float mx = b0 * v0->mx + b1 * v1->mx + b2 * v2->mx, my = b0 * v0->my + b1 * v1->my + b2 * v2->my,
                          mz = b0 * v0->mz + b1 * v1->mz + b2 * v2->mz;
                    float ml = sqrtf(mx * mx + my * my + mz * mz);
                    if (ml > 1e-6f) { mx /= ml; my /= ml; mz /= ml; }
                    const float *e = p->envp;
                    fs = (e[0] + mx * e[2] + my * e[3] + mz * e[4]) * (1.0f / 16.0f);
                    ft = (e[1] + mx * e[5] + my * e[6] + mz * e[7]) * (1.0f / 16.0f);
                }
                float tr, tg, tb, ta;
                /* Nivel de reduccion: segun cuantos texeles de la imagen caben en este pixel */
                int lv = 0;
                if (ti->levels > 1) {
                    float ux = fabsf((dsdx - fs * dwdx) * w), uy = fabsf((dsdy - fs * dwdy) * w);
                    float vx = fabsf((dtdx - ft * dwdx) * w), vy = fabsf((dtdy - ft * dwdy) * w);
                    float rho = ux > uy ? ux : uy;
                    if (vx > rho) rho = vx;
                    if (vy > rho) rho = vy;
                    union { float f; uint32_t u; } bits = {rho * tex_k * 0.75f};
                    lv = (int)((bits.u >> 23) & 255) - 127; /* parte entera de log2 */
                    if (lv < 0) lv = 0;
                    if (lv > ti->levels - 1) lv = ti->levels - 1;
                }
                const uint32_t *px = ti->mip[lv];
                int tw = ti->mw[lv], th = ti->mh[lv];
                float sc_s = (float)tw / (float)ti->w, sc_t = (float)th / (float)ti->h;
                fs *= sc_s;
                ft *= sc_t;
                if (ds->filter) { /* filtrado bilineal: mezcla de los 4 texeles vecinos */
                    /* coordenadas en coma fija 24.8 (el +65536 evita redondeos raros con negativos) */
                    int is = (int)((fs - 0.5f + 65536.0f) * 256.0f) - (65536 << 8);
                    int it = (int)((ft - 0.5f + 65536.0f) * 256.0f) - (65536 << 8);
                    int wx = is & 255, wy = it & 255, fl_s = is >> 8, fl_t = it >> 8;
                    int s0 = wrap_coord(fl_s, tw, rep_s, flip_s), s1 = wrap_coord(fl_s + 1, tw, rep_s, flip_s);
                    int t0 = wrap_coord(fl_t, th, rep_t, flip_t), t1 = wrap_coord(fl_t + 1, th, rep_t, flip_t);
                    uint32_t c[4] = {px[t0 * tw + s0], px[t0 * tw + s1], px[t1 * tw + s0], px[t1 * tw + s1]};
                    int wt[4] = {(256 - wx) * (256 - wy), wx * (256 - wy), (256 - wx) * wy, wx * wy}; /* suman 65536 */
                    if ((c[0] & c[1] & c[2] & c[3]) >> 24 == 255) { /* caso normal: los 4 opacos */
                        uint32_t sr = 0, sg = 0, sb = 0;
                        for (int k = 0; k < 4; k++) {
                            sr += ((c[k] >> 16) & 255) * wt[k];
                            sg += ((c[k] >> 8) & 255) * wt[k];
                            sb += (c[k] & 255) * wt[k];
                        }
                        tr = sr * (1.0f / (65536.0f * 255.0f));
                        tg = sg * (1.0f / (65536.0f * 255.0f));
                        tb = sb * (1.0f / (65536.0f * 255.0f));
                        ta = 1;
                    } else { /* con transparencia: los colores se pesan por su alfa para no ensuciar los bordes */
                        float sr = 0, sg = 0, sb = 0, sa = 0;
                        for (int k = 0; k < 4; k++) {
                            float ka = (float)(c[k] >> 24) * (float)wt[k];
                            sa += ka;
                            sr += ((c[k] >> 16) & 255) * ka;
                            sg += ((c[k] >> 8) & 255) * ka;
                            sb += (c[k] & 255) * ka;
                        }
                        if (sa <= 0) continue;
                        tr = sr / (sa * 255);
                        tg = sg / (sa * 255);
                        tb = sb / (sa * 255);
                        ta = sa * (1.0f / (65536.0f * 255.0f));
                    }
                } else {
                    uint32_t c = px[wrap_coord((int)(ft + 65536.0f) - 65536, th, rep_t, flip_t) * tw +
                                    wrap_coord((int)(fs + 65536.0f) - 65536, tw, rep_s, flip_s)];
                    tr = ((c >> 16) & 255) / 255.0f;
                    tg = ((c >> 8) & 255) / 255.0f;
                    tb = (c & 255) / 255.0f;
                    ta = (c >> 24) / 255.0f;
                }
                if (mode == 1) { /* decal */
                    r = tr * ta + r * (1 - ta);
                    g = tg * ta + g * (1 - ta);
                    b = tb * ta + b * (1 - ta);
                } else {
                    r *= tr; g *= tg; b *= tb; a *= ta;
                }
            }
            r += tone[0]; g += tone[1]; b += tone[2];
            if (ls && (p->env || mode == 2) && ds->shiny) {
                /* Mejora del port (la consola no la tiene): pintura, llantas y cristales barnizados. Son los
                 * materiales con luz en modo "brillo" o con reflejo, que en este juego son los de los coches.
                 * - Reflejo del cielo: la direccion reflejada elige entre cielo (color del fondo o de la niebla
                 *   de la pista, mas azul hacia arriba) y suelo oscuro.
                 * - Fresnel: de canto se refleja mucho mas que de frente.
                 * - Brillo de barniz: reflejo nitido del sol y otro mas ancho, con la direccion real de la
                 *   camara en vez de la fija de la consola. */
                float px = b0 * v0->vx + b1 * v1->vx + b2 * v2->vx, py = b0 * v0->vy + b1 * v1->vy + b2 * v2->vy,
                      pz = b0 * v0->vz + b1 * v1->vz + b2 * v2->vz;
                float il = 1.0f / sqrtf(px * px + py * py + pz * pz + 1e-12f);
                float vx = -px * il, vy = -py * il, vz = -pz * il; /* hacia la camara */
                float ndv = nx * vx + ny * vy + nz * vz;
                if (ndv < 0) ndv = 0;
                float f = 1 - ndv, f2 = f * f, fres = 0.15f + 0.70f * f2 * f2 * f;
                if (p->env) fres *= 0.6f; /* los cristales ya traen el reflejo de la consola */
                float rx = 2 * ndv * nx - vx, ry = 2 * ndv * ny - vy, rz = 2 * ndv * nz - vz; /* direccion reflejada */
                float sr, sg, sb;
                if (ry >= 0) { /* cielo sencillo (la camara va casi recta) */
                    float k = ry > 1 ? 1 : ry;
                    sr = ds->sky_h[0] + (ds->sky_z[0] - ds->sky_h[0]) * k;
                    sg = ds->sky_h[1] + (ds->sky_z[1] - ds->sky_h[1]) * k;
                    sb = ds->sky_h[2] + (ds->sky_z[2] - ds->sky_h[2]) * k;
                } else {
                    float k = -ry * 3 > 1 ? 1 : -ry * 3;
                    sr = ds->sky_h[0] * (0.55f - 0.4f * k);
                    sg = ds->sky_h[1] * (0.55f - 0.4f * k);
                    sb = ds->sky_h[2] * (0.55f - 0.4f * k);
                }
                /* La escena reflejada: si el reflejo apunta hacia dentro de la pantalla (lejos de la camara),
                 * se toma lo que se vio en esa direccion en el fotograma anterior (carretera, vallas, arboles,
                 * cielo). Si apunta hacia la camara, lo de detras no esta en pantalla: se queda el cielo. */
                float ws = 0.25f - rz * 1.5f;
                if (ds->prev && ws > 0) {
                    if (ws > 1) ws = 1;
                    int cx = x + (int)(rx * fb_w * 0.30f), cy = y - (int)(ry * fb_h * 0.45f), dd = scale * 2;
                    float er = 0, eg = 0, eb = 0;
                    int got = 0;
                    for (int tap = 0; tap < 4; tap++) { /* cuatro muestras: reflejo algo suave */
                        int qx = cx + ((tap & 1) ? dd : -dd), qy = cy + ((tap & 2) ? dd : -dd);
                        if (qx < 0) qx = 0;
                        if (qx >= fb_w) qx = fb_w - 1;
                        if (qy < 0) qy = 0;
                        if (qy >= fb_h) qy = fb_h - 1;
                        uint32_t c = ds->prev[qy * fb_w + qx];
                        if (!(c >> 24)) continue; /* ahi no hubo 3D (fondo 2D): se queda el cielo */
                        er += ((c >> 16) & 255) * (1.0f / 255); eg += ((c >> 8) & 255) * (1.0f / 255); eb += (c & 255) * (1.0f / 255);
                        got++;
                    }
                    if (got) {
                        float k = ws * got / 4.0f, ig = 1.0f / got;
                        sr += (er * ig - sr) * k; sg += (eg * ig - sg) * k; sb += (eb * ig - sb) * k;
                    }
                }
                r += (sr - r) * fres; g += (sg - g) * fres; b += (sb - b) * fres;
                for (int i = 0; i < 4; i++) {
                    if (!(ls->mask & (1u << i))) continue;
                    float hx = vx - ls->dir[i][0], hy = vy - ls->dir[i][1], hz = vz - ls->dir[i][2];
                    float hl = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz + 1e-12f);
                    float h = (nx * hx + ny * hy + nz * hz) * hl;
                    if (h <= 0) continue;
                    float h2 = h * h, h4 = h2 * h2, h8 = h4 * h4, h16 = h8 * h8, h32 = h16 * h16, h64 = h32 * h32;
                    float sp = h64 * h32 * 1.3f + h16 * 0.18f; /* nitido (h^96) + ancho (h^16) */
                    r += sp * ls->col[i][0]; g += sp * ls->col[i][1]; b += sp * ls->col[i][2];
                }
            }
            if (a <= 0 || a <= alpha_ref) continue;
            /* Texturas con recorte (sin semitransparencia de verdad): con filtro, el borde se corta a la mitad */
            if (ds->filter && !p->translucent && a < 0.5f) continue;
            uint32_t d = frow[x], da = d >> 24, oa = 255;
            int translucent_px = p->translucent && a < 0.999f;
            if (translucent_px) {
                if (trow[x] == poly_id) continue;
                trow[x] = poly_id;
                /* el alfa se guarda: el 3D se mezcla luego con las capas 2D de detras (video.c) */
                oa = (uint32_t)(a * 255);
                if (ds->blend && da) {
                    r = r * a + ((d >> 16) & 255) / 255.0f * (1 - a);
                    g = g * a + ((d >> 8) & 255) / 255.0f * (1 - a);
                    b = b * a + (d & 255) / 255.0f * (1 - a);
                    if (da > oa) oa = da;
                }
            }
            int ir = (int)(r * 255), ig = (int)(g * 255), ib = (int)(b * 255);
            if (ir > 255) ir = 255;
            if (ig > 255) ig = 255;
            if (ib > 255) ib = 255;
            frow[x] = (oa << 24) | (ir << 16) | (ig << 8) | ib;
            /* Como en la consola, lo que cuenta es el pixel: un poligono "translucido" (p. ej. con textura
             * A3I5, como las llantas) que deja un pixel opaco guarda su profundidad y su identificador
             * igual que uno opaco. Si no, las sombras y la niebla lo atravesaban. */
            if ((write_depth || !translucent_px) && how == 2 && !shadow) zrow[x] = depth;
            if (!translucent_px && !shadow) {
                arow[x] = (arow[x] & A_STENCIL) | poly_id | poly_fog;
                trow[x] = 0xFF;
            } else {
                arow[x] &= poly_fog | ~A_FOG; /* translucido: la niebla queda solo si los dos la llevan */
            }
        }
    }
}

/* El dibujado se reparte por franjas horizontales entre varios hilos del PC */
static const TexImage **poly_tex;
static RenderState draw_state;
static uint32_t clear_color;

/* La pantalla se parte en muchas franjas; cada hilo va cogiendo la siguiente libre. Asi el trabajo
 * se reparte bien aunque unas franjas (la carretera, el coche) cuesten mucho mas que otras (el cielo). */
#define BANDS 48
static int *poly_ymin, *poly_ymax; /* filas (a resolucion interna) que ocupa cada poligono */

/* Niebla: cuanto (0..128) se acerca al color de la niebla un pixel a esa profundidad. Como en la
 * consola: profundidad de 15 bits, la entrada N de la tabla esta a FOG_OFFSET + (N+1) * paso, y
 * entre entradas se interpola. */
static int fog_density(const RenderState *ds, float d15) {
    float pos = (d15 - ds->fog_offset) / (float)ds->fog_step - 1;
    if (pos <= 0) return ds->fog_table[0] >= 127 ? 128 : ds->fog_table[0];
    if (pos >= 31) return ds->fog_table[31] >= 127 ? 128 : ds->fog_table[31];
    int k = (int)pos;
    float f = pos - k;
    int den = (int)(ds->fog_table[k] * (1 - f) + ds->fog_table[k + 1] * f + 0.5f);
    return den >= 127 ? 128 : den;
}

static void fog_band(const RenderState *ds, int lo, int hi) {
    for (int i = lo * fb_w; i < hi * fb_w; i++) {
        if (!(abuf[i] & A_FOG)) continue;
        float z = zbuf[i], d15;
        if (z >= 1e29f) d15 = ds->clear_depth15;
        else if (ds->wbuffer) d15 = z * 8; /* W en coma fija 20.12, de 24 bits a 15: x4096 / 512 */
        else d15 = z * 16384 + 16383;
        int den = fog_density(ds, d15);
        if (!den) continue;
        float k = den / 128.0f;
        uint32_t c = fb[i];
        float a = (c >> 24) + (ds->fog_a - (c >> 24)) * k;
        uint32_t out = (uint32_t)(a + 0.5f) << 24;
        if (ds->fog_alpha_only) {
            out |= c & 0xFFFFFF;
        } else {
            for (int s = 0; s < 3; s++) {
                float v = (c >> (16 - s * 8)) & 255;
                v += (ds->fog_rgb[s] - v) * k;
                out |= (uint32_t)(v + 0.5f) << (16 - s * 8);
            }
        }
        fb[i] = out;
    }
}

static void draw_band(int lo, int hi) {
    uint32_t cc = io_reg(0x350);
    uint8_t clear_attr = (uint8_t)(((cc >> 24) & 63) | ((cc & 0x8000) ? A_FOG : 0));
    for (int i = lo * fb_w; i < hi * fb_w; i++) {
        fb[i] = clear_color;
        zbuf[i] = 1e30f;
    }
    memset(abuf + lo * fb_w, clear_attr, (size_t)(hi - lo) * fb_w);
    memset(tbuf + lo * fb_w, 0xFF, (size_t)(hi - lo) * fb_w);
    int shadow_drawn = 0; /* tras pintar sombras, la siguiente mascara empieza con las marcas limpias */
    /* Pasada 0: profundidad de los opacos sin recortes. Pasada 1: se pintan los opacos (los sin
     * recortes solo donde quedaron delante: cada pixel se texturiza e ilumina una sola vez).
     * Pasada 2: los translucidos, en el orden en que llegaron. */
    for (int pass = 0; pass < 3; pass++)
        for (int i = 0; i < poly_count; i++) {
            Poly *p = &polys[i];
            if (poly_ymax[i] < lo || poly_ymin[i] >= hi) continue;
            if (p->translucent != (pass == 2)) continue;
            if (((p->attr >> 16) & 31) == 0) continue; /* alambre: sin hacer */
            if (((p->attr >> 4) & 3) == 3) {
                if (!draw_state.shadows) continue;
                int mask = !((p->attr >> 24) & 63);
                if (mask && shadow_drawn) {
                    for (int k = lo * fb_w; k < hi * fb_w; k++) abuf[k] &= ~A_STENCIL;
                    shadow_drawn = 0;
                }
                if (!mask) shadow_drawn = 1;
            }
            int cutout = poly_tex[i] && poly_tex[i]->has_alpha;
            int how = pass == 2 ? 2 : pass == 0 ? 0 : (cutout ? 2 : 1);
            if (pass == 0 && (cutout || (p->attr & 0x4000))) continue; /* recortes y "profundidad igual": sin prepasada */
            if (pass == 1 && (p->attr & 0x4000)) how = 2;
            for (int k = 1; k + 1 < p->n; k++)
                draw_triangle(p, poly_tex[i], &p->v[0], &p->v[k], &p->v[k + 1], &draw_state, lo, hi, how);
        }
    if (draw_state.fog) fog_band(&draw_state, lo, hi);
}

/* ---- resplandor (bloom) -------------------------------------------------------------------------
 * Mejora del port: lo muy brillante (sol en la pintura, cielo claro, luces) suelta un halo suave.
 * Se reduce la imagen 3D a 1/4, se quedan solo las partes que pasan de un umbral, se difuminan y se
 * suman encima. Solo afecta a los pixeles con 3D. */
static float *bl_a, *bl_b;
static int bl_w, bl_h;

static void bloom_add_row(void *ctx, int y) {
    (void)ctx;
    float fy = (y + 0.5f) / 4 - 0.5f;
    int y0 = (int)floorf(fy), y1 = y0 + 1;
    float wy = fy - y0;
    if (y0 < 0) y0 = 0;
    if (y1 > bl_h - 1) y1 = bl_h - 1;
    if (y0 > bl_h - 1) y0 = bl_h - 1;
    uint32_t *row = &fb[y * fb_w];
    for (int x = 0; x < fb_w; x++) {
        uint32_t c = row[x];
        if (!(c >> 24)) continue;
        float fx = (x + 0.5f) / 4 - 0.5f;
        int x0 = (int)floorf(fx), x1 = x0 + 1;
        float wx = fx - x0;
        if (x0 < 0) x0 = 0;
        if (x1 > bl_w - 1) x1 = bl_w - 1;
        if (x0 > bl_w - 1) x0 = bl_w - 1;
        uint32_t out = c & 0xFF000000u;
        for (int k = 0; k < 3; k++) {
            float v = (bl_a[(y0 * bl_w + x0) * 3 + k] * (1 - wx) + bl_a[(y0 * bl_w + x1) * 3 + k] * wx) * (1 - wy) +
                      (bl_a[(y1 * bl_w + x0) * 3 + k] * (1 - wx) + bl_a[(y1 * bl_w + x1) * 3 + k] * wx) * wy;
            int ch = (int)((c >> (16 - 8 * k)) & 255) + (int)(v * 255 * 0.7f);
            out |= (uint32_t)(ch > 255 ? 255 : ch) << (16 - 8 * k);
        }
        row[x] = out;
    }
}

/* Las tres fases van por filas, repartidas entre los hilos del PC */
static void bloom_down_row(void *ctx, int y) { /* reducir a 1/4 quedandose con lo que pasa del umbral */
    (void)ctx;
    for (int x = 0; x < bl_w; x++) {
        float acc[3] = {0, 0, 0}, mean = 0;
        for (int j = 0; j < 4; j++) /* luminancia media del bloque: solo brilla lo que destaca sobre ella */
            for (int i = 0; i < 4; i++) {
                uint32_t c = fb[(y * 4 + j) * fb_w + x * 4 + i];
                if (c >> 24) mean += (((c >> 16) & 255) * 0.3f + ((c >> 8) & 255) * 0.59f + (c & 255) * 0.11f) / 255.0f;
            }
        mean /= 16;
        float thr = mean + 0.10f > 0.84f ? mean + 0.10f : 0.84f;
        for (int j = 0; j < 4; j++)
            for (int i = 0; i < 4; i++) {
                uint32_t c = fb[(y * 4 + j) * fb_w + x * 4 + i];
                if (!(c >> 24)) continue;
                float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
                float l = r * 0.3f + g * 0.59f + b * 0.11f, k = (l - thr) / 0.16f;
                if (k <= 0) continue;
                if (k > 1) k = 1;
                acc[0] += r * k; acc[1] += g * k; acc[2] += b * k;
            }
        for (int k = 0; k < 3; k++) bl_a[(y * bl_w + x) * 3 + k] = acc[k] / 16;
    }
}

/* difuminado gaussiano separable, radio 6 a 1/4 (unos 24 pixeles a resolucion interna) */
static const float bl_wt[7] = {0.1974f, 0.1747f, 0.1210f, 0.0656f, 0.0278f, 0.0092f, 0.0024f};

static void bloom_blur_row(void *ctx, int y) {
    int vertical = ctx != 0;
    const float *src = vertical ? bl_b : bl_a;
    float *dst = vertical ? bl_a : bl_b;
    for (int x = 0; x < bl_w; x++)
        for (int k = 0; k < 3; k++) {
            float v = src[(y * bl_w + x) * 3 + k] * bl_wt[0];
            for (int t = 1; t < 7; t++) {
                if (vertical) {
                    int a1 = y - t < 0 ? 0 : y - t, a2 = y + t > bl_h - 1 ? bl_h - 1 : y + t;
                    v += (src[(a1 * bl_w + x) * 3 + k] + src[(a2 * bl_w + x) * 3 + k]) * bl_wt[t];
                } else {
                    int a1 = x - t < 0 ? 0 : x - t, a2 = x + t > bl_w - 1 ? bl_w - 1 : x + t;
                    v += (src[(y * bl_w + a1) * 3 + k] + src[(y * bl_w + a2) * 3 + k]) * bl_wt[t];
                }
            }
            dst[(y * bl_w + x) * 3 + k] = v;
        }
}

static void bloom_pass(void) {
    if (!bl_a) {
        bl_w = fb_w / 4;
        bl_h = fb_h / 4;
        bl_a = calloc((size_t)bl_w * bl_h * 3, sizeof(float));
        bl_b = calloc((size_t)bl_w * bl_h * 3, sizeof(float));
    }
    pool_run(bl_h, bloom_down_row, 0);
    pool_run(bl_h, bloom_blur_row, 0);          /* horizontal: a -> b */
    pool_run(bl_h, bloom_blur_row, (void *)1);  /* vertical:   b -> a */
    pool_run(fb_h, bloom_add_row, 0);
}

static void band_job(void *ctx, int b) {
    (void)ctx;
    draw_band(fb_h * b / BANDS, fb_h * (b + 1) / BANDS);
}

static int gpu_render(void); /* gpu.inc.c, al final de este archivo */
extern int gpu_bloom_done;
static double gpu_t[6];
static double gpu_now(void);

static void render(void) {
    if (!scale) {
        scale = video_scale();
        if (getenv("NFS_TESS_ROUND")) tess_round = atoi(getenv("NFS_TESS_ROUND")) / 100.0f;
        fb_w = 256 * scale;
        fb_h = 192 * scale;
        fb_bufs[0] = calloc(fb_w * fb_h, 4);
        fb_bufs[1] = calloc(fb_w * fb_h, 4);
        fb = fb_bufs[0];
        zbuf = malloc(sizeof(float) * fb_w * fb_h);
        abuf = malloc((size_t)fb_w * fb_h);
        tbuf = malloc((size_t)fb_w * fb_h);
        poly_tex = malloc(sizeof(*poly_tex) * MAX_POLYS);
        poly_ymin = malloc(sizeof(int) * MAX_POLYS);
        poly_ymax = malloc(sizeof(int) * MAX_POLYS);
    }
    LARGE_INTEGER t0, t1, tf;
    QueryPerformanceCounter(&t0);
    static uint32_t last_vramcnt[2];
    if (io_reg(0x240) != last_vramcnt[0] || io_reg(0x244) != last_vramcnt[1]) { /* cambio de bancos de VRAM */
        last_vramcnt[0] = io_reg(0x240);
        last_vramcnt[1] = io_reg(0x244);
        tex_generation++;
        perf_tex_flushes++;
    }
    map_texture_memory();
    uint32_t cc = io_reg(0x350), disp = io_reg(0x060);
    uint32_t clear_alpha = (cc >> 16) & 31;
    uint32_t r = cc & 31, g = (cc >> 5) & 31, b = (cc >> 10) & 31;
    clear_color = clear_alpha ? ((clear_alpha * 255 / 31) << 24) | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2) : 0;
    draw_state.wbuffer = (swap_flags >> 1) & 1;
    draw_state.alpha_test = (disp & 4) ? (int)(io_reg(0x340) & 31) : -1;
    draw_state.blend = (disp >> 3) & 1;
    draw_state.filter = video_filter();
    draw_state.pixel_light = video_pixel_light();
    draw_state.shadows = video_shadows();
    draw_state.env_pixel = draw_state.pixel_light;
    draw_state.shiny = video_shiny() && draw_state.pixel_light;
    draw_state.highlight = (disp >> 1) & 1;
    for (int i = 0; i < 32; i++) rgb15(io_reg(0x380 + (i & ~1) * 2) >> ((i & 1) * 16), draw_state.toon[i]);
    draw_state.fog = ((disp >> 7) & 1) && video_fog();
    draw_state.fog_alpha_only = (disp >> 6) & 1;
    draw_state.fog_step = 0x400 >> ((disp >> 8) & 15);
    if (draw_state.fog_step < 1) draw_state.fog_step = 1;
    draw_state.fog_offset = io_reg(0x35C) & 0x7FFF;
    uint32_t fc = io_reg(0x358);
    draw_state.fog_rgb[0] = ((fc & 31) << 3 | (fc & 31) >> 2);
    draw_state.fog_rgb[1] = (((fc >> 5) & 31) << 3 | ((fc >> 5) & 31) >> 2);
    draw_state.fog_rgb[2] = (((fc >> 10) & 31) << 3 | ((fc >> 10) & 31) >> 2);
    draw_state.fog_a = ((fc >> 16) & 31) * 255 / 31.0f;
    for (int i = 0; i < 32; i++) draw_state.fog_table[i] = (io_reg(0x360 + (i & ~3)) >> ((i & 3) * 8)) & 127;
    draw_state.clear_depth15 = io_reg(0x354) & 0x7FFF;
    { /* cielo para el reflejo de la pintura: el de la niebla si la hay (asi casa con la calima), si no el
       * color de fondo; hacia arriba, mas azul */
        float h[3] = {0.75f, 0.8f, 0.85f};
        if (draw_state.fog) for (int k = 0; k < 3; k++) h[k] = draw_state.fog_rgb[k] / 255.0f;
        else if (clear_color >> 24) for (int k = 0; k < 3; k++) h[k] = ((clear_color >> (16 - 8 * k)) & 255) / 255.0f;
        static const float zen[3] = {0.40f, 0.58f, 0.85f};
        for (int k = 0; k < 3; k++) {
            draw_state.sky_h[k] = h[k] * 0.9f + 0.1f;
            draw_state.sky_z[k] = h[k] * 0.45f + zen[k] * 0.55f;
        }
    }
    double tt0 = gpu_now();
    for (int i = 0; i < poly_count; i++) {
        Poly *p = &polys[i];
        float lo = p->v[0].y, hi = p->v[0].y;
        for (int k = 1; k < p->n; k++) {
            if (p->v[k].y < lo) lo = p->v[k].y;
            if (p->v[k].y > hi) hi = p->v[k].y;
        }
        poly_ymin[i] = (int)(lo * scale) - 1;
        poly_ymax[i] = (int)(hi * scale) + 1;
        poly_tex[i] = get_texture(p->tex, p->pal);
        if (p->lit && poly_tex[i]) note_lit_texture(poly_tex[i]);
    }
    vram_epoch++; /* lo que el juego escriba a partir de ahora es posterior a estas texturas */
    gpu_t[4] += gpu_now() - tt0;

    if (getenv("NFS_SHADOW_LOG") && g_frame == (uint32_t)atoi(getenv("NFS_SHADOW_LOG"))) { /* depuracion */
        for (int i = 0; i < poly_count; i++) {
            Poly *p = &polys[i];
            if (((p->attr >> 4) & 3) != 3) continue;
            fprintf(stderr, "[sombra] %d attr %08x id %d alfa %d caras %s%s n %d:", i, p->attr, (p->attr >> 24) & 63,
                    (p->attr >> 16) & 31, (p->attr & 0x80) ? "delante " : "", (p->attr & 0x40) ? "detras" : "", p->n);
            for (int k = 0; k < p->n; k++) fprintf(stderr, " (%.1f,%.1f z%.4f)", p->v[k].x, p->v[k].y, p->v[k].z);
            fprintf(stderr, " color %.2f %.2f %.2f%c", p->v[0].r, p->v[0].g, p->v[0].b, 10);
        }
    }
    perf_polys = poly_count;
    perf_raw = (int)raw_polys;
    perf_trans = 0;
    for (int i = 0; i < poly_count; i++) perf_trans += polys[i].translucent;
    fb_cur ^= 1; /* se dibuja en una imagen; la otra (la anterior) es la que se refleja */
    draw_state.prev = frames_drawn ? fb_bufs[fb_cur ^ 1] : 0;
    fb = fb_bufs[fb_cur];
    draw_state.bloom = video_bloom();
    if (!gpu_render()) pool_run(BANDS, band_job, 0); /* con la grafica, o por software */
    double tb0 = gpu_now();
    if (draw_state.bloom && !gpu_bloom_done) bloom_pass(); /* con la grafica ya lo hizo ella */
    gpu_t[5] += gpu_now() - tb0;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&tf);
    perf_3d_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000 / (double)tf.QuadPart;
    frames_drawn++;
    if (getenv("NFS_3D_LOG") && frames_drawn % 600 == 1)
        fprintf(stderr, "[texturas] sustitutas cargadas: %d, volcadas: %d%c", tex_loaded, tex_dumped, 10);
    if (getenv("NFS_3D_LOG") && frames_drawn % 60 == 1)
        fprintf(stderr, "[3d] fotograma 3D %u: %d poligonos, %u vertices%c", frames_drawn, poly_count, vertex_total, 10);
    if (getenv("NFS_FX_LOG") && frames_drawn % 120 == 1) { /* que efectos pide el juego de verdad */
        int modes[4] = {0}, fogged = 0, ids = 0;
        for (int i = 0; i < poly_count; i++) {
            modes[(polys[i].attr >> 4) & 3]++;
            if (polys[i].attr & 0x8000) fogged++;
            if ((polys[i].attr >> 24) & 63) ids++;
        }
        uint32_t toon_or = 0;
        for (int i = 0; i < 16; i++) toon_or |= io_reg(0x380 + i * 4);
        fprintf(stderr, "[fx] 3D %u: disp3dcnt %04x niebla color %08x desplaz %04x tabla %08x %08x..%08x toon %s | "
                        "modos normal %d decal %d brillo %d sombra %d | con niebla %d | con id %d%c",
                frames_drawn, disp & 0xFFFF, io_reg(0x358), io_reg(0x35C) & 0x7FFF, io_reg(0x360), io_reg(0x364),
                io_reg(0x37C), toon_or ? "con datos" : "vacia", modes[0], modes[1], modes[2], modes[3], fogged, ids, 10);
        int env = 0, env_lit = 0, envw[8] = {0};
        for (int i = 0; i < poly_count; i++)
            if (polys[i].env) { env++; env_lit += polys[i].lit; envw[(polys[i].tex >> 20) & 7]++; }
        fprintf(stderr, "[fx] reflejo generado: %d poligonos (%d con luz); por ancho de textura 8..1024: %d %d %d %d %d %d %d %d%c",
                env, env_lit, envw[0], envw[1], envw[2], envw[3], envw[4], envw[5], envw[6], envw[7], 10);
    }
}

/* ---- ejecucion de una orden ------------------------------------------------------------------ */
static void command(uint32_t cmd, const uint32_t *p) {
    Mtx m;
    switch (cmd) {
    case 0x10: mtx_mode = p[0] & 3; break;
    case 0x11: /* push */
        if (mtx_mode == 0) { memcpy(proj_stack, proj, sizeof(Mtx)); proj_ptr = 1; }
        else if (mtx_mode == 3) memcpy(tex_stack, tex_mtx, sizeof(Mtx));
        else {
            memcpy(pos_stack[stack_ptr & 31], pos, sizeof(Mtx));
            memcpy(vec_stack[stack_ptr & 31], vec, sizeof(Mtx));
            stack_ptr = (stack_ptr + 1) & 63;
        }
        break;
    case 0x12: /* pop */
        if (mtx_mode == 0) { memcpy(proj, proj_stack, sizeof(Mtx)); proj_ptr = 0; }
        else if (mtx_mode == 3) memcpy(tex_mtx, tex_stack, sizeof(Mtx));
        else {
            stack_ptr = (stack_ptr - ((int32_t)(p[0] << 26) >> 26)) & 63;
            memcpy(pos, pos_stack[stack_ptr & 31], sizeof(Mtx));
            memcpy(vec, vec_stack[stack_ptr & 31], sizeof(Mtx));
        }
        clip_dirty = 1;
        break;
    case 0x13: /* store */
        if (mtx_mode == 0) memcpy(proj_stack, proj, sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex_stack, tex_mtx, sizeof(Mtx));
        else { memcpy(pos_stack[p[0] & 31], pos, sizeof(Mtx)); memcpy(vec_stack[p[0] & 31], vec, sizeof(Mtx)); }
        break;
    case 0x14: /* restore */
        if (mtx_mode == 0) memcpy(proj, proj_stack, sizeof(Mtx));
        else if (mtx_mode == 3) memcpy(tex_mtx, tex_stack, sizeof(Mtx));
        else { memcpy(pos, pos_stack[p[0] & 31], sizeof(Mtx)); memcpy(vec, vec_stack[p[0] & 31], sizeof(Mtx)); }
        clip_dirty = 1;
        break;
    case 0x15: identity(m); load_matrix(m); break;
    case 0x16: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; load_matrix(m); break;
    case 0x17: identity(m); for (int i = 0; i < 12; i++) m[(i / 3) * 4 + i % 3] = (int32_t)p[i]; load_matrix(m); break;
    case 0x18: for (int i = 0; i < 16; i++) m[i] = (int32_t)p[i]; mult_matrix(m); break;
    case 0x19: identity(m); for (int i = 0; i < 12; i++) m[(i / 3) * 4 + i % 3] = (int32_t)p[i]; mult_matrix(m); break;
    case 0x1A: identity(m); for (int i = 0; i < 9; i++) m[(i / 3) * 4 + i % 3] = (int32_t)p[i]; mult_matrix(m); break;
    case 0x1B: { /* escala: no afecta a la matriz de vectores */
        identity(m);
        m[0] = (int32_t)p[0]; m[5] = (int32_t)p[1]; m[10] = (int32_t)p[2];
        if (mtx_mode == 0) mult(proj, m); else if (mtx_mode == 3) mult(tex_mtx, m); else mult(pos, m);
        clip_dirty = 1;
        break;
    }
    case 0x1C: identity(m); m[12] = (int32_t)p[0]; m[13] = (int32_t)p[1]; m[14] = (int32_t)p[2]; mult_matrix(m); break;
    case 0x20: cur_lit = 0; cur_r = (p[0] & 31) / 31.0f; cur_g = ((p[0] >> 5) & 31) / 31.0f; cur_b = ((p[0] >> 10) & 31) / 31.0f; break;
    case 0x21: do_normal(p[0]); break;
    case 0x22:
        raw_s = (int16_t)p[0];
        raw_t = (int16_t)(p[0] >> 16);
        if ((cur_tex >> 30) == 1) {
            tc_s = (int32_t)(((int64_t)raw_s * tex_mtx[0] + (int64_t)raw_t * tex_mtx[4] + tex_mtx[8] + tex_mtx[12]) >> 12);
            tc_t = (int32_t)(((int64_t)raw_s * tex_mtx[1] + (int64_t)raw_t * tex_mtx[5] + tex_mtx[9] + tex_mtx[13]) >> 12);
        } else {
            tc_s = raw_s;
            tc_t = raw_t;
        }
        break;
    case 0x23: submit_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), (int16_t)p[1]); break;
    case 0x24: submit_vertex((int16_t)(p[0] << 6), (int16_t)((p[0] >> 10) << 6), (int16_t)((p[0] >> 20) << 6)); break;
    case 0x25: submit_vertex((int16_t)p[0], (int16_t)(p[0] >> 16), last_z); break;
    case 0x26: submit_vertex((int16_t)p[0], last_y, (int16_t)(p[0] >> 16)); break;
    case 0x27: submit_vertex(last_x, (int16_t)p[0], (int16_t)(p[0] >> 16)); break;
    case 0x28: submit_vertex((int16_t)(last_x + s10(p[0])), (int16_t)(last_y + s10(p[0] >> 10)), (int16_t)(last_z + s10(p[0] >> 20))); break;
    case 0x29: pending_attr = p[0]; break;
    case 0x2A: cur_tex = p[0]; break;
    case 0x2B: cur_pal = p[0] & 0x1FFF; break;
    case 0x30:
        rgb15(p[0], mat_dif);
        rgb15(p[0] >> 16, mat_amb);
        if (p[0] & 0x8000) { cur_r = mat_dif[0]; cur_g = mat_dif[1]; cur_b = mat_dif[2]; cur_lit = 0; }
        ls_dirty = 1;
        break;
    case 0x31: rgb15(p[0], mat_spe); rgb15(p[0] >> 16, mat_emi); shininess_on = (p[0] >> 15) & 1; ls_dirty = 1; break;
    case 0x32: { /* direccion de una luz, transformada por la matriz de vectores actual */
        int i = p[0] >> 30;
        float d[3] = {s10(p[0]) / 512.0f, s10(p[0] >> 10) / 512.0f, s10(p[0] >> 20) / 512.0f};
        for (int j = 0; j < 3; j++) light_dir[i][j] = (d[0] * vec[j] + d[1] * vec[4 + j] + d[2] * vec[8 + j]) / 4096.0f;
        light_half[i][0] = light_dir[i][0] / 2;
        light_half[i][1] = light_dir[i][1] / 2;
        light_half[i][2] = (light_dir[i][2] - 1) / 2;
        ls_dirty = 1;
        break;
    }
    case 0x33: rgb15(p[0], light_col[p[0] >> 30]); ls_dirty = 1; break;
    case 0x34: for (int i = 0; i < 128; i++) shininess[i] = (uint8_t)(p[i / 4] >> ((i & 3) * 8)); ls_dirty = 1; break;
    case 0x40: cur_attr = pending_attr; prim_type = p[0] & 3; prim_count = 0; break;
    case 0x41: break;
    case 0x50: /* swap buffers: se dibuja lo acumulado */
        swap_flags = p[0];
        {
            LARGE_INTEGER q0, q1, qf;
            QueryPerformanceCounter(&q0);
            finish_polygons();
            QueryPerformanceCounter(&q1);
            QueryPerformanceFrequency(&qf);
            perf_3d_ms += (double)(q1.QuadPart - q0.QuadPart) * 1000 / (double)qf.QuadPart;
        }
        render();
        poly_count = 0;
        raw_polys = 0;
        ls_count = 0;
        ls_dirty = 1;
        vertex_total = 0;
        prim_type = -1;
        break;
    case 0x60: vp_x1 = p[0] & 255; vp_y1 = (p[0] >> 8) & 255; vp_x2 = (p[0] >> 16) & 255; vp_y2 = p[0] >> 24; break;
    case 0x70: break; /* prueba de caja: siempre se contesta "visible" */
    case 0x71: { /* prueba de posicion */
        int32_t x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
        last_x = x; last_y = y; last_z = z;
        update_clip();
        break;
    }
    case 0x72: break;
    default: break;
    }
}

/* ---- modelos sustitutos ------------------------------------------------------------------------
 * El juego manda cada pieza de un modelo (una "forma" del NSBMD) al chip 3D como una lista de
 * visualizacion, por DMA. io.c calcula su huella (FNV-1a de 64 bits de sus bytes, la misma que
 * apunta tools/extract_models.py) y pregunta aqui: si hay un modelo sustituto para esa huella, se
 * dibuja ese en lugar de la lista original. El material, la textura, la pintura y las matrices ya
 * los ha puesto el juego antes, asi que el sustituto solo trae geometria: posicion, coordenadas de
 * textura, normal (piezas con luz) o color (piezas sin luz).
 *
 * Los sustitutos son archivos <huella>.nmesh en <carpeta de modelos>\reemplazos (por defecto
 * %USERPROFILE%\nfs-nitro-models; NFS_MODELS cambia la carpeta, NFS_MESHES=0 los apaga). Los genera
 * tools/models/make_replacement.py a partir de un OBJ exportado de Blender. */
typedef struct MeshVtx {
    int32_t x, y, z;          /* en las unidades de la lista original, coma fija 20.12 */
    int16_t s, t;             /* texeles en coma fija 12.4, como la orden TEXCOORD */
    int16_t nx, ny, nz;       /* normal, -511..511 como la orden NORMAL */
    uint16_t col;             /* color 15 bits, como la orden COLOR */
} MeshVtx;

typedef struct Mesh {
    uint64_t hash;
    int flags, count;         /* flags: 1 manda normales, 2 coordenadas de textura, 4 colores (como la pieza
                               * original), 8 no subdividir en el port, 16/32 luz de freno/marcha atras;
                               * count: vertices (de tres en tres) */
    MeshVtx *v;
} Mesh;

static Mesh *meshes;
static int mesh_count, mesh_slots, meshes_on = -1;
int perf_meshes;              /* piezas sustituidas en el ultimo fotograma */

static void meshes_init(void) {
    const char *e = getenv("NFS_MESHES");
    meshes_on = !(e && !strcmp(e, "0"));
    if (!meshes_on) return;
    static char def_dir[512];
    snprintf(def_dir, sizeof def_dir, "%s/nfs-nitro-models", getenv("USERPROFILE") ? getenv("USERPROFILE") : ".");
    const char *dir = getenv("NFS_MODELS") ? getenv("NFS_MODELS") : def_dir;
    char pattern[600], path[700];
    snprintf(pattern, sizeof pattern, "%s/reemplazos/*.nmesh", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) { meshes_on = 0; return; }
    int n = 0;
    do n++; while (FindNextFileA(h, &fd));
    FindClose(h);
    mesh_slots = n * 2 + 8;
    meshes = calloc(mesh_slots, sizeof(Mesh));
    h = FindFirstFileA(pattern, &fd);
    do {
        unsigned long long hash;
        if (sscanf(fd.cFileName, "%16llx", &hash) != 1) continue;
        snprintf(path, sizeof path, "%s/reemplazos/%s", dir, fd.cFileName);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint32_t head[3];
        if (fread(head, 4, 3, f) == 3 && head[0] == 0x31534D4Eu && head[2] % 3 == 0 && head[2] < 3000000) {
            MeshVtx *v = malloc(sizeof(MeshVtx) * head[2]);
            if (v && fread(v, sizeof(MeshVtx), head[2], f) == head[2]) {
                Mesh *m = &meshes[hash % (uint64_t)mesh_slots];
                while (m->v) m = &meshes[(m - meshes + 1) % mesh_slots];
                m->hash = hash;
                m->flags = (int)head[1];
                m->count = (int)head[2];
                m->v = v;
                mesh_count++;
            } else {
                free(v);
            }
        }
        fclose(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    fprintf(stderr, "[modelos] %d piezas sustitutas en %s/reemplazos%c", mesh_count, dir, 10);
    if (!mesh_count) meshes_on = 0;
}

int gx_has_replacements(void) {
    if (meshes_on < 0) meshes_init();
    return meshes_on;
}

static void draw_mesh(const Mesh *m) {
    uint32_t p = 0;
    command(0x40, &p); /* triangulos */
    cur_notess = (m->flags & 8) != 0; /* 8: ya viene muy subdividido: el port no lo subdivide mas */
    cur_glow = (m->flags >> 4) & 3;     /* 16 luz de freno, 32 luz de marcha atras */
    for (int i = 0; i < m->count; i++) {
        const MeshVtx *v = &m->v[i];
        if (m->flags & 2) {
            p = (uint16_t)v->s | ((uint32_t)(uint16_t)v->t << 16);
            command(0x22, &p);
        }
        if (m->flags & 4) { /* antes que la normal: si hay las dos, manda la luz */
            p = v->col;
            command(0x20, &p);
        }
        if (m->flags & 1) {
            p = ((uint32_t)v->nx & 0x3FF) | (((uint32_t)v->ny & 0x3FF) << 10) | (((uint32_t)v->nz & 0x3FF) << 20);
            command(0x21, &p);
        }
        submit_vertex(v->x, v->y, v->z);
    }
    cur_notess = 0;
    cur_glow = 0;
    command(0x41, &p);
}

static uint32_t packed, cur_cmd, params[32];
static int have, need, packed_left;

/* io.c: ha llegado por DMA una lista con esta huella. 1 = se ha dibujado un sustituto en su lugar */
int gx_try_replace(uint64_t hash) {
    if (!gx_has_replacements() || cur_cmd || packed_left) return 0; /* solo entre ordenes completas */
    if (!polys) return 0;
    for (int probe = 0; probe < mesh_slots; probe++) {
        Mesh *m = &meshes[(hash + probe) % (uint64_t)mesh_slots];
        if (!m->v) return 0;
        if (m->hash == hash) {
            draw_mesh(m);
            perf_meshes++;
            return 1;
        }
    }
    return 0;
}

/* ---- entrada de ordenes (cola empaquetada y puertos directos) -------------------------------- */
static void next_packed(void) {
    while (packed_left > 0) {
        cur_cmd = packed & 0xFF;
        packed >>= 8;
        packed_left--;
        need = PARAMS[cur_cmd];
        have = 0;
        if (cur_cmd == 0 && packed == 0) { packed_left = 0; cur_cmd = 0; return; }
        if (need) return;
        if (cur_cmd) command(cur_cmd, params);
    }
    cur_cmd = 0;
}

void gx_write(uint32_t off, uint32_t value) {
    if (!polys) {
        polys = malloc(sizeof(Poly) * MAX_POLYS);
        lstates = malloc(sizeof(LightState) * MAX_LSTATES);
        raws = malloc(sizeof(RawPoly) * MAX_RAW);
        welds = malloc(sizeof(Weld) * WELD_SLOTS);
        identity(proj); identity(pos); identity(vec); identity(tex_mtx);
    }
    if (off == 0x400) { /* cola: palabras con 4 ordenes empaquetadas, seguidas de sus parametros */
        if (!cur_cmd && packed_left == 0) {
            packed = value;
            packed_left = 4;
            next_packed();
        } else {
            params[have++] = value;
            if (have == need) {
                command(cur_cmd, params);
                next_packed();
            }
        }
        return;
    }
    if (off >= 0x440 && off < 0x600) { /* puerto directo de una orden */
        static uint32_t dcmd, dparams[32];
        static int dhave;
        uint32_t cmd = (off - 0x400) / 4;
        if (cmd != dcmd) { dcmd = cmd; dhave = 0; }
        dparams[dhave++] = value;
        if (dhave >= PARAMS[cmd]) {
            command(cmd, dparams);
            dhave = 0;
            dcmd = 0;
        }
    }
}

uint32_t gx_read(uint32_t off) {
    if (off == 0x600) /* GXSTAT: nunca ocupado, cola vacia, prueba de caja = visible */
        return 0x06000002u | ((uint32_t)(stack_ptr & 31) << 8) | ((uint32_t)proj_ptr << 13) | (io_reg(0x600) & 0xC0000000u);
    if (off == 0x604) return (raw_polys & 0xFFF) | (vertex_total << 16);
    if (off >= 0x620 && off < 0x630) { /* resultado de la prueba de posicion */
        update_clip();
        int j = (off - 0x620) / 4;
        return (uint32_t)(((int64_t)last_x * clip[j] + (int64_t)last_y * clip[4 + j] + (int64_t)last_z * clip[8 + j] +
                           (int64_t)4096 * clip[12 + j]) >> 12);
    }
    if (off >= 0x640 && off < 0x680) { update_clip(); return (uint32_t)clip[(off - 0x640) / 4]; }
    if (off >= 0x680 && off < 0x6A4) { int i = (off - 0x680) / 4; return (uint32_t)vec[(i / 3) * 4 + i % 3]; }
    return 0;
}

/* Para el compositor: imagen 3D actual (o 0 si aun no hay) */
const uint32_t *gx_frame(int *w, int *h) {
    *w = fb_w;
    *h = fb_h;
    return frames_drawn ? fb : 0;
}

/* Dibujado con la tarjeta grafica */
#include "gpu.inc.c"
