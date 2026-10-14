/* Ventana de Windows con las dos pantallas de la DS, teclado y raton, y la configuracion
 * grafica (tamano, resolucion interna, suavizado, disposicion de las pantallas).
 *
 * Teclas: flechas = cruceta, Z = A, X = B, A = Y, S = X, Q = L, W = R, Intro = Start,
 * Retroceso = Select. Raton con el boton izquierdo sobre la pantalla tactil = lapiz.
 * Mando (XInput: los de Xbox y compatibles): gatillo derecho o A = A (acelerar / aceptar), gatillo izquierdo
 * o B = B (frenar / atras), X = Y, Y = X, RB = R (nitro), LB = L (freno de mano), Start, Back = Select,
 * cruceta y palanca izquierda = cruceta.
 * F1 cambia la disposicion de las pantallas; F2 activa/desactiva el filtrado de texturas;
 * F3 la luz por pixel; F4 cambia la subdivision de los modelos iluminados (0, 1 o 2 niveles);
 * F5 la niebla; F6 las sombras; F7 el barniz de los coches; F8 el resplandor;
 * F9 dibujar el 3D con la tarjeta grafica o por software; F11 pantalla completa (Alt+Intro tambien).
 *
 * Variables de entorno (todas opcionales):
 *   NFS_LAYOUT=n       0 = una pantalla encima de otra; 1 = una al lado de otra;
 *                      2 = la del 3D grande y la tactil a media escala al lado (por defecto);
 *                      3 = solo la pantalla del juego (F1 para ver la tactil cuando haga falta)
 *   NFS_FULLSCREEN=1   pantalla completa (sin bordes, a la resolucion del monitor; las pantallas de la DS
 *                      se amplian a lo que quepa conservando la forma 4:3)
 *   NFS_SCALE=n        tamano de la pantalla principal, en multiplos de 256x192 (por defecto, lo que quepa)
 *   NFS_AA=n           suavizado de bordes por supermuestreo: el 3D se dibuja a n veces el tamano
 *                      mostrado y se reduce promediando (1 = sin suavizado)
 *   NFS_3D_SCALE=n     fuerza la resolucion interna (multiplos de 256x192) en vez de NFS_SCALE * NFS_AA
 *   NFS_FILTER=0       desactiva el filtrado bilineal de texturas
 *   NFS_PIXEL_LIGHT=0  luz por vertice, como la consola (por defecto, por pixel)
 *   NFS_TESS=n         subdivision de los modelos iluminados: cada triangulo en 4^n (por defecto 1; 0 = no)
 *   NFS_FOG=0          sin niebla
 *   NFS_SHADOWS=0      sin sombras
 *   NFS_GPU=0          dibujar el 3D por software en vez de con la tarjeta grafica
 *   NFS_MSAA=n         muestras de suavizado de bordes con la tarjeta grafica (por defecto 4)
 *   NFS_BLOOM=0        sin resplandor (bloom)
 *   NFS_SHARP=0        texturas con mipmaps (de lejos mas suaves, sin parpadeo); 1 = siempre nitidas
 *   NFS_SHINY=0        coches sin barniz (reflejo del cielo, Fresnel y brillos de la pintura)
 *   NFS_SPIKES=ms      apunta los fotogramas que tarden mas de esos milisegundos (tirones)
 *   NFS_HEADLESS=1     sin ventana (para pruebas automaticas)
 *   NFS_SHOT_EVERY=n   guarda una captura shot_NNNNN.png cada n fotogramas (NFS_SHOT_FROM / NFS_SHOT_TO acotan)
 *   NFS_INPUT=archivo  guion de entrada para pruebas. Una orden por linea:
 *                        FOTOGRAMA touch X Y      toca la pantalla tactil en (X, Y)
 *                        FOTOGRAMA release        levanta el lapiz
 *                        FOTOGRAMA key NOMBRE     pulsa un boton durante 6 fotogramas
 *                        FOTOGRAMA hold NOMBRE    deja un boton pulsado
 *                        FOTOGRAMA unhold NOMBRE  lo suelta
 *                      (botones: A B SELECT START RIGHT LEFT UP DOWN R L X Y) */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "sys.h"

void video_render(uint32_t *out);
void video_save_png(const char *path, const uint32_t *pixels, int w, int h);

