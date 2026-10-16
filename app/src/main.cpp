// kbm_manager: instala, actualiza, activa y desactiva el plugin kbm_pad (teclado + raton = mando).
// Base de pantalla/mando/HTTPS tomada de McStore.
#include "platform.h"
#include "font.h"
#include "icon_small.h"
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
// Archivo directo del repo: mismo servidor que VERSION (funciona en la PS4) y sin redirecciones.
#define URL_PRX_RAW  "https://raw.githubusercontent.com/" REPO "/main/dist/kbm_pad.prx"

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
                      C_WARN = RGB(240, 190, 80), C_ACC = RGB(110, 160, 255);

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

// Descarga `url` a PRX_TMP. Devuelve 200 si fue bien; si no, deja el motivo en err.
// `bytes` recibe los bytes realmente escritos (no depende de stat).
static int download_prx(const char *url, long *bytes, char *err, size_t errcap) {
    FILE *f = fopen(PRX_TMP, "wb");
    if (!f) { snprintf(err, errcap, "no puedo escribir en %s", DIR_PLUGINS); return -2; }
    g_dlDone = 0; g_dlTotal = -1;
    long got = 0;
    int r = plat_https_get(url, NULL, &got, 0, f, dl_cb, NULL, err, errcap);
    if (fclose(f) != 0 && r == 200) { snprintf(err, errcap, "error al guardar el archivo"); r = -3; }
    if (bytes) *bytes = got;
    if (r != 200) plat_remove(PRX_TMP);
    return r;
}

// Un .prx de PS4 empieza por la firma SELF 4F 15 3D 1D. Se lee con libc (la via con la que se escribio).
static bool prx_header_ok(const char *path, unsigned char h[4]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(h, 1, 4, f);
    fclose(f);
    return n == 4 && h[0] == 0x4F && h[1] == 0x15 && h[2] == 0x3D && h[3] == 0x1D;
}

