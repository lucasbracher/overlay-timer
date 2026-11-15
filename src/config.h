#ifndef OVERLAY_TIMER_CONFIG_H
#define OVERLAY_TIMER_CONFIG_H

#include <stdbool.h>
#include "util.h"

/* Screen corner. */
typedef enum {
    CORNER_TL = 0, /* top-left */
    CORNER_TR,     /* top-right */
    CORNER_BL,     /* bottom-left */
    CORNER_BR      /* bottom-right */
} corner_t;

/* Time display format. */
typedef enum {
    FMT_MMSS = 0,
    FMT_HHMMSS
} time_format_t;

/* Chosen window backend. */
typedef enum {
    BACKEND_AUTO = 0,
    BACKEND_X11,
    BACKEND_WAYLAND,
    BACKEND_MACOS
} backend_t;

#define MAX_SCREENS 16

/*
 * Full overlay-timer configuration.
 * Loaded from the INI file and overridden by CLI flags.
 */
typedef struct {
    /* [appearance] */
    char  *color;           /* text; name or #RRGGBB */
    int    alpha;           /* 0..100 (visibility) */
    int    size;            /* 0..100 (% of the smaller dimension) */
    char  *font;            /* family (may be "") */

    /* [position] */
    corner_t corner;
    int    screens[MAX_SCREENS];
    int    n_screens;

    /* [timer] */
    time_format_t format;

    /* [pomodoro] */
    int    pomodoro_minutes;
    int    break_minutes;
    int    long_break_minutes;
    int    long_break_every;  /* 0 = no long break */
    char  *pomodoro_color;
    char  *break_color;

    /* [audio] */
    char  *sounds_dir;
    bool   enable_sound;

    /* [window] */
    backend_t backend;
} config_t;

/* Fill `cfg` with default values (equivalent to the default config.ini). */
void config_set_defaults(config_t *cfg);

/* Make a deep copy of `src` into `dst` (dst must not hold valid data;
 * if it does, call config_free first). */
void config_copy(config_t *dst, const config_t *src);

/* Free the allocated fields (does not free the pointer itself). */
void config_free(config_t *cfg);

/* Load/merge values from an INI file onto `cfg` (absent fields are kept
 * unchanged). Returns true if the file was read; false if it does not exist
 * or there was an I/O error (individual value errors produce log_warn and are
 * ignored). */
bool config_load_file(config_t *cfg, const char *path);

/* Write `cfg` to `path` in INI format (creates directories if needed).
 * Returns true on success. */
bool config_save_file(const config_t *cfg, const char *path);

/* Ensure the default config exists on disk (creates it if missing).
 * Returns true if the file exists at the end. */
bool config_ensure_default(const char *path);

/* Enum <-> string conversion helpers (for INI and CLI). */
const char *corner_to_str(corner_t c);
bool corner_from_str(const char *s, corner_t *out);
const char *format_to_str(time_format_t f);
bool format_from_str(const char *s, time_format_t *out);
const char *backend_to_str(backend_t b);
bool backend_from_str(const char *s, backend_t *out);

/* Parse a list of screen indices "0,1,2" into cfg->screens.
 * Returns true on success. */
bool parse_screen_list(const char *s, int *out, int max, int *out_n);

/* Apply a single "flat" key (color, alpha, size, font, corner, screen,
 * format, pomodoro_minutes, break_minutes, long_break_minutes,
 * long_break_every, pomodoro_color, break_color, sounds_dir, enable_sound,
 * backend) to `cfg`. Used by the IPC protocol ("SET key=value" messages).
 * Returns true if the key is known and the value is valid. */
bool config_set_kv(config_t *cfg, const char *key, const char *value);

#endif /* OVERLAY_TIMER_CONFIG_H */
