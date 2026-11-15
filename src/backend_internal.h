#ifndef OVERLAY_TIMER_BACKEND_INTERNAL_H
#define OVERLAY_TIMER_BACKEND_INTERNAL_H

#include "backend.h"

/*
 * Common infrastructure for the concrete backends.
 *
 * Each backend (X11, Wayland) implements a set of static functions and
 * exposes a `backend_vtable_t`. The `overlay_backend_t` wrapper holds the vtable
 * and an opaque pointer to the backend-specific state. The public functions
 * forward through the vtable. This lets both backends coexist in the same
 * binary without symbol collisions.
 */

typedef struct backend_vtable {
    int  (*get_fd)(void *impl);
    void (*pump)(void *impl);
    void (*render)(void *impl, const overlay_frame_t *frame);
    void (*destroy)(void *impl);
    const char *name;
} backend_vtable_t;

struct overlay_backend {
    const backend_vtable_t *vt;
    void *impl;
};

/* Allocates a wrapper with the given vtable and impl. */
overlay_backend_t *backend_wrap(const backend_vtable_t *vt, void *impl);

/* Construtores concretos. */
#ifdef WITH_X11
overlay_backend_t *backend_create_x11(const config_t *cfg, char **err);
#endif
#ifdef WITH_WAYLAND
overlay_backend_t *backend_create_wayland(const config_t *cfg, char **err);
#endif

#endif /* OVERLAY_TIMER_BACKEND_INTERNAL_H */
