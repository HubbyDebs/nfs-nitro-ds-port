/* Menu de opciones que sale antes de arrancar el juego: disposicion de pantallas, resolucion y
 * mejoras graficas. Lo elegido se guarda en opciones.ini (en la carpeta desde la que se arranca,
 * junto a la partida guardada) y se pasa al resto del programa como las mismas variables de
 * entorno NFS_* de siempre, asi win.c y gx.c no cambian.
 *
 * No sale con NFS_HEADLESS (pruebas automaticas) ni con NFS_NOMENU=1. Si una variable NFS_* ya
 * viene puesta desde fuera, el menu empieza con ese valor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <shlobj.h>
#include "controls.h"

#define INI_FILE "opciones.ini"
#define MAX_ITEMS 10

typedef struct Combo {
    const char *env;
    const wchar_t *label;
    const wchar_t *items[MAX_ITEMS]; /* terminada en 0 */
    const char *values[MAX_ITEMS];   /* 0 = automatico (no se pone la variable) */
    int deflt, sel;
    HWND ctl;
} Combo;

typedef struct Check {
    const char *env;
    const wchar_t *label;
    int deflt;
    int only_when_on; /* la variable solo se pone si esta marcada (el programa mira si existe); no se
                       * guarda: vale solo para esta partida (la marca capturar_texturas.cmd) */
    int on;
    HWND ctl;
} Check;

static Combo combos[] = {
    {"NFS_LAYOUT", L"Disposición de las pantallas",
     {L"Pantalla del juego grande y la táctil al lado", L"Una encima de otra (como la consola)", L"Una al lado de otra",
      L"Solo la pantalla del juego (F1 para ver la táctil)", 0},
     {"2", "0", "1", "3"}, 0},
    {"NFS_SCALE", L"Tamaño de la ventana",
     {L"Automático (lo más grande que quepa)", L"x2", L"x3", L"x4", L"x5", L"x6", 0},
     {0, "2", "3", "4", "5", "6"}, 0},
    {"NFS_3D_SCALE", L"Resolución del 3D",
     {L"Automática (la de la ventana)", L"x1   256 x 192 (como la consola)", L"x2   512 x 384", L"x3   768 x 576",
      L"x4   1024 x 768", L"x5   1280 x 960", L"x6   1536 x 1152 (solo con la gráfica)", L"x8   2048 x 1536 (gráfica, lento)", 0},
     {0, "1", "2", "3", "4", "5", "6", "8"}, 0},
    {"NFS_AA", L"Supermuestreo",
     {L"Automático", L"No", L"x2 (más lento)", 0},
     {0, "1", "2"}, 0},
    {"NFS_TESS", L"Subdivisión de los coches",
     {L"No (polígonos originales)", L"1 nivel (recomendado)", L"2 niveles (más lento)", 0},
     {"0", "1", "2"}, 1},
    {"NFS_MSAA", L"Suavizado de bordes (gráfica)",
     {L"No", L"x2", L"x4 (recomendado)", L"x8", 0},
     {"1", "2", "4", "8"}, 2},
};

static Check checks[] = {
    {"NFS_SOUND", L"Sonido", 1, 0},
    {"NFS_FULLSCREEN", L"Pantalla completa (F11 o Alt+Intro para cambiar al jugar; Esc vuelve a ventana)", 0, 0},
    {"NFS_GPU", L"Dibujar el 3D con la tarjeta gráfica (si falla, se usa el procesador)", 1, 0},
    {"NFS_FILTER", L"Texturas suavizadas (filtro bilineal)", 1, 0},
    {"NFS_TEXPACK", L"Texturas ampliadas con IA", 1, 0},
    {"NFS_PIXEL_LIGHT", L"Luz por píxel", 1, 0},
    {"NFS_FOG", L"Niebla", 1, 0},
    {"NFS_SHADOWS", L"Sombras", 1, 0},
    {"NFS_SHINY", L"Coches con barniz: reflejo del cielo y brillos (mejora del port)", 1, 0},
    {"NFS_BLOOM", L"Resplandor en las zonas muy brillantes (bloom)", 1, 0},
    {"NFS_MESHES", L"Modelos 3D nuevos (carpeta nfs-nitro-models\\reemplazos)", 1, 0},
    {"NFS_FPS120", L"120 imágenes por segundo (experimental; para monitores de 120 Hz o más)", 0, 0},
    {"NFS_SHARP", L"Texturas nítidas también de lejos (filtro anisotrópico x16)", 1, 0},
    {"NFS_TEXDUMP", L"Guardar las texturas nuevas para ampliarlas después", 0, 1},
};

