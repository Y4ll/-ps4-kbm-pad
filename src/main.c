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

#include "Detour.h"
#include "Utilities.h"

/* ------------------------------------------------- MANDO (sin header) */
/* El layout de KbmPadData cambia entre releases de OpenOrbis; aqui va el
 * de la rama master (el real de ScePadData). Importa el orden y el tamano. */
#define KBM_PAD_BUTTON_L3        0x0002
#define KBM_PAD_BUTTON_R3        0x0004
#define KBM_PAD_BUTTON_OPTIONS   0x0008
#define KBM_PAD_BUTTON_UP        0x0010
#define KBM_PAD_BUTTON_RIGHT     0x0020
#define KBM_PAD_BUTTON_DOWN      0x0040
#define KBM_PAD_BUTTON_LEFT      0x0080
#define KBM_PAD_BUTTON_L2        0x0100
#define KBM_PAD_BUTTON_R2        0x0200
#define KBM_PAD_BUTTON_L1        0x0400
#define KBM_PAD_BUTTON_R1        0x0800
#define KBM_PAD_BUTTON_TRIANGLE  0x1000
#define KBM_PAD_BUTTON_CIRCLE    0x2000
#define KBM_PAD_BUTTON_CROSS     0x4000
#define KBM_PAD_BUTTON_SQUARE    0x8000
#define KBM_PAD_BUTTON_TOUCH_PAD 0x100000

typedef struct { float x, y, z; } KbmVec3;
typedef struct { float x, y, z, w; } KbmVec4;
typedef struct { uint8_t x, y; } KbmStick;
typedef struct { uint8_t l2, r2; } KbmAnalog;
typedef struct { uint16_t x, y; uint8_t finger; uint8_t pad[3]; } KbmTouch;
typedef struct { uint8_t fingers; uint8_t pad1[3]; uint32_t pad2; KbmTouch touch[2]; } KbmTouchData;

typedef struct KbmPadData {
    uint32_t     buttons;
    KbmStick     leftStick;
    KbmStick     rightStick;
    KbmAnalog    analogButtons;
    uint16_t     padding;
    KbmVec4      orientation;
    KbmVec3      acceleration;
    KbmVec3      angularVelocity;
    KbmTouchData touch;
    uint8_t      connected;
    uint64_t     timestamp;
    uint8_t      ext[16];
    uint8_t      count;
    uint8_t      unknown[15];
} KbmPadData;

extern int32_t scePadRead(int32_t handle, KbmPadData *data, int32_t count);
extern int32_t scePadReadState(int32_t handle, KbmPadData *data);

/* ------------------------------------------------ TECLADO (sin header) */
/* Las releases de OpenOrbis no traen orbis/Keyboard.h; estos tipos son los
 * del header de la rama master. */
#define ORBIS_KEYBOARD_MOD_LEFT_CTRL   1
#define ORBIS_KEYBOARD_MOD_LEFT_SHIFT  2
#define ORBIS_KEYBOARD_MOD_RIGHT_CTRL  16
#define ORBIS_KEYBOARD_MOD_RIGHT_SHIFT 32
#define ORBIS_KEYBOARD_KEY_F8          65

typedef struct OrbisKeyboardData {
    uint32_t timestamp;
    uint8_t  padding[12];
    int32_t  unk1;
    int32_t  nkeys;
    uint32_t locks;
    uint32_t mods;
    uint16_t keycodes[32];
} OrbisKeyboardData;

/* Teclado y raton NO se enlazan en tiempo de compilacion: si el juego no tiene
 * libSceKeyboard/libSceMouse cargadas, el modulo entero fallaba al cargar y no
 * llegaba ni a module_start. Se resuelven en tiempo de ejecucion (resolve_input_libs). */
static int32_t (*p_kbInit)(void);
static int32_t (*p_kbOpen)(int32_t userID, int32_t type, int32_t index, void *param);
static int32_t (*p_kbRead)(int32_t handle, OrbisKeyboardData *data);

/* IDs de sysmodule (libSceKeyboard.sprx / libSceMouse.sprx) */
#define KBM_SYSMODULE_KEYBOARD 0x0106
#define KBM_SYSMODULE_MOUSE    0x00A9

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

static int32_t (*p_mInit)(void);
static int32_t (*p_mOpen)(int32_t userId, int32_t type, int32_t index, void *param);
static int32_t (*p_mRead)(int32_t handle, void *data, int32_t count);

#define MOUSE_BTN_LEFT  0x1
#define MOUSE_BTN_RIGHT 0x2

#define KBM_BUILD "build-dyn-2"   /* marca para saber que .prx esta cargado */

