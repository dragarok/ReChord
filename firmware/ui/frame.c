/*
 * frame.c -- the key bar, the header, toasts and the drawing helpers (frame.h). Geometry and
 * colours are DESIGN.md 2.1 and tools/ui_mockups.py's header() / keybar() (project repo), with
 * the key bar on the panel's top rows.
 */
#include <stdio.h>
#include <string.h>

#include "frame.h"
#include "ui.h"
#include "services/input.h"

/* ---- drawing helpers ---------------------------------------------------------------------------- */

static void icon_fn(const display_band_t *b)
{
    const icon_t *ic = &icon_table[b->arg & 0xFFu];
    uint16_t c = (uint16_t)(b->arg >> 16);

    for (int y = b->ya; y <= b->yb; y++) {
        unsigned bits = ic->rows[y - b->y0];
        uint16_t *row = b->band + (y - b->band_y) * DISPLAY_W;

        for (int x = b->cx0; x <= b->cx1; x++)
            if (bits & (0x8000u >> (x - b->x0)))
                row[x] = c;
    }
}

void ui_icon(int id, int x, int y, uint16_t c)
{
    if (id >= 0 && id < ICON_COUNT)
        display_custom(x, y, x + icon_table[id].w - 1, y + ICON_H - 1, icon_fn, (uint32_t)id | (uint32_t)c << 16, 0);
}

int ui_text(const font_t *f, int x, int base, int max_w, int align, const char *s, uint16_t c, int y0, int y1)
{
    char buf[200];
    int n, w, xa, a, b;

    if (!s || !*s || max_w <= 0)
        return 0;
    n = text_fit(f, s, -1, max_w, buf, sizeof buf);
    if (n <= 0)
        return 0;
    w = text_width(f, buf, n);
    xa = align == 1 ? x + (max_w - w) / 2 : align == 2 ? x + max_w - w : x;
    a = xa - 1 > x ? xa - 1 : x;                         /* the box: the run, 1 px each side, in the field */
    b = xa + w < x + max_w - 1 ? xa + w : x + max_w - 1;
    display_text(f, a, y0, b, y1, xa, base, buf, n, c);
    return w;
}

void ui_fill(int x, int y, int w, int h, uint16_t c)
{
    if (w > 0 && h > 0)
        display_fill_rect(x, y, x + w - 1, y + h - 1, c);
}

void ui_frame_rect(int x, int y, int w, int h, uint16_t c)
{
    ui_fill(x, y, w, 1, c);
    ui_fill(x, y + h - 1, w, 1, c);
    ui_fill(x, y + 1, 1, h - 2, c);
    ui_fill(x + w - 1, y + 1, 1, h - 2, c);
}

uint16_t ui_mix(uint16_t a, uint16_t b, int t)
{
    int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;

    return (uint16_t)(r << 11 | g << 5 | bl);
}

/* ---- the key bar -------------------------------------------------------------------------------- */

static keybar_t s_kb;                     /* the screen's labels */
static const keybar_t *s_layer;           /* the shortcut layer, NULL: off */
static int s_pressed = KEY_NONE, s_prog;
static struct { uint8_t icon, tone, on; uint32_t until; char text[64]; } s_toast;

int key_slot(int key)
{
    switch (key) {
    case KEY_UP:     return SLOT_VOLP;
    case KEY_DOWN:   return SLOT_VOLM;
    case KEY_SELECT: return SLOT_PLAY;
    case KEY_MENU:   return SLOT_MENU;
    case KEY_BACK:   return SLOT_BACK;
    default:         return -1;
    }
}

static int same(const char *a, const char *b)
{
    return (!a || !*a) ? (!b || !*b) : (b && !strcmp(a, b));
}

static void inval_slot(int i)
{
    if (i >= 0)
        ui_invalidate(i * SLOT_W, 0, i * SLOT_W + SLOT_W - 1, KB_H - 2);
}

static void inval_bar(void)
{
    ui_invalidate(0, 0, DISPLAY_W - 1, KB_H - 1);
}

void frame_keys(const keybar_t *k)
{
    for (int i = 0; i < NSLOTS; i++)
        if (!same(k->tap[i], s_kb.tap[i]) || !same(k->hold[i], s_kb.hold[i])) {
            s_kb.tap[i] = k->tap[i];
            s_kb.hold[i] = k->hold[i];
            if (!s_layer && !s_toast.on)
                inval_slot(i);
        }
}

static int has_hold(int slot)
{
    const keybar_t *k = s_layer ? s_layer : &s_kb;

    return slot >= 0 && k->hold[slot] && *k->hold[slot];
}

void frame_press(int key)
{
    if (key == s_pressed)
        return;
    if (!s_toast.on) {
        inval_slot(key_slot(s_pressed));
        inval_slot(key_slot(key));
    }
    s_pressed = key_slot(key) >= 0 ? key : KEY_NONE;
    s_prog = 0;
}

