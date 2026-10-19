/*
 * lib_idx.c -- reading a committed index for the views (lib_int.h): the trailer, rows through
 * their section's block index, a small sector cache (the rows of a list page are next to each
 * other: a page costs one or two sectors once cached), the folder table's lookups.
 */
#include <stdio.h>
#include <string.h>

#include "lib_int.h"

lib_view_t g_lib_view[2];

#define NCACHE 4                     /* sectors, in the scratch buffer's tail (lib_int.h LT_IDXC) */
static struct {
    int vol;
    uint32_t sec, age;
} s_c[NCACHE];
#define CBUF(i) ((uint32_t *)(void *)(LT_IDXC + 512u * (i)))
static uint32_t s_age;
static fs_file_t *s_f[2];            /* each view's IDX, open while it is valid */

void lib_place_names(int vol, char *dir, int dn, char *base, int bn);   /* lib_job.c */

void idx_cache_drop(void)
{
    for (int i = 0; i < NCACHE; i++)
        s_c[i].age = 0;
}

void idx_drop(int vol)
{
    if (s_f[vol])
        fs_close(s_f[vol]);
    s_f[vol] = 0;
    g_lib_view[vol].ok = 0;
    for (int i = 0; i < NCACHE; i++)
        if (s_c[i].vol == vol)
            s_c[i].age = 0;
}

static int trailer_of(fs_file_t *f, lib_trailer_t *t)
{
    long size = fs_file_size(f);

    if (size < (long)sizeof *t || fs_seek(f, size - (long)sizeof *t) < 0 ||
        fs_read(f, t, sizeof *t) != (long)sizeof *t)
        return -1;
    if (t->magic != LIB_IDX_MAGIC || t->version != LIB_VERSION ||
        t->crc != lib_crc(0, t, (uint32_t)(sizeof *t - 4u)))
        return -2;
    return 0;
}

int idx_load_at(int vol, const char *dir, const char *base)
{
    char p[48];
    lib_trailer_t t;
    int best = -1;
    uint32_t seq = 0;

    idx_drop(vol);
    for (int k = 0; k < 2; k++) {
        fs_file_t *f;

        snprintf(p, sizeof p, "%s/%s.IX%c", dir, base, 'A' + k);
        if (!(f = fs_open_read(p)))
            continue;
        if (trailer_of(f, &t) == 0 && (best < 0 || t.seq > seq)) {
            best = k;
            seq = t.seq;
        }
        fs_close(f);
    }
    if (best < 0)
        return -1;
    snprintf(p, sizeof p, "%s/%s.IX%c", dir, base, 'A' + best);
    if (!(s_f[vol] = fs_open_read(p)) || trailer_of(s_f[vol], &g_lib_view[vol].tr)) {
        idx_drop(vol);
        return -1;
    }
    g_lib_view[vol].size = (uint32_t)fs_file_size(s_f[vol]);
    g_lib_view[vol].slot = best;
    snprintf(g_lib_view[vol].dir, sizeof g_lib_view[vol].dir, "%s", dir);
    snprintf(g_lib_view[vol].base, sizeof g_lib_view[vol].base, "%s", base);
    g_lib_view[vol].ok = 1;
    return 0;
}

int idx_load(int vol)
{
    char dir[24], base[12];

    lib_place_names(vol, dir, sizeof dir, base, sizeof base);
    return idx_load_at(vol, dir, base);
}

/* Volume vol is gone: its handles with it (fs_target freed them; closing one now could close a
 * handle someone else has been given since). The views on it, and any read from it, end. */
void lib_forget_handles_vol(int vol)
{
    for (int v = 0; v < 2; v++)
        if (g_lib_view[v].ok && (v == vol || !strncmp(g_lib_view[v].dir, vol ? "/SD" : "/Internal", vol ? 3 : 9))) {
            s_f[v] = 0;
            idx_drop(v);
        }
}

