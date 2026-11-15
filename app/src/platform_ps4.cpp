// Implementación para PS4 (OpenOrbis): video, mando y reloj.
#include "platform.h"

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/Http.h>
#include <orbis/Ssl.h>
#include <orbis/Net.h>
#include <orbis/Sysmodule.h>
#include <thread>
#include <stdlib.h>
#include <string.h>

#ifndef ORBIS_PAD_BUTTON_R3
#define ORBIS_PAD_BUTTON_R3 0x00000004
#endif
#ifndef ORBIS_PAD_BUTTON_TOUCH_PAD
#define ORBIS_PAD_BUTTON_TOUCH_PAD 0x00100000
#endif

static int g_video = -1;
static OrbisKernelEqueue g_eq;
static uint32_t *g_fb[2];
static int g_cur = 0;
static int g_frame = 0;
static int g_pad = -1;

// ---------- red / HTTPS (mismos pasos que el ejemplo net_http de OpenOrbis) ----------
#define NET_POOLSIZE (4 * 1024)
static int g_netMem = 0, g_httpCtx = 0, g_sslCtx = 0;
static bool g_httpOk = false;

static int skip_ssl_callback(int, unsigned int, void *const[], int, void *) {
    return 1;  // aceptar el certificado (la PS4 trae autoridades antiguas)
}

static bool https_init() {
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET) < 0) return false;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP) < 0) return false;
    if (sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL) < 0) return false;
    sceNetInit();
    int r = sceNetPoolCreate("kbmNet", NET_POOLSIZE, 0);
    if (r < 0) return false;
    g_netMem = r;
    r = sceSslInit(SSL_POOLSIZE);
    if (r < 0) return false;
    g_sslCtx = r;
    r = sceHttpInit(g_netMem, g_sslCtx, LIBHTTP_POOLSIZE);
    if (r < 0) return false;
    g_httpCtx = r;
    return true;
}

struct HttpIds {
    int tpl = 0, conn = 0, req = 0;
    ~HttpIds() {
        if (req > 0) sceHttpDeleteRequest(req);
        if (conn > 0) sceHttpDeleteConnection(conn);
        if (tpl > 0) sceHttpDeleteTemplate(tpl);
    }
};

