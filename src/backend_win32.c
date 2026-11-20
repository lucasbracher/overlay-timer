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
 * Monitor changes: WM_DISPLAYCHANGE / WM_SETTINGCHANGE(SPI_SETWORKAREA) /
 * WM_DPICHANGED mark the topology dirty and tick() rebuilds the windows.
 * Those messages are broadcast to top-level windows only, and there may be
 * no overlay window at all (no monitor), so a hidden, never-shown top-level
 * helper window lives for the whole backend lifetime to receive them.
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
#include "util.h"

#define WINDOW_CLASS L"OverlayTimerWindow"
#define OT_MAX_MONITORS 32

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

typedef struct {
    HWND hwnd;
    int x, y, w, h;      /* work area, in physical pixels */
    int min_dim;
    cairo_surface_t *surface;
    cairo_t *cr;
} win_overlay_t;

typedef struct {
    win_overlay_t overlays[MAX_SCREENS];
    int n_overlays;
    HINSTANCE hinst;
    HWND helper;         /* hidden top-level window: receives broadcasts */
    bool class_registered;

    int requested[MAX_SCREENS];
    int n_requested;
    topo_watch_t topo;
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

/* All windows of the class share this procedure; the backend pointer comes
 * from CreateWindowExW's lpParam. */
static LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
    }
    win_backend_t *b = (win_backend_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_DISPLAYCHANGE:
        if (b) topo_mark(&b->topo);
        return 0;
    case WM_DPICHANGED:
        /* Do not let Windows resize us: the rebuild recomputes everything. */
        if (b) topo_mark(&b->topo);
        return 0;
    case WM_SETTINGCHANGE:
        if (b && wp == SPI_SETWORKAREA) topo_mark(&b->topo);
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Per-monitor DPI awareness (v2) when available, so every monitor's work
 * area is reported in real pixels even with mixed scale factors; falls back
 * to system DPI awareness on older Windows. Looked up dynamically so the
 * binary still runs where the API does not exist. */
static void set_dpi_awareness(void) {
    typedef BOOL (WINAPI *set_ctx_fn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    set_ctx_fn fn = NULL;
    if (user32) {
        FARPROC p = GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        memcpy(&fn, &p, sizeof fn);
    }
    /* (HANDLE)-4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (!fn || !fn((HANDLE)(INT_PTR)-4))
        SetProcessDPIAware();
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

static void destroy_overlays(win_backend_t *b) {
    for (int i = 0; i < b->n_overlays; i++) {
        win_overlay_t *o = &b->overlays[i];
        if (o->cr) cairo_destroy(o->cr);
        if (o->surface) cairo_surface_destroy(o->surface);
        if (o->hwnd) DestroyWindow(o->hwnd);
        memset(o, 0, sizeof *o);
    }
    b->n_overlays = 0;
}

/* Creates the overlay window for one monitor work area. On failure the
 * partially created resources stay in `o` for destroy_overlays(). */
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
        o->x, o->y, o->w, o->h, NULL, NULL, b->hinst, b);
    if (!o->hwnd) return false;

    o->surface = cairo_win32_surface_create_with_dib(CAIRO_FORMAT_ARGB32,
                                                     o->w, o->h);
    if (cairo_surface_status(o->surface) != CAIRO_STATUS_SUCCESS) return false;
    o->cr = cairo_create(o->surface);

    ShowWindow(o->hwnd, SW_SHOWNOACTIVATE);
    return true;
}

/* Throws away the overlays and recreates them on the current monitors. */
static void build_overlays(win_backend_t *b, screen_layout_t *out) {
    destroy_overlays(b);

    monitor_list_t mons;
    memset(&mons, 0, sizeof mons);
    EnumDisplayMonitors(NULL, NULL, collect_monitor, (LPARAM)&mons);

    screen_layout_t l;
    backend_resolve_screens(b->requested, b->n_requested, mons.n, &l);
    for (int i = 0; i < l.n_shown; i++) {
        win_overlay_t *o = &b->overlays[b->n_overlays];
        b->n_overlays++;
        if (!create_overlay(b, o, mons.work[l.shown[i]])) {
            /* Skip this monitor, keep the others. */
            log_error(_("win32: cannot create overlay window"));
            if (o->cr) cairo_destroy(o->cr);
            if (o->surface) cairo_surface_destroy(o->surface);
            if (o->hwnd) DestroyWindow(o->hwnd);
            memset(o, 0, sizeof *o);
            b->n_overlays--;
        }
    }

    backend_log_layout("win32", b->requested, b->n_requested, &l);
    if (out) {
        *out = l;
        out->n_shown = b->n_overlays;
    }
}

static void win_set_screens(void *impl, const int *screens, int n,
                            screen_layout_t *out) {
    win_backend_t *b = impl;
    if (n > MAX_SCREENS) n = MAX_SCREENS;
    memcpy(b->requested, screens, sizeof(int) * (size_t)n);
    b->n_requested = n;
    build_overlays(b, out);
}

static bool win_tick(void *impl) {
    win_backend_t *b = impl;
    if (!topo_due(&b->topo)) return false;
    log_info(_("win32: monitor configuration changed"));
    build_overlays(b, NULL);
    return true;
}

static void win_destroy(void *impl) {
    win_backend_t *b = impl;
    if (!b) return;
    destroy_overlays(b);
    if (b->helper) DestroyWindow(b->helper);
    if (b->class_registered) UnregisterClassW(WINDOW_CLASS, b->hinst);
    free(b);
}

static const backend_vtable_t WIN32_VT = {
    .get_fd = win_get_fd,
    .pump = win_pump,
    .render = win_render,
    .set_screens = win_set_screens,
    .tick = win_tick,
    .destroy = win_destroy,
    .name = "win32",
};

overlay_backend_t *backend_create_win32(const config_t *cfg, char **err) {
    win_backend_t *b = calloc(1, sizeof *b);
    if (!b) { if (err) *err = dupf("out of memory"); return NULL; }

    /* Work in physical pixels (the text size is a % of the screen). */
    set_dpi_awareness();

    b->hinst = GetModuleHandleW(NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = overlay_wndproc;
    wc.hInstance = b->hinst;
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (err) *err = dupf(_("cannot register window class"));
        free(b);
        return NULL;
    }
    b->class_registered = true;

    /* Hidden top-level window (never shown): receives WM_DISPLAYCHANGE even
     * when there is no overlay. Not HWND_MESSAGE: message-only windows do
     * not get broadcasts. */
    b->helper = CreateWindowExW(WS_EX_TOOLWINDOW, WINDOW_CLASS,
                                L"overlay-timer-helper", WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, b->hinst, b);
    if (!b->helper)
        log_warn(_("win32: cannot create helper window; monitor changes "
                   "may not be detected"));

    int n = cfg->n_screens < MAX_SCREENS ? cfg->n_screens : MAX_SCREENS;
    memcpy(b->requested, cfg->screens, sizeof(int) * (size_t)n);
    b->n_requested = n;
    build_overlays(b, NULL);

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
