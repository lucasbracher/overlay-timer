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
#include "wlr-layer-shell-client-protocol.h"

/* ------------------------------------------------------------------ */
/* Estruturas                                                         */
/* ------------------------------------------------------------------ */

#define MAX_OUTPUTS (MAX_SCREENS * 2)

typedef struct {
    struct wl_output *output;
    int32_t width, height;  /* from the mode event */
    int32_t scale;
    bool have_mode;
} wl_output_info_t;

typedef struct wl_backend wl_backend_t;

typedef struct {
    wl_backend_t *owner;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    int w, h, min_dim;
    bool configured;

    /* Buffer compartilhado. */
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

    wl_output_info_t outputs[MAX_OUTPUTS];
    int n_outputs;

    wl_overlay_t overlays[MAX_SCREENS];
    int n_overlays;

    char *last_text, *last_label;
    overlay_frame_t last;
    bool has_last;
};

static char *dupf(const char *s) { return s ? strdup(s) : NULL; }

/* ------------------------------------------------------------------ */
/* Buffer SHM                                                         */
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

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    wl_overlay_t *o = data;
    o->configured = false;
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
/* Registry                                                           */
/* ------------------------------------------------------------------ */

static void output_mode(void *data, struct wl_output *out, uint32_t flags,
                        int32_t w, int32_t h, int32_t refresh) {
    (void)out; (void)refresh;
    wl_output_info_t *info = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        info->width = w;
        info->height = h;
        info->have_mode = true;
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
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }

static const struct wl_output_listener OUTPUT_LISTENER = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

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
        if (b->n_outputs < MAX_OUTPUTS) {
            wl_output_info_t *info = &b->outputs[b->n_outputs];
            info->scale = 1;
            info->output = wl_registry_bind(reg, name, &wl_output_interface,
                                            version < 2 ? version : 2);
            wl_output_add_listener(info->output, &OUTPUT_LISTENER, info);
            b->n_outputs++;
        }
    }
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t n) {
    (void)d;(void)r;(void)n;
}
static const struct wl_registry_listener REGISTRY_LISTENER = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* Vtable                                                             */
/* ------------------------------------------------------------------ */

static int wl_get_fd(void *impl) {
    wl_backend_t *b = impl;
    return wl_display_get_fd(b->display);
}

static void wl_store_last(wl_backend_t *b, const overlay_frame_t *f) {
    free(b->last_text);
    free(b->last_label);
    b->last_text = dupf(f->text);
    b->last_label = dupf(f->label);
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

static void wl_destroy(void *impl) {
    wl_backend_t *b = impl;
    if (!b) return;
    for (int i = 0; i < b->n_overlays; i++) {
        wl_overlay_t *o = &b->overlays[i];
        overlay_free_buffer(o);
        if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
        if (o->surface) wl_surface_destroy(o->surface);
    }
    for (int i = 0; i < b->n_outputs; i++)
        if (b->outputs[i].output) wl_output_destroy(b->outputs[i].output);
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
    wl_display_roundtrip(b->display); /* output events (mode/scale) */

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

    for (int i = 0; i < cfg->n_screens; i++) {
        int idx = cfg->screens[i];
        if (idx < 0 || idx >= b->n_outputs) {
            fprintf(stderr, _("warning: screen index %d out of range (have %d)\n"),
                    idx, b->n_outputs);
            continue;
        }

        wl_overlay_t *o = &b->overlays[b->n_overlays];
        o->owner = b;
        o->buf_fd = -1;
        o->w = b->outputs[idx].width;
        o->h = b->outputs[idx].height;
        o->min_dim = (o->w && o->h) ? (o->w < o->h ? o->w : o->h) : 0;

        o->surface = wl_compositor_create_surface(b->compositor);
        o->layer = zwlr_layer_shell_v1_get_layer_surface(
            b->layer_shell, o->surface, b->outputs[idx].output,
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
        b->n_overlays++;
    }

    if (b->n_overlays == 0) {
        if (err) *err = dupf(_("no valid screen to display on"));
        wl_destroy(b);
        return NULL;
    }

    wl_display_roundtrip(b->display); /* receive initial configure */
    return backend_wrap(&WL_VT, b);
}
