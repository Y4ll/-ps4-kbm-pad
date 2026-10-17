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
#include "HDE64.h"
#include "Utilities.h"
#include "Patcher.h"
#include "Syscall.h"

/* Los macros HOOK() del SDK llaman a klog (= printf de libc, variadica). El README
 * del SDK avisa de que el registro con va_args cuelga los plugins, asi que aqui
 * se anula: ningun codigo de este plugin usa funciones variadicas. */
#undef klog
#define klog(...) ((void)0)

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
/* Variantes "Ext" de libScePad. Como gamepad_helper (oficial), los hooks NO usan el
 * trampolin del Detour (HOOK_CONTINUE): llaman a scePadReadExt / scePadReadStateExt,
 * que tienen el mismo efecto que el original y evitan copiar el prologo de
 * scePadRead (instrucciones relativas copiadas a otro sitio = cierre del juego). */
extern int32_t scePadReadExt(int32_t handle, KbmPadData *data, int32_t count);
extern int32_t scePadReadStateExt(int32_t handle, KbmPadData *data);

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
#define MOUSE_DECAY     0.40f   /* 0 = sin inercia, 0.9 = mucha inercia */
#define MOUSE_DEADZONE  0.15f   /* zona muerta del stick del juego que se salta el raton */
#define INVERT_MOUSE_Y  0       /* 1 = invertir eje Y del raton         */
#define TOGGLE_KEYCODE  ORBIS_KEYBOARD_KEY_F8   /* activar / pausar     */
#define AIM_TOGGLE      0       /* 1 = clic derecho alterna la mira (L2); 0 = mantener */

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

#define KBM_BUILD "build-cfg-5"   /* marca para saber que .prx esta cargado */

/* Etapas para diagnosticar (se eligen con -DKBM_STAGE=n desde el Makefile):
 *   1  solo aviso y log (sin enganches ni entrada)
 *   2  etapa 1 + enganches de scePadRead/scePadReadState que solo llaman al original
 *   3  completo (teclado y raton)  <- kbm_pad.prx */
#ifndef KBM_STAGE
#define KBM_STAGE 3
#endif
#ifndef KBM_NAME
#define KBM_NAME "kbm_pad"
#endif

/* ------------------------------------------------- REGISTRO A ARCHIVO */
/* Sin klog ni PC: el plugin escribe /data/GoldHEN/kbm_pad.log (se abre por
 * FTP). Cada linea se abre/escribe/cierra para que no se pierda nada si el
 * juego se cuelga. Las etiquetas asm evitan chocar con las declaraciones del
 * header del toolchain. Flags FreeBSD: O_WRONLY=1, O_APPEND=8, O_CREAT=0x200. */
#define KBM_LOG_PATH "/data/GoldHEN/kbm_pad.log"

/* Ancla de .data.rel.ro. Hallazgo (comparando los .prx por dentro): todos los
 * plugins que cargan en la PS4 tienen un segmento PT_SCE_RELRO (0x61000010) y
 * todos los que no daban senales no lo tenian. Los probes lo tienen porque
 * llevan un arreglo const de punteros (PATHS[], que con -fPIC cae en
 * .data.rel.ro); main.c no tenia ningun dato asi. Esta tabla fuerza ese
 * segmento. Debe usarse (kbm_start) o el compilador la descarta. */
static const char *const kbm_relro_anchor[] = {
    KBM_BUILD,
    KBM_LOG_PATH,
    KBM_NAME,
};

extern int32_t kbm_sys_open(const char *path, int32_t flags, int32_t mode)
    __asm__("sceKernelOpen");
extern int64_t kbm_sys_write(int32_t fd, const void *buf, size_t n)
    __asm__("sceKernelWrite");
extern int32_t kbm_sys_close(int32_t fd)
    __asm__("sceKernelClose");

/* Formateo y copia SIN funciones de libc. El plugin de prueba (que no usa libc)
 * si mostraba avisos y escribia logs dentro del juego; kbm_pad, que usaba
 * snprintf/vsnprintf/memset, no. Por eso todo lo que se muestra o se registra
 * se arma aqui con codigo propio. Soporta %s %d %u %x %p %%. */
static void kbm_zero(void *p, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    for (size_t i = 0; i < n; i++)
        b[i] = 0;
}

static void kbm_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

/* Constructor de linea SIN funciones variadicas: se arma con lb_s / lb_d / lb_x
 * y se escribe con lb_flush. */
static char   g_lb[512];
static size_t g_lbn;

static void lb_c(char c)
{
    if (g_lbn + 1 < sizeof(g_lb))
        g_lb[g_lbn++] = c;
}

static void lb_s(const char *s)
{
    if (!s)
        s = "(null)";
    while (*s)
        lb_c(*s++);
}

static void lb_u(uint64_t v, unsigned base)
{
    char tmp[24];
    int n = 0;
    if (v == 0)
        tmp[n++] = '0';
    while (v) {
        unsigned d = (unsigned)(v % base);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v /= base;
    }
    while (n)
        lb_c(tmp[--n]);
}

static void lb_d(int32_t v)
{
    if (v < 0) {
        lb_c('-');
        lb_u((uint64_t)(-(int64_t)v), 10);
    } else {
        lb_u((uint64_t)v, 10);
    }
}

static void lb_x(uint64_t v)
{
    lb_c('0');
    lb_c('x');
    lb_u(v, 16);
}

static void lb_flush(void)
{
    size_t n = g_lbn;
    g_lbn = 0;
    if (n == 0)
        return;
    g_lb[n] = 0;
    static const char *const paths[] = {
        KBM_LOG_PATH, "/data/kbm_pad.log", "/user/data/kbm_pad.log",
    };
    for (size_t k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
        int32_t fd = kbm_sys_open(paths[k], 0x0001 | 0x0008 | 0x0200, 0666);
        if (fd >= 0) {
            kbm_sys_write(fd, g_lb, n);
            kbm_sys_close(fd);
        }
    }
}

/* Una sola cadena, una linea. */
static void L(const char *a)
{
    lb_s(a);
    lb_flush();
}

/* Agrega src al final de dst (para armar el texto de los avisos). */
static void kbm_cat(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (dst[i] && i + 1 < cap)
        i++;
    size_t j = 0;
    while (src[j] && i + 1 < cap)
        dst[i++] = src[j++];
    dst[i] = 0;
}