int plat_https_get(const char *url, char **out_buf, long *out_len, long max_len,
                   FILE *fp, PlatProgressCb cb, void *ctx, char *err, size_t errcap) {
    if (!g_httpOk) { snprintf(err, errcap, "no se pudo iniciar la red HTTPS"); return -1; }

    HttpIds h;
    h.tpl = sceHttpCreateTemplate(g_httpCtx, "Mozilla/5.0 (PLAYSTATION 4; 1.00)", ORBIS_HTTP_VERSION_1_1, 1);
    if (h.tpl < 0) { snprintf(err, errcap, "HTTP: plantilla 0x%08X", (unsigned)h.tpl); h.tpl = 0; return -1; }
    sceHttpsSetSslCallback(h.tpl, skip_ssl_callback, NULL);
    sceHttpSetResolveTimeOut(h.tpl, 15 * 1000 * 1000);
    sceHttpSetConnectTimeOut(h.tpl, 15 * 1000 * 1000);

    h.conn = sceHttpCreateConnectionWithURL(h.tpl, url, 1);
    if (h.conn < 0) { snprintf(err, errcap, "no se pudo conectar (0x%08X)", (unsigned)h.conn); h.conn = 0; return -1; }
    h.req = sceHttpCreateRequestWithURL(h.conn, ORBIS_METHOD_GET, url, 0);
    if (h.req < 0) { snprintf(err, errcap, "peticion invalida (0x%08X)", (unsigned)h.req); h.req = 0; return -1; }

    int r = sceHttpSendRequest(h.req, NULL, 0);
    if (r < 0) { snprintf(err, errcap, "sin respuesta (0x%08X)", (unsigned)r); return -1; }

    int32_t status = 0;
    if (sceHttpGetStatusCode(h.req, &status) < 0) { snprintf(err, errcap, "respuesta HTTP invalida"); return -1; }
    if (status != 200) { snprintf(err, errcap, "el servidor respondio HTTP %d", (int)status); return status; }

    long total = -1;
    int lenType = 0;
    uint64_t lenVal = 0;
    if (sceHttpGetResponseContentLength(h.req, &lenType, &lenVal) >= 0 && lenType == ORBIS_HTTP_CONTENTLEN_EXIST)
        total = (long)lenVal;

    char *buf = NULL;
    long got = 0;
    if (out_buf) {
        buf = (char *)malloc((size_t)max_len + 1);
        if (!buf) { snprintf(err, errcap, "sin memoria"); return -1; }
    }
    char *chunk = (char *)malloc(64 * 1024);
    if (!chunk) { free(buf); snprintf(err, errcap, "sin memoria"); return -1; }

    bool ok = true;
    long last_cb = 0;
    for (;;) {
        int n = sceHttpReadData(h.req, chunk, 64 * 1024);
        if (n < 0) { snprintf(err, errcap, "conexion interrumpida (0x%08X)", (unsigned)n); ok = false; break; }
        if (n == 0) break;
        if (buf) {
            if (got + n > max_len) { snprintf(err, errcap, "respuesta demasiado grande"); ok = false; break; }
            memcpy(buf + got, chunk, (size_t)n);
        } else if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
            snprintf(err, errcap, "error al escribir el archivo (disco lleno?)");
            ok = false;
            break;
        }
        got += n;
        if (cb && (got - last_cb >= 32768 || (total >= 0 && got >= total))) {
            last_cb = got;
            if (!cb(got, total, ctx)) { snprintf(err, errcap, "descarga cancelada"); ok = false; break; }
        }
    }
    free(chunk);
    if (ok && total >= 0 && got < total) {
        snprintf(err, errcap, "descarga incompleta (%ld de %ld bytes)", got, total);
        ok = false;
    }
    if (!ok) { free(buf); return -1; }
    if (out_buf) { buf[got] = 0; *out_buf = buf; }
    if (out_len) *out_len = got;  // tambien al guardar en archivo: bytes realmente descargados
    return 200;
}

// Salir del sandbox de la app para poder leer /app_tmp (donde Minecraft monta sus datos).
// Mismo metodo que Homebrew Store (LightningMods/PS4-Store, GPLv3): se carga Media/jb.prx y se llama jailbreak_me().
static int g_jb = -100;
static void jailbreak_try() {
    int m = sceKernelLoadStartModule("/app0/Media/jb.prx", 0, NULL, 0, 0, 0);
    if (m < 0) { g_jb = m; return; }
    int (*fn)(void) = NULL;
    if (sceKernelDlsym(m, "jailbreak_me", (void **)&fn) < 0 || !fn) { g_jb = -101; return; }
    g_jb = fn();
}
int plat_jb_status() { return g_jb; }


// ---------- teclado y raton USB (se cargan en tiempo de ejecucion, igual que hace el plugin) ----------
extern "C" const char *sceKernelGetFsSandboxRandomWord(void) __attribute__((weak));

struct UsbKb {
    uint32_t timestamp;
    uint8_t padding[12];
    int32_t unk1;
    int32_t nkeys;
    uint32_t locks;
    uint32_t mods;
    uint16_t keycodes[32];
};
struct UsbMouse {   // mismo formato que usa el plugin kbm_pad
    uint64_t timestamp;
    uint8_t connected;
    uint8_t pad0[3];
    uint32_t buttons;
    int32_t xAxis, yAxis, wheel, tilt;
    uint8_t reserve[8];
};
union UsbMouseBuf { UsbMouse m; uint8_t raw[128]; };

static int (*kbInit)(void) = NULL;
static int (*kbOpen)(int, int, int, void *) = NULL;
static int (*kbRead)(int, void *) = NULL;
static int (*msInit)(void) = NULL;
static int (*msOpen)(int, int, int, void *) = NULL;
static int (*msRead)(int, void *, int) = NULL;
static int g_uid = 0;
static int g_kbh[2] = {-1, -1};
static int g_msh[2] = {-1, -1};
static bool g_usbLoaded = false;
static unsigned char g_zero8[8];
static PlatPointer g_ptr;
static bool g_capture = false;
static int g_keyEvent = 0;
static uint16_t g_prevKeys[64];
static int g_prevN = 0;
static uint32_t g_prevMods = 0;
static int g_wheelQ = 0;          // muescas de rueda pendientes (una por cuadro)
static int g_wheelDir = 0;
static int g_usbRetry = 0;
static bool g_rightPrev = false;