/* ---- configuracion ------------------------------------------------------------------------- */
static int configured, headless, layout, win_scale, internal_scale, filter = 1, shot_every, fullscreen;
static int sharp = 1, pixel_light = 1, tess = 2, fog = 1, shadows = 1, shiny = 1, bloom = 1, gpu = 1, msaa = 4;
static uint32_t shot_from, shot_to = 0xFFFFFFFFu;

static int env_int(const char *name, int fallback) { return getenv(name) ? atoi(getenv(name)) : fallback; }

/* Tamano de la ventana para una disposicion y una escala */
static void layout_size(int lay, int s, int *w, int *h) {
    if (lay == 0) { *w = 256 * s; *h = 384 * s; }
    else if (lay == 1) { *w = 512 * s; *h = 192 * s; }
    else if (lay == 3) { *w = 256 * s; *h = 192 * s; }
    else { *w = 256 * s + 256 * (s / 2 > 0 ? s / 2 : 1); *h = 192 * s; }
}

static void configure(void) {
    if (configured) return;
    configured = 1;
    headless = getenv("NFS_HEADLESS") != 0;
    filter = env_int("NFS_FILTER", 1) != 0;
    pixel_light = env_int("NFS_PIXEL_LIGHT", 1) != 0;
    tess = env_int("NFS_TESS", 1);
    fog = env_int("NFS_FOG", 1) != 0;
    shadows = env_int("NFS_SHADOWS", 1) != 0;
    shiny = env_int("NFS_SHINY", 1) != 0;
    bloom = env_int("NFS_BLOOM", 1) != 0;
    sharp = env_int("NFS_SHARP", 1) != 0;
    gpu = env_int("NFS_GPU", 1) != 0;
    msaa = env_int("NFS_MSAA", 4);
    if (tess < 0) tess = 0;
    if (tess > 4) tess = 4;
    shot_every = env_int("NFS_SHOT_EVERY", 0);
    if (getenv("NFS_SHOT_FROM")) shot_from = (uint32_t)atoi(getenv("NFS_SHOT_FROM"));
    if (getenv("NFS_SHOT_TO")) shot_to = (uint32_t)atoi(getenv("NFS_SHOT_TO"));
    if (headless) {
        layout = 0;
        win_scale = internal_scale = env_int("NFS_3D_SCALE", 1);
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN) - 90; /* sitio para bordes y barra */
        layout = env_int("NFS_LAYOUT", 2);
        if (layout < 0 || layout > 3) layout = 2;
        fullscreen = env_int("NFS_FULLSCREEN", 0) != 0;
        win_scale = env_int("NFS_SCALE", 0);
        if (fullscreen) { /* la pantalla del juego ocupa casi todo el alto del monitor: escala redondeada hacia arriba */
            int full_h = GetSystemMetrics(SM_CYSCREEN);
            win_scale = (full_h + 191) / 192;
            if (win_scale > 8) win_scale = 8;
        } else if (win_scale < 1) { /* la escala mayor que quepa en la pantalla */
            int w, h;
            for (win_scale = 8; win_scale > 1; win_scale--) {
                layout_size(layout, win_scale, &w, &h);
                if (w <= sw && h <= sh) break;
            }
        }
        /* Resolucion interna: con suavizado (NFS_AA) se dibuja a mas tamano del que se muestra. Sin
         * pedir nada, en ventanas grandes se dibuja a x3 y se amplia con filtro (x4 va justo en las
         * pistas pesadas con el dibujado por software). */
        int aa = env_int("NFS_AA", win_scale <= 2 ? 2 : 1);
        if (aa < 1) aa = 1;
        int deflt = win_scale * aa;
        if (!getenv("NFS_AA") && deflt > 3 && !gpu) deflt = 3; /* por software, x3 como mucho */
        internal_scale = env_int("NFS_3D_SCALE", deflt);
    }
    if (internal_scale < 1) internal_scale = 1;
    if (internal_scale > 8) internal_scale = 8;
    if (win_scale < 1) win_scale = 1;
    fprintf(stderr, "[video] disposicion %d, pantalla principal x%d, resolucion interna x%d (%dx%d), filtro %s%c", layout,
            win_scale, internal_scale, 256 * internal_scale, 192 * internal_scale, filter ? "si" : "no", 10);
}