static void kbm_cat_u(char *dst, size_t cap, uint32_t v)
{
    char t[12], o[12];
    int n = 0, k = 0;
    if (v == 0)
        t[n++] = '0';
    while (v) {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n)
        o[k++] = t[--n];
    o[k] = 0;
    kbm_cat(dst, cap, o);
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

static int32_t g_notify_rc = 0x7FFFFFFF;   /* resultado del ultimo aviso (se publica en kbm_pad.status) */

static void notify(const char *msg)
{
    KbmNotifyRequest req;
    kbm_zero(&req, sizeof(req));
    req.type = 0;
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    kbm_copy(req.message, sizeof(req.message), msg);
    kbm_copy(req.icon_image_uri, sizeof(req.icon_image_uri),
             "cxml://psnotification/tex_icon_system");
    int32_t r = kbm_send_notification(0, &req, sizeof(req), 0);
    g_notify_rc = r;
    lb_s("[kbm_pad] aviso \""); lb_s(msg); lb_s("\" -> "); lb_d(r); lb_s("\n"); lb_flush();
}

/* Mensajes de seguimiento: solo al log, SIN aviso en pantalla (solo se muestran 2 avisos:
 * el de arranque y el de actualizacion). */
static void note(const char *msg)
{
    lb_s("[kbm_pad] "); lb_s(msg); lb_s("\n"); lb_flush();
}

#if KBM_STAGE >= 2
/* --------------------------------------------- ENGANCHE DEL MANDO (comun) */
HOOK_INIT(scePadReadState);
HOOK_INIT(scePadRead);

static int32_t scePadReadState_hook(int32_t handle, KbmPadData *data);
static int32_t scePadRead_hook(int32_t handle, KbmPadData *data, int32_t count);

extern const char *kbm_sandbox_word(void) __asm__("sceKernelGetFsSandboxRandomWord");

static Patcher g_patchReadExt;
static Patcher g_patchReadStateExt;
static bool    g_hooks_on;

/* "/<palabra del sandbox>/common/lib/<name>", igual que gamepad_helper. Sin snprintf. */
static void sys_lib_path(char *dst, size_t cap, const char *name)
{
    const char *w = kbm_sandbox_word();
    dst[0] = 0;
    kbm_cat(dst, cap, "/");
    kbm_cat(dst, cap, w ? w : "system");
    kbm_cat(dst, cap, "/common/lib/");
    kbm_cat(dst, cap, name);
}

/* Agrega n bytes en hexadecimal ("b9 01 00") al final de dst. */
static void kbm_hex(char *dst, size_t cap, const uint8_t *p, int n)
{
    static const char hx[] = "0123456789abcdef";
    size_t i = 0;
    while (dst[i] && i + 1 < cap)
        i++;
    for (int k = 0; k < n && i + 4 < cap; k++) {
        dst[i++] = hx[p[k] >> 4];
        dst[i++] = hx[p[k] & 15];
        if (k + 1 < n)
            dst[i++] = ' ';
    }
    dst[i] = 0;
}

/* Copia fiel de gamepad_helper: cargar libScePad, parchear el inicio de
 * scePadReadExt / scePadReadStateExt y enganchar scePadRead / scePadReadState (HOOK32).
 * Antes de tocar nada se comprueban los primeros bytes y se muestran en pantalla:
 * si no son los esperados (mov ecx/edx, imm32) NO se parchea ni se engancha, para
 * que el juego no se cierre y se vea por que. */
static int install_pad_hooks(void)
{
    char path[128];
    int  id = 0;
    sys_lib_path(path, sizeof(path), "libScePad.sprx");
    int r = sys_dynlib_load_prx(path, &id);
    lb_s("[kbm_pad] libScePad "); lb_s(path); lb_s(" -> "); lb_d(r);
    lb_s(" id="); lb_d(id); lb_s("\n"); lb_flush();

    const uint8_t *rd   = (const uint8_t *)(uintptr_t)scePadRead;
    const uint8_t *rdx  = (const uint8_t *)(uintptr_t)scePadReadExt;
    const uint8_t *st   = (const uint8_t *)(uintptr_t)scePadReadState;
    const uint8_t *stx  = (const uint8_t *)(uintptr_t)scePadReadStateExt;

    char msg[160];
    msg[0] = 0;
    kbm_cat(msg, sizeof(msg), KBM_NAME ": Read ");
    kbm_hex(msg, sizeof(msg), rd, 6);
    kbm_cat(msg, sizeof(msg), " | Ext ");
    kbm_hex(msg, sizeof(msg), rdx, 6);
    note(msg);
    msg[0] = 0;
    kbm_cat(msg, sizeof(msg), KBM_NAME ": State ");
    kbm_hex(msg, sizeof(msg), st, 6);
    kbm_cat(msg, sizeof(msg), " | Ext ");
    kbm_hex(msg, sizeof(msg), stx, 6);
    note(msg);

    if (rd[0] == 0xE9 || st[0] == 0xE9) {
        /* HOOK32 seguiria ese salto y engancharia scePadReadExt: recursion infinita */
        note(KBM_NAME ": scePadRead empieza con jmp, no se engancha");
        return 1;
    }
    if (rdx[0] != 0xB9 || stx[0] != 0xBA) {
        note(KBM_NAME ": scePadReadExt distinto al esperado, no se engancha");
        return 2;
    }

    Patcher_Construct(&g_patchReadExt);
    Patcher_Construct(&g_patchReadStateExt);
    static const uint8_t xor_ecx_ecx[5] = {0x31, 0xC9, 0x90, 0x90, 0x90};
    static const uint8_t xor_edx_edx[5] = {0x31, 0xD2, 0x90, 0x90, 0x90};
    Patcher_Install_Patch(&g_patchReadExt, (uint64_t)(uintptr_t)scePadReadExt,
                          xor_ecx_ecx, sizeof(xor_ecx_ecx));
    Patcher_Install_Patch(&g_patchReadStateExt, (uint64_t)(uintptr_t)scePadReadStateExt,
                          xor_edx_edx, sizeof(xor_edx_edx));
    L("[kbm_pad] parches Ext instalados\n");

    HOOK32(scePadRead);
    HOOK32(scePadReadState);
    L("[kbm_pad] HOOK32 instalados\n");
    g_hooks_on = true;
    return 0;
}

static void remove_pad_hooks(void)
{
    if (!g_hooks_on)
        return;
    g_hooks_on = false;
    UNHOOK(scePadReadState);
    UNHOOK(scePadRead);
    Patcher_Destroy(&g_patchReadExt);
    Patcher_Destroy(&g_patchReadStateExt);
}
#endif /* KBM_STAGE >= 2 */

#if KBM_STAGE >= 3
/* ------------------------------------------------------------ ESTADO */

static bool    g_ready;
static bool    g_failed;
static bool    g_enabled = true;
static int32_t g_kb = -1;
static int32_t g_mouse = -1;
static int32_t g_mouse2 = -1;           /* segundo handle del raton (otro indice USB), -1 si no hay */
static uint8_t g_open_zero[8];          /* parametros de apertura: 8 bytes en cero, como en las apps que si lo abren */
static bool     g_hide = true;         /* ini: ocultar=1 -> el juego no ve el teclado ni el raton */
static bool     g_hide_done;
static bool     g_libs_early;           /* las librerias ya se resolvieron y ocultaron en plugin_load */
static int      g_hide_state;           /* 0 = apagado, 1 = activo, 2 = parcial o no se pudo */
static uint32_t g_dbg_mmove, g_dbg_stick;   /* diagnostico: muestras con movimiento / veces que el stick derecho se movio */
static void    *p_kbReadFn;             /* sceKeyboardRead (solo para ocultarlo) */
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
    lb_s("[kbm_pad] cargar "); lb_s(path); lb_s(" -> handle="); lb_x((uint32_t)h);
    lb_s(" res="); lb_d(res); lb_s("\n"); lb_flush();
    return h;
}

static void *find_sym(int32_t handle, const char *name)
{
    void *addr = NULL;
    int32_t r = kbm_dlsym(handle, name, &addr);
    lb_s("[kbm_pad] dlsym "); lb_s(name); lb_s(" -> "); lb_d(r); lb_s(" addr=");
    lb_x((uint64_t)(uintptr_t)addr); lb_s("\n"); lb_flush();
    return r == 0 ? addr : NULL;
}

/* Ruta con la palabra del sandbox primero; si falla, la ruta fija. */
static int32_t load_sys_lib(const char *name)
{
    char path[128];
    sys_lib_path(path, sizeof(path), name);
    int32_t h = load_lib(path);
    if (h > 0)
        return h;
    path[0] = 0;
    kbm_cat(path, sizeof(path), "/system/common/lib/");
    kbm_cat(path, sizeof(path), name);
    return load_lib(path);
}

static bool resolve_input_libs(void)
{
    int32_t hk = load_sys_lib("libSceKeyboard.sprx");
    if (hk > 0) {
        p_kbInit = (int32_t (*)(void))find_sym(hk, "sceKeyboardInit");
        p_kbOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))find_sym(hk, "sceKeyboardOpen");
        p_kbRead = (int32_t (*)(int32_t, OrbisKeyboardData *))find_sym(hk, "sceKeyboardReadState");
        p_kbReadFn = find_sym(hk, "sceKeyboardRead");
    }
    int32_t hm = load_sys_lib("libSceMouse.sprx");
    if (hm > 0) {
        p_mInit = (int32_t (*)(void))find_sym(hm, "sceMouseInit");
        p_mOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))find_sym(hm, "sceMouseOpen");
        p_mRead = (int32_t (*)(int32_t, void *, int32_t))find_sym(hm, "sceMouseRead");
    }
    bool kb_ok = p_kbInit && p_kbOpen && p_kbRead;
    bool m_ok  = p_mInit && p_mOpen && p_mRead;
    lb_s("[kbm_pad] funciones: teclado="); lb_s(kb_ok ? "OK" : "FALLO");
    lb_s(" raton="); lb_s(m_ok ? "OK" : "FALLO"); lb_s("\n"); lb_flush();
    if (!kb_ok) p_kbRead = NULL;
    if (!m_ok)  p_mRead = NULL;
    return kb_ok || m_ok;
}

/* ------------------------------------------ OCULTAR TECLADO Y RATON AL JUEGO */
/* Con el plugin activo el juego debe ver solo un mando: si detecta el teclado o el raton
 * USB pasa a su "modo teclado y raton" e ignora el stick que inyecta el plugin. Se
 * enganchan las funciones de libSceKeyboard / libSceMouse: para el juego devuelven
 * "sin dispositivo". El plugin sigue usando las originales (por el stub del Detour).
 * Solo se engancha una funcion si sus primeras instrucciones se pueden copiar sin tocar
 * (sin saltos relativos ni accesos RIP-relativos): el Detour las copia tal cual y,
 * si no, el juego se cerraria. Ante cualquier duda no se toca nada. */
#define KBM_NO_DEVICE ((int32_t)0x80DF0006)

static Detour g_dt_kbOpen  = { DetourMode_x64 };
static Detour g_dt_kbState = { DetourMode_x64 };
static Detour g_dt_kbRead  = { DetourMode_x64 };
static Detour g_dt_mOpen   = { DetourMode_x64 };
static Detour g_dt_mRead   = { DetourMode_x64 };

static int32_t kb_open_hide(int32_t u, int32_t t, int32_t i, void *p) { (void)u; (void)t; (void)i; (void)p; return KBM_NO_DEVICE; }
static int32_t kb_state_hide(int32_t h, void *d) { (void)h; (void)d; return KBM_NO_DEVICE; }
static int32_t kb_read_hide(int32_t h, void *d, int32_t n) { (void)h; (void)d; (void)n; return 0; }
static int32_t m_open_hide(int32_t u, int32_t t, int32_t i, void *p) { (void)u; (void)t; (void)i; (void)p; return KBM_NO_DEVICE; }
static int32_t m_read_hide(int32_t h, void *d, int32_t n) { (void)h; (void)d; (void)n; return 0; }

static bool prologue_safe(const void *fn)
{
    size_t size = 0;
    while (size < 14) {
        hde64s hs;
        uint32_t n = hde64_disasm((const uint8_t *)fn + size, &hs);
        if (n == 0 || (hs.flags & (F_ERROR | F_RELATIVE)))
            return false;
        if ((hs.flags & F_MODRM) && hs.modrm_mod == 0 && hs.modrm_rm == 5)
            return false;                       /* [RIP + disp32] */
        if (hs.opcode == 0xC3 || hs.opcode == 0xC2 || hs.opcode == 0xCC)
            return false;                       /* la funcion acaba antes de 14 bytes */
        size += n;
    }
    return true;
}

static void *hide_one(Detour *d, void *fn, void *hook, const char *name)
{
    if (!fn)
        return NULL;
    if (!prologue_safe(fn)) {
        lb_s("[kbm_pad] ocultar: "); lb_s(name); lb_s(" prologo no seguro, no se toca\n"); lb_flush();
        return NULL;
    }
    Detour_Construct(d, DetourMode_x64);
    void *stub = Detour_DetourFunction(d, (uint64_t)(uintptr_t)fn, hook);
    lb_s("[kbm_pad] ocultar: "); lb_s(name); lb_s(stub ? " enganchada\n" : " fallo al enganchar\n"); lb_flush();
    return stub;
}

