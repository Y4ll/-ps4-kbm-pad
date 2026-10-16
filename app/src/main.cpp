// kbm_manager: instala, actualiza, activa y desactiva el plugin kbm_pad (teclado + raton = mando).
// Base de pantalla/mando/HTTPS tomada de McStore.
#include "platform.h"
#include "font.h"
#include <stdlib.h>
#include <string.h>

#ifndef APP_VERSION
#define APP_VERSION "1.00"
#endif

#define DIR_GOLDHEN  "/data/GoldHEN"
#define DIR_PLUGINS  "/data/GoldHEN/plugins"
#define PRX_PATH     "/data/GoldHEN/plugins/kbm_pad.prx"
#define PRX_TMP      "/data/GoldHEN/plugins/kbm_pad.prx.tmp"
#define PLUGINS_INI  "/data/GoldHEN/plugins.ini"
#define KBM_INI      "/data/GoldHEN/kbm_pad.ini"
#define KBM_STATUS   "/data/GoldHEN/kbm_pad.status"
#define REPO         "Y4ll/-ps4-kbm-pad"
#define URL_VERSION  "https://raw.githubusercontent.com/" REPO "/main/VERSION"
#define URL_RELEASE  "https://github.com/" REPO "/releases/download/v"

// ---------------- dibujo ----------------
static uint32_t *FB = 0;
static inline uint32_t RGB(int r, int g, int b) { return 0x80000000u | (r << 16) | (g << 8) | b; }

static void fill(int x, int y, int w, int h, uint32_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t *row = FB + (size_t)yy * SCREEN_W + x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}

static void text(int x, int y, const char *s, int scale, uint32_t col) {
    for (int i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *g = FONT8X16[ch - 32];
        for (int r = 0; r < 16; r++) {
            unsigned char bits = g[r];
            if (!bits) continue;
            for (int c = 0; c < 8; c++)
                if (bits & (0x80 >> c)) fill(x + (i * 8 + c) * scale, y + r * scale, scale, scale, col);
        }
    }
}

static const uint32_t C_BG = RGB(18, 22, 32), C_PANEL = RGB(30, 36, 52), C_TXT = RGB(235, 238, 245),
                      C_DIM = RGB(150, 158, 175), C_OK = RGB(90, 210, 120), C_BAD = RGB(235, 90, 90),
                      C_WARN = RGB(240, 190, 80), C_ACC = RGB(90, 150, 255);

// ---------------- archivos ----------------
static char *read_file(const char *path, long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 1 << 20) { fclose(f); return NULL; }
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return NULL; }
    long r = (long)fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = 0;
    if (len) *len = r;
    return b;
}

static bool write_file(const char *path, const char *data, long len) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    return ok;
}

static bool exists(const char *p, long *size = NULL) {
    bool d; long s;
    if (!plat_stat(p, &d, &s)) return false;
    if (size) *size = s;
    return true;
}

// Valor de "clave=valor" en un texto (ultima aparicion; ignora comentarios). -1 si no esta.
static int ini_get_int(const char *txt, const char *key) {
    int res = -1;
    size_t kl = strlen(key);
    const char *p = txt;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        const char *q = p;
        while (q < p + n && (*q == ' ' || *q == '\t')) q++;
        if (q + kl < p + n && strncmp(q, key, kl) == 0) {
            const char *v = q + kl;
            while (v < p + n && (*v == ' ' || *v == '\t')) v++;
            if (v < p + n && *v == '=') {
                v++;
                while (v < p + n && (*v == ' ' || *v == '\t')) v++;
                res = atoi(v);
            }
        }
        if (!e) break;
        p = e + 1;
    }
    return res;
}

