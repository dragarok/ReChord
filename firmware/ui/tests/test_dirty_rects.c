/*
 * test_dirty_rects.c -- ui.c's dirty rectangles: merging by cost, the flush order, the column
 * windows display_flush_rect() gets, one frame per ui_render(). Standalone (its own display
 * stand-ins), so it runs while the rest of ui/tests waits for its repair:
 *   cc -std=c99 -Wall -Werror -I firmware/ui -I firmware -I firmware/drivers \
 *      firmware/ui/tests/test_dirty_rects.c firmware/ui/ui.c
 */
#include <stdio.h>

#include "ui.h"

static int checks, fails;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static struct { int x0, y0, x1, y1; } s_fl[32];
static int s_nfl, s_frames, s_in, s_draws;

void display_flush_rect(int x0, int y0, int x1, int y1)
{
    CHECK(s_in);                                       /* inside the frame */
    if (s_nfl < 32) {
        s_fl[s_nfl].x0 = x0; s_fl[s_nfl].y0 = y0; s_fl[s_nfl].x1 = x1; s_fl[s_nfl].y1 = y1;
    }
    s_nfl++;
}
void display_flush_rows(int y0, int y1) { display_flush_rect(0, y0, DISPLAY_W - 1, y1); }
void display_frame_begin(void) { s_in = 1; }
void display_frame_end(void) { s_in = 0; s_frames++; }
/* (ui.c's event loop skeleton) */
int input_poll(input_event_t *ev) { (void)ev; return 0; }

static void draw(void) { s_draws++; }
static const ui_screen_t k_screen = { "t", NULL, NULL, draw, NULL };

static long area_sum(void)
{
    long a = 0;
    for (int i = 0; i < ui_dirty_band_count(); i++) {
        int x0, y0, x1, y1;

        ui_dirty_rect(i, &x0, &y0, &x1, &y1);
        a += (long)(x1 - x0 + 1) * (y1 - y0 + 1);
    }
    return a;
}

/* Every pixel marked dirty is inside some rectangle. */
static int covers(int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int in = 0;

            for (int i = 0; i < ui_dirty_band_count() && !in; i++) {
                int a, b, c, d;

                ui_dirty_rect(i, &a, &b, &c, &d);
                in = x >= a && x <= c && y >= b && y <= d;
            }
            if (!in)
                return 0;
        }
    return 1;
}

int main(void)
{
    int x0, y0, x1, y1;

    ui_init();
    ui_push_screen(&k_screen);
    CHECK(ui_dirty_band_count() == 1);
    ui_render();
    CHECK(s_draws == 1 && s_frames == 1 && s_nfl == 1);
    CHECK(s_fl[0].x0 == 0 && s_fl[0].x1 == DISPLAY_W - 1 && s_fl[0].y0 == 0 && s_fl[0].y1 == DISPLAY_H - 1);

    /* a key-bar slot and a header field far apart: two windows, their own columns */
    s_nfl = 0;
    ui_invalidate(64, 0, 127, 19);
    ui_invalidate(250, 20, 312, 37);
    CHECK(ui_dirty_band_count() == 2);
    ui_render();
    CHECK(s_nfl == 2 && s_frames == 2);
    CHECK(s_fl[0].x0 == 64 && s_fl[0].x1 == 127 && s_fl[0].y0 == 0 && s_fl[0].y1 == 19);
    CHECK(s_fl[1].x0 == 250 && s_fl[1].x1 == 312 && s_fl[1].y0 == 20 && s_fl[1].y1 == 37);

    /* touching, overlapping and nested rectangles merge into their box */
    ui_invalidate(0, 38, DISPLAY_W - 1, 59);
    ui_invalidate(0, 60, DISPLAY_W - 1, 81);           /* the next list row */
    CHECK(ui_dirty_band_count() == 1);
    ui_invalidate(10, 40, 20, 50);                     /* inside */
    CHECK(ui_dirty_band_count() == 1);
    ui_dirty_rect(0, &x0, &y0, &x1, &y1);
    CHECK(x0 == 0 && y0 == 38 && x1 == DISPLAY_W - 1 && y1 == 81);
    ui_render();

    /* two pieces of one row a few pixels apart merge (a window costs more than the gap) */
    ui_invalidate(100, 100, 140, 110);
    ui_invalidate(143, 100, 180, 110);
    CHECK(ui_dirty_band_count() == 1);
    ui_render();

    /* but full-width rows 1 row apart stay apart (320 px of gap > a window) */
    ui_invalidate(0, 10, DISPLAY_W - 1, 20);
    ui_invalidate(0, 22, DISPLAY_W - 1, 30);
    CHECK(ui_dirty_band_count() == 2);
    ui_render();

    /* clipping */
    ui_invalidate(-5, -5, 3, 3);
    ui_invalidate(DISPLAY_W - 2, DISPLAY_H - 2, DISPLAY_W + 9, DISPLAY_H + 99);
    ui_invalidate(400, 10, 500, 20);                   /* off screen: nothing */
    CHECK(ui_dirty_band_count() == 2);
    ui_dirty_rect(1, &x0, &y0, &x1, &y1);
    CHECK(x0 == DISPLAY_W - 2 && x1 == DISPLAY_W - 1 && y1 == DISPLAY_H - 1);
    s_nfl = 0;
    ui_render();
    CHECK(s_nfl == 2 && s_fl[0].y0 == 0 && s_fl[1].y0 == DISPLAY_H - 2);   /* top first */

    /* overflow: 40 scattered small rectangles, never more than 8, everything still covered, and
     * the cheapest merges taken (far less than the whole screen) */
    {
        unsigned r = 7;
        int xs[40], ys[40];

        for (int i = 0; i < 40; i++) {
            r = r * 1103515245u + 12345u;
            xs[i] = (int)((r >> 8) % 300);
            r = r * 1103515245u + 12345u;
            ys[i] = (int)((r >> 8) % 160);
            ui_invalidate(xs[i], ys[i], xs[i] + 9, ys[i] + 5);
            CHECK(ui_dirty_band_count() <= 8);
        }
        for (int i = 0; i < 40; i++)
            CHECK(covers(xs[i], ys[i], xs[i] + 9, ys[i] + 5));
        CHECK(area_sum() < (long)DISPLAY_W * DISPLAY_H);
        s_nfl = 0;
        ui_render();
        CHECK(s_nfl >= 1 && s_nfl <= 8);
        for (int i = 1; i < s_nfl; i++)
            CHECK(s_fl[i].y0 >= s_fl[i - 1].y0);
    }
    printf("test_dirty_rects: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
