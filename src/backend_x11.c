#include "backend.h"
#include "backend_internal.h"

#include <cairo/cairo-xcb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/randr.h>
#include <xcb/shape.h>
#include <xcb/xcb.h>

#include "i18n.h"
#include "render.h"

typedef struct {
    xcb_window_t win;
    int x, y, w, h;
    int min_dim;
    cairo_surface_t *surface;
    cairo_t *cr;
} x11_overlay_t;

typedef struct {
    xcb_connection_t *conn;
    xcb_screen_t *screen;
    xcb_visualtype_t *argb_visual;
    uint8_t argb_depth;
    xcb_colormap_t colormap;

    x11_overlay_t *overlays;
    int n_overlays;

    char *last_text;
    char *last_label;
    overlay_frame_t last;
    bool has_last;
} x11_backend_t;

static char *dupf(const char *s) { return s ? strdup(s) : NULL; }

static xcb_visualtype_t *find_argb_visual(xcb_screen_t *screen, uint8_t *depth_out) {
    xcb_depth_iterator_t di = xcb_screen_allowed_depths_iterator(screen);
    for (; di.rem; xcb_depth_next(&di)) {
        if (di.data->depth != 32) continue;
        xcb_visualtype_iterator_t vi = xcb_depth_visuals_iterator(di.data);
        for (; vi.rem; xcb_visualtype_next(&vi)) {
            if (vi.data->_class == XCB_VISUAL_CLASS_TRUE_COLOR) {
                *depth_out = 32;
                return vi.data;
            }
        }
    }
    return NULL;
}

static void set_input_passthrough(xcb_connection_t *conn, xcb_window_t win) {
    xcb_shape_rectangles(conn, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT,
                         XCB_CLIP_ORDERING_UNSORTED, win, 0, 0, 0, NULL);
}

static void set_window_hints(xcb_connection_t *conn, xcb_window_t win) {
    struct { const char *name; xcb_atom_t atom; } atoms[] = {
        {"_NET_WM_WINDOW_TYPE", 0},
        {"_NET_WM_WINDOW_TYPE_NOTIFICATION", 0},
        {"_NET_WM_STATE", 0},
        {"_NET_WM_STATE_ABOVE", 0},
    };
    for (size_t i = 0; i < sizeof atoms / sizeof atoms[0]; i++) {
        xcb_intern_atom_cookie_t c =
            xcb_intern_atom(conn, 0, (uint16_t)strlen(atoms[i].name), atoms[i].name);
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(conn, c, NULL);
        if (r) { atoms[i].atom = r->atom; free(r); }
    }
    if (atoms[0].atom && atoms[1].atom)
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atoms[0].atom,
                            XCB_ATOM_ATOM, 32, 1, &atoms[1].atom);
    if (atoms[2].atom && atoms[3].atom)
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, win, atoms[2].atom,
                            XCB_ATOM_ATOM, 32, 1, &atoms[3].atom);
}

static int enumerate_monitors(xcb_connection_t *conn, xcb_screen_t *screen,
                              int *xs, int *ys, int *ws, int *hs, int max) {
    int n = 0;
    xcb_randr_get_monitors_cookie_t c =
        xcb_randr_get_monitors(conn, screen->root, 1);
    xcb_randr_get_monitors_reply_t *r =
        xcb_randr_get_monitors_reply(conn, c, NULL);
    if (r) {
        xcb_randr_monitor_info_iterator_t it =
            xcb_randr_get_monitors_monitors_iterator(r);
        for (; it.rem && n < max; xcb_randr_monitor_info_next(&it)) {
            xcb_randr_monitor_info_t *m = it.data;
            xs[n] = m->x; ys[n] = m->y; ws[n] = m->width; hs[n] = m->height;
            n++;
        }
        free(r);
    }
    if (n == 0) {
        xs[0] = 0; ys[0] = 0;
        ws[0] = screen->width_in_pixels;
        hs[0] = screen->height_in_pixels;
        n = 1;
    }
    return n;
}

static int x11_get_fd(void *impl) {
    x11_backend_t *b = impl;
    return xcb_get_file_descriptor(b->conn);
}

static void draw_all(x11_backend_t *b, const overlay_frame_t *f) {
    for (int i = 0; i < b->n_overlays; i++) {
        x11_overlay_t *o = &b->overlays[i];
        render_frame(o->cr, o->w, o->h, o->min_dim, f);
        cairo_surface_flush(o->surface);
    }
    xcb_flush(b->conn);
}

static void store_last(x11_backend_t *b, const overlay_frame_t *f) {
    free(b->last_text);
    free(b->last_label);
    b->last_text = dupf(f->text);
    b->last_label = dupf(f->label);
    b->last = *f;
    b->last.text = b->last_text;
    b->last.label = b->last_label;
    b->has_last = true;
}

