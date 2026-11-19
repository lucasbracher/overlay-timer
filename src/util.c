#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h> /* _mkdir */
#else
#include <pwd.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

static log_level_t g_log_level = LOG_INFO;
static FILE *g_log_fp = NULL; /* NULL => stderr */

void log_set_level(log_level_t level) { g_log_level = level; }

bool log_set_file(const char *path) {
    FILE *fp = fopen(path, "a" FOPEN_CLOEXEC);
    if (!fp) return false;
    if (g_log_fp) fclose(g_log_fp);
    g_log_fp = fp;
    return true;
}

static const char *level_name(log_level_t level) {
    switch (level) {
        case LOG_DEBUG: return "DEBUG";
        case LOG_INFO:  return "INFO";
        case LOG_WARN:  return "WARN";
        case LOG_ERROR: return "ERROR";
        default:        return "?";
    }
}

void log_msg(log_level_t level, const char *fmt, ...) {
    if (level < g_log_level) return;

    FILE *fp = g_log_fp ? g_log_fp : stderr;

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    local_time(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);

    fprintf(fp, "[%s] %-5s ", ts, level_name(level));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);

    fputc('\n', fp);
    fflush(fp);
}

/* ------------------------------------------------------------------ */
/* Strings / memory                                                   */
/* ------------------------------------------------------------------ */

static void oom(void) {
    fputs("overlay-timer: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) oom();
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) oom();
    return q;
}

char *xstrdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *str_trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

bool str_ieq(const char *a, const char *b) {
    if (!a || !b) return a == b;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return *a == *b;
}

char *str_tok(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    if (!s) return NULL;
    s += strspn(s, delim);
    if (!*s) { *save = NULL; return NULL; }
    char *end = s + strcspn(s, delim);
    if (*end) { *end = '\0'; *save = end + 1; }
    else      { *save = NULL; }
    return s;
}

struct tm *local_time(const time_t *t, struct tm *out) {
#ifdef _WIN32
    return localtime_s(out, t) == 0 ? out : NULL;
#else
    return localtime_r(t, out);
#endif
}

/* ------------------------------------------------------------------ */
/* XDG paths                                                          */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
/*
 * Windows: paths come from %USERPROFILE%, %APPDATA% and %LOCALAPPDATA%.
 * Backslashes are converted to '/', which the Windows APIs accept, so the
 * rest of the code can keep joining paths with '/'.
 */
static char *win_env_path(const char *env) {
    const char *v = getenv(env);
    if (!v || !*v) return NULL;
    char *out = xstrdup(v);
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    size_t len = strlen(out);
    while (len > 3 && out[len - 1] == '/') out[--len] = '\0';
    return out;
}

/* Absolute path: "C:/..." or "//server/...". */
static bool is_abs_path(const char *p) {
    if (!p) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return isalpha((unsigned char)p[0]) && p[1] == ':' &&
           (p[2] == '/' || p[2] == '\\');
}

static const char *home_dir(void) {
    static char *home = NULL;
    if (!home) home = win_env_path("USERPROFILE");
    if (!home) home = xstrdup("C:/");
    return home;
}
#else
static bool is_abs_path(const char *p) { return p && p[0] == '/'; }

static const char *home_dir(void) {
    const char *h = getenv("HOME");
    if (h && *h) return h;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_dir) ? pw->pw_dir : "/tmp";
}
#endif

char *path_expand_tilde(const char *path) {
    if (!path) return NULL;
    if (path[0] != '~') return xstrdup(path);

    /* Supports only "~" and "~/..." (not "~user"). */
    if (path[1] != '\0' && path[1] != '/') return xstrdup(path);

    const char *home = home_dir();
    const char *rest = path + 1; /* includes the '/' if present */
    size_t n = strlen(home) + strlen(rest) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s%s", home, rest);
    return out;
}

static char *xdg_dir(const char *env, const char *fallback_rel) {
    const char *v = getenv(env);
    if (is_abs_path(v)) return xstrdup(v);

    const char *home = home_dir();
    size_t n = strlen(home) + 1 + strlen(fallback_rel) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s/%s", home, fallback_rel);
    return out;
}