static bool install_plugin() {
    check_latest();  // solo informativo: si falla, igual se intenta la descarga
    plat_mkdir(DIR_GOLDHEN);
    plat_mkdir(DIR_PLUGINS);
    // 1) archivo directo del repo (el mismo servidor que ya responde para VERSION)
    // 2) respaldo: asset de la release (GitHub lo redirige a otro servidor)
    char rel[200] = "";
    if (S.latest > 0) snprintf(rel, sizeof(rel), URL_RELEASE "%d/kbm_pad.prx", S.latest);
    const char *urls[2] = { URL_PRX_RAW, rel[0] ? rel : NULL };
    char errs[2][80] = { "", "" };
    bool got_ok = false;
    for (int i = 0; i < 2 && !got_ok; i++) {
        if (!urls[i]) continue;
        long bytes = 0;
        if (download_prx(urls[i], &bytes, errs[i], sizeof(errs[i])) != 200) continue;
        unsigned char hd[4] = {0, 0, 0, 0};
        if (bytes >= 4096 && prx_header_ok(PRX_TMP, hd)) { got_ok = true; break; }
        snprintf(errs[i], sizeof(errs[i]), "archivo no valido (%ld bytes, %02X%02X%02X%02X)", bytes, hd[0], hd[1], hd[2], hd[3]);
        plat_remove(PRX_TMP);
    }
    if (!got_ok) {
        // Si ya habia un plugin instalado, al menos se deja listado en plugins.ini.
        bool listed = exists(PRX_PATH) && plugins_ini_add();
        snprintf(g_msg, sizeof(g_msg), "Descarga fallida: %s%s%s%s", errs[0],
                 errs[1][0] ? " | " : "", errs[1], listed ? " - el plugin instalado ya esta en plugins.ini" : "");
        g_msgCol = C_BAD;
        return false;
    }
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
// Dibujo por software con bordes suavizados (sin libm: raiz cuadrada propia).
static float fsq(float v) {
    if (v <= 0.0f) return 0.0f;
    union { float f; uint32_t i; } u;
    u.f = v;
    u.i = 0x1fbd1df5u + (u.i >> 1);          // aproximacion inicial de sqrt
    float x = u.f;
    x = 0.5f * (x + v / x);
    x = 0.5f * (x + v / x);
    x = 0.5f * (x + v / x);
    return x;
}
static inline float fab(float v) { return v < 0 ? -v : v; }
static inline float fcl(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static inline void blend_px(int x, int y, uint32_t c, int a) {
    if (a <= 0 || x < 0 || y < 0 || x >= SCREEN_W || y >= SCREEN_H) return;
    uint32_t *p = FB + (size_t)y * SCREEN_W + x;
    if (a >= 255) { *p = c; return; }
    uint32_t d = *p;
    int r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
    int g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
    int b = ((c & 255) * a + (d & 255) * (255 - a)) / 255;
    *p = RGB(r, g, b);
}
static void hspan(int x0, int x1, int y, uint32_t c, int a) {   // [x0, x1)
    if (y < 0 || y >= SCREEN_H) return;
    if (x0 < 0) x0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (x0 >= x1) return;
    if (a >= 255) { uint32_t *row = FB + (size_t)y * SCREEN_W; for (int x = x0; x < x1; x++) row[x] = c; return; }
    for (int x = x0; x < x1; x++) blend_px(x, y, c, a);
}
// distancia con signo al rectangulo redondeado (negativa dentro)
static float sdf_rr(float px, float py, float x, float y, float w, float h, float r) {
    float qx = fab(px - (x + w * 0.5f)) - (w * 0.5f - r);
    float qy = fab(py - (y + h * 0.5f)) - (h * 0.5f - r);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    float m = qx > qy ? qx : qy;
    return fsq(ax * ax + ay * ay) + (m < 0 ? m : 0) - r;
}
static void rrect(int x, int y, int w, int h, int r, uint32_t c, int alpha = 255) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int yy = y; yy < y + h; yy++) {
        if (yy < y + r || yy >= y + h - r) {
            hspan(x + r, x + w - r, yy, c, alpha);
            for (int k = 0; k < r; k++) {
                int xl = x + k, xr = x + w - 1 - k;
                float cl = fcl(0.5f - sdf_rr(xl + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r));
                blend_px(xl, yy, c, (int)(alpha * cl));
                blend_px(xr, yy, c, (int)(alpha * cl));
            }
        } else hspan(x, x + w, yy, c, alpha);
    }
}
// solo el borde (para resplandor y contornos)
static void rframe(int x, int y, int w, int h, int r, int th, uint32_t c, int alpha) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    for (int yy = y; yy < y + h; yy++) {
        bool band = (yy < y + r + th) || (yy >= y + h - r - th);
        for (int xx = x; xx < x + w; xx++) {
            if (!band && xx >= x + th + 1 && xx < x + w - th - 1) { xx = x + w - th - 2; continue; }
            float d = sdf_rr(xx + 0.5f, yy + 0.5f, (float)x, (float)y, (float)w, (float)h, (float)r);
            float cv = fcl(0.5f - d) * fcl(d + th + 0.5f);
            blend_px(xx, yy, c, (int)(alpha * cv));
        }
    }
}
static void disc(int cx, int cy, int r, uint32_t c, int alpha = 255) { rrect(cx - r, cy - r, r * 2, r * 2, r, c, alpha); }
static void ring(int cx, int cy, int r, int th, uint32_t c) {
    for (int yy = cy - r - 1; yy <= cy + r + 1; yy++)
        for (int xx = cx - r - 1; xx <= cx + r + 1; xx++) {
            float d = fsq((xx + 0.5f - cx) * (xx + 0.5f - cx) + (yy + 0.5f - cy) * (yy + 0.5f - cy));
            float cv = fcl(r - d + 0.5f) * fcl(d - (r - th) + 0.5f);
            blend_px(xx, yy, c, (int)(255 * cv));
        }
}
static void line(int x0, int y0, int x1, int y1, int th, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (n == 0) n = 1;
    for (int i = 0; i <= n; i++) disc(x0 + dx * i / n, y0 + dy * i / n, th / 2 > 0 ? th / 2 : 1, c);
}
static int text_w(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }
static void text_r(int xr, int y, const char *s, int scale, uint32_t col) { text(xr - text_w(s, scale), y, s, scale, col); }