int video_scale(void) { configure(); return internal_scale; }
int video_filter(void) { configure(); return filter; }
int video_pixel_light(void) { configure(); return pixel_light; }
int video_tess(void) { configure(); return tess; }
int video_fog(void) { configure(); return fog; }
int video_shadows(void) { configure(); return shadows; }
int video_shiny(void) { configure(); return shiny; }
int video_bloom(void) { configure(); return bloom; }
int video_sharp(void) { configure(); return sharp; } /* sin mipmaps: texturas nitidas de lejos */
int video_gpu(void) { configure(); return gpu; }
int video_msaa(void) { configure(); return msaa; }

/* ---- imagen --------------------------------------------------------------------------------- */
static uint32_t *frame;   /* las dos pantallas a resolucion interna: (256*S) x (384*S), la de arriba primero */
static uint32_t *present; /* lo que se pinta en la ventana, ya a su tamano */
static int present_w, present_h;
static HWND window;
static LARGE_INTEGER freq, next_tick, perf_last;
static double perf_2d, perf_present;
static int touch_down, touch_x, touch_y;
static struct { int x, y, w, h; } touch_rect; /* donde esta la pantalla tactil dentro de la ventana */

/* Copia una pantalla (a resolucion interna S) a la imagen de la ventana con escala ts.
 * Si S es multiplo de ts, cada pixel de salida es la media de un bloque: suavizado de bordes. */
typedef struct BlitJob {
    const uint32_t *src;
    int S, dx, dy, ow, oh; /* resolucion interna; rectangulo de destino */
} BlitJob;

