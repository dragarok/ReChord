/*
 * nav.c -- the page stack, the key dispatch with the global keys (DESIGN.md 3.1) and the list
 * template (2.2 L, the question's choices).
 *
 * Every pass the top page's model is compared with what is on the panel and only what changed is
 * invalidated (a list row, a key-bar slot, a header field); ui_render() then rebuilds the display
 * list from the model and pushes just those rectangles, so a partial repaint always equals a full
 * one (tests/cfw/m31_ui.sh checks it after every key).
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "nav.h"
#include "ui.h"
#include "drivers/clock.h"
#include "services/config.h"
#include "services/log.h"
#include "services/power.h"
#include "services/screenshot.h"
#include "player.h"

const input_event_t *g_ev;
int g_locked;

/* ---- the stack --------------------------------------------------------------------------------- */

#define NAV_MAX 16
typedef struct { const page_t *p; int16_t cur, top, arg; } ent_t;
static ent_t s_nav[NAV_MAX];
static int s_n;

typedef struct { uint16_t id, num; uint8_t icon, flags; char t[160]; char v[48]; } rc_t;
static struct {                           /* the visible rows, fitted */
    int n, y0, nr;
    rc_t c[NROWS];
} L;

static ent_t *top(void) { return s_n ? &s_nav[s_n - 1] : NULL; }
const page_t *nav_top(void) { return s_n ? s_nav[s_n - 1].p : NULL; }
const page_t *nav_below(void) { return s_n > 1 ? s_nav[s_n - 2].p : NULL; }
int nav_depth(void) { return s_n; }
int nav_arg(void) { return s_n ? s_nav[s_n - 1].arg : 0; }
int nav_cursor(void) { return s_n ? s_nav[s_n - 1].cur : 0; }

static int is_list(const page_t *p) { return p && p->kind != PK_CUSTOM; }

static void entered(int first)
{
    const page_t *p = nav_top();

    if (is_list(p)) {
        L.y0 = p->kind == PK_QUESTION ? 82 : BODY_Y;
        L.nr = p->kind == PK_QUESTION ? 4 : NROWS;
    }
    if (p && p->enter)
        p->enter(first);
    if (is_list(nav_top()) && nav_top() == p)
        list_reload();
    ui_invalidate_all();
    nav_keys_changed();
}

void nav_reset(const page_t *root)
{
    s_n = 0;
    nav_push(root, 0);
}

void nav_push(const page_t *p, int arg)
{
    if (s_n == NAV_MAX) {                 /* (deeper than any tree here: keep the newest levels) */
        memmove(s_nav + 1, s_nav + 2, (NAV_MAX - 2) * sizeof *s_nav);
        s_n--;
    }
    s_nav[s_n].p = p;
    s_nav[s_n].cur = s_nav[s_n].top = 0;
    s_nav[s_n].arg = (int16_t)arg;
    s_n++;
    log_printf("ui: open %s", p->name);
    entered(1);
}

void nav_pop(void)
{
    const page_t *p = nav_top();

    if (s_n <= 1)
        return;
    s_n--;
    if (p->leave)
        p->leave();
    entered(0);
}

void nav_pop_to(int depth)
{
    while (s_n > depth && s_n > 1) {
        const page_t *p = s_nav[--s_n].p;

        if (p->leave)
            p->leave();
    }
    entered(0);
}

void nav_home(void)
{
    nav_pop_to(1);
}

int nav_find(const page_t *p)
{
    for (int i = s_n - 1; i >= 0; i--)
        if (s_nav[i].p == p)
            return i;
    return -1;
}

/* ---- lists ------------------------------------------------------------------------------------- */

int list_count(void)
{
    return L.n;
}

/* Row i into cache slot s, fitted: the text before the value, the value at most 150 px. */
static int fill(int s, int i)
{
    static row_t r;                       /* (210 B: not on the stack) */
    char t[160], v[48];
    int right, vw = 0, ch;

    memset(&r, 0, sizeof r);
    r.icon = NO_ICON;
    nav_top()->row(i, &r);
    right = r.flags & R_CHEV ? 300 : 310;
    v[0] = 0;
    if (r.value[0]) {
        text_fit(&font_b13, r.value, -1, 150, v, sizeof v);
        vw = text_width(&font_b13, v, -1) + 8;
    }
    text_fit(r.flags & R_BOLD ? &font_m13 : &font_b13, r.text, -1, right - vw - 24, t, sizeof t);
    ch = L.c[s].num != r.num || L.c[s].icon != r.icon || L.c[s].flags != r.flags || strcmp(L.c[s].t, t) ||
         strcmp(L.c[s].v, v);
    L.c[s].id = r.id;
    L.c[s].num = r.num;
    L.c[s].icon = r.icon;
    L.c[s].flags = r.flags;
    memcpy(L.c[s].t, t, sizeof t);
    memcpy(L.c[s].v, v, sizeof v);
    return ch;
}

