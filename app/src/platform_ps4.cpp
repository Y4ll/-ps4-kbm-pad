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
    g_pad = scePadOpen(uid, 0, 0, NULL);
    return g_pad >= 0;
}

uint32_t plat_buttons() {
    OrbisPadData d;
    if (g_pad < 0 || scePadReadState(g_pad, &d) != 0) return 0;
    uint32_t b = d.buttons, m = 0;
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