static void blit_row(void *ctx, int y) {
    BlitJob *j = ctx;
    int S = j->S, sw = 256 * S, ow = j->ow;
    uint32_t *dst = present + (j->dy + y) * present_w + j->dx;
    if (ow == sw && j->oh == 192 * S) {
        memcpy(dst, j->src + (size_t)y * sw, sizeof(uint32_t) * ow);
    } else if (sw % ow == 0 && ow * 3 == j->oh * 4) {
        int f = sw / ow, n = f * f;
        for (int x = 0; x < ow; x++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int k = 0; k < f; k++) {
                const uint32_t *p = j->src + (size_t)(y * f + k) * sw + x * f;
                for (int i = 0; i < f; i++) {
                    r += (p[i] >> 16) & 255;
                    g += (p[i] >> 8) & 255;
                    b += p[i] & 255;
                }
            }
            dst[x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    } else { /* tamanos que no son multiplos: interpolacion bilineal (suave, sin pixeles repetidos) */
        int sh = 192 * S;
        double ry = (double)sh / j->oh, rx = (double)sw / ow;
        int fy = (int)(((y + 0.5) * ry - 0.5) * 65536.0);
        if (fy < 0) fy = 0;
        int y0 = fy >> 16, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1, wy = (fy >> 8) & 255;
        const uint32_t *r0 = j->src + (size_t)y0 * sw, *r1 = j->src + (size_t)y1 * sw;
        for (int x = 0; x < ow; x++) {
            int fx = (int)(((x + 0.5) * rx - 0.5) * 65536.0);
            if (fx < 0) fx = 0;
            int x0 = fx >> 16, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1, wx = (fx >> 8) & 255;
            uint32_t c00 = r0[x0], c01 = r0[x1], c10 = r1[x0], c11 = r1[x1], out = 0;
            for (int s = 0; s < 24; s += 8) {
                int top = (int)((c00 >> s) & 255) * (256 - wx) + (int)((c01 >> s) & 255) * wx;
                int bot = (int)((c10 >> s) & 255) * (256 - wx) + (int)((c11 >> s) & 255) * wx;
                out |= (uint32_t)(((top * (256 - wy) + bot * wy) >> 16) & 255) << s;
            }
            dst[x] = out;
        }
    }
}

static void blit_screen(const uint32_t *src, int S, const int r[4]) {
    if (r[2] <= 0 || r[3] <= 0) return;
    BlitJob job = {src, S, r[0], r[1], r[2], r[3]};
    pool_run(r[3], blit_row, &job);
}

static void set_rect(int r[4], int x, int y, int w, int h) { r[0] = x; r[1] = y; r[2] = w; r[3] = h; }

/* Donde va cada pantalla en la ventana segun la disposicion: [0] arriba, [1] tactil. x, y, ancho, alto.
 * En ventana, a escalas enteras; en pantalla completa, lo mas grande que quepa en 4:3, centrado. */
static void screen_rects(int r[2][4]) {
    int W = present_w, H = present_h;
    if (!fullscreen) {
        int s = win_scale, small = s / 2 > 0 ? s / 2 : 1;
        set_rect(r[0], 0, 0, 256 * s, 192 * s);
        if (layout == 0) set_rect(r[1], 0, 192 * s, 256 * s, 192 * s);
        else if (layout == 1) set_rect(r[1], 256 * s, 0, 256 * s, 192 * s);
        else if (layout == 3) set_rect(r[1], 0, 0, 0, 0);
        else set_rect(r[1], 256 * s, (192 * s - 192 * small) / 2, 256 * small, 192 * small);
    } else if (layout == 0) { /* una encima de otra */
        int h = H / 2 < W * 3 / 4 ? H / 2 : W * 3 / 4, w = h * 4 / 3;
        set_rect(r[0], (W - w) / 2, (H - 2 * h) / 2, w, h);
        set_rect(r[1], (W - w) / 2, (H - 2 * h) / 2 + h, w, h);
    } else if (layout == 1) { /* una al lado de otra */
        int w = W / 2 < H * 4 / 3 ? W / 2 : H * 4 / 3, h = w * 3 / 4;
        set_rect(r[0], (W - 2 * w) / 2, (H - h) / 2, w, h);
        set_rect(r[1], (W - 2 * w) / 2 + w, (H - h) / 2, w, h);
    } else if (layout == 3) { /* solo la del juego */
        int w = W < H * 4 / 3 ? W : H * 4 / 3, h = w * 3 / 4;
        set_rect(r[0], (W - w) / 2, (H - h) / 2, w, h);
        set_rect(r[1], 0, 0, 0, 0);
    } else { /* la del juego grande (todo el alto si cabe) y la tactil al lado con lo que sobre (hasta media) */
        int mw = H * 4 / 3 < W * 3 / 4 ? H * 4 / 3 : W * 3 / 4, mh = mw * 3 / 4;
        int sw = W - mw < mw / 2 ? W - mw : mw / 2, sh = sw * 3 / 4, x0 = (W - mw - sw) / 2;
        set_rect(r[0], x0, (H - mh) / 2, mw, mh);
        set_rect(r[1], x0 + mw, (H - sh) / 2, sw, sh);
    }
    touch_rect.x = r[1][0]; touch_rect.y = r[1][1]; touch_rect.w = r[1][2]; touch_rect.h = r[1][3];
}

static void compose(void) {
    int S = internal_scale, r[2][4];
    screen_rects(r);
    blit_screen(frame, S, r[0]);
    blit_screen(frame + 256 * S * 192 * S, S, r[1]);
}

int win_gpu_present; /* la ventana la pinta la grafica (no hace falta la copia del procesador) */
int video_gpu(void);

#define WIN_STYLE (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)
static void resize_window(void) {
    if (fullscreen) { /* el monitor donde esta la ventana, entero */
        MONITORINFO mi = {sizeof mi};
        HMONITOR mon = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
        GetMonitorInfoA(mon, &mi);
        present_w = mi.rcMonitor.right - mi.rcMonitor.left;
        present_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        present = realloc(present, sizeof(uint32_t) * present_w * present_h);
        memset(present, 0, sizeof(uint32_t) * present_w * present_h);
        if (!window) return;
        SetWindowLongA(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, present_w, present_h, SWP_FRAMECHANGED);
        return;
    }
    layout_size(layout, win_scale, &present_w, &present_h);
    present = realloc(present, sizeof(uint32_t) * present_w * present_h);
    memset(present, 0, sizeof(uint32_t) * present_w * present_h); /* los huecos quedan en negro */
    if (!window) return;
    if ((DWORD)GetWindowLongA(window, GWL_STYLE) & WS_POPUP) { /* volver de pantalla completa */
        SetWindowLongA(window, GWL_STYLE, WIN_STYLE | WS_VISIBLE);
        SetWindowPos(window, 0, 40, 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    RECT r = {0, 0, present_w, present_h};
    AdjustWindowRect(&r, (DWORD)GetWindowLongA(window, GWL_STYLE), FALSE);
    SetWindowPos(window, 0, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        fprintf(stderr, "[ventana] cerrada por el usuario%c", 10);
        exit(0);
    case WM_KEYDOWN:
        if (wp == VK_F1 && !(lp & 0x40000000)) { /* cambiar la disposicion de las pantallas */
            layout = (layout + 1) % 4;
            resize_window();
        }
        if (wp == VK_F11 && !(lp & 0x40000000)) { /* pantalla completa / ventana */
            fullscreen = !fullscreen;
            resize_window();
        }
        if (wp == VK_ESCAPE && fullscreen) { fullscreen = 0; resize_window(); }
        if (wp == VK_F2 && !(lp & 0x40000000)) filter = !filter;
        if (wp == VK_F3 && !(lp & 0x40000000)) pixel_light = !pixel_light;
        if (wp == VK_F4 && !(lp & 0x40000000)) tess = (tess + 1) % 3;
        if (wp == VK_F5 && !(lp & 0x40000000)) fog = !fog;
        if (wp == VK_F6 && !(lp & 0x40000000)) shadows = !shadows;
        if (wp == VK_F7 && !(lp & 0x40000000)) shiny = !shiny;
        if (wp == VK_F8 && !(lp & 0x40000000)) bloom = !bloom;
        if (wp == VK_F9 && !(lp & 0x40000000)) gpu = !gpu;
        return 0;
    case WM_SYSKEYDOWN: /* Alt+Intro: pantalla completa */
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & 0x40000000)) {
            fullscreen = !fullscreen;
            resize_window();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
        int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
        int x = -1, y = -1;
        if (touch_rect.w > 0 && touch_rect.h > 0) {
            x = (mx - touch_rect.x) * 256 / touch_rect.w;
            y = (my - touch_rect.y) * 192 / touch_rect.h;
        }
        touch_down = (wp & MK_LBUTTON) && mx >= touch_rect.x && my >= touch_rect.y && x >= 0 && y >= 0 && x < 256 && y < 192;
        if (touch_down) { touch_x = x; touch_y = y; }
        if (msg == WM_LBUTTONDOWN) SetCapture(h);
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        return 0;
    }
    case WM_PAINT: {
        if (win_gpu_present) { ValidateRect(h, 0); return 0; } /* la pinta OpenGL */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), present_w, -present_h, 1, 32, BI_RGB}};
        SetDIBitsToDevice(dc, 0, 0, present_w, present_h, 0, 0, 0, present_h, present, &bi, DIB_RGB_COLORS);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcA(h, msg, wp, lp);
}