int idx_read(int vol, uint32_t off, void *dst, uint32_t len)
{
    uint8_t *o = dst;

    if (!g_lib_view[vol].ok || off > g_lib_view[vol].size || len > g_lib_view[vol].size - off)
        return -1;
    while (len) {
        uint32_t sec = off / 512u, k = 512u - off % 512u;
        int hit = -1, old = 0;

        for (int i = 0; i < NCACHE; i++) {
            if (s_c[i].age && s_c[i].vol == vol && s_c[i].sec == sec)
                hit = i;
            if (s_c[i].age < s_c[old].age)
                old = i;
        }
        if (hit < 0) {
            uint32_t n = g_lib_view[vol].size - sec * 512u;

            hit = old;
            s_c[hit].age = 0;
            if (fs_seek(s_f[vol], (long)(sec * 512u)) < 0 ||
                fs_read(s_f[vol], CBUF(hit), n < 512u ? n : 512u) < 0)
                return -1;
            s_c[hit].vol = vol;
            s_c[hit].sec = sec;
        }
        s_c[hit].age = ++s_age;
        if (k > len)
            k = len;
        memcpy(o, (uint8_t *)CBUF(hit) + off % 512u, k);
        o += k;
        off += k;
        len -= k;
    }
    return 0;
}

static uint32_t rd32(int vol, uint32_t off)
{
    uint8_t b[4];

    return idx_read(vol, off, b, 4) ? 0xFFFFFFFFu : lib_le32(b);
}

uint32_t idx_rows(int vol, int sec)
{
    return g_lib_view[vol].ok ? g_lib_view[vol].tr.sec[sec].rows : 0u;
}

static uint32_t nblk(const lib_sec_t *s, int sec)
{
    return sec == LS_SONGS ? 0u : (s->rows + LIB_BLK - 1u) / LIB_BLK;
}

uint32_t idx_letter(int vol, int sec, int letter)
{
    const lib_sec_t *s = &g_lib_view[vol].tr.sec[sec];

    if (!g_lib_view[vol].ok)
        return 0;
    return rd32(vol, s->off + s->blk + 4u * nblk(s, sec) + 4u * (uint32_t)letter);
}

/* Row i of a section with a block index: its offset in the file. */
static uint32_t row_off(int vol, int sec, uint32_t i)
{
    const lib_sec_t *s = &g_lib_view[vol].tr.sec[sec];
    uint32_t off = rd32(vol, s->off + s->blk + 4u * (i / LIB_BLK));

    if (off == 0xFFFFFFFFu)
        return off;
    off += s->off;
    for (uint32_t k = i % LIB_BLK; k; k--) {
        uint8_t b[2];

        if (idx_read(vol, off, b, 2))
            return 0xFFFFFFFFu;
        off += lib_le16(b);
    }
    return off;
}

/* The u8-counted strings from p on, each moved down a byte and ended with a NUL. */
static const char *cstr(uint8_t **pp, uint8_t *end, uint8_t *len)
{
    uint8_t *p = *pp, n;

    if (p >= end)
        return *len = 0, "";
    n = p[0];
    if (n > end - p - 1)
        n = (uint8_t)(end - p - 1);
    memmove(p, p + 1, n);
    p[n] = 0;
    *len = n;
    *pp = p + n + 1;
    return (const char *)p;
}

