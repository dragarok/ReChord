/*
 * icons.h -- the UI's 12x12 1-bit icons (DESIGN.md 2.3), drawn into a row band.
 *
 * The bitmaps are tools/ui_mockups.py's ICONS (project repo), turned into
 * fonts/icons_data.c by tools/gen_font.py; ids in fonts/icons_ids.h (ICON_FOLDER ...,
 * in that order). Every icon is 12 rows; most are 12 wide, the chevron 7.
 */
#ifndef RECHORD_ICONS_H
#define RECHORD_ICONS_H

#include <stdint.h>

#include "fonts/icons_ids.h"

#define ICON_H 12

typedef struct {
    uint8_t w;
    uint16_t rows[ICON_H];            /* bit 15 = the leftmost pixel */
#if ICON_HAS_HALF
    uint16_t half[ICON_H];            /* 50 % pixels */
#endif
} icon_t;

extern const icon_t icon_table[ICON_COUNT];

/*
 * Draw icon `id` with its top-left pixel at panel (x, y) in `color`, into the band
 * holding panel rows band_y .. band_y+band_h-1, 320 RGB565 pixels a row (as
 * text_draw_band). Clipped to the band and to columns 0..319. Returns the icon's
 * width (0 for an unknown id).
 */
int icon_draw_band(int id, uint16_t *band, int band_y, int band_h, int x, int y, uint16_t color);
int icon_width(int id);

#endif /* RECHORD_ICONS_H */
