/*
 * test_display_list.c -- the display list v2 (display.h): every op kind, the column windows, and
 * the invariant the UI relies on: a flush of any rectangle, or any set of rectangles that covers
 * a change, leaves the panel exactly as a full flush would ("partial == full").
 *
 * A fake text engine (text.h) stands in for drivers/text.c: a deterministic glyph pattern
 * blended over the band, so text ops exercise the same clip and band arithmetic.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "panel_io.h"
#include "panel_io_sim.h"
#include "text.h"
#include "test_util.h"

/* (a handle only: the fake engine ignores what it points to, so the real text.h's struct font fits) */
static const uint8_t k_font_handle[4];
#define k_font (*(const font_t *)(const void *)k_font_handle)
#define ADV 7
static int s_text_calls;

static uint16_t blend(uint16_t d, uint16_t s)
{
    return (uint16_t)(((d & 0xF7DEu) >> 1) + ((s & 0xF7DEu) >> 1));
}

/* (text.h contract: clip_x1 is exclusive.) Glyph i of the run: a 6 x 10 cell at pen + 7 i, rows baseline - 9 .. baseline; its pixels where
 * (x * 7 + y * 3 + byte) % 5 == 0, blended (so the order of drawing shows). */
int text_draw_band(const font_t *f, uint16_t *band, int band_y, int band_h,
                   int x, int baseline, int clip_x0, int clip_x1,
                   const char *utf8, int nbytes, uint16_t fg)
{
    (void)f;
    s_text_calls++;
    for (int i = 0; i < nbytes; i++)
        for (int y = baseline - 9; y <= baseline; y++)
            for (int px = x + i * ADV; px < x + i * ADV + 6; px++) {
                if (y < band_y || y >= band_y + band_h || px < clip_x0 || px >= clip_x1)
                    continue;
                if ((px * 7 + y * 3 + (uint8_t)utf8[i]) % 5 == 0) {
                    uint16_t *p = &band[(y - band_y) * DISPLAY_W + px];

                    *p = blend(*p, fg);
                }
            }
    return nbytes * ADV;
}

int text_width(const font_t *f, const char *utf8, int nbytes)
{
    (void)f;
    (void)utf8;
    return nbytes * ADV;
}

/* A progress bar: arg = 0..1000 per mille filled; opaque. */
static void bar(const display_band_t *b)
{
    int split = b->x0 + (int)((b->x1 - b->x0 + 1) * b->arg / 1000u);

    for (int y = b->ya; y <= b->yb; y++)
        for (int x = b->cx0; x <= b->cx1; x++)
            b->band[(y - b->band_y) * DISPLAY_W + x] = x < split ? 0xF800u : ((x ^ y) & 1 ? 0x2104u : 0x4208u);
}

/* Not opaque: a diagonal dotted line over what is there. */
static void dots(const display_band_t *b)
{
    for (int y = b->ya; y <= b->yb; y++)
        for (int x = b->cx0; x <= b->cx1; x++)
            if ((x - b->x0) == (y - b->y0) * 3 % (b->x1 - b->x0 + 1))
                b->band[(y - b->band_y) * DISPLAY_W + x] = (uint16_t)(0x1234u + b->arg);
}

static const uint8_t k_icon[12 * 2] = {         /* 12 x 12: a frame with a cross */
    0xFF, 0xF0, 0x80, 0x10, 0xA0, 0x50, 0x90, 0x90, 0x89, 0x10, 0x86, 0x10,
    0x86, 0x10, 0x89, 0x10, 0x90, 0x90, 0xA0, 0x50, 0x80, 0x10, 0xFF, 0xF0,
};

static uint16_t snap_a[DISPLAY_H][DISPLAY_W], snap_b[DISPLAY_H][DISPLAY_W];

static void snapshot(uint16_t s[DISPLAY_H][DISPLAY_W])
{
    for (int y = 0; y < DISPLAY_H; y++)
        for (int x = 0; x < DISPLAY_W; x++)
            s[y][x] = panel_io_sim_pixel(x, y);
}

static int diff(uint16_t a[DISPLAY_H][DISPLAY_W], uint16_t b[DISPLAY_H][DISPLAY_W])
{
    int n = 0;

    for (int y = 0; y < DISPLAY_H; y++)
        for (int x = 0; x < DISPLAY_W; x++)
            n += a[y][x] != b[y][x];
    return n;
}

static unsigned s_rng = 12345u;
static int rnd(int n)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 8) % (unsigned)n);
}

