#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

/* ------------------------------------------------------------------ */
/* Defaults                                                           */
/* ------------------------------------------------------------------ */

void config_set_defaults(config_t *cfg) {
    memset(cfg, 0, sizeof *cfg);

    cfg->color = xstrdup("red");
    cfg->alpha = 20;
    cfg->size = 20;
    cfg->font = xstrdup("");

    cfg->corner = CORNER_BR;
    cfg->screens[0] = 0;
    cfg->n_screens = 1;

    cfg->format = FMT_MMSS;

    cfg->pomodoro_minutes = 25;
    cfg->break_minutes = 5;
    cfg->long_break_minutes = 15;
    cfg->long_break_every = 0;
    cfg->pomodoro_color = xstrdup("red");
    cfg->break_color = xstrdup("green");

    cfg->sounds_dir = xstrdup("~/.local/share/overlay-timer/sounds");
    cfg->enable_sound = true;

    cfg->backend = BACKEND_AUTO;
}

void config_copy(config_t *dst, const config_t *src) {
    *dst = *src;
    dst->color = xstrdup(src->color);
    dst->font = xstrdup(src->font);
    dst->pomodoro_color = xstrdup(src->pomodoro_color);
    dst->break_color = xstrdup(src->break_color);
    dst->sounds_dir = xstrdup(src->sounds_dir);
}

void config_free(config_t *cfg) {
    if (!cfg) return;
    free(cfg->color);
    free(cfg->font);
    free(cfg->pomodoro_color);
    free(cfg->break_color);
    free(cfg->sounds_dir);
    cfg->color = cfg->font = cfg->pomodoro_color = NULL;
    cfg->break_color = cfg->sounds_dir = NULL;
}

/* ------------------------------------------------------------------ */
/* Enum <-> string conversions                                        */
/* ------------------------------------------------------------------ */

const char *corner_to_str(corner_t c) {
    switch (c) {
        case CORNER_TL: return "tl";
        case CORNER_TR: return "tr";
        case CORNER_BL: return "bl";
        case CORNER_BR: return "br";
        default:        return "br";
    }
}

bool corner_from_str(const char *s, corner_t *out) {
    if (!s) return false;
    if (str_ieq(s, "tl")) { *out = CORNER_TL; return true; }
    if (str_ieq(s, "tr")) { *out = CORNER_TR; return true; }
    if (str_ieq(s, "bl")) { *out = CORNER_BL; return true; }
    if (str_ieq(s, "br")) { *out = CORNER_BR; return true; }
    return false;
}

const char *format_to_str(time_format_t f) {
    return f == FMT_HHMMSS ? "hhmmss" : "mmss";
}

bool format_from_str(const char *s, time_format_t *out) {
    if (!s) return false;
    if (str_ieq(s, "hhmmss")) { *out = FMT_HHMMSS; return true; }
    if (str_ieq(s, "mmss"))   { *out = FMT_MMSS; return true; }
    return false;
}

const char *backend_to_str(backend_t b) {
    switch (b) {
        case BACKEND_X11:     return "x11";
        case BACKEND_WAYLAND: return "wayland";
        case BACKEND_MACOS:   return "macos";
        case BACKEND_WIN32:   return "win32";
        case BACKEND_AUTO:
        default:              return "auto";
    }
}

bool backend_from_str(const char *s, backend_t *out) {
    if (!s) return false;
    if (str_ieq(s, "auto"))    { *out = BACKEND_AUTO; return true; }
    if (str_ieq(s, "x11"))     { *out = BACKEND_X11; return true; }
    if (str_ieq(s, "wayland")) { *out = BACKEND_WAYLAND; return true; }
    if (str_ieq(s, "macos"))   { *out = BACKEND_MACOS; return true; }
    if (str_ieq(s, "win32"))   { *out = BACKEND_WIN32; return true; }
    return false;
}

bool parse_screen_list(const char *s, int *out, int max, int *out_n) {
    if (!s || !out || !out_n) return false;
    int n = 0;
    const char *p = s;
    while (*p) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (!*p) break;
        if (!isdigit((unsigned char)*p)) return false;
        long v = 0;
        while (isdigit((unsigned char)*p)) { v = v * 10 + (*p - '0'); p++; }
        if (n >= max) return false;
        out[n++] = (int)v;
        while (isspace((unsigned char)*p)) p++;
        if (*p && *p != ',') return false;
    }
    if (n == 0) return false;
    *out_n = n;
    return true;
}

/* ------------------------------------------------------------------ */
/* INI parser                                                         */
/* ------------------------------------------------------------------ */

static bool parse_bool(const char *s, bool *out) {
    if (str_ieq(s, "true") || str_ieq(s, "yes") || str_ieq(s, "1") ||
        str_ieq(s, "on")) { *out = true; return true; }
    if (str_ieq(s, "false") || str_ieq(s, "no") || str_ieq(s, "0") ||
        str_ieq(s, "off")) { *out = false; return true; }
    return false;
}

