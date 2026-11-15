// Pantalla de ajustes de kbm_manager: camara, botones, movimiento y raton.
// Se incluye en main.cpp (usa sus funciones de dibujo y archivos). Todo se guarda solo en
// /data/GoldHEN/kbm_pad.ini; el plugin vigila el archivo y aplica los cambios.
#pragma once
#include "ini_default.h"

// ---------------- archivo de configuracion ----------------
static char *g_cfg = NULL;

static void cf_load() {
    free(g_cfg);
    g_cfg = read_file(KBM_INI, NULL);
    if (!g_cfg) {   // aun no existe: se parte de la plantilla (se escribe al guardar el primer cambio)
        g_cfg = (char *)malloc(sizeof(KBM_INI_DEFAULT));
        if (g_cfg) memcpy(g_cfg, KBM_INI_DEFAULT, sizeof(KBM_INI_DEFAULT));
    }
}

// Si la linea p[0..n) es "clave = valor", devuelve el inicio del valor (si no, NULL).
static const char *cf_line_value(const char *p, size_t n, const char *key) {
    const char *q = p, *end = p + n;
    while (q < end && (*q == ' ' || *q == '\t')) q++;
    if (q >= end || *q == '#' || *q == ';') return NULL;
    size_t kl = strlen(key);
    if ((size_t)(end - q) < kl || strncmp(q, key, kl) != 0) return NULL;
    q += kl;
    while (q < end && (*q == ' ' || *q == '\t')) q++;
    if (q >= end || *q != '=') return NULL;
    q++;
    while (q < end && (*q == ' ' || *q == '\t')) q++;
    return q;
}

// Ultimo valor de `key` (el plugin aplica las lineas en orden, asi que gana la ultima).
static bool cf_get(const char *key, char *out, size_t cap) {
    out[0] = 0;
    bool found = false;
    const char *p = g_cfg ? g_cfg : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        const char *v = cf_line_value(p, n, key);
        if (v) {
            size_t len = (size_t)(p + n - v);
            while (len > 0 && (v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\r')) len--;
            if (len >= cap) len = cap - 1;
            memcpy(out, v, len);
            out[len] = 0;
            found = true;
        }
        if (!e) break;
        p = e + 1;
    }
    return found;
}

// Cambia (o agrega) `clave=valor`, quita duplicados y guarda el archivo.
static bool cf_set(const char *key, const char *val) {
    if (!g_cfg) cf_load();
    if (!g_cfg) return false;
    size_t cap = strlen(g_cfg) + strlen(key) + strlen(val) + 16;
    char *out = (char *)malloc(cap);
    if (!out) return false;
    size_t o = 0;
    bool done = false;
    const char *p = g_cfg;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (cf_line_value(p, n, key)) {
            if (!done) {
                o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, val);
                done = true;
            }   // los duplicados se descartan
        } else {
            memcpy(out + o, p, n);
            o += n;
            out[o++] = '\n';
        }
        if (!e) break;
        p = e + 1;
    }
    if (!done) o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, val);
    out[o] = 0;
    plat_mkdir(DIR_GOLDHEN);
    bool ok = write_file(KBM_INI, out, (long)o);
    if (ok) { free(g_cfg); g_cfg = out; }
    else free(out);
    return ok;
}

// ---------------- numeros (centesimas enteras, sin coma flotante) ----------------
static int cf_parse100(const char *s) {
    int sign = 1, w = 0, f = 0, d = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') w = w * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',') {
        s++;
        while (*s >= '0' && *s <= '9') { if (d < 2) { f = f * 10 + (*s - '0'); d++; } s++; }
    }
    while (d < 2) { f *= 10; d++; }
    return sign * (w * 100 + f);
}
static void cf_fmt100(int v, char *o, size_t n) {
    int a = v < 0 ? -v : v;
    snprintf(o, n, "%s%d.%02d", v < 0 ? "-" : "", a / 100, a % 100);
}

// ---------------- listas de valores ----------------
static const char *const CF_KEYS[] = {
    "ninguna",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
    "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z",
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "espacio", "enter", "tab", "esc", "borrar", "caps", "shift", "ctrl", "alt",
    "flecha_arriba", "flecha_abajo", "flecha_izq", "flecha_der",
};
#define CF_NKEYS ((int)(sizeof(CF_KEYS) / sizeof(CF_KEYS[0])))

static const char *const CF_BTNS[] = {
    "ninguna", "cruz", "circulo", "cuadrado", "triangulo", "l1", "r1", "l2", "r2", "l3", "r3",
    "options", "touchpad", "dpad_arriba", "dpad_abajo", "dpad_izq", "dpad_der",
};
#define CF_NBTNS ((int)(sizeof(CF_BTNS) / sizeof(CF_BTNS[0])))