/* ---- guion de entrada (NFS_INPUT) ------------------------------------------------------------ */
#define MAX_EVENTS 512
static struct { uint32_t frame; int kind, a, b; } events[MAX_EVENTS]; /* kind: 0 touch, 1 release, 2 key, 3 hold, 4 unhold */
static int event_count, event_next;
static uint32_t script_keys, script_key_until[12];
static const char *KEY_NAMES[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};

static void load_script(const char *path) {
    FILE *f = fopen(path, "r");
    char line[128], word[32], name[32];
    if (!f) { fprintf(stderr, "[entrada] no se puede abrir %s%c", path, 10); return; }
    while (fgets(line, sizeof line, f) && event_count < MAX_EVENTS) {
        unsigned fr;
        int x = 0, y = 0;
        if (sscanf(line, "%u %31s", &fr, word) != 2) continue;
        events[event_count].frame = fr;
        if (!strcmp(word, "touch") && sscanf(line, "%*u %*s %d %d", &x, &y) == 2) {
            events[event_count].kind = 0; events[event_count].a = x; events[event_count].b = y;
        } else if (!strcmp(word, "release")) {
            events[event_count].kind = 1;
        } else if ((!strcmp(word, "key") || !strcmp(word, "hold") || !strcmp(word, "unhold")) &&
                   sscanf(line, "%*u %*s %31s", name) == 1) {
            int k = 0;
            while (k < 12 && strcmp(KEY_NAMES[k], name)) k++;
            if (k == 12) continue;
            events[event_count].kind = word[0] == 'k' ? 2 : word[0] == 'h' ? 3 : 4;
            events[event_count].a = k;
        } else {
            continue;
        }
        event_count++;
    }
    fclose(f);
    fprintf(stderr, "[entrada] guion %s: %d ordenes%c", path, event_count, 10);
}