int frame_pressed(void)
{
    return s_pressed;
}

void frame_layer(const keybar_t *k)
{
    if (k == s_layer)
        return;
    s_layer = k;
    inval_bar();
}

void frame_toast(int icon, int tone, const char *text)
{
    s_toast.icon = (uint8_t)icon;
    s_toast.tone = (uint8_t)tone;
    snprintf(s_toast.text, sizeof s_toast.text, "%s", text);
    s_toast.until = time_ms() + 1500u;
    s_toast.on = 1;
    inval_bar();
}

void frame_tick(uint32_t now)
{
    if (s_toast.on && (int32_t)(now - s_toast.until) >= 0) {
        s_toast.on = 0;
        inval_bar();
    }
    if (s_pressed != KEY_NONE && has_hold(key_slot(s_pressed))) {
        int p = input_hold_progress(s_pressed);

        if (p != s_prog) {
            s_prog = p;
            if (!s_toast.on)
                inval_slot(key_slot(s_pressed));
        }
    }
}

static void draw_keybar(void)
{
    const theme_t *t = T;
    const keybar_t *k = s_layer ? s_layer : &s_kb;
    uint16_t kt = t->key_text, kh = t->key_hold, dv = t->divider;

    ui_fill(0, 0, DISPLAY_W, KB_H - 1, t->keybar_bg);
    ui_fill(0, KB_H - 1, DISPLAY_W, 1, t->divider);
    if (s_toast.on) {
        uint16_t c = s_toast.tone == TOAST_WARN ? t->warn : s_toast.tone == TOAST_DANGER ? t->danger : t->text;
        int tx = 8;

        if (s_toast.icon != NO_ICON) {
            ui_icon(s_toast.icon, 8, 4, s_toast.tone == TOAST_TEXT ? t->accent : c);
            tx = 26;
        }
        ui_text(&font_b13, tx, 15, DISPLAY_W - tx - 8, 0, s_toast.text, c, 0, KB_H - 2);
        return;
    }
    if (s_layer) {                         /* the shortcut layer: the bar inverts */
        ui_fill(0, 0, DISPLAY_W, KB_H - 1, t->sel_bg);
        kt = kh = dv = t->sel_text;
    }
    for (int i = 0; i < NSLOTS; i++) {
        int x0 = i * SLOT_W, hold = k->hold[i] && *k->hold[i], pressed = key_slot(s_pressed) == i;
        const char *tap = k->tap[i] ? k->tap[i] : "";

        if (i)
            ui_fill(x0, 3, 1, 13, dv);
        if (pressed) {
            int fh = hold ? (19 * s_prog + 5) / 10 : 19;

            ui_fill(x0 + 1, 0, SLOT_W - 1, 19, t->surface);
            ui_fill(x0 + 1, 0, SLOT_W - 1, fh < 2 ? 2 : fh, t->key_press);
        }
        if (hold) {
            ui_text(&font_s8, x0 + 2, 9, SLOT_W - 4, 1, tap, *tap ? kt : kh, 0, KB_H - 2);
            ui_text(&font_s8, x0 + 2, 17, SLOT_W - 4, 1, k->hold[i], pressed ? kt : kh, 0, KB_H - 2);
        } else {
            ui_text(&font_s8, x0 + 2, 13, SLOT_W - 4, 1, tap, kt, 0, KB_H - 2);
        }
    }
}

/* ---- the header --------------------------------------------------------------------------------- */

enum { FD_TITLE, FD_PLAY, FD_LOCK, FD_OUT, FD_VOL, FD_BATT, NFD };
typedef struct { int16_t x0, x1; uint32_t sig; } fld_t;

static hdr_t s_h;