/* A busy random screen: every op kind, overlapping, some off the edges. */
static void scene(unsigned seed)
{
    static char texts[40][24];
    unsigned keep = s_rng;

    s_rng = seed;
    display_clear((uint16_t)rnd(0x10000));
    for (int i = 0; i < 40; i++) {
        int x = rnd(DISPLAY_W + 40) - 20, y = rnd(DISPLAY_H + 20) - 10;
        int w = rnd(120) + 1, h = rnd(40) + 1;
        uint16_t c = (uint16_t)rnd(0x10000);

        switch (rnd(7)) {
        case 0:
            display_fill_rect(x, y, x + w, y + h, c);
            break;
        case 1:
            display_hline(x, x + w, y, c);
            break;
        case 2:
            snprintf(texts[i], sizeof texts[i], "t%d ab", i);
            display_draw_text(x, y, texts[i], c, (uint16_t)~c);
            break;
        case 3:
            snprintf(texts[i], sizeof texts[i], "Song %d - Artist", i);
            display_text(&k_font, x, y, x + w + 40, y + 21, x + 2, y + 16, texts[i], (int)strlen(texts[i]), c);
            break;
        case 4:
            display_icon(x, y, 12, 12, k_icon, c);
            break;
        case 5:
            display_custom(x, y, x + w, y + h, bar, (uint32_t)rnd(1001), 1);
            break;
        default:
            display_custom(x, y, x + w, y + h, dots, (uint32_t)rnd(100), 0);
            break;
        }
    }
    s_rng = keep;
}

static void test_ops(void)
{
    panel_io_sim_reset();
    display_clear(0x0000);
    display_fill_rect(10, 10, 19, 19, 0xF800);
    display_hline(0, 9, 30, 0x07E0);
    display_icon(40, 40, 12, 12, k_icon, 0xFFFF);
    display_custom(100, 100, 199, 109, bar, 500, 1);
    display_draw_text(200, 0, "Hi", 0xFFFF, 0x001F);
    display_flush_rows(0, DISPLAY_H - 1);
    CHECK(panel_io_sim_pixel(10, 10) == 0xF800);
    CHECK(panel_io_sim_pixel(19, 19) == 0xF800);
    CHECK(panel_io_sim_pixel(20, 20) == 0x0000);
    CHECK(panel_io_sim_pixel(0, 30) == 0x07E0 && panel_io_sim_pixel(9, 30) == 0x07E0);
    CHECK(panel_io_sim_pixel(10, 30) == 0x0000 && panel_io_sim_pixel(0, 31) == 0x0000);
    CHECK(panel_io_sim_pixel(40, 40) == 0xFFFF);       /* icon frame */
    CHECK(panel_io_sim_pixel(41, 41) == 0x0000);       /* transparent */
    CHECK(panel_io_sim_pixel(51, 51) == 0xFFFF);
    CHECK(panel_io_sim_pixel(52, 52) == 0x0000);       /* (12 x 12) */
    CHECK(panel_io_sim_pixel(149, 100) == 0xF800);     /* half the bar */
    CHECK(panel_io_sim_pixel(150, 100) != 0xF800);
    {                                                  /* the 'H' cell: only fg and bg, both */
        int fg = 0, bg = 0;

        for (int y = 0; y < 8; y++)
            for (int x = 200; x < 208; x++) {
                fg += panel_io_sim_pixel(x, y) == 0xFFFF;
                bg += panel_io_sim_pixel(x, y) == 0x001F;
            }
        CHECK(fg > 8 && bg > 8 && fg + bg == 64);
    }
    CHECK(panel_io_sim_overflow() == 0);

    /* text runs: the engine is called once per band the op touches, inside its clip box */
    s_text_calls = 0;
    display_clear(0x0000);
    display_text(&k_font, 0, 20, 99, 41, 5, 36, "abc", 3, 0xFFFF);   /* rows 20..41: bands 16-31, 32-47 */
    display_flush_rows(0, DISPLAY_H - 1);
    CHECK(s_text_calls == 2);
    s_text_calls = 0;
    display_flush_rect(200, 0, 300, DISPLAY_H - 1);                     /* outside its box */
    CHECK(s_text_calls == 0);

    /* an opaque op drops what it covers; a clear empties the list */
    display_clear(0);
    for (int i = 0; i < 50; i++)
        display_fill_rect(0, 0, 10, 10, (uint16_t)i);
    CHECK(display_op_count() == 2);
    display_icon(0, 0, 12, 12, k_icon, 1);
    display_icon(0, 0, 12, 12, k_icon, 2);                   /* not opaque: both stay */
    CHECK(display_op_count() == 4);
    display_clear(0);
    CHECK(display_op_count() == 1);
}

static void test_overflow(void)
{
    unsigned before = display_ops_dropped;

    display_clear(0);
    for (int i = 0; i < DISPLAY_OPS_MAX + 10; i++)
        display_icon(i % 300, i % 150, 12, 12, k_icon, 1);
    CHECK(display_op_count() == DISPLAY_OPS_MAX);
    CHECK(display_ops_dropped == before + 11);                /* the clear and 10 icons */
    /* text arena: long strings, many times over (compaction keeps the live ones) */
    display_clear(0);
    for (int i = 0; i < 400; i++) {
        char s[64];

        snprintf(s, sizeof s, "%03d the quick brown fox jumps over the lazy dog", i);
        display_draw_text(0, (i % 20) * 8, s, 0xFFFF, 0);   /* the same rows again and again */
    }
    CHECK(display_op_count() == 21);                          /* the clear + one per row */
    panel_io_sim_reset();
    display_flush_rows(0, DISPLAY_H - 1);
    snapshot(snap_a);
    display_clear(0);
    for (int i = 380; i < 400; i++) {
        char s[64];

        snprintf(s, sizeof s, "%03d the quick brown fox jumps over the lazy dog", i);
        display_draw_text(0, (i % 20) * 8, s, 0xFFFF, 0);
    }
    panel_io_sim_reset();
    display_flush_rows(0, DISPLAY_H - 1);
    snapshot(snap_b);
    CHECK(diff(snap_a, snap_b) == 0);
}

