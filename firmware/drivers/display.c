/*
 * display.c -- ReChord clean-room RGB565 display driver.
 *
 * Implements the display.h contract against the panel_io.h transport.
 * Pure C99, no hardware headers: everything target-specific is behind
 * panel_io (panel_io_rknanoc.c on target, panel_io_sim.c on host).
 *
 * Draw calls append ops to a display list; a flush renders a rectangle of it band by band
 * (DISPLAY_BAND_ROWS rows, only the rectangle's columns) into one of two band buffers and hands
 * each band to panel_io_blit(). The transport may still be sending a band (DMA) while the next
 * one renders into the other buffer.
 */

#include <string.h>

#include "display.h"
#include "font.h"
#include "text.h"
#include "panel_io.h"

/* DCS command bytes. */
#define DCS_CASET  0x2A  /* column address set */
#define DCS_RASET  0x2B  /* row address set    */
#define DCS_RAMWR  0x2C  /* memory write       */

#define TEXT_BYTES 4096
#define BAND       DISPLAY_BAND_ROWS

enum { OP_FILL, OP_TEXT8, OP_TEXT, OP_ICON, OP_CUSTOM };
#define F_OPAQUE 1u

/* 28 bytes. */
typedef struct {
    int16_t x0, y0, x1, y1;          /* bounding box, inclusive, clipped to the screen */
    uint8_t kind, flags;
    uint16_t c;                      /* FILL: the colour; TEXT8, TEXT, ICON: fg */
    uint16_t bg;                     /* TEXT8: cell background */
    uint16_t len;                    /* TEXT8, TEXT: bytes in the arena; ICON: w | h << 8 */
    int16_t ox, oy;                  /* TEXT8: cell top-left; TEXT: pen x, baseline; ICON: top-left */
    const void *p;                   /* TEXT: font; ICON: bits; CUSTOM: renderer */
    uint32_t arg;                    /* TEXT8, TEXT: arena offset; CUSTOM: the caller's value */
} op_t;

static op_t s_ops[DISPLAY_OPS_MAX];
static unsigned s_nops;
static char s_text[TEXT_BYTES];
static unsigned s_text_used;
static uint16_t s_band[2][BAND * DISPLAY_W] __attribute__((aligned(4)));
static unsigned s_k;                 /* the band buffer the next band renders into */
unsigned display_ops_dropped;        /* list overflowed (oldest op lost) */

static display_stats_t s_st;
static display_log_fn s_log;
static int s_in_frame;
static uint32_t s_frame_t0, s_rows, s_px;
static uint32_t s_mark_ms;
static int s_marked;

static int has_text(const op_t *o)
{
    return o->kind == OP_TEXT8 || o->kind == OP_TEXT;
}

/* Strings sit in the arena in op order (both only ever append, and a dropped op keeps the order),
 * so the live ones slide down in place. */
static void text_compact(void)
{
    unsigned used = 0;

    for (unsigned i = 0; i < s_nops; i++) {
        if (has_text(&s_ops[i])) {
            memmove(s_text + used, s_text + s_ops[i].arg, s_ops[i].len);
            s_ops[i].arg = used;
            used += s_ops[i].len;
        }
    }
    s_text_used = used;
}

/* A new op with its box clipped (NULL: nothing visible). An opaque one first drops every older op
 * inside its box. */
static op_t *add_op(int x0, int y0, int x1, int y1, unsigned kind, unsigned flags)
{
    op_t *o;

    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > DISPLAY_W - 1) { x1 = DISPLAY_W - 1; }
    if (y1 > DISPLAY_H - 1) { y1 = DISPLAY_H - 1; }
    if (x0 > x1 || y0 > y1)
        return NULL;
    if (flags & F_OPAQUE) {
        unsigned w = 0;

        for (unsigned r = 0; r < s_nops; r++) {
            const op_t *q = &s_ops[r];

            if (!(q->x0 >= x0 && q->x1 <= x1 && q->y0 >= y0 && q->y1 <= y1))
                s_ops[w++] = *q;
        }
        s_nops = w;
        if (!s_nops)
            s_text_used = 0;                         /* nothing left in the arena */
    }
    if (s_nops == DISPLAY_OPS_MAX) {                 /* full: forget the oldest */
        memmove(s_ops, s_ops + 1, (DISPLAY_OPS_MAX - 1u) * sizeof(op_t));
        s_nops--;
        display_ops_dropped++;
    }
    o = &s_ops[s_nops++];
    memset(o, 0, sizeof *o);
    o->x0 = (int16_t)x0; o->y0 = (int16_t)y0; o->x1 = (int16_t)x1; o->y1 = (int16_t)y1;
    o->kind = (uint8_t)kind;
    o->flags = (uint8_t)flags;
    return o;
}