static void hide_devices(void)
{
    if (g_hide_done)
        return;
    g_hide_done = true;
    if (!g_hide) {
        g_hide_state = 0;
        lb_s("[kbm_pad] ocultar: desactivado (ocultar=0)\n"); lb_flush();
        return;
    }
    int want = 0, ok = 0;
    void *s;
    if (p_kbOpen) {
        want++;
        s = hide_one(&g_dt_kbOpen, (void *)p_kbOpen, (void *)kb_open_hide, "sceKeyboardOpen");
        if (s) { p_kbOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))s; ok++; }
    }
    if (p_kbRead) {
        want++;
        s = hide_one(&g_dt_kbState, (void *)p_kbRead, (void *)kb_state_hide, "sceKeyboardReadState");
        if (s) { p_kbRead = (int32_t (*)(int32_t, OrbisKeyboardData *))s; ok++; }
    }
    if (p_kbReadFn) {
        want++;
        s = hide_one(&g_dt_kbRead, p_kbReadFn, (void *)kb_read_hide, "sceKeyboardRead");
        if (s) ok++;     /* el plugin no usa sceKeyboardRead: no hace falta guardar el stub */
    }
    if (p_mOpen) {
        want++;
        s = hide_one(&g_dt_mOpen, (void *)p_mOpen, (void *)m_open_hide, "sceMouseOpen");
        if (s) { p_mOpen = (int32_t (*)(int32_t, int32_t, int32_t, void *))s; ok++; }
    }
    if (p_mRead) {
        want++;
        s = hide_one(&g_dt_mRead, (void *)p_mRead, (void *)m_read_hide, "sceMouseRead");
        if (s) { p_mRead = (int32_t (*)(int32_t, void *, int32_t))s; ok++; }
    }
    g_hide_state = (want > 0 && ok == want) ? 1 : 2;
    lb_s("[kbm_pad] ocultar: "); lb_d(ok); lb_s(" de "); lb_d(want); lb_s(" funciones\n"); lb_flush();
}

/* ------------------------------------------------ AUTO-ACTUALIZACION */
/* Al arrancar el juego, un hilo espera unos segundos, lee VERSION del repositorio
 * (por HTTPS, con la verificacion de certificados del sistema) y, si es mayor que
 * KBM_VERSION, descarga el .prx de la release vN y lo deja en lugar del actual. Se
 * aplica al reiniciar el juego. Todo es opcional y a prueba de fallos:
 *   - /data/GoldHEN/kbm_pad.noupdate        -> desactiva la auto-actualizacion
 *   - /data/GoldHEN/kbm_pad.update.lock     -> intento en curso; si queda de un intento
 *                                              anterior 2 veces seguidas (cierre del
 *                                              juego), se crea kbm_pad.update.failed
 *   - /data/GoldHEN/kbm_pad.update.failed   -> auto-actualizacion desactivada; borrala
 *                                              por FTP para volver a activarla. */
#ifndef KBM_VERSION
#define KBM_VERSION 0
#endif
#define KBM_UPD_VERSION_URL "https://raw.githubusercontent.com/Y4ll/-ps4-kbm-pad/main/VERSION"
#define KBM_UPD_PRX_PREFIX  "https://github.com/Y4ll/-ps4-kbm-pad/releases/download/v"
#define KBM_UPD_PRX_SUFFIX  "/kbm_pad.prx"
#define KBM_PRX_PATH        "/data/GoldHEN/plugins/kbm_pad.prx"
#define KBM_PRX_NEW         "/data/GoldHEN/plugins/kbm_pad.prx.new"
#define KBM_UPD_LOCK        "/data/GoldHEN/kbm_pad.update.lock"
#define KBM_UPD_FAILED      "/data/GoldHEN/kbm_pad.update.failed"
#define KBM_UPD_OFF         "/data/GoldHEN/kbm_pad.noupdate"

extern int64_t kbm_sys_read(int32_t fd, void *buf, size_t n) __asm__("sceKernelRead");
extern int32_t kbm_sys_rename(const char *from, const char *to) __asm__("sceKernelRename");
extern int32_t kbm_sys_unlink(const char *path) __asm__("sceKernelUnlink");
extern int32_t kbm_usleep(uint32_t us) __asm__("sceKernelUsleep");
extern int32_t kbm_thread_create(void **th, const void *attr, void *(*entry)(void *),
                                 void *arg, const char *name) __asm__("scePthreadCreate");

static int32_t (*u_netInit)(void);
static int32_t (*u_poolCreate)(const char *name, int32_t size, int32_t flags);
static int32_t (*u_poolDestroy)(int32_t pool);
static int32_t (*u_sslInit)(size_t poolSize);
static int32_t (*u_sslTerm)(int32_t ctx);
static int32_t (*u_httpInit)(int32_t netPool, int32_t sslCtx, size_t poolSize);
static int32_t (*u_httpTerm)(int32_t ctx);
static int32_t (*u_tmplCreate)(int32_t ctx, const char *ua, int32_t ver, int32_t proxy);
static int32_t (*u_tmplDelete)(int32_t tmpl);
static int32_t (*u_connURL)(int32_t tmpl, const char *url, int32_t keepalive);
static int32_t (*u_connDelete)(int32_t conn);
static int32_t (*u_reqURL)(int32_t conn, int32_t method, const char *url, uint64_t len);
static int32_t (*u_reqDelete)(int32_t req);
static int32_t (*u_send)(int32_t req, const void *post, size_t size);
static int32_t (*u_status)(int32_t req, int32_t *code);
static int32_t (*u_read)(int32_t req, void *data, size_t size);
static int32_t (*u_autoRedir)(int32_t id, int32_t enable);
static int32_t (*u_tmoConn)(int32_t id, uint32_t us);
static int32_t (*u_tmoRecv)(int32_t id, uint32_t us);
static int32_t (*u_tmoSend)(int32_t id, uint32_t us);
static int32_t (*u_tmoResolve)(int32_t id, uint32_t us);

static void *upd_sym(int32_t h, const char *name)
{
    void *a = NULL;
    if (h > 0 && kbm_dlsym(h, name, &a) == 0)
        return a;
    return NULL;
}

static bool upd_exists(const char *path)
{
    int32_t fd = kbm_sys_open(path, 0, 0);
    if (fd < 0)
        return false;
    kbm_sys_close(fd);
    return true;
}

static void upd_write_small(const char *path, const char *text, size_t n)
{
    int32_t fd = kbm_sys_open(path, 0x0001 | 0x0200 | 0x0400, 0666);   /* WRONLY|CREAT|TRUNC */
    if (fd >= 0) {
        kbm_sys_write(fd, text, n);
        kbm_sys_close(fd);
    }
}

static bool upd_load_api(void)
{
    int32_t hn = load_sys_lib("libSceNet.sprx");
    int32_t hs = load_sys_lib("libSceSsl.sprx");
    int32_t hh = load_sys_lib("libSceHttp.sprx");
    u_netInit    = (int32_t (*)(void))upd_sym(hn, "sceNetInit");
    u_poolCreate = (int32_t (*)(const char *, int32_t, int32_t))upd_sym(hn, "sceNetPoolCreate");
    u_poolDestroy = (int32_t (*)(int32_t))upd_sym(hn, "sceNetPoolDestroy");
    u_sslInit    = (int32_t (*)(size_t))upd_sym(hs, "sceSslInit");
    u_sslTerm    = (int32_t (*)(int32_t))upd_sym(hs, "sceSslTerm");
    u_httpInit   = (int32_t (*)(int32_t, int32_t, size_t))upd_sym(hh, "sceHttpInit");
    u_httpTerm   = (int32_t (*)(int32_t))upd_sym(hh, "sceHttpTerm");
    u_tmplCreate = (int32_t (*)(int32_t, const char *, int32_t, int32_t))upd_sym(hh, "sceHttpCreateTemplate");
    u_tmplDelete = (int32_t (*)(int32_t))upd_sym(hh, "sceHttpDeleteTemplate");
    u_connURL    = (int32_t (*)(int32_t, const char *, int32_t))upd_sym(hh, "sceHttpCreateConnectionWithURL");
    u_connDelete = (int32_t (*)(int32_t))upd_sym(hh, "sceHttpDeleteConnection");
    u_reqURL     = (int32_t (*)(int32_t, int32_t, const char *, uint64_t))upd_sym(hh, "sceHttpCreateRequestWithURL");
    u_reqDelete  = (int32_t (*)(int32_t))upd_sym(hh, "sceHttpDeleteRequest");
    u_send       = (int32_t (*)(int32_t, const void *, size_t))upd_sym(hh, "sceHttpSendRequest");
    u_status     = (int32_t (*)(int32_t, int32_t *))upd_sym(hh, "sceHttpGetStatusCode");
    u_read       = (int32_t (*)(int32_t, void *, size_t))upd_sym(hh, "sceHttpReadData");
    u_autoRedir  = (int32_t (*)(int32_t, int32_t))upd_sym(hh, "sceHttpSetAutoRedirect");
    u_tmoConn    = (int32_t (*)(int32_t, uint32_t))upd_sym(hh, "sceHttpSetConnectTimeOut");
    u_tmoRecv    = (int32_t (*)(int32_t, uint32_t))upd_sym(hh, "sceHttpSetRecvTimeOut");
    u_tmoSend    = (int32_t (*)(int32_t, uint32_t))upd_sym(hh, "sceHttpSetSendTimeOut");
    u_tmoResolve = (int32_t (*)(int32_t, uint32_t))upd_sym(hh, "sceHttpSetResolveTimeOut");
    return u_netInit && u_poolCreate && u_poolDestroy && u_sslInit && u_sslTerm &&
           u_httpInit && u_httpTerm && u_tmplCreate && u_tmplDelete && u_connURL &&
           u_connDelete && u_reqURL && u_reqDelete && u_send && u_status && u_read &&
           u_autoRedir && u_tmoConn && u_tmoRecv && u_tmoSend && u_tmoResolve;
}

/* GET de una URL https. Si fd >= 0 el cuerpo se escribe en ese archivo; si no, va a mem
 * (con terminador). Devuelve 0 si todo salio bien (codigo 200). */