// Codigo HID del teclado -> nombre que entiende el plugin (NULL si no esta soportada).
static const char *cf_hid_name(int code, char *buf) {
    if (code >= 4 && code <= 29) { buf[0] = (char)('a' + code - 4); buf[1] = 0; return buf; }
    if (code >= 30 && code <= 38) { buf[0] = (char)('1' + code - 30); buf[1] = 0; return buf; }
    if (code == 39) return "0";
    if (code >= 58 && code <= 69) { snprintf(buf, 8, "f%d", code - 57); return buf; }
    switch (code) {
    case 44: return "espacio";
    case 40: return "enter";
    case 43: return "tab";
    case 42: return "borrar";
    case 57: return "caps";
    case 82: return "flecha_arriba";
    case 81: return "flecha_abajo";
    case 80: return "flecha_izq";
    case 79: return "flecha_der";
    case 0x122: return "shift";
    case 0x111: return "ctrl";
    case 0x144: return "alt";
    }
    return NULL;
}

static int cf_index(const char *const *list, int n, const char *name) {
    for (int i = 0; i < n; i++) if (strcmp(list[i], name) == 0) return i;
    return 0;
}

// ---------------- filas ----------------
enum { RK_PRESET, RK_NUM, RK_BOOL, RK_KEYS, RK_KEY1, RK_BTN };
struct CfRow { int kind; const char *label; const char *key; const char *def; int mn, mx, st; };
struct CfPage { const char *name; const CfRow *rows; int n; };

static const CfRow PG_CAM[] = {
    {RK_PRESET, "Preajuste de camara", "", "", 0, 0, 0},
    {RK_NUM,  "Sensibilidad horizontal", "sens_x", "6.0", 50, 4000, 50},
    {RK_NUM,  "Sensibilidad vertical", "sens_y", "6.0", 50, 4000, 50},
    {RK_NUM,  "Sensibilidad al apuntar", "sens_mira", "0.75", 10, 300, 5},
    {RK_NUM,  "Suavizado", "suavizado", "0.40", 0, 90, 5},
    {RK_BOOL, "Suavizado dinamico", "suavizado_dinamico", "1", 0, 0, 0},
    {RK_NUM,  "Zona muerta del juego", "zona_muerta", "0.15", 0, 60, 1},
    {RK_NUM,  "Curva de aceleracion", "curva", "0.40", 0, 300, 10},
    {RK_BOOL, "Invertir eje vertical", "invertir_y", "0", 0, 0, 0},
    {RK_BOOL, "Clic derecho alterna la mira", "alternar_mira", "0", 0, 0, 0},
    {RK_BOOL, "Ocultar teclado y raton al juego", "ocultar", "1", 0, 0, 0},
};
static const CfRow PG_BTN[] = {
    {RK_KEYS, "Cruz (X)", "cruz", "espacio", 0, 0, 0},
    {RK_KEYS, "Circulo (O)", "circulo", "e", 0, 0, 0},
    {RK_KEYS, "Cuadrado", "cuadrado", "q", 0, 0, 0},
    {RK_KEYS, "Triangulo", "triangulo", "r", 0, 0, 0},
    {RK_KEYS, "L1", "l1", "f", 0, 0, 0},
    {RK_KEYS, "R1", "r1", "g", 0, 0, 0},
    {RK_KEYS, "L2", "l2", "ninguna", 0, 0, 0},
    {RK_KEYS, "R2", "r2", "ninguna", 0, 0, 0},
    {RK_KEYS, "L3 (clic stick izq.)", "l3", "shift", 0, 0, 0},
    {RK_KEYS, "R3 (clic stick der.)", "r3", "ctrl,v", 0, 0, 0},
    {RK_KEYS, "Options", "options", "enter", 0, 0, 0},
    {RK_KEYS, "Touchpad", "touchpad", "tab", 0, 0, 0},
    {RK_KEYS, "Cruceta arriba", "dpad_arriba", "flecha_arriba", 0, 0, 0},
    {RK_KEYS, "Cruceta abajo", "dpad_abajo", "flecha_abajo", 0, 0, 0},
    {RK_KEYS, "Cruceta izquierda", "dpad_izq", "flecha_izq", 0, 0, 0},
    {RK_KEYS, "Cruceta derecha", "dpad_der", "flecha_der", 0, 0, 0},
};
static const CfRow PG_MOVE[] = {
    {RK_KEY1, "Mover adelante", "adelante", "w", 0, 0, 0},
    {RK_KEY1, "Mover atras", "atras", "s", 0, 0, 0},
    {RK_KEY1, "Mover a la izquierda", "izquierda", "a", 0, 0, 0},
    {RK_KEY1, "Mover a la derecha", "derecha", "d", 0, 0, 0},
    {RK_KEY1, "Tecla de caminar", "tecla_caminar", "ninguna", 0, 0, 0},
    {RK_NUM,  "Fuerza al caminar", "caminar", "0.50", 10, 90, 5},
    {RK_KEY1, "Modo conducir", "tecla_conducir", "f7", 0, 0, 0},
    {RK_KEY1, "Pausar o activar", "tecla_pausa", "f8", 0, 0, 0},
    {RK_KEY1, "Sensibilidad menos", "tecla_sens_menos", "f9", 0, 0, 0},
    {RK_KEY1, "Sensibilidad mas", "tecla_sens_mas", "f10", 0, 0, 0},
    {RK_KEY1, "Recargar ajustes", "tecla_recargar", "f6", 0, 0, 0},
};
static const CfRow PG_MOUSE[] = {
    {RK_BTN, "Clic izquierdo", "raton_izq", "r2", 0, 0, 0},
    {RK_BTN, "Clic derecho", "raton_der", "l2", 0, 0, 0},
    {RK_BTN, "Clic de la rueda", "raton_medio", "r3", 0, 0, 0},
    {RK_BTN, "Boton lateral 1", "raton_lat1", "ninguna", 0, 0, 0},
    {RK_BTN, "Boton lateral 2", "raton_lat2", "ninguna", 0, 0, 0},
    {RK_BTN, "Rueda hacia arriba", "rueda_arriba", "ninguna", 0, 0, 0},
    {RK_BTN, "Rueda hacia abajo", "rueda_abajo", "ninguna", 0, 0, 0},
};
#define CF_PAGES 4
static const CfPage CF_PAGE[CF_PAGES] = {
    {"Camara", PG_CAM, (int)(sizeof(PG_CAM) / sizeof(PG_CAM[0]))},
    {"Botones", PG_BTN, (int)(sizeof(PG_BTN) / sizeof(PG_BTN[0]))},
    {"Movimiento", PG_MOVE, (int)(sizeof(PG_MOVE) / sizeof(PG_MOVE[0]))},
    {"Raton", PG_MOUSE, (int)(sizeof(PG_MOUSE) / sizeof(PG_MOUSE[0]))},
};

