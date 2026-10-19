/*
 * lib_ui.c -- the library's screens (docs/LIBRARY.md 5), in the library overlay: Library, All songs,
 * Albums, Artists, Genres, an album's / artist's / genre's rows, Favourites, Playlists and a list's
 * entries; the options of a row (play, shuffle, add to favourites, add to a playlist, remove, move,
 * rename, delete, jump to a letter), the playlist picker, the letter strip and the name entry.
 *
 * Lists are virtual: nav.c asks for the visible rows only, each read from the index through its
 * sector cache (services/lib_idx.c). A page's context (which list, which range) is kept per stack
 * depth here, beside nav.c's own entry. PLAY plays the list from the row (an album: the album; All
 * songs: the 400 around it); PLAY held plays the row's album / artist / genre / playlist; MENU held
 * opens the row's options.
 *
 * Names: a new playlist is "Playlist N" at once (one key, no typing), or the album's or artist's
 * name when it is made from one; Rename opens the name entry (VOL+ / VOL- move along the letters,
 * PLAY types, PLAY held changes case, BACK deletes, BACK held cancels, MENU saves).
 */
#include <stdio.h>
#include <string.h>

#include "nav.h"
#include "ui.h"
#include "audio/audio.h"
#include "drivers/clock.h"
#include "player.h"
#include "services/dirlist.h"
#include "services/lib_int.h"
#include "services/lib_m3u.h"
#include "services/queue.h"
#include "services/config.h"

#define MINUS "\xE2\x88\x92"
#define DOT " \xC2\xB7 "

/* screens.c and player_audio.c (resident) */
void np_play_fn(const char *name, int (*start)(void));
int player_open_list(int n, int start, int (*get)(int i, fs_ref_t *r, int *kind), const char *src);

/* ---- context per stack depth ---- */

enum { V_ROOT, V_SONGS, V_ALBUMS, V_ARTISTS, V_GENRES, V_ALBUM, V_ARTIST, V_GENRE, V_ASONGS, V_LISTS, V_LIST,
       V_OPTS, V_PICK, V_LETTER, V_TEXT, V_ASK };

typedef struct {
    uint8_t kind, vol, sec, list;    /* V_*, the index's volume, its section, the list (V_LIST) */
    uint32_t first, n;               /* the range of sec this page shows */
    uint32_t a_first, d;             /* an artist's tracks (its "All songs"), for V_ARTIST */
    int32_t cur;                     /* its cursor (kept by tick: its options read it) */
    char name[32];                   /* its title */
} ctx_t;

#define NCTX 16                      /* nav.c NAV_MAX */
static ctx_t s_ctx[NCTX];
static uint8_t s_rbuf[600];          /* one row being read */
static int s_vol;                    /* the volume the library shows */
static uint32_t s_changed;           /* g_lib.changed when the pages were last read */

static ctx_t *C(void) { int d = nav_depth(); return &s_ctx[d > 0 && d <= NCTX ? d - 1 : 0]; }
static ctx_t *below(void) { int d = nav_depth(); return &s_ctx[d > 1 && d <= NCTX ? d - 2 : 0]; }

extern const page_t pg_lroot, pg_lview, pg_llists, pg_lopts, pg_lpick, pg_lletter, pg_ltext, pg_lask;

static void push(const page_t *p, int kind, int vol, int sec, uint32_t first, uint32_t n, const char *name)
{
    int d = nav_depth();
    ctx_t *c = &s_ctx[d < NCTX ? d : NCTX - 1];

    memset(c, 0, sizeof *c);
    c->kind = (uint8_t)kind;
    c->vol = (uint8_t)vol;
    c->sec = (uint8_t)sec;
    c->first = first;
    c->n = n;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    nav_push(p, kind);
}

/* ---- keys ---- */

#define LIST_MOVE { KEY_UP, IG_DOWN, A_UP, "UP" }, { KEY_UP, IG_REPEAT, A_UP, NULL }, \
                  { KEY_DOWN, IG_DOWN, A_DOWN, "DOWN" }, { KEY_DOWN, IG_REPEAT, A_DOWN, NULL }
#define k_lst k_list                /* screens.c's: the same keys as its lists */
#define k_opt k_opts
#define k_ask k_question
static const keydef_t k_txt[] = {
    { KEY_UP, IG_DOWN, A_UP, "\xE2\x80\xB9" }, { KEY_UP, IG_REPEAT, A_UP, NULL },
    { KEY_DOWN, IG_DOWN, A_DOWN, "\xE2\x80\xBA" }, { KEY_DOWN, IG_REPEAT, A_DOWN, NULL },
    { KEY_SELECT, IG_TAP, A_OPEN, "TYPE" }, { KEY_SELECT, IG_HOLD, A_OPEN_HOLD, "Aa" },
    { KEY_MENU, IG_TAP, A_DONE, "DONE" },
    { KEY_BACK, IG_TAP, A_BACK, "DEL" }, { KEY_BACK, IG_HOLD, A_CANCEL, "CANCEL" },
    { 0 }
};

/* ---- helpers ---- */