// Pone activo=<v> en kbm_pad.ini conservando el resto del archivo.
static bool ini_set_activo(int v) {
    plat_mkdir(DIR_GOLDHEN);
    long n = 0;
    char *old = read_file(KBM_INI, &n);
    size_t cap = (size_t)n + 64;
    char *out = (char *)malloc(cap);
    if (!out) { free(old); return false; }
    size_t o = 0;
    bool done = false;
    const char *p = old ? old : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        const char *q = p;
        while (q < p + ln && (*q == ' ' || *q == '\t')) q++;
        bool isKey = (q + 5 < p + ln + 1) && strncmp(q, "activo", 6) == 0 &&
                     (q[6] == '=' || q[6] == ' ' || q[6] == '\t');
        if (isKey && !done) {
            o += (size_t)snprintf(out + o, cap - o, "activo=%d\n", v);
            done = true;
        } else if (!isKey) {
            memcpy(out + o, p, ln);
            o += ln;
            out[o++] = '\n';
        }  // duplicados de activo= se descartan
        if (!e) break;
        p = e + 1;
    }
    if (!done) o += (size_t)snprintf(out + o, cap - o, "activo=%d\n", v);
    bool ok = write_file(KBM_INI, out, (long)o);
    free(out);
    free(old);
    return ok;
}

static bool line_has_kbm(const char *p, size_t ln) {
    static const char key[] = "kbm_pad.prx";
    for (size_t i = 0; i + sizeof(key) - 1 <= ln; i++)
        if (strncmp(p + i, key, sizeof(key) - 1) == 0) return true;
    return false;
}

static bool plugins_ini_has() {
    char *t = read_file(PLUGINS_INI, NULL);
    if (!t) return false;
    bool found = false;
    const char *p = t;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        if (p[0] != ';' && p[0] != '#' && line_has_kbm(p, ln)) found = true;
        if (!e) break;
        p = e + 1;
    }
    free(t);
    return found;
}

// Anade la ruta del plugin justo debajo de [default] sin tocar nada mas.
static bool plugins_ini_add() {
    if (plugins_ini_has()) return true;
    plat_mkdir(DIR_GOLDHEN);
    long n = 0;
    char *old = read_file(PLUGINS_INI, &n);
    size_t cap = (size_t)n + 128;
    char *out = (char *)malloc(cap);
    if (!out) { free(old); return false; }
    size_t o = 0;
    bool done = false;
    const char *p = old ? old : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        size_t k = ln;
        while (k > 0 && (p[k - 1] == '\r' || p[k - 1] == ' ')) k--;
        memcpy(out + o, p, k);
        o += k;
        out[o++] = '\n';
        if (!done && k == 9 && strncmp(p, "[default]", 9) == 0) {
            o += (size_t)snprintf(out + o, cap - o, "%s\n", PRX_PATH);
            done = true;
        }
        if (!e) break;
        p = e + 1;
    }
    if (!done) {
        // sin seccion [default]: se crea al principio (las demas secciones quedan intactas)
        char *n2 = (char *)malloc(o + 128);
        if (!n2) { free(out); free(old); return false; }
        size_t h = (size_t)snprintf(n2, 128, "[default]\n%s\n", PRX_PATH);
        memcpy(n2 + h, out, o);
        free(out);
        out = n2;
        o += h;
    }
    bool ok = write_file(PLUGINS_INI, out, (long)o);
    free(out);
    free(old);
    return ok;
}

static bool plugins_ini_remove() {
    long n = 0;
    char *old = read_file(PLUGINS_INI, &n);
    if (!old) return true;
    char *out = (char *)malloc((size_t)n + 2);
    if (!out) { free(old); return false; }
    size_t o = 0;
    const char *p = old;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t ln = e ? (size_t)(e - p) : strlen(p);
        if (!(p[0] != ';' && p[0] != '#' && line_has_kbm(p, ln))) {
            memcpy(out + o, p, ln);
            o += ln;
            out[o++] = '\n';
        }
        if (!e) break;
        p = e + 1;
    }
    bool ok = write_file(PLUGINS_INI, out, (long)o);
    free(out);
    free(old);
    return ok;
}

// ---------------- estado ----------------
struct State {
    bool prx = false; long prxSize = 0;
    bool listed = false;
    int activo = -1;       // segun kbm_pad.ini (-1 = sin definir => el plugin usa 1)
    int instVer = -1;      // version que publica el plugin en kbm_pad.status (-1 = aun no)
    int teclado = -1, raton = -1;
    int latest = -1;       // ultima version en GitHub (-1 = sin consultar)
};
static State S;
static char g_msg[200] = "";
static uint32_t g_msgCol = C_DIM;

