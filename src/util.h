#ifndef OVERLAY_TIMER_UTIL_H
#define OVERLAY_TIMER_UTIL_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* fopen() mode suffix for close-on-exec. Windows' UCRT rejects the 'e' flag
 * (fopen returns NULL), so it is empty there. Usage: fopen(p, "r" FOPEN_CLOEXEC). */
#ifdef _WIN32
#define FOPEN_CLOEXEC ""
#else
#define FOPEN_CLOEXEC "e"
#endif

/* ------------------------------------------------------------------ */
/* Logging                                                            */
/* ------------------------------------------------------------------ */

typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR
} log_level_t;

/* Set the minimum level displayed (default: LOG_INFO). */
void log_set_level(log_level_t level);

/* Redirect logs to a file (e.g. after daemonizing).
 * Pass NULL to go back to stderr. Returns true on success. */
bool log_set_file(const char *path);

void log_msg(log_level_t level, const char *fmt, ...);

#define log_debug(...) log_msg(LOG_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_INFO, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_WARN, __VA_ARGS__)
#define log_error(...) log_msg(LOG_ERROR, __VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Strings                                                            */
/* ------------------------------------------------------------------ */

/* Safe strdup that aborts on out of memory. */
char *xstrdup(const char *s);

/* malloc/realloc that abort on out of memory. */
void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);

/* Trim leading/trailing whitespace (in-place). Returns the pointer. */
char *str_trim(char *s);

/* Case-insensitive comparison. */
bool str_ieq(const char *a, const char *b);

/* Portable strtok_r (not guaranteed on Windows). */
char *str_tok(char *s, const char *delim, char **save);

/* Portable localtime_r (localtime_s on Windows). Returns `out` or NULL. */
struct tm *local_time(const time_t *t, struct tm *out);

/* ------------------------------------------------------------------ */
/* XDG paths                                                          */
/* ------------------------------------------------------------------ */

/* Expand a leading '~' to the home directory. Returns an allocated string
 * (caller frees). If it does not start with '~', returns a copy of `path`. */
char *path_expand_tilde(const char *path);

/* XDG base directories. All return an allocated string (caller frees). */
char *xdg_config_home(void);  /* $XDG_CONFIG_HOME  or ~/.config          */
char *xdg_data_home(void);    /* $XDG_DATA_HOME    or ~/.local/share     */
char *xdg_state_home(void);   /* $XDG_STATE_HOME   or ~/.local/state     */
char *xdg_runtime_dir(void);  /* $XDG_RUNTIME_DIR, $TMPDIR or /tmp fallback */

/* Full path of the default config: <config_home>/overlay-timer/config.ini */
char *default_config_path(void);

/* Create a directory tree (like mkdir -p). Returns true on success. */
bool mkdirs(const char *path, unsigned int mode);

/* ------------------------------------------------------------------ */
/* Time and color parsing                                             */
/* ------------------------------------------------------------------ */

/* Parse "MM:SS" or "HH:MM:SS" into total seconds.
 * Returns true on success; *out receives the seconds. */
bool parse_time(const char *s, long *out_seconds);

/* Format seconds as "HH:MM:SS" (force_hh=true) or choose automatically:
 * uses "HH:MM:SS" if there are hours, otherwise "MM:SS".
 * Writes into buf (size bufsz). Returns buf. */
char *format_time(long seconds, bool force_hh, char *buf, size_t bufsz);

typedef struct {
    uint8_t r, g, b;
} rgb_t;

/* Parse a color: a known name ("red", "green", ...) or "#RRGGBB".
 * Returns true on success. */
bool parse_color(const char *s, rgb_t *out);

#endif /* OVERLAY_TIMER_UTIL_H */
