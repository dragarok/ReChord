/*
 * nav.h -- the screens (DESIGN.md 1.2, 2.2, 3): a stack of pages, the key tables, the global
 * keys, and the list / question templates.
 *
 * A page is const data: its name (logged as "ui: open <name>"), its kind, its key table and its
 * callbacks. The key table is data, {key, gesture, action, label}: the key bar's labels come from
 * the same rows the keys are dispatched by, so a label cannot say something the key does not do.
 * A label VERB asks the page (PLAY's verb depends on the focused row: OPEN, PLAY, CHANGE...).
 *
 * Lists address rows by the page's own ids (row_t.id), never by position: a row that is hidden
 * or added (Now Playing on Home, a device on Bluetooth) does not move what the others do.
 */
#ifndef RECHORD_UI_NAV_H
#define RECHORD_UI_NAV_H

#include <stdint.h>

#include "frame.h"
#include "services/input.h"

/* ---- actions ---- */
enum {
    A_NONE = 0,
    A_UP, A_DOWN,                 /* list cursor (DOWN: one step, wraps; REPEAT: steps, stops at the ends) */
    A_OPEN, A_OPEN_HOLD,          /* PLAY on the focused row, tap / hold */
    A_BACK, A_HOME,               /* up one level / Home */
    A_NP, A_OPTIONS,              /* MENU: Now Playing <-> the library / Options */
    A_CLOSE,                      /* Options: close */
    A_VOL_UP, A_VOL_DN,           /* volume (Now Playing, screen off) / an editor's value */
    A_PLAY, A_PLAY_HOLD, A_PLAY_END,   /* Now Playing's PLAY: tap (1x, 2x, 3x), hold, the window closing */
    A_CANCEL, A_DONE, A_DEFAULT,  /* editors */
    A_SAFE,                       /* a question's BACK: the safe answer */
    A_END,                        /* end a USB session */
    A_NOP,                        /* a key that does nothing here (slot flash only) */
    A_GO_NP,                      /* a menu's PLAY hold: Now Playing when a track is loaded (else A_OPEN_HOLD) */
    A_SEEK, A_SEEK_GO             /* Now Playing's VOL 2x+hold: seek while held, release = go */
};

typedef struct {
    uint8_t key, gest, act;       /* KEY_*, IG_*, A_* */
    const char *label;            /* key bar: an IG_DOWN / IG_TAP row's on the top line, IG_HOLD's below */
} keydef_t;
#define VERB ((const char *)1)

/* ---- rows ---- */
enum { R_CHEV = 1, R_OFF = 2, R_RADIO = 4, R_ON = 8, R_PLAYING = 16, R_NUM = 32, R_BOLD = 64 };
typedef struct {
    uint16_t id, num;             /* the page's row id; R_NUM: the number shown */
    uint8_t icon, flags;
    char text[200];
    char value[48];
} row_t;

/* ---- pages ---- */
enum { PK_LIST, PK_QUESTION, PK_CUSTOM };
typedef struct page page_t;
struct page {
    const char *name;
    uint8_t kind, icon, live;     /* live: rows re-read every 250 ms, a changed one repainted */
    const keydef_t *keys;
    const char *title;            /* or title_fn */
    void (*title_fn)(char *out, int cap);
    int (*count)(void);
    void (*row)(int i, row_t *r);
    void (*act)(int act, int i);  /* what the nav does not: PLAY on row i, editor keys... */
    void (*enter)(int first);     /* on top: pushed (first = 1) or returned to */
    void (*leave)(void);          /* popped */
    void (*draw)(void);           /* PK_CUSTOM: the body; PK_QUESTION: the prompt */
    void (*tick)(void);           /* every pass on top */
    const char *(*verb)(int i, int hold);
    const page_t *options;        /* MENU hold (NULL: the listening rows only) */
};

/* ---- the stack ---- */
void nav_reset(const page_t *root);
void nav_push(const page_t *p, int arg);
void nav_pop(void);
void nav_home(void);
const page_t *nav_top(void);
const page_t *nav_below(void);            /* the page under the top one (NULL) */
int nav_depth(void);
int nav_arg(void);                        /* the top entry's argument */
int nav_cursor(void);
void nav_set_cursor(int i);               /* (keeps the row on the page) */
int nav_find(const page_t *p);            /* depth of the topmost entry of p, -1 */
void nav_pop_to(int depth);               /* pop until depth entries are left */

/* ---- lists ---- */
void list_reload(void);                   /* count and every visible row again, repainted */
void list_row_changed(int i);             /* one row again (repainted if visible and changed) */
void list_move(int d, int wrap);
int list_count(void);

/* ---- the frame, from the nav's state ---- */
void nav_keys_changed(void);              /* the key bar labels again (a VERB changed) */
void nav_draw(void);                      /* the frame and the top page (ui_render's draw) */
void nav_key(const input_event_t *ev);    /* every gesture */
void nav_tick(void);                      /* every pass */
void nav_live_now(void);
void nav_after(void);                     /* after the frame: a play the screen asked for (screens.c) */                  /* a live list's rows again at the next pass */
int nav_volume_keys(void);                /* VOL keys are volume now (Now Playing, screen off) */
extern const input_event_t *g_ev;         /* the event being handled */

/* ---- shared ---- */
extern int g_locked;
void nav_toast(int icon, int tone, const char *fmt, ...);
void nav_header(hdr_t *h);                /* the services' state into the header (screens.c) */
extern const page_t pg_np, pg_home, pg_opts, pg_dac, pg_vols;
void np_act(int act, int i);              /* Now Playing's keys (also with the screen off) */
void np_seek_cancel(void);                /* a seek being held ends without going (the chord) */
int np_loaded(void);

#endif