// Preajustes: sens_x, sens_y, suavizado, curva, sens_mira
struct CfPreset { const char *name; const char *v[5]; };
static const CfPreset CF_PRESETS[] = {
    {"Suave", {"4.0", "4.0", "0.55", "0.20", "0.60"}},
    {"Normal", {"6.0", "6.0", "0.40", "0.40", "0.75"}},
    {"Rapido", {"9.0", "9.0", "0.30", "0.50", "0.80"}},
    {"Muy rapido", {"13.0", "13.0", "0.20", "0.60", "0.85"}},
    {"Preciso", {"7.0", "7.0", "0.10", "0.20", "0.70"}},
};
#define CF_NPRESETS ((int)(sizeof(CF_PRESETS) / sizeof(CF_PRESETS[0])))
static const char *const CF_PRESET_KEYS[5] = {"sens_x", "sens_y", "suavizado", "curva", "sens_mira"};

// ---------------- valores de una fila ----------------
static void cf_value(const CfRow &r, char *out, size_t cap) {
    if (!cf_get(r.key, out, cap) || !out[0]) snprintf(out, cap, "%s", r.def);
}

// Parte "a, b ,c" en trozos (se conservan los "ninguna" para no mover posiciones).
static int cf_split(const char *val, char sl[][24], int maxn) {
    int n = 0;
    const char *p = val;
    while (*p && n < maxn) {
        int k = 0;
        while (*p && *p != ',') {
            if (*p != ' ' && *p != '\t' && k < 23) sl[n][k++] = *p;
            p++;
        }
        sl[n][k] = 0;
        if (*p == ',') p++;
        if (k) n++;
    }
    return n;
}

static void cf_join(char sl[][24], int n, char *out, size_t cap) {
    while (n > 1 && strcmp(sl[n - 1], "ninguna") == 0) n--;   // sin "ninguna" al final
    out[0] = 0;
    size_t o = 0;
    for (int i = 0; i < n && o + 26 < cap; i++) {
        if (i) out[o++] = ',';
        size_t l = strlen(sl[i]);
        memcpy(out + o, sl[i], l);
        o += l;
        out[o] = 0;
    }
    if (n == 0 || (n == 1 && strcmp(sl[0], "ninguna") == 0)) snprintf(out, cap, "ninguna");
}