static int upd_get(int32_t tmpl, const char *url, int32_t fd, char *mem, size_t cap,
                   size_t *got, uint8_t *first4)
{
    static uint8_t buf[8192];
    *got = 0;
    int32_t conn = u_connURL(tmpl, url, 1);
    if (conn < 0)
        return -1;
    int rc = -2;
    int32_t req = u_reqURL(conn, 0 /* GET */, url, 0);
    if (req >= 0) {
        int32_t status = 0;
        if (u_send(req, NULL, 0) < 0) {
            rc = -3;
        } else if (u_status(req, &status) < 0 || status != 200) {
            rc = -4;
        } else {
            size_t total = 0;
            rc = 0;
            for (;;) {
                int32_t n = u_read(req, buf, sizeof(buf));
                if (n < 0) { rc = -5; break; }
                if (n == 0) break;
                if (first4)
                    for (int32_t i = 0; i < n && total + (size_t)i < 4; i++)
                        first4[total + (size_t)i] = buf[i];
                if (fd >= 0) {
                    if (kbm_sys_write(fd, buf, (size_t)n) != n) { rc = -6; break; }
                } else {
                    if (total + (size_t)n + 1 > cap) { rc = -7; break; }
                    for (int32_t i = 0; i < n; i++)
                        mem[total + (size_t)i] = (char)buf[i];
                }
                total += (size_t)n;
                if (total > 1024u * 1024u) { rc = -8; break; }
            }
            if (fd < 0 && rc == 0)
                mem[total] = 0;
            *got = total;
        }
        u_reqDelete(req);
    }
    u_connDelete(conn);
    return rc;
}

static void upd_do(void)
{
    if (!upd_load_api()) {
        L("[kbm_pad] update: faltan funciones de red\n");
        return;
    }
    u_netInit();
    int32_t pool = u_poolCreate("kbm_upd", 64 * 1024, 0);
    int32_t ssl  = u_sslInit(256 * 1024);
    int32_t http = u_httpInit(pool, ssl, 128 * 1024);
    int32_t tmpl = http >= 0 ? u_tmplCreate(http, "kbm_pad-updater/1", 2 /* HTTP 1.1 */, 1) : -1;
    if (tmpl >= 0) {
        u_autoRedir(tmpl, 1);
        u_tmoResolve(tmpl, 10u * 1000u * 1000u);
        u_tmoConn(tmpl, 10u * 1000u * 1000u);
        u_tmoSend(tmpl, 10u * 1000u * 1000u);
        u_tmoRecv(tmpl, 15u * 1000u * 1000u);

        char vbuf[32];
        size_t got = 0;
        int rc = upd_get(tmpl, KBM_UPD_VERSION_URL, -1, vbuf, sizeof(vbuf), &got, NULL);
        uint32_t remote = 0;
        bool digits = false;
        if (rc == 0) {
            for (size_t i = 0; i < got && vbuf[i] >= '0' && vbuf[i] <= '9'; i++) {
                remote = remote * 10 + (uint32_t)(vbuf[i] - '0');
                digits = true;
            }
        }
        lb_s("[kbm_pad] update: rc="); lb_d(rc); lb_s(" remota="); lb_d((int32_t)remote);
        lb_s(" local="); lb_d(KBM_VERSION); lb_s("\n"); lb_flush();

        if (rc == 0 && digits && remote > (uint32_t)KBM_VERSION) {
            char url[160];
            url[0] = 0;
            kbm_cat(url, sizeof(url), KBM_UPD_PRX_PREFIX);
            kbm_cat_u(url, sizeof(url), remote);
            kbm_cat(url, sizeof(url), KBM_UPD_PRX_SUFFIX);

            int32_t fd = kbm_sys_open(KBM_PRX_NEW, 0x0001 | 0x0200 | 0x0400, 0666);
            if (fd >= 0) {
                uint8_t first4[4] = {0, 0, 0, 0};
                size_t size = 0;
                int drc = upd_get(tmpl, url, fd, NULL, 0, &size, first4);
                kbm_sys_close(fd);
                /* FSELF: magic 4f 15 3d 1d y un tamano razonable */
                bool ok = drc == 0 && size > 16384 && size < 1024u * 1024u &&
                          first4[0] == 0x4f && first4[1] == 0x15 &&
                          first4[2] == 0x3d && first4[3] == 0x1d;
                lb_s("[kbm_pad] update: descarga rc="); lb_d(drc); lb_s(" bytes="); lb_d((int32_t)size);
                lb_s(ok ? " OK\n" : " INVALIDA\n"); lb_flush();
                if (ok && kbm_sys_rename(KBM_PRX_NEW, KBM_PRX_PATH) == 0) {
                    char msg[96];
                    msg[0] = 0;
                    kbm_cat(msg, sizeof(msg), "kbm_pad actualizado a v");
                    kbm_cat_u(msg, sizeof(msg), remote);
                    kbm_cat(msg, sizeof(msg), ". Reinicia el juego.");
                    notify(msg);
                } else {
                    kbm_sys_unlink(KBM_PRX_NEW);
                }
            }
        }
        u_tmplDelete(tmpl);
    }
    if (http >= 0) u_httpTerm(http);
    if (ssl >= 0)  u_sslTerm(ssl);
    if (pool >= 0) u_poolDestroy(pool);
}

static void upd_run(void)
{
    if (upd_exists(KBM_UPD_OFF) || upd_exists(KBM_UPD_FAILED))
        return;
    /* El archivo lock cuenta intentos sin terminar (el juego se cerro a medias). */
    int tries = 0;
    int32_t fd = kbm_sys_open(KBM_UPD_LOCK, 0, 0);
    if (fd >= 0) {
        char c = 0;
        if (kbm_sys_read(fd, &c, 1) == 1 && c >= '0' && c <= '9')
            tries = c - '0';
        kbm_sys_close(fd);
    }
    if (tries >= 2) {
        upd_write_small(KBM_UPD_FAILED, "1", 1);
        kbm_sys_unlink(KBM_UPD_LOCK);
        note("kbm_pad: auto-actualizacion desactivada tras un fallo");
        return;
    }
    char t = (char)('0' + tries + 1);
    upd_write_small(KBM_UPD_LOCK, &t, 1);
    upd_do();
    kbm_sys_unlink(KBM_UPD_LOCK);
}

static void *upd_thread(void *arg)
{
    (void)arg;
    kbm_usleep(20u * 1000u * 1000u);   /* dejar que el juego termine de arrancar */
    upd_run();
    return NULL;
}

static void start_update_thread(void)
{
    void *th = NULL;
    kbm_thread_create(&th, NULL, upd_thread, NULL, "kbm_upd");
}

/* ------------------------------------------------ CONFIGURACION (kbm_pad.ini) */
/* Todo se puede cambiar editando /data/GoldHEN/kbm_pad.ini por FTP (se crea solo la
 * primera vez) y pulsando F6 dentro del juego para recargarlo, sin recompilar.
 * Sin funciones variadicas ni libc: el analizador es propio. */
#define KBM_INI_PATH "/data/GoldHEN/kbm_pad.ini"

#define KEY_NONE  0xFFFF   /* "ninguna" */
#define KEY_BAD   0xFFFE   /* nombre de tecla desconocido */
/* Modificadores: 0x100 + mascara de OrbisKeyboardData.mods (izq | der) */
#define KEY_SHIFT 0x122
#define KEY_CTRL  0x111
#define KEY_ALT   0x144

#define MOUSE_BTN_MIDDLE 0x4

typedef struct { uint16_t code; uint32_t mask; } KbmBind;
#define KBM_MAX_BINDS 48
static KbmBind  g_binds[KBM_MAX_BINDS];
static int      g_nbinds;
static uint32_t g_mouse_map[3];            /* izq, der, medio -> boton del mando */
static uint16_t g_k_pause, g_k_drive, g_k_sens_up, g_k_sens_dn, g_k_reload;
static uint16_t g_k_fwd, g_k_back, g_k_left, g_k_right;
static float    g_sens_x, g_sens_y, g_smooth, g_deadzone, g_accel;
static bool     g_invert_y, g_aim_toggle;
static bool     g_drive;                   /* modo conducir */
static float    g_steer;                   /* volante suavizado */
static int32_t  g_user;

typedef struct { const char *name; uint32_t mask; } KbmBtnName;
static const KbmBtnName k_btn_names[] = {
    {"cruz", KBM_PAD_BUTTON_CROSS},       {"circulo", KBM_PAD_BUTTON_CIRCLE},
    {"cuadrado", KBM_PAD_BUTTON_SQUARE},  {"triangulo", KBM_PAD_BUTTON_TRIANGLE},
    {"l1", KBM_PAD_BUTTON_L1},            {"r1", KBM_PAD_BUTTON_R1},
    {"l2", KBM_PAD_BUTTON_L2},            {"r2", KBM_PAD_BUTTON_R2},
    {"l3", KBM_PAD_BUTTON_L3},            {"r3", KBM_PAD_BUTTON_R3},
    {"options", KBM_PAD_BUTTON_OPTIONS},  {"touchpad", KBM_PAD_BUTTON_TOUCH_PAD},
    {"dpad_arriba", KBM_PAD_BUTTON_UP},   {"dpad_abajo", KBM_PAD_BUTTON_DOWN},
    {"dpad_izq", KBM_PAD_BUTTON_LEFT},    {"dpad_der", KBM_PAD_BUTTON_RIGHT},
};

typedef struct { const char *name; uint16_t code; } KbmKeyName;
static const KbmKeyName k_key_names[] = {
    {"espacio", 44}, {"space", 44}, {"enter", 40}, {"intro", 40}, {"tab", 43},
    {"esc", 41}, {"borrar", 42}, {"caps", 57},
    {"flecha_arriba", 82}, {"flecha_abajo", 81}, {"flecha_izq", 80}, {"flecha_der", 79},
    {"shift", KEY_SHIFT}, {"ctrl", KEY_CTRL}, {"alt", KEY_ALT}, {"ninguna", KEY_NONE},
};