/* The op's string into the arena; 0 = no room (the op is taken back). */
static int add_text(op_t *o, const char *s, unsigned len)
{
    if (s_text_used + len > TEXT_BYTES)
        text_compact();
    if (s_text_used + len > TEXT_BYTES) {
        s_nops--;                                    /* (o is the last op) */
        display_ops_dropped++;
        return 0;
    }
    memcpy(s_text + s_text_used, s, len);
    o->arg = s_text_used;
    o->len = (uint16_t)len;
    s_text_used += len;
    return 1;
}

/* ---- rendering ----------------------------------------------------------------------------------- */

#if defined(__GNUC__) && !defined(__clang__)
#define HOT __attribute__((optimize("O2")))            /* (the image is -Os: these are the hot loops) */
#else
#define HOT
#endif

static HOT void fill_span(uint16_t *p, int n, uint16_t c)
{
    uint32_t w = (uint32_t)c | (uint32_t)c << 16;

    if (n > 0 && ((uintptr_t)p & 2u)) {
        *p++ = c;
        n--;
    }
    {
        uint32_t *q = (uint32_t *)(void *)p;

        for (; n >= 8; n -= 8, q += 4) {
            q[0] = w; q[1] = w; q[2] = w; q[3] = w;
        }
        for (; n >= 2; n -= 2)
            *q++ = w;
        p = (uint16_t *)(void *)q;
    }
    if (n > 0)
        *p = c;
}

static HOT void draw_text8(const op_t *o, uint16_t *buf, int by, int a, int b, int xa, int xb)
{
    const char *s = s_text + o->arg;
    int k0 = (xa - o->ox) / FONT_CELL_W, k1 = (xb - o->ox) / FONT_CELL_W;

    for (int r = a; r <= b; r++) {
        uint16_t *row = buf + (r - by) * DISPLAY_W;
        int gr = r - o->oy;

        for (int k = k0; k <= k1 && k < (int)o->len; k++) {
            unsigned bits = font_glyph((uint8_t)s[k])[gr];
            int cx = o->ox + k * FONT_CELL_W;
            int c0 = xa > cx ? xa - cx : 0, c1 = xb < cx + 7 ? xb - cx : 7;

            for (int c = c0; c <= c1; c++)
                row[cx + c] = (bits & (0x80u >> c)) ? o->c : o->bg;
        }
    }
}

static HOT void draw_icon(const op_t *o, uint16_t *buf, int by, int a, int b, int xa, int xb)
{
    const uint8_t *bits = o->p;
    int w = o->len & 0xFF, stride = (w + 7) / 8;

    for (int r = a; r <= b; r++) {
        uint16_t *row = buf + (r - by) * DISPLAY_W;
        const uint8_t *src = bits + (r - o->oy) * stride;

        for (int x = xa; x <= xb; x++) {
            int c = x - o->ox;

            if (src[c >> 3] & (0x80u >> (c & 7)))
                row[x] = o->c;
        }
    }
}

/* Rows by .. by+bh-1, columns cx0..cx1 of the list into buf (row r at buf + r * DISPLAY_W). */
static HOT void render(uint16_t *buf, int by, int bh, int cx0, int cx1)
{
    int ye = by + bh - 1;
    unsigned i, start = 0;
    int covered = 0;

    /* everything below the last opaque op that covers the whole window is hidden */
    for (i = s_nops; i-- > 0; ) {
        const op_t *o = &s_ops[i];

        if ((o->flags & F_OPAQUE) && o->x0 <= cx0 && o->x1 >= cx1 && o->y0 <= by && o->y1 >= ye) {
            start = i;
            covered = 1;
            break;
        }
    }
    if (!covered)
        for (int r = 0; r < bh; r++)
            fill_span(buf + r * DISPLAY_W + cx0, cx1 - cx0 + 1, 0);
    for (i = start; i < s_nops; i++) {
        const op_t *o = &s_ops[i];
        int a, b, xa, xb;

        if (o->y1 < by || o->y0 > ye || o->x1 < cx0 || o->x0 > cx1)
            continue;
        a = o->y0 > by ? o->y0 : by;
        b = o->y1 < ye ? o->y1 : ye;
        xa = o->x0 > cx0 ? o->x0 : cx0;
        xb = o->x1 < cx1 ? o->x1 : cx1;
        switch (o->kind) {
        case OP_FILL:
            for (int r = a; r <= b; r++)
                fill_span(buf + (r - by) * DISPLAY_W + xa, xb - xa + 1, o->c);
            break;
        case OP_TEXT8:
            draw_text8(o, buf, by, a, b, xa, xb);
            break;
        case OP_TEXT:
            (void)text_draw_band(o->p, buf + (a - by) * DISPLAY_W, a, b - a + 1, o->ox, o->oy, xa, xb + 1,
                                 s_text + o->arg, o->len, o->c);   /* (text.h: clip_x1 exclusive) */
            break;
        case OP_ICON:
            draw_icon(o, buf, by, a, b, xa, xb);
            break;
        case OP_CUSTOM: {
            display_band_t c;

            c.band = buf;
            c.band_y = by;
            c.band_h = bh;
            c.x0 = o->x0; c.y0 = o->y0; c.x1 = o->x1; c.y1 = o->y1;
            c.ya = a; c.yb = b;
            c.cx0 = xa; c.cx1 = xb;
            c.arg = o->arg;
            ((display_band_fn)o->p)(&c);
            break;
        }
        default:
            break;
        }
    }
}