static char g_cfMsg[200] = "";
static uint32_t g_cfMsgCol = 0;
static int g_cfMsgTtl = 0;
static void cf_say(uint32_t col, const char *s) {
    snprintf(g_cfMsg, sizeof(g_cfMsg), "%s", s);
    g_cfMsgCol = col;
    g_cfMsgTtl = 60 * 8;
}

static bool cf_is_pretty_key(const char *k) { return k[0] && !k[1]; }
static void cf_show_key(const char *k, char *out, size_t cap) {
    if (cf_is_pretty_key(k) && k[0] >= 'a' && k[0] <= 'z') snprintf(out, cap, "%c", k[0] - 32);
    else snprintf(out, cap, "%s", k);
}

// Avisa si `name` ya esta en otro boton o movimiento.
static void cf_check_conflict(const CfRow &self, const char *name) {
    if (strcmp(name, "ninguna") == 0) return;
    for (int pg = 1; pg <= 2; pg++) {
        const CfPage &P = CF_PAGE[pg];
        for (int i = 0; i < P.n; i++) {
            const CfRow &o = P.rows[i];
            if (&o == &self || (o.kind != RK_KEYS && o.kind != RK_KEY1)) continue;
            char v[160], sl[6][24];
            cf_value(o, v, sizeof(v));
            int n = cf_split(v, sl, 6);
            for (int k = 0; k < n; k++)
                if (strcmp(sl[k], name) == 0) {
                    char m[160], shown[24];
                    cf_show_key(name, shown, sizeof(shown));
                    snprintf(m, sizeof(m), "Aviso: la tecla %s tambien esta en \"%s\".", shown, o.label);
                    cf_say(C_WARN, m);
                    return;
                }
        }
    }
}

static void cf_saved(bool ok) {
    if (!ok) cf_say(C_BAD, "No pude guardar kbm_pad.ini (revisa permisos de /data/GoldHEN).");
    else if (g_cfMsgTtl <= 0) cf_say(C_OK, "Guardado. El plugin lo aplica solo (o pulsa F6 en el juego).");
}

// ---------------- acciones sobre la fila ----------------
static int g_cfPreset = -1;

static void cf_apply_preset(int idx) {
    g_cfPreset = (idx + CF_NPRESETS) % CF_NPRESETS;
    bool ok = true;
    for (int i = 0; i < 5; i++) ok = cf_set(CF_PRESET_KEYS[i], CF_PRESETS[g_cfPreset].v[i]) && ok;
    char m[100];
    snprintf(m, sizeof(m), "Preajuste \"%s\" aplicado.", CF_PRESETS[g_cfPreset].name);
    cf_say(ok ? C_OK : C_BAD, ok ? m : "No pude guardar kbm_pad.ini.");
}

static void cf_adjust(const CfRow &r, int dir, int slot) {
    char v[160];
    switch (r.kind) {
    case RK_PRESET: cf_apply_preset(g_cfPreset < 0 ? (dir > 0 ? 0 : CF_NPRESETS - 1) : g_cfPreset + dir); break;
    case RK_NUM: {
        cf_value(r, v, sizeof(v));
        int x = cf_parse100(v) + dir * r.st;
        if (x < r.mn) x = r.mn;
        if (x > r.mx) x = r.mx;
        char t[24];
        cf_fmt100(x, t, sizeof(t));
        cf_saved(cf_set(r.key, t));
        break;
    }
    case RK_BOOL: {
        cf_value(r, v, sizeof(v));
        cf_saved(cf_set(r.key, atoi(v) != 0 ? "0" : "1"));
        break;
    }
    case RK_KEYS: {
        cf_value(r, v, sizeof(v));
        char sl[6][24];
        int n = cf_split(v, sl, 6);
        while (n <= slot && n < 6) snprintf(sl[n++], 24, "ninguna");
        int idx = cf_index(CF_KEYS, CF_NKEYS, sl[slot]);
        idx = (idx + dir + CF_NKEYS) % CF_NKEYS;
        snprintf(sl[slot], 24, "%s", CF_KEYS[idx]);
        char j[160];
        cf_join(sl, n, j, sizeof(j));
        cf_saved(cf_set(r.key, j));
        cf_check_conflict(r, CF_KEYS[idx]);
        break;
    }
    case RK_KEY1: {
        cf_value(r, v, sizeof(v));
        int idx = cf_index(CF_KEYS, CF_NKEYS, v);
        idx = (idx + dir + CF_NKEYS) % CF_NKEYS;
        cf_saved(cf_set(r.key, CF_KEYS[idx]));
        cf_check_conflict(r, CF_KEYS[idx]);
        break;
    }
    case RK_BTN: {
        cf_value(r, v, sizeof(v));
        int idx = cf_index(CF_BTNS, CF_NBTNS, v);
        idx = (idx + dir + CF_NBTNS) % CF_NBTNS;
        cf_saved(cf_set(r.key, CF_BTNS[idx]));
        break;
    }
    }
}

