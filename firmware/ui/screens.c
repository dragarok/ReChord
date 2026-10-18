/*
 * screens.c -- the screen tree of DESIGN.md 1.2 on the templates of 2.2 (nav.c, frame.c):
 * Home, Folders and the folder browser, Now Playing (and the USB DAC session on its template),
 * Options, Queue, Track info, Settings with its groups, the choice list, the slider, the
 * questions (cable, power menu, restore defaults) and the viewers (About, Diagnostics, Battery,
 * the USB disk session). Bluetooth's screen is startup/ap_bt.c.
 *
 * Not built yet, so not in the tree (no dead rows): Library (needs the tag index), EQ,
 * ReplayGain, volume limit, sleep timer, clock, Visualizer, Key guide, letter jumps.
 */
#include <stdio.h>
#include <string.h>

#include "nav.h"
#include "ui.h"
#include "audio/audio.h"
#include "drivers/bt_jieli.h"
#include "drivers/clock.h"
#include "drivers/usb_uac.h"
#include "services/config.h"
#include "services/dirlist.h"
#include "services/fs.h"
#include "services/log.h"
#include "services/power.h"
#include "services/queue.h"
#include "player.h"

/* ap_ui.c */
void ap_later(void (*fn)(void));
int ap_cable(void);
void ap_usb_disk(void), ap_usb_dac(void), ap_dac_test(void), ap_usb_console(void), ap_usb_probe(void);
void ap_usb_detect(void), ap_bt_listen(void), ap_te_probe(void), ap_update(void), ap_power_off(void), ap_screen(int on);
void ap_cable_choice(int c);
const char *ap_keys_text(void);
int ap_diag_count(void);
void ap_diag_row(int i, row_t *r);
const char *player_source(void);
extern const page_t pg_bt;
const char *bt_state_text(bt_state_t st);   /* ap_bt.c */

#define MINUS "\xE2\x88\x92"
#define DOT " \xC2\xB7 "

/* ---- key tables (DESIGN.md 3.2) ---------------------------------------------------------------- */

#define LIST_MOVE { KEY_UP, IG_DOWN, A_UP, "UP" }, { KEY_UP, IG_REPEAT, A_UP, NULL }, \
                  { KEY_DOWN, IG_DOWN, A_DOWN, "DOWN" }, { KEY_DOWN, IG_REPEAT, A_DOWN, NULL }
