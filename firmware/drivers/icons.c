/*
 * icons.c -- the 1-bit icon blitter (icons.h).
 */
#include "icons.h"

int icon_width(int id)
{
    return id >= 0 && id < ICON_COUNT ? icon_table[id].w : 0;
}

int icon_draw_band(int id, uint16_t *band, int band_y, int band_h, int x, int y, uint16_t color)
{
    const icon_t *ic;
    int r0, r1;

    if (id < 0 || id >= ICON_COUNT)
        return 0;
    ic = &icon_table[id];
    if (!band || band_h <= 0)
        return ic->w;
    r0 = y > band_y ? 0 : band_y - y;
    r1 = y + ICON_H < band_y + band_h ? ICON_H : band_y + band_h - y;
    for (int r = r0; r < r1; r++) {
        uint16_t *row = band + (y + r - band_y) * 320;
        unsigned bits = ic->rows[r];
#if ICON_HAS_HALF
        unsigned half = ic->half[r];
#endif

        for (int c = 0; c < ic->w; c++) {
            int px = x + c;

            if (px < 0 || px >= 320)
                continue;
            if (bits & (0x8000u >> c)) {
                row[px] = color;
            }
#if ICON_HAS_HALF
            else if (half & (0x8000u >> c)) {
                uint16_t b = row[px];   /* 50 %: average per channel */

                row[px] = (uint16_t)((((b ^ color) & 0xF7DEu) >> 1) + (b & color));
            }
#endif
        }
    }
    return ic->w;
}
