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
#define ORBIS_KEYBOARD_MOD_LEFT_ALT    4
#define ORBIS_KEYBOARD_MOD_RIGHT_ALT   64
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
#define K_ESC   41
#define K_1     30
#define K_2     31
#define K_3     32
#define K_4     33
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
#define MOUSE_BTN_MIDDLE 0x4   /* supuesto: bit2 = rueda pulsada */
#define MOUSE_BTN_BACK   0x8   /* supuesto: lateral 1 */
#define MOUSE_BTN_FWD    0x10  /* supuesto: lateral 2 */

#define KBM_BUILD "build-ext-1"   /* marca para saber que .prx esta cargado */

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
    kbm_zero(&req, sizeof(req));
    req.type = 0;
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    kbm_copy(req.message, sizeof(req.message), msg);
    kbm_copy(req.icon_image_uri, sizeof(req.icon_image_uri),
             "cxml://psnotification/tex_icon_system");
    int32_t r = kbm_send_notification(0, &req, sizeof(req), 0);
    lb_s("[kbm_pad] aviso \""); lb_s(msg); lb_s("\" -> "); lb_d(r); lb_s("\n"); lb_flush();
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
static bool install_pad_hooks(void)
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
    notify(msg);
    msg[0] = 0;
    kbm_cat(msg, sizeof(msg), KBM_NAME ": State ");
    kbm_hex(msg, sizeof(msg), st, 6);
    kbm_cat(msg, sizeof(msg), " | Ext ");
    kbm_hex(msg, sizeof(msg), stx, 6);
    notify(msg);

    if (rd[0] == 0xE9 || st[0] == 0xE9) {
        /* HOOK32 seguiria ese salto y engancharia scePadReadExt: recursion infinita */
        notify(KBM_NAME ": scePadRead empieza con jmp, no se engancha");
        return false;
    }
    if (rdx[0] != 0xB9 || stx[0] != 0xBA) {
        notify(KBM_NAME ": scePadReadExt distinto al esperado, no se engancha");
        return false;
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
    return true;
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
        notify("kbm_pad: auto-actualizacion desactivada tras un fallo");
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

static void ensure_input(void)
{
    if (g_ready || g_failed)
        return;

    notify("kbm_pad: paso 1 (iniciando entrada)");
    int32_t user = 0;
    sceUserServiceGetInitialUser(&user);
    notify("kbm_pad: paso 2 (usuario OK)");

    /* Los sysmodule IDs hacen que el sistema cargue las librerias; despues se
     * resuelven por ruta y dlsym. */
    sceSysmoduleLoadModule(KBM_SYSMODULE_KEYBOARD);
    sceSysmoduleLoadModule(KBM_SYSMODULE_MOUSE);
    notify("kbm_pad: paso 3 (sysmodule OK)");

    if (!resolve_input_libs()) {
        L("[kbm_pad] no se pudo resolver teclado ni raton\n");
        notify("kbm_pad: no se pudo cargar teclado/raton");
        g_failed = true;
        return;
    }

    notify("kbm_pad: paso 4 (librerias resueltas)");
    if (p_kbInit && p_kbOpen) {
        p_kbInit();
        g_kb = p_kbOpen(user, 0, 0, NULL);
    }
    if (p_mInit && p_mOpen) {
        p_mInit();
        g_mouse = p_mOpen(user, 0, 0, NULL);
    }

    lb_s("[kbm_pad] user="); lb_d(user); lb_s(" keyboard="); lb_d(g_kb);
    lb_s(" mouse="); lb_d(g_mouse); lb_s("\n"); lb_flush();

    /* Se ejecuta en la primera llamada al hook: si este aviso sale, el hook
     * del mando SI se esta disparando. */
    char msg[96];
    msg[0] = 0;
    kbm_cat(msg, sizeof(msg), "kbm_pad: teclado=");
    kbm_cat(msg, sizeof(msg), g_kb >= 0 ? "OK" : "FALLO");
    kbm_cat(msg, sizeof(msg), ", raton=");
    kbm_cat(msg, sizeof(msg), g_mouse >= 0 ? "OK" : "FALLO");
    notify(msg);

    if (g_kb < 0 && g_mouse < 0) {
        L("[kbm_pad] no se abrio ni teclado ni raton\n");
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
    kbm_zero(&kd, sizeof(kd));
    if (g_kb >= 0 && p_kbRead && p_kbRead(g_kb, &kd) < 0)
        kbm_zero(&kd, sizeof(kd));

    /* Toggle con F8 (flanco de subida) */
    bool t = has_key(&kd, TOGGLE_KEYCODE);
    if (t && !g_toggle_down) {
        g_enabled = !g_enabled;
        L(g_enabled ? "[kbm_pad] ACTIVO\n" : "[kbm_pad] PAUSA\n");
        notify(g_enabled ? "kbm_pad ACTIVO" : "kbm_pad PAUSA");
    }
    g_toggle_down = t;

    /* Raton: acumular deltas aunque este en pausa, para no acumular salto */
    int32_t dx = 0, dy = 0, dw = 0;
    /* Estado de botones PERSISTENTE: sin muestras nuevas (raton quieto) el boton
     * sigue como estaba; antes se ponia a 0 y la mira (L2) se soltaba sola. */
    static uint32_t g_mbtn;
    uint32_t mbtn;
    if (g_mouse >= 0 && p_mRead) {
        for (int i = 0; i < 8; i++) {
            KbmMouseBuf mb;
            kbm_zero(&mb, sizeof(mb));
            int32_t n = p_mRead(g_mouse, &mb, 1);
            if (n <= 0)
                break;
            dx += mb.m.xAxis;
            dy += mb.m.yAxis;
            dw += mb.m.wheel;
            if (mb.m.buttons != g_mbtn) {   /* diagnostico: aviso en cada cambio de botones */
                static int g_btn_notices;
                if (g_btn_notices < 16) {
                    g_btn_notices++;
                    char m[64];
                    m[0] = 0;
                    kbm_cat(m, sizeof(m), "raton botones=");
                    lb_s(""); /* mantiene el constructor de linea vacio */
                    char hx[12];
                    static const char h[] = "0123456789abcdef";
                    uint32_t v = mb.m.buttons;
                    int k = 0;
                    for (int sh = 28; sh >= 0; sh -= 4)
                        hx[k++] = h[(v >> sh) & 15];
                    hx[k] = 0;
                    kbm_cat(m, sizeof(m), hx);
                    notify(m);
                }
            }
            g_mbtn = mb.m.buttons;
        }
    }
    mbtn = g_mbtn;

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
#if AIM_TOGGLE
    {   /* un clic activa la mira, otro clic la quita (no hace falta mantener) */
        static bool g_aim_on, g_right_prev;
        bool right = (mbtn & MOUSE_BTN_RIGHT) != 0;
        if (right && !g_right_prev)
            g_aim_on = !g_aim_on;
        g_right_prev = right;
        if (g_aim_on) { d->buttons |= KBM_PAD_BUTTON_L2; d->analogButtons.l2 = 255; }
    }
#else
    if (mbtn & MOUSE_BTN_RIGHT) { d->buttons |= KBM_PAD_BUTTON_L2; d->analogButtons.l2 = 255; }
#endif

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

    /* Extras: 1-4 = cruceta (arriba, derecha, abajo, izquierda), Esc = Options, Alt = circulo */
    if (has_key(&kd, K_1)) d->buttons |= KBM_PAD_BUTTON_UP;
    if (has_key(&kd, K_2)) d->buttons |= KBM_PAD_BUTTON_RIGHT;
    if (has_key(&kd, K_3)) d->buttons |= KBM_PAD_BUTTON_DOWN;
    if (has_key(&kd, K_4)) d->buttons |= KBM_PAD_BUTTON_LEFT;
    if (has_key(&kd, K_ESC)) d->buttons |= KBM_PAD_BUTTON_OPTIONS;
    if (kd.mods & (ORBIS_KEYBOARD_MOD_LEFT_ALT | ORBIS_KEYBOARD_MOD_RIGHT_ALT))
        d->buttons |= KBM_PAD_BUTTON_CIRCLE;

    /* Raton extra: rueda = L1 (arriba) / R1 (abajo) en pulsos cortos; clic central = R3;
     * lateral 1 = L3; lateral 2 = circulo. */
    {
        static int g_wheel_l1, g_wheel_r1;
        if (dw > 0) g_wheel_l1 = 4;
        if (dw < 0) g_wheel_r1 = 4;
        if (g_wheel_l1 > 0) { g_wheel_l1--; d->buttons |= KBM_PAD_BUTTON_L1; }
        if (g_wheel_r1 > 0) { g_wheel_r1--; d->buttons |= KBM_PAD_BUTTON_R1; }
        if (mbtn & MOUSE_BTN_MIDDLE) d->buttons |= KBM_PAD_BUTTON_R3;
        if (mbtn & MOUSE_BTN_BACK)   d->buttons |= KBM_PAD_BUTTON_L3;
        if (mbtn & MOUSE_BTN_FWD)    d->buttons |= KBM_PAD_BUTTON_CIRCLE;
    }
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
    if (!seen) { seen = true; L("[kbm_pad] hook scePadReadState: 1a llamada\n"); notify("kbm_pad: hook ReadState llamado"); }
    int32_t ret = scePadReadStateExt(handle, data);
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
    if (!seen) { seen = true; L("[kbm_pad] hook scePadRead: 1a llamada\n"); notify("kbm_pad: hook Read llamado"); }
    int32_t ret = scePadReadExt(handle, data, count);
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
#endif /* KBM_STAGE >= 3 */

#if KBM_STAGE == 2
/* Etapa 2: enganches que solo reenvian a las funciones Ext (como gamepad_helper). */
static int32_t scePadReadState_hook(int32_t handle, KbmPadData *data)
{
    static bool seen;
    if (!seen) { seen = true; notify(KBM_NAME ": hook ReadState llamado"); }
    return scePadReadStateExt(handle, data);
}

static int32_t scePadRead_hook(int32_t handle, KbmPadData *data, int32_t count)
{
    static bool seen;
    if (!seen) { seen = true; notify(KBM_NAME ": hook Read llamado"); }
    return scePadReadExt(handle, data, count);
}
#endif /* KBM_STAGE == 2 */

/* ---------------------------------------------- INICIO / FIN (idempotentes) */
static bool g_hooked;

static void kbm_start(void)
{
    if (g_hooked)
        return;
    g_hooked = true;
    lb_s("["); lb_s(KBM_NAME); lb_s("] cargado ("); lb_s(kbm_relro_anchor[0]);
    lb_s(", etapa "); lb_d(KBM_STAGE); lb_s(")\n"); lb_flush();
#if KBM_STAGE >= 3
    {
        char hello[48];
        hello[0] = 0;
        kbm_cat(hello, sizeof(hello), KBM_NAME " cargado v");
        kbm_cat_u(hello, sizeof(hello), KBM_VERSION);
        notify(hello);
    }
#else
    notify(KBM_NAME " cargado");
#endif
#if KBM_STAGE >= 2
    if (install_pad_hooks())
        notify(KBM_NAME ": enganches listos");
#endif
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
