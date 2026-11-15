#include "render.h"

#include <pango/pangocairo.h>
#include <string.h>

/* Margin from the edge, as a fraction of the smaller dimension. */
#define MARGIN_FRAC 0.03
/* Label size relative to the counter. */
#define LABEL_FRAC 0.35
/* Spacing between counter and label, fraction of the counter height. */
#define GAP_FRAC 0.15

void render_clear(cairo_t *cr) {
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
}

static bool corner_is_top(corner_t c) {
    return c == CORNER_TL || c == CORNER_TR;
}
static bool corner_is_left(corner_t c) {
    return c == CORNER_TL || c == CORNER_BL;
}

/* Creates a Pango layout with the given family/size. */
static PangoLayout *make_layout(cairo_t *cr, const char *text,
                                const char *font, double px) {
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text ? text : "", -1);

    PangoFontDescription *desc = pango_font_description_new();
    if (font && *font)
        pango_font_description_set_family(desc, font);
    pango_font_description_set_absolute_size(desc, px * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    return layout;
}

void render_frame(cairo_t *cr, int mon_w, int mon_h, int min_dim,
                  const overlay_frame_t *f) {
    render_clear(cr);
    if (!f || !f->visible || !f->text || !*f->text) return;

    double margin = MARGIN_FRAC * min_dim;
    double main_px = (f->size / 100.0) * min_dim;
    if (main_px < 1) main_px = 1;

    const bool has_label = f->label && *f->label;
    double label_px = has_label ? main_px * LABEL_FRAC : 0;
    double gap = has_label ? main_px * GAP_FRAC : 0;

    /* Measure the counter. */
    PangoLayout *main_l = make_layout(cr, f->text, f->font, main_px);
    int mw, mh;
    pango_layout_get_pixel_size(main_l, &mw, &mh);

    /* Measure the label (if any). */
    PangoLayout *label_l = NULL;
    int lw = 0, lh = 0;
    if (has_label) {
        label_l = make_layout(cr, f->label, f->font, label_px);
        pango_layout_get_pixel_size(label_l, &lw, &lh);
    }

    /* Total block: counter + (gap + label). */
    int block_w = mw > lw ? mw : lw;
    int block_h = mh + (has_label ? (int)gap + lh : 0);

    /* X/Y position of the block according to the corner. */
    double bx = corner_is_left(f->corner) ? margin
                                          : (mon_w - margin - block_w);
    double by = corner_is_top(f->corner) ? margin
                                         : (mon_h - margin - block_h);

    /* Vertical order:
     *  - counter on top (corners TL/TR): counter, then label below.
     *  - counter at bottom (BL/BR): label on top, then counter.
     * Since the "counter" is always the edge element, we interpret the rule as:
     *   top corner    -> label BELOW the counter;
     *   bottom corner -> label ABOVE the counter. */
    double main_y, label_y;
    if (corner_is_top(f->corner)) {
        main_y = by;
        label_y = by + mh + gap;
    } else {
        label_y = by;
        main_y = by + (has_label ? lh + gap : 0);
    }

    /* Horizontal alignment within the block. */
    double main_x, label_x;
    if (corner_is_left(f->corner)) {
        main_x = bx;
        label_x = bx;
    } else {
        main_x = bx + (block_w - mw);
        label_x = bx + (block_w - lw);
    }

    double a = f->alpha / 100.0;
    cairo_set_source_rgba(cr, f->color.r / 255.0, f->color.g / 255.0,
                          f->color.b / 255.0, a);

    cairo_move_to(cr, main_x, main_y);
    pango_cairo_show_layout(cr, main_l);

    if (has_label) {
        cairo_move_to(cr, label_x, label_y);
        pango_cairo_show_layout(cr, label_l);
        g_object_unref(label_l);
    }
    g_object_unref(main_l);
}
