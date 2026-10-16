/*
 * kbm_probe - plugin GoldHEN minimo de diagnostico (v2).
 *
 * Solo depende de libkernel. En module_start y plugin_load:
 *   1) muestra un aviso en pantalla (si sale, el plugin SI se cargo);
 *   2) escribe una linea en varios archivos, por si el juego no puede escribir
 *      en alguna de las carpetas (si aparece alguno, el plugin SI se cargo).
 * Si no sale ninguna de las dos cosas, el plugin no se esta ejecutando.
 */
#include <stdint.h>
#include <stddef.h>

#define PROBE_BUILD "probe-v2"

extern int32_t probe_open(const char *path, int32_t flags, int32_t mode)
    __asm__("sceKernelOpen");
extern int64_t probe_write(int32_t fd, const void *buf, size_t n)
    __asm__("sceKernelWrite");
extern int32_t probe_close(int32_t fd)
    __asm__("sceKernelClose");

/* Misma estructura de aviso que kbm_pad (3120 bytes, target_id = -1). */
typedef struct ProbeNotify {
    int32_t type, req_id, priority, msg_id, target_id, user_id;
    int32_t unk1, unk2, app_id, error_num, unk3;
    uint8_t use_icon_image_uri;
    char    message[1024];
    char    icon_image_uri[1024];
    char    unk[1024];
} ProbeNotify;

_Static_assert(sizeof(ProbeNotify) == 3120, "ProbeNotify debe medir 3120");

extern int32_t probe_send_notification(int32_t device, ProbeNotify *req,
                                       size_t size, int32_t blocking)
    __asm__("sceKernelSendNotificationRequest");

static size_t probe_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void probe_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    while (src[i] && i + 1 < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
}

static void probe_notify(const char *msg)
{
    ProbeNotify req;
    unsigned char *p = (unsigned char *)&req;
    for (size_t i = 0; i < sizeof(req); i++)
        p[i] = 0;
    req.type = 0;
    req.target_id = -1;
    req.use_icon_image_uri = 1;
    probe_copy(req.message, sizeof(req.message), msg);
    probe_copy(req.icon_image_uri, sizeof(req.icon_image_uri),
               "cxml://psnotification/tex_icon_system");
    probe_send_notification(0, &req, sizeof(req), 0);
}

static const char *const PATHS[] = {
    "/data/GoldHEN/kbm_probe.log",
    "/data/kbm_probe.log",
    "/user/data/kbm_probe.log",
    "/mnt/usb0/kbm_probe.log",
    "/mnt/usb1/kbm_probe.log",
};

static void probe_log(const char *msg)
{
    size_t n = probe_len(msg);
    for (size_t k = 0; k < sizeof(PATHS) / sizeof(PATHS[0]); k++) {
        /* O_WRONLY=1, O_APPEND=8, O_CREAT=0x200 */
        int32_t fd = probe_open(PATHS[k], 0x0001 | 0x0008 | 0x0200, 0666);
        if (fd >= 0) {
            probe_write(fd, msg, n);
            probe_close(fd);
        }
    }
}

__attribute__((visibility("default"))) const char *g_pluginName = "kbm_probe";

__attribute__((visibility("default")))
int plugin_load(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    probe_log("[kbm_probe " PROBE_BUILD "] plugin_load llamado\n");
    probe_notify("kbm_probe: plugin_load OK");
    return 0;
}

__attribute__((visibility("default")))
int plugin_unload(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    probe_log("[kbm_probe " PROBE_BUILD "] plugin_unload llamado\n");
    return 0;
}

__attribute__((visibility("default")))
int module_start(size_t argc, const void *args)
{
    (void)argc; (void)args;
    probe_log("[kbm_probe " PROBE_BUILD "] module_start llamado\n");
    probe_notify("kbm_probe: module_start OK");
    return 0;
}

__attribute__((visibility("default")))
int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    probe_log("[kbm_probe " PROBE_BUILD "] module_stop llamado\n");
    return 0;
}
