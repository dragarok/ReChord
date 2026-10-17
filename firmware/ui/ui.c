/*
 * ui.c — ReChord UI core implementation (see ui.h for the design).
 *
 * Dirty-region model: up to 8 inclusive rectangles. Two merge when their bounding box costs no
 * more pixels than both apart plus one flush window (overlapping, touching, nested or nearly
 * so); a full set merges its cheapest pair. Each rectangle is flushed on its own, top to bottom,
 * with a column window (display_flush_rect), so a key-bar slot or a header field pushes only
 * its own pixels.
 */
#include <stddef.h>
#include "ui.h"
#include "theme.h"

#define UI_DIRTY_MAX 8
/* What one more flush window costs, in pixels pushed: CASET/RASET/RAMWR per band and the band's
 * render setup. Two rectangles merge when their bounding box costs no more than both apart. */
#define UI_WINDOW_COST 256

typedef struct dirty_rect {
    int x0, y0, x1, y1;   /* inclusive */
} dirty_rect_t;

static const ui_screen_t *s_stack[UI_STACK_MAX];
static int                s_depth;
static int                s_running;
static dirty_rect_t       s_dirty[UI_DIRTY_MAX];
static int                s_dirty_count;

/* ---- dirty rectangles -------------------------------------------------- */

static long area(const dirty_rect_t *r)
{
    return (long)(r->x1 - r->x0 + 1) * (long)(r->y1 - r->y0 + 1);
}

static dirty_rect_t bound(const dirty_rect_t *a, const dirty_rect_t *b)
{
    dirty_rect_t r;

    r.x0 = a->x0 < b->x0 ? a->x0 : b->x0;
    r.y0 = a->y0 < b->y0 ? a->y0 : b->y0;
    r.x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    r.y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    return r;
}

/* Pixels a merge would push beyond the two apart, less the window it saves. */
static long merge_cost(const dirty_rect_t *a, const dirty_rect_t *b)
{
    dirty_rect_t u = bound(a, b);

    return area(&u) - area(a) - area(b) - UI_WINDOW_COST;
}

static void dirty_remove(int i)
{
    for (; i + 1 < s_dirty_count; i++)
        s_dirty[i] = s_dirty[i + 1];
    s_dirty_count--;
}

/* Merge every pair that is cheaper merged (overlapping, touching, nested or close), until none. */
static void dirty_normalize(void)
{
    int i, j, again = 1;

    while (again) {
        again = 0;
        for (i = 0; i < s_dirty_count && !again; i++)
            for (j = i + 1; j < s_dirty_count && !again; j++)
                if (merge_cost(&s_dirty[i], &s_dirty[j]) <= 0) {
                    s_dirty[i] = bound(&s_dirty[i], &s_dirty[j]);
                    dirty_remove(j);
                    again = 1;
                }
    }
    /* flush order: top to bottom, then left to right (the eye sees the change start at once) */
    for (i = 1; i < s_dirty_count; i++) {
        dirty_rect_t r = s_dirty[i];

        for (j = i - 1; j >= 0 && (s_dirty[j].y0 > r.y0 || (s_dirty[j].y0 == r.y0 && s_dirty[j].x0 > r.x0)); j--)
            s_dirty[j + 1] = s_dirty[j];
        s_dirty[j + 1] = r;
    }
}

