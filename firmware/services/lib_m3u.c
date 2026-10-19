/*
 * lib_m3u.c -- favourites and playlists: M3U files in UTF-8 ("M3U8"), keyed by path, that a PC
 * reads and edits too (docs/LIBRARY.md 3, 5).
 *
 *   #EXTM3U
 *   #PLAYLIST:Road trip
 *   #EXTINF:245,Arijit Singh - Tum Hi Ho
 *   ../Music/Arijit Singh - King Of Bollywood/1. Tum Hi Ho.flac
 *
 * Where: the library data folder (lib_data_dir) of the volume the setting picks (lib_lists_vol):
 * FAVORITE.M3U and PL_001.M3U .. PL_999.M3U, which the device writes, and any other .m3u / .m3u8
 * there, which it reads and edits the same way. A path is written relative to the list's folder
 * when the track is on the same volume (what a PC player expects), else as the device's own
 * "/Internal/...". Paths read: relative ("..", "."), the device's "/SD/...", "/Internal/...", a PC's
 * "X:\...", "\..." or "/..." (from the list's volume root), file:// URIs (%XX decoded); '\' or '/'.
 *
 * Edits are crash-safe: an append grows the file (a cut keeps the old length); a removal, a move
 * or a rename writes the new list to LIBEDIT.TMP, then over the list (fs_target.c switches the
 * directory entry at fs_close: a cut leaves the old list whole).
 */
#include <stdio.h>
#include <string.h>

#include "lib_int.h"
#include "lib_m3u.h"

lib_list_t g_lists[M3U_LISTS];
int g_nlists;
static int s_open = -1;              /* the list whose entries are indexed below */
static uint32_t s_off[M3U_SPARSE];   /* every M3U_EVERY-th entry's first line */
static uint32_t s_n;
/* the buffers: in the scratch buffer's tail (lib_int.h), used only within a call */
#define s_rbuf  LT_RBUF
#define s_rbuf2 LT_RBUF2
#define s_wbuf  LT_WBUF
#define s_line  LT_LINE
#define s_info  LT_INFO
#define RBUF 512u
#define WBUF 512u

static const char k_fav[] = "FAVORITE.M3U";

static int ext_m3u(const char *n)
{
    const char *d = strrchr(n, '.');

    return d && (d[1] | 32) == 'm' && d[2] == '3' && (d[3] | 32) == 'u' && (!d[4] || (d[4] == '8' && !d[5]));
}

static int same_name(const char *a, const char *b)   /* ASCII case-insensitive */
{
    while (*a && ((*a | 0x20) == (*b | 0x20) || *a == *b))
        a++, b++;
    return !*a && !*b;
}

static const char *lists_dir(void)
{
    return lib_data_dir(lib_lists_vol());
}

void m3u_path(int k, char *out, int n)
{
    snprintf(out, (unsigned)n, "%s/%s", lists_dir(), g_lists[k].file);
}

/* ---- lines and paths ---- */

/* The next line from r into out (no CR / LF; cut at M3U_LINE - 1): its length, -1 at the end. */
static int next_line(lr_t *r, char *out, uint32_t *start)
{
    uint32_t n = 0;

    *start = lr_tell(r);
    for (;;) {
        const uint8_t *p = lr_peek(r, 1);

        if (!p) {
            out[n] = 0;
            return lr_tell(r) > *start ? (int)n : -1;
        }
        r->i++;
        if (*p == '\n')
            break;
        if (*p != '\r' && n < M3U_LINE - 1u)
            out[n++] = (char)*p;
    }
    out[n] = 0;
    return (int)n;
}

static int is_entry(const char *l)
{
    while (*l == ' ' || *l == '\t')
        l++;
    return *l && *l != '#';
}