static void run_script(void) {
    while (event_next < event_count && events[event_next].frame <= g_frame) {
        int kind = events[event_next].kind, a = events[event_next].a;
        if (kind == 0) { touch_down = 1; touch_x = a; touch_y = events[event_next].b; }
        else if (kind == 1) touch_down = 0;
        else if (kind == 2) script_key_until[a] = g_frame + 6;
        else script_key_until[a] = kind == 3 ? 0xFFFFFFFFu : 0;
        event_next++;
    }
    script_keys = 0;
    for (int k = 0; k < 12; k++)
        if (script_key_until[k] > g_frame) script_keys |= 1u << k;
}

/* ---- una vez por fotograma ---------------------------------------------------------------------- */
static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    configure();
    frame = calloc((size_t)256 * internal_scale * 384 * internal_scale, 4);
    if (getenv("NFS_INPUT")) load_script(getenv("NFS_INPUT"));
    if (headless) return;
    resize_window();
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursorA(0, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "NfsNitroPort";
    RegisterClassA(&wc);
    RECT r = {0, 0, present_w, present_h};
    DWORD style = fullscreen ? WS_POPUP : WIN_STYLE;
    AdjustWindowRect(&r, style, FALSE);
    window = CreateWindowA("NfsNitroPort", "NFS Nitro (port)  -  F1 pantallas  F2 filtro  F3 luz por pixel  F4 subdivision  F5 niebla  F6 sombras  F7 brillo  F8 resplandor  F9 grafica/software  F11 pantalla completa", style | WS_VISIBLE,
                           fullscreen ? 0 : CW_USEDEFAULT, fullscreen ? 0 : CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    if (fullscreen) resize_window(); /* al monitor donde ha quedado */
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&next_tick);
}

/* NFS_SPIKES=ms: apunta cada fotograma que tarde mas de esos milisegundos (tirones), con lo que paso */
static void spike_check(void) {
    static int threshold = -1;
    static LARGE_INTEGER f, last;
    static double last_3d, last_tex;
    static int last_dec, last_disk, last_flush;
    extern double perf_3d_ms, perf_tex_ms;
    extern int perf_tex_decoded, perf_tex_disk, perf_tex_flushes, perf_polys, perf_trans, perf_raw;
    if (threshold < 0) threshold = env_int("NFS_SPIKES", 0);
    if (!threshold) return;
    LARGE_INTEGER now;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    if (last.QuadPart) {
        double ms = (double)(now.QuadPart - last.QuadPart) * 1000 / (double)f.QuadPart;
        if (ms > threshold)
            fprintf(stderr, "[tiron] fotograma %u: %.1f ms | 3D %.1f ms (%d enviados, %d dibujados, %d translucidos) | texturas %.1f ms: %d descodificadas, %d buscadas en disco, %d renovaciones%c",
                    g_frame, ms, perf_3d_ms - last_3d, perf_raw, perf_polys, perf_trans, perf_tex_ms - last_tex, perf_tex_decoded - last_dec,
                    perf_tex_disk - last_disk, perf_tex_flushes - last_flush, 10);
    }
    last = now;
    last_3d = perf_3d_ms; last_tex = perf_tex_ms;
    last_dec = perf_tex_decoded; last_disk = perf_tex_disk; last_flush = perf_tex_flushes;
}

