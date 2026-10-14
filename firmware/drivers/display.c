/*
 * display.c -- ReChord clean-room RGB565 display driver.
 *
 * Implements the display.h contract against the panel_io.h transport.
 * Pure C99, no hardware headers: everything target-specific is behind
 * panel_io (panel_io_rknanoc.c on target, panel_io_sim.c on host).
 *
 * Draw calls only record opaque rectangles (see below); display_flush_rows()
 * renders row ranges and pushes them over DCS 0x2A/0x2B/0x2C.
 */

#include <string.h>

#include "display.h"
#include "font.h"
#include "panel_io.h"

/* DCS command bytes used by the flush path. */
#define DCS_CASET  0x2A  /* column address set */
#define DCS_RASET  0x2B  /* row address set    */
#define DCS_RAMWR  0x2C  /* memory write       */

/* No framebuffer: the screen is kept as a list of opaque rectangles (fills and
 * text cells) since the last full clear, and display_flush_rows() renders it
 * strip by strip into an 8-row buffer on the way to the panel. A new rectangle
 * drops every older one it covers, so redrawing the same rows does not grow the
 * list. ~8 KiB instead of a 106 KiB (170 x 320 x 2) framebuffer. */
#define OPS_MAX    160
#define TEXT_BYTES 3072
#define STRIP_ROWS 8

typedef struct {
    int16_t x0, y0, x1, y1;          /* inclusive, clipped to the screen */
    uint16_t fg, bg;                 /* fill: bg */
    uint16_t text;                   /* offset in s_text, 0xFFFF = fill */
} op_t;

static op_t s_ops[OPS_MAX];
static unsigned s_nops;
static char s_text[TEXT_BYTES];
static unsigned s_text_used;
static uint16_t s_strip[STRIP_ROWS][DISPLAY_W];
unsigned display_ops_dropped;        /* list overflowed (oldest op lost) */

static void text_compact(void)
{
    char tmp[TEXT_BYTES];
    unsigned used = 0;

    for (unsigned i = 0; i < s_nops; i++) {
        if (s_ops[i].text != 0xFFFFu) {
            const char *t = s_text + s_ops[i].text;
            unsigned n = (unsigned)strlen(t) + 1u;

            memcpy(tmp + used, t, n);
            s_ops[i].text = (uint16_t)used;
            used += n;
        }
    }
    memcpy(s_text, tmp, used);
    s_text_used = used;
}

static void add_op(int x0, int y0, int x1, int y1, uint16_t fg, uint16_t bg, const char *text, unsigned len)
{
    op_t o;
    unsigned w = 0;

    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > DISPLAY_W - 1) { x1 = DISPLAY_W - 1; }
    if (y1 > DISPLAY_H - 1) { y1 = DISPLAY_H - 1; }
    if (x0 > x1 || y0 > y1)
        return;
    /* drop what the new opaque rectangle hides */
    for (unsigned r = 0; r < s_nops; r++) {
        const op_t *q = &s_ops[r];

        if (!(q->x0 >= x0 && q->x1 <= x1 && q->y0 >= y0 && q->y1 <= y1))
            s_ops[w++] = *q;
    }
    s_nops = w;
    if (s_nops == OPS_MAX) {                         /* full: forget the oldest */
        memmove(s_ops, s_ops + 1, (OPS_MAX - 1u) * sizeof(op_t));
        s_nops--;
        display_ops_dropped++;
    }
    o.x0 = (int16_t)x0; o.y0 = (int16_t)y0; o.x1 = (int16_t)x1; o.y1 = (int16_t)y1;
    o.fg = fg;
    o.bg = bg;
    o.text = 0xFFFFu;
    if (text) {
        if (s_text_used + len + 1u > TEXT_BYTES)
            text_compact();
        if (s_text_used + len + 1u > TEXT_BYTES)
            return;                                  /* cannot happen with OPS_MAX lines */
        memcpy(s_text + s_text_used, text, len);
        s_text[s_text_used + len] = 0;
        o.text = (uint16_t)s_text_used;
        s_text_used += len + 1u;
    }
    s_ops[s_nops++] = o;
    if (s_nops == 1u && !text)
        s_text_used = 0;                             /* a full clear: arena empty again */
}