static const char k_ini_default[] =
    "# kbm_pad - configuracion. Edita este archivo por FTP y en el juego pulsa F6 para recargarlo.\n"
    "# Las lineas que empiezan con # se ignoran.\n"
    "# Teclas: a-z, 0-9, f1-f12, espacio, enter, tab, esc, borrar, caps, shift, ctrl, alt,\n"
    "# flecha_arriba, flecha_abajo, flecha_izq, flecha_der, ninguna.\n"
    "# Un boton puede tener varias teclas separadas por coma (ejemplo: r3=ctrl,v).\n"
    "\n"
    "# --- Estado ---\n"
    "# activo=1 enciende el plugin y activo=0 lo pausa (teclado y raton dejan de ser mando).\n"
    "# La app kbm_manager cambia esta linea; el plugin vigila el archivo y aplica los\n"
    "# cambios solo, sin pulsar F6. El estado real queda en kbm_pad.status.\n"
    "activo=1\n"
    "\n"
    "# --- Camara (raton) ---\n"
    "# sens_x / sens_y: sube para girar mas rapido. suavizado: 0 = directo, 0.8 = muy suave.\n"
    "# zona_muerta: la del stick del juego (sube si la camara no arranca con movimientos lentos).\n"
    "# curva: 0 = lineal; mas alto = movimientos rapidos giran mucho mas que los lentos.\n"
    "sens_x=6.0\n"
    "sens_y=6.0\n"
    "suavizado=0.40\n"
    "zona_muerta=0.15\n"
    "curva=0.40\n"
    "invertir_y=0\n"
    "alternar_mira=0\n"
    "\n"
    "# --- Ocultar teclado y raton al juego ---\n"
    "# ocultar=1: el juego solo ve un mando (no detecta el teclado ni el raton USB).\n"
    "# ocultar=0: los deja visibles. El cambio se aplica al reiniciar el juego.\n"
    "ocultar=1\n"
    "\n"
    "# --- Teclas de control ---\n"
    "tecla_pausa=f8\n"
    "tecla_conducir=f7\n"
    "tecla_sens_menos=f9\n"
    "tecla_sens_mas=f10\n"
    "tecla_recargar=f6\n"
    "\n"
    "# --- Movimiento (stick izquierdo) ---\n"
    "adelante=w\n"
    "atras=s\n"
    "izquierda=a\n"
    "derecha=d\n"
    "\n"
    "# --- Botones del mando ---\n"
    "cruz=espacio\n"
    "circulo=e\n"
    "cuadrado=q\n"
    "triangulo=r\n"
    "l1=f\n"
    "r1=g\n"
    "l2=ninguna\n"
    "r2=ninguna\n"
    "l3=shift\n"
    "r3=ctrl,v\n"
    "options=enter\n"
    "touchpad=tab\n"
    "dpad_arriba=flecha_arriba\n"
    "dpad_abajo=flecha_abajo\n"
    "dpad_izq=flecha_izq\n"
    "dpad_der=flecha_der\n"
    "\n"
    "# --- Raton: izq, der y medio pueden ser cualquier boton del mando (o ninguno) ---\n"
    "raton_izq=r2\n"
    "raton_der=l2\n"
    "raton_medio=r3\n"
    "\n"
    "# Modo conducir (tecla_conducir, F7): W = acelerar (R2), S = frenar o reversa (L2),\n"
    "# A y D = volante. Vuelve a pulsarla para regresar al modo normal.\n";

static float kbm_absf(float v) { return v < 0.0f ? -v : v; }
static float kbm_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static size_t kbm_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool kbm_streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static float parse_float(const char *s)
{
    float sign = 1.0f, v = 0.0f, scale = 0.1f;
    if (*s == '-') {
        sign = -1.0f;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0f + (float)(*s - '0');
        s++;
    }
    if (*s == '.' || *s == ',') {
        s++;
        while (*s >= '0' && *s <= '9') {
            v += (float)(*s - '0') * scale;
            scale *= 0.1f;
            s++;
        }
    }
    return sign * v;
}

static uint16_t parse_key(const char *s)
{
    if (!s[0])
        return KEY_BAD;
    if (!s[1]) {
        char c = s[0];
        if (c >= 'a' && c <= 'z') return (uint16_t)(4 + (c - 'a'));
        if (c >= '1' && c <= '9') return (uint16_t)(30 + (c - '1'));
        if (c == '0') return 39;
    }
    if (s[0] == 'f' && s[1] >= '1' && s[1] <= '9') {
        int n = s[1] - '0';
        if (s[2] == 0)
            return (uint16_t)(58 + n - 1);
        if (n == 1 && s[2] >= '0' && s[2] <= '2' && s[3] == 0)
            return (uint16_t)(58 + (10 + (s[2] - '0')) - 1);
        return KEY_BAD;
    }
    for (size_t i = 0; i < sizeof(k_key_names) / sizeof(k_key_names[0]); i++)
        if (kbm_streq(s, k_key_names[i].name))
            return k_key_names[i].code;
    return KEY_BAD;
}

static uint32_t find_button(const char *s)
{
    for (size_t i = 0; i < sizeof(k_btn_names) / sizeof(k_btn_names[0]); i++)
        if (kbm_streq(s, k_btn_names[i].name))
            return k_btn_names[i].mask;
    return 0;
}

static void bind_clear(uint32_t mask)
{
    int w = 0;
    for (int i = 0; i < g_nbinds; i++)
        if (g_binds[i].mask != mask)
            g_binds[w++] = g_binds[i];
    g_nbinds = w;
}

static void bind_add(uint16_t code, uint32_t mask)
{
    if (code == KEY_NONE || g_nbinds >= KBM_MAX_BINDS)
        return;
    g_binds[g_nbinds].code = code;
    g_binds[g_nbinds].mask = mask;
    g_nbinds++;
}

static void config_defaults(void)
{
    g_nbinds = 0;
    bind_add(K_SPACE, KBM_PAD_BUTTON_CROSS);
    bind_add(K_E, KBM_PAD_BUTTON_CIRCLE);
    bind_add(K_Q, KBM_PAD_BUTTON_SQUARE);
    bind_add(K_R, KBM_PAD_BUTTON_TRIANGLE);
    bind_add(K_F, KBM_PAD_BUTTON_L1);
    bind_add(K_G, KBM_PAD_BUTTON_R1);
    bind_add(K_ENTER, KBM_PAD_BUTTON_OPTIONS);
    bind_add(K_TAB, KBM_PAD_BUTTON_TOUCH_PAD);
    bind_add(KEY_SHIFT, KBM_PAD_BUTTON_L3);
    bind_add(KEY_CTRL, KBM_PAD_BUTTON_R3);
    bind_add(25 /* v */, KBM_PAD_BUTTON_R3);
    bind_add(K_UP, KBM_PAD_BUTTON_UP);
    bind_add(K_DOWN, KBM_PAD_BUTTON_DOWN);
    bind_add(K_LEFT, KBM_PAD_BUTTON_LEFT);
    bind_add(K_RIGHT, KBM_PAD_BUTTON_RIGHT);
    g_mouse_map[0] = KBM_PAD_BUTTON_R2;
    g_mouse_map[1] = KBM_PAD_BUTTON_L2;
    g_mouse_map[2] = KBM_PAD_BUTTON_R3;
    g_k_pause = TOGGLE_KEYCODE;   /* F8 */
    g_k_drive = 64;               /* F7 */
    g_k_sens_dn = 66;             /* F9 */
    g_k_sens_up = 67;             /* F10 */
    g_k_reload = 63;              /* F6 */
    g_k_fwd = K_W;
    g_k_back = K_S;
    g_k_left = K_A;
    g_k_right = K_D;
    g_sens_x = g_sens_y = MOUSE_SENS;
    g_smooth = MOUSE_DECAY;
    g_deadzone = MOUSE_DEADZONE;
    g_accel = 0.40f;
    g_invert_y = INVERT_MOUSE_Y != 0;
    g_aim_toggle = AIM_TOGGLE != 0;
    g_hide = true;
}

static void ini_bad(const char *key, const char *val)
{
    lb_s("[kbm_pad] ini: no entiendo \""); lb_s(key); lb_s("="); lb_s(val); lb_s("\"\n");
    lb_flush();
}

static void ini_apply(const char *key, const char *val)
{
    if (kbm_streq(key, "activo"))      { g_enabled = parse_float(val) != 0.0f; return; }
    if (kbm_streq(key, "sens_x"))     { g_sens_x = kbm_clampf(parse_float(val), 0.1f, 60.0f); return; }
    if (kbm_streq(key, "sens_y"))      { g_sens_y = kbm_clampf(parse_float(val), 0.1f, 60.0f); return; }
    if (kbm_streq(key, "suavizado"))   { g_smooth = kbm_clampf(parse_float(val), 0.0f, 0.90f); return; }
    if (kbm_streq(key, "zona_muerta")) { g_deadzone = kbm_clampf(parse_float(val), 0.0f, 0.60f); return; }
    if (kbm_streq(key, "curva"))       { g_accel = kbm_clampf(parse_float(val), 0.0f, 3.0f); return; }
    if (kbm_streq(key, "invertir_y"))  { g_invert_y = parse_float(val) != 0.0f; return; }
    if (kbm_streq(key, "ocultar")) { g_hide = parse_float(val) != 0.0f; return; }
    if (kbm_streq(key, "alternar_mira")) { g_aim_toggle = parse_float(val) != 0.0f; return; }

    uint16_t *kp = NULL;
    if (kbm_streq(key, "tecla_pausa"))       kp = &g_k_pause;
    else if (kbm_streq(key, "tecla_conducir"))    kp = &g_k_drive;
    else if (kbm_streq(key, "tecla_sens_menos"))  kp = &g_k_sens_dn;
    else if (kbm_streq(key, "tecla_sens_mas"))    kp = &g_k_sens_up;
    else if (kbm_streq(key, "tecla_recargar"))    kp = &g_k_reload;
    else if (kbm_streq(key, "adelante"))          kp = &g_k_fwd;
    else if (kbm_streq(key, "atras"))             kp = &g_k_back;
    else if (kbm_streq(key, "izquierda"))         kp = &g_k_left;
    else if (kbm_streq(key, "derecha"))           kp = &g_k_right;
    if (kp) {
        uint16_t c = parse_key(val);
        if (c == KEY_BAD) ini_bad(key, val);
        else *kp = c;
        return;
    }

    int mi = -1;
    if (kbm_streq(key, "raton_izq")) mi = 0;
    else if (kbm_streq(key, "raton_der")) mi = 1;
    else if (kbm_streq(key, "raton_medio")) mi = 2;
    if (mi >= 0) {
        uint32_t m = find_button(val);
        if (m == 0 && !kbm_streq(val, "ninguno") && !kbm_streq(val, "ninguna")) ini_bad(key, val);
        else g_mouse_map[mi] = m;
        return;
    }

    uint32_t mask = find_button(key);
    if (mask) {
        bind_clear(mask);
        char tok[24];
        const char *p = val;
        while (*p) {
            int n = 0;
            while (*p && *p != ',') {
                if (*p != ' ' && *p != '\t' && n < 23)
                    tok[n++] = *p;
                p++;
            }
            tok[n] = 0;
            if (*p == ',')
                p++;
            if (n) {
                uint16_t c = parse_key(tok);
                if (c == KEY_BAD) ini_bad(key, tok);
                else bind_add(c, mask);
            }
        }
        return;
    }
    ini_bad(key, val);
}