/* ------------------------------------------------- REGISTRO A ARCHIVO */
/* Sin klog ni PC: el plugin escribe /data/GoldHEN/kbm_pad.log (se abre por
 * FTP). Cada linea se abre/escribe/cierra para que no se pierda nada si el
 * juego se cuelga. Las etiquetas asm evitan chocar con las declaraciones del
 * header del toolchain. Flags FreeBSD: O_WRONLY=1, O_APPEND=8, O_CREAT=0x200. */
#include <stdarg.h>

#define KBM_LOG_PATH "/data/GoldHEN/kbm_pad.log"

extern int32_t kbm_sys_open(const char *path, int32_t flags, int32_t mode)
    __asm__("sceKernelOpen");
extern int64_t kbm_sys_write(int32_t fd, const void *buf, size_t n)
    __asm__("sceKernelWrite");
extern int32_t kbm_sys_close(int32_t fd)
    __asm__("sceKernelClose");

static void kbm_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    int32_t fd = kbm_sys_open(KBM_LOG_PATH, 0x0001 | 0x0008 | 0x0200, 0666);
    if (fd >= 0) {
        kbm_sys_write(fd, buf, (size_t)n);
        kbm_sys_close(fd);
    }
    printf("%s", buf);   /* y tambien al klog */
}

/* ------------------------------------------------- NOTIFICACIONES */
/* Aviso en pantalla (el toast de la PS4) via libkernel, sin librerias extra.
 * Layout copiado (verificado desensamblando plugin_template.prx de GoldHEN):
 * 11 enteros de cabecera, 1 byte useIconImageUri, y 3 bloques de 1024 bytes
 * (mensaje, URI del icono, sin uso) = 3120 bytes. El campo target_id DEBE ser
 * -1 (todos los usuarios); con 0 la PS4 descarta el aviso y no sale nada.
 * La etiqueta asm evita chocar con cualquier declaracion distinta que traiga
 * el header del toolchain. */
typedef struct KbmNotifyRequest {
    int32_t type;
    int32_t req_id;
    int32_t priority;
    int32_t msg_id;
    int32_t target_id;
    int32_t user_id;
    int32_t unk1;
    int32_t unk2;
    int32_t app_id;
    int32_t error_num;
    int32_t unk3;
    uint8_t use_icon_image_uri;
    char    message[1024];
    char    icon_image_uri[1024];
    char    unk[1024];
} KbmNotifyRequest;

_Static_assert(sizeof(KbmNotifyRequest) == 3120, "KbmNotifyRequest debe medir 3120");

extern int32_t kbm_send_notification(int32_t device, KbmNotifyRequest *req,
                                     size_t size, int32_t blocking)
    __asm__("sceKernelSendNotificationRequest");

static void notify(const char *msg)
{
    KbmNotifyRequest req;
    memset(&req, 0, sizeof(req));
    req.type = 0;
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    snprintf(req.message, sizeof(req.message), "%s", msg);
    snprintf(req.icon_image_uri, sizeof(req.icon_image_uri), "%s",
             "cxml://psnotification/tex_icon_system");
    int32_t r = kbm_send_notification(0, &req, sizeof(req), 0);
    kbm_log("[kbm_pad] aviso \"%s\" -> %d\n", msg, r);
}

/* ------------------------------------------------------------ ESTADO */
typedef int32_t (*scePadReadState_t)(int32_t, KbmPadData *);
typedef int32_t (*scePadRead_t)(int32_t, KbmPadData *, int32_t);

HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);

static bool    g_ready;
static bool    g_failed;
static bool    g_enabled = true;
static bool    g_toggle_down;
static int32_t g_kb = -1;
static int32_t g_mouse = -1;
static float   g_rx, g_ry;   /* inercia del stick derecho */

/* Carga un .sprx del sistema y obtiene su handle. Etiquetas asm para no chocar
 * con las declaraciones del header del toolchain. */
extern int32_t kbm_load_module(const char *path, size_t argc, const void *argv,
                               uint32_t flags, void *opt, int32_t *res)
    __asm__("sceKernelLoadStartModule");
extern int32_t kbm_dlsym(int32_t handle, const char *name, void **out)
    __asm__("sceKernelDlsym");

static int32_t load_lib(const char *path)
{
    int32_t res = 0;
    int32_t h = kbm_load_module(path, 0, NULL, 0, NULL, &res);
    kbm_log("[kbm_pad] cargar %s -> handle=0x%x res=%d\n", path, (unsigned)h, res);
    return h;
}

static void *find_sym(int32_t handle, const char *name)
{
    void *addr = NULL;
    int32_t r = kbm_dlsym(handle, name, &addr);
    kbm_log("[kbm_pad] dlsym %s -> %d addr=%p\n", name, r, addr);
    return r == 0 ? addr : NULL;
}

