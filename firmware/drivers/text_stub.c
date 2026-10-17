/*
 * text_stub.c -- weak stand-ins for the text engine (text.h) until drivers/text.c is linked: they
 * draw nothing and measure nothing. A strong definition (text.c) replaces them at link time.
 */
#include "text.h"

__attribute__((weak)) int text_draw_band(const font_t *f, uint16_t *band, int band_y, int band_h,
                                         int x, int baseline, int clip_x0, int clip_x1,
                                         const char *utf8, int nbytes, uint16_t fg)
{
    (void)f; (void)band; (void)band_y; (void)band_h; (void)x; (void)baseline;
    (void)clip_x0; (void)clip_x1; (void)utf8; (void)nbytes; (void)fg;
    return 0;
}

__attribute__((weak)) int text_width(const font_t *f, const char *utf8, int nbytes)
{
    (void)f; (void)utf8; (void)nbytes;
    return 0;
}