#define N_COMBOS (int)(sizeof combos / sizeof combos[0])
#define N_CHECKS (int)(sizeof checks / sizeof checks[0])
#define ID_PLAY IDOK
#define ID_QUIT IDCANCEL
#define ID_PRESET 300
#define ID_CONTROLS 302

/* Controles (controls.h): tecla y mando de cada boton de la consola */
static int ctl_key[DS_BUTTONS];
static unsigned ctl_pad[DS_BUTTONS];

/* Perfiles de calidad. Cada numero es la opcion elegida en el desplegable o la casilla, en el mismo
 * orden que combos[] y checks[]; -1 = no se toca (la disposicion y el tamano de la ventana no son
 * mejoras, y lo de guardar texturas es aparte). "Original" deja la niebla y las sombras porque la
 * consola tambien las tiene. */
typedef struct Preset {
    const wchar_t *name;
    int combo[6], check[14];
} Preset;

static const Preset presets[] = {
    {L"Personalizada", {-1, -1, -1, -1, -1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}},
    {L"Original (como la consola, sin mejoras)", {-1, -1, 1, 1, 0, 0}, {-1, -1, -1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, -1}},
    {L"Recomendada", {-1, -1, 0, 0, 1, 2}, {-1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, 1, -1}},
    {L"Máxima potencia gráfica (puede ir lento)", {-1, -1, 6, 0, 2, 3}, {-1, -1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, 1, -1}},
};
#define N_PRESETS (int)(sizeof presets / sizeof presets[0])
static HWND preset_ctl;

static int result = -1;

/* ---- opciones.ini -------------------------------------------------------------------------- */
/* Carpeta del juego (la ROM extraida con ndstool): argumento del programa, opciones.ini o C:\nfs */
static char rom_dir[MAX_PATH * 2] = "C:\\nfs";
static HWND rom_edit;
#define ID_ROM_BROWSE 401

static const char *ini_value(char lines[][600], int n, const char *key) {
    size_t k = strlen(key);
    for (int i = 0; i < n; i++)
        if (!strncmp(lines[i], key, k) && lines[i][k] == '=') return lines[i] + k + 1;
    return 0;
}

static void load_options(void) {
    static char lines[64][600];
    int n = 0;
    FILE *f = fopen(INI_FILE, "r");
    if (f) {
        while (n < 64 && fgets(lines[n], sizeof lines[n], f)) {
            lines[n][strcspn(lines[n], "\r\n")] = 0;
            n++;
        }
        fclose(f);
    }
    const char *rom = getenv("NFS_ROM") ? getenv("NFS_ROM") : ini_value(lines, n, "NFS_ROM");
    if (rom && *rom) snprintf(rom_dir, sizeof rom_dir, "%s", rom);
    for (int c = 0; c < N_COMBOS; c++) {
        Combo *o = &combos[c];
        const char *v = getenv(o->env) ? getenv(o->env) : ini_value(lines, n, o->env);
        o->sel = o->deflt;
        if (!v) continue;
        for (int i = 0; o->items[i]; i++)
            if (o->values[i] ? !strcmp(o->values[i], v) : !strcmp(v, "auto")) o->sel = i;
    }
    for (int c = 0; c < N_CHECKS; c++) {
        Check *o = &checks[c];
        const char *v = getenv(o->env) ? getenv(o->env) : o->only_when_on ? 0 : ini_value(lines, n, o->env);
        o->on = v ? atoi(v) != 0 : o->deflt;
    }
    for (int b = 0; b < DS_BUTTONS; b++) {
        char name[32];
        snprintf(name, sizeof name, "NFS_KEY_%s", control_defs[b].env);
        const char *v = getenv(name) ? getenv(name) : ini_value(lines, n, name);
        ctl_key[b] = v ? (int)strtol(v, 0, 0) : control_defs[b].key;
        snprintf(name, sizeof name, "NFS_PAD_%s", control_defs[b].env);
        v = getenv(name) ? getenv(name) : ini_value(lines, n, name);
        ctl_pad[b] = v ? (unsigned)strtoul(v, 0, 0) : control_defs[b].pad;
    }
}