void win_frame(void) {
    init();
    spike_check();
    run_script();
    LARGE_INTEGER p0, p1, p2;
    QueryPerformanceCounter(&p0);
    int shot = shot_every && g_frame >= shot_from && g_frame <= shot_to && g_frame % (uint32_t)shot_every == 0;
    if (!headless && !shot && video_gpu()) { /* con la grafica: ella mezcla el 2D con el 3D y pinta la ventana */
        int r[2][4];
        screen_rects(r);
        win_gpu_present = video_present_gpu(r[0], r[1], window, present_w, present_h);
    } else {
        win_gpu_present = 0;
    }
    if ((!headless && !win_gpu_present) || shot) video_render(frame);
    if (shot) {
        char name[64];
        snprintf(name, sizeof name, "shot_%05u.png", g_frame);
        video_save_png(name, frame, 256 * internal_scale, 384 * internal_scale);
    }
    if (headless) return;

    QueryPerformanceCounter(&p1);
    if (!win_gpu_present) compose();
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (!win_gpu_present) {
        InvalidateRect(window, 0, FALSE);
        UpdateWindow(window);
    }

    QueryPerformanceCounter(&p2);
    perf_2d += (double)(p1.QuadPart - p0.QuadPart) * 1000 / (double)freq.QuadPart;
    perf_present += (double)(p2.QuadPart - p1.QuadPart) * 1000 / (double)freq.QuadPart;

    /* 60 fotogramas por segundo */
    LARGE_INTEGER now;
    next_tick.QuadPart += freq.QuadPart / 60;
    QueryPerformanceCounter(&now);
    if (getenv("NFS_PERF")) { /* cuanto tarda de verdad cada fotograma (sin contar la espera) */
        static double sum, worst;
        static int n;
        if (perf_last.QuadPart) {
            double ms = (double)(now.QuadPart - perf_last.QuadPart) * 1000 / (double)freq.QuadPart;
            sum += ms;
            if (ms > worst) worst = ms;
            if (++n == 300) {
                extern double perf_3d_ms;
                extern int perf_tex_flushes, perf_tex_evicted;
                fprintf(stderr, "[rendimiento] fotograma %u: media %.1f ms, peor %.1f ms (limite 16.7) | 3D %.1f, capas 2D %.1f, ventana %.1f, texturas renovadas %d, echadas %d%c",
                        g_frame, sum / n, worst, perf_3d_ms / n, perf_2d / n, perf_present / n, perf_tex_flushes, perf_tex_evicted, 10);
                perf_tex_flushes = 0;
                perf_3d_ms = perf_2d = perf_present = 0;
                sum = worst = 0;
                n = 0;
            }
        }
    }
    if (now.QuadPart > next_tick.QuadPart + freq.QuadPart / 4) next_tick = now; /* muy retrasado: no recuperar */
    while (now.QuadPart < next_tick.QuadPart) {
        LONGLONG left_ms = (next_tick.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
        if (left_ms > 1) Sleep((DWORD)(left_ms - 1));
        QueryPerformanceCounter(&now);
    }
    perf_last = now;
}

void win_save_screenshot(const char *path) {
    init();
    video_render(frame);
    int S = internal_scale, K = S == 1 ? 3 : 1, w = 256 * S, h = 384 * S; /* a tamano original, ampliada x3 */
    uint32_t *big = malloc(sizeof(uint32_t) * w * K * h * K);
    for (int y = 0; y < h * K; y++)
        for (int x = 0; x < w * K; x++) big[y * w * K + x] = frame[(y / K) * w + x / K];
    video_save_png(path, big, w * K, h * K);
    free(big);
    for (int e = 0; e < 2; e++) { /* estado de los dos motores 2D, para depurar */
        uint8_t *r = io_mem(e);
        uint32_t d = r[0] | (r[1] << 8) | (r[2] << 16) | ((uint32_t)r[3] << 24);
        fprintf(stderr, "[video] motor %c: DISPCNT=%08x modo=%u BG0-3=%04x %04x %04x %04x brillo=%04x%c", 'A' + e, d,
                d & 7, r[8] | (r[9] << 8), r[10] | (r[11] << 8), r[12] | (r[13] << 8), r[14] | (r[15] << 8),
                r[0x6C] | (r[0x6D] << 8), 10);
    }
    fprintf(stderr, "[video] POWCNT1=%04x VRAMCNT A-G=%08x %08x H,I=%04x%c", io_reg(0x304) & 0xFFFF, io_reg(0x240),
            io_reg(0x244), io_reg(0x248) & 0xFFFF, 10);
}

/* ---- botones ------------------------------------------------------------------------------------- */
static int key(int vk) { return !headless && window && GetForegroundWindow() == window && (GetAsyncKeyState(vk) & 0x8000); }

/* Mando por XInput (la DLL se carga al vuelo: si no esta, no hay mando y ya) */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } PadGamepad;
typedef struct { DWORD packet; PadGamepad pad; } PadState;
static uint32_t pad_buttons(void) { /* bits en el orden de KEYINPUT; 0x400 = X, 0x800 = Y de la consola */
    static DWORD (WINAPI *get_state)(DWORD, PadState *);
    static int tried;
    static DWORD last_try;
    if (!tried) {
        tried = 1;
        HMODULE dll = LoadLibraryA("xinput1_4.dll");
        if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
        if (dll) get_state = (DWORD (WINAPI *)(DWORD, PadState *))(void *)GetProcAddress(dll, "XInputGetState");
    }
    if (!get_state || headless || !window || GetForegroundWindow() != window) return 0;
    static int connected = -1;
    if (connected == 0 && GetTickCount() - last_try < 2000) return 0; /* sin mando: buscarlo cada 2 s (es lento) */
    PadState st;
    for (DWORD i = 0; i < 4; i++) {
        if (get_state(i, &st) != 0) continue;
        if (connected != 1) fprintf(stderr, "[mando] conectado (%lu)%c", (unsigned long)i, 10);
        connected = 1;
        const PadGamepad *g = &st.pad;
        uint32_t b = 0;
        if ((g->buttons & 0x1000) || g->rt > 60) b |= 0x001;  /* A o gatillo derecho -> A */
        if ((g->buttons & 0x2000) || g->lt > 60) b |= 0x002;  /* B o gatillo izquierdo -> B */
        if (g->buttons & 0x0020) b |= 0x004;                  /* Back -> Select */
        if (g->buttons & 0x0010) b |= 0x008;                  /* Start */
        if ((g->buttons & 0x0008) || g->lx > 12000) b |= 0x010;
        if ((g->buttons & 0x0004) || g->lx < -12000) b |= 0x020;
        if ((g->buttons & 0x0001) || g->ly > 12000) b |= 0x040;
        if ((g->buttons & 0x0002) || g->ly < -12000) b |= 0x080;
        if (g->buttons & 0x0200) b |= 0x100;                  /* RB -> R (nitro) */
        if (g->buttons & 0x0100) b |= 0x200;                  /* LB -> L (freno de mano) */
        if (g->buttons & 0x8000) b |= 0x400;                  /* Y -> X */
        if (g->buttons & 0x4000) b |= 0x800;                  /* X -> Y */
        return b;
    }
    if (connected == 1) fprintf(stderr, "[mando] desconectado%c", 10);
    connected = 0;
    last_try = GetTickCount();
    return 0;
}