// Asigna una tecla concreta (captura) a la fila / hueco.
static void cf_assign(const CfRow &r, int slot, const char *name) {
    char v[160];
    if (r.kind == RK_KEYS) {
        cf_value(r, v, sizeof(v));
        char sl[6][24];
        int n = cf_split(v, sl, 6);
        while (n <= slot && n < 6) snprintf(sl[n++], 24, "ninguna");
        snprintf(sl[slot], 24, "%s", name);
        char j[160];
        cf_join(sl, n, j, sizeof(j));
        cf_saved(cf_set(r.key, j));
        cf_check_conflict(r, name);
    } else if (r.kind == RK_KEY1) {
        cf_saved(cf_set(r.key, name));
        cf_check_conflict(r, name);
    }
}

static void cf_clear_slot(const CfRow &r, int slot) {
    char v[160];
    if (r.kind == RK_KEYS) {
        cf_value(r, v, sizeof(v));
        char sl[6][24];
        int n = cf_split(v, sl, 6);
        if (slot >= n) return;
        snprintf(sl[slot], 24, "ninguna");
        char j[160];
        cf_join(sl, n, j, sizeof(j));
        cf_saved(cf_set(r.key, j));
    } else if (r.kind == RK_KEY1 || r.kind == RK_BTN) {
        cf_saved(cf_set(r.key, "ninguna"));
    }
}

static void cf_reset_row(const CfRow &r) {
    if (r.kind == RK_PRESET) { cf_apply_preset(1); return; }
    cf_saved(cf_set(r.key, r.def));
    cf_say(C_OK, "Valor por defecto restaurado.");
}

static void cf_reset_page(const CfPage &P) {
    bool ok = true;
    for (int i = 0; i < P.n; i++) if (P.rows[i].kind != RK_PRESET) ok = cf_set(P.rows[i].key, P.rows[i].def) && ok;
    cf_say(ok ? C_OK : C_BAD, ok ? "Pagina restaurada a los valores por defecto." : "No pude guardar kbm_pad.ini.");
}

// ---------------- dibujo ----------------
static void cf_chip(int x, int y, int w, const char *label, bool active, bool dim) {
    rrect(x, y + 6, w, 44, 12, active ? RGB(70, 96, 160) : RGB(52, 64, 96));
    if (active) rframe(x, y + 6, w, 44, 12, 3, RGB(110, 160, 255), 255);
    int tw = text_w(label, 2);
    text(x + (w - tw) / 2, y + 12, label, 2, dim ? RGB(120, 130, 155) : C_TXT);
}