static bool resolve_input_libs(void)
{
    int32_t hk = load_lib("/system/common/lib/libSceKeyboard.sprx");
    if (hk > 0) {
        p_kbInit = (int32_t (*)(void))find_sym(hk, "sceKeyboardInit");
        p_kbOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))find_sym(hk, "sceKeyboardOpen");
        p_kbRead = (int32_t (*)(int32_t, OrbisKeyboardData *))find_sym(hk, "sceKeyboardReadState");
    }
    int32_t hm = load_lib("/system/common/lib/libSceMouse.sprx");
    if (hm > 0) {
        p_mInit = (int32_t (*)(void))find_sym(hm, "sceMouseInit");
        p_mOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))find_sym(hm, "sceMouseOpen");
        p_mRead = (int32_t (*)(int32_t, void *, int32_t))find_sym(hm, "sceMouseRead");
    }
    bool kb_ok = p_kbInit && p_kbOpen && p_kbRead;
    bool m_ok  = p_mInit && p_mOpen && p_mRead;
    kbm_log("[kbm_pad] funciones: teclado=%s raton=%s\n", kb_ok ? "OK" : "FALLO", m_ok ? "OK" : "FALLO");
    if (!kb_ok) p_kbRead = NULL;
    if (!m_ok)  p_mRead = NULL;
    return kb_ok || m_ok;
}

