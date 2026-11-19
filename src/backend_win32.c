/*
 * Windows backend (Win32 + cairo-win32).
 *
 * One layered, click-through, topmost window per selected screen, covering
 * the monitor's work area (excluding the taskbar). Each frame is drawn with
 * render_frame() into a Cairo DIB surface and pushed to the window with
 * UpdateLayeredWindow() (per-pixel alpha). Cairo's ARGB32 is premultiplied
 * BGRA, which is exactly what UpdateLayeredWindow expects, so no WM_PAINT
 * handling is needed.
 *
 * Window messages have no pollable fd, so get_fd() returns -1 and the daemon
 * calls pump() on every loop iteration (at least once per second).
 */
#include <windows.h>

#include <cairo.h>
#include <cairo-win32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_internal.h"
#include "i18n.h"
#include "render.h"

#define WINDOW_CLASS L"OverlayTimerWindow"
#define OT_MAX_MONITORS 32

typedef struct {
    HWND hwnd;
    int x, y, w, h;      /* work area, in physical pixels */
    int min_dim;
    cairo_surface_t *surface;
    cairo_t *cr;
} win_overlay_t;

typedef struct {
    win_overlay_t *overlays;
    int n_overlays;
    HINSTANCE hinst;
} win_backend_t;

/* Monitor list, primary first (index 0 = main screen, like the others). */
typedef struct {
    RECT work[OT_MAX_MONITORS];
    int n;
} monitor_list_t;

static char *dupf(const char *s) { return s ? strdup(s) : NULL; }

static BOOL CALLBACK collect_monitor(HMONITOR mon, HDC hdc, LPRECT rc,
                                     LPARAM data) {
    (void)hdc; (void)rc;
    monitor_list_t *list = (monitor_list_t *)data;
    if (list->n >= OT_MAX_MONITORS) return FALSE;

    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    if (mi.dwFlags & MONITORINFOF_PRIMARY) {
        /* Insert the primary at the front. */
        memmove(&list->work[1], &list->work[0], sizeof(RECT) * (size_t)list->n);
        list->work[0] = mi.rcWork;
    } else {
        list->work[list->n] = mi.rcWork;
    }
    list->n++;
    return TRUE;
}

static int win_get_fd(void *impl) {
    (void)impl;
    return -1;
}

static void win_pump(void *impl) {
    (void)impl;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static void win_render(void *impl, const overlay_frame_t *frame) {
    win_backend_t *b = impl;
    for (int i = 0; i < b->n_overlays; i++) {
        win_overlay_t *o = &b->overlays[i];
        render_frame(o->cr, o->w, o->h, o->min_dim, frame);
        cairo_surface_flush(o->surface);

        POINT dst = { o->x, o->y };
        SIZE size = { o->w, o->h };
        POINT src = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(o->hwnd, NULL, &dst, &size,
                            cairo_win32_surface_get_dc(o->surface), &src, 0,
                            &blend, ULW_ALPHA);
    }
}

static void win_destroy(void *impl) {
    win_backend_t *b = impl;
    if (!b) return;
    for (int i = 0; i < b->n_overlays; i++) {
        win_overlay_t *o = &b->overlays[i];
        if (o->cr) cairo_destroy(o->cr);
        if (o->surface) cairo_surface_destroy(o->surface);
        if (o->hwnd) DestroyWindow(o->hwnd);
    }
    free(b->overlays);
    UnregisterClassW(WINDOW_CLASS, b->hinst);
    free(b);
}

static const backend_vtable_t WIN32_VT = {
    .get_fd = win_get_fd,
    .pump = win_pump,
    .render = win_render,
    .destroy = win_destroy,
    .name = "win32",
};

/* Creates the overlay window for one monitor work area. */
static bool create_overlay(win_backend_t *b, win_overlay_t *o, RECT r) {
    o->x = r.left;
    o->y = r.top;
    o->w = r.right - r.left;
    o->h = r.bottom - r.top;
    o->min_dim = o->w < o->h ? o->w : o->h;

    o->hwnd = CreateWindowExW(
        WS_EX_LAYERED       /* per-pixel alpha */
        | WS_EX_TRANSPARENT /* click-through */
        | WS_EX_TOPMOST     /* above normal windows */
        | WS_EX_TOOLWINDOW  /* not in taskbar / Alt+Tab */
        | WS_EX_NOACTIVATE, /* never takes focus */
        WINDOW_CLASS, L"overlay-timer", WS_POPUP,
        o->x, o->y, o->w, o->h, NULL, NULL, b->hinst, NULL);
    if (!o->hwnd) return false;

    o->surface = cairo_win32_surface_create_with_dib(CAIRO_FORMAT_ARGB32,
                                                     o->w, o->h);
    if (cairo_surface_status(o->surface) != CAIRO_STATUS_SUCCESS) return false;
    o->cr = cairo_create(o->surface);

    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    return true;
}

overlay_backend_t *backend_create_win32(const config_t *cfg, char **err) {
    win_backend_t *b = calloc(1, sizeof *b);
    if (!b) { if (err) *err = dupf("out of memory"); return NULL; }

    /* Work in physical pixels (the text size is a % of the screen). */
    SetProcessDPIAware();

    b->hinst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DefWindowProcW; /* no input to handle */
    wc.hInstance = b->hinst;
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (err) *err = dupf(_("cannot register window class"));
        free(b);
        return NULL;
    }

    monitor_list_t mons;
    memset(&mons, 0, sizeof mons);
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&mons);

    b->overlays = calloc(cfg->n_screens > 0 ? (size_t)cfg->n_screens : 1,
                         sizeof(win_overlay_t));
    if (!b->overlays) {
        if (err) *err = dupf("out of memory");
        win_destroy(b);
        return NULL;
    }

    for (int i = 0; i < cfg->n_screens; i++) {
        int idx = cfg->screens[i];
        if (idx < 0 || idx >= mons.n) {
            fprintf(stderr,
                    _("warning: screen index %d out of range (have %d)\n"),
                    idx, mons.n);
            continue;
        }
        win_overlay_t *o = &b->overlays[b->n_overlays];
        b->n_overlays++; /* count it so win_destroy cleans up on failure */
        if (!create_overlay(b, o, mons.work[idx])) {
            if (err) *err = dupf(_("cannot create overlay window"));
            win_destroy(b);
            return NULL;
        }
    }

    if (b->n_overlays == 0) {
        if (err) *err = dupf(_("no valid screen to display on"));
        win_destroy(b);
        return NULL;
    }

    overlay_backend_t *w = backend_wrap(&WIN32_VT, b);
    if (!w) {
        win_destroy(b);
        if (err) *err = dupf("out of memory");
    }
    return w;
}
