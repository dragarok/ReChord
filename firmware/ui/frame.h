/*
 * frame.h -- the frame every screen shares (DESIGN.md 2.1, revision 2: no key bar): the header line
 * (panel rows 0..17) with the 2-px hold line under it (rows 18..19, the divider on 19), and the
 * body (rows 20..169), plus the drawing helpers the templates use (text runs, 1-bit icons, fills;
 * all display-list ops, colours only from theme_get()). Toasts and the VOL+ & VOL- shortcut layer
 * take the header line for a moment.
 *
 * Change detection is per field: frame_status() compares each header field (its text and its x
 * position) and invalidates only what changed (the volume field is about 40 x 18 px). The key
 * labels (frame_keys) are not drawn; they only say which keys have a hold (the hold line).
 */
#ifndef RECHORD_UI_FRAME_H
#define RECHORD_UI_FRAME_H

#include <stdint.h>

#include "drivers/display.h"
#include "drivers/text.h"
#include "drivers/icons.h"
#include "../theme/theme.h"

#define HDR_Y    0                /* header: rows 0..19 (text 0..17, the hold line 18..19, divider on 19) */
#define HOLD_Y   18
#define BODY_Y   20               /* body: rows 20..169 */
#define LIST_Y   21               /* list rows: 7 of 21 px from row 21 */
#define ROW_H    21
#define NROWS    7
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

/* ---- the keys (not drawn: their labels say which keys have a hold) ---- */
enum { SLOT_VOLP, SLOT_VOLM, SLOT_PLAY, SLOT_MENU, SLOT_BACK, NSLOTS };
int key_slot(int key);                                      /* KEY_* -> SLOT_*, -1 */
typedef struct { const char *tap[NSLOTS], *hold[NSLOTS]; } keybar_t;
void frame_keys(const keybar_t *k);       /* this screen's labels (the Key guide's data) */
void frame_press(int key);                /* a key down: its hold line while it has a hold; KEY_NONE: up */
int  frame_pressed(void);
void frame_layer(const char *text);       /* the shortcut layer: the header line inverted, naming the keys; NULL: off */
enum { TOAST_TEXT, TOAST_WARN, TOAST_DANGER };
void frame_toast(int icon, int tone, const char *text);   /* 1.5 s in place of the header line */

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