// Simbolos de los botones de PlayStation: 0 = X, 1 = cuadrado, 2 = triangulo, 3 = circulo
static void glyph(int kind, int cx, int cy, int r, bool dimmed) {
    static const uint32_t cols[4] = {RGB(100, 150, 255), RGB(230, 120, 190), RGB(90, 210, 130), RGB(240, 100, 100)};
    uint32_t c = dimmed ? RGB(96, 104, 126) : cols[kind];
    ring(cx, cy, r, r / 8 + 2, c);
    int s = r * 4 / 9, t = r / 7 + 2;
    if (kind == 0) { line(cx - s, cy - s, cx + s, cy + s, t, c); line(cx + s, cy - s, cx - s, cy + s, t, c); }
    else if (kind == 1) { line(cx - s, cy - s, cx + s, cy - s, t, c); line(cx + s, cy - s, cx + s, cy + s, t, c); line(cx + s, cy + s, cx - s, cy + s, t, c); line(cx - s, cy + s, cx - s, cy - s, t, c); }
    else if (kind == 2) { int h = s + s / 6; line(cx, cy - h, cx + h, cy + s * 2 / 3, t, c); line(cx + h, cy + s * 2 / 3, cx - h, cy + s * 2 / 3, t, c); line(cx - h, cy + s * 2 / 3, cx, cy - h, t, c); }
    else ring(cx, cy, s + 1, t, c);
}

static void keycap_label(int &x, int y, const char *key, const char *label) {
    int kw = text_w(key, 2) + 28;
    rrect(x, y, kw, 44, 10, RGB(60, 72, 104));
    rframe(x, y, kw, 44, 10, 2, RGB(110, 160, 255), 255);
    text(x + 14, y + 6, key, 2, RGB(235, 238, 245));
    x += kw + 12;
    text(x, y + 6, label, 2, C_DIM);
    x += text_w(label, 2) + 40;
}

static void dot_row(int y, const char *label, uint32_t dotcol, const char *val, uint32_t valcol) {
    disc(1128, y + 16, 10, dotcol);
    text(1160, y, label, 2, C_DIM);
    text_r(1790, y, val, 2, valcol);
}

