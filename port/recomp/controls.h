/* Controles configurables: los 12 botones de la consola, su tecla y su control del mando (XInput).
 * Lo usan el menu (menu.c, ventana "Controles", guarda en opciones.ini como NFS_KEY_<boton> y NFS_PAD_<boton>)
 * y la ventana del juego (win.c, que lee esas variables). */
#ifndef CONTROLS_H
#define CONTROLS_H
#include <windows.h>

/* Controles del mando como bits: los botones de XInput tal cual (16 bits bajos) y, encima, gatillos y palanca */
#define PAD_LT 0x10000u
#define PAD_RT 0x20000u
#define PAD_LS_RIGHT 0x40000u
#define PAD_LS_LEFT 0x80000u
#define PAD_LS_UP 0x100000u
#define PAD_LS_DOWN 0x200000u

/* Botones de la consola, en el orden de los bits de KEYINPUT; los dos ultimos (X, Y) los lee el ARM7 */
enum { DS_A, DS_B, DS_SELECT, DS_START, DS_RIGHT, DS_LEFT, DS_UP, DS_DOWN, DS_R, DS_L, DS_X, DS_Y, DS_BUTTONS };

typedef struct ControlDef {
    const char *env;      /* sufijo: NFS_KEY_<env>, NFS_PAD_<env> */
    const wchar_t *label; /* lo que se ve en el menu */
    int key;              /* tecla por defecto (codigo virtual de Windows) */
    unsigned pad;         /* controles del mando por defecto */
} ControlDef;

static const ControlDef control_defs[DS_BUTTONS] = {
    {"A", L"A (acelerar / aceptar)", 'Z', 0x1000u | PAD_RT},
    {"B", L"B (frenar / atrás)", 'X', 0x2000u | PAD_LT},
    {"SELECT", L"Select", VK_BACK, 0x0020u},
    {"START", L"Start (pausa)", VK_RETURN, 0x0010u},
    {"RIGHT", L"Derecha", VK_RIGHT, 0x0008u | PAD_LS_RIGHT},
    {"LEFT", L"Izquierda", VK_LEFT, 0x0004u | PAD_LS_LEFT},
    {"UP", L"Arriba", VK_UP, 0x0001u | PAD_LS_UP},
    {"DOWN", L"Abajo", VK_DOWN, 0x0002u | PAD_LS_DOWN},
    {"R", L"R (nitro)", 'W', 0x0200u},
    {"L", L"L (freno de mano)", 'Q', 0x0100u},
    {"X", L"X (salto / giro)", 'S', 0x8000u},
    {"Y", L"Y", 'A', 0x4000u},
};

/* Estado del mando (el primero conectado) como bits PAD_*; 0 si no hay. La DLL se carga al vuelo. */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XPadGamepad;
typedef struct { DWORD packet; XPadGamepad pad; } XPadState;
static unsigned pad_read_raw(int *connected) {
    static DWORD (WINAPI *get_state)(DWORD, XPadState *);
    static int tried;
    static DWORD last_fail;
    static int any = -1;
    if (!tried) {
        tried = 1;
        HMODULE dll = LoadLibraryA("xinput1_4.dll");
        if (!dll) dll = LoadLibraryA("xinput9_1_0.dll");
        if (dll) get_state = (DWORD (WINAPI *)(DWORD, XPadState *))(void *)GetProcAddress(dll, "XInputGetState");
    }
    if (connected) *connected = 0;
    if (!get_state) return 0;
    if (any == 0 && GetTickCount() - last_fail < 2000) return 0; /* sin mando: buscarlo cada 2 s (es lento) */
    XPadState st;
    for (DWORD i = 0; i < 4; i++) {
        if (get_state(i, &st) != 0) continue;
        any = 1;
        if (connected) *connected = 1;
        const XPadGamepad *g = &st.pad;
        unsigned b = g->buttons;
        if (g->lt > 60) b |= PAD_LT;
        if (g->rt > 60) b |= PAD_RT;
        if (g->lx > 12000) b |= PAD_LS_RIGHT;
        if (g->lx < -12000) b |= PAD_LS_LEFT;
        if (g->ly > 12000) b |= PAD_LS_UP;
        if (g->ly < -12000) b |= PAD_LS_DOWN;
        return b;
    }
    any = 0;
    last_fail = GetTickCount();
    return 0;
}

/* Nombre de un control del mando (uno solo) */
static const wchar_t *pad_name(unsigned bit) {
    static const struct { unsigned bit; const wchar_t *name; } n[] = {
        {0x0001, L"Cruceta arriba"}, {0x0002, L"Cruceta abajo"}, {0x0004, L"Cruceta izquierda"}, {0x0008, L"Cruceta derecha"},
        {0x0010, L"Start"}, {0x0020, L"Back"}, {0x0040, L"Palanca izq. (pulsar)"}, {0x0080, L"Palanca der. (pulsar)"},
        {0x0100, L"LB"}, {0x0200, L"RB"}, {0x1000, L"A"}, {0x2000, L"B"}, {0x4000, L"X"}, {0x8000, L"Y"},
        {PAD_LT, L"Gatillo izquierdo (LT)"}, {PAD_RT, L"Gatillo derecho (RT)"}, {PAD_LS_RIGHT, L"Palanca derecha"},
        {PAD_LS_LEFT, L"Palanca izquierda"}, {PAD_LS_UP, L"Palanca arriba"}, {PAD_LS_DOWN, L"Palanca abajo"},
    };
    for (size_t i = 0; i < sizeof n / sizeof n[0]; i++)
        if (n[i].bit == bit) return n[i].name;
    return L"?";
}

#endif