/* ---- frame accounting ----------------------------------------------------------------------------- */

static void frame_done(uint32_t t0)
{
    uint32_t us = panel_io_now_us() - t0;

    if ((int32_t)us < 0)
        us = 0;                      /* (a clock that stepped back: no 4294960 ms frames) */

    s_st.frames++;
    s_st.last_us = us;
    if (us > s_st.worst_us)
        s_st.worst_us = us;
    s_st.last_rows = s_rows;
    s_st.last_px = s_px;
    if (us >= DISPLAY_SLOW_MS * 1000u) {
        s_st.slow++;
        if (s_log)
            s_log("ui: frame %u ms rows %u px %u path %s", (unsigned)(us / 1000u), (unsigned)s_rows,
                  (unsigned)s_px, panel_io_path());
    }
    if (s_marked && s_px) {
        uint32_t ms = panel_io_now_ms() - s_mark_ms;

        s_marked = 0;
        s_st.key_last_ms = ms;
        if (ms > s_st.key_worst_ms)
            s_st.key_worst_ms = ms;
        if (s_log)
            s_log("ui: key->px %u ms px %u", (unsigned)ms, (unsigned)s_px);   /* (px: what that frame pushed) */
    }
    s_rows = s_px = 0;
}

void display_frame_begin(void)
{
    s_in_frame = 1;
    s_frame_t0 = panel_io_now_us();
    s_rows = s_px = 0;
}

void display_frame_end(void)
{
    if (!s_in_frame)
        return;
    s_in_frame = 0;
    if (s_px)
        frame_done(s_frame_t0);
}

void display_mark_input(uint32_t t_ms)
{
    if (!s_marked) {                                 /* the oldest key not yet answered */
        s_marked = 1;
        s_mark_ms = t_ms;
    }
}

void display_set_logger(display_log_fn fn)
{
    s_log = fn;
}

const display_stats_t *display_stats(void)
{
    return &s_st;
}

/* ---------------------------------------------------------------------------
 * Panel power-on init -- see panel_init_rknanoc.c.
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

/* ---- the list -------------------------------------------------------------------------------------- */

void display_fill_rect(int x0, int y0, int x1, int y1, uint16_t rgb565)
{
    op_t *o = add_op(x0, y0, x1, y1, OP_FILL, F_OPAQUE);

    if (o)
        o->c = rgb565;
}

void display_hline(int x0, int x1, int y, uint16_t rgb565)
{
    display_fill_rect(x0, y, x1, y, rgb565);
}

void display_clear(uint16_t rgb565)
{
    display_fill_rect(0, 0, DISPLAY_W - 1, DISPLAY_H - 1, rgb565);
}

void display_draw_text(int x, int y, const char *s, uint16_t fg, uint16_t bg)
{
    unsigned len = (unsigned)strlen(s), vis = len;
    op_t *o;

    if (x >= DISPLAY_W || y >= DISPLAY_H || y + FONT_CELL_H <= 0 || !len)
        return;
    if (x + (int)len * FONT_CELL_W > DISPLAY_W)      /* keep only what can show */
        vis = (unsigned)((DISPLAY_W - x + FONT_CELL_W - 1) / FONT_CELL_W);
    o = add_op(x, y, x + (int)vis * FONT_CELL_W - 1, y + FONT_CELL_H - 1, OP_TEXT8, F_OPAQUE);
    if (o && add_text(o, s, vis)) {
        o->c = fg;
        o->bg = bg;
        o->ox = (int16_t)x;
        o->oy = (int16_t)y;
    }
}

