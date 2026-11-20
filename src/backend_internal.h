#ifndef OVERLAY_TIMER_BACKEND_INTERNAL_H
#define OVERLAY_TIMER_BACKEND_INTERNAL_H

#include "backend.h"

/*
 * Common infrastructure for the concrete backends.
 *
 * Each backend (X11, Wayland, macOS, Win32) implements a set of static functions and
 * exposes a `backend_vtable_t`. The `overlay_backend_t` wrapper holds the vtable
 * and an opaque pointer to the backend-specific state. The public functions
 * forward through the vtable. This lets both backends coexist in the same
 * binary without symbol collisions.
 *
 * Screen handling: each backend keeps the *requested* screen list and can
 * rebuild its overlays from a fresh monitor enumeration at any time, either
 * because the daemon asked (set_screens) or because it detected a topology
 * change (marked with topo_mark() from its event handlers and applied, once
 * the burst of events settles, from tick()). The wrapper caches the last
 * frame and re-renders it after every rebuild.
 */

typedef struct backend_vtable {
    int  (*get_fd)(void *impl);
    void (*pump)(void *impl);
    void (*render)(void *impl, const overlay_frame_t *frame);
    /* Store the requested screens and rebuild now; fill *out. */
    void (*set_screens)(void *impl, const int *screens, int n,
                        screen_layout_t *out);
    /* Periodic work; returns true if the overlays were rebuilt. */
    bool (*tick)(void *impl);
    void (*destroy)(void *impl);
    const char *name;
} backend_vtable_t;

struct overlay_backend {
    const backend_vtable_t *vt;
    void *impl;

    /* Last frame rendered (owned copies of the strings). */
    overlay_frame_t last;
    char *last_text, *last_label;
    bool has_last;
};

/* Allocates a wrapper with the given vtable and impl. */
overlay_backend_t *backend_wrap(const backend_vtable_t *vt, void *impl);

/* Monotonic clock in milliseconds. */
long backend_now_ms(void);

/* Debounce for monitor topology changes: event handlers call topo_mark();
 * topo_due() returns true (once) after no new mark for TOPO_DEBOUNCE_MS. */
#define TOPO_DEBOUNCE_MS 300
typedef struct {
    bool pending;
    long since_ms;
} topo_watch_t;

void topo_mark(topo_watch_t *t);
bool topo_due(topo_watch_t *t);

/* Logs the layout chosen by a (re)build. */
void backend_log_layout(const char *backend, const int *req, int nreq,
                        const screen_layout_t *l);

/* Concrete constructors. */
#ifdef WITH_X11
overlay_backend_t *backend_create_x11(const config_t *cfg, char **err);
#endif
#ifdef WITH_WAYLAND
overlay_backend_t *backend_create_wayland(const config_t *cfg, char **err);
#endif
#ifdef WITH_MACOS
overlay_backend_t *backend_create_macos(const config_t *cfg, char **err);
#endif
#ifdef WITH_WIN32
overlay_backend_t *backend_create_win32(const config_t *cfg, char **err);
#endif

#endif /* OVERLAY_TIMER_BACKEND_INTERNAL_H */