static void *sym_from(const char *name, const char *fn) {
    char path[160];
    int h = -1;
    const char *w = sceKernelGetFsSandboxRandomWord ? sceKernelGetFsSandboxRandomWord() : NULL;
    if (w) {
        snprintf(path, sizeof(path), "/%s/common/lib/%s", w, name);
        h = sceKernelLoadStartModule(path, 0, NULL, 0, 0, 0);
    }
    if (h <= 0) {
        snprintf(path, sizeof(path), "/system/common/lib/%s", name);
        h = sceKernelLoadStartModule(path, 0, NULL, 0, 0, 0);
    }
    if (h <= 0) return NULL;
    void *f = NULL;
    if (sceKernelDlsym(h, fn, &f) < 0) return NULL;
    return f;
}

static void usb_load() {
    if (g_usbLoaded) return;
    g_usbLoaded = true;
    sceSysmoduleLoadModule(0x0106);   // teclado
    sceSysmoduleLoadModule(0x00A9);   // raton
    kbInit = (int (*)(void))sym_from("libSceKeyboard.sprx", "sceKeyboardInit");
    kbOpen = (int (*)(int, int, int, void *))sym_from("libSceKeyboard.sprx", "sceKeyboardOpen");
    kbRead = (int (*)(int, void *))sym_from("libSceKeyboard.sprx", "sceKeyboardReadState");
    msInit = (int (*)(void))sym_from("libSceMouse.sprx", "sceMouseInit");
    msOpen = (int (*)(int, int, int, void *))sym_from("libSceMouse.sprx", "sceMouseOpen");
    msRead = (int (*)(int, void *, int))sym_from("libSceMouse.sprx", "sceMouseRead");
    if (!kbInit || !kbOpen || !kbRead) kbRead = NULL;
    if (!msInit || !msOpen || !msRead) msRead = NULL;
}

static void usb_open() {
    if (kbRead && g_kbh[0] < 0) {
        kbInit();
        static const int tries[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};   // {indice, con_params}
        int nf = 0;
        for (int k = 0; k < 4 && nf < 2; k++) {
            int h = kbOpen(g_uid, 0, tries[k][0], tries[k][1] ? (void *)g_zero8 : NULL);
            if (h < 0) continue;
            if (nf == 1 && h == g_kbh[0]) continue;
            g_kbh[nf++] = h;
        }
    }
    if (msRead && g_msh[0] < 0) {
        msInit();
        for (int idx = 0; idx < 2; idx++) {
            int h = msOpen(g_uid, 0, idx, g_zero8);
            if (h < 0) continue;
            if (g_msh[0] < 0) g_msh[0] = h; else if (g_msh[1] < 0) g_msh[1] = h;
        }
        if (g_msh[0] < 0) g_msh[0] = msOpen(g_uid, 0, 0, NULL);
    }
}

int plat_devices() { return (g_kbh[0] >= 0 ? 1 : 0) | (g_msh[0] >= 0 ? 2 : 0); }
const PlatPointer *plat_pointer() { return &g_ptr; }
void plat_capture(bool on) { g_capture = on; g_keyEvent = 0; }
int plat_key_event() { int k = g_keyEvent; g_keyEvent = 0; return k; }

static bool kd_has(const uint16_t *k, int n, int code) {
    for (int i = 0; i < n; i++) if (k[i] == code) return true;
    return false;
}

