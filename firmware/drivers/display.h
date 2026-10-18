/*
 * display.h -- ReChord clean-room RGB565 display driver (public API).
 *
 * Hardware contract (docs/re/route-b-minimum.md section 3,
 * "Display contract"):
 *   - DCS-style panel driven over the MCU command/data interface
 *   - windowing by 0x2A CASET (column set), 0x2B RASET (row set),
 *     pixel stream by 0x2C RAMWR
 *   - RGB565 pixels, 320 px wide, framebuffer row pitch 640 bytes
 *     (320 px * 2 B) -- see DISPLAY_ROW_BYTES
 *
 * Model: a display list (no framebuffer, see "the display list" below), rendered band by band
 * into a 16-row buffer on the way to the panel by display_flush_rows() / display_flush_rect().
 * Rectangles use INCLUSIVE coordinates (so the full screen is
 * display_fill_rect(0, 0, DISPLAY_W - 1, DISPLAY_H - 1, c)).
 *
 * The panel transport lives behind panel_io.h (panel_io_rknanoc.c on
 * target, panel_io_sim.c on the host), so this file's logic is unit-
 * testable with plain `cc`.
 */

#ifndef RECHORD_DISPLAY_H
#define RECHORD_DISPLAY_H

#include <stdint.h>

/* Screen width in pixels (hardware contract: 320 px, x range 0..0x13F). */
#define DISPLAY_W 320

/*
 * Screen height in pixels.
 *
 * SINGLE CONFIG KNOB, documented: 170 rows is the height observed in the
 * boot area of the reference firmware (0..0xA9 inclusive; see
 * docs/re/route-b-minimum.md section 3), and the extracted init windows
 * (docs/rewrite/drivers.md) confirm a 170-row visible band on the panel.
 * Raising this define grows the framebuffer and everything follows
 * automatically -- but check PANEL_ROW_OFFSET first.
 */
#define DISPLAY_H 170

/*
 * Physical panel row index of logical row 0 (hardware contract,
 * extracted from the reference flush routine at 0x03051D40: every RASET
 * parameter is the row index + 35). The visible 170-row band sits at
 * panel rows 35..204 (0x23..0xCC); the extracted variant-B init window
 * 0x2B = 00 23 00 CC corroborates exactly this band. Do not change
 * without hardware evidence.
 */
#define PANEL_ROW_OFFSET 35

/* Framebuffer row pitch in BYTES -- fixed by the hardware contract
 * ("640 bytes/row framebuffer layout"). Changing DISPLAY_W changes this;
 * do not change it independently. */
#define DISPLAY_ROW_BYTES (DISPLAY_W * 2)

/* Rows rendered at a time (one band buffer, 16 x 320 x 2 = 10 KiB; RECHO 0.10 pushes 16-row bands too).
 * Two band buffers: while one is on its way to the panel (DMA), the next band renders into the other. */
#define DISPLAY_BAND_ROWS 16

/*
 * Bring up the panel transport and the panel itself (panel power-on init),
 * clear the screen to black and flush it, mirroring the observed
 * one-shot draw_init contract (panel_init + clear screen + flush).
 * Safe to call once at boot; drawing afterwards is visible only after a flush.
 */
void display_init(void);

/* ---- the display list --------------------------------------------------------------------------
 * There is no framebuffer: every draw call appends an op to a list, and a flush renders the list
 * band by band (DISPLAY_BAND_ROWS rows, only the columns flushed) on its way to the panel. Ops draw
 * in call order (painter's order). An OPAQUE op (fill, 8x8 text cell, opaque custom) drops every
 * older op it covers entirely, so a screen that repaints itself does not grow the list. A screen
 * starts its draw() with display_clear(): one op again. Every op has a bounding box (x and y), so
 * a flush of a rectangle visits only the ops that touch it.
 * Coordinates are panel pixels, INCLUSIVE, clipped to the screen; an empty box draws nothing.
 * Nothing is visible until display_flush_rows() / display_flush_rect().
 *
 * Every op must be a pure function of its pixel position: a flush of any sub-rectangle produces
 * exactly the pixels a full flush would (tests: drivers/tests/test_display.c, "partial == full").
 */