/* The same scene flushed by random rectangles is the full flush; a change flushed by any set of
 * rectangles covering it is the change flushed in full. */
static void test_partial_equals_full(void)
{
    for (unsigned s = 1; s <= 30; s++) {
        panel_io_sim_reset();
        scene(s);
        display_flush_rows(0, DISPLAY_H - 1);
        snapshot(snap_a);
        for (int k = 0; k < 20; k++) {
            int x0 = rnd(DISPLAY_W), y0 = rnd(DISPLAY_H);

            display_flush_rect(x0, y0, x0 + rnd(DISPLAY_W), y0 + rnd(60));
        }
        display_flush_rect(DISPLAY_W - 1, DISPLAY_H - 1, DISPLAY_W - 1, DISPLAY_H - 1);   /* 1 px */
        display_flush_rect(0, 0, 0, DISPLAY_H - 1);                                       /* 1 column */
        snapshot(snap_b);
        CHECK(diff(snap_a, snap_b) == 0);
        CHECK(panel_io_sim_overflow() == 0);

        /* a change in a region R, flushed as random pieces that together cover R */
        {
            int rx0 = rnd(250), ry0 = rnd(120), rx1 = rx0 + 12 + rnd(58), ry1 = ry0 + 12 + rnd(38);

            scene(s);
            display_fill_rect(rx0 + 2, ry0 + 2, rx1 - 2, ry1 - 2, (uint16_t)rnd(0x10000));
            display_custom(rx0, ry0, rx1, ry1, dots, 7, 0);
            display_icon(rx0, ry0, 12, 12, k_icon, 0xFFE0);
            display_text(&k_font, rx0, ry0, rx1, ry1, rx0 + 1, ry0 + 12, "xyz", 3, 0x07FF);
            for (int y = ry0; y <= ry1; ) {                   /* row strips, each split in two */
                int h = rnd(20) + 1, xm = rx0 + rnd(rx1 - rx0 + 1);

                display_flush_rect(rx0, y, xm, y + h - 1);
                display_flush_rect(xm + 1, y, rx1, y + h - 1);
                y += h;
            }
            snapshot(snap_b);
            panel_io_sim_reset();
            display_flush_rows(0, DISPLAY_H - 1);
            snapshot(snap_a);
            CHECK(diff(snap_a, snap_b) == 0);
        }
    }
}

static char s_line[160];
static int s_lines;
static void logger(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_line, sizeof s_line, fmt, ap);
    va_end(ap);
    s_lines++;
}

static void test_frame_log(void)
{
    display_set_logger(logger);
    scene(3);
    s_lines = 0;
    display_frame_begin();
    panel_io_sim_advance_us(5000);
    display_flush_rect(0, 0, 63, 19);
    display_frame_end();
    CHECK(s_lines == 0);                                      /* 5 ms: not slow */
    CHECK(display_stats()->last_px == 64u * 20u && display_stats()->last_rows == 20u);

    display_mark_input(panel_io_now_ms());
    display_frame_begin();
    panel_io_sim_advance_us(40000);
    display_flush_rows(0, 21);
    display_flush_rect(0, 100, 99, 109);
    display_frame_end();
    CHECK(s_lines == 2);                                      /* the frame, then key->px */
    CHECK(strcmp(s_line, "ui: key->px 40 ms px 8040") == 0);
    CHECK(display_stats()->last_px == 320u * 22u + 100u * 10u);
    CHECK(display_stats()->key_last_ms == 40u);

    s_lines = 0;                                              /* a frame that pushed nothing */
    display_mark_input(panel_io_now_ms());
    display_frame_begin();
    display_frame_end();
    CHECK(s_lines == 0);
    panel_io_sim_advance_us(1000);
    display_flush_rows(0, 0);                                 /* a flush outside a frame */
    CHECK(s_lines == 1 && strcmp(s_line, "ui: key->px 1 ms px 320") == 0);

    s_lines = 0;                     /* the clock steps back 7 ms mid-frame (device 2026-10-18, the boot's
                                      * splash: "ui: frame 4294960 ms rows 33 px 10560"): no slow frame */
    display_frame_begin();
    panel_io_sim_advance_us((uint32_t)-7000);
    display_flush_rows(0, 32);
    display_frame_end();
    CHECK(s_lines == 0);
    CHECK(display_stats()->last_us == 0u);
    panel_io_sim_advance_us(7000);
    display_set_logger(NULL);
}

int main(void)
{
    display_init();
    test_ops();
    test_overflow();
    test_partial_equals_full();
    test_frame_log();
    TEST_END("test_display_list");
}
