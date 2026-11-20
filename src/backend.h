#ifndef OVERLAY_TIMER_BACKEND_H
#define OVERLAY_TIMER_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include "config.h"
#include "util.h"

/*
 * Abstract window backend interface (X11 / Wayland / macOS / Win32).
 *
 * The daemon creates a backend, integrates its fd into its poll() and, on each
 * tick or event, calls backend_render() with the current frame. The backend draws
 * the translucent, click-through text in the corner(s) of the selected screen(s).
 */

/* Content to draw at a given instant. */
typedef struct {
    const char *text;   /* main counter, e.g. "24:59" */
    const char *label;  /* small label (e.g. "2/4"); NULL/"" = none */
    rgb_t color;        /* text color */
    int   alpha;        /* visibility 0..100 */
    int   size;         /* % of the screen's smaller dimension */
    const char *font;   /* family (may be "") */
    corner_t corner;    /* corner */
    bool  visible;      /* false = frame off (blink) */
} overlay_frame_t;

typedef struct overlay_backend overlay_backend_t;

/* Creates the concrete backend for `which` (AUTO/X11/WAYLAND), using the screens
 * from `cfg->screens`. On error returns NULL and (if err!=NULL) *err receives an
 * allocated string with the message (caller frees). */
overlay_backend_t *backend_create(backend_t which, const config_t *cfg,
                                  char **err);

/* Returns an fd that becomes readable when there are events to process (for poll).
 * Returns -1 if the backend does not expose an fd. */
int backend_get_fd(overlay_backend_t *b);

/* Processes pending server events (e.g. Expose). Redraws if needed
 * using the last frame. */
void backend_pump_events(overlay_backend_t *b);

/* Draws `frame` on all screens. */
void backend_render(overlay_backend_t *b, const overlay_frame_t *frame);

/* Runs time-based backend work (debounced rebuild after a monitor
 * connect/disconnect). Call on every daemon loop iteration. */
void backend_tick(overlay_backend_t *b);

/* ------------------------- Screen selection ------------------------- */

/* Which monitors are actually used, given the requested screen indices. */
typedef struct {
    int  n_monitors;          /* monitors currently available */
    int  n_shown;             /* overlays created */
    int  shown[MAX_SCREENS];  /* monitor index of each overlay */
    bool fallback;            /* no requested screen exists: showing on 0 */
} screen_layout_t;

/* Pure: requested ∩ [0, nmon), deduplicated, in request order. If that is
 * empty and nmon > 0, falls back to screen 0. */
void backend_resolve_screens(const int *req, int nreq, int nmon,
                             screen_layout_t *out);

/* Human-readable summary of a layout, e.g.
 * "showing on screen(s) 0 (1 monitor(s)); unavailable: 1". */
void backend_describe_layout(const int *req, int nreq,
                             const screen_layout_t *l, char *buf, size_t sz);

/* Replaces the requested screen list and rebuilds the overlays now (the
 * timer keeps running). `msg` receives a summary for the IPC reply.
 * Returns the number of overlays shown. */
int backend_set_screens(overlay_backend_t *b, const int *screens, int n,
                        char *msg, size_t msgsz);

/* Destroys the backend and frees resources. */
void backend_destroy(overlay_backend_t *b);

/* Readable name of the backend actually selected. */
const char *backend_name(overlay_backend_t *b);

#endif /* OVERLAY_TIMER_BACKEND_H */