// Lee teclado y raton; devuelve los BTN_* que generan.
static uint32_t usb_poll() {
    if (!g_usbLoaded) return 0;
    if (g_kbh[0] < 0 && g_msh[0] < 0 && (g_usbRetry++ % 120) == 0) usb_open();   // por si se conectan despues
    else if ((g_kbh[0] < 0 || g_msh[0] < 0) && (g_usbRetry++ % 240) == 0) usb_open();

    uint32_t m = 0;
    // ---- teclado ----
    uint16_t keys[64];
    int n = 0;
    uint32_t mods = 0;
    for (int i = 0; i < 2; i++) {
        if (g_kbh[i] < 0 || !kbRead) continue;
        UsbKb kd;
        memset(&kd, 0, sizeof(kd));
        if (kbRead(g_kbh[i], &kd) < 0) continue;
        mods |= kd.mods;
        for (int k = 0; k < kd.nkeys && k < 32 && n < 64; k++)
            if (kd.keycodes[k]) keys[n++] = kd.keycodes[k];
    }
    bool shift = (mods & 0x22) != 0;
    // eventos de pulsacion (para capturar teclas)
    int ev = 0;
    for (int i = 0; i < n && !ev; i++) if (!kd_has(g_prevKeys, g_prevN, keys[i])) ev = keys[i];
    if (!ev && n == 0) {   // modificador solo: se acepta al pulsarlo (sin otra tecla)
        uint32_t nw = mods & ~g_prevMods;
        if (nw & 0x22) ev = 0x122; else if (nw & 0x11) ev = 0x111; else if (nw & 0x44) ev = 0x144;
    }
    if (ev) g_keyEvent = ev;
    if (!g_capture) {
        for (int i = 0; i < n; i++) {
            switch (keys[i]) {
            case 82: m |= BTN_UP; break;
            case 81: m |= BTN_DOWN; break;
            case 80: m |= BTN_LEFT; break;
            case 79: m |= BTN_RIGHT; break;
            case 40: case 44: case 88: case 12: m |= BTN_CROSS; break;      // Enter, Espacio, Enter num, I
            case 41: case 20: m |= BTN_CIRCLE; break;                        // Esc, Q
            case 42: case 76: case 4: m |= BTN_SQUARE; break;                // Retroceso, Supr, A
            case 24: case 21: m |= BTN_TRIANGLE; break;                      // U, R
            case 18: case 58: m |= BTN_OPTIONS; break;                       // O, F1
            case 78: m |= BTN_R1; break;                                     // AvPag
            case 75: m |= BTN_L1; break;                                     // RePag
            case 43: m |= shift ? BTN_L1 : BTN_R1; break;                    // Tab / Shift+Tab
            }
        }
    }
    for (int i = 0; i < n; i++) g_prevKeys[i] = keys[i];
    g_prevN = n;
    g_prevMods = mods;

    // ---- raton ----
    int dx = 0, dy = 0, wheel = 0;
    uint32_t btn = 0;
    bool got = false;
    for (int h = 0; h < 2 && msRead; h++) {
        if (g_msh[h] < 0) continue;
        for (int i = 0; i < 256; i++) {
            UsbMouseBuf mb;
            memset(&mb, 0, sizeof(mb));
            if (msRead(g_msh[h], &mb, 1) <= 0) break;
            if (mb.m.timestamp == 0 && !mb.m.connected && !mb.m.buttons && !mb.m.xAxis && !mb.m.yAxis && !mb.m.wheel) break;
            dx += mb.m.xAxis; dy += mb.m.yAxis; wheel += mb.m.wheel;
            btn = mb.m.buttons;
            got = true;
        }
    }
    if (got) {
        if (dx || dy) g_ptr.active = true;
        g_ptr.x += dx * 2; g_ptr.y += dy * 2;
        if (g_ptr.x < 0) g_ptr.x = 0;
        if (g_ptr.y < 0) g_ptr.y = 0;
        if (g_ptr.x > SCREEN_W - 1) g_ptr.x = SCREEN_W - 1;
        if (g_ptr.y > SCREEN_H - 1) g_ptr.y = SCREEN_H - 1;
        bool l = (btn & 1) != 0;
        g_ptr.leftPressed = l && !g_ptr.left;
        g_ptr.leftReleased = !l && g_ptr.left;
        g_ptr.left = l;
        bool r = (btn & 2) != 0;
        if (r && !g_rightPrev) m |= BTN_CIRCLE;   // clic derecho = volver
        g_rightPrev = r;
        if (wheel) { g_wheelDir = wheel > 0 ? 1 : -1; g_wheelQ += wheel > 0 ? wheel : -wheel; if (g_wheelQ > 6) g_wheelQ = 6; }
        if (l || r) g_ptr.active = true;
    } else {
        g_ptr.leftPressed = g_ptr.leftReleased = false;
    }
    if (g_wheelQ > 0) {   // una muesca por cuadro, con un cuadro libre entre muescas
        static bool gap = false;
        if (!gap) { m |= g_wheelDir > 0 ? BTN_UP : BTN_DOWN; g_wheelQ--; }
        gap = !gap;
    }
    return m;
}

