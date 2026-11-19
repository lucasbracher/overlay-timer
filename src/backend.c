#include "backend.h"
#include "backend_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i18n.h"

static char *dupstr(const char *s) { return s ? strdup(s) : NULL; }

overlay_backend_t *backend_wrap(const backend_vtable_t *vt, void *impl) {
    overlay_backend_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->vt = vt;
    b->impl = impl;
    return b;
}

/* Detects the session type from the environment. */
static backend_t detect_backend(void) {
#ifdef WITH_MACOS
    return BACKEND_MACOS;
#endif
#ifdef WITH_WIN32
    return BACKEND_WIN32;
#endif
    const char *stype = getenv("XDG_SESSION_TYPE");
    if (stype) {
        if (strcmp(stype, "wayland") == 0) return BACKEND_WAYLAND;
        if (strcmp(stype, "x11") == 0) return BACKEND_X11;
    }
    if (getenv("WAYLAND_DISPLAY")) return BACKEND_WAYLAND;
    if (getenv("DISPLAY")) return BACKEND_X11;
    return BACKEND_X11;
}

overlay_backend_t *backend_create(backend_t which, const config_t *cfg,
                                  char **err) {
    if (err) *err = NULL;

    backend_t target = (which == BACKEND_AUTO) ? detect_backend() : which;

    if (target == BACKEND_WIN32) {
#ifdef WITH_WIN32
        return backend_create_win32(cfg, err);
#else
        if (err) *err = dupstr(_("Win32 backend not built"));
        return NULL;
#endif
    }

    if (target == BACKEND_MACOS) {
#ifdef WITH_MACOS
        return backend_create_macos(cfg, err);
#else
        if (err) *err = dupstr(_("macOS backend not built"));
        return NULL;
#endif
    }

    if (target == BACKEND_WAYLAND) {
#ifdef WITH_WAYLAND
        return backend_create_wayland(cfg, err);
#else
        if (which == BACKEND_AUTO) {
            fprintf(stderr,
                    _("warning: Wayland session detected but Wayland backend "
                      "not built; trying X11 (XWayland).\n"));
            target = BACKEND_X11;
        } else {
            if (err) *err = dupstr(_("Wayland backend not built"));
            return NULL;
        }
#endif
    }

    if (target == BACKEND_X11) {
#ifdef WITH_X11
        return backend_create_x11(cfg, err);
#else
        if (err) *err = dupstr(_("X11 backend not built"));
        return NULL;
#endif
    }

    if (err) *err = dupstr(_("no suitable backend available"));
    return NULL;
}

/* --------------------------- Dispatch --------------------------- */

int backend_get_fd(overlay_backend_t *b) {
    return b->vt->get_fd ? b->vt->get_fd(b->impl) : -1;
}

void backend_pump_events(overlay_backend_t *b) {
    if (b->vt->pump) b->vt->pump(b->impl);
}

void backend_render(overlay_backend_t *b, const overlay_frame_t *frame) {
    if (b->vt->render) b->vt->render(b->impl, frame);
}

void backend_destroy(overlay_backend_t *b) {
    if (!b) return;
    if (b->vt->destroy) b->vt->destroy(b->impl);
    free(b);
}

const char *backend_name(overlay_backend_t *b) {
    return b->vt->name ? b->vt->name : "?";
}
