/*
 * frame.h -- the frame every screen shares (DESIGN.md 2.1): the key bar under the physical keys
 * (panel rows 0..19), the header (rows 20..37) and the body (rows 38..169), plus the drawing
 * helpers the templates use (text runs, 1-bit icons, fills; all display-list ops, colours only
 * from theme_get()).
 *
 * Change detection is per field: frame_keys() compares each slot's labels, frame_status() each
 * header field (its text and its x position), and invalidates only what changed (a slot is
 * 64 x 20 px, the volume field about 40 x 18).
 */
#ifndef RECHORD_UI_FRAME_H
#define RECHORD_UI_FRAME_H

#include <stdint.h>

#include "drivers/display.h"
#include "drivers/text.h"
#include "drivers/icons.h"
#include "../theme/theme.h"

#define KB_H     20               /* key bar: rows 0..19 (divider on 19) */
#define HDR_Y    20               /* header: rows 20..37 (divider on 37) */
#define BODY_Y   38               /* body: rows 38..169 */
#define ROW_H    22
#define NROWS    6
#define SLOT_W   64
#define NO_ICON  0xFF

#define T        (theme_get())

/* ---- drawing helpers ---- */
void ui_icon(int id, int x, int y, uint16_t c);
/* A UTF-8 run cut to max_w px ("..." when it does not fit), aligned 0 left, 1 centre, 2 right inside
 * x .. x + max_w - 1, baseline `base`, clipped to rows y0..y1. Returns the drawn width. */
int ui_text(const font_t *f, int x, int base, int max_w, int align, const char *s, uint16_t c, int y0, int y1);
void ui_fill(int x, int y, int w, int h, uint16_t c);      /* w x h at (x, y) */
void ui_frame_rect(int x, int y, int w, int h, uint16_t c); /* 1-px outline */
uint16_t ui_mix(uint16_t a, uint16_t b, int t256);          /* a..b, t/256 of the way */

/* ---- the key bar ---- */
enum { SLOT_VOLP, SLOT_VOLM, SLOT_PLAY, SLOT_MENU, SLOT_BACK, NSLOTS };
int key_slot(int key);                                      /* KEY_* -> SLOT_*, -1 */
typedef struct { const char *tap[NSLOTS], *hold[NSLOTS]; } keybar_t;
void frame_keys(const keybar_t *k);       /* this screen's labels (compared slot by slot) */
void frame_press(int key);                /* key-down feedback; KEY_NONE: released */
int  frame_pressed(void);
void frame_layer(const keybar_t *k);      /* the chord's shortcut layer (inverted), NULL: off */
enum { TOAST_TEXT, TOAST_WARN, TOAST_DANGER };
void frame_toast(int icon, int tone, const char *text);   /* 1.5 s over the key bar */

/* ---- the header ---- */
typedef struct {
    uint8_t icon;                 /* section icon (NO_ICON: none) */
    uint8_t np;                   /* Now Playing: note icon, the queue position, the mode icon */
    uint8_t mode_icon;            /* NP: ICON_SHUFFLE / REPEAT / REPEAT1 or NO_ICON */
    uint8_t play;                 /* lists: 0 none, 1 playing, 2 paused (+ elapsed) */
    uint8_t out;                  /* output icon: ICON_SPEAKER (jack), ICON_BT, ICON_USBC */
    uint8_t lock, charging, batt; /* lock icon; battery 0..100 */
    char title[96];               /* or the NP position "1 / 24" */
    char elapsed[8];
    char vol[16];                 /* "−32", "BT −20" */
} hdr_t;
void frame_status(const hdr_t *h);        /* compares field by field */

/* ---- each pass / each frame ---- */
void frame_tick(uint32_t now);            /* hold fill, toast timeout */
void frame_draw(void);                    /* the key bar and header ops (a screen's draw() calls it) */
void frame_invalidate(void);              /* the whole frame again (theme change, panel redraw) */

#endif