static void inval_slot(int s)
{
    if (s >= 0 && s < L.nr)
        ui_invalidate(0, L.y0 + s * ROW_H, 315, L.y0 + s * ROW_H + ROW_H - 1);
}

static void inval_list(void)
{
    ui_invalidate(0, L.y0, DISPLAY_W - 1, L.y0 + L.nr * ROW_H - 1);
}

static void clamp_view(ent_t *e)
{
    if (e->cur >= L.n)
        e->cur = (int16_t)(L.n ? L.n - 1 : 0);
    if (e->cur < 0)
        e->cur = 0;
    if (e->top > e->cur)
        e->top = e->cur;
    if (e->cur >= e->top + L.nr)
        e->top = (int16_t)(e->cur - L.nr + 1);
    if (e->top > L.n - L.nr)
        e->top = (int16_t)(L.n - L.nr > 0 ? L.n - L.nr : 0);
}

void list_reload(void)
{
    ent_t *e = top();
    const page_t *p = nav_top();

    if (!is_list(p))
        return;
    L.n = p->count ? p->count() : 0;
    clamp_view(e);
    for (int s = 0; s < L.nr && e->top + s < L.n; s++)
        fill(s, e->top + s);
    inval_list();
    nav_keys_changed();
}

void list_row_changed(int i)
{
    ent_t *e = top();

    if (is_list(nav_top()) && i >= e->top && i < e->top + L.nr && i < L.n && fill(i - e->top, i))
        inval_slot(i - e->top);
}

void nav_set_cursor(int i)
{
    ent_t *e = top();

    if (!is_list(nav_top())) {
        if (e)
            e->cur = (int16_t)i;
        return;
    }
    L.n = nav_top()->count ? nav_top()->count() : 0;
    e->cur = (int16_t)i;
    if (i < e->top || i >= e->top + L.nr)
        e->top = (int16_t)(i - 1 > 0 ? i - 1 : 0);
    list_reload();
}

void list_move(int d, int wrap)
{
    ent_t *e = top();
    int c, old = e->cur, otop = e->top;

    if (L.n <= 0)
        return;
    c = e->cur + d;
    if (c < 0)
        c = wrap && e->cur == 0 ? L.n - 1 : 0;
    else if (c >= L.n)
        c = wrap && e->cur == L.n - 1 ? 0 : L.n - 1;
    if (c == old)
        return;
    e->cur = (int16_t)c;
    clamp_view(e);
    if (e->top != otop) {
        for (int s = 0; s < L.nr && e->top + s < L.n; s++)
            fill(s, e->top + s);
        inval_list();
    } else {
        inval_slot(old - e->top);
        inval_slot(c - e->top);
    }
    nav_keys_changed();
}

static void draw_row(int s, int y, int sel)
{
    const theme_t *t = T;
    const rc_t *c = &L.c[s];
    int off = c->flags & R_OFF, right = 310, vw = 0;
    uint16_t fg = sel ? t->sel_text : off ? t->text_off : t->text;
    uint16_t dim = sel ? t->sel_text : t->text_dim;
    uint16_t ic = sel ? t->sel_text : c->flags & R_PLAYING ? t->accent : off ? t->text_off : t->text_dim;

    ui_fill(0, y, 316, ROW_H, sel ? t->sel_bg : t->background);
    if (c->flags & R_PLAYING) {
        ui_icon(ICON_SPEAKER, 6, y + 5, ic);
    } else if (c->flags & R_RADIO) {
        ui_icon(c->flags & R_ON ? ICON_RADIO_ON : ICON_RADIO_OFF, 6, y + 5,
                sel ? t->sel_text : c->flags & R_ON ? t->accent : t->text_dim);
    } else if (c->flags & R_NUM) {
        char n[8];

        snprintf(n, sizeof n, "%u", c->num);
        ui_text(&font_s8, 2, y + 15, 18, 2, n, dim, y, y + ROW_H - 1);
    } else if (c->icon != NO_ICON) {
        ui_icon(c->icon, 6, y + 5, ic);
    }
    if (c->flags & R_CHEV) {
        ui_icon(ICON_CHEV, right - 6, y + 6, dim);
        right -= 10;
    }
    if (c->v[0]) {
        vw = text_width(&font_b13, c->v, -1);
        ui_text(&font_b13, right - vw, y + 15, vw, 0, c->v, sel ? t->sel_text : off ? t->text_off : t->value,
                y, y + ROW_H - 1);
        vw += 8;
    }
    ui_text(c->flags & R_BOLD ? &font_m13 : &font_b13, 24, y + 15, right - vw - 24, 0, c->t, fg, y, y + ROW_H - 1);
}