/* "a/b/../c" -> "a/c" in place, '\' read as '/', never above "/Volume". */
static void tidy(char *p)
{
    char *o = p, *s = p, *floor;

    for (char *q = p; *q; q++)
        if (*q == '\\')
            *q = '/';
    floor = strchr(p + 1, '/');
    floor = floor ? floor : p + strlen(p);
    while (*s) {
        if (s[0] == '.' && (s[1] == '/' || !s[1]) && s > p && s[-1] == '/') {
            s += s[1] ? 2 : 1;
            continue;
        }
        if (s[0] == '.' && s[1] == '.' && (s[2] == '/' || !s[2]) && s > p && s[-1] == '/') {
            if (o - 1 > floor) {
                o--;
                while (o > floor + 1 && o[-1] != '/')
                    o--;
            }
            s += s[2] ? 3 : 2;
            continue;
        }
        if (s[0] == '/' && o > p && o[-1] == '/') {
            s++;
            continue;
        }
        *o++ = *s++;
    }
    if (o > floor + 1 && o[-1] == '/')
        o--;
    *o = 0;
}

static const char *find_dash(const char *s)           /* " - " (the libc has no strstr) */
{
    for (; *s; s++)
        if (s[0] == ' ' && s[1] == '-' && s[2] == ' ')
            return s;
    return 0;
}

