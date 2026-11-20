#include "backend.h"
#include "backend_internal.h"

#include <cairo/cairo.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "i18n.h"
#include "render.h"
#include "util.h"
#include "wlr-layer-shell-client-protocol.h"

/* ------------------------------------------------------------------ */
/* Structures                                                         */
/* ------------------------------------------------------------------ */

#define MAX_OUTPUTS (MAX_SCREENS * 2)

typedef struct wl_backend wl_backend_t;

/* Outputs come and go at runtime (hot-plug), so each one is heap-allocated:
 * its address is the listener data and must stay valid while the array of
 * pointers is compacted. */
typedef struct {
    wl_backend_t *owner;
    struct wl_output *output;
    uint32_t global_name;   /* registry name, to match global_remove */
    int32_t width, height;  /* from the mode event */
    int32_t scale;
    bool done;              /* initial burst of output events finished */
} wl_output_info_t;

typedef struct {
    wl_backend_t *owner;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    int w, h, min_dim;
    bool configured;

    /* Shared buffer. */
    int   buf_fd;
    void *buf_data;
    size_t buf_size;
    struct wl_buffer *buffer;
    cairo_surface_t *cairo_surf;
} wl_overlay_t;

struct wl_backend {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;

    wl_output_info_t *outputs[MAX_OUTPUTS];
    int n_outputs;

    wl_overlay_t overlays[MAX_SCREENS];
    int n_overlays;

    int requested[MAX_SCREENS];
    int n_requested;

    bool ready;          /* startup done: output changes now mark dirty */
    topo_watch_t topo;

    char *last_text, *last_label;
    overlay_frame_t last;
    bool has_last;
};

static char *dupf(const char *s) { return s ? strdup(s) : NULL; }

/* ------------------------------------------------------------------ */
/* SHM buffer                                                         */
/* ------------------------------------------------------------------ */

static int create_shm_file(size_t size) {
    int fd = memfd_create("overlay-timer-shm", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static void overlay_free_buffer(wl_overlay_t *o) {
    if (o->cairo_surf) { cairo_surface_destroy(o->cairo_surf); o->cairo_surf = NULL; }
    if (o->buffer) { wl_buffer_destroy(o->buffer); o->buffer = NULL; }
    if (o->buf_data) { munmap(o->buf_data, o->buf_size); o->buf_data = NULL; }
    if (o->buf_fd >= 0) { close(o->buf_fd); o->buf_fd = -1; }
    o->buf_size = 0;
}

/* (Re)creates the buffer for the overlay's current size. */
static bool overlay_ensure_buffer(wl_overlay_t *o) {
    size_t stride = (size_t)o->w * 4;
    size_t size = stride * o->h;
    if (o->buffer && o->buf_size == size) return true;

    overlay_free_buffer(o);

    int fd = create_shm_file(size);
    if (fd < 0) return false;
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return false; }

    struct wl_shm_pool *pool = wl_shm_create_pool(o->owner->shm, fd, (int32_t)size);
    o->buffer = wl_shm_pool_create_buffer(pool, 0, o->w, o->h,
                                          (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    o->buf_fd = fd;
    o->buf_data = data;
    o->buf_size = size;
    o->cairo_surf = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, o->w, o->h, (int)stride);
    return true;
}

/* ------------------------------------------------------------------ */
/* layer_surface                                                      */
/* ------------------------------------------------------------------ */

static void draw_overlay(wl_overlay_t *o, const overlay_frame_t *f);

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h) {
    wl_overlay_t *o = data;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0) o->w = (int)w;
    if (h > 0) o->h = (int)h;
    o->min_dim = o->w < o->h ? o->w : o->h;
    o->configured = true;
    if (o->owner->has_last)
        draw_overlay(o, &o->owner->last);
}

/* The compositor closed the surface (typically: its output went away). It
 * must not be used again; the debounced rebuild destroys it. */
static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    wl_overlay_t *o = data;
    o->configured = false;
    topo_mark(&o->owner->topo);
}

static const struct zwlr_layer_surface_v1_listener LS_LISTENER = {
    .configure = ls_configure,
    .closed = ls_closed,
};

static void draw_overlay(wl_overlay_t *o, const overlay_frame_t *f) {
    if (!o->configured || o->w <= 0 || o->h <= 0) return;
    if (!overlay_ensure_buffer(o)) return;

    cairo_t *cr = cairo_create(o->cairo_surf);
    render_frame(cr, o->w, o->h, o->min_dim, f);
    cairo_destroy(cr);
    cairo_surface_flush(o->cairo_surf);

    wl_surface_attach(o->surface, o->buffer, 0, 0);
    wl_surface_damage_buffer(o->surface, 0, 0, o->w, o->h);
    wl_surface_commit(o->surface);
}

/* ------------------------------------------------------------------ */
/* Outputs                                                            */
/* ------------------------------------------------------------------ */