static bool parse_int_range(const char *s, int lo, int hi, int *out) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) return false;
    if (v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

static void set_str(char **dst, const char *val) {
    free(*dst);
    *dst = xstrdup(val);
}

bool config_set_kv(config_t *cfg, const char *key, const char *value) {
    if (!cfg || !key || !value) return false;

    if (str_ieq(key, "color")) { set_str(&cfg->color, value); return true; }
    if (str_ieq(key, "font"))  { set_str(&cfg->font, value); return true; }
    if (str_ieq(key, "pomodoro_color")) { set_str(&cfg->pomodoro_color, value); return true; }
    if (str_ieq(key, "break_color"))    { set_str(&cfg->break_color, value); return true; }
    if (str_ieq(key, "sounds_dir"))     { set_str(&cfg->sounds_dir, value); return true; }

    if (str_ieq(key, "alpha")) { int v; if (!parse_int_range(value, 0, 100, &v)) return false; cfg->alpha = v; return true; }
    if (str_ieq(key, "size"))  { int v; if (!parse_int_range(value, 0, 100, &v)) return false; cfg->size = v; return true; }

    if (str_ieq(key, "corner"))  return corner_from_str(value, &cfg->corner);
    if (str_ieq(key, "format"))  return format_from_str(value, &cfg->format);
    if (str_ieq(key, "backend")) return backend_from_str(value, &cfg->backend);

    if (str_ieq(key, "screen")) {
        int tmp[MAX_SCREENS], n;
        if (!parse_screen_list(value, tmp, MAX_SCREENS, &n)) return false;
        memcpy(cfg->screens, tmp, sizeof(int) * n);
        cfg->n_screens = n;
        return true;
    }

    if (str_ieq(key, "pomodoro_minutes"))   { int v; if (!parse_int_range(value, 1, 1440, &v)) return false; cfg->pomodoro_minutes = v; return true; }
    if (str_ieq(key, "break_minutes"))      { int v; if (!parse_int_range(value, 0, 1440, &v)) return false; cfg->break_minutes = v; return true; }
    if (str_ieq(key, "long_break_minutes")) { int v; if (!parse_int_range(value, 0, 1440, &v)) return false; cfg->long_break_minutes = v; return true; }
    if (str_ieq(key, "long_break_every"))   { int v; if (!parse_int_range(value, 0, 100, &v)) return false; cfg->long_break_every = v; return true; }

    if (str_ieq(key, "enable_sound")) { bool b; if (!parse_bool(value, &b)) return false; cfg->enable_sound = b; return true; }

    return false;
}

/* Apply a key=value from a given section to the config. */
static void apply_kv(config_t *cfg, const char *section, const char *key,
                     const char *val, const char *path, int line) {
#define WARN_BAD() \
    log_warn(_("%s:%d: invalid value for '%s': '%s'"), path, line, key, val)

    if (str_ieq(section, "appearance")) {
        if (str_ieq(key, "color")) { set_str(&cfg->color, val); return; }
        if (str_ieq(key, "alpha")) {
            int v; if (parse_int_range(val, 0, 100, &v)) cfg->alpha = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "size")) {
            int v; if (parse_int_range(val, 0, 100, &v)) cfg->size = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "font")) { set_str(&cfg->font, val); return; }
    } else if (str_ieq(section, "position")) {
        if (str_ieq(key, "corner")) {
            corner_t c; if (corner_from_str(val, &c)) cfg->corner = c; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "screen")) {
            int tmp[MAX_SCREENS], n;
            if (parse_screen_list(val, tmp, MAX_SCREENS, &n)) {
                memcpy(cfg->screens, tmp, sizeof(int) * n);
                cfg->n_screens = n;
            } else WARN_BAD();
            return;
        }
    } else if (str_ieq(section, "timer")) {
        if (str_ieq(key, "format")) {
            time_format_t f; if (format_from_str(val, &f)) cfg->format = f; else WARN_BAD();
            return;
        }
    } else if (str_ieq(section, "pomodoro")) {
        if (str_ieq(key, "pomodoro_minutes")) {
            int v; if (parse_int_range(val, 1, 1440, &v)) cfg->pomodoro_minutes = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "break_minutes")) {
            int v; if (parse_int_range(val, 0, 1440, &v)) cfg->break_minutes = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "long_break_minutes")) {
            int v; if (parse_int_range(val, 0, 1440, &v)) cfg->long_break_minutes = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "long_break_every")) {
            int v; if (parse_int_range(val, 0, 100, &v)) cfg->long_break_every = v; else WARN_BAD();
            return;
        }
        if (str_ieq(key, "pomodoro_color")) { set_str(&cfg->pomodoro_color, val); return; }
        if (str_ieq(key, "break_color")) { set_str(&cfg->break_color, val); return; }
    } else if (str_ieq(section, "audio")) {
        if (str_ieq(key, "sounds_dir")) { set_str(&cfg->sounds_dir, val); return; }
        if (str_ieq(key, "enable_sound")) {
            bool b; if (parse_bool(val, &b)) cfg->enable_sound = b; else WARN_BAD();
            return;
        }
    } else if (str_ieq(section, "window")) {
        if (str_ieq(key, "backend")) {
            backend_t b; if (backend_from_str(val, &b)) cfg->backend = b; else WARN_BAD();
            return;
        }
    }

    log_warn(_("%s:%d: unknown key '%s' in section '[%s]'"), path, line, key,
             section);