static void draw_all(const char *busy) {
    FB = plat_framebuffer();
    // fondo: degradado y resplandor azul como el icono (se calcula una vez)
    static uint32_t *g_bg = 0;
    if (!g_bg) {
        g_bg = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
        if (g_bg) {
            for (int y = 0; y < SCREEN_H; y++) {
                float t = (float)y / SCREEN_H;
                float br = 16 + 14 * t, bg = 22 + 18 * t, bb = 40 + 34 * t;
                float dy = (y - 600) * 1.35f;
                for (int x = 0; x < SCREEN_W; x++) {
                    float dx = (float)(x - 960);
                    float d2 = dx * dx + dy * dy;
                    float w = 1.0f - d2 / (950.0f * 950.0f);
                    float k = w > 0 ? w * w * 0.34f : 0.0f;
                    g_bg[(size_t)y * SCREEN_W + x] = RGB((int)(br + (30 - br) * k), (int)(bg + (70 - bg) * k), (int)(bb + (150 - bb) * k));
                }
            }
        }
    }
    if (g_bg) memcpy(FB, g_bg, (size_t)SCREEN_W * SCREEN_H * 4);
    else fill(0, 0, SCREEN_W, SCREEN_H, RGB(20, 27, 50));
    const uint32_t PANEL = RGB(38, 48, 74), PANEL2 = RGB(70, 84, 116), LINE = RGB(66, 80, 118), BLUE = RGB(110, 160, 255);
    bool on = S.prx && (S.activo < 0 ? 1 : S.activo);

    // ----- cabecera (zona segura: 96 px a los lados, 54 arriba)
    for (int j = 0; j < 96; j++)
        for (int i = 0; i < 96; i++) {
            float cv = fcl(0.5f - sdf_rr(i + 0.5f, j + 0.5f, 0, 0, 96, 96, 22));
            const unsigned char *q = &ICON96[(j * 96 + i) * 3];
            blend_px(96 + i, 56 + j, RGB(q[0], q[1], q[2]), (int)(255 * cv));
        }
    rframe(96, 56, 96, 96, 22, 2, RGB(110, 160, 255), 120);
    text(212, 58, "kbm_manager", 4, C_TXT);
    text(214, 130, "Teclado + raton como mando para tu PS4", 2, C_DIM);
    char vb[48];
    snprintf(vb, sizeof(vb), "app v%s", APP_VERSION);
    int vw = text_w(vb, 2) + 48;
    rrect(1824 - vw, 78, vw, 52, 26, PANEL2);
    text(1824 - vw + 24, 88, vb, 2, C_DIM);

    // ----- tarjeta principal: estado y interruptor
    const int HX = 96, HY = 188, HW = 944, HH = 424;
    if (on) for (int i = 1; i <= 7; i++) rframe(HX - i * 2, HY - i * 2, HW + i * 4, HH + i * 4, 30 + i * 2, 3, BLUE, 46 - i * 6);
    rrect(HX, HY, HW, HH, 30, PANEL);
    rframe(HX, HY, HW, HH, 30, on ? 4 : 3, on ? BLUE : LINE, 255);
    text(HX + 50, HY + 40, "TECLADO + RATON", 2, C_DIM);
    const char *st = !S.prx ? "NO INSTALADO" : (on ? "ACTIVADO" : "DESACTIVADO");
    uint32_t stc = !S.prx ? C_WARN : (on ? BLUE : RGB(190, 196, 210));
    text(HX + 50, HY + 92, st, 5, stc);
    const char *sub = !S.prx ? "Pulsa X para instalar el plugin" : (on ? "Listo: abre un juego y usa el teclado" : "En pausa: el mando funciona normal");
    text(HX + 50, HY + 196, sub, 2, C_DIM);
    // interruptor grande
    {
        int tx = HX + HW - 50 - 220, ty = HY + 36;
        uint32_t track = !S.prx ? RGB(56, 64, 88) : (on ? BLUE : RGB(86, 96, 124));
        rrect(tx, ty, 220, 108, 54, track);
        int kx = on ? tx + 220 - 54 : tx + 54;
        disc(kx, ty + 54, 44, S.prx ? RGB(235, 238, 245) : RGB(150, 158, 175));
    }
    // pista para el boton Cuadrado
    glyph(1, HX + 78, HY + 280, 26, !S.prx);
    text(HX + 124, HY + 264, S.prx ? "Activar / desactivar" : "Instala el plugin primero", 2, S.prx ? C_TXT : C_DIM);
    hspan(HX + 50, HX + HW - 50, HY + 322, LINE, 255);
    if (S.teclado >= 0) {
        int cx = HX + 50;
        for (int i = 0; i < 2; i++) {
            int ok = i == 0 ? S.teclado : S.raton;
            rrect(cx, HY + 340, 270, 60, 30, PANEL2);
            disc(cx + 34, HY + 370, 11, ok ? C_OK : C_BAD);
            text(cx + 62, HY + 354, i == 0 ? (ok ? "Teclado OK" : "Teclado no") : (ok ? "Raton OK" : "Raton no"), 2, C_TXT);
            cx += 294;
        }
    } else {
        text(HX + 50, HY + 354, "Abre un juego para detectar teclado y raton", 2, C_DIM);
    }

    // ----- tarjeta de estado
    const int RX = 1064, RY = 188, RW = 760, RH = 424;
    rrect(RX, RY, RW, RH, 30, PANEL);
    text(RX + 50, RY + 40, "ESTADO", 2, C_DIM);
    char b[64];
    int ry = RY + 98;
    dot_row(ry, "Plugin instalado", S.prx ? C_OK : C_BAD, S.prx ? "SI" : "NO", S.prx ? C_OK : C_BAD); ry += 72;
    dot_row(ry, "En plugins.ini", S.listed ? C_OK : C_BAD, S.listed ? "SI" : "NO", S.listed ? C_OK : C_BAD); ry += 72;
    if (S.instVer > 0) snprintf(b, sizeof(b), "v%d", S.instVer); else snprintf(b, sizeof(b), "%s", S.prx ? "al abrir juego" : "-");
    dot_row(ry, "Version instalada", RGB(120, 130, 155), b, C_TXT); ry += 72;
    bool newer = S.instVer > 0 && S.latest > S.instVer;
    if (S.latest > 0) snprintf(b, sizeof(b), "v%d%s", S.latest, newer ? " (nueva)" : ""); else snprintf(b, sizeof(b), "pulsa X");
    dot_row(ry, "Ultima version", newer ? C_WARN : RGB(120, 130, 155), b, newer ? C_WARN : C_TXT);

    // ----- barra de mensajes
    rrect(96, 640, 1728, 92, 22, PANEL);
    uint32_t mc = busy ? C_ACC : g_msgCol;
    rrect(96, 640, 12, 92, 6, (busy || g_msg[0]) ? mc : LINE);
    if (busy) {
        text(140, 656, busy, 2, C_ACC);
        if (g_dlTotal > 0) {
            rrect(140, 704, 1640, 12, 6, RGB(56, 66, 98));
            int fw = (int)(1640L * g_dlDone / g_dlTotal);
            if (fw >= 12) rrect(140, 704, fw, 12, 6, C_ACC);
        }
    } else {
        text(140, 668, g_msg[0] ? g_msg : "Listo.", 2, g_msg[0] ? g_msgCol : C_DIM);
    }

    // ----- botones de accion
    struct Btn { int kind; const char *title; const char *sub; bool enabled; };
    Btn bt[3] = {
        {0, S.prx ? "Actualizar" : "Instalar", "Descarga desde GitHub", true},
        {1, on ? "Desactivar" : "Activar", "Teclado + raton", S.prx},
        {2, "Desinstalar", "Quita el plugin", S.prx},
    };
    for (int i = 0; i < 3; i++) {
        int bx = 96 + i * (560 + 24), by = 760;
        rrect(bx, by, 560, 124, 28, bt[i].enabled ? PANEL2 : PANEL);
        if (bt[i].enabled) rframe(bx, by, 560, 124, 28, 3, BLUE, 255);
        glyph(bt[i].kind, bx + 76, by + 62, 36, !bt[i].enabled);
        text(bx + 144, by + 24, bt[i].title, 3, bt[i].enabled ? C_TXT : RGB(120, 128, 150));
        text(bx + 144, by + 80, bt[i].sub, 2, bt[i].enabled ? C_DIM : RGB(100, 108, 130));
    }
    text(96, 898, "Tras instalar o cambiar algo, abre el juego (si ya estaba abierto, cierralo antes).", 2, RGB(120, 128, 150));

    // ----- barra inferior con las teclas del juego y Salir
    rrect(96, 940, 1728, 76, 22, RGB(30, 38, 62));
    int kx = 130;
    keycap_label(kx, 956, "F7", "Conducir");
    keycap_label(kx, 956, "F9/F10", "Velocidad");
    keycap_label(kx, 956, "F8", "Pausar");
    keycap_label(kx, 956, "F6", "Recargar");
    glyph(3, 1824 - 40 - text_w("Salir", 2) - 44, 978, 22, false);
    text_r(1824 - 40, 962, "Salir", 2, C_TXT);
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
                if (S.latest > 0) sayd(C_OK, had ? "Plugin actualizado a v%d" : "Plugin instalado: v%d (activado)", S.latest);
                else say(C_OK, had ? "Plugin actualizado" : "Plugin instalado (activado)");
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