/* Solid rectangle (opaque). */
void display_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565);

/* Horizontal line, one pixel high (an opaque fill). */
void display_hline(int x0, int x1, int y, uint16_t rgb565);

/* Whole screen in one colour: the list starts over (opaque, covers everything). */
void display_clear(uint16_t rgb565);

/*
 * ASCII string with its top-left glyph cell at (x, y), 8x8 font (font.h), opaque (fg = glyph
 * pixels, bg = cell background). Clipped. The legacy text of today's screens.
 */
void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg);

/*
 * A UTF-8 run in a proportional font (drivers/text.h, text_draw_band): pen at x, baseline in panel
 * coordinates, glyphs (2-bit alpha) blended over what lies below in fg. Drawn only inside the clip
 * box x0..x1, y0..y1 (inclusive, like every display.h box; display.c hands text.h its exclusive end), which is also the op's bounding box: give the box the text lives in (a list
 * row, a header field). Not opaque. The bytes are copied (the string may be a temporary); the
 * font is not (a const font_t from text.h).
 */
struct font;                         /* font_t of text.h */
void display_text(const struct font *f, int x0, int y0, int x1, int y1, int x, int baseline,
                  const char *utf8, int nbytes, uint16_t fg);

/*
 * A 1-bit icon, top-left at (x, y), w x h pixels: rows of (w + 7) / 8 bytes, bit 7 of a row's first
 * byte = its leftmost pixel; 1 = fg, 0 = transparent (not opaque). 'bits' is NOT copied: it must
 * outlive the op (const data in flash).
 */
void display_icon(int x, int y, int w, int h, const uint8_t *bits, uint16_t fg);

/*
 * A custom band renderer, for things that are cheaper to compute than to describe in ops (a
 * progress bar, a level meter, a spectrum column): fn is called for every band that a flush
 * renders through the op's box x0..x1, y0..y1. It writes the pixels of its box that lie in the band
 * (rows band_y .. band_y + band_h - 1, panel coordinates) and in the column window cx0..cx1
 * (already intersected with the box): pixel (x, y) is band[(y - band_y) * DISPLAY_W + x]. It must
 * not write anything else, and each pixel must depend only on (x, y, arg): never on which band or
 * window is being rendered. opaque = 1 promises every pixel of the box is written (the op then
 * drops the ops it covers). 'arg' is the caller's value (a position, a percentage, a pointer).
 */
typedef struct {
    uint16_t *band;                  /* row r, column x: band[r * DISPLAY_W + x] */
    int band_y, band_h;              /* panel rows of the band */
    int x0, y0, x1, y1;              /* the op's box (screen-clipped) */
    int ya, yb;                      /* rows of the box inside this band (inclusive) */
    int cx0, cx1;                    /* columns to write (inclusive) */
    uint32_t arg;
} display_band_t;
typedef void (*display_band_fn)(const display_band_t *b);
void display_custom(int x0, int y0, int x1, int y1, display_band_fn fn, uint32_t arg, int opaque);

/* Ops in the list now, the most ever, and ops lost to a full list (the oldest goes first). */
#define DISPLAY_OPS_MAX 192
unsigned display_op_count(void);
extern unsigned display_ops_dropped;

/* ---- flush -------------------------------------------------------------------------------------- */

/*
 * Push framebuffer rows y0..y1 (INCLUSIVE) to the panel, full width. Rows are clamped to
 * 0..DISPLAY_H-1; a reversed or empty range is a no-op.
 * Logical row y goes to physical panel row y + PANEL_ROW_OFFSET (see PANEL_ROW_OFFSET above).
 */