#ifdef _WIN32
/* An XDG_* variable still wins (handy for tests); otherwise the Windows
 * folder from `win_env`, falling back to ~/<fallback_rel>. */
static char *win_dir(const char *xdg_env, const char *win_env,
                     const char *fallback_rel) {
    const char *v = getenv(xdg_env);
    if (is_abs_path(v)) return xstrdup(v);
    char *w = win_env_path(win_env);
    if (w) return w;
    return xdg_dir(xdg_env, fallback_rel);
}

char *xdg_config_home(void) { return win_dir("XDG_CONFIG_HOME", "APPDATA", "AppData/Roaming"); }
char *xdg_data_home(void)   { return win_dir("XDG_DATA_HOME", "LOCALAPPDATA", "AppData/Local"); }
char *xdg_state_home(void)  { return win_dir("XDG_STATE_HOME", "LOCALAPPDATA", "AppData/Local"); }

char *xdg_runtime_dir(void) {
    char *base = win_dir("XDG_RUNTIME_DIR", "LOCALAPPDATA", "AppData/Local");
    size_t n = strlen(base) + sizeof "/overlay-timer";
    char *out = xmalloc(n);
    snprintf(out, n, "%s/overlay-timer", base);
    free(base);
    return out;
}
#else
char *xdg_config_home(void) { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
char *xdg_data_home(void)   { return xdg_dir("XDG_DATA_HOME", ".local/share"); }
char *xdg_state_home(void)  { return xdg_dir("XDG_STATE_HOME", ".local/state"); }

char *xdg_runtime_dir(void) {
    const char *v = getenv("XDG_RUNTIME_DIR");
    if (v && v[0] == '/') return xstrdup(v);
#ifdef __APPLE__
    /* macOS has no XDG_RUNTIME_DIR; $TMPDIR is per-user and private. */
    const char *t = getenv("TMPDIR");
    if (t && t[0] == '/') {
        size_t len = strlen(t);
        while (len > 1 && t[len - 1] == '/') len--;
        char *out = xmalloc(len + sizeof "/overlay-timer");
        memcpy(out, t, len);
        strcpy(out + len, "/overlay-timer");
        return out;
    }
#endif
    /* Insecure but functional fallback: /tmp/overlay-timer-<uid> */
    char buf[64];
    snprintf(buf, sizeof buf, "/tmp/overlay-timer-%u", (unsigned)getuid());
    return xstrdup(buf);
}
#endif

char *default_config_path(void) {
    char *cfg = xdg_config_home();
    const char *rel = "/overlay-timer/config.ini";
    size_t n = strlen(cfg) + strlen(rel) + 1;
    char *out = xmalloc(n);
    snprintf(out, n, "%s%s", cfg, rel);
    free(cfg);
    return out;
}

#ifdef _WIN32
static int make_dir(const char *p, unsigned int mode) { (void)mode; return _mkdir(p); }
static bool is_sep(char c) { return c == '/' || c == '\\'; }
#else
static int make_dir(const char *p, unsigned int mode) { return mkdir(p, (mode_t)mode); }
static bool is_sep(char c) { return c == '/'; }
#endif

bool mkdirs(const char *path, unsigned int mode) {
    if (!path || !*path) return false;

    char *tmp = xstrdup(path);
    size_t len = strlen(tmp);
    if (len > 1 && is_sep(tmp[len - 1])) tmp[len - 1] = '\0';

    /* Skip the root ("/" or, on Windows, a drive like "C:/"). */
    char *start = tmp + 1;
#ifdef _WIN32
    if (isalpha((unsigned char)tmp[0]) && tmp[1] == ':') start = tmp + 3;
    if (start > tmp + strlen(tmp)) start = tmp + strlen(tmp);
#endif

    bool ok = true;
    for (char *p = start; *p; p++) {
        if (is_sep(*p)) {
            char c = *p;
            *p = '\0';
            if (make_dir(tmp, mode) != 0 && errno != EEXIST) { ok = false; break; }
            *p = c;
        }
    }
    if (ok && make_dir(tmp, mode) != 0 && errno != EEXIST) ok = false;

    free(tmp);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Time                                                               */
/* ------------------------------------------------------------------ */

bool parse_time(const char *s, long *out_seconds) {
    if (!s || !out_seconds) return false;

    long parts[3] = {0, 0, 0};
    int nparts = 0;
    const char *p = s;

    while (*p && nparts < 3) {
        if (!isdigit((unsigned char)*p)) return false;
        long v = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            if (v > 1000000L) return false; /* sanity */
            p++;
        }
        parts[nparts++] = v;
        if (*p == ':') {
            p++;
            if (*p == '\0') return false; /* ':' at the end */
        } else if (*p != '\0') {
            return false;
        }
    }
    if (*p != '\0' || nparts == 0) return false;

    long total;
    if (nparts == 1) {
        total = parts[0];                 /* SS */
    } else if (nparts == 2) {
        if (parts[1] >= 60) return false; /* MM:SS */
        total = parts[0] * 60 + parts[1];
    } else {
        if (parts[1] >= 60 || parts[2] >= 60) return false; /* HH:MM:SS */
        total = parts[0] * 3600 + parts[1] * 60 + parts[2];
    }
    *out_seconds = total;
    return true;
}

char *format_time(long seconds, bool force_hh, char *buf, size_t bufsz) {
    if (seconds < 0) seconds = 0;
    long h = seconds / 3600;
    long m = (seconds % 3600) / 60;
    long sx = seconds % 60;

    if (force_hh || h > 0)
        snprintf(buf, bufsz, "%02ld:%02ld:%02ld", h, m, sx);
    else
        snprintf(buf, bufsz, "%02ld:%02ld", m, sx);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Color                                                              */
/* ------------------------------------------------------------------ */

struct named_color {
    const char *name;
    rgb_t rgb;
};

static const struct named_color NAMED_COLORS[] = {
    {"red",     {0xff, 0x00, 0x00}},
    {"green",   {0x00, 0x80, 0x00}},
    {"lime",    {0x00, 0xff, 0x00}},
    {"blue",    {0x00, 0x00, 0xff}},
    {"yellow",  {0xff, 0xff, 0x00}},
    {"orange",  {0xff, 0xa5, 0x00}},
    {"cyan",    {0x00, 0xff, 0xff}},
    {"magenta", {0xff, 0x00, 0xff}},
    {"white",   {0xff, 0xff, 0xff}},
    {"black",   {0x00, 0x00, 0x00}},
    {"gray",    {0x80, 0x80, 0x80}},
    {"grey",    {0x80, 0x80, 0x80}},
};

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_color(const char *s, rgb_t *out) {
    if (!s || !out) return false;

    if (s[0] == '#') {
        const char *h = s + 1;
        size_t len = strlen(h);
        if (len == 6) {
            int v[6];
            for (int i = 0; i < 6; i++) {
                v[i] = hexval(h[i]);
                if (v[i] < 0) return false;
            }
            out->r = (uint8_t)(v[0] * 16 + v[1]);
            out->g = (uint8_t)(v[2] * 16 + v[3]);
            out->b = (uint8_t)(v[4] * 16 + v[5]);
            return true;
        }
        if (len == 3) {
            int v[3];
            for (int i = 0; i < 3; i++) {
                v[i] = hexval(h[i]);
                if (v[i] < 0) return false;
            }
            out->r = (uint8_t)(v[0] * 17);
            out->g = (uint8_t)(v[1] * 17);
            out->b = (uint8_t)(v[2] * 17);
            return true;
        }
        return false;
    }

    for (size_t i = 0; i < sizeof NAMED_COLORS / sizeof NAMED_COLORS[0]; i++) {
        if (str_ieq(s, NAMED_COLORS[i].name)) {
            *out = NAMED_COLORS[i].rgb;
            return true;
        }
    }
    return false;
}