static int hexv(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

/* An entry's line, from a list in folder dir, as a device path ("/SD/Music/x.flac"). */
static void resolve(const char *line, const char *dir, char *out, int n)
{
    const char *vol = !strncmp(dir, "/SD", 3) ? "/SD" : "/Internal";
    int uri = 0;

    while (*line == ' ' || *line == '\t')
        line++;
    if (!strncmp(line, "file://", 7)) {
        uri = 1;
        line += 7;
        if (line[0] == '/' && line[1] && line[2] == ':')
            line++;                      /* file:///C:/... */
    }
    if (!strncmp(line, "/SD/", 4) || !strncmp(line, "/Internal/", 10))
        snprintf(out, (unsigned)n, "%s", line);
    else if ((((line[0] | 32) >= 'a' && (line[0] | 32) <= 'z') && line[1] == ':') || line[0] == '\\' || line[0] == '/')
        snprintf(out, (unsigned)n, "%s/%s", vol, line + (line[1] == ':' ? 2 : 0) + 1);
    else
        snprintf(out, (unsigned)n, "%s/%s", dir, line);
    if (uri) {                           /* %20 and the like */
        char *o = out;

        for (const char *s = out; *s; s++)
            if (*s == '%' && hexv(s[1]) >= 0 && hexv(s[2]) >= 0) {
                *o++ = (char)(hexv(s[1]) << 4 | hexv(s[2]));
                s += 2;
            } else {
                *o++ = *s;
            }
        *o = 0;
    }
    tidy(out);
}

/* The line a track's path is written as: relative to the lists folder on the same volume. */
static void rel_path(const char *path, char *out, int n)
{
    const char *dir = lists_dir();
    const char *slash = strchr(dir + 1, '/');
    size_t vl = slash ? (size_t)(slash - dir) : strlen(dir);

    if (strncmp(path, dir, vl) || path[vl] != '/')
        snprintf(out, (unsigned)n, "%s", path);              /* the other volume: absolute */
    else if (slash)
        snprintf(out, (unsigned)n, "../%s", path + vl + 1);  /* /V/RECHORD -> ../x */
    else
        snprintf(out, (unsigned)n, "%s", path + vl + 1);     /* lists at the root: x */
}

static int dur_of(const char *info)                   /* "#EXTINF:245,..." */
{
    int d = 0;

    if (info[8] == '-')
        return -1;
    for (const char *q = info + 8; *q >= '0' && *q <= '9'; q++)
        d = d * 10 + (*q - '0');
    return d;
}

/* title / artist from "#EXTINF:d,Artist - Title", else from the file's name */
static void row_of(m3u_ent_t *e, const char *info)
{
    const char *c = info[0] ? strchr(info, ',') : 0, *dash;
    const char *f = strrchr(e->path, '/');

    e->dur = info[0] ? dur_of(info) : -1;
    if (!c || !c[1]) {
        snprintf(e->title, sizeof e->title, "%s", f ? f + 1 : e->path);
        if (strrchr(e->title, '.') && strrchr(e->title, '.') != e->title)
            *strrchr(e->title, '.') = 0;
        return;
    }
    c++;
    if ((dash = find_dash(c)) != 0) {
        size_t al = (size_t)(dash - c) < sizeof e->artist - 1 ? (size_t)(dash - c) : sizeof e->artist - 1;

        memcpy(e->artist, c, al);
        e->artist[al] = 0;
        snprintf(e->title, sizeof e->title, "%s", dash + 3);
    } else {
        snprintf(e->title, sizeof e->title, "%s", c);
    }
}

/* ---- the lists ---- */

/* The display name: #PLAYLIST, else ours "Playlist 7", else the file name without its extension. */
static void list_name(lib_list_t *L, const char *path)
{
    lr_t r;
    uint32_t at;
    int k = 0;

    snprintf(L->name, sizeof L->name, "%s", L->file);
    if (strrchr(L->name, '.'))
        *strrchr(L->name, '.') = 0;
    if (!strncmp(L->file, "PL_", 3) && strlen(L->file) == 10)
        snprintf(L->name, sizeof L->name, "Playlist %d", (L->file[3] - '0') * 100 + (L->file[4] - '0') * 10 + (L->file[5] - '0'));
    if (lr_open(&r, path, s_rbuf, RBUF))
        return;
    while (k++ < 8 && next_line(&r, s_line, &at) >= 0) {
        if (!strncmp(s_line, "#PLAYLIST:", 10) && s_line[10]) {
            snprintf(L->name, sizeof L->name, "%s", s_line + 10);
            break;
        }
        if (is_entry(s_line))
            break;
    }
    lr_close(&r);
}

static int lists_cb(void *u, const char *name, const fs_ref_t *r)
{
    lib_list_t *L;

    if ((r->flags & FS_REF_DIR) || !ext_m3u(name) || strlen(name) >= sizeof g_lists[0].file || name[0] == '.')
        return 0;
    if (same_name(name, k_fav)) {
        g_lists[0].size = r->size;
        return 0;
    }
    if (g_nlists == M3U_LISTS)
        return 1;
    L = &g_lists[g_nlists++];
    memset(L, 0, sizeof *L);
    snprintf(L->file, sizeof L->file, "%s", name);
    L->size = r->size;
    return 0;
}

int m3u_lists(void)
{
    char p[80];

    memset(&g_lists[0], 0, sizeof g_lists[0]);
    snprintf(g_lists[0].file, sizeof g_lists[0].file, "%s", k_fav);
    snprintf(g_lists[0].name, sizeof g_lists[0].name, "Favourites");
    g_lists[0].fav = 1;
    g_nlists = 1;
    s_open = -1;
    fs_scan(lists_dir(), lists_cb, 0);
    for (int k = 1; k < g_nlists; k++) {
        m3u_path(k, p, sizeof p);
        list_name(&g_lists[k], p);
    }
    for (int i = 2; i < g_nlists; i++)  /* by name */
        for (int k = i; k > 1 && lib_coll((const uint8_t *)g_lists[k].name, (unsigned)strlen(g_lists[k].name),
                                          (const uint8_t *)g_lists[k - 1].name,
                                          (unsigned)strlen(g_lists[k - 1].name), 0) < 0; k--) {
            lib_list_t t = g_lists[k];

            g_lists[k] = g_lists[k - 1];
            g_lists[k - 1] = t;
        }
    return g_nlists;
}

int m3u_open(int k)
{
    char p[80];
    lr_t r;
    uint32_t at, info = 0xFFFFFFFFu;

    s_open = -1;
    s_n = 0;
    if (k < 0 || k >= g_nlists)
        return -1;
    m3u_path(k, p, sizeof p);
    if (lr_open(&r, p, s_rbuf, RBUF) == 0) {
        while (next_line(&r, s_line, &at) >= 0 && s_n < M3U_MAX) {
            if (!strncmp(s_line, "#EXTINF:", 8))
                info = at;               /* (an entry's block starts at its #EXTINF) */
            if (!is_entry(s_line))
                continue;
            if (s_n % M3U_EVERY == 0)
                s_off[s_n / M3U_EVERY] = info != 0xFFFFFFFFu ? info : at;
            info = 0xFFFFFFFFu;
            s_n++;
        }
        lr_close(&r);
    }
    g_lists[k].n = (uint16_t)s_n;        /* (no file yet: an empty list) */
    s_open = k;
    return (int)s_n;
}

int m3u_count(void)
{
    return s_open >= 0 ? (int)s_n : 0;
}

int m3u_entry(int i, m3u_ent_t *e)
{
    char p[80];
    lr_t r;
    uint32_t at;
    int k;

    memset(e, 0, sizeof *e);
    if (s_open < 0 || i < 0 || (uint32_t)i >= s_n)
        return -1;
    m3u_path(s_open, p, sizeof p);
    if (lr_open(&r, p, s_rbuf, RBUF))
        return -1;
    lr_seek(&r, s_off[i / M3U_EVERY]);
    k = i / M3U_EVERY * M3U_EVERY;
    s_info[0] = 0;
    while (next_line(&r, s_line, &at) >= 0) {
        if (!strncmp(s_line, "#EXTINF:", 8)) {
            memcpy(s_info, s_line, M3U_LINE);
            continue;
        }
        if (!is_entry(s_line))
            continue;
        if (k++ == i) {
            resolve(s_line, lists_dir(), e->path, sizeof e->path);
            break;
        }
        s_info[0] = 0;
    }
    lr_close(&r);
    if (!e->path[0])
        return -1;
    row_of(e, s_info);
    return 0;
}

int m3u_has(int k, const char *path)
{
    char p[80], q[M3U_PATH];
    lr_t r;
    uint32_t at;
    int found = 0;

    m3u_path(k, p, sizeof p);
    if (lr_open(&r, p, s_rbuf, RBUF))
        return 0;
    while (!found && next_line(&r, s_line, &at) >= 0)
        if (is_entry(s_line)) {
            resolve(s_line, lists_dir(), q, sizeof q);
            found = same_name(q, path);
        }
    lr_close(&r);
    return found;
}

/* ---- writing ---- */

static void put_s(lw_t *w, const char *s)
{
    lw_put(w, s, (uint32_t)strlen(s));
}

static void put_header(lw_t *w, const char *name)
{
    put_s(w, "#EXTM3U\r\n#PLAYLIST:");
    put_s(w, name);
    put_s(w, "\r\n");
}

static void put_entry(lw_t *w, const m3u_ent_t *a)
{
    char line[M3U_PATH + 16];

    snprintf(line, sizeof line, "#EXTINF:%d,", a->dur >= 0 ? a->dur : -1);
    put_s(w, line);
    if (a->artist[0]) {
        put_s(w, a->artist);
        put_s(w, " - ");
    }
    put_s(w, a->title);
    put_s(w, "\r\n");
    rel_path(a->path, line, sizeof line);
    put_s(w, line);
    put_s(w, "\r\n");
}

int m3u_append(int k, const m3u_ent_t *a, int n)
{
    char p[80];
    lw_t w;
    int fresh;

    if (!g_lib.writable[lib_lists_vol()] || k < 0 || k >= g_nlists)
        return -1;
    if ((s_open == k ? s_n : g_lists[k].n) + (uint32_t)n > M3U_MAX)
        return -5;
    m3u_path(k, p, sizeof p);
    fresh = !fs_exists(p);
    if (lw_open(&w, p, s_wbuf, WBUF, !fresh))
        return -2;
    if (fresh)
        put_header(&w, g_lists[k].name);
    for (int i = 0; i < n; i++)
        put_entry(&w, &a[i]);
    if (lw_close(&w))
        return -3;
    if (s_open == k)
        m3u_open(k);
    else
        g_lists[k].n = (uint16_t)(g_lists[k].n + n);
    return 0;
}

/* List k written again through LIBEDIT.TMP: entry `drop` left out, entries `swap` and swap + 1
 * traded, the #PLAYLIST line set to `name` (each optional: -1, NULL); then over the list. */
static int rewrite(int k, int drop, int swap, const char *name)
{
    char held[2 * M3U_LINE + 8];         /* (on the stack: this is rare) */
    char p[80], t[80];
    lr_t r;
    lw_t w;
    uint32_t at;
    int e = -1, named = 0, ok = 1;

    if (!g_lib.writable[lib_lists_vol()] || k < 0 || k >= g_nlists)
        return -1;
    m3u_path(k, p, sizeof p);
    snprintf(t, sizeof t, "%s/LIBEDIT.TMP", lists_dir());
    if (lr_open(&r, p, s_rbuf, RBUF))
        return -2;
    if (lw_open(&w, t, s_wbuf, WBUF, 0)) {
        lr_close(&r);
        return -2;
    }
    held[0] = s_info[0] = 0;
    while (next_line(&r, s_line, &at) >= 0) {
        if (!strncmp(s_line, "#PLAYLIST:", 10) && name) {
            if (!named)
                put_header(&w, name) /* (#EXTM3U came already: harmless twice) */;
            named = 1;
            continue;
        }
        if (!strncmp(s_line, "#EXTINF:", 8)) {
            memcpy(s_info, s_line, M3U_LINE);
            continue;
        }
        if (!is_entry(s_line)) {
            if (name && !named && !strncmp(s_line, "#EXTM3U", 7)) {
                put_header(&w, name);
                named = 1;
                continue;
            }
            put_s(&w, s_line);
            put_s(&w, "\r\n");
            continue;
        }
        if (++e == drop) {
            s_info[0] = 0;
            continue;
        }
        if (e == swap) {                 /* held back until the entry after it is out */
            snprintf(held, sizeof held, "%s%s%s\r\n", s_info, s_info[0] ? "\r\n" : "", s_line);
            s_info[0] = 0;
            continue;
        }
        if (s_info[0]) {
            put_s(&w, s_info);
            put_s(&w, "\r\n");
            s_info[0] = 0;
        }
        put_s(&w, s_line);
        put_s(&w, "\r\n");
        if (held[0]) {
            put_s(&w, held);
            held[0] = 0;
        }
    }
    if (held[0])
        put_s(&w, held);
    lr_close(&r);
    if (name && !named)
        put_header(&w, name);
    if (lw_close(&w))
        return -3;
    if (lr_open(&r, t, s_rbuf, RBUF) || lw_open(&w, p, s_wbuf, WBUF, 0)) {
        lr_close(&r);
        return -4;
    }
    for (uint32_t left = r.end; left && ok; ) {
        uint32_t c = left < RBUF ? left : (uint32_t)RBUF;
        const uint8_t *q = lr_peek(&r, c);

        if (!q)
            ok = 0;
        else {
            lw_put(&w, q, c);
            r.i += c;
            left -= c;
        }
    }
    lr_close(&r);
    if (!ok)
        lw_abort(&w);
    else if (lw_close(&w))
        ok = 0;
    if (ok)
        fs_remove(t);
    if (s_open == k)
        m3u_open(k);
    return ok ? 0 : -4;
}

int m3u_remove(int k, int i) { return rewrite(k, i, -1, 0); }
int m3u_move(int k, int i, int dir) { return rewrite(k, -1, dir < 0 ? i - 1 : i, 0); }

int m3u_rename(int k, const char *name)
{
    int r;

    if (k <= 0 || !name[0])
        return -1;
    if ((r = rewrite(k, -1, -1, name)) == 0)
        snprintf(g_lists[k].name, sizeof g_lists[k].name, "%s", name);
    return r;
}

int m3u_delete(int k)
{
    char p[80];

    if (k <= 0 || k >= g_nlists || !g_lib.writable[lib_lists_vol()])
        return -1;
    m3u_path(k, p, sizeof p);
    return fs_remove(p);
}

int m3u_create(const char *name)
{
    char p[80], f[16], nm[32];
    lw_t w;
    int num;

    if (!g_lib.writable[lib_lists_vol()])
        return -1;
    for (num = 1; num < 1000; num++) {
        snprintf(f, sizeof f, "PL_%03d.M3U", num);
        snprintf(p, sizeof p, "%s/%s", lists_dir(), f);
        if (!fs_exists(p))
            break;
    }
    if (num == 1000 || lw_open(&w, p, s_wbuf, WBUF, 0))
        return -2;
    if (!name || !name[0]) {
        snprintf(nm, sizeof nm, "Playlist %d", num);
        name = nm;
    }
    put_header(&w, name);
    if (lw_close(&w))
        return -3;
    m3u_lists();
    for (int k = 1; k < g_nlists; k++)
        if (same_name(g_lists[k].file, f))
            return k;
    return -4;
}

/* ---- the lists from one folder into the lists folder (the setting, a fallback that ended) ---- */

/* Every list in folder `from` merged into the lists folder: its entries that are not there
 * appended (a list missing there gets its file name), then the source removed -- only once every
 * one of its entries is in a target that closed whole. Lists moved, or < 0. */
extern int emmc_sync(void) __attribute__((weak));   /* (drivers/emmc_rknanoc.c; none on the host) */

int m3u_move_all(const char *from)
{
    char files[M3U_LISTS][32];           /* (on the stack: this is rare) */
    fs_dirent_t de;
    uint32_t got, total;
    int n = 0, moved = 0;

    if (!strcmp(from, lists_dir()) || !g_lib.writable[lib_lists_vol()])
        return 0;
    for (uint32_t i = 0; fs_list_dir(from, i, &de, 1, &got, &total) == 0 && got && n < M3U_LISTS; i++)
        if (!de.is_dir && ext_m3u(de.name) && strlen(de.name) < sizeof files[0])
            snprintf(files[n++], sizeof files[0], "%s", de.name);
    m3u_lists();
    for (int f = 0; f < n; f++) {
        m3u_ent_t a;
        char src[80];
        lr_t r;
        uint32_t at, pos = 0;
        int k = -1, bad = 0;

        snprintf(src, sizeof src, "%s/%s", from, files[f]);
        for (int i = 0; i < g_nlists; i++)
            if (same_name(g_lists[i].file, files[f]))
                k = i;
        if (k < 0) {
            if (g_nlists == M3U_LISTS)
                continue;
            k = g_nlists++;
            memset(&g_lists[k], 0, sizeof g_lists[k]);
            snprintf(g_lists[k].file, sizeof g_lists[k].file, "%s", files[f]);
            list_name(&g_lists[k], src);
        }
        s_info[0] = 0;
        for (;;) {                        /* (the source again after each m3u_has: one buffer each) */
            int ln;

            if (lr_open(&r, src, s_rbuf2, 416u)) {
                bad = 1;
                break;
            }
            lr_seek(&r, pos);
            while ((ln = next_line(&r, s_line, &at)) >= 0 && !is_entry(s_line))
                if (!strncmp(s_line, "#EXTINF:", 8))
                    memcpy(s_info, s_line, M3U_LINE);
            pos = lr_tell(&r);
            lr_close(&r);
            if (ln < 0)
                break;
            memset(&a, 0, sizeof a);
            resolve(s_line, from, a.path, sizeof a.path);
            row_of(&a, s_info);
            if (s_info[0] && strchr(s_info, ',')) {   /* (keep the line as it was) */
                const char *c = strchr(s_info, ',') + 1;

                snprintf(a.title, sizeof a.title, "%s", c);
                a.artist[0] = 0;
            }
            s_info[0] = 0;
            if (!m3u_has(k, a.path) && m3u_append(k, &a, 1) != 0) {
                bad = 1;
                break;
            }
        }
        /* the card's writes are queued (drivers/emmc.h): on it before the source goes (the queue
         * drained: emmc_sync; a card pulled or failed meanwhile, -32: the source kept) */
        if (!bad && lib_lists_vol() == 1 && emmc_sync && emmc_sync())
            bad = 1;
        if (!bad && fs_remove(src) == 0)
            moved++;
    }
    m3u_lists();
    if (moved)
        lib_log("lib: %d list%s moved from %s to %s", moved, moved == 1 ? "" : "s", from, lists_dir());
    return moved;
}
