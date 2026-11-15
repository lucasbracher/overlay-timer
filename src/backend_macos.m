/*
 * macOS backend (AppKit).
 *
 * One borderless, transparent, click-through window per selected screen,
 * covering the screen's visible frame (excluding menu bar and Dock). Each
 * frame is drawn with render_frame() into a Cairo image surface, converted to
 * a CGImage and set as the contents of the window's layer.
 *
 * Cocoa has no pollable fd, so get_fd() returns -1 and the daemon calls
 * pump() on every loop iteration; pump() drains pending events without
 * blocking. NSApp's run loop is never started.
 *
 * Compiled without ARC (manual retain/release), so NSWindow pointers can live
 * in plain C structs.
 *
 * IMPORTANT: this backend must be created after daemonize(): AppKit must not
 * be initialized before fork().
 */

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_internal.h"
#include "i18n.h"
#include "render.h"

typedef struct {
    NSWindow *win;
    int w, h;            /* logical size (points) */
    int min_dim;
    double scale;        /* backing scale factor (2.0 on Retina) */
    cairo_surface_t *surface;  /* pixel size = logical size * scale */
    cairo_t *cr;
} mac_overlay_t;

typedef struct {
    mac_overlay_t *overlays;
    int n_overlays;
    CGColorSpaceRef colorspace;
} mac_backend_t;

static char *dupf(const char *s) { return s ? strdup(s) : NULL; }

/* Copies the surface pixels into a new CGImage (caller releases). Cairo's
 * ARGB32 is premultiplied, native-endian 32-bit, i.e. BGRA in memory on
 * little-endian machines. */
static CGImageRef image_from_surface(cairo_surface_t *s, CGColorSpaceRef cs) {
    int pw = cairo_image_surface_get_width(s);
    int ph = cairo_image_surface_get_height(s);
    int stride = cairo_image_surface_get_stride(s);
    const unsigned char *data = cairo_image_surface_get_data(s);

    CFDataRef d = CFDataCreate(NULL, data, (CFIndex)stride * ph);
    CGDataProviderRef p = CGDataProviderCreateWithCFData(d);
    CGImageRef img = CGImageCreate(
        (size_t)pw, (size_t)ph, 8, 32, (size_t)stride, cs,
        kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Host,
        p, NULL, false, kCGRenderingIntentDefault);
    CGDataProviderRelease(p);
    CFRelease(d);
    return img;
}

static int mac_get_fd(void *impl) {
    (void)impl;
    return -1;
}

static void mac_pump(void *impl) {
    (void)impl;
    @autoreleasepool {
        NSEvent *ev;
        while ((ev = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES]))
            [NSApp sendEvent:ev];
    }
}

static void mac_render(void *impl, const overlay_frame_t *frame) {
    mac_backend_t *b = impl;
    @autoreleasepool {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];  /* no fade animations */
        for (int i = 0; i < b->n_overlays; i++) {
            mac_overlay_t *o = &b->overlays[i];
            render_frame(o->cr, o->w, o->h, o->min_dim, frame);
            cairo_surface_flush(o->surface);

            CGImageRef img = image_from_surface(o->surface, b->colorspace);
            o->win.contentView.layer.contents = (id)img;
            CGImageRelease(img);
        }
        [CATransaction commit];
        /* Push the changes now: the Cocoa run loop is not running. */
        [CATransaction flush];
    }
}

static void mac_destroy(void *impl) {
    mac_backend_t *b = impl;
    if (!b) return;
    @autoreleasepool {
        for (int i = 0; i < b->n_overlays; i++) {
            mac_overlay_t *o = &b->overlays[i];
            if (o->cr) cairo_destroy(o->cr);
            if (o->surface) cairo_surface_destroy(o->surface);
            if (o->win) {
                [o->win orderOut:nil];
                [o->win close];
                [o->win release];
            }
        }
    }
    free(b->overlays);
    if (b->colorspace) CGColorSpaceRelease(b->colorspace);
    free(b);
}

static const backend_vtable_t MACOS_VT = {
    .get_fd = mac_get_fd,
    .pump = mac_pump,
    .render = mac_render,
    .destroy = mac_destroy,
    .name = "macos",
};

/* Creates the overlay window for one screen. */
static void create_overlay(mac_overlay_t *o, NSScreen *screen) {
    NSRect r = screen.visibleFrame;
    o->w = (int)r.size.width;
    o->h = (int)r.size.height;
    o->min_dim = o->w < o->h ? o->w : o->h;
    o->scale = screen.backingScaleFactor > 0 ? screen.backingScaleFactor : 1.0;

    NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                styleMask:NSWindowStyleMaskBorderless
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    win.releasedWhenClosed = NO;
    win.opaque = NO;
    win.backgroundColor = [NSColor clearColor];
    win.hasShadow = NO;
    win.ignoresMouseEvents = YES;  /* click-through */
    win.level = NSStatusWindowLevel;
    win.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                             NSWindowCollectionBehaviorStationary |
                             NSWindowCollectionBehaviorFullScreenAuxiliary |
                             NSWindowCollectionBehaviorIgnoresCycle;

    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, r.size.width,
                                                            r.size.height)];
    view.wantsLayer = YES;
    view.layer.contentsScale = o->scale;
    view.layer.contentsGravity = kCAGravityResize;
    win.contentView = view;
    [view release];

    [win orderFrontRegardless];  /* show without activating the app */
    o->win = win;

    int pw = (int)(o->w * o->scale), ph = (int)(o->h * o->scale);
    o->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    o->cr = cairo_create(o->surface);
    cairo_scale(o->cr, o->scale, o->scale);  /* draw in logical units */
}

overlay_backend_t *backend_create_macos(const config_t *cfg, char **err) {
    mac_backend_t *b = calloc(1, sizeof *b);
    if (!b) { if (err) *err = dupf("out of memory"); return NULL; }

    @autoreleasepool {
        [NSApplication sharedApplication];
        /* No Dock icon, no menu bar, never takes focus. */
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];

        b->colorspace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

        NSArray<NSScreen *> *screens = [NSScreen screens];
        int nmon = (int)screens.count;

        b->overlays = calloc(cfg->n_screens > 0 ? cfg->n_screens : 1,
                             sizeof(mac_overlay_t));
        for (int i = 0; i < cfg->n_screens; i++) {
            int idx = cfg->screens[i];
            if (idx < 0 || idx >= nmon) {
                fprintf(stderr,
                        _("warning: screen index %d out of range (have %d)\n"),
                        idx, nmon);
                continue;
            }
            create_overlay(&b->overlays[b->n_overlays], screens[idx]);
            b->n_overlays++;
        }
    }

    if (b->n_overlays == 0) {
        if (err) *err = dupf(_("no valid screen to display on"));
        mac_destroy(b);
        return NULL;
    }

    overlay_backend_t *w = backend_wrap(&MACOS_VT, b);
    if (!w) {
        mac_destroy(b);
        if (err) *err = dupf("out of memory");
    }
    return w;
}