static char *ini_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    char *e = s + kbm_len(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = 0;
    return s;
}

static char g_ini[4096];

static void config_load(void)
{
    config_defaults();
    if (!upd_exists(KBM_INI_PATH)) {
        upd_write_small(KBM_INI_PATH, k_ini_default, sizeof(k_ini_default) - 1);
        L("[kbm_pad] config: se creo kbm_pad.ini con los valores por defecto\n");
        return;
    }
    int32_t fd = kbm_sys_open(KBM_INI_PATH, 0, 0);
    if (fd < 0)
        return;
    size_t tot = 0;
    for (;;) {
        int64_t r = kbm_sys_read(fd, g_ini + tot, sizeof(g_ini) - 1 - tot);
        if (r <= 0)
            break;
        tot += (size_t)r;
        if (tot >= sizeof(g_ini) - 1)
            break;
    }
    kbm_sys_close(fd);
    g_ini[tot] = 0;

    char *p = g_ini;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        if (*p)
            *p++ = 0;
        line = ini_trim(line);
        if (!*line || *line == '#' || *line == ';')
            continue;
        char *eq = line;
        while (*eq && *eq != '=')
            eq++;
        if (!*eq)
            continue;
        *eq = 0;
        char *key = ini_trim(line);
        char *val = ini_trim(eq + 1);
        for (char *q = key; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        for (char *q = val; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        ini_apply(key, val);
    }
    L("[kbm_pad] config: kbm_pad.ini cargado\n");
}

/* ------------------------------------------------------------ DISPOSITIVOS */
static void cat_f1(char *dst, size_t cap, float v)   /* un decimal */
{
    int t = (int)(v * 10.0f + 0.5f);
    kbm_cat_u(dst, cap, (uint32_t)(t / 10));
    kbm_cat(dst, cap, ".");
    kbm_cat_u(dst, cap, (uint32_t)(t % 10));
}

static void open_devices(bool first)
{
    bool had_kb = g_kb >= 0, had_m = g_mouse >= 0;
    if (g_kb < 0 && p_kbInit && p_kbOpen) {
        p_kbInit();
        g_kb = p_kbOpen(g_user, 0, 0, NULL);
    }
    if (g_mouse < 0 && p_mInit && p_mOpen) {
        p_mInit();
        /* El raton puede estar en el indice 0 o 1 segun el receptor USB: se abren los dos
         * y se guardan los handles que abran bien. Parametros: 8 bytes en cero. */
        for (int32_t idx = 0; idx < 2; idx++) {
            int32_t h = p_mOpen(g_user, 0, idx, g_open_zero);
            lb_s("[kbm_pad] sceMouseOpen indice="); lb_d(idx); lb_s(" -> "); lb_d(h); lb_s("\n"); lb_flush();
            if (h < 0)
                continue;
            if (g_mouse < 0)
                g_mouse = h;
            else if (g_mouse2 < 0)
                g_mouse2 = h;
        }
        if (g_mouse < 0) {      /* respaldo: la apertura antigua con parametro nulo */
            g_mouse = p_mOpen(g_user, 0, 0, NULL);
            lb_s("[kbm_pad] sceMouseOpen (param nulo) -> "); lb_d(g_mouse); lb_s("\n"); lb_flush();
        }
    }
    bool kb_new = !had_kb && g_kb >= 0, m_new = !had_m && g_mouse >= 0;
    if (!kb_new && !m_new && !first)
        return;

    lb_s("[kbm_pad] user="); lb_d(g_user); lb_s(" keyboard="); lb_d(g_kb);
    lb_s(" mouse="); lb_d(g_mouse); lb_s(" mouse2="); lb_d(g_mouse2); lb_s("\n"); lb_flush();

    char msg[96];
    msg[0] = 0;
    kbm_cat(msg, sizeof(msg), "kbm_pad: ");
    if (g_kb >= 0 && g_mouse >= 0)  kbm_cat(msg, sizeof(msg), "teclado y raton listos");
    else if (g_kb >= 0)             kbm_cat(msg, sizeof(msg), "teclado listo (conecta el raton)");
    else if (g_mouse >= 0)          kbm_cat(msg, sizeof(msg), "raton listo (conecta el teclado)");
    else                            kbm_cat(msg, sizeof(msg), "conecta teclado y raton");
    if (first && g_kb >= 0 && g_mouse >= 0)
        note(msg);          /* todo listo al arrancar: sin aviso extra en pantalla (solo al log) */
    else
        notify(msg);        /* falta un dispositivo, o se acaba de conectar uno */
}

/* ---------------------------------------------- CONTROL DESDE LA APP
 * La app (kbm_manager) escribe "activo=0/1" y demas ajustes en kbm_pad.ini; aqui se
 * vigila el archivo (cada ~2 s) y se recarga solo cuando cambia. El estado real
 * (activo, modo conducir, dispositivos, sensibilidad) se publica en kbm_pad.status
 * para que la app lo muestre. */
#define KBM_STATUS_PATH "/data/GoldHEN/kbm_pad.status"

static uint32_t g_ini_hash;
static bool     g_ini_hash_ok;

/* Huella (FNV-1a) del contenido del ini; 0 si no se puede leer. */
static uint32_t ini_fingerprint(void)
{
    int32_t fd = kbm_sys_open(KBM_INI_PATH, 0, 0);
    if (fd < 0)
        return 0;
    uint32_t h = 2166136261u;
    uint8_t buf[256];
    for (;;) {
        int64_t r = kbm_sys_read(fd, buf, sizeof(buf));
        if (r <= 0)
            break;
        for (int64_t i = 0; i < r; i++)
            h = (h ^ buf[i]) * 16777619u;
    }
    kbm_sys_close(fd);
    return h ? h : 1;
}

static void ini_mark_seen(void)
{
    g_ini_hash = ini_fingerprint();
    g_ini_hash_ok = true;
}

static uint32_t kbm_bl(uint32_t v)   /* numero de bits de v (0 si v == 0) */
{
    uint32_t n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

/* Escribe kbm_pad.status y deja el resultado en el log (las primeras veces, o siempre que
 * falle): asi, si el archivo no aparece en la consola, el log dice si fallo open() o write(). */
static void status_write(const char *text, size_t n)
{
    static int logged;
    int32_t fd = kbm_sys_open(KBM_STATUS_PATH, 0x0001 | 0x0200 | 0x0400, 0666);   /* WRONLY|CREAT|TRUNC */
    int64_t w = -1;
    if (fd >= 0) {
        w = kbm_sys_write(fd, text, n);
        kbm_sys_close(fd);
    }
    bool bad = fd < 0 || w != (int64_t)n;
    if ((logged < 3 || bad) && logged < 12) {
        logged++;
        lb_s("[kbm_pad] estado: open="); lb_d(fd); lb_s(" write="); lb_d((int32_t)w);
        lb_s(" de "); lb_d((int32_t)n); lb_s(bad ? " FALLO\n" : " OK\n"); lb_flush();
    }
}

/* force = true escribe aunque nada haya cambiado. Ademas, aunque nada cambie, se reescribe
 * cada ~30 s (15 llamadas desde external_poll): si el archivo se borra por FTP reaparece. */
static void status_publish(bool force)
{
    static uint32_t last_sig = 0xFFFFFFFFu;
    static int beat;
    uint32_t sig = (g_enabled ? 1u : 0u) | (g_drive ? 2u : 0u) |
                   (g_kb >= 0 ? 4u : 0u) | (g_mouse >= 0 ? 8u : 0u) |
                   ((uint32_t)g_hide_state << 4) |
                   ((uint32_t)(g_sens_x * 10.0f) << 8) |
                   (kbm_bl(g_dbg_mmove) << 18) | (kbm_bl(g_dbg_stick) << 24);
    if (!force && sig == last_sig && ++beat < 15)
        return;
    beat = 0;
    last_sig = sig;
    char s[280];
    s[0] = 0;
    kbm_cat(s, sizeof(s), "version=");   kbm_cat_u(s, sizeof(s), KBM_VERSION);
    kbm_cat(s, sizeof(s), "\nactivo=");   kbm_cat_u(s, sizeof(s), g_enabled ? 1 : 0);
    kbm_cat(s, sizeof(s), "\nconducir="); kbm_cat_u(s, sizeof(s), g_drive ? 1 : 0);
    kbm_cat(s, sizeof(s), "\nteclado=");  kbm_cat_u(s, sizeof(s), g_kb >= 0 ? 1 : 0);
    kbm_cat(s, sizeof(s), "\nraton=");    kbm_cat_u(s, sizeof(s), g_mouse >= 0 ? 1 : 0);
    kbm_cat(s, sizeof(s), "\nsens=");     cat_f1(s, sizeof(s), g_sens_x);
    kbm_cat(s, sizeof(s), "\naviso=");    kbm_cat_u(s, sizeof(s), (uint32_t)g_notify_rc);
    kbm_cat(s, sizeof(s), "\nocultar=");  kbm_cat_u(s, sizeof(s), (uint32_t)g_hide_state);
    kbm_cat(s, sizeof(s), "\nocultar_pronto=");  kbm_cat_u(s, sizeof(s), g_libs_early ? 1 : 0);
    kbm_cat(s, sizeof(s), "\nraton_mov="); kbm_cat_u(s, sizeof(s), g_dbg_mmove);
    kbm_cat(s, sizeof(s), "\nstick_der="); kbm_cat_u(s, sizeof(s), g_dbg_stick);
    kbm_cat(s, sizeof(s), "\n");
    status_write(s, kbm_len(s));
}

static void external_poll(void)
{
    static int tick;
    if (++tick < 120)
        return;
    tick = 0;
    uint32_t h = ini_fingerprint();
    if (g_ini_hash_ok && h != 0 && h != g_ini_hash) {
        bool was = g_enabled;
        config_load();
        L("[kbm_pad] config: kbm_pad.ini cambio, recargado\n");
        if (g_enabled != was)
            notify(g_enabled ? "kbm_pad ACTIVO (teclado y raton = mando)"
                             : "kbm_pad PAUSA (mando normal)");
    }
    g_ini_hash = h;
    g_ini_hash_ok = true;
    status_publish(false);
}

/* Deteccion automatica: si falta algun dispositivo se reintenta abrirlo cada ~2 s,
 * asi se puede conectar con el juego ya abierto. */
static void poll_devices(void)
{
    external_poll();
    /* Segundo aviso, unos segundos despues de que el juego empiece a leer el mando
     * (~300 lecturas): confirma que el enganche funciona y si hay teclado y raton. */
    static int late;
    if (late >= 0 && ++late >= 300) {
        late = -1;
        char m[96];
        m[0] = 0;
        kbm_cat(m, sizeof(m), KBM_NAME " v");
        kbm_cat_u(m, sizeof(m), KBM_VERSION);
        kbm_cat(m, sizeof(m), g_kb >= 0 ? ": teclado SI" : ": teclado NO");
        kbm_cat(m, sizeof(m), g_mouse >= 0 ? ", raton SI" : ", raton NO");
        note(m);            /* diagnostico: solo al log, sin aviso en pantalla */
    }
    static int tick;
    if (g_kb >= 0 && g_mouse >= 0)
        return;
    if (++tick < 120)
        return;
    tick = 0;
    open_devices(false);
}

static void ensure_input(void)
{
    if (g_ready || g_failed)
        return;

    note("kbm_pad: paso 1 (iniciando entrada)");
    sceUserServiceGetInitialUser(&g_user);
    note("kbm_pad: paso 2 (usuario OK)");

    config_load();
    ini_mark_seen();

    /* Si plugin_load ya las resolvio (y enganchó la ocultacion), NO se resuelven otra vez:
     * dlsym devolveria las funciones originales y pisaria los stubs de los enganches. */
    if (!g_libs_early) {
        /* Los sysmodule IDs hacen que el sistema cargue las librerias; despues se
         * resuelven por ruta y dlsym. */
        sceSysmoduleLoadModule(KBM_SYSMODULE_KEYBOARD);
        sceSysmoduleLoadModule(KBM_SYSMODULE_MOUSE);
        note("kbm_pad: paso 3 (sysmodule OK)");

        if (!resolve_input_libs()) {
            L("[kbm_pad] no se pudo resolver teclado ni raton\n");
            notify("kbm_pad: no se pudo cargar teclado/raton");
            g_failed = true;
            return;
        }
    }

    note("kbm_pad: paso 4 (librerias resueltas)");
    hide_devices();       /* ya hecho si fue temprano (g_hide_done) */
    open_devices(true);
    g_ready = true;   /* aunque falte algun dispositivo: poll_devices lo reintenta */
    status_publish(true);   /* el archivo existe desde el primer segundo, no a los ~2 s */
}

/* Se llama desde plugin_load, ANTES de que el juego inicialice su entrada. Asi el juego
 * llama a sceKeyboardOpen / sceMouseOpen ya enganchadas y nunca llega a ver el teclado ni
 * el raton (si se enganchaba en la primera lectura del mando, el juego ya los habia
 * abierto). Solo actua con ocultar=1; con ocultar=0 todo queda como antes. */
static void early_hide(void)
{
    config_load();
    if (!g_hide)
        return;
    sceSysmoduleLoadModule(KBM_SYSMODULE_KEYBOARD);
    sceSysmoduleLoadModule(KBM_SYSMODULE_MOUSE);
    if (!resolve_input_libs()) {
        lb_s("[kbm_pad] ocultar temprano: no se pudieron resolver las librerias\n"); lb_flush();
        return;     /* ensure_input lo reintenta en la primera lectura del mando */
    }
    g_libs_early = true;
    hide_devices();
    lb_s("[kbm_pad] ocultar temprano: hecho en plugin_load\n"); lb_flush();
}

static bool has_key(const OrbisKeyboardData *kd, uint16_t code)
{
    for (int i = 0; i < kd->nkeys && i < 32; i++)
        if (kd->keycodes[i] == code)
            return true;
    return false;
}

static bool key_down(const OrbisKeyboardData *kd, uint16_t code)
{
    if (code == KEY_NONE || code == KEY_BAD)
        return false;
    if (code >= 0x100)
        return (kd->mods & (uint32_t)(code & 0xFF)) != 0;
    return has_key(kd, code);
}

static bool edge(bool down, bool *prev)
{
    bool r = down && !*prev;
    *prev = down;
    return r;
}

static uint8_t to_axis(float v)   /* -1..1 -> 0..255 */
{
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (uint8_t)(128.0f + v * 127.0f);
}

/* Salta la zona muerta del juego: 0 se queda en 0; cualquier otro valor arranca en
 * g_deadzone y llega a 1.0. */
static float comp_deadzone(float v)
{
    if (v > -0.02f && v < 0.02f)
        return 0.0f;
    float a = kbm_absf(v);
    if (a > 1.0f)
        a = 1.0f;
    a = g_deadzone + a * (1.0f - g_deadzone);
    return v < 0.0f ? -a : a;
}

static void pad_press(KbmPadData *d, uint32_t mask)
{
    d->buttons |= mask;
    if (mask & KBM_PAD_BUTTON_L2) d->analogButtons.l2 = 255;
    if (mask & KBM_PAD_BUTTON_R2) d->analogButtons.r2 = 255;
}

/* Mezcla teclado + raton en un KbmPadData. */
static void apply_kbm(KbmPadData *d)
{
    OrbisKeyboardData kd;
    kbm_zero(&kd, sizeof(kd));
    if (g_kb >= 0 && p_kbRead) {
        static int kb_err_logged, kb_key_logged;
        int32_t krc = p_kbRead(g_kb, &kd);
        if (krc < 0) {
            if (kb_err_logged < 3) {
                kb_err_logged++;
                lb_s("[kbm_pad] teclado: lectura fallo rc="); lb_d(krc); lb_s("\n"); lb_flush();
            }
            kbm_zero(&kd, sizeof(kd));
        } else if (kd.nkeys > 0 && kb_key_logged < 5) {
            kb_key_logged++;
            lb_s("[kbm_pad] teclado: lectura OK, teclas="); lb_d(kd.nkeys);
            lb_s(" ocultar="); lb_d(g_hide_state); lb_s("\n"); lb_flush();
        }
    }

    /* Teclas de control (flanco de subida) */
    static bool e_pause, e_drive, e_up, e_dn, e_reload;
    if (edge(key_down(&kd, g_k_pause), &e_pause)) {
        g_enabled = !g_enabled;
        L(g_enabled ? "[kbm_pad] ACTIVO\n" : "[kbm_pad] PAUSA\n");
        notify(g_enabled ? "kbm_pad ACTIVO (teclado y raton = mando)" : "kbm_pad PAUSA (mando normal)");
    }
    if (edge(key_down(&kd, g_k_drive), &e_drive)) {
        g_drive = !g_drive;
        g_steer = 0.0f;
        notify(g_drive ? "kbm_pad: modo CONDUCIR (W acelera, S frena, A/D volante)"
                       : "kbm_pad: modo NORMAL");
    }
    bool sens_up = edge(key_down(&kd, g_k_sens_up), &e_up);
    bool sens_dn = edge(key_down(&kd, g_k_sens_dn), &e_dn);
    if (sens_up || sens_dn) {
        float f = sens_up ? 1.2f : (1.0f / 1.2f);
        g_sens_x = kbm_clampf(g_sens_x * f, 0.5f, 60.0f);
        g_sens_y = kbm_clampf(g_sens_y * f, 0.5f, 60.0f);
        char m[64];
        m[0] = 0;
        kbm_cat(m, sizeof(m), "kbm_pad: sensibilidad ");
        cat_f1(m, sizeof(m), g_sens_x);
        notify(m);
    }
    if (edge(key_down(&kd, g_k_reload), &e_reload)) {
        config_load();
        ini_mark_seen();
        notify("kbm_pad: configuracion recargada");
    }

    /* Raton: acumular deltas aunque este en pausa, para no acumular salto */
    int32_t dx = 0, dy = 0;
    /* Estado de botones PERSISTENTE: sin muestras nuevas (raton quieto) el boton
     * sigue como estaba. */
    static uint32_t g_mbtn;
    int32_t mhs[2] = { g_mouse, g_mouse2 };
    for (int hi = 0; hi < 2 && p_mRead; hi++) {
        if (mhs[hi] < 0)
            continue;
        /* Se leen TODAS las muestras pendientes (hasta 256): con pocas por lectura la
         * cola se llenaba y el movimiento iba a saltos. */
        for (int i = 0; i < 256; i++) {
            KbmMouseBuf mb;
            kbm_zero(&mb, sizeof(mb));
            int32_t n = p_mRead(mhs[hi], &mb, 1);
            if (n <= 0)
                break;
            /* Un indice sin raton devuelve un registro vacio (todo en cero) en cada lectura:
             * se descarta y se pasa al siguiente handle, sin tocar deltas ni botones. */
            if (mb.m.timestamp == 0 && !mb.m.connected && !mb.m.buttons &&
                !mb.m.xAxis && !mb.m.yAxis && !mb.m.wheel)
                break;
            dx += mb.m.xAxis;
            dy += mb.m.yAxis;
            if (mb.m.xAxis || mb.m.yAxis)
                g_dbg_mmove++;
            if (mb.m.buttons != g_mbtn) {   /* diagnostico: cambios de botones al log */
                static int g_btn_notices;
                if (g_btn_notices < 8) {
                    g_btn_notices++;
                    char m[64], hx[12];
                    static const char h[] = "0123456789abcdef";
                    uint32_t v = mb.m.buttons;
                    int k = 0;
                    for (int sh = 28; sh >= 0; sh -= 4)
                        hx[k++] = h[(v >> sh) & 15];
                    hx[k] = 0;
                    m[0] = 0;
                    kbm_cat(m, sizeof(m), "raton botones=");
                    kbm_cat(m, sizeof(m), hx);
                    note(m);
                }
            }
            g_mbtn = mb.m.buttons;
        }
    }
    uint32_t mbtn = g_mbtn;

    if (!g_enabled)
        return;

    bool k_fwd = key_down(&kd, g_k_fwd), k_back = key_down(&kd, g_k_back);
    bool k_left = key_down(&kd, g_k_left), k_right = key_down(&kd, g_k_right);

    if (g_drive) {
        /* Conducir: W = acelerar (R2), S = frenar / reversa (L2), A y D = volante con
         * rampa suave (el giro completo e instantaneo era demasiado brusco). */
        if (k_fwd)  pad_press(d, KBM_PAD_BUTTON_R2);
        if (k_back) pad_press(d, KBM_PAD_BUTTON_L2);
        float tx = (k_right ? 1.0f : 0.0f) - (k_left ? 1.0f : 0.0f);
        g_steer += (tx * 1.05f - g_steer) * (tx != 0.0f ? 0.35f : 0.55f);
        if (kbm_absf(g_steer) < 0.03f)
            g_steer = 0.0f;
        if (g_steer != 0.0f)
            d->leftStick.x = to_axis(g_steer);
    } else {
        /* Stick izquierdo: adelante / atras / izquierda / derecha */
        float lx = (k_right ? 1.0f : 0.0f) - (k_left ? 1.0f : 0.0f);
        float ly = (k_back ? 1.0f : 0.0f) - (k_fwd ? 1.0f : 0.0f);
        if (lx != 0.0f || ly != 0.0f) {
            if (lx != 0.0f && ly != 0.0f) {   /* diagonal: no pasar de 1.0 */
                lx *= 0.7071f;
                ly *= 0.7071f;
            }
            d->leftStick.x = to_axis(lx);
            d->leftStick.y = to_axis(ly);
        }
    }

    /* Stick derecho = camara: sensibilidad por eje, curva y suavizado configurables */
    float fx = (float)dx * g_sens_x / 127.0f;
    float fy = (float)dy * g_sens_y / 127.0f;
    if (g_invert_y)
        fy = -fy;
    fx *= 1.0f + g_accel * kbm_absf(fx);
    fy *= 1.0f + g_accel * kbm_absf(fy);
    g_rx = g_rx * g_smooth + fx * (1.0f - g_smooth);
    g_ry = g_ry * g_smooth + fy * (1.0f - g_smooth);
    {
        /* El juego ignora el stick por debajo de su zona muerta: sin compensarla, los
         * movimientos lentos del raton se perdian y la camara iba a saltos. */
        float cx = comp_deadzone(g_rx), cy = comp_deadzone(g_ry);
        if (cx != 0.0f || cy != 0.0f) {
            d->rightStick.x = to_axis(cx);
            d->rightStick.y = to_axis(cy);
            g_dbg_stick++;
        }
    }

    /* Botones del raton -> botones del mando (configurable) */
    {
        uint32_t pm = 0;
        if (mbtn & MOUSE_BTN_LEFT)
            pm |= g_mouse_map[0];
        bool right = (mbtn & MOUSE_BTN_RIGHT) != 0;
        if (g_aim_toggle) {   /* un clic activa, otro clic desactiva */
            static bool aim_on, right_prev;
            if (right && !right_prev)
                aim_on = !aim_on;
            right_prev = right;
            if (aim_on)
                pm |= g_mouse_map[1];
        } else if (right) {
            pm |= g_mouse_map[1];
        }
        if (mbtn & MOUSE_BTN_MIDDLE)
            pm |= g_mouse_map[2];
        if (pm)
            pad_press(d, pm);
    }

    /* Teclas -> botones del mando (tabla configurable) */
    for (int i = 0; i < g_nbinds; i++)
        if (key_down(&kd, g_binds[i].code))
            pad_press(d, g_binds[i].mask);
}

/* Rellena un KbmPadData neutro cuando no hay mando fisico. */
static void fake_pad(KbmPadData *d)
{
    kbm_zero(d, sizeof(*d));
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
    if (!seen) { seen = true; L("[kbm_pad] hook scePadReadState: 1a llamada\n"); note("kbm_pad: hook ReadState llamado"); }
    int32_t ret = scePadReadStateExt(handle, data);
    ensure_input();
    if (!g_ready || data == NULL)
        return ret;
    poll_devices();
    if (g_kb < 0 && g_mouse < 0)   /* sin teclado ni raton: el mando no se toca */
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
    if (!seen) { seen = true; L("[kbm_pad] hook scePadRead: 1a llamada\n"); note("kbm_pad: hook Read llamado"); }
    int32_t ret = scePadReadExt(handle, data, count);
    ensure_input();
    if (!g_ready || data == NULL || count < 1)
        return ret;
    poll_devices();
    if (g_kb < 0 && g_mouse < 0)   /* sin teclado ni raton: el mando no se toca */
        return ret;

    if (ret <= 0) {          /* no hay mando real */
        fake_pad(&data[0]);
        ret = 1;
    }
    if (ret > count)
        ret = count;
    apply_kbm(&data[ret - 1]);   /* la muestra mas reciente (consume el raton una sola vez) */
    /* Con varias muestras pendientes, el juego puede quedarse con cualquiera: copiamos la
     * entrada inyectada a todas para no soltar botones ni centrar los sticks entre medias. */
    for (int32_t i = 0; i < ret - 1; i++) {
        data[i].buttons = data[ret - 1].buttons;
        data[i].leftStick = data[ret - 1].leftStick;
        data[i].rightStick = data[ret - 1].rightStick;
        data[i].analogButtons = data[ret - 1].analogButtons;
    }
    return ret;
}
#endif /* KBM_STAGE >= 3 */

#if KBM_STAGE == 2
/* Etapa 2: enganches que solo reenvian a las funciones Ext (como gamepad_helper). */
static int32_t scePadReadState_hook(int32_t handle, KbmPadData *data)
{
    static bool seen;
    if (!seen) { seen = true; note(KBM_NAME ": hook ReadState llamado"); }
    return scePadReadStateExt(handle, data);
}

static int32_t scePadRead_hook(int32_t handle, KbmPadData *data, int32_t count)
{
    static bool seen;
    if (!seen) { seen = true; note(KBM_NAME ": hook Read llamado"); }
    return scePadReadExt(handle, data, count);
}
#endif /* KBM_STAGE == 2 */

/* ---------------------------------------------- INICIO / FIN (idempotentes) */
static bool g_hooked;

static void kbm_start(void)
{
    if (g_hooked)
        return;
    /* Con la linea en [default] de plugins.ini GoldHEN carga el plugin en TODAS las apps,
     * tambien en la app kbm_manager (se reconoce por /app0/Media/jb.prx). Alli no debe
     * enganchar el mando, ni abrir red ni actualizarse: pisaria a la propia app. */
#if KBM_STAGE >= 3
    if (upd_exists("/app0/Media/jb.prx")) {
        lb_s("[kbm_pad] app kbm_manager detectada: el plugin no se activa aqui\n"); lb_flush();
        return;
    }
#endif
    g_hooked = true;
    lb_s("["); lb_s(KBM_NAME); lb_s("] cargado ("); lb_s(kbm_relro_anchor[0]);
    lb_s(", etapa "); lb_d(KBM_STAGE); lb_s(")\n"); lb_flush();
    {
        /* UN solo aviso de arranque con el resultado. */
        char hello[64];
        int err = 0;
#if KBM_STAGE >= 2
        err = install_pad_hooks();
#endif
#if KBM_STAGE >= 3
        early_hide();
#endif
        hello[0] = 0;
        kbm_cat(hello, sizeof(hello), KBM_NAME);
#if KBM_STAGE >= 3
        kbm_cat(hello, sizeof(hello), " v");
        kbm_cat_u(hello, sizeof(hello), KBM_VERSION);
#endif
        if (err == 0) {
            kbm_cat(hello, sizeof(hello), " listo");
        } else {
            kbm_cat(hello, sizeof(hello), ": error al enganchar (");
            kbm_cat_u(hello, sizeof(hello), (uint32_t)err);
            kbm_cat(hello, sizeof(hello), ")");
        }
        notify(hello);
    }
#if KBM_STAGE >= 3
    start_update_thread();
#endif
}

static void kbm_stop(void)
{
    if (!g_hooked)
        return;
    g_hooked = false;
#if KBM_STAGE >= 2
    remove_pad_hooks();
#endif
    lb_s("["); lb_s(KBM_NAME); lb_s("] descargado\n"); lb_flush();
}

/* ------------------------------------------------------ ENTRY POINTS */
/* El plugin_loader.prx de GoldHEN carga el modulo, busca con sceKernelDlsym
 * g_pluginName / plugin_load / plugin_unload y, si faltan plugin_load o
 * plugin_unload, NO da el plugin por cargado. Por eso se exportan con
 * visibilidad por defecto (no static). */
__attribute__((visibility("default"))) const char *g_pluginName = KBM_NAME;
__attribute__((visibility("default"))) const char *g_pluginDesc = "Teclado y raton USB como DualShock 4";
__attribute__((visibility("default"))) const char *g_pluginAuth = "Y4ll";
__attribute__((visibility("default"))) uint32_t g_pluginVersion = 0x00000100; /* 1.00 */

__attribute__((visibility("default")))
int plugin_load(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    lb_s("[kbm_pad] plugin_load llamado ("); lb_s(KBM_BUILD); lb_s(")\n"); lb_flush();
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
    /* Como todos los plugins oficiales de GoldHEN: module_start no hace nada;
     * el trabajo va en plugin_load (lo llama el plugin_loader despues). */
    return 0;
}

__attribute__((visibility("default")))
int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    kbm_stop();
    return 0;
}