static void fmt_time(char *o, int cap, uint32_t s)
{
    if (s >= 3600u)
        snprintf(o, (unsigned)cap, "%u:%02u:%02u", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u));
    else
        snprintf(o, (unsigned)cap, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

static void put(char *o, int cap, const char *s, const char *dflt)
{
    snprintf(o, (unsigned)cap, "%s", s && s[0] ? s : dflt);
}

static int rd(int vol, int sec, uint32_t i, lib_row_t *r)
{
    return idx_row(vol, sec, i, r, s_rbuf, sizeof s_rbuf);
}

static const char *vol_title(int v) { return v ? "SD card" : "Internal"; }

/* The volume the library shows: the card's when it has music (on a 512 MB player the music is on
 * the card), else Internal's. */
static int default_vol(void)
{
    if (g_lib_view[1].ok && g_lib_view[1].tr.ntracks)
        return 1;
    if (g_lib_view[0].ok && g_lib_view[0].tr.ntracks)
        return 0;
    return g_lib.mounted[1] ? 1 : 0;
}

static int playing(const fs_ref_t *r)
{
    audio_status_t st;

    audio_status(&st);
    return st.seq && st.track.ref.clus == r->clus && st.track.ref.pclus == r->pclus && st.track.ref.vol == r->vol;
}

/* ---- paths ---- */

static struct { const char *name; fs_ref_t *r; int found; } s_find;

static int find_cb(void *u, const char *name, const fs_ref_t *r)
{
    const char *a = name, *b = s_find.name;

    while (*a && ((*a | 0x20) == (*b | 0x20) || *a == *b))
        a++, b++;
    if (*a || *b || (r->flags & FS_REF_DIR))
        return 0;
    *s_find.r = *r;
    s_find.found = 1;
    return 1;
}

/* A device path ("/SD/Music/x.flac") to its file's ref: its folder scanned for the name. */
int lib_path_ref(const char *path, fs_ref_t *r)
{
    char dir[M3U_PATH];
    const char *slash = strrchr(path, '/');

    if (!slash || slash == path || (size_t)(slash - path) >= sizeof dir)
        return -1;
    memcpy(dir, path, (size_t)(slash - path));
    dir[slash - path] = 0;
    s_find.name = slash + 1;
    s_find.r = r;
    s_find.found = 0;
    fs_scan(dir, find_cb, 0);
    return s_find.found ? 0 : -1;
}

/* A track's device path from its ref: its folder's path from the index, its name from the FS. */
static uint32_t s_pclus = 0xFFFFFFFFu;
static char s_pdir[M3U_PATH];

int lib_ref_path(const fs_ref_t *r, char *out, int n)
{
    char name[FS_NAME_MAX];

    if (r->pclus != s_pclus || r->vol != (s_pdir[1] == 'S')) {
        char p[M3U_PATH - 12];

        if (!g_lib_view[r->vol].ok || idx_folder_path(r->vol, r->pclus, p, sizeof p))
            return -1;
        snprintf(s_pdir, sizeof s_pdir, "/%s%s%s", r->vol ? "SD" : "Internal", p[0] ? "/" : "", p);
        s_pclus = r->pclus;
    }
    if (fs_ref_name(r, name, sizeof name))
        return -1;
    if (snprintf(out, (unsigned)n, "%s/%s", s_pdir, name) >= n)
        return -1;
    return 0;
}

/* ---- playing ---- */

static struct {
    uint8_t vol, sec, list, from_list, mode;  /* mode: P_RANGE, P_SHUFFLE, P_GENRE */
    uint32_t first, n, start;        /* rows [first, first + n) of sec, from row `start` */
    uint32_t off, stride, total;     /* P_SHUFFLE: row i = (off + i x stride) mod total */
    uint32_t ga, gn, gk, gj, gfirst, gcount;  /* P_GENRE: albums [ga, ga + gn) of GENALB; the cursor */
} P;
enum { P_RANGE, P_SHUFFLE, P_GENRE };
static m3u_ent_t s_e;                /* one entry being read (never two at once) */
static char s_src[160];              /* the start track's folder (the resume record) */
static int s_missing;

static int get_row(int i, fs_ref_t *r, int *kind)
{
    lib_row_t x;
    uint32_t k = P.first + (uint32_t)i;
    int sec = P.sec;

    if (P.mode == P_SHUFFLE) {
        k = (uint32_t)((P.off + (uint64_t)i * P.stride) % P.total);
        sec = LS_ALBTRK;
    } else if (P.mode == P_GENRE) {      /* (asked for in order: 0, 1, 2 ...) */
        while (P.gj >= P.gcount) {
            if (P.gk >= P.gn || rd(P.vol, LS_GENALB, P.ga + P.gk++, &x))
                return -1;
            P.gfirst = x.a;
            P.gcount = x.b;
            P.gj = 0;
        }
        k = P.gfirst + P.gj++;
        sec = LS_ALBTRK;
    }
    if (rd(P.vol, sec, k, &x))
        return -1;
    *r = x.ref;
    *kind = x.kind;
    return 0;
}

static int get_entry(int i, fs_ref_t *r, int *kind)
{
    m3u_ent_t *e = &s_e;

    if (m3u_entry((int)P.first + i, e) || !(*kind = dirlist_kind(e->path)) || lib_path_ref(e->path, r)) {
        s_missing++;
        return -1;
    }
    return 0;
}

static int start_play(void)
{
    fs_ref_t r;
    int kind, rc;
    char *s;

    s_missing = 0;
    s_src[0] = 0;
    if (P.from_list) {
        if (m3u_open(P.list) < 0)
            return PLAYER_ERR_EMPTY;
        if (m3u_entry((int)(P.first + P.start), &s_e) == 0 && (s = strrchr(s_e.path, '/')) != 0) {
            *s = 0;
            snprintf(s_src, sizeof s_src, "%s", s_e.path);
        }
        rc = player_open_list((int)P.n, (int)P.start, get_entry, s_src);
        if (s_missing)
            nav_toast(ICON_WARN, TOAST_WARN, "%d not found", s_missing);
        return rc;
    }
    if (P.mode == P_RANGE && get_row((int)P.start, &r, &kind) == 0 && lib_ref_path(&r, s_src, sizeof s_src) == 0 &&
        (s = strrchr(s_src, '/')) != 0)
        *s = 0;
    P.gk = P.gj = P.gcount = 0;
    return player_open_list((int)P.n, (int)P.start, get_row, s_src);
}

/* Play rows [first, first + n) of sec from row `at` (Q_MAX of them around it). */
static void play_range(int vol, int sec, uint32_t first, uint32_t n, uint32_t at, const char *name)
{
    uint32_t lo = 0;

    if (!n)
        return;
    if (n > Q_MAX) {
        lo = at - first > 50u ? at - first - 50u : 0u;
        if (lo + Q_MAX > n)
            lo = n - Q_MAX;
        n = Q_MAX;
    }
    memset(&P, 0, sizeof P);
    P.vol = (uint8_t)vol;
    P.sec = (uint8_t)sec;
    P.first = first + lo;
    P.n = n;
    P.start = at - first - lo;
    np_play_fn(name, start_play);
}

static void play_list(int k, int at, const char *name)
{
    int n = m3u_open(k), lo = 0;

    if (n <= 0) {
        nav_toast(ICON_NOTE, TOAST_TEXT, "This list is empty");
        return;
    }
    if (n > Q_MAX) {
        lo = at > 50 ? at - 50 : 0;
        if (lo + Q_MAX > n)
            lo = n - Q_MAX;
        n = Q_MAX;
    }
    memset(&P, 0, sizeof P);
    P.from_list = 1;
    P.list = (uint8_t)k;
    P.first = (uint32_t)lo;
    P.n = (uint32_t)n;
    P.start = (uint32_t)(at - lo);
    np_play_fn(name, start_play);
}

static uint32_t s_rng = 0x2545F491u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static uint32_t gcd(uint32_t a, uint32_t b)
{
    while (b) {
        uint32_t t = a % b;

        a = b;
        b = t;
    }
    return a;
}

/* Q_MAX rows of ALBTRK at most: a genre's albums' tracks, or (genre_n < 0) any, shuffled. */
static void play_rows(int vol, uint32_t genre_first, int genre_n, const char *name)
{
    uint32_t n = 0, total = idx_rows(vol, LS_ALBTRK);
    lib_row_t r;

    memset(&P, 0, sizeof P);
    P.vol = (uint8_t)vol;
    if (genre_n < 0) {                   /* shuffle all: distinct rows, a random stride */
        if (!total)
            return;
        s_rng ^= time_ms() * 2654435761u;
        P.mode = P_SHUFFLE;
        P.total = total;
        P.off = rnd() % total;
        P.stride = 1u + rnd() % total;
        while (total > 1u && (P.stride % total == 0u || gcd(P.stride % total, total) != 1u))
            P.stride = P.stride % total + 1u;
        n = total < Q_MAX ? total : Q_MAX;
    } else {
        P.mode = P_GENRE;
        P.ga = genre_first;
        P.gn = (uint32_t)genre_n;
        for (int a = 0; a < genre_n && n < Q_MAX; a++) {
            if (rd(vol, LS_GENALB, genre_first + (uint32_t)a, &r))
                break;
            n += r.b;
        }
        if (n > Q_MAX)
            n = Q_MAX;
    }
    P.n = n;
    if (n)
        np_play_fn(name, start_play);
}

/* ---- adding to lists (a few rows now; an album's or artist's as a task, a slice a pass) ---- */

static struct {
    uint8_t on, vol, list;
    uint32_t first, n, done, added, skipped;
} A;

static int add_track(int k, const lib_row_t *r)
{
    m3u_ent_t *e = &s_e;

    memset(e, 0, sizeof *e);
    if (lib_ref_path(&r->ref, e->path, sizeof e->path))
        return -1;
    if (g_lists[k].fav && m3u_has(k, e->path))
        return 1;
    snprintf(e->title, sizeof e->title, "%s", r->s1);
    snprintf(e->artist, sizeof e->artist, "%s", r->s2);
    e->dur = r->dur ? r->dur : -1;
    return m3u_append(k, e, 1);
}

/* Rows [first, first + n) of ALBTRK to list k: at once when few, else a slice each pass. */
static void add_rows(int k, int vol, uint32_t first, uint32_t n)
{
    if (!g_lib.writable[lib_lists_vol()]) {
        nav_toast(ICON_WARN, TOAST_WARN, "Lists are read-only here");
        return;
    }
    if (A.on) {
        nav_toast(ICON_QUEUE, TOAST_TEXT, "Still adding the last ones");
        return;
    }
    memset(&A, 0, sizeof A);
    A.on = 1;
    A.vol = (uint8_t)vol;
    A.list = (uint8_t)k;
    A.first = first;
    A.n = n > M3U_MAX ? M3U_MAX : n;
    lib_add_step(n <= 12u ? 1000000u : 3000u);
}

/* The adding task's slice (lib_job.c calls it every pass while it runs): 1 when it is done. */
int lib_add_step(uint32_t us)
{
    uint64_t t_end = lib_now_us() + us;
    lib_row_t r;

    if (!A.on)
        return 1;
    while (A.done < A.n && lib_now_us() < t_end) {
        int rc;

        if (rd(A.vol, LS_ALBTRK, A.first + A.done++, &r))
            continue;
        rc = add_track(A.list, &r);
        if (rc == 0)
            A.added++;
        else if (rc == 1)
            A.skipped++;
        else
            break;
    }
    if (A.done < A.n && A.added + A.skipped == A.done)
        return 0;
    A.on = 0;
    if (A.added + A.skipped < A.n)
        nav_toast(ICON_WARN, TOAST_WARN, "Added %u, %u could not be", (unsigned)A.added, (unsigned)(A.n - A.added - A.skipped));
    else if (A.added)
        nav_toast(ICON_CHECK, TOAST_TEXT, "Added %u to %s", (unsigned)A.added, g_lists[A.list].name);
    else
        nav_toast(ICON_CHECK, TOAST_TEXT, "Already in %s", g_lists[A.list].name);
    return 1;
}

int lib_adding(void) { return A.on; }

/* ---- Library ---- */

enum { R_STATUS, R_SONGS, R_ALBUMS, R_ARTISTS, R_GENRES, R_FAVS, R_LISTS, R_SHUFFLE, R_SOURCE, R_REFRESH, R_N };

static int root_ids(uint8_t *ids)
{
    int n = 0, v = s_vol, ok = g_lib_view[v].ok && g_lib_view[v].tr.ntracks, busy, bv;

    busy = lib_busy(&bv, 0);
    if (!ok || busy)
        ids[n++] = R_STATUS;
    if (ok) {
        ids[n++] = R_SONGS;
        ids[n++] = R_ALBUMS;
        ids[n++] = R_ARTISTS;
        if (g_lib_view[v].tr.ngenres > 1 || (g_lib_view[v].tr.ngenres == 1 && idx_rows(v, LS_GENRES)))
            ids[n++] = R_GENRES;
    }
    ids[n++] = R_FAVS;
    ids[n++] = R_LISTS;
    if (ok)
        ids[n++] = R_SHUFFLE;
    if (g_lib_view[0].ok && g_lib_view[0].tr.ntracks && g_lib_view[1].ok && g_lib_view[1].tr.ntracks)
        ids[n++] = R_SOURCE;
    ids[n++] = R_REFRESH;
    return n;
}

static int root_count(void)
{
    uint8_t ids[R_N];

    if (g_lib.changed != s_changed) {   /* (an index came or went: the lists again) */
        s_changed = g_lib.changed;
        if (!(g_lib_view[s_vol].ok && g_lib_view[s_vol].tr.ntracks))
            s_vol = default_vol();
        m3u_lists();
    }
    return root_ids(ids);
}

static void root_row(int i, row_t *r)
{
    static const char *const names[R_N] = { "Indexing", "All songs", "Albums", "Artists", "Genres", "Favourites",
                                            "Playlists", "Shuffle all", "Source", "Refresh library" };
    static const uint8_t icons[R_N] = { ICON_UPDATE, ICON_NOTE, ICON_ALBUM, ICON_ARTIST, ICON_GENRE, ICON_CHECK,
                                        ICON_LIBRARY, ICON_SHUFFLE, ICON_SD, ICON_UPDATE };
    uint8_t ids[R_N];
    const lib_trailer_t *t = &g_lib_view[s_vol].tr;
    int id, bv, pct;

    root_ids(ids);
    id = ids[i];
    r->id = (uint16_t)id;
    r->icon = icons[id];
    r->flags = id == R_SHUFFLE || id == R_REFRESH || id == R_SOURCE || id == R_STATUS ? 0 : R_CHEV;
    snprintf(r->text, sizeof r->text, "%s", names[id]);
    switch (id) {
    case R_STATUS:
        if (lib_busy(&bv, &pct)) {
            snprintf(r->text, sizeof r->text, "Indexing %s", vol_title(bv));
            snprintf(r->value, sizeof r->value, "%d %%", pct);
        } else {
            snprintf(r->text, sizeof r->text, "%s", !g_lib.mounted[s_vol] ? "No SD card" : "No music found yet");
            r->flags = R_OFF;
        }
        break;
    case R_SONGS:   snprintf(r->value, sizeof r->value, "%u", (unsigned)t->ntracks); break;
    case R_ALBUMS:  snprintf(r->value, sizeof r->value, "%u", (unsigned)t->nalbums); break;
    case R_ARTISTS: snprintf(r->value, sizeof r->value, "%u", (unsigned)t->nartists); break;
    case R_GENRES:  snprintf(r->value, sizeof r->value, "%u", (unsigned)t->ngenres); break;
    case R_FAVS:
        if (g_nlists)
            snprintf(r->value, sizeof r->value, "%s", g_lists[0].size ? "" : "none yet");
        break;
    case R_LISTS:   snprintf(r->value, sizeof r->value, "%d", g_nlists > 0 ? g_nlists - 1 : 0); break;
    case R_SOURCE:  snprintf(r->value, sizeof r->value, "%s", vol_title(s_vol)); break;
    }
}

static void root_title(char *o, int cap)
{
    snprintf(o, (unsigned)cap, "Library" DOT "%s", vol_title(s_vol));
}

static void root_act(int a, int i)
{
    uint8_t ids[R_N];
    const lib_trailer_t *t = &g_lib_view[s_vol].tr;

    if (a != A_OPEN && a != A_OPEN_HOLD)
        return;
    if (i >= root_ids(ids))
        return;
    switch (ids[i]) {
    case R_SONGS:
        if (a == A_OPEN_HOLD)
            play_range(s_vol, LS_SONGS, 0, t->ntracks, 0, "All songs");
        else
            push(&pg_lview, V_SONGS, s_vol, LS_SONGS, 0, idx_rows(s_vol, LS_SONGS), "All songs");
        break;
    case R_ALBUMS:  push(&pg_lview, V_ALBUMS, s_vol, LS_ALBUMS, 0, idx_rows(s_vol, LS_ALBUMS), "Albums"); break;
    case R_ARTISTS: push(&pg_lview, V_ARTISTS, s_vol, LS_ARTISTS, 0, idx_rows(s_vol, LS_ARTISTS), "Artists"); break;
    case R_GENRES:  push(&pg_lview, V_GENRES, s_vol, LS_GENRES, 0, idx_rows(s_vol, LS_GENRES), "Genres"); break;
    case R_FAVS:
        if (a == A_OPEN_HOLD) {
            play_list(0, 0, "Favourites");
        } else {
            m3u_lists();
            m3u_open(0);
            push(&pg_lview, V_LIST, s_vol, 0, 0, 0, "Favourites");
            C()->list = 0;
        }
        break;
    case R_LISTS:   m3u_lists(); push(&pg_llists, V_LISTS, s_vol, 0, 0, 0, "Playlists"); break;
    case R_SHUFFLE: play_rows(s_vol, 0, -1, "Shuffle all"); break;
    case R_SOURCE:
        s_vol = !s_vol;
        list_reload();
        break;
    case R_REFRESH:
        lib_request(s_vol, 1);
        nav_toast(ICON_UPDATE, TOAST_TEXT, "Reading every file of %s again", vol_title(s_vol));
        list_reload();
        break;
    }
}

static const char *root_verb(int i, int hold)
{
    uint8_t ids[R_N];
    int id = i < root_ids(ids) ? ids[i] : R_STATUS;

    if (hold)
        return id == R_SONGS || id == R_FAVS ? "PLAY ALL" : "";
    return id == R_SHUFFLE ? "PLAY" : id == R_REFRESH ? "REFRESH" : id == R_SOURCE ? "CHANGE" : id == R_STATUS ? "" : "OPEN";
}

static void root_enter(int first)
{
    if (first) {
        s_vol = default_vol();
        m3u_lists();
        s_changed = g_lib.changed;
    }
}

const page_t pg_lroot = {
    .name = "library", .kind = PK_LIST, .icon = ICON_LIBRARY, .live = 1, .keys = k_lst, .title_fn = root_title,
    .count = root_count, .row = root_row, .act = root_act, .enter = root_enter, .verb = root_verb,
};

/* ---- the views: a range of an index section, or a list's entries ---- */

static int view_count(void)
{
    ctx_t *c = C();

    if (c->kind == V_LIST)
        return m3u_count();
    if (c->kind == V_ARTIST)
        return (int)c->n + 1;            /* "All songs", then the albums */
    return (int)c->n;
}

/* row i of the page as an index row (its section and number), -1 for V_ARTIST's first row */
static int view_rd(ctx_t *c, int i, lib_row_t *r, int *sec)
{
    uint32_t k = c->first + (uint32_t)i;

    *sec = c->sec;
    if (c->kind == V_ARTIST) {
        if (i == 0)
            return -1;
        k--;
    }
    return rd(c->vol, *sec, k, r) ? -2 : 0;
}

static void view_row(int i, row_t *r)
{
    ctx_t *c = C();
    lib_row_t x;
    int sec, rc;
    char v[16];

    r->id = (uint16_t)i;
    if (c->kind == V_LIST) {
        if (m3u_entry(i, &s_e))
            return;
        r->icon = ICON_NOTE;
        put(r->text, sizeof r->text, s_e.title, "?");
        put(r->value, sizeof r->value, s_e.artist, "");
        return;
    }
    rc = view_rd(c, i, &x, &sec);
    if (rc == -1) {
        r->icon = ICON_NOTE;
        r->flags = R_CHEV;
        snprintf(r->text, sizeof r->text, "All songs");
        snprintf(r->value, sizeof r->value, "%u", (unsigned)c->d);
        return;
    }
    if (rc)
        return;
    switch (sec) {
    case LS_ALBTRK:
    case LS_SONGS:
        put(r->text, sizeof r->text, x.s1, "?");
        if (c->kind == V_ALBUM) {
            if (x.trk) {
                r->flags = R_NUM;
                r->num = x.trk;
            }
            fmt_time(v, sizeof v, x.dur);
            snprintf(r->value, sizeof r->value, "%s", x.dur ? v : "");
        } else {
            r->icon = ICON_NOTE;
            put(r->value, sizeof r->value, x.s2, "");
        }
        if (playing(&x.ref))
            r->flags = (uint8_t)((r->flags & ~R_NUM) | R_PLAYING);
        break;
    case LS_ALBUMS:
    case LS_ARTALB:
    case LS_GENALB:
        r->icon = ICON_ALBUM;
        r->flags = R_CHEV;
        put(r->text, sizeof r->text, x.s1, "Unknown album");
        if (c->kind == V_ARTIST && x.year)
            snprintf(r->value, sizeof r->value, "%u", x.year);
        else
            put(r->value, sizeof r->value, c->kind == V_ARTIST ? "" : x.s2, c->kind == V_ARTIST ? "" : "Unknown artist");
        break;
    case LS_ARTISTS:
        r->icon = ICON_ARTIST;
        r->flags = R_CHEV;
        put(r->text, sizeof r->text, x.s1, "Unknown artist");
        snprintf(r->value, sizeof r->value, x.d == 1 ? "%u song%s" : "%u albums", x.d == 1 ? (unsigned)x.b : (unsigned)x.d,
                 x.b == 1 ? "" : "s");
        break;
    case LS_GENRES:
        r->icon = ICON_GENRE;
        r->flags = R_CHEV;
        put(r->text, sizeof r->text, x.s1, "Unknown genre");
        snprintf(r->value, sizeof r->value, "%u album%s", (unsigned)x.d, x.d == 1 ? "" : "s");
        break;
    }
}

static void view_title(char *o, int cap)
{
    snprintf(o, (unsigned)cap, "%s", C()->name);
}

static void open_row(ctx_t *c, int i, int hold)
{
    lib_row_t x;
    int sec, rc = view_rd(c, i, &x, &sec);
    char nm[48];

    if (c->kind == V_LIST) {
        play_list(c->list, i, c->name);
        return;
    }
    if (rc == -1) {                      /* an artist's "All songs" */
        if (hold)
            play_range(c->vol, LS_ALBTRK, c->a_first, c->d, c->a_first, c->name);
        else
            push(&pg_lview, V_ASONGS, c->vol, LS_ALBTRK, c->a_first, c->d, c->name);
        return;
    }
    if (rc)
        return;
    snprintf(nm, sizeof nm, "%s", x.s1[0] ? x.s1 : sec == LS_ARTISTS ? "Unknown artist" : sec == LS_GENRES ?
             "Unknown genre" : "Unknown album");
    switch (sec) {
    case LS_ALBTRK:
        play_range(c->vol, LS_ALBTRK, c->first, c->n, c->first + (uint32_t)i - (c->kind == V_ARTIST), c->name);
        break;
    case LS_SONGS:
        play_range(c->vol, LS_SONGS, c->first, c->n, c->first + (uint32_t)i, "All songs");
        break;
    case LS_ALBUMS:
    case LS_ARTALB:
    case LS_GENALB:
        if (hold)
            play_range(c->vol, LS_ALBTRK, x.a, x.b, x.a, nm);
        else
            push(&pg_lview, V_ALBUM, c->vol, LS_ALBTRK, x.a, x.b, nm);
        break;
    case LS_ARTISTS:
        if (hold) {
            play_range(c->vol, LS_ALBTRK, x.a, x.b, x.a, nm);
        } else {
            push(&pg_lview, V_ARTIST, c->vol, LS_ARTALB, x.c, x.d, nm);
            C()->a_first = x.a;
            C()->d = x.b;
        }
        break;
    case LS_GENRES:
        if (hold)
            play_rows(c->vol, x.c, (int)x.d, nm);
        else
            push(&pg_lview, V_GENRE, c->vol, LS_GENALB, x.c, x.d, nm);
        break;
    }
}

static void view_act(int a, int i)
{
    if ((a == A_OPEN || a == A_OPEN_HOLD) && i < view_count())
        open_row(C(), i, a == A_OPEN_HOLD);
}

static const char *view_verb(int i, int hold)
{
    ctx_t *c = C();

    if (i >= view_count())
        return "";
    if (c->kind == V_LIST || c->kind == V_ALBUM || c->kind == V_SONGS || c->kind == V_ASONGS)
        return hold ? "" : "PLAY";
    return hold ? "PLAY ALL" : "OPEN";
}

static void view_tick(void)
{
    ctx_t *c = C();

    c->cur = nav_cursor();
    if (g_lib.changed != s_changed && c->kind != V_LIST) {
        int k = nav_find(&pg_lroot);

        s_changed = g_lib.changed;      /* (a new index under the page: back to Library) */
        if (k >= 0)
            nav_pop_to(k + 1);
    }
}

static void view_enter(int first)
{
    ctx_t *c = C();

    if (c->kind == V_LIST && !first)
        m3u_open(c->list);
    if (first && c->kind == V_ALBUM) {   /* the playing track's row */
        lib_row_t x;
        int sec;

        for (int i = 0; i < (int)c->n && i < 400; i++)
            if (view_rd(c, i, &x, &sec) == 0 && playing(&x.ref)) {
                nav_set_cursor(i);
                break;
            }
    }
}

const page_t pg_lview = {
    .name = "libview", .kind = PK_LIST, .icon = ICON_LIBRARY, .keys = k_lst, .title_fn = view_title,
    .count = view_count, .row = view_row, .act = view_act, .enter = view_enter, .tick = view_tick,
    .verb = view_verb, .options = &pg_lopts,
};

/* ---- Playlists ---- */

static int lists_count(void) { return g_nlists; }    /* "New playlist", then g_lists[1..] */

static void lists_row(int i, row_t *r)
{
    r->id = (uint16_t)i;
    if (i == 0) {
        r->icon = ICON_LIBRARY;
        snprintf(r->text, sizeof r->text, "New playlist");
        snprintf(r->value, sizeof r->value, "%s", g_lib.writable[lib_lists_vol()] ? "" : "read-only");
        if (!g_lib.writable[lib_lists_vol()])
            r->flags = R_OFF;
        return;
    }
    r->icon = ICON_QUEUE;
    r->flags = R_CHEV;
    snprintf(r->text, sizeof r->text, "%s", g_lists[i].name);
}

static void list_open(int k)
{
    m3u_open(k);
    push(&pg_lview, V_LIST, s_vol, 0, 0, 0, g_lists[k].name);
    C()->list = (uint8_t)k;
}

static void lists_act(int a, int i)
{
    if (i >= g_nlists)
        return;
    if (i == 0) {                        /* a new one: named at once, Rename names it again */
        int k;

        if (a != A_OPEN)
            return;
        if ((k = m3u_create(0)) < 0) {
            nav_toast(ICON_WARN, TOAST_WARN, "Cannot make a playlist here (%d)", k);
            return;
        }
        list_reload();
        nav_set_cursor(k);
        nav_toast(ICON_CHECK, TOAST_TEXT, "%s made", g_lists[k].name);
        return;
    }
    if (a == A_OPEN_HOLD)
        play_list(i, 0, g_lists[i].name);
    else if (a == A_OPEN)
        list_open(i);
}

static const char *lists_verb(int i, int hold) { return i == 0 ? (hold ? "" : "MAKE") : hold ? "PLAY ALL" : "OPEN"; }
static void lists_tick(void) { C()->cur = nav_cursor(); }
static void lists_enter(int first)
{
    if (!first)
        m3u_lists();
}

const page_t pg_llists = {
    .name = "playlists", .kind = PK_LIST, .icon = ICON_LIBRARY, .keys = k_lst, .title = "Playlists",
    .count = lists_count, .row = lists_row, .act = lists_act, .enter = lists_enter, .tick = lists_tick,
    .verb = lists_verb, .options = &pg_lopts,
};

/* ---- what an add or an option is about ---- */

static struct {
    uint8_t what, vol;               /* AB_*: the rows of ALBTRK [first, first + n), or one track by path */
    uint32_t first, n;
    m3u_ent_t one;                   /* AB_PATH: Now Playing's track, a list's entry */
    char name[48];                   /* a new playlist's name when made from it */
} B;
enum { AB_NONE, AB_ROWS, AB_PATH };

static void add_to(int k)
{
    if (B.what == AB_ROWS) {
        add_rows(k, B.vol, B.first, B.n);
    } else if (B.what == AB_PATH) {
        int rc = g_lists[k].fav && m3u_has(k, B.one.path) ? 1 : m3u_append(k, &B.one, 1);

        nav_toast(rc ? ICON_WARN : ICON_CHECK, rc ? TOAST_WARN : TOAST_TEXT, rc == 1 ? "Already in %s" :
                  rc ? "Could not add to %s" : "Added to %s", g_lists[k].name);
    }
}

/* The options' subject: the row under the cursor of the page below. 0 when there is none. */
enum { O_PLAY, O_SHUF, O_FAV, O_UNFAV, O_ADDPL, O_REMOVE, O_UP, O_DOWN, O_RENAME, O_DELETE, O_LETTER, O_N };
static uint8_t s_oids[O_N], s_on;
static lib_row_t s_orow;
static int s_osec;

static int subject(void)
{
    ctx_t *b = below();
    int i = b->cur, rc;

    memset(&B, 0, sizeof B);
    s_osec = -1;
    if (b->kind == V_LISTS) {
        if (i <= 0 || i >= g_nlists)
            return 0;
        snprintf(B.name, sizeof B.name, "%s", g_lists[i].name);
        return 1;
    }
    if (b->kind == V_LIST) {
        if (m3u_entry(i, &B.one))
            return 0;
        B.what = AB_PATH;
        return 1;
    }
    if (b->kind == V_ROOT)
        return 0;
    rc = view_rd(b, i, &s_orow, &s_osec);
    if (rc == -1) {                      /* an artist's All songs */
        B.what = AB_ROWS;
        B.vol = b->vol;
        B.first = b->a_first;
        B.n = b->d;
        snprintf(B.name, sizeof B.name, "%s", b->name);
        s_osec = LS_ARTISTS;
        return 1;
    }
    if (rc)
        return 0;
    B.vol = b->vol;
    B.what = AB_ROWS;
    switch (s_osec) {
    case LS_ALBTRK:
    case LS_SONGS:
        B.first = s_orow.a;
        B.n = 1;
        snprintf(B.name, sizeof B.name, "%s", s_orow.s1);
        break;
    case LS_ALBUMS:
    case LS_ARTALB:
    case LS_GENALB:
    case LS_ARTISTS:
        B.first = s_orow.a;
        B.n = s_orow.b;
        snprintf(B.name, sizeof B.name, "%s", s_orow.s1);
        break;
    default:
        B.what = AB_NONE;
    }
    return 1;
}

static void opt_build(void)
{
    ctx_t *b = below();
    int ok = subject(), lists = g_lib.writable[lib_lists_vol()];

    s_on = 0;
    if (!ok) {
        if (b->kind == V_SONGS || b->kind == V_ALBUMS || b->kind == V_ARTISTS || b->kind == V_GENRES)
            s_oids[s_on++] = O_LETTER;
        return;
    }
    if (b->kind == V_LISTS) {
        s_oids[s_on++] = O_PLAY;
        s_oids[s_on++] = O_SHUF;
        if (lists) {
            s_oids[s_on++] = O_RENAME;
            s_oids[s_on++] = O_DELETE;
        }
        return;
    }
    if (b->kind == V_LIST) {
        if (lists) {
            if (b->list != 0)
                s_oids[s_on++] = O_FAV;
            s_oids[s_on++] = O_ADDPL;
            s_oids[s_on++] = O_REMOVE;
            if (b->cur > 0)
                s_oids[s_on++] = O_UP;
            if (b->cur + 1 < m3u_count())
                s_oids[s_on++] = O_DOWN;
        }
        return;
    }
    if (s_osec != LS_ALBTRK && s_osec != LS_SONGS) {
        s_oids[s_on++] = O_PLAY;
        s_oids[s_on++] = O_SHUF;
    }
    if (lists && B.what == AB_ROWS) {
        int fav = 0;

        if (B.n == 1)                    /* a track: in the favourites already? */
            fav = lib_ref_path(&s_orow.ref, s_e.path, sizeof s_e.path) == 0 && m3u_has(0, s_e.path);
        s_oids[s_on++] = fav ? O_UNFAV : O_FAV;
        s_oids[s_on++] = O_ADDPL;
    }
    if (b->kind == V_SONGS || b->kind == V_ALBUMS || b->kind == V_ARTISTS || b->kind == V_GENRES)
        s_oids[s_on++] = O_LETTER;
}

static int opt_count(void) { return s_on; }

static void opt_row(int i, row_t *r)
{
    static const char *const names[O_N] = { "Play", "Shuffle", "Add to favourites", "Remove from favourites",
                                            "Add to playlist", "Remove from list", "Move up", "Move down",
                                            "Rename", "Delete playlist", "Jump to letter" };
    static const uint8_t icons[O_N] = { ICON_PLAY, ICON_SHUFFLE, ICON_CHECK, ICON_CHECK, ICON_QUEUE, ICON_WARN,
                                        ICON_PREV, ICON_NEXT, ICON_LETTER, ICON_WARN, ICON_LETTER };
    int id = s_oids[i < s_on ? i : 0];

    r->id = (uint16_t)id;
    r->icon = icons[id];
    snprintf(r->text, sizeof r->text, "%s", names[id]);
    if (id == O_ADDPL || id == O_RENAME || id == O_LETTER)
        r->flags = R_CHEV;
    if (id == O_FAV && B.what == AB_ROWS && B.n > 1)
        snprintf(r->value, sizeof r->value, "%u songs", (unsigned)B.n);
}

static void opt_title(char *o, int cap)
{
    snprintf(o, (unsigned)cap, B.name[0] ? "Options" DOT "%s" : "Options", B.name);
}

static int fav_index(const char *path)   /* its entry in the favourites, -1 */
{
    char want[M3U_PATH];
    int n = m3u_open(0);

    snprintf(want, sizeof want, "%s", path);
    for (int i = 0; i < n; i++)
        if (m3u_entry(i, &s_e) == 0 && !strcmp(s_e.path, want))
            return i;
    return -1;
}

static void opt_act(int a, int i)
{
    ctx_t *b = below();
    int id;

    if (a != A_OPEN || i >= s_on)
        return;
    id = s_oids[i];
    switch (id) {
    case O_PLAY:
    case O_SHUF:
        if (id == O_SHUF && queue_mode() != Q_SHUFFLE) {
            queue_set_mode(Q_SHUFFLE);   /* (as the browser's Shuffle folder: the play mode) */
            g_cfg.play = Q_SHUFFLE;
            config_save_later();
        }
        nav_pop();
        if (b->kind == V_LISTS)
            play_list(b->cur, 0, g_lists[b->cur].name);
        else if (s_osec == LS_GENRES)
            play_rows(b->vol, s_orow.c, (int)s_orow.d, B.name);
        else if (b->kind == V_LIST)
            play_list(b->list, 0, b->name);
        else
            play_range(B.vol, LS_ALBTRK, B.first, B.n, B.first, B.name);
        break;
    case O_FAV:
        nav_pop();
        add_to(0);
        break;
    case O_UNFAV: {
        int k;

        nav_pop();
        if (lib_ref_path(&s_orow.ref, s_e.path, sizeof s_e.path) == 0 && (k = fav_index(s_e.path)) >= 0 && m3u_remove(0, k) == 0)
            nav_toast(ICON_CHECK, TOAST_TEXT, "Removed from favourites");
        else
            nav_toast(ICON_WARN, TOAST_WARN, "Could not remove it");
        break;
    }
    case O_ADDPL:
        push(&pg_lpick, V_PICK, b->vol, 0, 0, 0, "Add to playlist");
        break;
    case O_REMOVE:
    case O_UP:
    case O_DOWN: {
        int k = b->list, cur = b->cur, rc;

        nav_pop();
        rc = id == O_REMOVE ? m3u_remove(k, cur) : m3u_move(k, cur, id == O_UP ? -1 : 1);
        list_reload();
        if (rc)
            nav_toast(ICON_WARN, TOAST_WARN, "Could not change the list (%d)", rc);
        else if (id != O_REMOVE)
            nav_set_cursor(cur + (id == O_UP ? -1 : 1));
        break;
    }
    case O_RENAME:
        push(&pg_ltext, V_TEXT, b->vol, 0, (uint32_t)b->cur, 0, g_lists[b->cur].name);
        break;
    case O_DELETE:
        push(&pg_lask, V_ASK, b->vol, 0, (uint32_t)b->cur, 0, g_lists[b->cur].name);
        break;
    case O_LETTER:
        push(&pg_lletter, V_LETTER, b->vol, b->sec, 0, 0, "Jump to letter");
        break;
    }
}

static void opt_enter(int first)
{
    if (first)
        opt_build();
}

const page_t pg_lopts = {
    .name = "liboptions", .kind = PK_LIST, .icon = ICON_GEAR, .keys = k_opt, .title_fn = opt_title,
    .count = opt_count, .row = opt_row, .act = opt_act, .enter = opt_enter,
};

/* ---- the playlist picker ---- */

static int pick_count(void) { return g_nlists; }    /* "New playlist", then the playlists */

static void pick_row(int i, row_t *r)
{
    r->id = (uint16_t)i;
    r->icon = i ? ICON_QUEUE : ICON_LIBRARY;
    snprintf(r->text, sizeof r->text, "%s", i ? g_lists[i].name : "New playlist");
    if (!i && B.name[0] && B.what == AB_ROWS && B.n > 1)
        snprintf(r->value, sizeof r->value, "%s", B.name);
}

static void pick_act(int a, int i)
{
    int k = i;

    if (a != A_OPEN || i >= g_nlists)
        return;
    if (i == 0 && (k = m3u_create(B.what == AB_ROWS && B.n > 1 ? B.name : 0)) < 0) {
        nav_toast(ICON_WARN, TOAST_WARN, "Cannot make a playlist here (%d)", k);
        return;
    }
    nav_pop_to(nav_depth() - 2);        /* the picker and the options */
    add_to(k);
}

static void pick_enter(int first)
{
    if (first)
        m3u_lists();
}

const page_t pg_lpick = {
    .name = "libpick", .kind = PK_LIST, .icon = ICON_QUEUE, .keys = k_opt, .title = "Add to playlist",
    .count = pick_count, .row = pick_row, .act = pick_act, .enter = pick_enter,
};

/* ---- the letter strip ---- */

static int let_count(void) { return LIB_LETTERS; }

static void let_row(int i, row_t *r)
{
    ctx_t *c = below();
    uint32_t at = idx_letter(c->vol, c->sec, i), next = i + 1 < LIB_LETTERS ? idx_letter(c->vol, c->sec, i + 1) : c->n;

    r->id = (uint16_t)i;
    r->icon = ICON_LETTER;
    if (i == 0)
        snprintf(r->text, sizeof r->text, "# 0-9");
    else if (i == LIB_LETTERS - 1)
        snprintf(r->text, sizeof r->text, "Other scripts");
    else
        snprintf(r->text, sizeof r->text, "%c", 'A' + i - 1);
    if (next > at)
        snprintf(r->value, sizeof r->value, "%u", (unsigned)(next - at));
    else
        r->flags = R_OFF;
}

static void let_act(int a, int i)
{
    ctx_t *c = &s_ctx[nav_depth() >= 3 ? nav_depth() - 3 : 0];   /* the list, under the options */
    uint32_t at;

    if (a != A_OPEN)
        return;
    at = idx_letter(c->vol, c->sec, i);
    if (at >= c->n) {
        nav_toast(ICON_LETTER, TOAST_TEXT, "Nothing there");
        return;
    }
    nav_pop_to(nav_depth() - 2);
    nav_set_cursor((int)at);
}

static void let_enter(int first)
{
    if (first) {                         /* below() is the options: the list is under them */
        ctx_t *list = &s_ctx[nav_depth() >= 3 ? nav_depth() - 3 : 0];

        s_ctx[nav_depth() - 2] = *list;
        for (int i = 0; i < LIB_LETTERS; i++)   /* the cursor on the list's own letter */
            if (idx_letter(list->vol, list->sec, i) > (uint32_t)list->cur) {
                nav_set_cursor(i > 0 ? i - 1 : 0);
                break;
            }
    }
}

const page_t pg_lletter = {
    .name = "libletter", .kind = PK_LIST, .icon = ICON_LETTER, .keys = k_opt, .title = "Jump to letter",
    .count = let_count, .row = let_row, .act = let_act, .enter = let_enter,
};

/* ---- the name entry ---- */

static const char k_chars[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-'&!?.,()#+";
#define NCHARS ((int)sizeof k_chars - 1)
static struct { char s[32]; int ch, lower; } X;

static void txt_draw(void)
{
    const theme_t *t = T;
    char line[40];

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    ui_text(&font_b13, 16, 44, 288, 0, "Name", t->text_dim, 30, 48);
    snprintf(line, sizeof line, "%s_", X.s);
    ui_frame_rect(12, 52, 296, 30, t->divider);
    ui_text(&font_t17, 20, 74, 280, 0, line, t->text, 54, 80);
    for (int k = -4; k <= 4; k++) {     /* the letters, the chosen one in the middle */
        int i = (X.ch + k + NCHARS) % NCHARS, x = 160 + k * 32 - 14;
        char c[4] = { k_chars[i], 0 };

        if (c[0] == ' ')
            memcpy(c, "\xE2\x90\xA3", 4);       /* (the space as a visible sign) */
        else if (X.lower && c[0] >= 'A' && c[0] <= 'Z')
            c[0] = (char)(c[0] + 32);
        if (!k)
            ui_fill(x, 96, 28, 34, t->sel_bg);
        ui_text(k ? &font_m13 : &font_t17, x, 120, 28, 1, c, k ? t->text_dim : t->sel_text, 96, 129);
    }
    ui_text(&font_s8, 0, 150, DISPLAY_W, 1, "VOL MOVE" DOT "PLAY TYPE" DOT "HOLD Aa" DOT "BACK DEL" DOT "MENU DONE",
            t->text_dim, 140, 152);
}

static void txt_act(int a, int i)
{
    int n = (int)strlen(X.s);

    switch (a) {
    case A_UP:
    case A_DOWN: {
        int st = g_ev && g_ev->gesture == IG_REPEAT ? g_ev->steps : 1;

        X.ch = (X.ch + (a == A_DOWN ? st : -st) % NCHARS + NCHARS) % NCHARS;
        ui_invalidate(0, 94, DISPLAY_W - 1, 131);
        return;
    }
    case A_OPEN:
        if (n < (int)sizeof X.s - 1) {
            char c = k_chars[X.ch];

            X.s[n] = X.lower && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
            X.s[n + 1] = 0;
            if (n == 0 && c != ' ')
                X.lower = 1;             /* (after a capital: small letters, as names go) */
        }
        break;
    case A_OPEN_HOLD:
        X.lower = !X.lower;
        break;
    case A_BACK:
        if (!n) {
            nav_pop();
            return;
        }
        while (n > 0 && ((uint8_t)X.s[n - 1] & 0xC0u) == 0x80u)
            n--;                         /* (a whole character) */
        X.s[n > 0 ? n - 1 : 0] = 0;
        if (!X.s[0])
            X.lower = 0;
        break;
    case A_CANCEL:
        nav_pop();
        return;
    case A_DONE: {
        ctx_t *c = C();
        int k = (int)c->first, rc;
        char nm[32];

        while (n && X.s[n - 1] == ' ')
            X.s[--n] = 0;
        snprintf(nm, sizeof nm, "%s", X.s);
        nav_pop_to(nav_depth() - 2);    /* the entry and the options */
        if (!nm[0])
            return;
        rc = m3u_rename(k, nm);
        m3u_lists();
        list_reload();
        if (rc)
            nav_toast(ICON_WARN, TOAST_WARN, "Could not rename it (%d)", rc);
        else
            nav_toast(ICON_CHECK, TOAST_TEXT, "Renamed: %s", nm);
        return;
    }
    default:
        return;
    }
    ui_invalidate(0, 50, DISPLAY_W - 1, 131);
}

static void txt_enter(int first)
{
    if (first) {
        snprintf(X.s, sizeof X.s, "%s", C()->name);
        X.ch = 1;
        X.lower = X.s[0] != 0;
    }
}

static void txt_title(char *o, int cap) { snprintf(o, (unsigned)cap, "Rename playlist"); }

const page_t pg_ltext = {
    .name = "libname", .kind = PK_CUSTOM, .icon = ICON_LETTER, .keys = k_txt, .title_fn = txt_title,
    .act = txt_act, .enter = txt_enter, .draw = txt_draw,
};

/* ---- delete? ---- */

static int ask_count(void) { return 2; }
static void ask_row(int i, row_t *r)
{
    r->id = (uint16_t)i;
    r->icon = i ? ICON_CHECK : ICON_WARN;
    snprintf(r->text, sizeof r->text, "%s", i ? "Keep it" : "Delete it");
}
static void ask_draw(void)
{
    const theme_t *t = T;
    char l1[64];

    snprintf(l1, sizeof l1, "Delete %s?", C()->name);
    ui_fill(0, BODY_Y, DISPLAY_W, 43, t->background);
    ui_icon(ICON_WARN, 10, 28, t->warn);
    ui_text(&font_m13, 28, 38, 272, 0, l1, t->warn, 24, 42);
    ui_text(&font_b13, 28, 56, 272, 0, "The songs stay; only the list goes", t->text_dim, 42, 61);
    ui_fill(0, 63, DISPLAY_W, 1, t->divider);
}
static void ask_enter(int first)
{
    if (first)
        nav_set_cursor(1);
}
static void ask_act(int a, int i)
{
    int k = (int)C()->first;

    if (a != A_OPEN && a != A_SAFE)
        return;
    nav_pop_to(nav_depth() - 2);        /* the question and the options */
    if (a == A_OPEN && i == 0) {
        int rc = m3u_delete(k);

        m3u_lists();
        list_reload();
        nav_toast(rc ? ICON_WARN : ICON_CHECK, rc ? TOAST_WARN : TOAST_TEXT, rc ? "Could not delete it" : "Deleted");
    }
}
static const char *ask_verb(int i, int hold) { return hold ? "" : "KEEP"; }

const page_t pg_lask = {
    .name = "libdelete", .kind = PK_QUESTION, .icon = ICON_WARN, .keys = k_ask, .title = "Delete playlist",
    .count = ask_count, .row = ask_row, .act = ask_act, .enter = ask_enter, .draw = ask_draw, .verb = ask_verb,
};

/* ---- the ways in (startup/ap_ovl.c's gates) ---- */

void lib_ui_open(void)
{
    s_ctx[nav_depth() < NCTX ? nav_depth() : NCTX - 1].kind = V_ROOT;
    nav_push(&pg_lroot, 0);
}

int lib_ui_home_value(char *out, int n)
{
    int v = default_vol(), bv, pct;

    out[0] = 0;
    if (lib_busy(&bv, &pct))
        snprintf(out, (unsigned)n, "indexing %d %%", pct);
    else if (g_lib_view[v].ok && g_lib_view[v].tr.ntracks)
        snprintf(out, (unsigned)n, "%u songs", (unsigned)g_lib_view[v].tr.ntracks);
    return 0;
}

/* Now Playing's track as a list entry: its path (from the index, else the queue's folder). */
static int np_entry(m3u_ent_t *e)
{
    audio_status_t st;
    track_info_t ti;
    char name[FS_NAME_MAX];

    memset(e, 0, sizeof *e);
    audio_status(&st);
    if (!st.seq)
        return -1;
    if (lib_ref_path(&st.track.ref, e->path, sizeof e->path)) {
        const char *src = player_source();

        if (!src || src[0] != '/' || fs_ref_name(&st.track.ref, name, sizeof name) ||
            snprintf(e->path, sizeof e->path, "%s/%s", src, name) >= (int)sizeof e->path)
            return -1;
        {
            fs_ref_t r;              /* (a queue of a whole tree: the source is not its folder) */

            if (lib_path_ref(e->path, &r) || r.clus != st.track.ref.clus)
                return -1;
        }
    }
    player_get_track(&ti);
    snprintf(e->title, sizeof e->title, "%s", ti.title);
    snprintf(e->artist, sizeof e->artist, "%s", ti.artist);
    e->dur = ti.duration_ms > 0 ? (int)(ti.duration_ms / 1000) : -1;
    return 0;
}

static uint32_t s_fav_seq;
static int s_fav;

int lib_ui_np_fav(void)
{
    audio_status_t st;

    audio_status(&st);
    if (st.seq != s_fav_seq) {
        s_fav_seq = st.seq;
        if (!g_nlists)
            m3u_lists();
        s_fav = np_entry(&s_e) == 0 && m3u_has(0, s_e.path);
    }
    return s_fav;
}

void lib_ui_np(int what)
{
    if (!g_lib.writable[lib_lists_vol()]) {
        nav_toast(ICON_WARN, TOAST_WARN, "Lists are read-only here");
        return;
    }
    m3u_lists();
    memset(&B, 0, sizeof B);
    if (np_entry(&B.one)) {
        nav_toast(ICON_WARN, TOAST_WARN, "Cannot tell where this track is");
        return;
    }
    B.what = AB_PATH;
    if (what == LIB_NP_FAV) {
        int k;

        nav_pop();                       /* (the options) */
        if (lib_ui_np_fav() && (k = fav_index(B.one.path)) >= 0) {
            nav_toast(ICON_CHECK, TOAST_TEXT, m3u_remove(0, k) ? "Could not remove it" : "Removed from favourites");
        } else {
            add_to(0);
        }
        s_fav_seq = 0;
    } else {
        push(&pg_lpick, V_PICK, s_vol, 0, 0, 0, "Add to playlist");
    }
}

void lib_notice(const char *s)
{
    nav_toast(ICON_SD, TOAST_WARN, "%s", s);
}
