#ifndef OVERLAY_TIMER_RENDER_H
#define OVERLAY_TIMER_RENDER_H

#include <cairo.h>
#include "backend.h"

/*
 * Backend-independent rendering (Pango + Cairo).
 *
 * Draws the frame into a cairo context whose surface has the size of the
 * screen (monitor) in question. The positioning of the text in the corner and
 * the placement of the small label are resolved here.
 */

/* Clears the surface to fully transparent. */
void render_clear(cairo_t *cr);

/* Draws `frame` on a monitor of dimensions mon_w x mon_h (in pixels).
 * `min_dim` is the smaller dimension used to scale the font (normally
 * min(mon_w, mon_h)). If frame->visible is false, it only clears. */
void render_frame(cairo_t *cr, int mon_w, int mon_h, int min_dim,
                  const overlay_frame_t *frame);

#endif /* OVERLAY_TIMER_RENDER_H */