static void say(uint32_t col, const char *s) {
    snprintf(g_msg, sizeof(g_msg), "%s", s);
    g_msgCol = col;
}
static void sayf(uint32_t col, const char *fmt, const char *a) {
    snprintf(g_msg, sizeof(g_msg), fmt, a);
    g_msgCol = col;
}
static void sayd(uint32_t col, const char *fmt, int n) {
    snprintf(g_msg, sizeof(g_msg), fmt, n);
    g_msgCol = col;
}

static void refresh() {
    S.prx = exists(PRX_PATH, &S.prxSize);
    S.listed = plugins_ini_has();
    char *t = read_file(KBM_INI, NULL);
    S.activo = t ? ini_get_int(t, "activo") : -1;
    free(t);
    t = read_file(KBM_STATUS, NULL);
    S.instVer = t ? ini_get_int(t, "version") : -1;
    S.teclado = t ? ini_get_int(t, "teclado") : -1;
    S.raton = t ? ini_get_int(t, "raton") : -1;
    free(t);
}

static void draw_all(const char *title2 = NULL);

static bool check_latest() {
    char *buf = NULL; long len = 0; char err[160] = "";
    int r = plat_https_get(URL_VERSION, &buf, &len, 64, NULL, NULL, NULL, err, sizeof(err));
    if (r != 200 || !buf) { sayf(C_BAD, "No pude consultar la ultima version: %s", err); return false; }
    S.latest = atoi(buf);
    free(buf);
    return S.latest > 0;
}

static long g_dlDone = 0, g_dlTotal = -1;
static bool dl_cb(long done, long total, void *) {
    g_dlDone = done; g_dlTotal = total;
    draw_all("Descargando...");
    plat_present();
    return true;
}

static bool install_plugin() {
    if (!check_latest()) return false;
    plat_mkdir(DIR_GOLDHEN);
    plat_mkdir(DIR_PLUGINS);
    char url[200];
    snprintf(url, sizeof(url), URL_RELEASE "%d/kbm_pad.prx", S.latest);
    FILE *f = fopen(PRX_TMP, "wb");
    if (!f) { sayf(C_BAD, "No puedo escribir en %s (sin permisos?)", DIR_PLUGINS); return false; }
    char err[160] = "";
    g_dlDone = 0; g_dlTotal = -1;
    int r = plat_https_get(url, NULL, NULL, 0, f, dl_cb, NULL, err, sizeof(err));
    fclose(f);
    if (r != 200) { plat_remove(PRX_TMP); sayf(C_BAD, "Descarga fallida: %s", err); return false; }
    long sz = 0;
    if (!exists(PRX_TMP, &sz) || sz < 4096) { plat_remove(PRX_TMP); say(C_BAD, "El archivo descargado no es valido"); return false; }
    plat_remove(PRX_PATH);
    if (plat_rename(PRX_TMP, PRX_PATH) != 0) { say(C_BAD, "No pude guardar kbm_pad.prx"); return false; }
    if (!plugins_ini_add()) { say(C_BAD, "Plugin guardado, pero no pude editar plugins.ini"); return false; }
    return true;
}

static void uninstall_plugin() {
    plugins_ini_remove();
    plat_remove(PRX_PATH);
    plat_remove(KBM_STATUS);
    say(C_WARN, "Plugin desinstalado (kbm_pad.ini se conserva).");
}

// ---------------- pantalla ----------------
static void yesno(int x, int y, const char *label, int v, const char *yes, const char *no) {
    text(x, y, label, 2, C_DIM);
    text(x + 420, y, v < 0 ? "?" : (v ? yes : no), 2, v < 0 ? C_DIM : (v ? C_OK : C_BAD));
}