static void output_mode(void *data, struct wl_output *out, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh) {
    (void)out; (void)refresh;
    wl_output_info_t *info = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        info->width = w;
        info->height = h;
    }
}
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t sp, const char *make,
                            const char *model, int32_t transform) {
    (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sp;(void)make;(void)model;(void)transform;
}
static void output_scale(void *data, struct wl_output *o, int32_t factor) {
    (void)o;
    wl_output_info_t *info = data;
    info->scale = factor;
}
/* Mode/size changes of an existing output are handled by the compositor
 * re-configuring the layer surface; only a *new* output needs a rebuild. */
static void output_done(void *data, struct wl_output *o) {
    (void)o;
    wl_output_info_t *info = data;
    if (info->done) return;
    info->done = true;
    if (info->owner->ready) topo_mark(&info->owner->topo);
}

static const struct wl_output_listener OUTPUT_LISTENER = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void output_free(wl_output_info_t *info) {
    if (!info) return;
    if (info->output) wl_output_destroy(info->output);
    free(info);
}

/* ------------------------------------------------------------------ */
/* Registry                                                           */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    wl_backend_t *b = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        b->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, v);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        b->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        uint32_t v = version < 4 ? version : 4;
        b->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, v);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (b->n_outputs >= MAX_OUTPUTS) return;
        wl_output_info_t *info = calloc(1, sizeof *info);
        if (!info) return;
        info->owner = b;
        info->global_name = name;
        info->scale = 1;
        info->output = wl_registry_bind(reg, name, &wl_output_interface,
                                        version < 2 ? version : 2);
        wl_output_add_listener(info->output, &OUTPUT_LISTENER, info);
        b->outputs[b->n_outputs++] = info;
        /* Version 1 has no `done` event: treat it as complete right away. */
        if (version < 2) output_done(info, info->output);
    }
}

static void registry_global_remove(void *data, struct wl_registry *r,
                                   uint32_t name) {
    (void)r;
    wl_backend_t *b = data;
    for (int i = 0; i < b->n_outputs; i++) {
        if (b->outputs[i]->global_name != name) continue;
        /* Overlays on this output are destroyed by the rebuild; the
         * compositor has already closed (or will close) their surfaces. */
        output_free(b->outputs[i]);
        memmove(&b->outputs[i], &b->outputs[i + 1],
                sizeof(b->outputs[0]) * (size_t)(b->n_outputs - i - 1));
        b->n_outputs--;
        topo_mark(&b->topo);
        return;
    }
}

static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* Overlays                                                           */
/* ------------------------------------------------------------------ */

static void destroy_overlays(wl_backend_t *b) {
    for (int i = 0; i < b->n_overlays; i++) {
        wl_overlay_t *o = &b->overlays[i];
        overlay_free_buffer(o);
        if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
        if (o->surface) wl_surface_destroy(o->surface);
        memset(o, 0, sizeof *o);
        o->buf_fd = -1;
    }
    b->n_overlays = 0;
}