void display_text(const struct font *f, int x0, int y0, int x1, int y1, int x, int baseline,
                  const char *utf8, int nbytes, uint16_t fg)
{
    op_t *o;

    if (!f || !utf8 || nbytes <= 0 || x > x1)
        return;
    o = add_op(x0, y0, x1, y1, OP_TEXT, 0);
    if (o && add_text(o, utf8, (unsigned)nbytes)) {
        o->c = fg;
        o->ox = (int16_t)x;
        o->oy = (int16_t)baseline;
        o->p = f;
    }
}

void display_icon(int x, int y, int w, int h, const uint8_t *bits, uint16_t fg)
{
    op_t *o;

    if (!bits || w <= 0 || h <= 0 || w > 255 || h > 255)
        return;
    o = add_op(x, y, x + w - 1, y + h - 1, OP_ICON, 0);
    if (o) {
        o->c = fg;
        o->len = (uint16_t)(w | h << 8);
        o->ox = (int16_t)x;
        o->oy = (int16_t)y;
        o->p = bits;
    }
}

void display_custom(int x0, int y0, int x1, int y1, display_band_fn fn, uint32_t arg, int opaque)
{
    op_t *o;

    if (!fn)
        return;
    o = add_op(x0, y0, x1, y1, OP_CUSTOM, opaque ? F_OPAQUE : 0);
    if (o) {
        o->p = (const void *)fn;
        o->arg = arg;
    }
}

unsigned display_op_count(void)
{
    return s_nops;
}

/* ---- flush -------------------------------------------------------------------------------------- */

void display_flush_rect(int x0, int y0, int x1, int y1)
{
    uint32_t t0 = panel_io_now_us(), t;

    if (x0 < 0) { x0 = 0; }
    if (y0 < 0) { y0 = 0; }
    if (x1 > DISPLAY_W - 1) { x1 = DISPLAY_W - 1; }
    if (y1 > DISPLAY_H - 1) { y1 = DISPLAY_H - 1; }
    if (x0 > x1 || y0 > y1)
        return;
    for (int y = y0; y <= y1; y += BAND) {
        int n = y1 - y + 1 < BAND ? y1 - y + 1 : BAND;
        uint16_t *buf = s_band[s_k];

        s_k ^= 1u;                   /* (the other buffer may still be on its way to the panel) */
        t = panel_io_now_us();
        render(buf, y, n, x0, x1);
        s_st.render_us += panel_io_now_us() - t;
        t = panel_io_now_us();
        panel_io_blit(x0, y + PANEL_ROW_OFFSET, x1, y + n - 1 + PANEL_ROW_OFFSET, buf + x0, DISPLAY_W);
        s_st.push_us += panel_io_now_us() - t;
    }
    t = panel_io_now_us();
    panel_io_sync();
    s_st.push_us += panel_io_now_us() - t;
    s_rows += (uint32_t)(y1 - y0 + 1);
    s_px += (uint32_t)(y1 - y0 + 1) * (uint32_t)(x1 - x0 + 1);
    if (!s_in_frame)
        frame_done(t0);
}

void display_flush_rows(int y0, int y1)
{
    display_flush_rect(0, y0, DISPLAY_W - 1, y1);
}

const uint16_t *display_render_rows(int y, int n)
{
    if (y < 0) { y = 0; }
    if (y > DISPLAY_H - 1) { y = DISPLAY_H - 1; }
    if (n > BAND) { n = BAND; }
    if (n > DISPLAY_H - y) { n = DISPLAY_H - y; }
    if (n < 1) { n = 1; }
    panel_io_sync();
    render(s_band[0], y, n, 0, DISPLAY_W - 1);
    return s_band[0];
}

/* The generic blit, for transports without a faster one (panel_io_sim.c): the window, then the
 * pixels one by one. */
void panel_io_blit_generic(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch);
void panel_io_blit_generic(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch)
{
    panel_io_send_cmd(DCS_CASET);
    panel_io_send_data((uint8_t)(x0 >> 8));
    panel_io_send_data((uint8_t)x0);
    panel_io_send_data((uint8_t)(x1 >> 8));
    panel_io_send_data((uint8_t)x1);
    panel_io_send_cmd(DCS_RASET);
    panel_io_send_data((uint8_t)(y0 >> 8));
    panel_io_send_data((uint8_t)y0);
    panel_io_send_data((uint8_t)(y1 >> 8));
    panel_io_send_data((uint8_t)y1);
    panel_io_send_cmd(DCS_RAMWR);
    for (int y = y0; y <= y1; y++, src += pitch)
        for (int x = 0; x <= x1 - x0; x++)
            panel_io_send_pixel(src[x]);
}
