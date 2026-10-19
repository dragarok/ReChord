/*
 * dirlist.c -- a sorted folder view (dirlist.h). Entries: a ref (20 bytes) and a
 * name in a pool; sorted in place (insertion sort on an index array: a folder
 * page is at most DL_MAX entries and is sorted once per visit).
 */
#include <string.h>

#include "dirlist.h"

#define POOL 16384u
#define NAME_MAX_KEEP 120u                   /* longer names are cut (display only) */

typedef struct {
    fs_ref_t ref;
    uint16_t name;                           /* offset in s_pool */
    uint8_t kind;
} dl_ent_t;

static dl_ent_t s_ent[DL_MAX];
static uint16_t s_idx[DL_MAX];               /* sorted order */
static char s_pool[POOL];
static uint32_t s_pool_used;
static int s_n, s_skip, s_seen, s_more;
static int s_root;                           /* listing a volume's root ("/Internal", "/SD") */

static int ext_is(const char *dot, const char *ext)
{
    for (dot++; *dot && *ext; dot++, ext++)
        if ((*dot | 0x20) != *ext)
            return 0;
    return !*dot && !*ext;
}

int dirlist_kind(const char *name)
{
    static const struct { const char *ext; uint8_t kind; } k[] = {
        { "flac", DL_FLAC }, { "wav", DL_WAV }, { "aif", DL_AIFF }, { "aiff", DL_AIFF }, { "aifc", DL_AIFF },
        { "mp3", DL_MP3 }, { "mp2", DL_MP3 }, { "m4a", DL_M4A }, { "m4b", DL_M4A }, { "mp4", DL_M4A },
        { "alac", DL_M4A }, { "ogg", DL_OGG }, { "oga", DL_OGG }, { "opus", DL_OPUS }, { "wv", DL_WV },
        { "dsf", DL_DSF }, { "dff", DL_DFF }, { "ape", DL_APE }, { "wma", DL_WMA }, { "asf", DL_WMA },
    };
    const char *dot = strrchr(name, '.');

    if (!dot)
        return DL_NONE;
    for (unsigned i = 0; i < sizeof k / sizeof k[0]; i++)
        if (ext_is(dot, k[i].ext))
            return k[i].kind;
    return DL_NONE;
}

static int fold(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int dirlist_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {   /* numbers by value */
            const char *x = a, *y = b;
            int lx, ly, d = 0;

            while (*x == '0')
                x++;
            while (*y == '0')
                y++;
            for (lx = 0; x[lx] >= '0' && x[lx] <= '9'; lx++)
                ;
            for (ly = 0; y[ly] >= '0' && y[ly] <= '9'; ly++)
                ;
            if (lx != ly)
                return lx - ly;
            for (int i = 0; i < lx && !d; i++)
                d = x[i] - y[i];
            if (d)
                return d;
            a = x + lx;
            b = y + ly;
            continue;
        }
        if (fold((uint8_t)*a) != fold((uint8_t)*b))
            return fold((uint8_t)*a) - fold((uint8_t)*b);
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

static int add(void *u, const char *name, const fs_ref_t *r)
{
    int dir = (r->flags & FS_REF_DIR) != 0, kind = dir ? DL_NONE : dirlist_kind(name);
    uint32_t len;

    if (name[0] == '.' || (!dir && !kind))
        return 0;                            /* hidden, or not something we play */
    if (dir && s_root && fold((uint8_t)name[0]) == 'r' && !dirlist_cmp(name, "RECHORD"))
        return 0;                            /* the library's own folder (services/lib_job.c) */
    if (s_seen++ < s_skip)
        return 0;
    if (s_n == DL_MAX) {
        s_more = 1;
        return 1;
    }
    len = (uint32_t)strlen(name);
    if (len > NAME_MAX_KEEP)
        len = NAME_MAX_KEEP;
    if (s_pool_used + len + 1u > POOL) {
        s_more = 1;                          /* out of name room: the rest on the next page */
        return 1;
    }
    memcpy(s_pool + s_pool_used, name, len);
    s_pool[s_pool_used + len] = 0;
    s_ent[s_n].ref = *r;
    s_ent[s_n].name = (uint16_t)s_pool_used;
    s_ent[s_n].kind = (uint8_t)kind;
    s_pool_used += len + 1u;
    s_n++;
    return 0;
}

static int before(int i, int j)              /* folders first, then natural order */
{
    int di = (s_ent[i].ref.flags & FS_REF_DIR) != 0, dj = (s_ent[j].ref.flags & FS_REF_DIR) != 0;

    if (di != dj)
        return di;
    return dirlist_cmp(s_pool + s_ent[i].name, s_pool + s_ent[j].name) < 0;
}

int dirlist_load(const char *path, const fs_ref_t *dir, int page, int *more)
{
    int r;

    s_n = s_seen = s_more = 0;
    s_pool_used = 0;
    s_skip = page * DL_MAX;
    s_root = path && path[0] == '/' && path[1] && !strchr(path + 1, '/');
    r = path ? fs_scan(path, add, NULL) : fs_scan_ref(dir, add, NULL);
    if (r < 0) {
        s_n = 0;
        return r;
    }
    for (int i = 0; i < s_n; i++) {
        int j = i;

        s_idx[i] = (uint16_t)i;
        while (j > 0 && before(s_idx[j], s_idx[j - 1])) {
            uint16_t t = s_idx[j];

            s_idx[j] = s_idx[j - 1];
            s_idx[--j] = t;
        }
    }
    if (more)
        *more = s_more;
    return s_n;
}

int dirlist_count(void) { return s_n; }
const char *dirlist_name(int i) { return s_pool + s_ent[s_idx[i]].name; }
const fs_ref_t *dirlist_ref(int i) { return &s_ent[s_idx[i]].ref; }
int dirlist_is_dir(int i) { return (s_ent[s_idx[i]].ref.flags & FS_REF_DIR) != 0; }
int dirlist_kind_at(int i) { return s_ent[s_idx[i]].kind; }