#undef WARN_BAD
}

bool config_load_file(config_t *cfg, const char *path) {
    char *rpath = path_expand_tilde(path);
    FILE *fp = fopen(rpath, "r" FOPEN_CLOEXEC);
    if (!fp) { free(rpath); return false; }

    char section[64] = "";
    char line[1024];
    int lineno = 0;

    while (fgets(line, sizeof line, fp)) {
        lineno++;
        char *s = str_trim(line);
        if (*s == '\0' || *s == ';' || *s == '#') continue;

        if (*s == '[') {
            char *end = strchr(s, ']');
            if (!end) { log_warn(_("%s:%d: malformed section"), path, lineno); continue; }
            *end = '\0';
            snprintf(section, sizeof section, "%s", str_trim(s + 1));
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) { log_warn(_("%s:%d: missing '='"), path, lineno); continue; }
        *eq = '\0';
        char *key = str_trim(s);
        char *val = str_trim(eq + 1);
        apply_kv(cfg, section, key, val, path, lineno);
    }

    fclose(fp);
    free(rpath);
    return true;
}

/* ------------------------------------------------------------------ */
/* INI writing                                                        */
/* ------------------------------------------------------------------ */

static void write_screens(FILE *fp, const config_t *cfg) {
    fputs("screen = ", fp);
    for (int i = 0; i < cfg->n_screens; i++)
        fprintf(fp, "%s%d", i ? "," : "", cfg->screens[i]);
    fputc('\n', fp);
}

bool config_save_file(const config_t *cfg, const char *path) {
    char *rpath = path_expand_tilde(path);

    /* Ensure the parent directory. */
    char *slash = strrchr(rpath, '/');
    if (slash && slash != rpath) {
        *slash = '\0';
        mkdirs(rpath, 0755);
        *slash = '/';
    }

    FILE *fp = fopen(rpath, "w" FOPEN_CLOEXEC);
    if (!fp) { free(rpath); return false; }

    fputs("; overlay-timer — configuration file\n", fp);
    fputs("; Automatically generated. May be edited by hand.\n\n", fp);

    fputs("[appearance]\n", fp);
    fprintf(fp, "color = %s\n", cfg->color ? cfg->color : "");
    fprintf(fp, "alpha = %d\n", cfg->alpha);
    fprintf(fp, "size = %d\n", cfg->size);
    fprintf(fp, "font = %s\n\n", cfg->font ? cfg->font : "");

    fputs("[position]\n", fp);
    fprintf(fp, "corner = %s\n", corner_to_str(cfg->corner));
    write_screens(fp, cfg);
    fputc('\n', fp);

    fputs("[timer]\n", fp);
    fprintf(fp, "format = %s\n\n", format_to_str(cfg->format));

    fputs("[pomodoro]\n", fp);
    fprintf(fp, "pomodoro_minutes = %d\n", cfg->pomodoro_minutes);
    fprintf(fp, "break_minutes = %d\n", cfg->break_minutes);
    fprintf(fp, "long_break_minutes = %d\n", cfg->long_break_minutes);
    fprintf(fp, "long_break_every = %d\n", cfg->long_break_every);
    fprintf(fp, "pomodoro_color = %s\n", cfg->pomodoro_color ? cfg->pomodoro_color : "");
    fprintf(fp, "break_color = %s\n\n", cfg->break_color ? cfg->break_color : "");

    fputs("[audio]\n", fp);
    fprintf(fp, "sounds_dir = %s\n", cfg->sounds_dir ? cfg->sounds_dir : "");
    fprintf(fp, "enable_sound = %s\n\n", cfg->enable_sound ? "true" : "false");

    fputs("[window]\n", fp);
    fprintf(fp, "backend = %s\n", backend_to_str(cfg->backend));

    fclose(fp);
    free(rpath);
    return true;
}

bool config_ensure_default(const char *path) {
    char *rpath = path_expand_tilde(path);
    FILE *fp = fopen(rpath, "r" FOPEN_CLOEXEC);
    if (fp) { fclose(fp); free(rpath); return true; }
    free(rpath);

    config_t def;
    config_set_defaults(&def);
    bool ok = config_save_file(&def, path);
    config_free(&def);
    if (ok) log_info(_("created default config at %s"), path);
    return ok;
}