static void list_draw(void)
{
    const theme_t *t = T;
    const ent_t *e = top();
    int h = L.nr * ROW_H;

    ui_fill(0, L.y0, DISPLAY_W, h, t->background);
    if (!L.n) {
        ui_text(&font_b13, 0, L.y0 + 50, DISPLAY_W, 1, "Nothing here", t->text_dim, L.y0, L.y0 + h - 1);
        return;
    }
    for (int s = 0; s < L.nr && e->top + s < L.n; s++)
        draw_row(s, L.y0 + s * ROW_H, e->top + s == e->cur);
    if (L.n > L.nr) {
        int th = (h - 4) * L.nr / L.n, y;

        th = th < 8 ? 8 : th;
        y = L.y0 + 2 + (h - 4 - th) * e->top / (L.n - L.nr);
        ui_fill(317, L.y0 + 2, 1, h - 4, t->divider);
        ui_fill(316, y, 3, th, t->scroll);
    }
}

/* ---- the frame --------------------------------------------------------------------------------- */

void nav_keys_changed(void)
{
    const page_t *p = nav_top();
    keybar_t k;

    memset(&k, 0, sizeof k);
    for (const keydef_t *d = p ? p->keys : NULL; d && d->key; d++) {
        int s = key_slot(d->key);
        const char *l = d->label;

        if (s < 0 || !l)
            continue;
        if (l == VERB)
            l = p->verb ? p->verb(nav_cursor(), d->gest == IG_HOLD) : "OPEN";
        if (d->gest == IG_HOLD)
            k.hold[s] = l;
        else if (d->gest == IG_TAP || d->gest == IG_DOWN)
            k.tap[s] = l;
    }
    frame_keys(&k);
}

void nav_draw(void)
{
    const page_t *p = nav_top();

    display_clear(T->background);
    frame_draw();
    if (!p)
        return;
    if (p->kind == PK_QUESTION && p->draw)
        p->draw();
    if (is_list(p))
        list_draw();
    else if (p->draw)
        p->draw();
}

void nav_toast(int icon, int tone, const char *fmt, ...)
{
    char b[64];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    frame_toast(icon, tone, b);
    log_printf("ui: toast %s", b);
}

/* ---- keys -------------------------------------------------------------------------------------- */

static const keybar_t k_layer = {
    { "BRIGHT+", "BRIGHT\xE2\x88\x92", "SHOT", "LOCK", "PANEL" }, { 0 }
};
static uint32_t s_layer_at;               /* the shortcut layer: when it began, 0 = off */
static int s_shot;                        /* a screenshot after the next frame (without the layer) */
static struct { uint32_t t; const page_t *p; int d; } s_undo;   /* the last VOL press, for the chord */
static int s_menu_shown;                  /* the power menu came up for this press */

int nav_volume_keys(void)
{
    return !power_screen_on() || (nav_top() && nav_top()->kind == PK_CUSTOM && nav_top()->keys &&
                                  nav_top()->keys[0].act == A_VOL_UP);
}

static void log_key(const input_event_t *g)
{
    static const char *const k_names[] = { "none", "up", "down", "select", "back", "power", "menu", "chord" };
    const char *on = nav_top() ? nav_top()->name : "-";
    int vol = g->key == KEY_UP || g->key == KEY_DOWN, leg = 0;

    if (!(g->gesture == IG_DOWN || (g->gesture == IG_END && g->key != KEY_SELECT) ||
          (vol && (g->gesture == IG_TAP || g->gesture == IG_HOLD)))) {
        if (g->gesture == IG_REPEAT)
            log_printf("ui: key %s.repeat on %s t=%u held=%u steps=%u n=%u", input_key_name(g->key), on,
                       (unsigned)g->t_ms, (unsigned)g->held_ms, (unsigned)g->steps, (unsigned)g->repeat);
        else
            log_printf("ui: key %s.%s on %s t=%u held=%u", input_key_name(g->key), input_gesture_name(g), on,
                       (unsigned)g->t_ms, (unsigned)g->held_ms);
    }
    /* the old events' lines too (input_poll's: VOL on its press and repeats, the others on a tap or a
     * hold, the chord), which the tests count */
    if (vol && (g->gesture == IG_DOWN || g->gesture == IG_REPEAT))
        leg = g->gesture == IG_REPEAT ? 2 : 1;
    else if (!vol && (g->gesture == IG_TAP || g->gesture == IG_HOLD || g->gesture == IG_CHORD))
        leg = g->gesture == IG_HOLD ? 3 : 1;
    if (leg)
        log_printf("ui: key %s%s on %s", k_names[g->key < 8 ? g->key : 0],
                   leg == 2 ? " (repeat)" : leg == 3 ? " (hold)" : "", on);
}