static void x11_render(void *impl, const overlay_frame_t *frame) {
    x11_backend_t *b = impl;
    store_last(b, frame);
    draw_all(b, frame);
}

static void x11_pump(void *impl) {
    x11_backend_t *b = impl;
    xcb_generic_event_t *ev;
    bool need_redraw = false;
    while ((ev = xcb_poll_for_event(b->conn))) {
        uint8_t type = ev->response_type & ~0x80;
        if (type == XCB_EXPOSE) need_redraw = true;
        free(ev);
    }
    if (need_redraw && b->has_last)
        draw_all(b, &b->last);
}

static void x11_destroy(void *impl) {
    x11_backend_t *b = impl;
    if (!b) return;
    if (b->overlays) {
        for (int i = 0; i < b->n_overlays; i++) {
            x11_overlay_t *o = &b->overlays[i];
            if (o->cr) cairo_destroy(o->cr);
            if (o->surface) cairo_surface_destroy(o->surface);
            if (o->win) xcb_destroy_window(b->conn, o->win);
        }
        free(b->overlays);
    }
    if (b->conn) {
        if (b->colormap) xcb_free_colormap(b->conn, b->colormap);
        xcb_flush(b->conn);
        xcb_disconnect(b->conn);
    }
    free(b->last_text);
    free(b->last_label);
    free(b);
}

static const backend_vtable_t X11_VT = {
    .get_fd = x11_get_fd,
    .pump = x11_pump,
    .render = x11_render,
    .destroy = x11_destroy,
    .name = "x11",
};

overlay_backend_t *backend_create_x11(const config_t *cfg, char **err) {
    x11_backend_t *b = calloc(1, sizeof *b);
    if (!b) { if (err) *err = dupf("out of memory"); return NULL; }

    int screen_num = 0;
    b->conn = xcb_connect(NULL, &screen_num);
    if (xcb_connection_has_error(b->conn)) {
        if (err) *err = dupf(_("cannot connect to X server (is DISPLAY set?)"));
        free(b);
        return NULL;
    }

    const xcb_setup_t *setup = xcb_get_setup(b->conn);
    xcb_screen_iterator_t si = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&si);
    b->screen = si.data;

    b->argb_visual = find_argb_visual(b->screen, &b->argb_depth);
    if (!b->argb_visual) {
        if (err) *err = dupf(_("no 32-bit ARGB visual found (need a compositor)"));
        xcb_disconnect(b->conn);
        free(b);
        return NULL;
    }

    b->colormap = xcb_generate_id(b->conn);
    xcb_create_colormap(b->conn, XCB_COLORMAP_ALLOC_NONE, b->colormap,
                        b->screen->root, b->argb_visual->visual_id);

    int xs[MAX_SCREENS * 2], ys[MAX_SCREENS * 2], ws[MAX_SCREENS * 2], hs[MAX_SCREENS * 2];
    int nmon = enumerate_monitors(b->conn, b->screen, xs, ys, ws, hs,
                                  MAX_SCREENS * 2);

    b->overlays = calloc(cfg->n_screens, sizeof(x11_overlay_t));
    b->n_overlays = 0;

    for (int i = 0; i < cfg->n_screens; i++) {
        int idx = cfg->screens[i];
        if (idx < 0 || idx >= nmon) {
            fprintf(stderr, _("warning: screen index %d out of range (have %d)\n"),
                    idx, nmon);
            continue;
        }

        x11_overlay_t *o = &b->overlays[b->n_overlays];
        o->x = xs[idx]; o->y = ys[idx]; o->w = ws[idx]; o->h = hs[idx];
        o->min_dim = o->w < o->h ? o->w : o->h;

        o->win = xcb_generate_id(b->conn);
        uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL |
                        XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK |
                        XCB_CW_COLORMAP;
        uint32_t values[5] = { 0, 0, 1, XCB_EVENT_MASK_EXPOSURE, b->colormap };

        xcb_create_window(b->conn, b->argb_depth, o->win, b->screen->root,
                          (int16_t)o->x, (int16_t)o->y,
                          (uint16_t)o->w, (uint16_t)o->h, 0,
                          XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          b->argb_visual->visual_id, mask, values);

        set_window_hints(b->conn, o->win);
        set_input_passthrough(b->conn, o->win);
        xcb_map_window(b->conn, o->win);

        uint32_t stack = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(b->conn, o->win, XCB_CONFIG_WINDOW_STACK_MODE, &stack);

        o->surface = cairo_xcb_surface_create(b->conn, o->win, b->argb_visual,
                                              o->w, o->h);
        o->cr = cairo_create(o->surface);
        b->n_overlays++;
    }

    xcb_flush(b->conn);

    if (b->n_overlays == 0) {
        if (err) *err = dupf(_("no valid screen to display on"));
        x11_destroy(b);
        return NULL;
    }
    return backend_wrap(&X11_VT, b);
}