static void draw_all(const char *busy) {
    FB = plat_framebuffer();
    fill(0, 0, SCREEN_W, SCREEN_H, C_BG);
    text(120, 70, "kbm_manager", 5, C_TXT);
    text(120, 160, "Teclado + raton como mando (plugin kbm_pad)  v" APP_VERSION, 2, C_DIM);

    fill(120, 220, 1680, 440, C_PANEL);
    int x = 170, y = 260;
    yesno(x, y, "Plugin instalado:", S.prx ? 1 : 0, "SI", "NO"); y += 50;
    yesno(x, y, "En plugins.ini:", S.listed ? 1 : 0, "SI", "NO"); y += 50;
    int act = S.activo < 0 ? 1 : S.activo;
    yesno(x, y, "Modo teclado+raton:", S.prx ? act : -1, "ACTIVADO", "DESACTIVADO"); y += 50;
    text(x, y, "Version instalada:", 2, C_DIM);
    char b[64];
    if (S.instVer > 0) snprintf(b, sizeof(b), "v%d", S.instVer); else snprintf(b, sizeof(b), "%s", S.prx ? "(se ve al abrir un juego)" : "-");
    text(x + 420, y, b, 2, C_TXT); y += 50;
    text(x, y, "Ultima version:", 2, C_DIM);
    if (S.latest > 0) snprintf(b, sizeof(b), "v%d%s", S.latest, (S.instVer > 0 && S.instVer < S.latest) ? "  (hay actualizacion)" : ""); else snprintf(b, sizeof(b), "(pulsa X para consultar)");
    text(x + 420, y, b, 2, (S.instVer > 0 && S.latest > S.instVer) ? C_WARN : C_TXT); y += 50;
    if (S.teclado >= 0) {
        text(x, y, "En el ultimo juego:", 2, C_DIM);
        snprintf(b, sizeof(b), "teclado %s, raton %s", S.teclado ? "OK" : "no", S.raton ? "OK" : "no");
        text(x + 420, y, b, 2, C_TXT);
    }

    if (busy) {
        text(170, 590, busy, 2, C_ACC);
        if (g_dlTotal > 0) {
            fill(170, 625, 1580, 14, RGB(50, 58, 80));
            fill(170, 625, (int)(1580L * g_dlDone / g_dlTotal), 14, C_ACC);
        }
    } else {
        text(170, 590, g_msg, 2, g_msgCol);
    }

    int by = 710;
    text(120, by, S.prx ? "[X] Actualizar / reinstalar plugin" : "[X] Descargar e instalar plugin", 3, C_TXT); by += 60;
    text(120, by, "[Cuadrado] Activar / desactivar modo teclado+raton", 3, S.prx ? C_TXT : C_DIM); by += 60;
    text(120, by, "[Triangulo] Desinstalar plugin", 3, S.prx ? C_TXT : C_DIM); by += 60;
    text(120, by, "[Circulo] Salir", 3, C_TXT);
    text(120, 960, "En el juego: F7 modo conducir  |  F8 / F9 velocidad de camara  |  F10 pausar  |  F6 recargar", 2, C_ACC);
    text(120, 1010, "Tras instalar o cambiar, abre el juego. Si el juego ya estaba abierto, cierralo y vuelve a abrirlo.", 2, C_DIM);
}

int main() {
    if (!plat_init()) return 1;
    refresh();
    if (plat_jb_status() != 0) sayd(C_WARN, "Aviso: sin permisos de sistema (jailbreak %d). Puede fallar la escritura.", plat_jb_status());
    uint32_t prev = 0xFFFFFFFFu;
    int frame = 0;
    for (;;) {
        uint32_t b = plat_buttons();
        uint32_t press = b & ~prev;
        prev = b;
        if (press & BTN_CIRCLE) break;
        if (press & BTN_CROSS) {
            bool had = S.prx;
            draw_all("Consultando GitHub...");
            plat_present();
            if (install_plugin()) {
                if (!had) { ini_set_activo(1); }
                sayd(C_OK, had ? "Plugin actualizado a v%d" : "Plugin instalado: v%d (activado)", S.latest);
            }
            refresh();
        }
        if (press & BTN_SQUARE) {
            if (!S.prx) say(C_WARN, "Primero instala el plugin (X).");
            else {
                int now = S.activo < 0 ? 1 : S.activo;
                if (ini_set_activo(now ? 0 : 1)) say(now ? C_WARN : C_OK, now ? "Modo teclado+raton DESACTIVADO" : "Modo teclado+raton ACTIVADO");
                else say(C_BAD, "No pude escribir kbm_pad.ini");
                refresh();
            }
        }
        if (press & BTN_TRIANGLE) {
            if (S.prx) { uninstall_plugin(); refresh(); }
        }
        if (++frame % 120 == 0) refresh();
        draw_all(NULL);
        plat_present();
    }
    return 0;
}