static void save_and_apply(void) {
    FILE *f = fopen(INI_FILE, "w");
    if (f) fprintf(f, "# Opciones del port de NFS Nitro (las cambia el menu del principio)\n");
    _putenv_s("NFS_ROM", rom_dir);
    if (f) fprintf(f, "NFS_ROM=%s\n", rom_dir);
    for (int c = 0; c < N_COMBOS; c++) {
        Combo *o = &combos[c];
        const char *v = o->values[o->sel];
        _putenv_s(o->env, v ? v : ""); /* "" la quita: automatico */
        if (f) fprintf(f, "%s=%s\n", o->env, v ? v : "auto");
    }
    for (int c = 0; c < N_CHECKS; c++) {
        Check *o = &checks[c];
        _putenv_s(o->env, o->on ? "1" : (o->only_when_on ? "" : "0"));
        if (f && !o->only_when_on) fprintf(f, "%s=%d\n", o->env, o->on);
    }
    for (int b = 0; b < DS_BUTTONS; b++) {
        char name[32], v[32];
        snprintf(name, sizeof name, "NFS_KEY_%s", control_defs[b].env);
        snprintf(v, sizeof v, "0x%02X", ctl_key[b]);
        _putenv_s(name, v);
        if (f) fprintf(f, "%s=%s\n", name, v);
        snprintf(name, sizeof name, "NFS_PAD_%s", control_defs[b].env);
        snprintf(v, sizeof v, "0x%06X", ctl_pad[b]);
        _putenv_s(name, v);
        if (f) fprintf(f, "%s=%s\n", name, v);
    }
    if (f) fclose(f);
}

/* ---- ventana --------------------------------------------------------------------------------- */
/* Controles con el aspecto actual de Windows (sin esto saldrian con el de Windows 95) */
static void enable_visual_styles(void) {
    static const char manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\"><dependency><dependentAssembly>"
        "<assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\" version=\"6.0.0.0\" "
        "processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
        "</dependentAssembly></dependency></assembly>\n";
    wchar_t path[MAX_PATH + 40];
    DWORD n = GetTempPathW(MAX_PATH, path);
    if (!n || n > MAX_PATH) return;
    wcscat(path, L"nfs_nitro_estilos.manifest");
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fwrite(manifest, 1, sizeof manifest - 1, f);
    fclose(f);
    ACTCTXW ac = {0};
    ac.cbSize = sizeof ac;
    ac.lpSource = path;
    HANDLE h = CreateActCtxW(&ac);
    ULONG_PTR cookie;
    if (h != INVALID_HANDLE_VALUE) ActivateActCtx(h, &cookie);
    HMODULE cc = LoadLibraryW(L"comctl32.dll");
    if (cc) {
        void(WINAPI * init)(void) = (void(WINAPI *)(void))GetProcAddress(cc, "InitCommonControls");
        if (init) init();
    }
}

static HFONT font, font_title, font_head, font_small;

static HWND add(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id,
                HFONT fnt) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent, (HMENU)(INT_PTR)id,
                             GetModuleHandleW(0), 0);
    SendMessageW(c, WM_SETFONT, (WPARAM)fnt, TRUE);
    return c;
}