int ap_power_ok(void);                    /* ap_ui.c: the power key's press began after boot's */
void ap_brightness(int d);
void ap_screen(int on);

static void layer_key(const input_event_t *ev)
{
    s_layer_at = 0;
    frame_layer(NULL);
    switch (ev->key) {
    case KEY_UP:
    case KEY_DOWN:
        ap_brightness(ev->key == KEY_UP ? 1 : -1);
        nav_toast(ICON_SUN, TOAST_TEXT, "Brightness %u / 8", g_cfg.brightness);
        break;
    case KEY_SELECT:
        s_shot = 1;                       /* after the frame without the layer */
        break;
    case KEY_MENU:
        g_locked ^= 1;
        nav_toast(ICON_LOCK, TOAST_TEXT, g_locked ? "Keys locked" : "Keys unlocked");
        break;
    case KEY_BACK:
        log_printf("ui: panel redraw");
        ui_invalidate_all();
        break;
    }
}

static const keydef_t *lookup(const page_t *p, const input_event_t *ev)
{
    for (const keydef_t *d = p ? p->keys : NULL; d && d->key; d++)
        if (d->key == ev->key && d->gest == ev->gesture)
            return d;
    return NULL;
}

void np_library(void);                    /* screens.c: the playing track's folder */
void np_band_off(void);                   /* screens.c: the progress band again */

static void toggle_np(void)
{
    int k = nav_find(&pg_np);

    if (nav_top() == &pg_np) {
        if (nav_below() && nav_below() != &pg_home)
            nav_pop();
        else
            np_library();
    } else if (k >= 0) {
        nav_pop_to(k + 1);
    } else if (np_loaded()) {
        nav_push(&pg_np, 0);
    } else {
        nav_toast(ICON_NOTE, TOAST_TEXT, "Nothing is playing");
    }
}

static void act(int a)
{
    const page_t *p = nav_top();

    switch (a) {
    case A_UP:
    case A_DOWN:
        if (is_list(p)) {
            int n = g_ev->gesture == IG_REPEAT ? g_ev->steps : 1;

            list_move(a == A_UP ? -n : n, g_ev->gesture == IG_DOWN);
        } else if (p->act) {
            p->act(a, nav_cursor());
        }
        break;
    case A_BACK:
    case A_CLOSE:
        nav_pop();
        break;
    case A_HOME:
        nav_home();
        break;
    case A_NP:
        toggle_np();
        break;
    case A_OPTIONS:
        nav_push(p->options ? p->options : &pg_opts, 0);
        break;
    case A_NOP:
        break;
    default:
        if (p->act)
            p->act(a, nav_cursor());
        break;
    }
}

