/*
 * kbm_pad - GoldHEN plugin: teclado + raton USB como un DualShock 4
 *
 * Engancha scePadRead / scePadReadState del juego y mezcla en los datos
 * del mando las entradas del teclado y del raton.
 *
 * ESTADO: sin probar en consola. Las estructuras del raton no estan
 * documentadas en OpenOrbis (ver KbmMouse abajo): verifica en tu PS4.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>
#include <orbis/Pad.h>
#include <orbis/Keyboard.h>

#include "Detour.h"
#include "Utilities.h"

/* ------------------------------------------------------------ CONFIG */
#define MOUSE_SENS      6.0f    /* sube = stick derecho mas sensible   */
#define MOUSE_DECAY     0.55f   /* 0 = sin inercia, 0.9 = mucha inercia */
#define INVERT_MOUSE_Y  0       /* 1 = invertir eje Y del raton         */
#define TOGGLE_KEYCODE  ORBIS_KEYBOARD_KEY_F8   /* activar / pausar     */

/* Codigos USB HID (los mismos que usa OrbisKeyboardData.keycodes) */
#define K_A 4
#define K_D 7
#define K_E 8
#define K_F 9
#define K_G 10
#define K_Q 20
#define K_R 21
#define K_S 22
#define K_W 26
#define K_ENTER 40
#define K_TAB   43
#define K_SPACE 44
#define K_RIGHT 79
#define K_LEFT  80
#define K_DOWN  81
#define K_UP    82

/* ------------------------------------------------- RATON (sin header) */
/* libSceMouse no esta documentada en OpenOrbis. Layout supuesto; el buffer
 * es grande a proposito para que una estructura real mas grande no
 * desborde la memoria. */
typedef struct KbmMouse {
    uint64_t timestamp;
    uint8_t  connected;
    uint8_t  pad0[3];
    uint32_t buttons;   /* bit0 = izq, bit1 = der, bit2 = medio */
    int32_t  xAxis;
    int32_t  yAxis;
    int32_t  wheel;
    int32_t  tilt;
    uint8_t  reserve[8];
} KbmMouse;

typedef union { KbmMouse m; uint8_t raw[128]; } KbmMouseBuf;

extern int32_t sceMouseInit(void);
extern int32_t sceMouseOpen(int32_t userId, int32_t type, int32_t index, void *param);
extern int32_t sceMouseRead(int32_t handle, void *data, int32_t count);

#define MOUSE_BTN_LEFT  0x1
#define MOUSE_BTN_RIGHT 0x2

/* ------------------------------------------------------------ ESTADO */
typedef int32_t (*scePadReadState_t)(int32_t, OrbisPadData *);
typedef int32_t (*scePadRead_t)(int32_t, OrbisPadData *, int32_t);

HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);

static bool    g_ready;
static bool    g_failed;
static bool    g_enabled = true;
static bool    g_toggle_down;
static int32_t g_kb = -1;
static int32_t g_mouse = -1;
static float   g_rx, g_ry;   /* inercia del stick derecho */

static void ensure_input(void)
{
    if (g_ready || g_failed)
        return;

    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    sceSysmoduleLoadModule(ORBIS_SYSMODULE_KEYBOARD);
    sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);

    sceKeyboardInit();
    g_kb = sceKeyboardOpen(user, 0, 0, NULL);

    sceMouseInit();
    g_mouse = sceMouseOpen(user, 0, 0, NULL);

    klog("[kbm_pad] user=%d keyboard=%d mouse=%d\n", user, g_kb, g_mouse);

    if (g_kb < 0 && g_mouse < 0) {
        klog("[kbm_pad] no se abrio ni teclado ni raton\n");
        g_failed = true;
        return;
    }
    g_ready = true;
}

static bool has_key(const OrbisKeyboardData *kd, uint16_t code)
{
    for (int i = 0; i < kd->nkeys && i < 32; i++)
        if (kd->keycodes[i] == code)
            return true;
    return false;
}

static uint8_t to_axis(float v)   /* -1..1 -> 0..255 */
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (uint8_t)(128.0f + v * 127.0f);
}