static void show_values(void) {
    for (int c = 0; c < N_COMBOS; c++) SendMessageW(combos[c].ctl, CB_SETCURSEL, combos[c].sel, 0);
    for (int c = 0; c < N_CHECKS; c++) SendMessageW(checks[c].ctl, BM_SETCHECK, checks[c].on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void read_values(void) {
    for (int c = 0; c < N_COMBOS; c++) {
        LRESULT s = SendMessageW(combos[c].ctl, CB_GETCURSEL, 0, 0);
        if (s >= 0) combos[c].sel = (int)s;
    }
    for (int c = 0; c < N_CHECKS; c++) checks[c].on = SendMessageW(checks[c].ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* Que perfil coincide con lo elegido (0 = ninguno: "Personalizada") */
static int matching_preset(void) {
    for (int p = 1; p < N_PRESETS; p++) {
        int ok = 1;
        for (int c = 0; c < N_COMBOS; c++)
            if (presets[p].combo[c] >= 0 && combos[c].sel != presets[p].combo[c]) ok = 0;
        for (int c = 0; c < N_CHECKS; c++)
            if (presets[p].check[c] >= 0 && checks[c].on != presets[p].check[c]) ok = 0;
        if (ok) return p;
    }
    return 0;
}

static void apply_preset(int p) {
    for (int c = 0; c < N_COMBOS; c++)
        if (presets[p].combo[c] >= 0) combos[c].sel = presets[p].combo[c];
    for (int c = 0; c < N_CHECKS; c++)
        if (presets[p].check[c] >= 0) checks[c].on = presets[p].check[c];
    show_values();
}

/* NFS_MENU_SNAPSHOT=prefijo (pruebas): el menu y la ventana de controles se dibujan fuera de la vista y se
 * guardan como <prefijo>_menu.png y <prefijo>_controles.png, sin esperar al usuario */
void video_save_png(const char *path, const unsigned int *pixels, int w, int h);
static void snapshot(HWND h, const char *suffix) {
    MSG m;
    for (int i = 0; i < 20; i++) { /* que se termine de pintar */
        while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(10);
    }
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC wdc = GetDC(h), mdc = CreateCompatibleDC(wdc);
    BITMAPINFO bi = {{sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB}};
    void *bits;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    HGDIOBJ old = SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 2 /* PW_RENDERFULLCONTENT */);
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s_%s.png", getenv("NFS_MENU_SNAPSHOT"), suffix);
    video_save_png(path, bits, w, hh);
    SelectObject(mdc, old);
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(h, wdc);
}

/* ---- ventana "Controles" ----------------------------------------------------------------------- */
static HWND ctl_key_btn[DS_BUTTONS], ctl_pad_btn[DS_BUTTONS];
static int capture_kind, capture_btn; /* 0 nada, 1 esperando tecla, 2 esperando mando */
static unsigned capture_prev;
static int controls_done;

static void key_name(int vk, wchar_t *out, int n) {
    if (!vk) { wcsncpy(out, L"(ninguna)", n); return; }
    switch (vk) { /* las que GetKeyNameText nombra mal o en ingles */
    case VK_LEFT: wcsncpy(out, L"Flecha izquierda", n); return;
    case VK_RIGHT: wcsncpy(out, L"Flecha derecha", n); return;
    case VK_UP: wcsncpy(out, L"Flecha arriba", n); return;
    case VK_DOWN: wcsncpy(out, L"Flecha abajo", n); return;
    case VK_RETURN: wcsncpy(out, L"Intro", n); return;
    case VK_BACK: wcsncpy(out, L"Retroceso", n); return;
    case VK_SPACE: wcsncpy(out, L"Espacio", n); return;
    case VK_SHIFT: case VK_LSHIFT: wcsncpy(out, L"Mayúsculas", n); return;
    case VK_CONTROL: case VK_LCONTROL: wcsncpy(out, L"Control", n); return;
    case VK_TAB: wcsncpy(out, L"Tabulador", n); return;
    }
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    if (vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT) lp |= 1 << 24;
    if (!sc || !GetKeyNameTextW(lp, out, n)) _snwprintf(out, n, L"Tecla %d", vk);
}

static void pad_text(unsigned mask, wchar_t *out, int n) {
    out[0] = 0;
    if (!mask) { wcsncpy(out, L"(ninguno)", n); return; }
    for (unsigned bit = 1; bit && bit <= PAD_LS_DOWN; bit <<= 1)
        if (mask & bit) {
            if (out[0]) wcsncat(out, L" o ", n - wcslen(out) - 1);
            wcsncat(out, pad_name(bit), n - wcslen(out) - 1);
        }
}

static void controls_show(void) {
    wchar_t t[128];
    for (int b = 0; b < DS_BUTTONS; b++) {
        if (capture_kind == 1 && capture_btn == b) wcscpy(t, L"Pulsa una tecla… (Esc: cancelar)");
        else key_name(ctl_key[b], t, 128);
        SetWindowTextW(ctl_key_btn[b], t);
        if (capture_kind == 2 && capture_btn == b) wcscpy(t, L"Pulsa en el mando… (Esc: cancelar)");
        else pad_text(ctl_pad[b], t, 128);
        SetWindowTextW(ctl_pad_btn[b], t);
    }
}

static void capture_start(HWND h, int kind, int b) {
    capture_kind = kind;
    capture_btn = b;
    capture_prev = kind == 2 ? pad_read_raw(0) : 0;
    if (kind == 2) SetTimer(h, 1, 30, 0);
    controls_show();
}

static void capture_end(HWND h) {
    KillTimer(h, 1);
    capture_kind = 0;
    controls_show();
}

static LRESULT CALLBACK controls_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= 600 && id < 600 + DS_BUTTONS) capture_start(h, 1, id - 600);
        else if (id >= 700 && id < 700 + DS_BUTTONS) capture_start(h, 2, id - 700);
        else if (id == 800) { /* restablecer */
            for (int b = 0; b < DS_BUTTONS; b++) { ctl_key[b] = control_defs[b].key; ctl_pad[b] = control_defs[b].pad; }
            capture_end(h);
        } else if (id == 801 || id == IDOK || id == IDCANCEL) DestroyWindow(h);
        return 0;
    }
    case WM_TIMER: { /* esperando un control del mando: el primero que se pulse ahora */
        int con;
        unsigned now = pad_read_raw(&con), fresh = now & ~capture_prev;
        capture_prev = now;
        if (capture_kind == 2 && fresh) {
            unsigned bit = fresh & (~fresh + 1);
            for (int b = 0; b < DS_BUTTONS; b++) ctl_pad[b] &= ~bit; /* cada control para un solo boton */
            ctl_pad[capture_btn] = bit;
            capture_end(h);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: KillTimer(h, 1); controls_done = 1; return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void controls_window(HWND parent) {
    static int registered;
    if (!registered) {
        registered = 1;
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = controls_proc;
        wc.hInstance = GetModuleHandleW(0);
        wc.lpszClassName = L"NfsNitroControles";
        wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        RegisterClassW(&wc);
    }
    const int CW = 760, M = 20, LW = 200, KW = 230, row = 34;
    int ch = 16 + 30 + 44 + 28 + DS_BUTTONS * row + 16 + 34 + 18;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT r = {0, 0, CW, ch};
    AdjustWindowRect(&r, style, FALSE);
    RECT pr;
    GetWindowRect(parent, &pr);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND h = CreateWindowExW(0, L"NfsNitroControles", L"Controles", style, pr.left + ((pr.right - pr.left) - ww) / 2,
                             pr.top + 40, ww, wh, parent, 0, GetModuleHandleW(0), 0);
    int y = 16;
    add(h, L"STATIC", L"Controles", 0, M, y, CW - 2 * M, 30, 0, font_title);
    y += 30;
    add(h, L"STATIC", L"Haz clic en una casilla y pulsa la tecla o el control del mando que quieras para ese botón de la "
                      L"consola. Las teclas F1-F11 y Esc están reservadas.", 0, M, y, CW - 2 * M, 40, 0, font_small);
    y += 44;
    add(h, L"STATIC", L"Botón de la consola", 0, M, y, LW, 24, 0, font_head);
    add(h, L"STATIC", L"Teclado", 0, M + LW, y, KW, 24, 0, font_head);
    add(h, L"STATIC", L"Mando", 0, M + LW + KW + 10, y, CW - 2 * M - LW - KW - 10, 24, 0, font_head);
    y += 28;
    for (int b = 0; b < DS_BUTTONS; b++) {
        add(h, L"STATIC", control_defs[b].label, 0, M, y + 5, LW, 22, 0, font);
        ctl_key_btn[b] = add(h, L"BUTTON", L"", WS_TABSTOP, M + LW, y, KW, 28, 600 + b, font);
        ctl_pad_btn[b] = add(h, L"BUTTON", L"", WS_TABSTOP, M + LW + KW + 10, y, CW - 2 * M - LW - KW - 10, 28, 700 + b, font);
        y += row;
    }
    y += 16;
    add(h, L"BUTTON", L"Restablecer", WS_TABSTOP, M, y, 140, 34, 800, font);
    add(h, L"BUTTON", L"Aceptar", BS_DEFPUSHBUTTON | WS_TABSTOP, CW - M - 140, y, 140, 34, 801, font);
    capture_kind = 0;
    controls_show();
    if (getenv("NFS_MENU_SNAPSHOT")) { /* pruebas */
        SetWindowLongW(h, GWL_EXSTYLE, GetWindowLongW(h, GWL_EXSTYLE) | WS_EX_LAYERED); SetLayeredWindowAttributes(h, 0, 1, LWA_ALPHA); /* transparente: no se ve */
        ShowWindow(h, SW_SHOWNOACTIVATE);
        snapshot(h, "controles");
        capture_start(h, 1, 0); /* asi se ve esperando una tecla */
        snapshot(h, "controles_esperando");
        DestroyWindow(h);
        return;
    }
    EnableWindow(parent, FALSE);
    ShowWindow(h, SW_SHOW);
    controls_done = 0;
    MSG m;
    while (!controls_done && GetMessageW(&m, 0, 0, 0) > 0) {
        if (capture_kind && (m.message == WM_KEYDOWN || m.message == WM_SYSKEYDOWN)) { /* la tecla elegida */
            int vk = (int)m.wParam;
            if (vk == VK_ESCAPE) capture_end(h);
            else if (capture_kind == 1 && !(vk >= VK_F1 && vk <= VK_F12) && vk != VK_MENU) {
                for (int b = 0; b < DS_BUTTONS; b++)
                    if (ctl_key[b] == vk) ctl_key[b] = 0; /* cada tecla para un solo boton */
                ctl_key[capture_btn] = vk;
                capture_end(h);
            }
            continue;
        }
        if (capture_kind && (m.message == WM_KEYUP || m.message == WM_CHAR || m.message == WM_SYSKEYUP)) continue;
        if (IsDialogMessageW(h, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
}

static LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == ID_PLAY) {
            wchar_t w[MAX_PATH * 2];
            GetWindowTextW(rom_edit, w, MAX_PATH * 2);
            WideCharToMultiByte(CP_ACP, 0, w, -1, rom_dir, sizeof rom_dir, 0, 0);
            char probe[MAX_PATH * 2 + 16];
            snprintf(probe, sizeof probe, "%s\\arm9.bin", rom_dir);
            if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
                MessageBoxW(h, L"En esa carpeta no está el juego extraído (falta arm9.bin).\n\n"
                               L"Elige la carpeta donde extrajiste la ROM con ndstool: la que tiene arm9.bin, "
                               L"header.bin y las carpetas data y overlay.", L"Carpeta del juego", MB_ICONWARNING);
                return 0;
            }
            read_values(); result = 1; DestroyWindow(h);
        }
        else if (id == ID_ROM_BROWSE) {
            BROWSEINFOW bi = {0};
            bi.hwndOwner = h;
            bi.lpszTitle = L"Carpeta del juego extraído (la que tiene arm9.bin, data y overlay)";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t w[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, w)) SetWindowTextW(rom_edit, w);
                CoTaskMemFree(pidl);
            }
        }
        else if (id == ID_QUIT) { result = 0; DestroyWindow(h); }
        else if (id == ID_CONTROLS) controls_window(h);
        else if (id == ID_PRESET && code == CBN_SELCHANGE) {
            int p = (int)SendMessageW(preset_ctl, CB_GETCURSEL, 0, 0);
            if (p > 0) apply_preset(p);
        } else if ((id >= 100 && id < 100 + N_COMBOS && code == CBN_SELCHANGE) ||
                   (id >= 200 && id < 200 + N_CHECKS && code == BN_CLICKED)) {
            read_values(); /* se ha tocado algo a mano: el perfil pasa a ser el que coincida, o "Personalizada" */
            SendMessageW(preset_ctl, CB_SETCURSEL, matching_preset(), 0);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: /* textos y casillas sobre fondo blanco */
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        result = 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* Devuelve 1 para jugar, 0 si el usuario cierra o pulsa Salir */
int launcher_menu(void) {
    load_options();
    enable_visual_styles();
    font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    font_title = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    font_head = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    font_small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = menu_proc;
    wc.hInstance = GetModuleHandleW(0);
    wc.lpszClassName = L"NfsNitroMenu";
    wc.hCursor = LoadCursorW(0, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(0, (LPCWSTR)IDI_APPLICATION);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    RegisterClassW(&wc);

    const int CW = 600, M = 24, LW = 210, row = 36;
    int y = 18;
    int ch = 18 + 34 + 30 + row + row + 14 + N_COMBOS * row + 34 + N_CHECKS * 28 + 16 + 58 + 20 + 34 + 20;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, CW, ch};
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    HWND win = CreateWindowExW(0, L"NfsNitroMenu", L"Need for Speed: Nitro (port) - Opciones", style,
                               (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                               0, 0, GetModuleHandleW(0), 0);

    add(win, L"STATIC", L"Need for Speed: Nitro", 0, M, y, CW - 2 * M, 30, 0, font_title);
    y += 34;
    add(win, L"STATIC", L"Elige la pantalla y las mejoras gráficas. Se guardan para la próxima vez.", 0, M, y,
        CW - 2 * M, 22, 0, font_small);
    y += 30;
    { /* carpeta del juego */
        wchar_t w[MAX_PATH * 2];
        MultiByteToWideChar(CP_ACP, 0, rom_dir, -1, w, MAX_PATH * 2);
        add(win, L"STATIC", L"Carpeta del juego", 0, M, y + 4, LW, 22, 0, font_head);
        rom_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", w, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   M + LW, y, CW - 2 * M - LW - 100, 28, win, (HMENU)(INT_PTR)400, GetModuleHandleW(0), 0);
        SendMessageW(rom_edit, WM_SETFONT, (WPARAM)font, TRUE);
        add(win, L"BUTTON", L"Buscar…", WS_TABSTOP, CW - M - 92, y, 92, 28, ID_ROM_BROWSE, font);
        y += row;
    }
    add(win, L"STATIC", L"Calidad gráfica", 0, M, y + 3, LW, 24, 0, font_head);
    preset_ctl = add(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, M + LW, y, CW - 2 * M - LW, 300,
                     ID_PRESET, font);
    for (int p = 0; p < N_PRESETS; p++) SendMessageW(preset_ctl, CB_ADDSTRING, 0, (LPARAM)presets[p].name);
    y += row + 14;
    for (int c = 0; c < N_COMBOS; c++) {
        add(win, L"STATIC", combos[c].label, 0, M, y + 4, LW, 22, 0, font);
        combos[c].ctl = add(win, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, M + LW, y, CW - 2 * M - LW,
                            300, 100 + c, font);
        for (int i = 0; combos[c].items[i]; i++) SendMessageW(combos[c].ctl, CB_ADDSTRING, 0, (LPARAM)combos[c].items[i]);
        y += row;
    }
    y += 6;
    add(win, L"STATIC", L"Efectos", 0, M, y, LW, 24, 0, font_head);
    y += 28;
    for (int c = 0; c < N_CHECKS; c++) {
        checks[c].ctl = add(win, L"BUTTON", checks[c].label, BS_AUTOCHECKBOX | WS_TABSTOP, M, y, CW - 2 * M, 24, 200 + c,
                            font);
        y += 28;
    }
    y += 16;
    add(win, L"STATIC",
        L"Durante el juego también se pueden cambiar: F1 pantallas, F2 filtro, F3 luz por píxel, "
        L"F4 subdivisión, F5 niebla, F6 sombras, F7 brillo, F8 resplandor, F9 gráfica/procesador, F11 pantalla completa.",
        0, M, y, CW - 2 * M, 58, 0, font_small);
    y += 58 + 20;
    add(win, L"BUTTON", L"Controles…", WS_TABSTOP, M, y, 140, 34, ID_CONTROLS, font);
    add(win, L"BUTTON", L"Salir", WS_TABSTOP, CW - M - 120 - 10 - 140, y, 120, 34, ID_QUIT, font);
    HWND play = add(win, L"BUTTON", L"Jugar", BS_DEFPUSHBUTTON | WS_TABSTOP, CW - M - 140, y, 140, 34, ID_PLAY, font);
    show_values();
    SendMessageW(preset_ctl, CB_SETCURSEL, matching_preset(), 0);

    if (getenv("NFS_MENU_SNAPSHOT")) { /* pruebas: fuera de la vista, captura y salir */
        SetWindowLongW(win, GWL_EXSTYLE, GetWindowLongW(win, GWL_EXSTYLE) | WS_EX_LAYERED); SetLayeredWindowAttributes(win, 0, 1, LWA_ALPHA);
        ShowWindow(win, SW_SHOWNOACTIVATE);
        snapshot(win, "menu");
        controls_window(win);
        DestroyWindow(win);
        return 0;
    }
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
    SetFocus(play);
    MSG m;
    while (GetMessageW(&m, 0, 0, 0) > 0) {
        if (IsDialogMessageW(win, &m)) continue; /* Tab, Intro = Jugar, Esc = Salir */
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (result == 1) save_and_apply();
    return result == 1;
}
