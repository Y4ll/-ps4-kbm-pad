// Capa de plataforma: lo único que cambia entre la PS4 y las pruebas en PC.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define SCREEN_W 1920
#define SCREEN_H 1080

enum {
    BTN_UP = 1 << 0,
    BTN_DOWN = 1 << 1,
    BTN_LEFT = 1 << 2,
    BTN_RIGHT = 1 << 3,
    BTN_CROSS = 1 << 4,
    BTN_CIRCLE = 1 << 5,
    BTN_SQUARE = 1 << 6,
    BTN_TRIANGLE = 1 << 7,
    BTN_L1 = 1 << 8,
    BTN_R1 = 1 << 9,
    BTN_OPTIONS = 1 << 10,
    BTN_R2 = 1 << 11,
    BTN_L2 = 1 << 12,
    BTN_L3 = 1 << 13,
    BTN_R3 = 1 << 14,
    BTN_TOUCH = 1 << 15,
};

bool plat_init();
uint32_t plat_buttons();          // botones mantenidos ahora mismo
uint32_t *plat_framebuffer();     // buffer de dibujo actual, SCREEN_W*SCREEN_H, 0x80RRGGBB
void plat_present();              // muestra el buffer y espera al vsync
void plat_sleep_ms(int ms);
// Audio: arranca un hilo que llama a render(buf,n) (n muestras S16 mono 48 kHz). render devuelve false si no hay nada que sonar.
void plat_audio_start(bool (*render)(int16_t *, int));
uint64_t plat_ms();               // reloj monotónico en milisegundos
const char *plat_data_dir();      // carpeta de datos de la app (config y descargas)
const char *plat_apptmp_dir();    // carpeta donde Minecraft monta sus datos mientras esta abierto (/app_tmp)

// Existe la ruta? Si existe, dice si es carpeta y su tamano en bytes.
bool plat_stat(const char *path, bool *isDir, long *size);

// Lista los nombres de una carpeta (sin "." ni ".."). Devuelve false si no se pudo abrir.
typedef void (*PlatDirCb)(const char *name, void *ctx);
bool plat_listdir(const char *path, PlatDirCb cb, void *ctx);

// HTTPS (en la PS4 usa libSceHttp). Mismo contrato que http_get de net.h:
// modo memoria (out_buf) o modo archivo (fp). Devuelve el codigo HTTP o -1 con texto en err.
typedef bool (*PlatProgressCb)(long done, long total, void *ctx);
int plat_https_get(const char *url, char **out_buf, long *out_len, long max_len,
                   FILE *fp, PlatProgressCb cb, void *ctx, char *err, size_t errcap);

// Control de la app de fondo (Minecraft en suspension).
// plat_game_in_background(): hay un juego abierto/suspendido? (en PC de prueba: MCSTORE_BG=1)
// plat_resume_game(cusa): trae ese juego de vuelta al frente (sceLncUtilLaunchApp). Devuelve el codigo (0 = ok).
bool plat_game_in_background();
int plat_resume_game(const char *cusa);
int plat_big_app_id();  // AppId del juego en segundo plano (<= 0: ninguno), para diagnostico
int plat_jb_status();  // resultado del jailbreak de la app (0 = ok, otro = error), para diagnostico