/* Registro KEYINPUT (0x04000130): un bit a 0 = boton pulsado */
uint32_t win_keys(void) {
    init();
    uint32_t k = 0x3FF & ~(pad_buttons() & 0x3FF);
    if (key('Z')) k &= ~0x001u;        /* A */
    if (key('X')) k &= ~0x002u;        /* B */
    if (key(VK_BACK)) k &= ~0x004u;    /* Select */
    if (key(VK_RETURN)) k &= ~0x008u;  /* Start */
    if (key(VK_RIGHT)) k &= ~0x010u;
    if (key(VK_LEFT)) k &= ~0x020u;
    if (key(VK_UP)) k &= ~0x040u;
    if (key(VK_DOWN)) k &= ~0x080u;
    if (key('W')) k &= ~0x100u;        /* R */
    if (key('Q')) k &= ~0x200u;        /* L */
    return k & ~(script_keys & 0x3FF);
}

/* Botones que en la consola lee el ARM7: bit 0 = X, bit 1 = Y, bit 6 = lapiz levantado */
uint32_t win_extra_keys(void) {
    uint32_t k = 0x7F, pad = pad_buttons();
    if (key('S') || (script_keys & 0x400) || (pad & 0x400)) k &= ~1u;
    if (key('A') || (script_keys & 0x800) || (pad & 0x800)) k &= ~2u;
    if (touch_down) k &= ~0x40u;
    return k;
}

int win_touch(int *x, int *y) {
    *x = touch_x;
    *y = touch_y;
    return touch_down;
}