static void cf_draw_row(const CfRow &r, int y, bool sel, int slot) {
    const uint32_t PANEL = RGB(38, 48, 74), PANEL2 = RGB(58, 72, 108), BLUE = RGB(110, 160, 255);
    rrect(96, y, 1728, 56, 16, sel ? PANEL2 : PANEL);
    if (sel) rframe(96, y, 1728, 56, 16, 3, BLUE, 255);
    text(126, y + 12, r.label, 2, sel ? C_TXT : RGB(200, 206, 220));
    char v[160], t[48];
    const int VX = 880;
    switch (r.kind) {
    case RK_PRESET: {
        const char *nm = g_cfPreset >= 0 ? CF_PRESETS[g_cfPreset].name : "elige uno";
        text(VX, y + 12, "<", 2, C_DIM);
        cf_chip(VX + 40, y, 360, nm, sel, g_cfPreset < 0);
        text(VX + 416, y + 12, ">", 2, C_DIM);
        break;
    }
    case RK_NUM: {
        cf_value(r, v, sizeof(v));
        int x = cf_parse100(v);
        if (x < r.mn) x = r.mn;
        if (x > r.mx) x = r.mx;
        cf_fmt100(x, t, sizeof(t));
        text(VX, y + 12, "<", 2, C_DIM);
        rrect(VX + 40, y + 23, 520, 10, 5, RGB(30, 38, 62));
        int fw = (int)(520L * (x - r.mn) / (r.mx - r.mn));
        if (fw >= 10) rrect(VX + 40, y + 23, fw, 10, 5, sel ? BLUE : RGB(90, 120, 190));
        disc(VX + 40 + fw, y + 28, 11, sel ? RGB(235, 238, 245) : RGB(170, 178, 200));
        text(VX + 590, y + 12, ">", 2, C_DIM);
        text_r(1794, y + 12, t, 2, sel ? C_TXT : C_ACC);
        break;
    }
    case RK_BOOL: {
        cf_value(r, v, sizeof(v));
        bool on = atoi(v) != 0;
        rrect(VX, y + 10, 96, 36, 18, on ? BLUE : RGB(86, 96, 124));
        disc(on ? VX + 96 - 18 : VX + 18, y + 28, 14, RGB(235, 238, 245));
        text(VX + 116, y + 12, on ? "si" : "no", 2, on ? C_OK : C_DIM);
        break;
    }
    case RK_KEYS: {
        cf_value(r, v, sizeof(v));
        char sl[6][24];
        int n = cf_split(v, sl, 6);
        for (int i = 0; i < 2; i++) {
            const char *k = i < n ? sl[i] : "ninguna";
            char shown[32];
            cf_show_key(k, shown, sizeof(shown));
            cf_chip(VX + i * 290, y, 270, shown, sel && slot == i, strcmp(k, "ninguna") == 0);
        }
        if (n > 2) {
            snprintf(t, sizeof(t), "+%d mas", n - 2);
            text(VX + 590, y + 12, t, 2, C_DIM);
        }
        break;
    }
    case RK_KEY1: {
        cf_value(r, v, sizeof(v));
        char shown[32];
        cf_show_key(v, shown, sizeof(shown));
        text(VX, y + 12, "<", 2, C_DIM);
        cf_chip(VX + 40, y, 300, shown, sel, strcmp(v, "ninguna") == 0);
        text(VX + 356, y + 12, ">", 2, C_DIM);
        break;
    }
    case RK_BTN: {
        cf_value(r, v, sizeof(v));
        for (char *q = v; *q; q++) if (*q >= 'a' && *q <= 'z' && (v[0] == 'l' || v[0] == 'r') && v[2] == 0) *q = (char)(*q - 32);
        text(VX, y + 12, "<", 2, C_DIM);
        cf_chip(VX + 40, y, 300, v, sel, strcmp(v, "ninguna") == 0);
        text(VX + 356, y + 12, ">", 2, C_DIM);
        break;
    }
    }
}

static const char *cf_footer(int kind) {
    bool kb = (plat_devices() & 1) != 0;
    if (kb && (kind == RK_KEYS)) return "Enter: pulsa la nueva tecla   1/2: tecla 1 o 2   Retroceso: quitar   R: defecto   Esc: volver";
    if (kb && (kind == RK_KEY1)) return "Enter: pulsa la nueva tecla   Izq/Der: lista   Retroceso: ninguna   R: defecto   Esc: volver";
    switch (kind) {
    case RK_PRESET: return "Izq/Der o X: aplicar preajuste     Triangulo: Normal     O: volver";
    case RK_NUM: return "Izq/Der: cambiar (mantenlo para ir rapido)     Triangulo: por defecto     O: volver";
    case RK_BOOL: return "Izq/Der o X: activar o desactivar     Triangulo: por defecto     O: volver";
    case RK_KEYS: return "X: tecla 1 o 2   Izq/Der: cambiar tecla   Cuadrado: quitar   Triangulo: defecto";
    default: return "Izq/Der: cambiar     Cuadrado: ninguna     Triangulo: por defecto     O: volver";
    }
}

static bool g_cfCapturing = false;