static const keydef_t k_list[] = {
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, VERB }, { KEY_SELECT, IG_HOLD, A_OPEN_HOLD, VERB },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { KEY_BACK, IG_TAP, A_BACK, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_menu[] = {       /* lists that are not music (Settings and its pages): PLAY held =
                                          * Now Playing (user request 2026-10-18; DEFAULT when nothing plays) */
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, VERB }, { KEY_SELECT, IG_HOLD, A_GO_NP, VERB },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { KEY_BACK, IG_TAP, A_BACK, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_home[] = {
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, VERB }, { KEY_SELECT, IG_HOLD, A_GO_NP, VERB },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { 0 }
};
static const keydef_t k_opts[] = {
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, "CHOOSE" },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_CLOSE, "CLOSE" },
    { KEY_BACK, IG_TAP, A_CLOSE, "CLOSE" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_choice[] = {
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, "CHOOSE" },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" },
    { KEY_BACK, IG_TAP, A_CANCEL, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_question[] = {
    LIST_MOVE,
    { KEY_SELECT, IG_TAP, A_OPEN, "CHOOSE" },
    { KEY_BACK, IG_TAP, A_SAFE, VERB },
    { 0 }
};
static const keydef_t k_np[] = {
    { KEY_UP, IG_DOWN, A_VOL_UP, "VOL+" }, { KEY_UP, IG_REPEAT, A_VOL_UP, NULL },
    { KEY_DOWN, IG_DOWN, A_VOL_DN, "VOL" MINUS }, { KEY_DOWN, IG_REPEAT, A_VOL_DN, NULL },
    { KEY_UP, IG_HOLD, A_SEEK, NULL }, { KEY_DOWN, IG_HOLD, A_SEEK, NULL },     /* 2x+hold: seek */
    { KEY_UP, IG_RELEASE, A_SEEK_GO, NULL }, { KEY_DOWN, IG_RELEASE, A_SEEK_GO, NULL },
    { KEY_SELECT, IG_TAP, A_PLAY, VERB }, { KEY_SELECT, IG_HOLD, A_PLAY_HOLD, VERB },
    { KEY_SELECT, IG_END, A_PLAY_END, NULL },
    { KEY_MENU, IG_TAP, A_NP, "LIBRARY" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { KEY_BACK, IG_TAP, A_BACK, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_dac[] = {
    { KEY_UP, IG_DOWN, A_VOL_UP, "VOL+" }, { KEY_UP, IG_REPEAT, A_VOL_UP, NULL },
    { KEY_DOWN, IG_DOWN, A_VOL_DN, "VOL" MINUS }, { KEY_DOWN, IG_REPEAT, A_VOL_DN, NULL },
    { KEY_SELECT, IG_TAP, A_PLAY, "PAUSE" }, { KEY_SELECT, IG_HOLD, A_PLAY_HOLD, "NEXT" },
    { KEY_SELECT, IG_END, A_PLAY_END, NULL },
    { KEY_MENU, IG_TAP, A_NOP, NULL },
    { KEY_BACK, IG_TAP, A_END, "END" }, { KEY_BACK, IG_HOLD, A_END, NULL },
    { 0 }
};
static const keydef_t k_slider[] = {
    { KEY_UP, IG_DOWN, A_VOL_UP, "+" }, { KEY_UP, IG_REPEAT, A_VOL_UP, NULL },
    { KEY_DOWN, IG_DOWN, A_VOL_DN, MINUS }, { KEY_DOWN, IG_REPEAT, A_VOL_DN, NULL },
    { KEY_SELECT, IG_TAP, A_DONE, "DONE" }, { KEY_SELECT, IG_HOLD, A_DEFAULT, "DEFAULT" },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" },
    { KEY_BACK, IG_TAP, A_CANCEL, "CANCEL" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_view[] = {          /* paged: VOL turns pages */
    { KEY_UP, IG_DOWN, A_UP, VERB }, { KEY_DOWN, IG_DOWN, A_DOWN, VERB },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { KEY_BACK, IG_TAP, A_BACK, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};
static const keydef_t k_session[] = {        /* the USB disk session: storage is the computer's */
    { KEY_BACK, IG_HOLD, A_NOP, "END" },
    { 0 }
};

/* ---- small helpers ------------------------------------------------------------------------------ */

static void fmt_time(char *o, int cap, uint32_t s)
{
    if (s >= 3600u)
        snprintf(o, (unsigned)cap, "%u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
    else
        snprintf(o, (unsigned)cap, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

static void fmt_db(char *o, int cap, int db)
{
    snprintf(o, (unsigned)cap, db < 0 ? MINUS "%d" : "%d", db < 0 ? -db : db);
}

/* len bytes of src into dst (cap), NUL-terminated. (The libc's printf has no "%.*s".) */
static void cpy(char *dst, int cap, const char *src, int len)
{
    if (len > cap - 1)
        len = cap - 1;
    memcpy(dst, src, (size_t)len);
    dst[len] = 0;
}

/* A file name as a row: the extension dropped (audio), a leading track number apart ("13. Name"). */
static int split_name(const char *name, int audio, char *out, int cap)
{
    const char *p = name, *dot = audio ? strrchr(name, '.') : NULL;
    int num = 0, nd = 0, len;

    while (*p >= '0' && *p <= '9' && nd < 3)
        num = num * 10 + (*p++ - '0'), nd++;
    if (audio && nd && (*p == '.' || *p == ' ' || *p == '-' || *p == '_') && p[1] && p != dot) {
        while (*p == '.' || *p == ' ' || *p == '-' || *p == '_')
            p++;
    } else {
        p = name;
        num = 0;
    }
    len = dot && dot > p ? (int)(dot - p) : (int)strlen(p);
    cpy(out, cap, p, len);
    return num;
}

static int playing_clus(uint32_t *clus)
{
    audio_status_t st;

    audio_status(&st);
    *clus = st.track.ref.clus;
    return st.seq != 0;
}

int np_loaded(void)
{
    uint32_t c;

    return playing_clus(&c);
}

static const char *const k_modes[] = { "Normal", "Shuffle", "Repeat one", "Repeat all" };

static int mode_icon(void)
{
    switch (queue_mode()) {
    case Q_SHUFFLE:    return ICON_SHUFFLE;
    case Q_REPEAT_ONE: return ICON_REPEAT1;
    case Q_REPEAT_ALL: return ICON_REPEAT;
    default:           return NO_ICON;
    }
}

/* ---- the header (nav_tick, every pass) --------------------------------------------------------- */

extern const page_t pg_dac;

void nav_header(hdr_t *h)
{
    const page_t *p = nav_top();
    const power_battery_t *b = power_battery();
    audio_status_t st;
    int ps = player_get_state();

    audio_status(&st);
    h->icon = p ? p->icon : NO_ICON;
    h->mode_icon = NO_ICON;
    if (p == &pg_np) {
        h->np = 1;
        snprintf(h->title, sizeof h->title, "%u / %u", st.seq ? (unsigned)st.track.index + 1u : 0u,
                 (unsigned)queue_count());
        h->mode_icon = (uint8_t)mode_icon();
    } else if (p) {
        if (p->title_fn)
            p->title_fn(h->title, sizeof h->title);
        else
            snprintf(h->title, sizeof h->title, "%s", p->title ? p->title : "");
        if (st.seq && (ps == PLAYER_PLAYING || ps == PLAYER_PAUSED) && p != &pg_dac) {
            h->play = ps == PLAYER_PLAYING ? 1 : 2;
            fmt_time(h->elapsed, sizeof h->elapsed, (uint32_t)(player_get_position_ms() / 1000));
        }
    }
    if (bt_routed()) {
        memcpy(h->vol, "BT ", 3);
        fmt_db(h->vol + 3, sizeof h->vol - 3, player_get_volume());
    } else {
        fmt_db(h->vol, sizeof h->vol, player_get_volume());
    }
    h->out = bt_state() == BT_CONNECTED ? ICON_BT : audio_output_usb() ? ICON_USBC : ICON_SPEAKER;
    h->lock = (uint8_t)g_locked;
    h->batt = (uint8_t)(b->pct10 / 10u > 100u ? 100u : b->pct10 / 10u);
    h->charging = b->vbus && !b->full;
}

/* ---- Home --------------------------------------------------------------------------------------- */

enum { H_NP, H_FOLDERS, H_QUEUE, H_BT, H_USB, H_SETTINGS, H_N };
extern const page_t pg_vols, pg_queue, pg_set;
enum { G_ROOT, G_PLAYBACK, G_DISPLAY, G_POWER, G_USB, G_SYSTEM, G_DEV };

static int home_ids(uint8_t *ids)
{
    int n = 0, loaded = np_loaded();

    for (int i = 0; i < H_N; i++)
        if ((i != H_NP && i != H_QUEUE) || loaded)
            ids[n++] = (uint8_t)i;
    return n;
}

static int home_count(void)
{
    uint8_t ids[H_N];

    return home_ids(ids);
}

static void np_title(char *o, int cap);

static void home_row(int i, row_t *r)
{
    static const uint8_t icons[H_N] = { ICON_PLAY, ICON_FOLDER, ICON_QUEUE, ICON_BT, ICON_USB, ICON_GEAR };
    static const char *const names[H_N] = { "Now Playing", "Folders", "Queue", "Bluetooth", "USB", "Settings" };
    uint8_t ids[H_N];
    int id;

    home_ids(ids);
    id = ids[i];
    r->id = (uint16_t)id;
    r->icon = icons[id];
    r->flags = R_CHEV;
    snprintf(r->text, sizeof r->text, "%s", names[id]);
    switch (id) {
    case H_NP:
        np_title(r->value, sizeof r->value);
        break;
    case H_QUEUE: {
        audio_status_t st;

        audio_status(&st);
        snprintf(r->value, sizeof r->value, "%u / %u", (unsigned)st.track.index + 1u, (unsigned)queue_count());
        break;
    }
    case H_BT:
        if (bt_state() == BT_CONNECTED)
            snprintf(r->value, sizeof r->value, "%s", bt_peer()->name[0] ? bt_peer()->name : "Connected");
        else
            snprintf(r->value, sizeof r->value, "%s", bt_state_text(bt_state()));
        break;
    case H_USB:
        snprintf(r->value, sizeof r->value, "%s", audio_output_usb() ? "USB-C earphones" : ap_cable() ? "cable in" :
                 "no cable");
        break;
    }
}

static void home_act(int a, int i)
{
    uint8_t ids[H_N];

    if (a != A_OPEN || i >= home_ids(ids))
        return;
    switch (ids[i]) {
    case H_NP:       nav_push(&pg_np, 0); break;
    case H_FOLDERS:  nav_push(&pg_vols, 0); break;
    case H_QUEUE:    nav_push(&pg_queue, 0); break;
    case H_BT:       nav_push(&pg_bt, 0); break;
    case H_USB:      nav_push(&pg_set, G_USB); break;
    case H_SETTINGS: nav_push(&pg_set, G_ROOT); break;
    }
}

static void home_enter(int first)
{
    if (first)
        nav_set_cursor(np_loaded() ? 0 : 0);   /* Now Playing when loaded, else Folders: both row 0 */
}

static const char *home_verb(int i, int hold) { return hold ? (np_loaded() ? "PLAYING" : "") : "OPEN"; }

const page_t pg_home = {
    .name = "home", .kind = PK_LIST, .icon = ICON_HOME, .live = 1, .keys = k_home, .title = "ReChord",
    .count = home_count, .row = home_row, .act = home_act, .enter = home_enter, .verb = home_verb,
};

/* ---- Folders: the volumes, then the folder browser ---------------------------------------------- */

static char s_path[256];                  /* the deepest browser level on the stack */
static char s_vols[2][16];
static int s_nvols;
extern const page_t pg_br, pg_br_opts;

static void vols_scan(void)
{
    fs_dirent_t e;
    uint32_t got, total;

    s_nvols = 0;
    for (uint32_t i = 0; i < 2; i++)
        if (fs_list_dir("/", i, &e, 1, &got, &total) == 0 && got)
            snprintf(s_vols[s_nvols++], sizeof s_vols[0], "%s", e.name);
}

static int vols_count(void)
{
    vols_scan();
    return s_nvols < 2 ? s_nvols + 1 : s_nvols;     /* no card: its row greyed */
}

static void vols_row(int i, row_t *r)
{
    int sd = i >= s_nvols || strcmp(s_vols[i], "Internal");

    r->id = (uint16_t)sd;
    r->icon = sd ? ICON_SD : ICON_CHIP;
    r->flags = R_CHEV;
    snprintf(r->text, sizeof r->text, "%s", sd ? "SD card" : "Internal");
    if (i >= s_nvols) {
        r->flags = R_OFF;
        snprintf(r->value, sizeof r->value, "no card");
    }
}

static int br_find(const char *name)
{
    for (int i = 0; i < dirlist_count(); i++)
        if (!strcmp(dirlist_name(i), name))
            return i;
    return -1;
}

/* Open the browser at path (a folder on a volume) and cursor: each level pushed with its cursor
 * on the next one down. */
static void browse_to(const char *path, int cursor)
{
    const char *p = path + 1, *q;
    int vi;

    q = strchr(p, '/');
    for (vi = 0; vi < s_nvols; vi++)
        if ((int)strlen(s_vols[vi]) == (q ? (int)(q - p) : (int)strlen(p)) && !strncmp(s_vols[vi], p, strlen(s_vols[vi])))
            break;
    if (vi == s_nvols)
        return;
    nav_set_cursor(vi);
    snprintf(s_path, sizeof s_path, "/%s", s_vols[vi]);
    nav_push(&pg_br, 1);
    while (q) {
        char name[FS_NAME_MAX];
        int k;

        p = q + 1;
        q = strchr(p, '/');
        cpy(name, sizeof name, p, q ? (int)(q - p) : (int)strlen(p));
        if ((k = br_find(name)) < 0 || !dirlist_is_dir(k))
            return;
        nav_set_cursor(k);
        {
            size_t l = strlen(s_path);

            if (l + strlen(name) + 2 > sizeof s_path)
                return;
            snprintf(s_path + l, sizeof s_path - l, "/%s", name);
        }
        nav_push(&pg_br, nav_arg() + 1);
    }
    if (nav_cursor() == 0 && cursor > 0)
        nav_set_cursor(cursor);
}

static void vols_enter(int first)
{
    vols_scan();
    if (first && g_cfg.browse[0] == '/' && g_cfg.browse[1])
        browse_to(g_cfg.browse, g_cfg.cursor);   /* Folders reopens where you were */
}

/* PLAY on a track or a folder: Now Playing at once, named from the row (DESIGN 4.1: within 100 ms);
 * the queue and the decoder start after that frame (nav_after()), as tags and format fill in. */
static struct { uint8_t kind, later; uint32_t old_seq; char path[300]; } s_pend;
enum { PEND_FILE = 1, PEND_TREE };

static void np_pending(const char *name);

static void play_later(int kind, const char *path, const char *name)
{
    audio_status_t st;

    audio_status(&st);
    snprintf(s_pend.path, sizeof s_pend.path, "%s", path);
    s_pend.kind = (uint8_t)kind;
    s_pend.later = 0;
    s_pend.old_seq = st.seq;
    if (nav_find(&pg_np) < 0)
        nav_push(&pg_np, 0);
    else
        nav_pop_to(nav_find(&pg_np) + 1);
    np_pending(name);
}

static long s_seek_go = -1;                /* a seek to start after the frame (np_act A_SEEK_GO), ms */

void nav_after(void)
{
    int k = s_pend.kind, rc;

    if (s_seek_go >= 0) {                     /* (after the band's frame: key->px stays the band's) */
        if (player_seek(s_seek_go) != PLAYER_OK)
            nav_toast(ICON_WARN, TOAST_WARN, "Seek failed");
        s_seek_go = -1;
    }

    if (!k)
        return;
    if (!s_pend.later) {                      /* the pass that put Now Playing on the panel ends there; the
                                               * track starts in the next: frame and start in one pass were
                                               * 13 + 90 ms on the device ("worst pass 103 ms on player: draw
                                               * 13, start 90", round 1), 8 + 47 in QEMU on the DMA path */
        s_pend.later = 1;
        return;
    }
    s_pend.kind = 0;
    rc = k == PEND_FILE ? player_open(s_pend.path) : player_open_tree(s_pend.path);
    if (rc == PLAYER_OK)
        rc = player_play();
    if (rc != PLAYER_OK) {
        const char *n = strrchr(s_pend.path, '/');

        log_printf("browser: play '%s' failed (%d)", n ? n + 1 : s_pend.path, rc);
        log_flush("error");
        s_pend.old_seq = 0xFFFFFFFFu;
        if (nav_top() == &pg_np)
            nav_pop();
        nav_toast(ICON_WARN, TOAST_DANGER, rc == PLAYER_ERR_EMPTY ? "Nothing to play here" : "Cannot play (%d)", rc);
    }
}

static void vols_act(int a, int i)
{
    char path[24];

    if (i >= s_nvols) {
        nav_toast(ICON_SD, TOAST_WARN, "No SD card");
        return;
    }
    snprintf(path, sizeof path, "/%s", s_vols[i]);
    if (a == A_OPEN) {
        snprintf(s_path, sizeof s_path, "%s", path);
        nav_push(&pg_br, 1);
    } else if (a == A_OPEN_HOLD) {
        play_later(PEND_TREE, path, s_vols[i]);
    }
}

static const char *vols_verb(int i, int hold) { return hold ? "PLAY ALL" : "OPEN"; }

const page_t pg_vols = {
    .name = "folders", .kind = PK_LIST, .icon = ICON_FOLDER, .live = 1, .keys = k_list, .title = "Folders",
    .count = vols_count, .row = vols_row, .act = vols_act, .enter = vols_enter, .verb = vols_verb,
};

static int s_br_n;

static void br_load(void)
{
    int more, n = dirlist_load(s_path, NULL, 0, &more);

    if (n < 0)
        log_printf("browser: list '%s' failed (%d)", s_path, n);
    s_br_n = n < 0 ? 0 : n;
}

static int br_count(void) { return s_br_n; }

static void br_row(int i, row_t *r)
{
    uint32_t clus;
    int dir = dirlist_is_dir(i);

    r->id = (uint16_t)i;
    r->icon = dir ? ICON_FOLDER : ICON_NOTE;
    r->flags = dir ? R_CHEV : 0;
    r->num = (uint16_t)split_name(dirlist_name(i), !dir, r->text, sizeof r->text);
    if (r->num)
        r->flags |= R_NUM;
    if (!dir && playing_clus(&clus) && dirlist_ref(i)->clus == clus)
        r->flags |= R_PLAYING;
}

static void br_title(char *o, int cap)
{
    const char *s = strrchr(s_path, '/');

    snprintf(o, (unsigned)cap, "%s", nav_arg() == 1 && !strcmp(s_path, "/SD") ? "SD card" : s ? s + 1 : s_path);
}

/* The playing track's row in this folder, -1. */
static int br_playing(void)
{
    uint32_t clus;

    if (!playing_clus(&clus))
        return -1;
    for (int i = 0; i < s_br_n; i++)
        if (!dirlist_is_dir(i) && dirlist_ref(i)->clus == clus)
            return i;
    return -1;
}

static void br_enter(int first)
{
    int k;

    br_load();                            /* (again on return: a play queue reuses the listing) */
    if ((k = br_playing()) >= 0 && (first || nav_below() != &pg_br))
        nav_set_cursor(k);                /* the library opens at the playing track */
    snprintf(g_cfg.browse, sizeof g_cfg.browse, "%s", s_path);
}

static void br_leave(void)
{
    char *s = strrchr(s_path, '/');

    if (s && s != s_path)
        *s = 0;
}

static void br_tick(void)
{
    if (!strcmp(g_cfg.browse, s_path))
        g_cfg.cursor = (uint16_t)nav_cursor();
}

static int br_join(char *out, int cap, const char *name)
{
    return snprintf(out, (unsigned)cap, "%s/%s", s_path, name) < cap ? 0 : -1;
}

static void br_act(int a, int i)
{
    char full[300];

    if (i >= s_br_n || br_join(full, sizeof full, dirlist_name(i)))
        return;
    if (a == A_OPEN && dirlist_is_dir(i)) {
        if (strlen(full) < sizeof s_path) {
            strcpy(s_path, full);
            nav_push(&pg_br, nav_arg() + 1);
        }
    } else if (a == A_OPEN) {
        play_later(PEND_FILE, full, dirlist_name(i));
    } else if (a == A_OPEN_HOLD && dirlist_is_dir(i)) {
        play_later(PEND_TREE, full, dirlist_name(i));
    } else if (a == A_OPEN_HOLD) {                     /* the folder from its first track */
        for (int k = 0; k < s_br_n; k++)
            if (!dirlist_is_dir(k)) {
                br_join(full, sizeof full, dirlist_name(k));
                play_later(PEND_FILE, full, dirlist_name(k));
                break;
            }
    }
}

static const char *br_verb(int i, int hold)
{
    if (i >= s_br_n)
        return "";
    return hold ? "PLAY ALL" : dirlist_is_dir(i) ? "OPEN" : "PLAY";
}

const page_t pg_br = {
    .name = "browser", .kind = PK_LIST, .icon = ICON_FOLDER, .keys = k_list, .title_fn = br_title,
    .count = br_count, .row = br_row, .act = br_act, .enter = br_enter, .leave = br_leave, .tick = br_tick,
    .verb = br_verb, .options = &pg_br_opts,
};

/* MENU on Now Playing with nothing under it: the playing track's folder. */
void np_library(void)
{
    const char *src = player_source();

    if (!src || src[0] != '/')
        return;
    nav_pop();
    snprintf(g_cfg.browse, sizeof g_cfg.browse, "%s", src);
    g_cfg.cursor = 0;
    nav_push(&pg_vols, 0);
}

/* ---- Now Playing (2.2 N) ------------------------------------------------------------------------ */

static const uint16_t k_hues[8] = {     /* the monogram tile's hues (content, not theme: tools/ui_mockups.py) */
    0x7A7A, 0x2C70, 0xC287, 0x3B78, 0xAA2B, 0x5C65, 0xB3C3, 0x4AF5
};

static struct {
    uint32_t seq, pos, dur, until, clus;
    char title[96], artist[64], album[64], next[64], init[4], codec[8], fmt[12], facts[40];
    uint8_t hue, hires, paused, band, win, err;   /* band: 0 progress, 1 volume, 2 seek */
    int16_t head;
} N;

/* The seek (VOL+ 2x+hold forward, VOL- 2x+hold back; DESIGN 2.2 N's Seek band): the target moves
 * 2 s every 100 ms held, 10 s after 2 s; the release goes there. Not part of N: a seek outlives
 * np_load(), and the volume before the presses is kept across them. */
static struct {
    uint8_t on, key;
    int8_t dir;
    uint32_t t0, from, to;           /* the hold's time, the position then, the target (ms) */
} K;
static int s_vol0;                   /* the volume before a VOL multi-press (its first press) */

static void np_title(char *o, int cap)
{
    track_info_t ti;

    player_get_track(&ti);
    split_name(ti.title, !ti.artist[0], o, cap);
}

static void initials(const char *s, char *o)
{
    const char *last = NULL, *p;
    int n = 0;

    for (p = s; *p; p++)
        if ((p == s || p[-1] == ' ') && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            last = p;
    for (p = s; *p && !n; p++)
        if ((p == s || p[-1] == ' ') && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9')))
            o[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    if (last && n && last != p - 1)
        o[n++] = (char)(*last >= 'a' && *last <= 'z' ? *last - 32 : *last);
    o[n] = 0;
}

static void np_load(const audio_status_t *st)
{
    static const char *const codecs[] = { "", "FLAC", "WAV", "MP3", "AIFF", "AAC", "ALAC", "OGG", "OPUS", "WAV",
                                          "AIFF", "WV", "DSD", "APE", "WMA", "WMA", "WMA" };
    track_info_t ti;
    const char *album;
    unsigned h = 0, r = st->track.rate;

    memset(&N, 0, sizeof N);
    N.seq = st->seq;
    if (!st->seq)
        return;
    N.clus = st->track.ref.clus;
    player_get_track(&ti);
    split_name(ti.title, !ti.artist[0], N.title, sizeof N.title);
    snprintf(N.artist, sizeof N.artist, "%s", ti.artist);
    snprintf(N.album, sizeof N.album, "%s", ti.album);
    N.dur = ti.duration_ms > 0 ? (uint32_t)(ti.duration_ms / 1000) : 0;
    album = N.album[0] ? N.album : N.title;
    initials(album, N.init);
    for (const char *p = album; *p; p++)
        h += (uint8_t)*p;
    N.hue = (uint8_t)(h % 8u);
    snprintf(N.codec, sizeof N.codec, "%s", st->track.codec < 17u ? codecs[st->track.codec] : "?");
    if (r % 1000u)
        snprintf(N.fmt, sizeof N.fmt, "%u/%u.%u", (unsigned)st->track.bits, r / 1000u, r % 1000u / 100u);
    else
        snprintf(N.fmt, sizeof N.fmt, "%u/%u", (unsigned)st->track.bits, r / 1000u);
    N.hires = st->track.bits > 16u || r > 48000u;
    {   /* the facts line: bit rate (the file's, or the MP3 frame's) and size */
        uint32_t sz = st->track.ref.size, kb = st->track.kbps ? st->track.kbps :
                      N.dur ? (uint32_t)((uint64_t)sz * 8u / 1000u / N.dur) : 0u;

        if (kb)
            snprintf(N.facts, sizeof N.facts, "%u KBPS \xC2\xB7 %u.%u MB", (unsigned)kb, (unsigned)(sz >> 20),
                     (unsigned)((sz & 0xFFFFFu) * 10u >> 20));
        else
            snprintf(N.facts, sizeof N.facts, "%u.%u MB", (unsigned)(sz >> 20), (unsigned)((sz & 0xFFFFFu) * 10u >> 20));
    }
    {
        int nx = queue_next_pos();
        char name[FS_NAME_MAX];

        if (nx >= 0 && queue_ref(nx) && fs_ref_name(queue_ref(nx), name, sizeof name) == 0) {
            int ascii = 1;

            split_name(name, 1, N.next, sizeof N.next);
            for (char *p = N.next; *p; p++) {
                ascii &= (uint8_t)*p < 0x80u;
                if (*p >= 'a' && *p <= 'z')
                    *p = (char)(*p - 32);
            }
            if (!ascii)
                N.next[0] = 0;
        }
    }
}

/* DESIGN 2.2 N, revision 2 (no key bar): the tile 112 px at (6, 24), the text column x 126..312,
 * badges y 108..118, the facts line at baseline 130, the band y 138..169. */
#define BAND_Y 138
#define ART_X 6
#define ART_Y 24
#define ART_S 112
#define TX 126
#define TW 186
#define BADGE_Y 108
#define PAUSE_X (ART_X + ART_S - 22)
#define PAUSE_Y (ART_Y + ART_S - 22)

static void np_inval_band(void) { ui_invalidate(0, BAND_Y, DISPLAY_W - 1, DISPLAY_H - 1); }

void np_band_off(void)                    /* (the chord took back the VOL press that showed it) */
{
    if (N.band == 1 && nav_top() == &pg_np) {
        N.band = 0;
        np_inval_band();
    }
}

static int head_x(void)
{
    return N.dur ? 8 + (int)(304u * (N.pos < N.dur ? N.pos : N.dur) / N.dur) : 8;
}

static void np_pending(const char *name)
{
    memset(&N, 0, sizeof N);
    N.seq = 0xFFFFFFFFu;                  /* (not a track yet: np_poll() takes the new one when it comes) */
    split_name(name, 1, N.title, sizeof N.title);
    initials(N.title, N.init);
    ui_invalidate(0, BODY_Y, DISPLAY_W - 1, DISPLAY_H - 1);
}

/* The seek's target after `held` ms of the hold: 2 s at once and every 100 ms, 10 s a step after
 * 2 s; within the track (to 1 s before its end). Stock MINIV390 also moves only the shown time while
 * the key is held and seeks once at the release (seg27 0x307e15e/0x307e21c, the release 0x307e2aa ->
 * 0x03088390: mailbox ch 1 cmd 0xB DECODE_SEEK, ms), in flat 8 s steps a repeat (~80 ms, mov r1,#8000
 * at 0x307e19a); here finer first, faster later (the user's ask). */
static uint32_t seek_target(uint32_t held)
{
    uint32_t d = held < 2000u ? 2000u * (1u + held / 100u) : 40000u + 10000u * (1u + (held - 2000u) / 100u);
    int64_t to = (int64_t)K.from + (K.dir > 0 ? (int64_t)d : -(int64_t)d);
    int64_t end = N.dur ? (int64_t)N.dur * 1000 - 1000 : to;

    if (to > end)
        to = end;
    return to < 0 ? 0u : (uint32_t)to;
}

/* DESIGN 2.2 N, Seek: the target marker (3 x 12) and the fill in accent, the target time (M13), the
 * delta centred, the total on the right. */
static void seek_band(void)
{
    const theme_t *t = T;
    char a[12], d[16];
    uint32_t to = K.to / 1000u, from = K.from / 1000u, dd = to > from ? to - from : from - to;
    int x = N.dur ? 8 + (int)(304u * (to < N.dur ? to : N.dur) / N.dur) : 8;

    ui_fill(8, BAND_Y + 4, 304, 4, t->bar_track);
    ui_fill(8, BAND_Y + 4, x - 8, 4, t->accent);
    ui_fill(x - 1, BAND_Y, 3, 12, t->accent);
    fmt_time(a, sizeof a, to);
    ui_text(&font_m13, 8, BAND_Y + 25, 62, 0, a, t->text, BAND_Y + 12, DISPLAY_H - 1);
    memcpy(d, to >= from ? "+" : MINUS, to >= from ? 2 : 4);
    fmt_time(d + strlen(d), (int)(sizeof d - strlen(d)), dd);
    ui_text(&font_b13, 72, BAND_Y + 25, 156, 1, d, t->accent, BAND_Y + 12, DISPLAY_H - 1);
    if (N.dur) {
        fmt_time(a, sizeof a, N.dur);
        ui_text(&font_b13, 232, BAND_Y + 25, 80, 2, a, t->text_dim, BAND_Y + 12, DISPLAY_H - 1);
    }
}

void np_seek_cancel(void)
{
    if (K.on) {
        K.on = 0;
        log_printf("ui: seek cancelled");
        if (N.band == 2) {
            N.band = 0;
            if (nav_top() == &pg_np)
                np_inval_band();
        }
    }
}

static void np_poll(void)
{
    audio_status_t st;
    int paused = player_get_state() == PLAYER_PAUSED;
    uint32_t pos;

    audio_status(&st);
    if (st.seq != N.seq && !(N.seq == 0xFFFFFFFFu && (s_pend.kind || st.seq == s_pend.old_seq))) {
        if (N.seq && N.seq != 0xFFFFFFFFu && st.seq && st.track.ref.clus == N.clus) {
            N.seq = st.seq;                   /* the same file again (a seek): nothing to redraw */
        } else {
            uint8_t band = N.band;
            uint32_t until = N.until;

            np_load(&st);
            N.band = band;                    /* (the volume band stays its 1.5 s) */
            N.until = until;
            ui_invalidate(0, BODY_Y, DISPLAY_W - 1, DISPLAY_H - 1);
        }
    }
    if (K.on) {                               /* the seek's target follows the time held */
        if (!input_held_ms(K.key)) {          /* (its release never came: another key took the ladder) */
            const input_event_t *e = g_ev;

            g_ev = NULL;
            np_act(A_SEEK_GO, 0);
            g_ev = e;
        } else {
            uint32_t to = seek_target(time_ms() - K.t0);

            if (to != K.to) {
                K.to = to;
                np_inval_band();
            }
        }
    }
    if (paused != N.paused) {
        N.paused = (uint8_t)paused;
        ui_invalidate(PAUSE_X, PAUSE_Y, PAUSE_X + 17, PAUSE_Y + 17);
    }
    if (N.band == 1 && (int32_t)(time_ms() - N.until) >= 0) {
        N.band = 0;
        np_inval_band();
    }
    pos = s_seek_go >= 0 ? N.pos : (uint32_t)(player_get_position_ms() / 1000);   /* (a seek about to go: its target) */
    if (pos != N.pos) {
        int a = head_x();

        N.pos = pos;
        if (!N.band) {
            int b = head_x();

            ui_invalidate((a < b ? a : b) - 1, BAND_Y + 2, (a > b ? a : b) + 1, BAND_Y + 9);
            ui_invalidate(8, BAND_Y + 11, 70, DISPLAY_H - 1);
            ui_invalidate(232, BAND_Y + 11, 312, DISPLAY_H - 1);
        }
    }
}

static void art(const char *init, int hue, int paused)
{
    uint16_t c = k_hues[hue & 7], dark = ui_mix(c, 0, 90);

    for (int k = 0; k < 4; k++)
        ui_fill(ART_X, ART_Y + k * (ART_S / 4), ART_S, ART_S / 4, ui_mix(c, dark, k * 85));
    if (init[0]) {
        int w = text_width(&font_d28, init, -1);

        ui_text(&font_d28, ART_X + (ART_S - w) / 2, ART_Y + ART_S / 2 + 10, w, 0, init, 0xFFFF, ART_Y, ART_Y + ART_S - 1);
    }
    if (paused) {
        ui_fill(PAUSE_X, PAUSE_Y, 18, 18, ui_mix(T->background, 0, 77));
        ui_icon(ICON_PAUSE, PAUSE_X + 3, PAUSE_Y + 3, 0xFFFF);
    }
}

static int badge(int x, const char *s, uint16_t bg, uint16_t fg, int outline)
{
    int w = text_width(&font_s8, s, -1) + 6;

    if (outline) {
        ui_frame_rect(x, BADGE_Y, w, 11, bg);
    } else {
        ui_fill(x + 1, BADGE_Y, w - 2, 11, bg);
        ui_fill(x, BADGE_Y + 1, 1, 9, bg);
        ui_fill(x + w - 1, BADGE_Y + 1, 1, 9, bg);
    }
    ui_text(&font_s8, x + 3, BADGE_Y + 9, w - 6, 0, s, fg, BADGE_Y, BADGE_Y + 10);
    return x + w + 4;
}

/* Title (T17, two lines), artist, album at x 114..312. */
static void text_col(const char *title, const char *artist, const char *album)
{
    const theme_t *t = T;
    text_line_t ln[2];
    int n = text_wrap(&font_t17, title, -1, TW, 2, ln), yb = n == 2 ? 42 : 48;
    char b[100];

    for (int i = 0; i < n; i++) {
        cpy(b, sizeof b, title + ln[i].off, ln[i].len);
        ui_text(&font_t17, TX, yb + 20 * i, TW, 0, b, t->text, BODY_Y + 2, BADGE_Y - 1);
    }
    yb = n == 2 ? 82 : 70;
    ui_text(&font_b13, TX, yb, TW, 0, artist, t->text, BODY_Y + 2, BADGE_Y - 1);
    ui_text(&font_b13, TX, yb + 18, TW, 0, album, t->text_dim, BODY_Y + 2, BADGE_Y - 1);
}

static void vol_band(int db, int bt)
{
    const theme_t *t = T;
    char v[16];
    int w = (db + 60) * 220 / 60;

    w = w < 0 ? 0 : w > 220 ? 220 : w;
    ui_icon(bt ? ICON_BT : ICON_SPEAKER, 8, BAND_Y + 9, t->accent);
    ui_fill(26, BAND_Y + 12, 220, 6, t->bar_track);
    ui_fill(26, BAND_Y + 12, w, 6, t->bar_fill);
    if (bt) {
        memcpy(v, "BT ", 3);
        fmt_db(v + 3, sizeof v - 3, db);
    } else {
        fmt_db(v, sizeof v - 3, db);
        memcpy(v + strlen(v), " dB", 4);
    }
    ui_text(&font_m13, 250, BAND_Y + 22, 62, 2, v, t->text, BAND_Y, DISPLAY_H - 1);
}

static void np_draw(void)
{
    const theme_t *t = T;
    char a[12], b[12];

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    if (!N.seq) {
        ui_text(&font_b13, 0, 92, DISPLAY_W, 1, "Nothing playing", t->text_dim, BODY_Y, BAND_Y);
        return;
    }
    art(N.init, N.hue, N.paused);
    if (N.seq == 0xFFFFFFFFu) {           /* starting: the row's name only */
        text_col(N.title, "", "");
        return;
    }
    text_col(N.title, N.artist, N.album);
    {
        int x = badge(TX, N.codec, N.hires ? t->hires : t->badge_bg, N.hires ? 0 : t->badge_text, 0);

        badge(x, N.fmt, N.hires ? t->hires : t->badge_bg, N.hires ? t->hires : t->text_dim, 1);
    }
    ui_text(&font_s8, TX, 130, TW, 0, N.facts, t->text_dim, 122, 132);
    if (N.band == 1) {
        vol_band(player_get_volume(), bt_routed());
        return;
    }
    if (N.band == 2) {
        seek_band();
        return;
    }
    {
        int hx = head_x();

        ui_fill(8, BAND_Y + 4, 304, 4, t->bar_track);
        ui_fill(8, BAND_Y + 4, hx - 8, 4, t->bar_fill);
        ui_fill(hx - 1, BAND_Y + 2, 2, 8, t->text);
    }
    fmt_time(a, sizeof a, N.pos);
    ui_text(&font_b13, 8, BAND_Y + 25, 62, 0, a, t->text, BAND_Y + 11, DISPLAY_H - 1);
    b[0] = 0;
    if (N.dur) {
        memcpy(b, MINUS, 3);
        fmt_time(b + 3, sizeof b - 3, N.dur > N.pos ? N.dur - N.pos : 0);
    }
    ui_text(&font_b13, 232, BAND_Y + 25, 80, 2, b, t->text_dim, BAND_Y + 11, DISPLAY_H - 1);
    if (N.next[0]) {
        char nx[72];

        snprintf(nx, sizeof nx, "NEXT  %s", N.next);
        ui_text(&font_s8, 72, BAND_Y + 23, 156, 1, nx, t->text_dim, BAND_Y + 11, DISPLAY_H - 1);
    }
}

static void ensure_playing(int rc)
{
    if (rc != PLAYER_OK)
        nav_toast(ICON_QUEUE, TOAST_TEXT, "End of the queue");
    else if (player_get_state() == PLAYER_PAUSED)
        player_play();
}

void np_act(int a, int i)
{
    const input_event_t *ev = g_ev;

    if (K.on && ev && ev->key != K.key && a != A_SEEK_GO)
        np_act(A_SEEK_GO, 0);                /* another key: the seek held so far goes first */
    switch (a) {
    case A_VOL_UP:
    case A_VOL_DN: {
        int n = ev && ev->gesture == IG_REPEAT ? ev->steps : 1, v = player_get_volume() + (a == A_VOL_UP ? n : -n);

        /* single presses stay instant; a second press held becomes the seek at its hold (no
         * repeat first), and the seek then takes the presses' steps back (s_vol0) */
        if (K.on || (ev && ev->gesture == IG_REPEAT && ev->taps >= 2))
            break;
        if (!ev || ev->taps <= 1)
            s_vol0 = player_get_volume();

        player_set_volume(v < -90 ? -90 : v > 0 ? 0 : v);
        N.band = 1;
        N.until = time_ms() + 1500u;
        if (nav_top() == &pg_np)
            np_inval_band();
        break;
    }
    case A_PLAY:                          /* the first tap at once; 2x / 3x when the window closes */
        if (ev->taps <= 1) {
            int st = player_get_state();

            if (st == PLAYER_PLAYING)
                player_pause();
            else
                log_printf("ui: play: %d", player_play());
        }
        N.win = 1;
        break;
    case A_PLAY_END:
        N.win = 0;
        if (ev->taps == 2)
            ensure_playing(player_next());
        else if (ev->taps == 3)
            ensure_playing(player_prev());
        break;
    case A_PLAY_HOLD:
        if (ev->taps <= 1)
            ensure_playing(player_next());
        break;
    case A_SEEK:                              /* VOL 2x (or 3x) + hold */
        if (!ev || ev->taps < 2 || K.on || !np_loaded())
            break;
        if (player_get_volume() != s_vol0) {
            log_printf("ui: volume back to %d dB (the seek's presses)", s_vol0);
            player_set_volume(s_vol0);
            N.band = 0;
        }
        if (!player_seekable()) {
            nav_toast(ICON_WARN, TOAST_WARN, "Can't seek in %s", N.codec[0] ? N.codec : "this format");
            if (nav_top() == &pg_np)
                np_inval_band();
            break;
        }
        K.on = 1;
        K.key = ev->key;
        K.dir = ev->key == KEY_UP ? 1 : -1;
        K.t0 = ev->t_ms;
        K.from = (uint32_t)player_get_position_ms();
        K.to = seek_target(0);
        N.band = 2;
        log_printf("ui: seek %s from %u ms", K.dir > 0 ? "forward" : "back", (unsigned)K.from);
        if (nav_top() == &pg_np)
            np_inval_band();
        break;
    case A_SEEK_GO:
        if (!K.on)
            break;
        if (ev && ev->key == K.key && ev->gesture == IG_RELEASE)
            K.to = seek_target(ev->t_ms - K.t0);     /* (the release's own time: exact, screen off too) */
        K.on = 0;
        N.band = 0;
        log_printf("ui: seek to %u ms (%c%u s)", (unsigned)K.to, K.to >= K.from ? '+' : '-',
                   (unsigned)((K.to >= K.from ? K.to - K.from : K.from - K.to) / 1000u));
        s_seek_go = (long)K.to;               /* (nav_after: once the band is gone from the panel) */
        N.pos = K.to / 1000u;
        if (nav_top() == &pg_np)
            np_inval_band();
        break;
    }
}

static const char *np_verb(int i, int hold)
{
    if (N.win)
        return hold ? "3x PREV" : "2x NEXT";
    return hold ? "NEXT" : player_get_state() == PLAYER_PLAYING ? "PAUSE" : "PLAY";
}

static void np_enter(int first)
{
    audio_status_t st;

    audio_status(&st);
    if (N.seq == 0xFFFFFFFFu && (s_pend.kind || st.seq == s_pend.old_seq))
        return;                           /* (starting: np_pending() has it) */
    np_load(&st);
    N.paused = player_get_state() == PLAYER_PAUSED;
    N.pos = (uint32_t)(player_get_position_ms() / 1000);
}

extern const page_t pg_np_opts;

const page_t pg_np = {
    .name = "player", .kind = PK_CUSTOM, .keys = k_np, .act = np_act, .enter = np_enter, .draw = np_draw,
    .tick = np_poll, .verb = np_verb, .options = &pg_np_opts,
};

/* ---- the USB DAC session (Now Playing's template; ap_ui.c runs it) ----------------------------- */

usb_uac_status_t g_uac;
int g_uac_leave;
static usb_uac_status_t s_uac_shown;

static void dac_lines(char *a, char *b, int cap)
{
    const usb_uac_status_t *u = &g_uac;

    if (!u->configured)
        snprintf(a, (unsigned)cap, "waiting for the host");
    else if (!u->bits)
        snprintf(a, (unsigned)cap, "%s speed, idle", u->high_speed ? "high" : "full");
    else
        snprintf(a, (unsigned)cap, "PCM %u bit" DOT "%u.%u kHz", (unsigned)u->bits, (unsigned)(u->rate / 1000u),
                 (unsigned)(u->rate % 1000u / 100u));
    if (u->mute)
        snprintf(b, (unsigned)cap, "host muted" DOT "late %u", (unsigned)u->underruns);
    else
        snprintf(b, (unsigned)cap, "host %d.%d dB" DOT "late %u" DOT "lost %u", (int)(u->volume_256db / 256),
                 (int)((u->volume_256db < 0 ? -u->volume_256db : u->volume_256db) % 256 * 10 / 256),
                 (unsigned)u->underruns, (unsigned)(u->dropped + u->missed));
}

static void dac_draw(void)
{
    const theme_t *t = T;
    char a[48], b[48], f[12];

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    art("DAC", 3, 0);
    dac_lines(a, b, sizeof a);
    text_col("Computer audio", a, b);
    snprintf(f, sizeof f, "%u/%u", (unsigned)g_uac.bits, (unsigned)(g_uac.rate / 1000u));
    if (g_uac.bits) {
        int hi = g_uac.bits > 16u || g_uac.rate > 48000u;
        int x = badge(TX, "PCM", hi ? t->hires : t->badge_bg, hi ? 0 : t->badge_text, 0);

        x = badge(x, f, hi ? t->hires : t->badge_bg, hi ? t->hires : t->text_dim, 1);
        badge(x, g_uac.high_speed ? "UAC2" : "UAC1", t->divider, t->text_dim, 1);
    }
    vol_band(player_get_volume(), bt_routed());
}

static void dac_tick(void)
{
    if (memcmp(&g_uac, &s_uac_shown, sizeof g_uac)) {
        usb_uac_status_t now = g_uac;
        char a[48], b[48], c[48], d[48];

        dac_lines(a, b, sizeof a);
        g_uac = s_uac_shown;              /* (what is on the panel, in the same words) */
        dac_lines(c, d, sizeof c);
        g_uac = now;
        if (strcmp(a, c) || strcmp(b, d))
            ui_invalidate(TX, BODY_Y, 312, BADGE_Y - 1);
        if (g_uac.bits != s_uac_shown.bits || g_uac.rate != s_uac_shown.rate || g_uac.high_speed != s_uac_shown.high_speed)
            ui_invalidate(TX, BADGE_Y, 312, BADGE_Y + 10);
        s_uac_shown = g_uac;
    }
}

static void dac_act(int a, int i)
{
    const input_event_t *ev = g_ev;

    switch (a) {
    case A_VOL_UP:
    case A_VOL_DN: {
        int n = ev->gesture == IG_REPEAT ? ev->steps : 1, v = player_get_volume() + (a == A_VOL_UP ? n : -n);

        player_set_volume(v < -90 ? -90 : v > 0 ? 0 : v);
        np_inval_band();
        break;
    }
    case A_PLAY_END:                      /* the host's transport once the taps are counted: one key each */
        usb_uac_key(ev->taps >= 3 ? USB_UAC_PREV : ev->taps == 2 ? USB_UAC_NEXT : USB_UAC_PLAY_PAUSE);
        break;
    case A_PLAY_HOLD:
        usb_uac_key(USB_UAC_NEXT);
        break;
    case A_END:
        g_uac_leave = 1;
        break;
    }
}

static void dac_enter(int first)
{
    memset(&s_uac_shown, 0, sizeof s_uac_shown);
}

const page_t pg_dac = {
    .name = "usbdac", .kind = PK_CUSTOM, .icon = ICON_DAC, .keys = k_dac, .title = "USB DAC",
    .act = dac_act, .enter = dac_enter, .draw = dac_draw, .tick = dac_tick,
};

/* ---- Options (MENU hold): the thing's verbs, then the listening rows ---------------------------- */

enum { O_PREV, O_QUEUE, O_INFO, O_FOLDER, O_PLAYF, O_SHUF, O_LOCK, O_MODE };
static const uint8_t k_np_o[] = { O_PREV, O_QUEUE, O_INFO, O_FOLDER, O_LOCK, O_MODE };
static const uint8_t k_br_o[] = { O_PLAYF, O_SHUF, O_LOCK, O_MODE };
static const uint8_t k_gen_o[] = { O_LOCK, O_MODE };
extern const page_t pg_info;

static const uint8_t *opt_ids(int *n)
{
    const page_t *p = nav_top();

    if (p == &pg_np_opts) {
        *n = sizeof k_np_o;
        return k_np_o;
    }
    if (p == &pg_br_opts) {
        *n = sizeof k_br_o;
        return k_br_o;
    }
    *n = sizeof k_gen_o;
    return k_gen_o;
}

static int opt_count(void)
{
    int n;

    opt_ids(&n);
    return n;
}

static void opt_row(int i, row_t *r)
{
    static const char *const names[] = { "Previous track", "Queue", "Track info", "Go to folder", "Play folder",
                                         "Shuffle folder", "Lock keys", "Play mode" };
    static const uint8_t icons[] = { ICON_PREV, ICON_QUEUE, ICON_INFO, ICON_FOLDER, ICON_PLAY, ICON_SHUFFLE,
                                     ICON_LOCK, ICON_REPEAT };
    int n, id = opt_ids(&n)[i];

    r->id = (uint16_t)id;
    r->icon = icons[id];
    snprintf(r->text, sizeof r->text, "%s", names[id]);
    if (id == O_QUEUE || id == O_INFO || id == O_FOLDER)
        r->flags = R_CHEV;
    if (id == O_MODE)
        snprintf(r->value, sizeof r->value, "%s", k_modes[queue_mode() % 4]);
    if (id == O_QUEUE)
        snprintf(r->value, sizeof r->value, "%u", (unsigned)queue_count());
}

static void opt_title(char *o, int cap)
{
    char t[96];

    t[0] = 0;
    if (nav_below() == &pg_np)
        np_title(t, sizeof t);
    else if (nav_below() == &pg_br) {
        br_title(t, sizeof t);
    }
    snprintf(o, (unsigned)cap, t[0] ? "Options" DOT "%s" : "Options", t);
}

static void opt_act(int a, int i)
{
    int n, id = opt_ids(&n)[i];
    char full[300];

    if (a != A_OPEN)
        return;
    switch (id) {
    case O_PREV:
        nav_pop();
        ensure_playing(player_prev());
        break;
    case O_QUEUE:
        nav_push(&pg_queue, 0);
        break;
    case O_INFO:
        nav_push(&pg_info, 0);
        break;
    case O_FOLDER:
        nav_pop_to(1);
        if (player_source() && player_source()[0] == '/') {
            snprintf(g_cfg.browse, sizeof g_cfg.browse, "%s", player_source());
            nav_push(&pg_vols, 0);
        }
        break;
    case O_PLAYF:
    case O_SHUF:
        nav_pop();                        /* (the browser again, its listing reloaded) */
        i = nav_cursor();
        if (i >= s_br_n)
            break;
        if (id == O_SHUF && queue_mode() != Q_SHUFFLE) {
            queue_set_mode(Q_SHUFFLE);
            g_cfg.play = Q_SHUFFLE;
            config_save_later();
        }
        br_join(full, sizeof full, dirlist_is_dir(i) ? dirlist_name(i) : "");
        if (!dirlist_is_dir(i))
            full[strlen(full) - 1] = 0;   /* (the folder itself) */
        play_later(PEND_TREE, full, strrchr(full, '/') ? strrchr(full, '/') + 1 : full);
        break;
    case O_LOCK:
        nav_pop();
        g_locked = 1;
        nav_toast(ICON_LOCK, TOAST_TEXT, "Keys locked");
        break;
    case O_MODE:
        nav_toast(mode_icon() == NO_ICON ? ICON_REPEAT : mode_icon(), TOAST_TEXT, "Play mode: %s",
                  k_modes[player_cycle_mode() % 4]);
        list_row_changed(i);
        break;
    }
}

#define OPTS(nm) { .name = "options", .kind = PK_LIST, .icon = ICON_GEAR, .keys = k_opts, .title_fn = opt_title, \
                   .count = opt_count, .row = opt_row, .act = opt_act }
const page_t pg_np_opts = OPTS(np), pg_br_opts = OPTS(br), pg_opts = OPTS(gen);

/* ---- Queue -------------------------------------------------------------------------------------- */

static void q_title(char *o, int cap)
{
    audio_status_t st;

    audio_status(&st);
    snprintf(o, (unsigned)cap, "Queue  %u / %u", st.seq ? (unsigned)st.track.index + 1u : 0u, (unsigned)queue_count());
}

static void q_row(int i, row_t *r)
{
    char name[FS_NAME_MAX];
    audio_status_t st;

    audio_status(&st);
    r->id = (uint16_t)i;
    r->num = (uint16_t)(i + 1);
    r->flags = st.seq && (int)st.track.index == i ? R_PLAYING : R_NUM;
    if (queue_ref(i) && fs_ref_name(queue_ref(i), name, sizeof name) == 0)
        split_name(name, 1, r->text, sizeof r->text);
}

static void q_enter(int first)
{
    audio_status_t st;

    audio_status(&st);
    if (first && st.seq)
        nav_set_cursor((int)st.track.index);
}

static void q_act(int a, int i)
{
    if (a == A_OPEN && i < queue_count()) {
        log_printf("ui: queue: play %d: %d", i, audio_play(i));
        if (nav_find(&pg_np) >= 0)
            nav_pop_to(nav_find(&pg_np) + 1);
        else
            nav_push(&pg_np, 0);
    }
}

static const char *q_verb(int i, int hold) { return hold ? "" : "PLAY"; }

const page_t pg_queue = {
    .name = "queue", .kind = PK_LIST, .icon = ICON_QUEUE, .keys = k_list, .title_fn = q_title,
    .count = queue_count, .row = q_row, .act = q_act, .enter = q_enter, .verb = q_verb,
};

/* ---- viewers (2.2 V): label / value lines, VOL turns pages -------------------------------------- */

#define VIEW_LINES 8                      /* DESIGN 2.2 V rev. 2: 8 lines, baselines 36 + 18 i */
#define VIEW_Y 36
static uint32_t s_view_sig[VIEW_LINES];

static void view_draw(void)
{
    const theme_t *t = T;
    const page_t *p = nav_top();
    static row_t r;
    int base = nav_arg() * VIEW_LINES, n = p->count();

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    for (int i = 0; i < VIEW_LINES && base + i < n; i++) {
        int y = VIEW_Y + 18 * i;

        memset(&r, 0, sizeof r);
        p->row(base + i, &r);
        ui_text(&font_b13, 10, y, 118, 0, r.text, t->text_dim, y - 15, y + 3);
        ui_text(&font_b13, 130, y, 182, 0, r.value, r.flags & R_OFF ? t->warn : t->text, y - 15, y + 3);
    }
}

static void view_tick(void)
{
    static uint32_t last;
    const page_t *p = nav_top();
    static row_t r;
    int base = nav_arg() * VIEW_LINES, n = p->count();

    if (time_ms() - last < 500u)
        return;
    last = time_ms();
    for (int i = 0; i < VIEW_LINES; i++) {
        uint32_t h = 2166136261u;

        memset(&r, 0, sizeof r);
        if (base + i < n)
            p->row(base + i, &r);
        for (const char *s = r.value; *s; s++)
            h = (h ^ (uint8_t)*s) * 16777619u;
        for (const char *s = r.text; *s; s++)
            h = (h ^ (uint8_t)*s) * 16777619u;
        if (h != s_view_sig[i]) {
            s_view_sig[i] = h;
            ui_invalidate(10, VIEW_Y + 18 * i - 15, 312, VIEW_Y + 18 * i + 3);
        }
    }
}

static void view_enter(int first) { memset(s_view_sig, 0, sizeof s_view_sig); }

static int view_pages(void) { return (nav_top()->count() + VIEW_LINES - 1) / VIEW_LINES; }

static void view_act(int a, int i)
{
    int pg = nav_arg() + (a == A_DOWN ? 1 : a == A_UP ? -1 : 0), n = view_pages();
    const page_t *p = nav_top();

    if (n < 2 || (a != A_UP && a != A_DOWN))
        return;
    pg = (pg + n) % n;
    nav_pop();                            /* (the same viewer again on its new page) */
    nav_push(p, pg);
}

static const char *view_verb(int i, int hold) { return view_pages() > 1 ? (hold ? "" : "PAGE") : ""; }

static void view_title(char *o, int cap, const char *name)
{
    if (view_pages() > 1)
        snprintf(o, (unsigned)cap, "%s  %d / %d", name, nav_arg() + 1, view_pages());
    else
        snprintf(o, (unsigned)cap, "%s", name);
}

#define VIEWER(nm, ttl, ic, cnt, rw) \
    static void nm##_title(char *o, int cap) { view_title(o, cap, ttl); } \
    const page_t pg_##nm = { .name = #nm, .kind = PK_CUSTOM, .icon = ic, .keys = k_view, .title_fn = nm##_title, \
        .count = cnt, .row = rw, .act = view_act, .enter = view_enter, .draw = view_draw, .tick = view_tick, \
        .verb = view_verb }

static void pair(row_t *r, const char *l, const char *v) { snprintf(r->text, sizeof r->text, "%s", l); snprintf(r->value, sizeof r->value, "%s", v); }

/* Track info */
static int info_count(void) { return 7; }
static void info_row(int i, row_t *r)
{
    static const char *const l[] = { "Title", "Artist", "Album", "File", "Format", "Length", "Queue" };
    track_info_t ti;
    audio_status_t st;
    char v[64];

    audio_status(&st);
    player_get_track(&ti);
    v[0] = 0;
    switch (i) {
    case 0: snprintf(v, sizeof v, "%s", N.seq ? N.title : ti.title); break;
    case 1: snprintf(v, sizeof v, "%s", ti.artist[0] ? ti.artist : MINUS); break;
    case 2: snprintf(v, sizeof v, "%s", ti.album[0] ? ti.album : MINUS); break;
    case 3: snprintf(v, sizeof v, "%s", st.track.name); break;
    case 4: snprintf(v, sizeof v, "%s" DOT "%u bit" DOT "%u.%u kHz" DOT "%u ch", N.codec, (unsigned)st.track.bits,
                     (unsigned)(st.track.rate / 1000u), (unsigned)(st.track.rate % 1000u / 100u), (unsigned)st.track.channels); break;
    case 5: fmt_time(v, sizeof v, (uint32_t)(ti.duration_ms / 1000)); break;
    case 6: snprintf(v, sizeof v, "%u / %u", (unsigned)st.track.index + 1u, (unsigned)queue_count()); break;
    }
    pair(r, l[i], v);
}
VIEWER(info, "Track info", ICON_INFO, info_count, info_row);

/* About */
static int about_count(void) { return 5; }
static void about_row(int i, row_t *r)
{
    char v[48];
    uint32_t s = time_ms() / 1000u;

    switch (i) {
    case 0: pair(r, "Firmware", "ReChord " __DATE__); break;
    case 1: pair(r, "Board", "512 MB (MINIV)"); break;
    case 2: pair(r, "Battery", power_battery_text()); break;
    case 3: snprintf(v, sizeof v, "%u h %02u min", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u)); pair(r, "Uptime", v); break;
    case 4: pair(r, "Display", panel_io_path()); break;
    }
}
VIEWER(about, "About", ICON_INFO, about_count, about_row);

/* Battery (RECHO's Debug > Power page) */
static int batt_count(void) { return 6; }
static void batt_row(int i, row_t *r)
{
    const power_battery_t *b = power_battery();
    char v[48];

    v[0] = 0;
    switch (i) {
    case 0:
        if (b->mv)
            snprintf(v, sizeof v, "%u.%u %%" DOT "%u mV", b->pct10 / 10u, b->pct10 % 10u, b->mv);
        else
            snprintf(v, sizeof v, "measuring");
        pair(r, "Charge", v);
        break;
    case 1: pair(r, "Power", !b->vbus ? "battery" : b->full ? "cable, full" : "cable, charging"); break;
    case 2:
        if (b->rate_ok)
            snprintf(v, sizeof v, "%d mV/min", b->mv_min);
        else
            snprintf(v, sizeof v, b->vbus ? "on battery only" : "after 90 s");
        pair(r, "Rate", v);
        break;
    case 3:
        snprintf(v, sizeof v, "%d.%u" DOT "%d.%u" DOT "%d.%u", b->pct_h10[0] / 10,
                 (unsigned)(b->pct_h10[0] < 0 ? -b->pct_h10[0] : b->pct_h10[0]) % 10u, b->pct_h10[1] / 10,
                 (unsigned)(b->pct_h10[1] < 0 ? -b->pct_h10[1] : b->pct_h10[1]) % 10u, b->pct_h10[2] / 10,
                 (unsigned)(b->pct_h10[2] < 0 ? -b->pct_h10[2] : b->pct_h10[2]) % 10u);
        pair(r, "%/h 1, 5, 30 min", v);
        break;
    case 4:
        if (b->left_min)
            snprintf(v, sizeof v, "%u h %02u min", b->left_min / 60u, b->left_min % 60u);
        else
            snprintf(v, sizeof v, "not yet");
        pair(r, "Time left", v);
        break;
    case 5:
        snprintf(v, sizeof v, "A %u MHz" DOT "B %u MHz", (unsigned)(clock_core_hz() / 1000000u),
                 (unsigned)(clock_core_b_hz() / 1000000u));
        pair(r, "Clocks", v);
        break;
    }
}
VIEWER(batt, "Battery", ICON_CHARGE, batt_count, batt_row);

/* Diagnostics (Developer): ap_ui.c's numbers */
VIEWER(diag, "Diagnostics", ICON_BUG, ap_diag_count, ap_diag_row);

/* The USB disk session (ap_ui.c pushes it, then the session holds the loop) */
static int disk_count(void) { return 4; }
static void disk_row(int i, row_t *r)
{
    static const char *const l[][2] = { { "Status", "Connected to a computer" }, { "", "" },
                                        { "To finish", "eject on the computer" }, { "", "or press POWER here" } };
    pair(r, l[i][0], l[i][1]);
}
const page_t pg_usbdisk = { .name = "usbdisk", .kind = PK_CUSTOM, .icon = ICON_USB, .keys = k_session,
    .title = "USB disk", .count = disk_count, .row = disk_row, .draw = view_draw };

/* ---- Settings (groups of rows that are data) --------------------------------------------------- */

enum { S_CYCLE, S_CHOICE, S_SLIDER, S_THEME, S_OPEN, S_DO, S_INFO };
typedef struct {
    uint8_t icon, kind, n, def;
    const char *label;
    uint8_t *field;
    const char *const *names;
    void (*fn)(void);                     /* S_DO: what it does (from the main loop); others: apply */
    const page_t *open;                   /* S_OPEN */
    uint8_t arg, cable;                   /* S_OPEN's argument; cable: greyed without the cable */
} set_t;

static void apply_play(void) { queue_set_mode(g_cfg.play); }
static void apply_bright(void) { power_brightness_changed(); }
static void restore_q(void);
extern const page_t pg_restore;

static const char *const n_resume[] = { "Off", "Folder", "Last track" };
static const char *const n_show[] = { "File names", "Tags" };
static const char *const n_screen[] = { "10 s", "30 s", "1 min", "5 min", "Never" };
static const char *const n_auto[] = { "5 min", "15 min", "30 min", "Never" };
static const char *const n_offon[] = { "Off", "On" };
static const char *const n_cpu[] = { "Auto", "Max" };
static const char *const n_usb[] = { "Ask", "USB disk", "USB DAC", "Charge only" };
static const char *const n_vbus[] = { "Port", "GPIO2.1" };
static const char *const n_dacp[] = { "Battery", "Charge", "Power only" };

static const set_t g_root[] = {
    { ICON_PLAY, S_OPEN, 0, 0, "Playback", 0, 0, 0, &pg_set, G_PLAYBACK },
    { ICON_SUN, S_OPEN, 0, 0, "Display", 0, 0, 0, &pg_set, G_DISPLAY },
    { ICON_POWER, S_OPEN, 0, 0, "Power", 0, 0, 0, &pg_set, G_POWER },
    { ICON_USB, S_OPEN, 0, 0, "USB", 0, 0, 0, &pg_set, G_USB },
    { ICON_BT, S_OPEN, 0, 0, "Bluetooth", 0, 0, 0, &pg_bt, 0 },
    { ICON_INFO, S_OPEN, 0, 0, "System", 0, 0, 0, &pg_set, G_SYSTEM },
};
static const set_t g_playback[] = {
    { ICON_REPEAT, S_CYCLE, 4, Q_NORMAL, "Play mode", &g_cfg.play, k_modes, apply_play },
    { ICON_PLAY, S_CYCLE, 3, CFG_RESUME_FOLDER, "Resume at power-on", &g_cfg.resume, n_resume },
    { ICON_NOTE, S_CYCLE, 2, 1, "Show", &g_cfg.tags, n_show },
};
static const set_t g_display[] = {
    { ICON_SUN, S_THEME, 0, 0, "Theme", &g_cfg.theme },
    { ICON_SUN, S_SLIDER, 8, 8, "Brightness", &g_cfg.brightness, 0, apply_bright },
    { ICON_MOON, S_CHOICE, 5, 1, "Screen off after", &g_cfg.screen_off, n_screen },
};
extern const page_t pg_batt, pg_about, pg_diag, pg_dev;
static const set_t g_power[] = {
    { ICON_MOON, S_CHOICE, 4, 1, "Auto power off", &g_cfg.auto_off, n_auto },
    { ICON_CHARGE, S_OPEN, 0, 0, "Battery", 0, 0, 0, &pg_batt },
    { ICON_CHIP, S_CYCLE, 2, 0, "CPU clock", &g_cfg.cpu, n_cpu },
};
static const set_t g_usb[] = {
    { ICON_USB, S_DO, 0, 0, "USB disk", 0, 0, ap_usb_disk, 0, 0, 1 },
    { ICON_DAC, S_DO, 0, 0, "USB DAC", 0, 0, ap_usb_dac, 0, 0, 1 },
    { ICON_USB, S_CHOICE, 4, CFG_USB_ASK, "On cable connect", &g_cfg.usb, n_usb },
    { ICON_USBC, S_CYCLE, 2, 0, "USB-C earphones", &g_cfg.usb_out, n_offon },
    { ICON_USBC, S_CYCLE, 2, 0, "USB-C power", &g_cfg.usb_vbus, n_vbus },
    { ICON_CHARGE, S_CYCLE, 3, CFG_DAC_BATTERY, "DAC power", &g_cfg.dac_charge, n_dacp },
};
static const set_t g_system[] = {
    { ICON_UPDATE, S_DO, 0, 0, "Firmware update", 0, 0, ap_update },
    { ICON_GEAR, S_DO, 0, 0, "Restore defaults", 0, 0, restore_q },
    { ICON_INFO, S_OPEN, 0, 0, "About", 0, 0, 0, &pg_about },
    { ICON_BUG, S_OPEN, 0, 0, "Developer", 0, 0, 0, &pg_set, G_DEV },
};
static const set_t g_dev[] = {
    { ICON_BUG, S_OPEN, 0, 0, "Diagnostics", 0, 0, 0, &pg_diag },
    { ICON_DAC, S_DO, 0, 0, "DAC test", 0, 0, ap_dac_test },
    { ICON_USB, S_DO, 0, 0, "USB console", 0, 0, ap_usb_console },
    { ICON_USBC, S_DO, 0, 0, "USB-C probe", 0, 0, ap_usb_probe },
    { ICON_USB, S_DO, 0, 0, "USB detect", 0, 0, ap_usb_detect },
    { ICON_BT, S_DO, 0, 0, "BT listen (test)", 0, 0, ap_bt_listen },
    { ICON_CHIP, S_INFO, 0, 0, "Keys" },
    { ICON_INFO, S_CYCLE, 2, 0, "Debug strip", &g_cfg.strip, n_offon },
    { ICON_SUN, S_DO, 0, 0, "Panel TE (test)", 0, 0, ap_te_probe },
};
static const struct { const char *title; uint8_t icon, n; const set_t *rows; } k_groups[] = {
    { "Settings", ICON_GEAR, 6, g_root }, { "Playback", ICON_PLAY, 3, g_playback },
    { "Display", ICON_SUN, 3, g_display }, { "Power", ICON_POWER, 3, g_power },
    { "USB", ICON_USB, 6, g_usb }, { "System", ICON_INFO, 4, g_system }, { "Developer", ICON_BUG, 9, g_dev },
};
#define GRP (k_groups[nav_arg() < 7 ? nav_arg() : 0])

static const set_t *s_edit;              /* the setting a choice list / the slider edits */
static uint8_t s_orig;

static const char *set_value(const set_t *s, char *b, int cap)
{
    b[0] = 0;
    switch (s->kind) {
    case S_CYCLE:
    case S_CHOICE:
        return s->names[*s->field < s->n ? *s->field : 0];
    case S_SLIDER:
        snprintf(b, (unsigned)cap, "%u / %u", *s->field, s->n);
        return b;
    case S_THEME:
        return theme_name(*s->field) ? theme_name(*s->field) : "";
    case S_INFO:
        return ap_keys_text();
    }
    if (s->open == &pg_set && s->arg == G_DISPLAY)
        return theme_name(g_cfg.theme);
    if (s->open == &pg_set && s->arg == G_USB)
        return n_usb[g_cfg.usb & 3u];
    if (s->open == &pg_set && s->arg == G_POWER) {
        snprintf(b, (unsigned)cap, "%u %%", power_battery()->pct10 / 10u);
        return b;
    }
    if (s->open == &pg_bt)
        return bt_state_text(bt_state());
    return b;
}

static int set_count(void) { return GRP.n; }

static void set_row(int i, row_t *r)
{
    const set_t *s = &GRP.rows[i];
    char b[48];

    r->id = (uint16_t)i;
    r->icon = s->icon;
    snprintf(r->text, sizeof r->text, "%s", s->label);
    snprintf(r->value, sizeof r->value, "%s", set_value(s, b, sizeof b));
    if (s->kind != S_CYCLE && s->kind != S_INFO)
        r->flags = R_CHEV;
    if (s->cable && !ap_cable()) {
        r->flags = R_OFF;
        snprintf(r->value, sizeof r->value, "no cable");
    }
}

static void set_title(char *o, int cap) { snprintf(o, (unsigned)cap, "%s", GRP.title); }

extern const page_t pg_choice, pg_slider;

static void set_act(int a, int i)
{
    const set_t *s = &GRP.rows[i];
    char b[48];

    if (a == A_OPEN_HOLD) {               /* DEFAULT */
        if (!s->field)
            return;
        *s->field = s->def;
        if (s->kind == S_THEME)
            theme_load_index(s->def), frame_invalidate(), ui_invalidate_all();
        if (s->fn)
            s->fn();
        config_save_later();
        nav_toast(ICON_CHECK, TOAST_TEXT, "%s: %s (default)", s->label, set_value(s, b, sizeof b));
        list_row_changed(i);
        return;
    }
    if (a != A_OPEN)
        return;
    log_printf("ui: row %s", s->label);
    if (s->cable && !ap_cable()) {
        nav_toast(ICON_USB, TOAST_WARN, "Connect the cable first");
        return;
    }
    switch (s->kind) {
    case S_CYCLE:
        if (s->field == &g_cfg.play)
            player_cycle_mode();
        else
            *s->field = (uint8_t)((*s->field + 1u) % s->n);
        if (s->fn && s->field != &g_cfg.play)
            s->fn();
        config_save_later();
        nav_toast(s->icon, TOAST_TEXT, "%s: %s", s->label, set_value(s, b, sizeof b));
        list_row_changed(i);
        break;
    case S_CHOICE:
    case S_THEME:
        s_edit = s;
        nav_push(&pg_choice, 0);
        break;
    case S_SLIDER:
        s_edit = s;
        nav_push(&pg_slider, 0);
        break;
    case S_OPEN:
        nav_push(s->open, s->arg);
        break;
    case S_DO:
        ap_later(s->fn);
        break;
    }
}

static const char *set_verb(int i, int hold)
{
    const set_t *s = &GRP.rows[i < GRP.n ? i : 0];

    if (hold)
        return np_loaded() ? "PLAYING" : s->field ? "DEFAULT" : "";
    return s->kind == S_OPEN || s->kind == S_DO ? "OPEN" : s->kind == S_INFO ? "" : "CHANGE";
}


const page_t pg_set = {
    .name = "settings", .kind = PK_LIST, .icon = ICON_GEAR, .live = 1, .keys = k_menu, .title_fn = set_title,
    .count = set_count, .row = set_row, .act = set_act, .verb = set_verb,
};

/* The choice list: radio marks, the cursor on the current value; the theme previews as it moves. */
static int s_preview = -1;

static int ch_count(void) { return s_edit->kind == S_THEME ? theme_count() : s_edit->n; }

static void ch_row(int i, row_t *r)
{
    r->id = (uint16_t)i;
    r->flags = R_RADIO | (i == s_orig ? R_ON : 0);
    snprintf(r->text, sizeof r->text, "%s", s_edit->kind == S_THEME ? theme_name(i) : s_edit->names[i]);
}

static void ch_title(char *o, int cap) { snprintf(o, (unsigned)cap, "%s", s_edit->label); }

static void ch_enter(int first)
{
    if (first) {
        s_orig = *s_edit->field;
        s_preview = s_orig;
        nav_set_cursor(s_orig);
    }
}

static void ch_tick(void)
{
    if (s_edit->kind == S_THEME && nav_cursor() != s_preview) {
        s_preview = nav_cursor();
        theme_load_index(s_preview);      /* the whole screen in that theme (one full frame) */
        ui_invalidate_all();
    }
}

static void ch_act(int a, int i)
{
    if (a == A_OPEN) {
        *s_edit->field = (uint8_t)i;
        if (s_edit->fn)
            s_edit->fn();
        config_save_later();
    } else if (a == A_CANCEL && s_edit->kind == S_THEME) {
        theme_load_index(s_orig);
    } else if (a != A_CANCEL) {
        return;
    }
    nav_pop();
}

const page_t pg_choice = {
    .name = "choice", .kind = PK_LIST, .icon = ICON_GEAR, .keys = k_choice, .title_fn = ch_title,
    .count = ch_count, .row = ch_row, .act = ch_act, .enter = ch_enter, .tick = ch_tick,
};

/* The slider (2.2 E): label, the value in D28, the bar, min / max, a note. */
static void sl_draw(void)
{
    const theme_t *t = T;
    char v[16], note[64];
    int lo = 1, hi = s_edit->n, x = 16 + 288 * (*s_edit->field - lo) / (hi - lo);

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    ui_text(&font_b13, 16, 44, 288, 0, s_edit->label, t->text_dim, 30, 48);
    snprintf(v, sizeof v, "%u / %u", *s_edit->field, hi);
    ui_text(&font_d28, 0, 84, DISPLAY_W, 1, v, t->text, 58, 92);
    ui_fill(16, 100, 288, 8, t->bar_track);
    ui_fill(16, 100, x - 16, 8, t->bar_fill);
    ui_fill(x - 2, 96, 4, 16, t->text);
    for (int k = 0; k <= hi - lo; k++)
        ui_fill(16 + 288 * k / (hi - lo), 110, 1, 3, t->text_dim);
    snprintf(v, sizeof v, "%d", lo);
    ui_text(&font_s8, 16, 124, 40, 0, v, t->text_dim, 116, 126);
    snprintf(v, sizeof v, "%d", hi);
    ui_text(&font_s8, 200, 124, 104, 2, v, t->text_dim, 116, 126);
    snprintf(note, sizeof note, "Changes apply as you press. CANCEL restores %u / %u.", s_orig, hi);
    ui_text(&font_b13, 16, 150, 288, 0, note, t->text_dim, 136, 156);
}

static void sl_enter(int first)
{
    if (first)
        s_orig = *s_edit->field;
}

static void sl_act(int a, int i)
{
    int v = *s_edit->field;

    switch (a) {
    case A_VOL_UP:
    case A_VOL_DN:
        v += a == A_VOL_UP ? 1 : -1;
        v = v < 1 ? 1 : v > s_edit->n ? s_edit->n : v;
        break;
    case A_DEFAULT:
        v = s_edit->def;
        break;
    case A_CANCEL:
        v = s_orig;
        /* fall through */
    case A_DONE:
        *s_edit->field = (uint8_t)v;
        if (s_edit->fn)
            s_edit->fn();
        config_save_later();
        nav_pop();
        return;
    default:
        return;
    }
    if (v != *s_edit->field) {
        *s_edit->field = (uint8_t)v;
        if (s_edit->fn)
            s_edit->fn();
        ui_invalidate(0, 58, DISPLAY_W - 1, 113);
    }
}

static void sl_title(char *o, int cap) { snprintf(o, (unsigned)cap, "%s", s_edit->label); }

const page_t pg_slider = {
    .name = "slider", .kind = PK_CUSTOM, .icon = ICON_SUN, .keys = k_slider, .title_fn = sl_title,
    .act = sl_act, .enter = sl_enter, .draw = sl_draw,
};

/* ---- questions (2.2 Q): a prompt, up to four choices; audio keeps playing ---------------------- */

static void q_prompt(int icon, uint16_t c, const char *l1, const char *l2)
{
    const theme_t *t = T;
    int x = 10;

    ui_fill(0, BODY_Y, DISPLAY_W, 43, t->background);
    if (icon != NO_ICON) {
        ui_icon(icon, 10, 28, c);
        x = 28;
    }
    ui_text(&font_m13, x, 38, 300 - x, 0, l1, c == t->accent ? t->text : c, 24, 42);
    ui_text(&font_b13, x, 56, 300 - x, 0, l2, t->text_dim, 42, 61);
    ui_fill(0, 63, DISPLAY_W, 1, t->divider);
}

/* The cable question (ap_ui.c pushes it on a cable plugged in with usb=ask). */
static uint32_t s_q_t0;
static int s_q_left;

static int cq_count(void) { return 3; }
static void cq_row(int i, row_t *r)
{
    static const char *const l[3][2] = { { "USB disk", "computer sees the files" },
                                         { "USB DAC", "play from the computer" }, { "Charge only", "" } };
    static const uint8_t ic[3] = { ICON_USB, ICON_DAC, ICON_CHARGE };

    r->id = (uint16_t)i;
    r->icon = ic[i];
    snprintf(r->text, sizeof r->text, "%s", l[i][0]);
    if (i == 2)
        snprintf(r->value, sizeof r->value, "keep playing  %d s", s_q_left);
    else
        snprintf(r->value, sizeof r->value, "%s", l[i][1]);
}
static void cq_draw(void) { q_prompt(ICON_USB, T->accent, "Cable connected", "What should the player do?"); }
static void cq_enter(int first)
{
    if (first) {
        s_q_t0 = time_ms();
        s_q_left = 10;
        nav_set_cursor(2);                /* the safe choice */
    }
}
static void cq_choose(int c)
{
    log_printf("usb: cable question: %s", c == 0 ? "disk" : c == 1 ? "dac" : "charge");
    nav_pop();
    ap_cable_choice(c);
}
static void cq_tick(void)
{
    int left = 10 - (int)((time_ms() - s_q_t0) / 1000u);

    if (!ap_cable()) {
        cq_choose(2);
        return;
    }
    if (left != s_q_left) {
        s_q_left = left;
        if (left <= 0)
            cq_choose(2);                 /* 10 s: charge only */
        else
            list_row_changed(2);
    }
}
static void cq_act(int a, int i)
{
    if (a == A_OPEN)
        cq_choose(i);
    else if (a == A_SAFE)
        cq_choose(2);
}
static const char *cq_verb(int i, int hold) { return hold ? "" : "CHARGE"; }
const page_t pg_cable = {
    .name = "cable", .kind = PK_QUESTION, .icon = ICON_USB, .keys = k_question, .title = "USB cable",
    .count = cq_count, .row = cq_row, .act = cq_act, .enter = cq_enter, .draw = cq_draw, .tick = cq_tick,
    .verb = cq_verb,
};

/* The power menu (POWER held 1 s; held on to 2 s: off). */
static int pm_count(void) { return 3; }
static void pm_row(int i, row_t *r)
{
    static const char *const l[3] = { "Power off", "Lock keys", "Screen off" };
    static const uint8_t ic[3] = { ICON_POWER, ICON_LOCK, ICON_MOON };

    r->id = (uint16_t)i;
    r->icon = ic[i];
    snprintf(r->text, sizeof r->text, "%s", l[i]);
}
static void pm_draw(void) { q_prompt(ICON_POWER, T->accent, "Power", "Hold POWER on to switch off"); }
static void pm_act(int a, int i)
{
    if (a != A_OPEN) {
        if (a == A_SAFE)
            nav_pop();
        return;
    }
    nav_pop();
    if (i == 0)
        ap_later(ap_power_off);
    else if (i == 1) {
        g_locked = 1;
        nav_toast(ICON_LOCK, TOAST_TEXT, "Keys locked");
    } else {
        ap_screen(0);
    }
}
static const char *pm_verb(int i, int hold) { return hold ? "" : "CANCEL"; }
const page_t pg_power = {
    .name = "power", .kind = PK_QUESTION, .icon = ICON_POWER, .keys = k_question, .title = "Power",
    .count = pm_count, .row = pm_row, .act = pm_act, .draw = pm_draw, .verb = pm_verb,
};

void np_power_menu(void)
{
    if (nav_top() != &pg_power && nav_top() != &pg_dac)
        nav_push(&pg_power, 0);
}

/* Restore defaults? */
static int rq_count(void) { return 2; }
static void rq_row(int i, row_t *r)
{
    r->id = (uint16_t)i;
    r->icon = i ? ICON_CHECK : ICON_WARN;
    snprintf(r->text, sizeof r->text, "%s", i ? "Keep my settings" : "Restore defaults");
}
static void rq_draw(void)
{
    q_prompt(ICON_WARN, T->warn, "Restore the default settings?", "Volume, folders and the paired device stay");
}
static void rq_enter(int first)
{
    if (first)
        nav_set_cursor(1);
}
static void rq_act(int a, int i)
{
    if (a != A_OPEN && a != A_SAFE)
        return;
    nav_pop();
    if (a == A_OPEN && i == 0) {
        config_defaults();                /* not volume, browse, track or BT (as RECHO) */
        queue_set_mode(g_cfg.play);
        theme_load_index(g_cfg.theme);
        power_brightness_changed();
        config_save_later();
        ui_invalidate_all();
        nav_toast(ICON_CHECK, TOAST_TEXT, "Settings restored");
    }
}
static const char *rq_verb(int i, int hold) { return hold ? "" : "CANCEL"; }
const page_t pg_restore = {
    .name = "restore", .kind = PK_QUESTION, .icon = ICON_GEAR, .keys = k_question, .title = "Restore defaults",
    .count = rq_count, .row = rq_row, .act = rq_act, .enter = rq_enter, .draw = rq_draw, .verb = rq_verb,
};
static void restore_q(void) { nav_push(&pg_restore, 0); }