void ui_invalidate(int x0, int y0, int x1, int y1)
{
    dirty_rect_t r;

    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > DISPLAY_W - 1)
        x1 = DISPLAY_W - 1;
    if (y1 > DISPLAY_H - 1)
        y1 = DISPLAY_H - 1;
    if (x0 > x1 || y0 > y1)
        return;
    r.x0 = x0; r.y0 = y0; r.x1 = x1; r.y1 = y1;
    if (s_dirty_count == UI_DIRTY_MAX) {
        /* full: merge the pair that costs least merged (the new one included) */
        int bi = 0, bj = 1, i, j;
        long best = 0x7FFFFFFFL;

        for (i = 0; i < s_dirty_count; i++) {
            long c = merge_cost(&s_dirty[i], &r);

            if (c < best) {
                best = c;
                bi = i;
                bj = -1;
            }
            for (j = i + 1; j < s_dirty_count; j++) {
                c = merge_cost(&s_dirty[i], &s_dirty[j]);
                if (c < best) {
                    best = c;
                    bi = i;
                    bj = j;
                }
            }
        }
        if (bj < 0) {
            s_dirty[bi] = bound(&s_dirty[bi], &r);
        } else {
            s_dirty[bi] = bound(&s_dirty[bi], &s_dirty[bj]);
            s_dirty[bj] = r;
        }
    } else {
        s_dirty[s_dirty_count++] = r;
    }
    dirty_normalize();
}

void ui_invalidate_all(void)
{
    ui_invalidate(0, 0, DISPLAY_W - 1, DISPLAY_H - 1);
}

bool ui_is_dirty(void)
{
    return s_dirty_count > 0;
}

int ui_dirty_band_count(void)
{
    return s_dirty_count;
}

bool ui_dirty_band(int idx, int *y0, int *y1)
{
    return ui_dirty_rect(idx, NULL, y0, NULL, y1);
}

bool ui_dirty_rect(int idx, int *x0, int *y0, int *x1, int *y1)
{
    if (idx < 0 || idx >= s_dirty_count)
        return false;
    if (x0)
        *x0 = s_dirty[idx].x0;
    if (y0)
        *y0 = s_dirty[idx].y0;
    if (x1)
        *x1 = s_dirty[idx].x1;
    if (y1)
        *y1 = s_dirty[idx].y1;
    return true;
}

/* ---- screen stack ----------------------------------------------------- */

bool ui_push_screen(const ui_screen_t *screen)
{
    if (screen == NULL || s_depth >= UI_STACK_MAX)
        return false;

    s_stack[s_depth++] = screen;
    if (screen->on_enter)
        screen->on_enter();
    /* the new screen owns the whole panel */
    ui_invalidate_all();
    return true;
}

bool ui_pop_screen(void)
{
    const ui_screen_t *leaving;
    const ui_screen_t *below;

    if (s_depth == 0)
        return false;

    leaving = s_stack[--s_depth];
    if (leaving->on_exit)
        leaving->on_exit();

    below = ui_top_screen();
    if (below && below->on_enter)
        below->on_enter();   /* re-enter the screen that was covered */

    ui_invalidate_all();
    return true;
}

const ui_screen_t *ui_top_screen(void)
{
    return (s_depth > 0) ? s_stack[s_depth - 1] : NULL;
}

int ui_stack_depth(void)
{
    return s_depth;
}

/* ---- event dispatch --------------------------------------------------- */

void ui_handle_event(const input_event_t *ev)
{
    const ui_screen_t *top = ui_top_screen();

    if (ev == NULL || top == NULL || top->handle_event == NULL)
        return;
    top->handle_event(ev);
}

/* ---- render ----------------------------------------------------------- */

void ui_render(void)
{
    const ui_screen_t *top = ui_top_screen();
    int i;

    if (top == NULL || s_dirty_count == 0)
        return;
    display_frame_begin();
    if (top->draw)
        top->draw();

    for (i = 0; i < s_dirty_count; i++)
        display_flush_rect(s_dirty[i].x0, s_dirty[i].y0, s_dirty[i].x1, s_dirty[i].y1);
    s_dirty_count = 0;
    display_frame_end();
}

/* ---- event loop skeleton ---------------------------------------------- */

void ui_quit(void)
{
    s_running = 0;
}

void ui_run(void)
{
    input_event_t ev;

    s_running = 1;
    while (s_running) {
        if (input_poll(&ev))
            ui_handle_event(&ev);
        ui_render();
    }
}

void ui_init(void)
{
    s_depth = 0;
    s_running = 0;
    s_dirty_count = 0;
}