static void cf_draw(int pg, int sel, int top, int slot) {
    FB = plat_framebuffer();
    const uint32_t PANEL2 = RGB(70, 84, 116), BLUE = RGB(110, 160, 255);
    for (int y = 0; y < SCREEN_H; y += 4) {
        int k = y * 20 / SCREEN_H;
        fill(0, y, SCREEN_W, 4, RGB(16 + k / 2, 22 + k, 40 + k * 2));
    }
    text(96, 50, "Ajustes", 4, C_TXT);
    text_r(1824, 66, "L1 / R1: cambiar de pagina", 2, C_DIM);
    for (int i = 0; i < CF_PAGES; i++) {
        int x = 96 + i * 412;
        bool a = i == pg;
        rrect(x, 124, 392, 60, 30, a ? PANEL2 : RGB(32, 40, 62));
        if (a) rframe(x, 124, 392, 60, 30, 3, BLUE, 255);
        int tw = text_w(CF_PAGE[i].name, 2);
        text(x + (392 - tw) / 2, 124 + 14, CF_PAGE[i].name, 2, a ? C_TXT : C_DIM);
    }
    const CfPage &P = CF_PAGE[pg];
    const int ROWH = 64, VIS = 10, LY = 212;
    for (int i = 0; i < VIS && top + i < P.n; i++) cf_draw_row(P.rows[top + i], LY + i * ROWH, top + i == sel, slot);
    if (top > 0) text_r(1824, LY - 28, "^ mas arriba", 2, C_DIM);
    if (top + VIS < P.n) text_r(1824, LY + VIS * ROWH - 4, "v mas abajo", 2, C_DIM);

    rrect(96, 872, 1728, 56, 16, RGB(38, 48, 74));
    if (g_cfMsgTtl > 0 && g_cfMsg[0]) {
        rrect(96, 872, 10, 56, 5, g_cfMsgCol);
        text(126, 884, g_cfMsg, 2, g_cfMsgCol);
    } else {
        text(126, 884, "Los cambios se guardan solos y el plugin los aplica en el juego.", 2, C_DIM);
    }
    rrect(96, 944, 1728, 64, 20, RGB(30, 38, 62));
    text(126, 960, cf_footer(P.rows[sel].kind), 2, C_DIM);
    if (g_cfCapturing) {
        rrect(380, 380, 1160, 260, 30, RGB(24, 30, 50));
        rframe(380, 380, 1160, 260, 30, 4, BLUE, 255);
        char t[120];
        snprintf(t, sizeof(t), "Pulsa la nueva tecla para: %s", P.rows[sel].label);
        text(960 - text_w(t, 2) / 2, 440, t, 2, C_TXT);
        text(960 - text_w("(Shift, Ctrl y Alt tambien valen)", 2) / 2, 500, "(Shift, Ctrl y Alt tambien valen)", 2, C_DIM);
        text(960 - text_w("Esc o clic derecho: cancelar", 2) / 2, 570, "Esc o clic derecho: cancelar", 2, C_DIM);
    }
    draw_cursor();
}

// ---------------- bucle de la pantalla ----------------
static bool cf_rep(uint32_t held, uint32_t m, int &cnt, int first, int every) {
    if (!(held & m)) { cnt = 0; return false; }
    int h = cnt++;
    return h == 0 || (h >= first && (h - first) % every == 0);
}

static const int CF_VX = 880, CF_LY = 212, CF_ROWH = 64, CF_VIS = 10;

// Valor del deslizador segun la posicion x del cursor (ajustado al paso).
static void cf_slider_to(const CfRow &r, int x) {
    int bx = CF_VX + 40, bw = 520;
    long rel = x - bx;
    if (rel < 0) rel = 0;
    if (rel > bw) rel = bw;
    int span = r.mx - r.mn;
    int v = r.mn + (int)((long)span * rel / bw);
    v = r.mn + ((v - r.mn + r.st / 2) / r.st) * r.st;
    if (v > r.mx) v = r.mx;
    char cur[48], t[24];
    cf_value(r, cur, sizeof(cur));
    if (cf_parse100(cur) == v) return;
    cf_fmt100(v, t, sizeof(t));
    cf_saved(cf_set(r.key, t));
}

static void cf_begin_capture(bool &capturing) {
    capturing = true;
    g_cfCapturing = true;
    plat_capture(true);
}