bool plat_init() {
    jailbreak_try();  // si falla no es fatal: la app sigue, pero no podra instalar en Minecraft
    g_video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0);
    if (g_video < 0) return false;

    if (sceKernelCreateEqueue(&g_eq, "kbm flip") < 0) return false;
    sceVideoOutAddFlipEvent(g_eq, g_video, 0);

    // Memoria de video: 2 buffers de 1920x1080x4 (~16,6 MB) dentro de 32 MB alineados a 2 MB.
    const size_t fbSize = (size_t)SCREEN_W * SCREEN_H * 4;
    const size_t total = 0x2000000;
    const size_t align = 0x200000;
    off_t off = 0;
    if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), total, align, 3, &off) < 0) return false;
    void *mem = NULL;
    if (sceKernelMapDirectMemory(&mem, total, 0x33, 0, off, align) < 0) return false;

    g_fb[0] = (uint32_t *)mem;
    g_fb[1] = (uint32_t *)((char *)mem + fbSize);

    OrbisVideoOutBufferAttribute attr;
    sceVideoOutSetBufferAttribute(&attr, 0x80000000, 1, 0, SCREEN_W, SCREEN_H, SCREEN_W);
    void *addrs[2] = {g_fb[0], g_fb[1]};
    if (sceVideoOutRegisterBuffers(g_video, 0, addrs, 2, &attr) != 0) return false;
    sceVideoOutSetFlipRate(g_video, 0);

    g_httpOk = https_init();  // si falla, solo el modo GitHub queda sin red

    // Mando del usuario principal.
    if (scePadInit() != 0) return false;
    OrbisUserServiceInitializeParams p;
    p.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
    sceUserServiceInitialize(&p);
    int32_t uid = 0;
    sceUserServiceGetInitialUser(&uid);
    g_uid = uid;
    g_pad = scePadOpen(uid, 0, 0, NULL);
    g_ptr.x = SCREEN_W / 2;
    g_ptr.y = SCREEN_H / 2;
    usb_load();   // teclado y raton: si falla, la app sigue solo con el mando
    usb_open();
    return g_pad >= 0 || plat_devices() != 0;
}

uint32_t plat_buttons() {
    uint32_t usb = usb_poll();
    OrbisPadData d;
    if (g_pad < 0 || scePadReadState(g_pad, &d) != 0) return usb;
    uint32_t b = d.buttons, m = usb;
    if (b & ORBIS_PAD_BUTTON_UP) m |= BTN_UP;
    if (b & ORBIS_PAD_BUTTON_DOWN) m |= BTN_DOWN;
    if (b & ORBIS_PAD_BUTTON_LEFT) m |= BTN_LEFT;
    if (b & ORBIS_PAD_BUTTON_RIGHT) m |= BTN_RIGHT;
    if (b & ORBIS_PAD_BUTTON_CROSS) m |= BTN_CROSS;
    if (b & ORBIS_PAD_BUTTON_CIRCLE) m |= BTN_CIRCLE;
    if (b & ORBIS_PAD_BUTTON_SQUARE) m |= BTN_SQUARE;
    if (b & ORBIS_PAD_BUTTON_TRIANGLE) m |= BTN_TRIANGLE;
    if (b & ORBIS_PAD_BUTTON_L1) m |= BTN_L1;
    if (b & ORBIS_PAD_BUTTON_R1) m |= BTN_R1;
    if (b & ORBIS_PAD_BUTTON_OPTIONS) m |= BTN_OPTIONS;
    if (b & ORBIS_PAD_BUTTON_R2) m |= BTN_R2;
    if (b & ORBIS_PAD_BUTTON_L2) m |= BTN_L2;
    if (b & ORBIS_PAD_BUTTON_L3) m |= BTN_L3;
    if (b & ORBIS_PAD_BUTTON_R3) m |= BTN_R3;
    if (b & ORBIS_PAD_BUTTON_TOUCH_PAD) m |= BTN_TOUCH;
    // stick izquierdo como cruceta (zona muerta amplia para que no se mueva solo)
    if (d.leftStick.y < 50) m |= BTN_UP;
    if (d.leftStick.y > 205) m |= BTN_DOWN;
    if (d.leftStick.x < 50) m |= BTN_LEFT;
    if (d.leftStick.x > 205) m |= BTN_RIGHT;
    return m;
}