/* Mezcla teclado + raton en un OrbisPadData. */
static void apply_kbm(OrbisPadData *d)
{
    OrbisKeyboardData kd;
    memset(&kd, 0, sizeof(kd));
    if (g_kb >= 0 && sceKeyboardReadState(g_kb, &kd) < 0)
        memset(&kd, 0, sizeof(kd));

    /* Toggle con F8 (flanco de subida) */
    bool t = has_key(&kd, TOGGLE_KEYCODE);
    if (t && !g_toggle_down) {
        g_enabled = !g_enabled;
        klog("[kbm_pad] %s\n", g_enabled ? "ACTIVO" : "PAUSA");
    }
    g_toggle_down = t;

    /* Raton: acumular deltas aunque este en pausa, para no acumular salto */
    int32_t dx = 0, dy = 0;
    uint32_t mbtn = 0;
    if (g_mouse >= 0) {
        for (int i = 0; i < 8; i++) {
            KbmMouseBuf mb;
            memset(&mb, 0, sizeof(mb));
            int32_t n = sceMouseRead(g_mouse, &mb, 1);
            if (n <= 0)
                break;
            dx += mb.m.xAxis;
            dy += mb.m.yAxis;
            mbtn = mb.m.buttons;
        }
    }

    if (!g_enabled)
        return;

    /* Stick izquierdo: WASD */
    float lx = (has_key(&kd, K_D) ? 1.0f : 0.0f) - (has_key(&kd, K_A) ? 1.0f : 0.0f);
    float ly = (has_key(&kd, K_S) ? 1.0f : 0.0f) - (has_key(&kd, K_W) ? 1.0f : 0.0f);
    if (lx != 0.0f || ly != 0.0f) {
        if (lx != 0.0f && ly != 0.0f) {   /* diagonal: no pasar de 1.0 */
            lx *= 0.7071f;
            ly *= 0.7071f;
        }
        d->leftStick.x = to_axis(lx);
        d->leftStick.y = to_axis(ly);
    }

    /* Stick derecho: movimiento del raton con algo de inercia */
#if INVERT_MOUSE_Y
    dy = -dy;
#endif
    g_rx = g_rx * MOUSE_DECAY + ((float)dx / 127.0f) * MOUSE_SENS * (1.0f - MOUSE_DECAY);
    g_ry = g_ry * MOUSE_DECAY + ((float)dy / 127.0f) * MOUSE_SENS * (1.0f - MOUSE_DECAY);
    if (g_rx != 0.0f || g_ry != 0.0f || dx || dy) {
        d->rightStick.x = to_axis(g_rx);
        d->rightStick.y = to_axis(g_ry);
    }

    /* Gatillos: clic izq = R2, clic der = L2 */
    if (mbtn & MOUSE_BTN_LEFT)  { d->buttons |= ORBIS_PAD_BUTTON_R2; d->analogButtons.r2 = 255; }
    if (mbtn & MOUSE_BTN_RIGHT) { d->buttons |= ORBIS_PAD_BUTTON_L2; d->analogButtons.l2 = 255; }

    /* Botones */
    if (has_key(&kd, K_SPACE)) d->buttons |= ORBIS_PAD_BUTTON_CROSS;
    if (has_key(&kd, K_E))     d->buttons |= ORBIS_PAD_BUTTON_CIRCLE;
    if (has_key(&kd, K_Q))     d->buttons |= ORBIS_PAD_BUTTON_SQUARE;
    if (has_key(&kd, K_R))     d->buttons |= ORBIS_PAD_BUTTON_TRIANGLE;
    if (has_key(&kd, K_F))     d->buttons |= ORBIS_PAD_BUTTON_L1;
    if (has_key(&kd, K_G))     d->buttons |= ORBIS_PAD_BUTTON_R1;
    if (has_key(&kd, K_ENTER)) d->buttons |= ORBIS_PAD_BUTTON_OPTIONS;
    if (has_key(&kd, K_TAB))   d->buttons |= ORBIS_PAD_BUTTON_TOUCH_PAD;
    if (kd.mods & (ORBIS_KEYBOARD_MOD_LEFT_SHIFT | ORBIS_KEYBOARD_MOD_RIGHT_SHIFT))
        d->buttons |= ORBIS_PAD_BUTTON_L3;
    if (kd.mods & (ORBIS_KEYBOARD_MOD_LEFT_CTRL | ORBIS_KEYBOARD_MOD_RIGHT_CTRL))
        d->buttons |= ORBIS_PAD_BUTTON_R3;

    /* D-pad: flechas */
    if (has_key(&kd, K_UP))    d->buttons |= ORBIS_PAD_BUTTON_UP;
    if (has_key(&kd, K_DOWN))  d->buttons |= ORBIS_PAD_BUTTON_DOWN;
    if (has_key(&kd, K_LEFT))  d->buttons |= ORBIS_PAD_BUTTON_LEFT;
    if (has_key(&kd, K_RIGHT)) d->buttons |= ORBIS_PAD_BUTTON_RIGHT;
}

/* Rellena un OrbisPadData neutro cuando no hay mando fisico. */
static void fake_pad(OrbisPadData *d)
{
    memset(d, 0, sizeof(*d));
    d->leftStick.x = d->leftStick.y = 128;
    d->rightStick.x = d->rightStick.y = 128;
    d->orientation.w = 1.0f;
    d->connected = 1;
    d->count = 1;
}

/* ------------------------------------------------------------- HOOKS */
static int32_t scePadReadState_hook(int32_t handle, OrbisPadData *data)
{
    int32_t ret = HOOK_CONTINUE(scePadReadState, scePadReadState_t, handle, data);
    ensure_input();
    if (!g_ready || data == NULL)
        return ret;

    if (ret != 0) {          /* no hay mando real: inventamos uno */
        fake_pad(data);
        ret = 0;
    }
    apply_kbm(data);
    return ret;
}

static int32_t scePadRead_hook(int32_t handle, OrbisPadData *data, int32_t count)
{
    int32_t ret = HOOK_CONTINUE(scePadRead, scePadRead_t, handle, data, count);
    ensure_input();
    if (!g_ready || data == NULL || count < 1)
        return ret;

    if (ret <= 0) {          /* no hay mando real */
        fake_pad(&data[0]);
        ret = 1;
    }
    if (ret > count)
        ret = count;
    apply_kbm(&data[ret - 1]);   /* la muestra mas reciente */
    return ret;
}

/* ------------------------------------------------------ ENTRY POINTS */
int module_start(size_t argc, const void *args)
{
    (void)argc; (void)args;
    klog("[kbm_pad] cargado\n");
    HOOK(scePadReadState);
    HOOK(scePadRead);
    return 0;
}

int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    UNHOOK(scePadReadState);
    UNHOOK(scePadRead);
    klog("[kbm_pad] descargado\n");
    return 0;
}