static void settings_screen() {
    cf_load();
    g_cfMsg[0] = 0;
    g_cfMsgTtl = 0;
    int pg = 0, sel = 0, top = 0, slot = 0, confirm = 0;
    int cu = 0, cd = 0, cl = 0, cr = 0;
    bool capturing = false;
    int dragRow = -1, dragWait = 0;
    uint32_t prev = plat_buttons();
    plat_key_event();
    for (;;) {
        uint32_t held = plat_buttons();
        uint32_t press = held & ~prev;
        prev = held;
        int kev = plat_key_event();
        const PlatPointer *mp = plat_pointer();
        const CfPage &P = CF_PAGE[pg];
        const CfRow &R = P.rows[sel];
        bool kb = (plat_devices() & 1) != 0;

        if (capturing) {
            if (kev == 41 || (press & BTN_CIRCLE)) {
                capturing = false; g_cfCapturing = false; plat_capture(false);
                cf_say(C_DIM, "Cancelado.");
            } else if (kev) {
                char buf[8];
                const char *nm = cf_hid_name(kev, buf);
                capturing = false; g_cfCapturing = false; plat_capture(false);
                if (nm) cf_assign(R, slot, nm);
                else cf_say(C_WARN, "Esa tecla no esta soportada por el plugin.");
            }
            if (g_cfMsgTtl > 0) g_cfMsgTtl--;
            cf_draw(pg, sel, top, slot);
            plat_present();
            continue;
        }

        // ---- teclado: 1 y 2 eligen el hueco de tecla ----
        if (R.kind == RK_KEYS && kev == 30) slot = 0;
        if (R.kind == RK_KEYS && kev == 31) slot = 1;

        // ---- raton ----
        if (mp->leftPressed) {
            int mx = mp->x, my = mp->y;
            if (my >= 124 && my < 184) {
                for (int i = 0; i < CF_PAGES; i++)
                    if (mx >= 96 + i * 412 && mx < 96 + i * 412 + 392) { pg = i; sel = top = slot = 0; confirm = 0; }
            } else if (my >= CF_LY && my < CF_LY + CF_VIS * CF_ROWH && mx >= 96 && mx < 1824) {
                int idx = top + (my - CF_LY) / CF_ROWH;
                if (idx < P.n && (my - CF_LY) % CF_ROWH < 56) {
                    if (idx != sel) slot = 0;
                    sel = idx;
                    const CfRow &Q = P.rows[sel];
                    if (mx >= CF_VX) {
                        switch (Q.kind) {
                        case RK_PRESET: cf_adjust(Q, mx < CF_VX + 220 ? -1 : +1, 0); break;
                        case RK_NUM:
                            if (mx < CF_VX + 30) cf_adjust(Q, -1, 0);
                            else if (mx >= CF_VX + 580) cf_adjust(Q, +1, 0);
                            else { cf_slider_to(Q, mx); dragRow = sel; dragWait = 0; }
                            break;
                        case RK_BOOL: cf_adjust(Q, +1, 0); break;
                        case RK_KEYS:
                            slot = mx >= CF_VX + 290 ? 1 : 0;
                            if (kb) cf_begin_capture(capturing);
                            break;
                        case RK_KEY1:
                            if (mx < CF_VX + 30) cf_adjust(Q, -1, 0);
                            else if (mx >= CF_VX + 345) cf_adjust(Q, +1, 0);
                            else if (kb) cf_begin_capture(capturing);
                            break;
                        case RK_BTN:
                            cf_adjust(Q, mx < CF_VX + 30 ? -1 : +1, 0);
                            break;
                        }
                    }
                }
            }
        }
        if (dragRow >= 0) {
            if (mp->left && dragRow == sel) {
                if (++dragWait >= 3) { cf_slider_to(P.rows[sel], mp->x); dragWait = 0; }
            } else {
                if (dragRow == sel) cf_slider_to(P.rows[sel], mp->x);
                dragRow = -1;
            }
        }
        if (capturing) continue;

        if (press & BTN_CIRCLE) break;
        if (cf_rep(held, BTN_UP, cu, 18, 4)) { sel = (sel + P.n - 1) % P.n; slot = 0; }
        if (cf_rep(held, BTN_DOWN, cd, 18, 4)) { sel = (sel + 1) % P.n; slot = 0; }
        int fast = (R.kind == RK_NUM) ? 3 : 9;
        if (cf_rep(held, BTN_LEFT, cl, 18, fast)) cf_adjust(R, -1, slot);
        if (cf_rep(held, BTN_RIGHT, cr, 18, fast)) cf_adjust(R, +1, slot);
        if (press & BTN_CROSS) {
            if (R.kind == RK_KEYS || R.kind == RK_KEY1) {
                if (kb) cf_begin_capture(capturing);
                else if (R.kind == RK_KEYS) slot = 1 - slot;
            } else if (R.kind == RK_BOOL || R.kind == RK_PRESET || R.kind == RK_BTN) cf_adjust(R, +1, slot);
        }
        if (press & BTN_SQUARE) cf_clear_slot(R, slot);
        if (press & BTN_TRIANGLE) cf_reset_row(R);
        if (press & BTN_OPTIONS) {
            if (confirm > 0) { cf_reset_page(P); confirm = 0; }
            else { confirm = 60 * 4; cf_say(C_WARN, "Pulsa Options (o) otra vez para restaurar TODA esta pagina."); }
        }
        if (confirm > 0) confirm--;
        if (press & (BTN_L1 | BTN_R1)) {
            pg = (pg + (press & BTN_R1 ? 1 : CF_PAGES - 1)) % CF_PAGES;
            sel = top = slot = 0;
            confirm = 0;
        }
        const CfPage &P2 = CF_PAGE[pg];
        if (sel >= P2.n) sel = P2.n - 1;
        if (sel < top) top = sel;
        if (sel >= top + CF_VIS) top = sel - CF_VIS + 1;
        if (g_cfMsgTtl > 0) g_cfMsgTtl--;
        cf_draw(pg, sel, top, slot);
        plat_present();
    }
    plat_capture(false);
    free(g_cfg);
    g_cfg = NULL;
}