/* Render rows [y, y + n) of the list into s_strip. */
static void render_strip(int y, int n)
{
    font_surface_t dst = { &s_strip[0][0], DISPLAY_W, n, DISPLAY_W };

    for (int r = 0; r < n; r++)
        for (int x = 0; x < DISPLAY_W; x++)
            s_strip[r][x] = 0;
    for (unsigned i = 0; i < s_nops; i++) {
        const op_t *o = &s_ops[i];

        if (o->y1 < y || o->y0 >= y + n)
            continue;
        if (o->text == 0xFFFFu) {
            int a = o->y0 > y ? o->y0 : y, b = o->y1 < y + n - 1 ? o->y1 : y + n - 1;

            for (int r = a; r <= b; r++) {
                uint16_t *p = &s_strip[r - y][o->x0];

                for (int x = o->x0; x <= o->x1; x++)
                    *p++ = o->bg;
            }
        } else {
            font_draw_text(&dst, o->x0, o->y0 - y, s_text + o->text, o->fg, o->bg);
        }
    }
}

/* Send a 16-bit parameter big-endian (high byte first), as DCS expects. */
static void send_u16(uint16_t v)
{
    panel_io_send_data((uint8_t)(v >> 8));
    panel_io_send_data((uint8_t)(v & 0xFFu));
}

/* ---------------------------------------------------------------------------
 * Panel power-on init -- see panel_init_rknanoc.c.
 *
 * The DCS init sequence extracted from the reference firmware lives in
 * panel_init_rknanoc.c (byte-exact tables for the two panel variants the
 * reference supports, emitted through panel_io). This file only calls
 * panel_init_sequence() from display_init() below.
 * ------------------------------------------------------------------------ */

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------ */

void display_init(void)
{
    backlight_on();
    panel_io_init();
    panel_init_sequence();
    /* Observed draw_init contract: panel_init + clear screen + flush. */
    display_clear(0x0000);
    display_flush_rows(0, DISPLAY_H - 1);
}

void display_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565)
{
    add_op(x0, y0, x1, y1, 0, rgb565, NULL, 0);
}

void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    unsigned len = (unsigned)strlen(s), vis = len;

    if (x >= DISPLAY_W || y >= DISPLAY_H || y + FONT_CELL_H <= 0)
        return;
    if (x + (int)len * FONT_CELL_W > DISPLAY_W)      /* keep only what can show */
        vis = (unsigned)((DISPLAY_W - x + FONT_CELL_W - 1) / FONT_CELL_W);
    add_op(x, y, x + (int)vis * FONT_CELL_W - 1, y + FONT_CELL_H - 1, fg, bg, s, vis);
}

void display_flush_rows(int y0, int y1)
{
    int x, y;

    if (y0 < 0) { y0 = 0; }
    if (y1 > DISPLAY_H - 1) { y1 = DISPLAY_H - 1; }
    if (y0 > y1) {
        return;   /* empty range */
    }

    /* CASET: full row width (extracted contract: columns 0..0x013F). */
    panel_io_send_cmd(DCS_CASET);
    send_u16(0);
    send_u16((uint16_t)(DISPLAY_W - 1));

    /* RASET: the inclusive row range y0..y1, mapped to physical panel
     * rows by PANEL_ROW_OFFSET (extracted contract: the reference's
     * flush routine sends row + 35 -- see display.h). */
    panel_io_send_cmd(DCS_RASET);
    send_u16((uint16_t)(y0 + PANEL_ROW_OFFSET));
    send_u16((uint16_t)(y1 + PANEL_ROW_OFFSET));

    /* RAMWR: the pixels, row-major, high byte first (panel_io contract),
     * rendered from the op list one strip at a time. */
    panel_io_send_cmd(DCS_RAMWR);
    for (y = y0; y <= y1; y += STRIP_ROWS) {
        int n = y1 - y + 1 < STRIP_ROWS ? y1 - y + 1 : STRIP_ROWS;

        render_strip(y, n);
        for (int r = 0; r < n; r++)
            for (x = 0; x < DISPLAY_W; x++)
                panel_io_send_pixel(s_strip[r][x]);
    }
}

void display_clear(uint16_t rgb565)
{
    display_fill_rect(0, 0, DISPLAY_W - 1, DISPLAY_H - 1, rgb565);
}
