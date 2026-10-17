/*
 * text.h -- proportional UTF-8 text into a display band (contract between the renderer and the
 * text engine).
 *
 * The text engine (drivers/text.c, fonts baked offline: 2-bit alpha glyphs) implements these; the
 * display list (display.c, display_text()) calls text_draw_band() for every band a text op
 * touches. Until text.c is linked, text_stub.c gives weak stand-ins that draw nothing.
 */
#ifndef RECHORD_TEXT_H
#define RECHORD_TEXT_H

#include <stdint.h>

typedef struct font font_t;           /* opaque */

/* Draw a UTF-8 run into a band buffer of 320 x band_h RGB565 pixels whose first row is panel row
 * band_y; x, baseline in panel coordinates; clipped to [clip_x0, clip_x1] and to the band's rows;
 * returns the advance in px. Glyphs are 2-bit alpha, blended over what is already in the band. */
int text_draw_band(const font_t *f, uint16_t *band, int band_y, int band_h,
                   int x, int baseline, int clip_x0, int clip_x1,
                   const char *utf8, int nbytes, uint16_t fg);
int text_width(const font_t *f, const char *utf8, int nbytes);

#endif