int idx_row(int vol, int sec, uint32_t i, lib_row_t *r, uint8_t *buf, uint32_t cap)
{
    uint32_t off, len;
    uint8_t *p, *end;

    memset(r, 0, sizeof *r);
    r->s1 = r->s2 = r->s3 = "";
    if (!g_lib_view[vol].ok || i >= g_lib_view[vol].tr.sec[sec].rows)
        return -1;
    if (sec == LS_SONGS) {
        i = rd32(vol, g_lib_view[vol].tr.sec[LS_SONGS].off + 4u * i);
        sec = LS_ALBTRK;
        if (i >= g_lib_view[vol].tr.sec[sec].rows)
            return -1;
    }
    if ((off = row_off(vol, sec, i)) == 0xFFFFFFFFu || idx_read(vol, off, buf, 2) ||
        (len = lib_le16(buf)) < 4u || len > cap - 1u || idx_read(vol, off, buf, len))
        return -1;
    p = buf;
    end = buf + len;
    switch (sec) {
    case LS_ALBTRK:
        r->kind = p[2];
        r->disc = p[3];
        r->trk = (uint16_t)lib_le16(p + 4);
        r->dur = (uint16_t)lib_le16(p + 6);
        r->year = (uint16_t)lib_le16(p + 8);
        ref_get(p + 10, &r->ref, vol);
        r->a = i;
        p += 10 + ROW_REF_BYTES;
        r->s1 = cstr(&p, end, &r->l1);
        r->s2 = cstr(&p, end, &r->l2);
        break;
    case LS_ARTALB:
    case LS_ALBUMS:
    case LS_GENALB:
        r->a = lib_le32(p + 2);              /* first track row */
        r->b = lib_le16(p + 6);              /* tracks */
        r->year = (uint16_t)lib_le16(p + 8);
        r->c = lib_le32(p + 10);             /* seconds */
        r->d = lib_le32(p + 14);             /* its row in ARTALB */
        p += 18;
        r->s1 = cstr(&p, end, &r->l1);       /* album */
        r->s2 = cstr(&p, end, &r->l2);       /* artist */
        r->s3 = cstr(&p, end, &r->l3);       /* genre */
        break;
    case LS_ARTISTS:
        r->a = lib_le32(p + 2);              /* first track row */
        r->b = lib_le32(p + 6);              /* tracks */
        r->c = lib_le32(p + 10);             /* first ARTALB row */
        r->d = lib_le16(p + 14);             /* albums */
        p += 16;
        r->s1 = cstr(&p, end, &r->l1);
        break;
    case LS_GENRES:
        r->c = lib_le32(p + 2);              /* first GENALB row */
        r->d = lib_le16(p + 6);              /* albums */
        p += 8;
        r->s1 = cstr(&p, end, &r->l1);
        break;
    case LS_FOLDERS:
        r->a = lib_le32(p + 2);              /* first cluster */
        r->b = lib_le32(p + 6);              /* its record in the RAW */
        p += 10;
        r->s1 = cstr(&p, end, &r->l1);       /* path */
        break;
    }
    return 0;
}

/* The FOLDERS row of first cluster clus: binary search over the blocks, then a block's rows. */
static int folder_find(int vol, uint32_t clus, lib_row_t *r, uint8_t *buf, uint32_t cap)
{
    const lib_sec_t *s = &g_lib_view[vol].tr.sec[LS_FOLDERS];
    uint32_t lo = 0, hi, b;

    if (!g_lib_view[vol].ok || !s->rows)
        return -1;
    hi = (s->rows + LIB_BLK - 1u) / LIB_BLK;
    while (hi - lo > 1u) {                   /* the last block whose first folder is <= clus */
        uint32_t mid = (lo + hi) / 2u;
        uint32_t off = rd32(vol, s->off + s->blk + 4u * mid);

        if (off == 0xFFFFFFFFu || rd32(vol, s->off + off + 2u) > clus)
            hi = mid;
        else
            lo = mid;
    }
    for (b = lo * LIB_BLK; b < s->rows && b < (lo + 1u) * LIB_BLK; b++) {
        if (idx_row(vol, LS_FOLDERS, b, r, buf, cap))
            return -1;
        if (r->a == clus)
            return 0;
        if (r->a > clus)
            break;
    }
    return -1;
}

int idx_folder_find(int vol, uint32_t clus, uint32_t *raw_off)
{
    uint8_t buf[300];
    lib_row_t r;

    if (folder_find(vol, clus, &r, buf, sizeof buf))
        return -1;
    *raw_off = r.b;
    return 0;
}

int idx_folder_path(int vol, uint32_t clus, char *out, int n)
{
    uint8_t buf[300];
    lib_row_t r;

    if (folder_find(vol, clus, &r, buf, sizeof buf) || r.l1 + 1 > n)
        return -1;
    memcpy(out, r.s1, r.l1 + 1u);
    return 0;
}
