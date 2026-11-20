#include "backend.h"
#include "backend_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i18n.h"
#include "util.h"

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

/* --------------------------- Helpers --------------------------- */

long backend_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void topo_mark(topo_watch_t *t) {
    t->pending = true;
    t->since_ms = backend_now_ms();
}

bool topo_due(topo_watch_t *t) {
    if (!t->pending) return false;
    if (backend_now_ms() - t->since_ms < TOPO_DEBOUNCE_MS) return false;
    t->pending = false;
    return true;
}

void backend_resolve_screens(const int *req, int nreq, int nmon,
                             screen_layout_t *out) {
    memset(out, 0, sizeof *out);
    out->n_monitors = nmon < 0 ? 0 : nmon;
    for (int i = 0; i < nreq && out->n_shown < MAX_SCREENS; i++) {
        int idx = req[i];
        if (idx < 0 || idx >= out->n_monitors) continue;
        bool dup = false;
        for (int j = 0; j < out->n_shown; j++)
            if (out->shown[j] == idx) { dup = true; break; }
        if (!dup) out->shown[out->n_shown++] = idx;
    }
    if (out->n_shown == 0 && out->n_monitors > 0) {
        out->shown[0] = 0;
        out->n_shown = 1;
        out->fallback = true;
    }
}

/* Appends "a,b,c" to buf. */
static size_t append_list(char *buf, size_t sz, size_t off, const int *v, int n) {
    for (int i = 0; i < n && off < sz; i++)
        off += (size_t)snprintf(buf + off, sz - off, "%s%d", i ? "," : "", v[i]);
    return off;
}

void backend_describe_layout(const int *req, int nreq,
                             const screen_layout_t *l, char *buf, size_t sz) {
    if (!buf || sz == 0) return;
    buf[0] = '\0';
    if (l->n_monitors == 0) {
        snprintf(buf, sz, "%s", _("no monitor available; waiting for one"));
        return;
    }

    size_t off = (size_t)snprintf(buf, sz, "%s", _("showing on screen(s) "));
    if (off >= sz) return;
    off = append_list(buf, sz, off, l->shown, l->n_shown);
    if (off < sz)
        off += (size_t)snprintf(buf + off, sz - off, _(" (%d monitor(s))"),
                                l->n_monitors);

    /* Requested indices that are not available (deduplicated). */
    int missing[MAX_SCREENS], nmiss = 0;
    for (int i = 0; i < nreq; i++) {
        if (req[i] >= 0 && req[i] < l->n_monitors) continue;
        bool dup = false;
        for (int j = 0; j < nmiss; j++)
            if (missing[j] == req[i]) { dup = true; break; }
        if (!dup && nmiss < MAX_SCREENS) missing[nmiss++] = req[i];
    }
    if (nmiss > 0 && off < sz) {
        off += (size_t)snprintf(buf + off, sz - off, "%s", _("; unavailable: "));
        if (off < sz) off = append_list(buf, sz, off, missing, nmiss);
    }
    if (l->fallback && off < sz)
        snprintf(buf + off, sz - off, "%s", _("; falling back to screen 0"));
}

void backend_log_layout(const char *backend, const int *req, int nreq,
                        const screen_layout_t *l) {
    char buf[256];
    backend_describe_layout(req, nreq, l, buf, sizeof buf);
    bool missing = l->n_shown == 0 || l->fallback;
    for (int i = 0; i < nreq && !missing; i++)
        if (req[i] < 0 || req[i] >= l->n_monitors) missing = true;
    if (missing)
        log_warn("%s: %s", backend, buf);
    else
        log_info("%s: %s", backend, buf);
}

static void store_last(overlay_backend_t *b, const overlay_frame_t *f) {
    char *t = dupstr(f->text), *l = dupstr(f->label);
    free(b->last_text);
    free(b->last_label);
    b->last_text = t;
    b->last_label = l;
    b->last = *f;
    b->last.text = b->last_text;
    b->last.label = b->last_label;
    b->has_last = true;
}

static void rerender_last(overlay_backend_t *b) {
    if (b->has_last && b->vt->render) b->vt->render(b->impl, &b->last);
}

/* --------------------------- Dispatch --------------------------- */

int backend_get_fd(overlay_backend_t *b) {
    return b->vt->get_fd ? b->vt->get_fd(b->impl) : -1;
}

void backend_pump_events(overlay_backend_t *b) {
    if (b->vt->pump) b->vt->pump(b->impl);
}

void backend_render(overlay_backend_t *b, const overlay_frame_t *frame) {
    store_last(b, frame);
    if (b->vt->render) b->vt->render(b->impl, frame);
}

void backend_tick(overlay_backend_t *b) {
    if (b->vt->tick && b->vt->tick(b->impl)) rerender_last(b);
}

int backend_set_screens(overlay_backend_t *b, const int *screens, int n,
                        char *msg, size_t msgsz) {
    if (!b->vt->set_screens) {
        if (msg && msgsz) snprintf(msg, msgsz, "%s",
                                   _("this backend cannot change screens"));
        return -1;
    }
    screen_layout_t l;
    b->vt->set_screens(b->impl, screens, n, &l);
    rerender_last(b);
    if (msg && msgsz) backend_describe_layout(screens, n, &l, msg, msgsz);
    return l.n_shown;
}

void backend_destroy(overlay_backend_t *b) {
    if (!b) return;
    if (b->vt->destroy) b->vt->destroy(b->impl);
    free(b->last_text);
    free(b->last_label);
    free(b);
}

const char *backend_name(overlay_backend_t *b) {
    return b->vt->name ? b->vt->name : "?";
}