static void ensure_input(void)
{
    if (g_ready || g_failed)
        return;

    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);

    /* Los sysmodule IDs hacen que el sistema cargue las librerias; despues se
     * resuelven por ruta y dlsym. */
    sceSysmoduleLoadModule(KBM_SYSMODULE_KEYBOARD);
    sceSysmoduleLoadModule(KBM_SYSMODULE_MOUSE);

    if (!resolve_input_libs()) {
        kbm_log("[kbm_pad] no se pudo resolver teclado ni raton\n");
        notify("kbm_pad: no se pudo cargar teclado/raton");
        g_failed = true;
        return;
    }

    if (p_kbInit && p_kbOpen) {
        p_kbInit();
        g_kb = p_kbOpen(user, 0, 0, NULL);
    }
    if (p_mInit && p_mOpen) {
        p_mInit();
        g_mouse = p_mOpen(user, 0, 0, NULL);
    }

    kbm_log("[kbm_pad] user=%d keyboard=%d mouse=%d\n", user, g_kb, g_mouse);

    /* Se ejecuta en la primera llamada al hook: si este aviso sale, el hook
     * del mando SI se esta disparando. */
    char msg[96];
    snprintf(msg, sizeof(msg), "kbm_pad: teclado=%s, raton=%s",
             g_kb >= 0 ? "OK" : "FALLO", g_mouse >= 0 ? "OK" : "FALLO");
    notify(msg);

    if (g_kb < 0 && g_mouse < 0) {
        kbm_log("[kbm_pad] no se abrio ni teclado ni raton\n");
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

/* Mezcla teclado + raton en un KbmPadData. */
static void apply_kbm(KbmPadData *d)
{
    OrbisKeyboardData kd;
    memset(&kd, 0, sizeof(kd));
    if (g_kb >= 0 && p_kbRead && p_kbRead(g_kb, &kd) < 0)
        memset(&kd, 0, sizeof(kd));

    /* Toggle con F8 (flanco de subida) */
    bool t = has_key(&kd, TOGGLE_KEYCODE);
    if (t && !g_toggle_down) {
        g_enabled = !g_enabled;
        kbm_log("[kbm_pad] %s\n", g_enabled ? "ACTIVO" : "PAUSA");
        notify(g_enabled ? "kbm_pad ACTIVO" : "kbm_pad PAUSA");
    }
    g_toggle_down = t;

    /* Raton: acumular deltas aunque este en pausa, para no acumular salto */
    int32_t dx = 0, dy = 0;
    uint32_t mbtn = 0;
    if (g_mouse >= 0 && p_mRead) {
        for (int i = 0; i < 8; i++) {
            KbmMouseBuf mb;
            memset(&mb, 0, sizeof(mb));
            int32_t n = p_mRead(g_mouse, &mb, 1);
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
    if (mbtn & MOUSE_BTN_LEFT)  { d->buttons |= KBM_PAD_BUTTON_R2; d->analogButtons.r2 = 255; }
    if (mbtn & MOUSE_BTN_RIGHT) { d->buttons |= KBM_PAD_BUTTON_L2; d->analogButtons.l2 = 255; }

    /* Botones */
    if (has_key(&kd, K_SPACE)) d->buttons |= KBM_PAD_BUTTON_CROSS;
    if (has_key(&kd, K_E))     d->buttons |= KBM_PAD_BUTTON_CIRCLE;
    if (has_key(&kd, K_Q))     d->buttons |= KBM_PAD_BUTTON_SQUARE;
    if (has_key(&kd, K_R))     d->buttons |= KBM_PAD_BUTTON_TRIANGLE;
    if (has_key(&kd, K_F))     d->buttons |= KBM_PAD_BUTTON_L1;
    if (has_key(&kd, K_G))     d->buttons |= KBM_PAD_BUTTON_R1;
    if (has_key(&kd, K_ENTER)) d->buttons |= KBM_PAD_BUTTON_OPTIONS;
    if (has_key(&kd, K_TAB))   d->buttons |= KBM_PAD_BUTTON_TOUCH_PAD;
    if (kd.mods & (ORBIS_KEYBOARD_MOD_LEFT_SHIFT | ORBIS_KEYBOARD_MOD_RIGHT_SHIFT))
        d->buttons |= KBM_PAD_BUTTON_L3;
    if (kd.mods & (ORBIS_KEYBOARD_MOD_LEFT_CTRL | ORBIS_KEYBOARD_MOD_RIGHT_CTRL))
        d->buttons |= KBM_PAD_BUTTON_R3;

    /* D-pad: flechas */
    if (has_key(&kd, K_UP))    d->buttons |= KBM_PAD_BUTTON_UP;
    if (has_key(&kd, K_DOWN))  d->buttons |= KBM_PAD_BUTTON_DOWN;
    if (has_key(&kd, K_LEFT))  d->buttons |= KBM_PAD_BUTTON_LEFT;
    if (has_key(&kd, K_RIGHT)) d->buttons |= KBM_PAD_BUTTON_RIGHT;
}

/* Rellena un KbmPadData neutro cuando no hay mando fisico. */
static void fake_pad(KbmPadData *d)
{
    memset(d, 0, sizeof(*d));
    d->leftStick.x = d->leftStick.y = 128;
    d->rightStick.x = d->rightStick.y = 128;
    d->orientation.w = 1.0f;
    d->connected = 1;
    d->count = 1;
}

/* ------------------------------------------------------------- HOOKS */
static int32_t scePadReadState_hook(int32_t handle, KbmPadData *data)
{
    static bool seen;
    if (!seen) { seen = true; kbm_log("[kbm_pad] hook scePadReadState: 1a llamada\n"); }
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

static int32_t scePadRead_hook(int32_t handle, KbmPadData *data, int32_t count)
{
    static bool seen;
    if (!seen) { seen = true; kbm_log("[kbm_pad] hook scePadRead: 1a llamada\n"); }
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

/* ---------------------------------------------- INICIO / FIN (idempotentes) */
static bool g_hooked;

static void kbm_start(void)
{
    if (g_hooked)
        return;
    g_hooked = true;
    kbm_log("[kbm_pad] cargado (%s)\n", KBM_BUILD);
    notify("kbm_pad cargado");
    kbm_log("[kbm_pad] enganchando scePadReadState...\n");
    HOOK(scePadReadState);
    kbm_log("[kbm_pad] enganchando scePadRead...\n");
    HOOK(scePadRead);
    kbm_log("[kbm_pad] enganches listos\n");
}

static void kbm_stop(void)
{
    if (!g_hooked)
        return;
    g_hooked = false;
    UNHOOK(scePadReadState);
    UNHOOK(scePadRead);
    kbm_log("[kbm_pad] descargado\n");
}

/* ------------------------------------------------------ ENTRY POINTS */
/* El plugin_loader.prx de GoldHEN carga el modulo, busca con sceKernelDlsym
 * g_pluginName / plugin_load / plugin_unload y, si faltan plugin_load o
 * plugin_unload, NO da el plugin por cargado. Por eso se exportan con
 * visibilidad por defecto (no static). */
__attribute__((visibility("default"))) const char *g_pluginName = "kbm_pad";

__attribute__((visibility("default")))
int plugin_load(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    kbm_log("[kbm_pad] plugin_load llamado (%s)\n", KBM_BUILD);
    kbm_start();
    return 0;   /* 0 = cargado; otro valor hace que el loader llame a plugin_unload */
}

__attribute__((visibility("default")))
int plugin_unload(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    kbm_stop();
    return 0;
}

__attribute__((visibility("default")))
int module_start(size_t argc, const void *args)
{
    (void)argc; (void)args;
    kbm_log("[kbm_pad] module_start llamado (%s)\n", KBM_BUILD);
    kbm_start();
    return 0;
}

__attribute__((visibility("default")))
int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    kbm_stop();
    return 0;
}