static void create_overlay(wl_backend_t *b, wl_overlay_t *o,
                           const wl_output_info_t *out) {
    memset(o, 0, sizeof *o);
    o->owner = b;
    o->buf_fd = -1;
    o->w = out->width;
    o->h = out->height;
    o->min_dim = (o->w && o->h) ? (o->w < o->h ? o->w : o->h) : 0;

    o->surface = wl_compositor_create_surface(b->compositor);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(
        b->layer_shell, o->surface, out->output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "overlay-timer");
    zwlr_layer_surface_v1_add_listener(o->layer, &LS_LISTENER, o);

    /* Anchor to all edges => covers the entire output. */
    zwlr_layer_surface_v1_set_anchor(
        o->layer,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        o->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

    /* Empty input region => click-through. */
    struct wl_region *empty = wl_compositor_create_region(b->compositor);
    wl_surface_set_input_region(o->surface, empty);
    wl_region_destroy(empty);

    wl_surface_commit(o->surface); /* initial commit without buffer */
}

/* Throws away the overlays and recreates them on the current outputs. The
 * surfaces are drawn when their `configure` arrives (no roundtrip here). */
static void build_overlays(wl_backend_t *b, screen_layout_t *out) {
    destroy_overlays(b);

    /* Screen indices count only outputs whose info is complete. */
    wl_output_info_t *ready[MAX_OUTPUTS];
    int nready = 0;
    for (int i = 0; i < b->n_outputs; i++)
        if (b->outputs[i]->done) ready[nready++] = b->outputs[i];

    screen_layout_t l;
    backend_resolve_screens(b->requested, b->n_requested, nready, &l);
    for (int i = 0; i < l.n_shown; i++)
        create_overlay(b, &b->overlays[b->n_overlays++], ready[l.shown[i]]);
    wl_display_flush(b->display);

    backend_log_layout("wayland", b->requested, b->n_requested, &l);
    if (out) *out = l;
}

/* ------------------------------------------------------------------ */
/* Vtable                                                             */
/* ------------------------------------------------------------------ */

static int wl_get_fd(void *impl) {
    wl_backend_t *b = impl;
    return wl_display_get_fd(b->display);
}

static void wl_store_last(wl_backend_t *b, const overlay_frame_t *f) {
    char *t = dupf(f->text), *l = dupf(f->label);
    free(b->last_text);
    free(b->last_label);
    b->last_text = t;
    b->last_label = l;
    b->last = *f;
    b->last.text = b->last_text;
    b->last.label = b->last_label;
    b->has_last = true;
}

static void wl_render(void *impl, const overlay_frame_t *frame) {
    wl_backend_t *b = impl;
    wl_store_last(b, frame);
    for (int i = 0; i < b->n_overlays; i++)
        draw_overlay(&b->overlays[i], frame);
    wl_display_flush(b->display);
}

static void wl_pump(void *impl) {
    wl_backend_t *b = impl;
    /* Called when the fd is readable: process without blocking. */
    while (wl_display_prepare_read(b->display) != 0)
        wl_display_dispatch_pending(b->display);
    wl_display_flush(b->display);
    wl_display_read_events(b->display);
    wl_display_dispatch_pending(b->display);
}

static void wl_set_screens(void *impl, const int *screens, int n,
                           screen_layout_t *out) {
    wl_backend_t *b = impl;
    if (n > MAX_SCREENS) n = MAX_SCREENS;
    memcpy(b->requested, screens, sizeof(int) * (size_t)n);
    b->n_requested = n;
    build_overlays(b, out);
}

static bool wl_tick(void *impl) {
    wl_backend_t *b = impl;
    if (!topo_due(&b->topo)) return false;
    log_info(_("wayland: output configuration changed"));
    build_overlays(b, NULL);
    return true;
}

static void wl_destroy(void *impl) {
    wl_backend_t *b = impl;
    if (!b) return;
    destroy_overlays(b);
    for (int i = 0; i < b->n_outputs; i++) output_free(b->outputs[i]);
    b->n_outputs = 0;
    if (b->layer_shell) zwlr_layer_shell_v1_destroy(b->layer_shell);
    if (b->shm) wl_shm_destroy(b->shm);
    if (b->compositor) wl_compositor_destroy(b->compositor);
    if (b->registry) wl_registry_destroy(b->registry);
    if (b->display) { wl_display_flush(b->display); wl_display_disconnect(b->display); }
    free(b->last_text);
    free(b->last_label);
    free(b);
}

static const backend_vtable_t WL_VT = {
    .get_fd = wl_get_fd,
    .pump = wl_pump,
    .render = wl_render,
    .set_screens = wl_set_screens,
    .tick = wl_tick,
    .destroy = wl_destroy,
    .name = "wayland",
};

/* ------------------------------------------------------------------ */
/* Creation                                                           */
/* ------------------------------------------------------------------ */

overlay_backend_t *backend_create_wayland(const config_t *cfg, char **err) {
    wl_backend_t *b = calloc(1, sizeof *b);
    if (!b) { if (err) *err = dupf("out of memory"); return NULL; }
    for (int i = 0; i < MAX_SCREENS; i++) b->overlays[i].buf_fd = -1;

    b->display = wl_display_connect(NULL);
    if (!b->display) {
        if (err) *err = dupf(_("cannot connect to Wayland display"));
        free(b);
        return NULL;
    }

    b->registry = wl_display_get_registry(b->display);
    wl_registry_add_listener(b->registry, &REGISTRY_LISTENER, b);
    wl_display_roundtrip(b->display); /* globals */
    wl_display_roundtrip(b->display); /* output events (mode/scale/done) */

    if (!b->compositor || !b->shm) {
        if (err) *err = dupf(_("Wayland compositor missing wl_compositor/wl_shm"));
        wl_destroy(b);
        return NULL;
    }
    if (!b->layer_shell) {
        if (err) *err = dupf(_("compositor does not support wlr-layer-shell "
                               "(e.g. GNOME/Mutter): Wayland overlay unsupported"));
        wl_destroy(b);
        return NULL;
    }

    int n = cfg->n_screens < MAX_SCREENS ? cfg->n_screens : MAX_SCREENS;
    memcpy(b->requested, cfg->screens, sizeof(int) * (size_t)n);
    b->n_requested = n;
    build_overlays(b, NULL);

    if (b->n_overlays == 0) {
        if (err) *err = dupf(_("no valid screen to display on"));
        wl_destroy(b);
        return NULL;
    }

    wl_display_roundtrip(b->display); /* receive initial configure */
    b->ready = true;
    /* Startup events must not trigger a rebuild. */
    b->topo.pending = false;

    overlay_backend_t *w = backend_wrap(&WL_VT, b);
    if (!w) {
        wl_destroy(b);
        if (err) *err = dupf("out of memory");
    }
    return w;
}