uint32_t *plat_framebuffer() { return g_fb[g_cur]; }

void plat_present() {
    sceVideoOutSubmitFlip(g_video, g_cur, ORBIS_VIDEO_OUT_FLIP_VSYNC, g_frame);
    for (;;) {
        OrbisVideoOutFlipStatus st;
        sceVideoOutGetFlipStatus(g_video, &st);
        if (st.flipArg == g_frame) break;
        OrbisKernelEvent evt;
        int count;
        if (sceKernelWaitEqueue(g_eq, &evt, 1, &count, 0) != 0) break;
    }
    g_frame++;
    g_cur ^= 1;
}

void plat_sleep_ms(int ms) { sceKernelUsleep((uint32_t)ms * 1000u); }
uint64_t plat_ms() { return sceKernelGetProcessTime() / 1000u; }
const char *plat_data_dir() { return "/data/GoldHEN"; }
const char *plat_apptmp_dir() { return "/app_tmp"; }

bool plat_stat(const char *path, bool *isDir, long *size) {
    OrbisKernelStat st;
    if (sceKernelStat(path, &st) < 0) return false;
    *isDir = (st.st_mode & 0170000) == 0040000;
    *size = (long)st.st_size;
    return true;
}

void plat_chmod(const char *path) { sceKernelChmod(path, 0777); }
int plat_mkdir(const char *path) { return sceKernelMkdir(path, 0777) < 0 ? -1 : 0; }
int plat_remove(const char *path) { return sceKernelUnlink(path) < 0 ? -1 : 0; }
int plat_rename(const char *from, const char *to) { return sceKernelRename(from, to) < 0 ? -1 : 0; }

// Listado de carpetas con las llamadas del kernel (la PS4 es FreeBSD 9: struct dirent antigua).
bool plat_listdir(const char *path, PlatDirCb cb, void *ctx) {
    int fd = sceKernelOpen(path, 0x00020000 /* O_RDONLY | O_DIRECTORY */, 0);
    if (fd < 0) return false;
    char *buf = (char *)malloc(8192);
    if (!buf) { sceKernelClose(fd); return false; }
    for (;;) {
        int n = sceKernelGetdents(fd, buf, 8192);
        if (n <= 0) break;
        int off = 0;
        while (off + 8 <= n) {
            // d_fileno (4) | d_reclen (2) | d_type (1) | d_namlen (1) | d_name
            unsigned short reclen = *(unsigned short *)(buf + off + 4);
            unsigned char namlen = *(unsigned char *)(buf + off + 7);
            if (reclen < 8) break;
            char name[256];
            if (namlen > 255) namlen = 255;
            memcpy(name, buf + off + 8, namlen);
            name[namlen] = 0;
            if (strcmp(name, ".") && strcmp(name, "..")) cb(name, ctx);
            off += reclen;
        }
    }
    free(buf);
    sceKernelClose(fd);
    return true;
}


// Sin audio ni juego en segundo plano en esta app.
void plat_audio_start(bool (*)(int16_t *, int)) {}
bool plat_game_in_background() { return false; }
int plat_resume_game(const char *) { return -1; }
int plat_big_app_id() { return 0; }