void nav_key(const input_event_t *ev)
{
    const keydef_t *d;
    int on = power_screen_on();
    unsigned seq;

    g_ev = ev;
    log_key(ev);
    power_activity();
    if (ev->key == KEY_POWER) {           /* tap: the screen off / on (the menu: nav_tick) */
        if (ev->gesture == IG_TAP && ap_power_ok() && !s_menu_shown)
            ap_screen(!on);
        if (ev->gesture == IG_TAP || ev->gesture == IG_RELEASE)
            s_menu_shown = 0;
        return;
    }
    if (ev->gesture == IG_CHORD) {        /* the shortcut layer; the first key's step undone */
        if (ev->t_ms - s_undo.t < 600u && s_undo.p == nav_top()) {
            if (is_list(s_undo.p)) {
                list_move(s_undo.d, 1);
            } else if (s_undo.p->act) {     /* the opposite step (volume, an editor's value) */
                input_event_t u = *ev;

                u.gesture = IG_DOWN;
                g_ev = &u;
                s_undo.p->act(s_undo.d > 0 ? A_VOL_UP : A_VOL_DN, nav_cursor());
                g_ev = ev;
                np_band_off();
            }
        }
        s_undo.p = NULL;
        frame_press(KEY_NONE);
        if (on) {
            s_layer_at = time_stamp(time_ms());
            frame_layer(&k_layer);
        }
        log_printf("ui: shortcut layer");
        return;
    }
    if (s_layer_at) {
        if ((ev->key == KEY_UP || ev->key == KEY_DOWN) ? ev->gesture == IG_DOWN : ev->gesture == IG_TAP)
            layer_key(ev);
        return;
    }
    if (!on) {                            /* screen off: the music keys work blind, MENU / BACK wake */
        if (ev->key == KEY_UP || ev->key == KEY_DOWN || ev->key == KEY_SELECT) {
            const page_t *mp = nav_top() == &pg_dac ? &pg_dac : &pg_np;

            d = lookup(mp, ev);
            if (d && !g_locked)
                mp->act(d->act, 0);
        } else if (ev->gesture == IG_TAP) {
            ap_screen(1);
        }
        return;
    }
    if (ev->gesture == IG_DOWN) {
        frame_press(ev->key);
        display_mark_input(ev->t_ms);     /* key->px: the slot's feedback */
    } else if (ev->gesture == IG_TAP || ev->gesture == IG_RELEASE) {
        frame_press(KEY_NONE);
    }
    if (g_locked) {
        if (ev->gesture == IG_TAP)        /* (on the release: the chord that unlocks starts with a VOL press) */
            nav_toast(ICON_LOCK, TOAST_WARN, "Keys locked: VOL+ & VOL\xE2\x88\x92, then MENU");
        return;
    }
    d = lookup(nav_top(), ev);
    if (!d || d->act == A_NONE)
        return;
    seq = ui_inval_seq;
    s_undo.p = NULL;
    act(d->act);
    if (ui_inval_seq == seq)
        return;
    display_mark_input(ev->t_ms);         /* key->px: the result (when the key changed something) */
    if (ev->gesture == IG_DOWN && (ev->key == KEY_UP || ev->key == KEY_DOWN)) {   /* for the chord */
        s_undo.t = ev->t_ms;
        s_undo.p = nav_top();
        s_undo.d = d->act == A_UP || d->act == A_VOL_UP ? (is_list(s_undo.p) ? 1 : -1) : (is_list(s_undo.p) ? -1 : 1);
    }
}

void np_power_menu(void);                 /* screens.c */

static uint32_t s_live;

void nav_live_now(void)
{
    s_live = time_ms() - 1000u;
}

void nav_tick(void)
{
    uint32_t now = time_ms();
    const page_t *p = nav_top();
    hdr_t h;

    if (s_layer_at && now - s_layer_at >= 2000u) {
        s_layer_at = 0;
        frame_layer(NULL);
    }
    if (s_shot && !s_layer_at && !ui_is_dirty()) {
        int r = screenshot_save();

        s_shot = 0;
        log_printf("ui: screenshot: %d", r);
        nav_toast(ICON_CHECK, TOAST_TEXT, r >= 0 ? "Screenshot saved" : "Screenshot failed");
    }
    if (!s_menu_shown && ap_power_ok() && input_held_ms(KEY_POWER) >= 1000u) {
        s_menu_shown = 1;                 /* (held on to 2 s: power off, ap_ui.c) */
        if (!power_screen_on())
            ap_screen(1);
        np_power_menu();
    }
    frame_tick(now);
    memset(&h, 0, sizeof h);
    nav_header(&h);
    {
        static uint8_t s_out = ICON_SPEAKER;

        if (h.out != s_out) {             /* (the header's output field: a BT link up or down) */
            s_out = h.out;
            log_printf("ui: header output %s", h.out == ICON_BT ? "bt" : h.out == ICON_USBC ? "usb-c" : "jack");
        }
    }
    frame_status(&h);
    if (p && p->tick)
        p->tick();
    p = nav_top();
    if (p && p->live && is_list(p) && now - s_live >= 250u) {
        ent_t *e = top();
        int n = p->count ? p->count() : 0;

        s_live = now;
        if (n != L.n) {                   /* rows came or went: the cursor stays on its row (by id) */
            static row_t r;
            int s = e->cur - e->top, id = s >= 0 && s < L.nr && e->cur < L.n ? L.c[s].id : -1;

            for (int i = 0; id >= 0 && i < n; i++) {
                memset(&r, 0, sizeof r);
                p->row(i, &r);
                if (r.id == id) {
                    e->cur = (int16_t)i;
                    break;
                }
            }
            list_reload();
        } else {
            for (int s = 0; s < L.nr && e->top + s < L.n; s++)
                if (fill(s, e->top + s))
                    inval_slot(s);
            nav_keys_changed();
        }
    }
    if (p && p->kind == PK_CUSTOM)
        nav_keys_changed();               /* (Now Playing's PLAY label follows the state) */
}