void display_flush_rows(int y0, int y1);

/*
 * Push only the rectangle x0..x1, y0..y1 (INCLUSIVE, clipped): CASET x0..x1, RASET, RAMWR per band,
 * and only those columns are rendered. The pixels equal a full flush's.
 */
void display_flush_rect(int x0, int y0, int x1, int y1);

/*
 * Screenshot seam (there is no framebuffer): render rows y..y+n-1 of what the panel shows into a
 * band buffer and return it -- n rows of DISPLAY_W RGB565 pixels, row-major, word aligned. n is
 * clamped to 1..DISPLAY_STRIP_ROWS and the screen; valid until the next display call.
 */
#define DISPLAY_STRIP_ROWS DISPLAY_BAND_ROWS
const uint16_t *display_render_rows(int y, int n);

/* ---- instrumentation ------------------------------------------------------------------------------
 * A frame is what one redraw pushes (ui_render: the screen's draw() and its dirty rectangles).
 * display_frame_end() logs "ui: frame <ms> rows <n> px <n> path <p>" when the frame took at least
 * DISPLAY_SLOW_MS from display_frame_begin(), and once after a display_mark_input(t, in), when that
 * frame put pixels on the panel:
 *   ui: key->px <ms> ms px <n>: in <ms>, loop <ms>, wait <ms>, render <us> us, push <us> us
 * <ms> from t, the finger's edge (the press for a DOWN, the release for a TAP or a RELEASE; a HOLD or
 * a REPEAT: when it happened), to the frame on the panel; in = t to the event (the input's
 * confirmation, services/input.h in_ms), loop = the event to its handling (display_mark_input's
 * call), wait = the handling to the frame's start, render and push = what the frame's bands spent
 * rendering and on the bus (push: the CPU's bus writes or the wait for the DMA); the rest of <ms> is
 * the screen's draw() building the list. A flush outside a frame counts as a frame of its own.
 * Lines go to the logger set with display_set_logger() (none: nothing is logged). */
#define DISPLAY_SLOW_MS 30u
void display_frame_begin(void);
void display_frame_end(void);
void display_mark_input(uint32_t t_ms, uint32_t in_ms);
typedef void (*display_log_fn)(const char *fmt, ...);
void display_set_logger(display_log_fn fn);

typedef struct {
    uint32_t frames, slow;           /* frames, and those over DISPLAY_SLOW_MS */
    uint32_t last_us, worst_us;      /* frame time: the last, the worst */
    uint32_t last_rows, last_px;     /* what the last frame pushed */
    uint32_t key_last_ms, key_worst_ms;   /* key->px */
    uint32_t render_us, push_us;     /* cumulative: render, push (wait included) */
} display_stats_t;
const display_stats_t *display_stats(void);

/*
 * Panel power-on init sequence: emits the DCS command stream extracted
 * from the reference firmware's panel bring-up (sleep out + delay, vendor
 * power/gamma commands, MADCTL 0x36, COLMOD 0x3A, INVON 0x21, DISPON
 * 0x29). Implemented in panel_init_rknanoc.c (byte-exact tables plus
 * per-command documentation; docs/rewrite/drivers.md has the full
 * trace).
 *
 * The reference has TWO init variants selected by a probed panel ID;
 * since panel_io has no bus-read primitive yet, the variant is chosen at
 * compile time (PANEL_INIT_VARIANT, default 1 = variant B -- UNVERIFIED
 * on hardware). display_init() calls this automatically.
 */
void panel_init_sequence(void);

/*
 * Emit one specific extracted init variant (0 = variant A, 1 = variant
 * B) regardless of the compile-time choice; returns 0 on success, -1 for
 * an unknown variant number (the image carries only the compile-time
 * variant: the other one is -1 there). Bring-up/test seam so both streams
 * can be exercised (the host tests).
 */
int panel_init_run_variant(int variant);

#endif /* RECHORD_DISPLAY_H */