static uint32_t hs(uint32_t h, const char *s)
{
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void battery(int x, int y, int pct, int charging)
{
    const theme_t *t = T;
    uint16_t c = pct <= 10 && !charging ? t->danger : t->text_dim;
    int w = (13 * pct + 50) / 100;

    ui_frame_rect(x, y, 17, 9, c);
    ui_fill(x + 17, y + 3, 2, 3, c);
    ui_fill(x + 2, y + 2, w < 1 ? 1 : w, 5, charging ? t->success : c);
}

#define HB 33                              /* the header's text baseline */
#define HY0 HDR_Y
#define HY1 (HDR_Y + 16)

/* Lay the header out right to left (and draw it when draw): each field's span and signature. */
static void hdr_pass(const hdr_t *h, fld_t *f, int draw)
{
    const theme_t *t = T;
    char pct[8];
    int x = 312 - 19, w, tx;

    memset(f, 0, NFD * sizeof *f);
    for (int i = 0; i < NFD; i++)
        f[i].x0 = 1, f[i].x1 = 0;          /* (empty) */
    snprintf(pct, sizeof pct, "%u%%", h->batt);
    w = text_width(&font_s8, pct, -1);
    f[FD_BATT].x0 = (int16_t)(x - w - 3);
    f[FD_BATT].x1 = 312;
    f[FD_BATT].sig = h->batt | (uint32_t)h->charging << 8 | 1u << 16;
    if (draw) {
        battery(x, 24, h->batt, h->charging);
        ui_text(&font_s8, x - w - 3, 32, w, 0, pct, t->text_dim, HY0, HY1);
    }
    x -= w + 3;
    w = text_width(&font_b13, h->vol, -1);
    x -= w + 8;
    f[FD_VOL].x0 = (int16_t)x;
    f[FD_VOL].x1 = (int16_t)(x + w - 1);
    f[FD_VOL].sig = hs(2166136261u, h->vol);
    if (draw)
        ui_text(&font_b13, x, HB, w, 0, h->vol, t->text, HY0, HY1);
    x -= 13;
    f[FD_OUT].x0 = (int16_t)x;
    f[FD_OUT].x1 = (int16_t)(x + 11);
    f[FD_OUT].sig = h->out | 1u << 8;
    if (draw)
        ui_icon(h->out, x, 23, h->out != ICON_SPEAKER ? t->accent : t->text_dim);
    if (h->lock) {
        x -= 15;
        f[FD_LOCK].x0 = (int16_t)x;
        f[FD_LOCK].x1 = (int16_t)(x + 11);
        f[FD_LOCK].sig = 1;
        if (draw)
            ui_icon(ICON_LOCK, x, 23, t->warn);
    }
    if (!h->np && h->play) {
        w = text_width(&font_b13, h->elapsed, -1);
        x -= w + 8;
        if (draw)
            ui_text(&font_b13, x, HB, w, 0, h->elapsed, t->text_dim, HY0, HY1);
        x -= 13;
        f[FD_PLAY].x0 = (int16_t)x;
        f[FD_PLAY].x1 = (int16_t)(x + 13 + w);
        f[FD_PLAY].sig = hs(h->play, h->elapsed);
        if (draw)
            ui_icon(h->play == 1 ? ICON_PLAY : ICON_PAUSE, x, 23, t->text_dim);
    }
    x -= 6;                                /* the title's right edge */
    f[FD_TITLE].x0 = 0;
    f[FD_TITLE].x1 = (int16_t)x;
    f[FD_TITLE].sig = hs(h->icon | (uint32_t)h->np << 8 | (uint32_t)h->mode_icon << 16 | (uint32_t)x << 24, h->title);
    if (!draw)
        return;
    tx = 5;
    if (h->np) {
        ui_icon(ICON_NOTE, 5, 23, t->text_dim);
        tx = 22 + ui_text(&font_m13, 22, HB, x - 22, 0, h->title, t->text, HY0, HY1) + 6;
        if (h->mode_icon != NO_ICON)
            ui_icon(h->mode_icon, tx, 23, t->accent);
        return;
    }
    if (h->icon != NO_ICON) {
        ui_icon(h->icon, 5, 23, t->text_dim);
        tx = 22;
    }
    ui_text(&font_m13, tx, HB, x - tx, 0, h->title, t->text, HY0, HY1);
}

void frame_status(const hdr_t *h)
{
    fld_t a[NFD], b[NFD];

    if (!memcmp(h, &s_h, sizeof *h))
        return;
    hdr_pass(&s_h, a, 0);
    hdr_pass(h, b, 0);
    for (int i = 0; i < NFD; i++) {
        int x0, x1;

        if (a[i].x0 == b[i].x0 && a[i].x1 == b[i].x1 && a[i].sig == b[i].sig)
            continue;
        x0 = a[i].x0 <= a[i].x1 ? a[i].x0 : b[i].x0;
        x1 = a[i].x0 <= a[i].x1 ? a[i].x1 : b[i].x1;
        if (b[i].x0 <= b[i].x1) {
            x0 = b[i].x0 < x0 ? b[i].x0 : x0;
            x1 = b[i].x1 > x1 ? b[i].x1 : x1;
        }
        if (x0 <= x1)
            ui_invalidate(x0 - 1, HY0, x1 + 1, HY1);
    }
    s_h = *h;
}

void frame_draw(void)
{
    const theme_t *t = T;
    fld_t f[NFD];

    draw_keybar();
    ui_fill(0, HDR_Y, DISPLAY_W, 17, t->header_bg);
    ui_fill(0, HDR_Y + 17, DISPLAY_W, 1, t->divider);
    hdr_pass(&s_h, f, 1);
}

void frame_invalidate(void)
{
    ui_invalidate(0, 0, DISPLAY_W - 1, BODY_Y - 1);
}
