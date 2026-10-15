/*
 * kbm_probe - plugin GoldHEN minimo de diagnostico.
 *
 * Solo depende de libkernel. Escribe /data/GoldHEN/kbm_probe.log en
 * module_start y en plugin_load. Sirve para saber si el plugin_loader de
 * GoldHEN llega a ejecutar un .prx construido con este toolchain, separado de
 * las dependencias (teclado, raton, pad...) del plugin completo kbm_pad.
 */
#include <stdint.h>
#include <stddef.h>

extern int32_t probe_open(const char *path, int32_t flags, int32_t mode)
    __asm__("sceKernelOpen");
extern int64_t probe_write(int32_t fd, const void *buf, size_t n)
    __asm__("sceKernelWrite");
extern int32_t probe_close(int32_t fd)
    __asm__("sceKernelClose");

static void probe_log(const char *msg)
{
    size_t n = 0;
    while (msg[n])
        n++;
    /* O_WRONLY=1, O_APPEND=8, O_CREAT=0x200 */
    int32_t fd = probe_open("/data/GoldHEN/kbm_probe.log", 0x0001 | 0x0008 | 0x0200, 0666);
    if (fd >= 0) {
        probe_write(fd, msg, n);
        probe_close(fd);
    }
}

__attribute__((visibility("default"))) const char *g_pluginName = "kbm_probe";

__attribute__((visibility("default")))
int plugin_load(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    probe_log("[kbm_probe] plugin_load llamado\n");
    return 0;
}

__attribute__((visibility("default")))
int plugin_unload(int argc, const char *argv[])
{
    (void)argc; (void)argv;
    probe_log("[kbm_probe] plugin_unload llamado\n");
    return 0;
}

__attribute__((visibility("default")))
int module_start(size_t argc, const void *args)
{
    (void)argc; (void)args;
    probe_log("[kbm_probe] module_start llamado\n");
    return 0;
}

__attribute__((visibility("default")))
int module_stop(size_t argc, const void *args)
{
    (void)argc; (void)args;
    probe_log("[kbm_probe] module_stop llamado\n");
    return 0;
}
