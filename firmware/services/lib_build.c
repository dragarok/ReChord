/*
 * lib_build.c -- the views, from a whole RAW, into the IDX of the same slot (lib_int.h). The IDX
 * is written front to back and its trailer, written and closed last, is the commit.
 *
 *   S1  every track by (artist, album, disc, track, title)  -> ALBTRK: the track rows; an album's
 *       tracks and an artist's are ranges of it. "artist" is the album artist, else the artist.
 *   TA  the albums, in that order (from S1)                 -> ARTALB: an artist's albums, a range;
 *                                                              ARTISTS (groups of TA)
 *   S3  the albums by (name, artist)                        -> ALBUMS
 *   S4  the albums by (genre, name, artist)                 -> GENALB, GENRES (groups of S4)
 *   S2  the tracks by (title, artist), as ALBTRK row numbers -> SONGS
 *   S5  the folders by first cluster, with their paths      -> FOLDERS (playlists' paths, the walk's
 *                                                              lookups)
 * Names are grouped by lib_coll() == 0 ("Beatles" and "beatles" are one artist); the first one
 * seen is shown. Every comparator ends on a unique field.
 *
 * An edit (j->inc): the walk marks the folders whose tracks it did not copy as they were (F_CHANGED).
 * Their tracks (A1, sorted) are merged into the previous index's own order of every track (its
 * ARTALB and ALBTRK rows, read as they are), less the old tracks of those folders and of folders
 * gone (D: the old folder table against the new one, S5 first): the ALBTRK pass sees the same S1
 * stream a whole build sorts, without sorting it. SONGS likewise: the old entries renumbered (the
 * rows removed and added before each, M_RM and M_INS), the added ones (A2, sorted) put in their
 * places by a binary search of the old SONGS. The album, artist and genre views are rebuilt from the
 * new ALBTRK pass as always (albums are few). So an edit writes the index once and sorts only what
 * changed: no S1 or S2 sort of every track. More than M_NINS tracks added, M_NRM removed, M_ND
 * folders or M_MAXROWS rows: a whole build.
 *
 * Scratch: [0, 2K) the reader, [2K, 4K) the sorter's or the album rows' writer, [4K, 6K) the
 * IDX writer, [6K, 32K) the sorter's arena, or a section's block index while its rows go out.
 */
#include <stdio.h>
#include <string.h>

#include "lib_int.h"

#define S8        ((uint8_t *)lib_scratch)
#define B_RDBUF   (S8 + 0)
#define B_RDCAP   2048u
#define B_TABUF   (S8 + 2048)            /* (the sorter's writer's place: never at the same time) */
#define B_IDXBUF  (S8 + 6656)
#define B_FRBUF   (S8 + 11264)           /* the folders file, beside the tracks (S1); S5's path */
#define B_PATH    ((char *)(S8 + 11264))
#define B_BLK     ((uint32_t *)(void *)(S8 + (j->inc ? 12288u : 11264u)))   /* a section's block index,
                                                                         * while its rows go out */
#define B_NBLK    (j->inc ? (M_BLKEND - 12288u) / 4u : (LIB_JOB_SCRATCH - 512u - 11264u) / 4u)
/* the merge's (j->inc) */
#define M_OLD     (S8 + 0)               /* 1 KiB: a stream of the old index (FOLDERS, ALBTRK, SONGS) */
#define M_ARD     (S8 + 1024)            /* 1 KiB: the new folders sorted (S5), the added tracks (A1, A2) */
#define M_RM      ((uint16_t *)(void *)(S8 + 11264))   /* the old rows removed, ascending (past the */
#define M_NRM     256u                                 /* sorter's arena: kept over the A2 sort) */
#define M_INS     (M_RM + M_NRM)         /* each added row: the old row it went before (ascending) */
#define M_NINS    256u
#define M_BLKEND  25728u                 /* the block index ends here in a merge */
#define M_A2W     (S8 + 25728)           /* 512: the added tracks' SONGS records, as they get their rows */
#define M_OART    (S8 + 26240)           /* 512: the old ARTALB rows */
#define M_REC     (S8 + 26752)           /* 896: an old track row as an S1 record */
#define M_D       ((uint32_t *)(void *)(S8 + 27648))   /* the folders whose old tracks go, ascending */
#define M_ND      128u                   /* (to B_KEY) */
#define M_MAXROWS ((M_BLKEND - 12288u) / 4u * LIB_BLK)
#define B_KEY(i)  (S8 + LIB_JOB_SCRATCH - 512u + 164u * (i))   /* the group's names (3 x 164) */

enum {
    B_S1, B_ALB, B_TRK, B_S2, B_ARTALB, B_ART, B_S3, B_ALBUMS, B_S4, B_GENALB, B_GENRES, B_SONGS,
    B_S5, B_FOLDERS, B_TRAILER, B_DONE, M_S5, M_A1, M_D2, M_A2, M_SONGS
};

/* ---- record fields ---- */

static const uint8_t *str_at(const uint8_t *p, int k)   /* the k-th u8-counted string from p */
{
    while (k--)
        p += 1u + p[0];
    return p;
}

#define S1_FIX 33u                       /* [u16 len] disc kind trk year dur t ref */
#define S1_STR(p, k) str_at((p) + S1_FIX, (k))          /* 0 G, 1 album, 2 title, 3 artist, 4 genre */
#define TA_FIX 18u                       /* [u16 len] first count year dur albidx */
#define TA_STR(p, k) str_at((p) + TA_FIX, (k))          /* 0 album, 1 G, 2 genre */

static int coll_s(const uint8_t *a, const uint8_t *b, int fl)
{
    return lib_coll(a + 1, a[0], b + 1, b[0], fl);
}

static int u32cmp(uint32_t a, uint32_t b) { return a < b ? -1 : a > b; }

static int cmp_s1(const uint8_t *a, const uint8_t *b)
{
    int c;

    if ((c = coll_s(S1_STR(a, 0), S1_STR(b, 0), LC_ARTIST)) || (c = coll_s(S1_STR(a, 1), S1_STR(b, 1), 0)))
        return c;
    if ((c = (int)a[2] - (int)b[2]) || (c = (int)lib_le16(a + 4) - (int)lib_le16(b + 4)))
        return c;                        /* disc, track */
    if ((c = coll_s(S1_STR(a, 2), S1_STR(b, 2), 0)))
        return c;
    if ((c = u32cmp(lib_le32(a + 22), lib_le32(b + 22))))   /* its folder, its slot: the same order */
        return c;                                            /* whatever was walked before it */
    return (int)lib_le16(a + 30) - (int)lib_le16(b + 30);
}

static int cmp_s2(const uint8_t *a, const uint8_t *b)  /* [u16 len][u32 row] title artist */
{
    int c;

    if ((c = coll_s(a + 6, b + 6, 0)) || (c = coll_s(str_at(a + 6, 1), str_at(b + 6, 1), LC_ARTIST)))
        return c;
    return u32cmp(lib_le32(a + 2), lib_le32(b + 2));
}

static int cmp_s3(const uint8_t *a, const uint8_t *b)
{
    int c;

    if ((c = coll_s(TA_STR(a, 0), TA_STR(b, 0), 0)) || (c = coll_s(TA_STR(a, 1), TA_STR(b, 1), LC_ARTIST)))
        return c;
    return u32cmp(lib_le32(a + 14), lib_le32(b + 14));
}

static int cmp_s4(const uint8_t *a, const uint8_t *b)
{
    int c = coll_s(TA_STR(a, 2), TA_STR(b, 2), 0);

    return c ? c : cmp_s3(a, b);
}

static int cmp_s5(const uint8_t *a, const uint8_t *b)  /* [u16 len][u32 clus][u32 raw] path */
{
    int c = u32cmp(lib_le32(a + 2), lib_le32(b + 2));

    return c ? c : u32cmp(lib_le32(a + 6), lib_le32(b + 6));
}

/* ---- helpers ---- */

void ref_put(uint8_t *o, const fs_ref_t *r)
{
    lib_put32(o, r->clus);
    lib_put32(o + 4, r->size);
    lib_put32(o + 8, r->pclus);
    lib_put32(o + 12, r->psize);
    lib_put16(o + 16, r->slot);
    o[18] = r->flags;
}

static uint32_t put_s(uint8_t *o, const uint8_t *s, uint32_t cap)   /* u8-counted, cut */
{
    uint32_t n = s[0];

    if (n > cap) {
        n = cap;
        while (n && (s[1 + n] & 0xC0u) == 0x80u)
            n--;
    }
    o[0] = (uint8_t)n;
    memcpy(o + 1, s + 1, n);
    return 1u + n;
}

static void tmp_path(char *p, int n, int k)
{
    lib_tmp_path(p, n);
    p[strlen(p) - 5] = (char)('0' + k);
}

static lr_t s_fr;                    /* the folders file while S1 reads the tracks file */

/* tmp: a temp file; -1 the folders file; -2 the tracks file (and the folders file beside it) */
static int open_in(lib_job_t *j, int tmp)
{
    char p[48];

    if (tmp < 0)
        snprintf(p, sizeof p, "%s/%s.R%c%c", j->dir, j->base, tmp == -2 ? 'T' : 'W', 'A' + j->slot);
    else
        tmp_path(p, sizeof p, tmp);
    if (lr_open(&j->in, p, B_RDBUF, B_RDCAP))
        return -1;
    if (tmp == -2) {
        snprintf(p, sizeof p, "%s/%s.RW%c", j->dir, j->base, 'A' + j->slot);
        if (lr_open(&s_fr, p, B_FRBUF, 1024u)) {
            lr_close(&j->in);
            return -1;
        }
        j->f_left = 0;
    }
    return 0;
}

/* ---- sections ---- */

static void sec_begin(lib_job_t *j)
{
    j->sec_start = j->idx.pos;
    j->rows = j->nblk = 0;
    j->bsub = 0;                         /* letters assigned so far */
    j->tr.flags = 0;
    j->sec_crc = 0;
}

static void sec_put(lib_job_t *j, const void *p, uint32_t n)
{
    j->sec_crc = lib_crc(j->sec_crc, p, n);
    lw_put(&j->idx, p, n);
}

static void sec_row(lib_job_t *j, const void *row, uint32_t len, int letter, int blk)
{
    if (blk && j->rows % LIB_BLK == 0u) {
        if (j->nblk < B_NBLK)
            B_BLK[j->nblk] = j->idx.pos - j->sec_start;
        j->nblk++;
    }
    if (letter >= 0)
        while (j->bsub <= letter && j->bsub < LIB_LETTERS)
            j->letter[j->bsub++] = j->rows;
    sec_put(j, row, len);
    j->rows++;
}

static void sec_end(lib_job_t *j, int sec)
{
    lib_sec_t *s = &j->tr.sec[sec];

    while (j->bsub < LIB_LETTERS)
        j->letter[j->bsub++] = j->rows;
    s->off = j->sec_start;
    s->rows = j->rows;
    s->blk = j->idx.pos - j->sec_start;
    for (uint32_t i = 0; i < j->nblk && i < B_NBLK; i++) {
        uint8_t b[4];

        lib_put32(b, B_BLK[i]);
        sec_put(j, b, 4);
    }
    for (int i = 0; i < LIB_LETTERS; i++) {
        uint8_t b[4];

        lib_put32(b, j->letter[i]);
        sec_put(j, b, 4);
    }
    s->len = j->idx.pos - j->sec_start;
    s->crc = j->sec_crc;
}

/* ---- producers: records into the sorter until it is full, the input ends, or time is up ---- */

/* S1 from the RAW: one record per track, its folder's location in each (the folders file says
 * how many of the tracks file's records are each folder's, in the same order) */
static int prod_s1(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        uint32_t at = lr_tell(&j->in), len;
        const uint8_t *p, *st;
        const uint8_t *g, *alb, *tit, *art, *gen, *aart;
        uint8_t *o;

        if (LIB_OVER(k, t_end))
            return 0;
        while (!j->f_left) {             /* the next folder with tracks */
            if (LIB_OVER(++k, t_end))
                return 0;                /* (a merge passes over thousands of folders) */
            if (!(p = lr_rec_raw(&s_fr)) || p[0] == LIB_RAW_E) {
                lr_close(&s_fr);
                return 1;
            }
            if (p[0] != LIB_RAW_F || (j->inc && !(p[22] & F_CHANGED)))
                continue;                /* (a merge: only the changed folders' tracks) */
            j->f_ref.pclus = lib_le32(p + 4);
            j->f_ref.psize = lib_le32(p + 8);
            j->f_ref.flags = p[22];
            j->f_left = lib_le32(p + 36);
            if (j->inc)                  /* (its tracks: the chain walked there first, a piece a slice) */
                j->f_skip = lib_le32(p + 32) + 1u;
        }
        if (j->f_skip) {
            int r = lib_chain_walk(j->in.f, j->f_skip - 1u, t_end);

            if (r == 0)
                return 0;
            lr_seek(&j->in, j->f_skip - 1u);
            j->f_skip = 0;
            at = lr_tell(&j->in);
        }
        if (!(p = lr_rec(&j->in)) || p[0] != LIB_RAW_T)
            return -1;                   /* (the files disagree) */
        st = p + 32;
        tit = st;
        art = str_at(st, 1);
        alb = str_at(st, 2);
        aart = str_at(st, 3);
        gen = str_at(st, 4);
        g = aart[0] ? aart : art;
        len = S1_FIX + (1u + (g[0] < 160u ? g[0] : 160u)) + (1u + (alb[0] < 160u ? alb[0] : 160u)) + 1u + tit[0] +
              (1u + (art[0] < 160u ? art[0] : 160u)) + (1u + (gen[0] < 63u ? gen[0] : 63u));
        if (!(o = srt_room(&j->srt, len))) {
            lr_seek(&j->in, at);         /* (again, once the run is written) */
            return 0;
        }
        o[2] = p[26];                    /* disc */
        o[3] = p[23];                    /* kind */
        lib_put16(o + 4, lib_le16(p + 24));  /* trk */
        lib_put16(o + 6, lib_le16(p + 28));  /* year */
        lib_put16(o + 8, lib_le16(p + 30));  /* dur */
        lib_put32(o + 10, j->ctr++);         /* t: the track's number in walk order */
        {
            fs_ref_t r;

            r.clus = lib_le32(p + 8);
            r.size = lib_le32(p + 12);
            r.pclus = j->f_ref.pclus;
            r.psize = j->f_ref.psize;
            r.slot = (uint16_t)lib_le16(p + 20);
            r.flags = (uint8_t)((p[22] & ~FS_REF_PCONTIG) | (j->f_ref.flags & FS_REF_CONTIG ? FS_REF_PCONTIG : 0));
            ref_put(o + 14, &r);
        }
        {
            uint8_t *q = o + S1_FIX;

            q += put_s(q, g, 160);
            q += put_s(q, alb, 160);
            q += put_s(q, tit, 255);
            q += put_s(q, art, 160);
            q += put_s(q, gen, 63);
            len = (uint32_t)(q - o);
        }
        lib_put16(o, len);
        srt_commit(&j->srt, len);
        j->f_left--;
    }
}

/* S2 from the sorted S1: title, artist and the row number */
static int prod_s2(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        uint32_t at = lr_tell(&j->in), len;
        const uint8_t *p, *tit, *art;
        uint8_t *o;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec16(&j->in)))
            return 1;
        tit = S1_STR(p, 2);
        art = S1_STR(p, 3);
        len = 6u + 1u + tit[0] + 1u + art[0];
        if (!(o = srt_room(&j->srt, len))) {
            lr_seek(&j->in, at);
            return 0;
        }
        lib_put16(o, len);
        lib_put32(o + 2, j->ctr++);
        memcpy(o + 6, tit, 1u + tit[0]);
        memcpy(o + 7 + tit[0], art, 1u + art[0]);
        srt_commit(&j->srt, len);
    }
}

/* S3 / S4 from the album rows: the rows themselves */
static int prod_ta(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        uint32_t at = lr_tell(&j->in), len;
        const uint8_t *p;
        uint8_t *o;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec16(&j->in)))
            return 1;
        len = lib_le16(p);
        if (!(o = srt_room(&j->srt, len))) {
            lr_seek(&j->in, at);
            return 0;
        }
        memcpy(o, p, len);
        srt_commit(&j->srt, len);
    }
}

/* S5 from the RAW: each folder's first cluster, record and path */
static int prod_s5(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        uint32_t at = lr_tell(&j->in), len, d, base, n;
        const uint8_t *p;
        uint8_t *o;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec_raw(&j->in)) || p[0] == LIB_RAW_E)
            return 1;
        if (p[0] != LIB_RAW_F)
            continue;
        d = p[23];
        if (d > LIB_DEPTH)
            continue;
        base = d ? j->path_off[d] : 0u;
        n = p[44];
        len = 10u + 1u + (d ? (base ? base + 1u : 0u) + n : 0u);
        if (base == 0xFFFFu || len > 10u + 1u + 255u) {
            j->path_off[d + 1u] = 0xFFFFu;   /* (a path over 255 bytes: not in the table, nor its
                                              * subfolders) */
            continue;
        }
        if (!(o = srt_room(&j->srt, len))) {
            lr_seek(&j->in, at);
            return 0;
        }
        if (d) {                         /* the path: its parent's, a slash, its name */
            if (base)
                B_PATH[base++] = '/';
            memcpy(B_PATH + base, p + 45, n);
            base += n;
        } else {
            base = 0;
        }
        if (d + 1u <= LIB_DEPTH + 1u)
            j->path_off[d + 1u] = (uint16_t)base;
        lib_put16(o, len);
        lib_put32(o + 2, lib_le32(p + 4));
        lib_put32(o + 6, at | (p[22] & F_CHANGED ? 0x80000000u : 0u));   /* (masked off in FOLDERS) */
        o[10] = (uint8_t)base;
        j->n5++;
        if (p[22] & F_CHANGED)
            j->n_add += lib_le32(p + 36);
        memcpy(o + 11, B_PATH, base);
        srt_commit(&j->srt, len);
    }
}

typedef int (*prod_t)(lib_job_t *j, uint64_t t_end);

/* Feed and run the sorter: 1 = sorted (j->srt.result), 0 = more, < 0 = failed. */
static int run_sort(lib_job_t *j, prod_t prod, uint64_t t_end)
{
    for (;;) {
        int r;

        if (srt_wants_input(&j->srt)) {
            if ((r = prod(j, t_end)) < 0)
                return r;
            if (r == 1)
                srt_end_input(&j->srt);
        }
        r = srt_step(&j->srt, t_end);
        if (r < 0)
            return r;
        if (r == 2) {
            lr_close(&j->in);
            return 1;
        }
        if (r == 0 || lib_now_us() >= t_end || lib_io_dirty())
            return 0;
    }
}

/* ---- consumers ---- */

static void ta_emit(lib_job_t *j, lw_t *w)    /* the album collected so far, as a row */
{
    uint8_t o[2 + 16 + 3 * 164];
    uint32_t n = TA_FIX;

    lib_put32(o + 2, j->grp_first);
    lib_put16(o + 6, j->grp_n);
    lib_put16(o + 8, j->grp_year);
    lib_put32(o + 10, j->grp_dur);
    lib_put32(o + 14, j->n_alb++);
    for (int i = 0; i < 3; i++) {
        o[n] = j->grp_kl[i];
        memcpy(o + n + 1, B_KEY(i), j->grp_kl[i]);
        n += 1u + j->grp_kl[i];
    }
    lib_put16(o, n);
    lw_put(w, o, n);
}

static void grp_key(lib_job_t *j, int i, const uint8_t *s)
{
    j->grp_kl[i] = s[0] < 163u ? s[0] : 163u;
    memcpy(B_KEY(i), s + 1, j->grp_kl[i]);
}

static int same(const lib_job_t *j, int i, const uint8_t *s, int fl)
{
    return lib_coll(B_KEY(i), j->grp_kl[i], s + 1, s[0], fl) == 0;
}

/* one pass over S1: the albums (into TA) and the track rows (ALBTRK) */
static void alb_add(lib_job_t *j, lw_t *ta, const uint8_t *p)
{
    {
        const uint8_t *g, *alb, *gen;

        g = S1_STR(p, 0);
        alb = S1_STR(p, 1);
        gen = S1_STR(p, 4);
        if (!j->grp_n || !same(j, 1, g, LC_ARTIST) || !same(j, 0, alb, 0)) {
            if (j->grp_n)
                ta_emit(j, ta);
            j->grp_first = j->rows;          /* (this record's row: sec_row has not counted it yet) */
            j->grp_n = j->grp_dur = j->grp_year = 0;
            grp_key(j, 0, alb);
            grp_key(j, 1, g);
            j->grp_kl[2] = 0;
        }
        if (!j->grp_kl[2] && gen[0])
            grp_key(j, 2, gen);
        if (!j->grp_year)
            j->grp_year = lib_le16(p + 6);
        j->grp_dur += lib_le16(p + 8);
        j->grp_n++;
    }
}

/* ---- the merge's streams (j->inc) ---- */

static lr_t s_old, s_oart, s_add;    /* the old index's rows (on its view's handle), the added tracks */
static lw_t s_a2;                    /* the added tracks' SONGS records */
static const uint8_t *s_alb;         /* the old album row of the old row being read (in M_OART) */

static int find32(const uint32_t *a, uint32_t n, uint32_t v)   /* v in the ascending a[0..n) */
{
    uint32_t lo = 0;

    while (n > lo) {
        uint32_t m = (lo + n) / 2u;

        if (a[m] == v)
            return 1;
        if (a[m] < v)
            lo = m + 1u;
        else
            n = m;
    }
    return 0;
}

static uint32_t below16(const uint16_t *a, uint32_t n, uint32_t v, int le)   /* entries < v (<= v) */
{
    uint32_t lo = 0;

    while (n > lo) {
        uint32_t m = (lo + n) / 2u;

        if (a[m] < v || (le && a[m] == v))
            lo = m + 1u;
        else
            n = m;
    }
    return lo;
}

static int find16(const uint16_t *a, uint32_t n, uint32_t v)
{
    uint32_t i = below16(a, n, v, 0);

    return i < n && a[i] == v;
}

/* An old ALBTRK row's number in the new one (the rows removed before it, the added ones put before it) */
static uint32_t new_row(const lib_job_t *j, uint32_t r)
{
    return r - below16(M_RM, j->n_rm, r, 0) + below16(M_INS, j->n_ins, r, 1);
}

/* a section's rows of the old index, as a stream */
static void old_attach(lib_job_t *j, lr_t *r, int sec, uint8_t *buf, uint32_t cap)
{
    const lib_sec_t *x = &g_lib_view[j->vol].tr.sec[sec];

    lr_attach(r, idx_file(j->vol), x->off, x->off + (sec == LS_SONGS ? x->rows * 4u : x->blk), buf, cap);
}

/* The next old ALBTRK row into M_REC as an S1 record (m_have), or counted as removed (its folder in
 * D). 0, or < 0: the old index does not read (-1), more removed than M_RM holds (-2). */
static int old_row(lib_job_t *j)
{
    const uint8_t *p, *t;
    uint8_t *o = M_REC, *q;
    uint32_t r = j->m_oi++;

    if (s_old.f != idx_file(j->vol))
        return -1;                       /* (the view was read again meanwhile) */
    while (r >= j->m_af + j->m_an) {     /* its album: the next ARTALB row */
        if (!(s_alb = lr_rec16(&s_oart)))
            return -1;
        j->m_af = lib_le32(s_alb + 2);
        j->m_an = lib_le16(s_alb + 6);
    }
    if (!(p = lr_rec16(&s_old)))
        return -1;
    if (find32(M_D, j->n_d, lib_le32(p + 18))) {
        if (j->n_rm == M_NRM)
            return -2;
        M_RM[j->n_rm++] = (uint16_t)r;
        return 0;
    }
    o[2] = p[3];                         /* disc, kind, trk, year, dur (cons_trk's row, back) */
    o[3] = p[2] & R_KIND;
    lib_put16(o + 4, lib_le16(p + 4));
    lib_put16(o + 6, lib_le16(p + 8));
    lib_put16(o + 8, lib_le16(p + 6));
    lib_put32(o + 10, 0);
    memcpy(o + 14, p + 10, ROW_REF_BYTES);
    q = o + S1_FIX;
    q += put_s(q, TA_STR(s_alb, 1), 160);    /* the album's names: the same (lib_coll) as its tracks' */
    q += put_s(q, TA_STR(s_alb, 0), 160);
    t = p + 10 + ROW_REF_BYTES;
    q += put_s(q, t, 255);                   /* title, artist */
    q += put_s(q, str_at(t, 1), 160);
    if (p[2] & R_NOGENRE)                    /* genre: none, its own, or its album's */
        *q++ = 0;
    else
        q += put_s(q, p[2] & R_GENRE ? str_at(t, 2) : TA_STR(s_alb, 2), 63);
    lib_put16(o, (uint32_t)(q - o));
    j->m_have = 1;
    return 0;
}

/* The next record of the merged S1 stream (*added: one of A1), NULL at its end or on j->merr. */
static const uint8_t *mrg_next(lib_job_t *j, int *added)
{
    const uint8_t *a;

    while (!j->m_have && j->m_oi < j->m_on)
        if ((j->merr = old_row(j)) < 0)
            return 0;
    a = lr_peek(&s_add, 2);
    if (a && !(a = lr_peek(&s_add, lib_le16(a))))
        return j->merr = -1, (const uint8_t *)0;
    *added = 0;
    if (j->m_have && (!a || cmp_s1(M_REC, a) < 0)) {
        j->m_have = 0;
        return M_REC;
    }
    if (!a)
        return 0;
    if (j->n_ins == M_NINS)
        return j->merr = -2, (const uint8_t *)0;
    M_INS[j->n_ins++] = (uint16_t)(j->m_have ? j->m_oi - 1u : j->m_on);   /* (the old row it goes before) */
    s_add.i += lib_le16(a);
    *added = 1;
    return a;
}

static int cons_trk(lib_job_t *j, lw_t *ta, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        const uint8_t *p, *tit, *art, *gen;
        uint8_t o[2 + 27 + 256 + 161 + 64];
        uint32_t n;
        int added = 0;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = j->inc ? mrg_next(j, &added) : lr_rec16(&j->in))) {
            if (j->merr)
                return j->merr;
            if (j->grp_n)
                ta_emit(j, ta);
            return 1;
        }
        alb_add(j, ta, p);
        tit = S1_STR(p, 2);
        art = S1_STR(p, 3);
        gen = S1_STR(p, 4);
        if (added) {                     /* its SONGS record, now that it has its row */
            uint8_t h[6];

            lib_put16(h, 6u + 1u + tit[0] + 1u + art[0]);
            lib_put32(h + 2, j->rows);
            lw_put(&s_a2, h, 6);
            lw_put(&s_a2, tit, 1u + tit[0]);
            lw_put(&s_a2, art, 1u + art[0]);
        }
        /* kind, and the genre when it is not its album's (a merge reads it back: lib_int.h R_*) */
        o[2] = (uint8_t)(p[3] | (!gen[0] ? R_NOGENRE : gen[0] != j->grp_kl[2] || memcmp(gen + 1, B_KEY(2), gen[0]) ? R_GENRE : 0u));
        o[3] = p[2];                     /* disc */
        lib_put16(o + 4, lib_le16(p + 4));   /* trk */
        lib_put16(o + 6, lib_le16(p + 8));   /* dur */
        lib_put16(o + 8, lib_le16(p + 6));   /* year */
        memcpy(o + 10, p + 14, ROW_REF_BYTES);
        n = 10u + ROW_REF_BYTES;
        n += put_s(o + n, tit, 255);
        n += put_s(o + n, art, 160);
        if (o[2] & R_GENRE)
            n += put_s(o + n, gen, 63);
        lib_put16(o, n);
        sec_row(j, o, n, -1, 1);
    }
}

/* the album rows as a section (ARTALB, ALBUMS, GENALB): each as it is */
static int cons_albrows(lib_job_t *j, uint64_t t_end, int letters)
{
    for (int k = 0; ; k++) {
        const uint8_t *p;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec16(&j->in)))
            return 1;
        sec_row(j, p, lib_le16(p), letters ? lib_letter(TA_STR(p, 0) + 1, TA_STR(p, 0)[0], 0) : -1, 1);
    }
}

/* groups of the album rows: ARTISTS (by artist, from TA) or GENRES (by genre, from S4) */
static void grp_emit(lib_job_t *j, int genres)
{
    uint8_t o[2 + 14 + 164];
    uint32_t n;

    if (genres) {
        lib_put32(o + 2, j->grp_first2);
        lib_put16(o + 6, j->grp_n);
        n = 8u;
    } else {
        lib_put32(o + 2, j->grp_first);
        lib_put32(o + 6, j->grp_dur);    /* tracks */
        lib_put32(o + 10, j->grp_first2);
        lib_put16(o + 14, j->grp_n);
        n = 16u;
    }
    o[n] = j->grp_kl[0];
    memcpy(o + n + 1, B_KEY(0), j->grp_kl[0]);
    n += 1u + j->grp_kl[0];
    lib_put16(o, n);
    sec_row(j, o, n, lib_letter(B_KEY(0), j->grp_kl[0], genres ? 0 : LC_ARTIST), 1);
}

static int cons_groups(lib_job_t *j, uint64_t t_end, int genres)
{
    for (int k = 0; ; k++) {
        const uint8_t *p, *key;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec16(&j->in))) {
            if (j->grp_n)
                grp_emit(j, genres);
            return 1;
        }
        key = TA_STR(p, genres ? 2 : 1);
        if (!j->grp_n || !same(j, 0, key, genres ? 0 : LC_ARTIST)) {
            if (j->grp_n)
                grp_emit(j, genres);
            grp_key(j, 0, key);
            j->grp_first = lib_le32(p + 2);
            j->grp_first2 = j->grp_dur2;
            j->grp_n = 0;
            j->grp_dur = 0;
        }
        j->grp_dur += lib_le16(p + 6);
        j->grp_n++;
        j->grp_dur2++;
    }
}

static int cons_songs(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        const uint8_t *p;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = lr_rec16(&j->in)))
            return 1;
        sec_row(j, p + 2, 4, lib_letter(p + 7, p[6], 0), 0);
    }
}

static int cons_folders(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        uint8_t *p;

        if (LIB_OVER(k, t_end))
            return 0;
        if (!(p = (uint8_t *)(uintptr_t)lr_rec16(&j->in)))
            return 1;
        p[9] &= 0x7Fu;                   /* (prod_s5's changed bit: not in the index) */
        sec_row(j, p, lib_le16(p), -1, 1);
    }
}

/* D: the old folder table (sorted by cluster) against the new one (S5): a folder gone, or one the
 * walk marked changed, loses its old tracks. 1 = done, 0 = more, < 0 = more than M_ND (a whole build). */
static int mrg_d(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        const uint8_t *o, *n;
        uint32_t oc = 0, nc = 0, d;

        if (LIB_OVER(k, t_end))
            return 0;
        if (s_old.f != idx_file(j->vol))
            return -2;
        if ((o = lr_peek(&s_old, 2)) && (o = lr_peek(&s_old, lib_le16(o))))
            oc = lib_le32(o + 2);
        if ((n = lr_peek(&s_add, 2)) && (n = lr_peek(&s_add, lib_le16(n))))
            nc = lib_le32(n + 2);
        if (!o && !n)
            return 1;
        if (n && (!o || nc <= oc)) {     /* a new folder: changed? (the same folder before: passed) */
            d = n[9] & 0x80u ? nc : 0u;
            if (o && oc == nc)
                s_old.i += lib_le16(o);
            s_add.i += lib_le16(n);
        } else {                         /* an old folder not in the new table: gone */
            d = oc;
            s_old.i += lib_le16(o);
        }
        if (d) {
            if (j->n_d == M_ND)
                return -2;
            M_D[j->n_d++] = d;
        }
    }
}

/* SONGS of an edit: the old entries renumbered (a removed row's dropped), the added ones (A2, sorted)
 * each at its place in them, found by a binary search (a probe a step). 1 = done, 0 = more, < 0 = I/O. */
static int a2_vs_old(lib_job_t *j, const uint8_t *a, uint32_t i)   /* < 0: the added one goes first */
{
    lib_row_t r;
    uint8_t *buf = S8 + 2048;            /* (the album rows' writer's place: closed now) */
    const uint8_t *art = str_at(a + 6, 1);
    int c;

    if (idx_row(j->vol, LS_SONGS, i, &r, buf, 600))
        return j->merr = -1, 0;
    if ((c = lib_coll(a + 7, a[6], (const uint8_t *)r.s1, r.l1, 0)) ||
        (c = lib_coll(art + 1, art[0], (const uint8_t *)r.s2, r.l2, LC_ARTIST)))
        return c;
    return lib_le32(a + 2) < new_row(j, r.a) ? -1 : 1;
}

static int mrg_songs(lib_job_t *j, uint64_t t_end)
{
    for (int k = 0; ; k++) {
        const uint8_t *a, *o;
        uint8_t b[4];

        if (LIB_OVER(k, t_end))
            return 0;
        if (j->merr || s_old.f != idx_file(j->vol))
            return -1;
        if ((a = lr_peek(&s_add, 2)) && !(a = lr_peek(&s_add, lib_le16(a))))
            return -1;
        if (a && j->m_pos < 0) {         /* its place: the first old entry after it, [m_lo, m_hi) */
            if (j->m_lo < j->m_hi) {
                uint32_t m = (j->m_lo + j->m_hi) / 2u;

                if (a2_vs_old(j, a, m) < 0)
                    j->m_hi = m;
                else
                    j->m_lo = m + 1u;
                if (lib_now_us() >= t_end)
                    return 0;            /* (a probe reads a few sectors: the time tested after each) */
                continue;
            }
            j->m_pos = (int)j->m_lo;
        }
        if (a && (uint32_t)j->m_pos <= j->m_si) {    /* the added one */
            sec_row(j, a + 2, 4, lib_letter(a + 7, a[6], 0), 0);
            s_add.i += lib_le16(a);
            j->m_pos = -1;
            j->m_lo = j->m_si;
            j->m_hi = j->m_sn;
            continue;
        }
        if (j->m_si == j->m_sn)
            return a ? -1 : 1;
        if (!(o = lr_peek(&s_old, 4)))
            return -1;
        while (j->m_ob + 1u < LIB_LETTERS && j->m_obn <= j->m_si) {   /* its letter: the old index's */
            j->m_ob++;
            j->m_obn = j->m_ob + 1u < LIB_LETTERS ? idx_letter(j->vol, LS_SONGS, (int)j->m_ob + 1) : 0xFFFFFFFFu;
        }
        if (!find16(M_RM, j->n_rm, lib_le32(o))) {
            lib_put32(b, new_row(j, lib_le32(o)));
            sec_row(j, b, 4, (int)j->m_ob, 0);
        }
        s_old.i += 4;
        j->m_si++;
    }
}

/* ---- the build ---- */

static lw_t s_ta;

static int start_sort(lib_job_t *j, lib_cmp_t cmp, int in, int a, int b)
{
    j->ctr = 0;
    if (open_in(j, in))
        return -1;
    srt_begin(&j->srt, cmp, a, b);
    return 0;
}

static void mrg_close(void)
{
    lr_close(&s_old);
    lr_close(&s_oart);
    lr_close(&s_add);
    if (s_a2.f)
        lw_abort(&s_a2);
}

/* A whole build: the views from the RAW alone (the first index, a full refresh, or an edit too big
 * to merge: why). */
static int whole(lib_job_t *j, const char *why)
{
    if (j->inc) {
        lib_log("lib: %s: a whole build (%s)", j->vol ? "SD" : "Internal", why);
        srt_abort(&j->srt);
        lr_close(&j->in);
        lr_close(&s_fr);
        mrg_close();
        if (s_ta.f)
            lw_abort(&s_ta);
        if (j->idx.f)
            lw_abort(&j->idx);
    }
    j->inc = 0;
    j->bph = B_S1;
    j->bsub = 0;
    return start_sort(j, cmp_s1, -2, 0, 1);
}

int build_start(lib_job_t *j)
{
    const lib_view_t *v = &g_lib_view[j->vol];

    memset(&j->tr, 0, sizeof j->tr);
    j->n_alb = j->n_art = j->n_gen = 0;
    j->ctr = 0;
    j->bsub = 0;
    j->merr = 0;
    j->n5 = j->n_add = j->n_d = j->n_rm = j->n_ins = 0;
    /* an edit of the index the walk compared with: merged, if that index is a version 2 one whose
     * folder table has every folder, and the new one fits the merge's block index */
    j->inc = !j->full && v->ok && v->tr.version == LIB_VERSION && v->tr.seq + 1u == j->seq && idx_file(j->vol) &&
             v->tr.sec[LS_FOLDERS].rows == v->tr.nfolders && j->ntracks <= M_MAXROWS && v->tr.ntracks <= 65535u;
    if (!j->inc)
        return whole(j, "");
    j->bph = M_S5;
    return start_sort(j, cmp_s5, -1, 3, 4);  /* (the folders first: D needs them) */
}

static int start_pass(lib_job_t *j, int in)
{
    j->grp_n = 0;
    j->grp_dur2 = 0;
    j->rows = 0;
    return open_in(j, in);
}

int build_step(lib_job_t *j, uint64_t t_end)
{
    int r = 0;
    char p[48], q[48];

    while (lib_now_us() < t_end) {
        if (j->idx.err)
            return -1;
        switch (j->bph) {
        case B_S1:
            /* (each step that writes ends its slice: the blocks a close or a removal queues are
             * programmed between slices, and a big write after them would wait for each) */
            if (!j->bsub) {
                if ((r = run_sort(j, prod_s1, t_end)) <= 0)
                    return r;
                j->s1 = j->srt.result;
                j->bsub = 1;
                if (lib_io_dirty())
                    return 0;
            }
            tmp_path(p, sizeof p, 2);    /* (the album rows' file and the slot's old index: removed first) */
            if (j->bsub == 1) {
                if (!lib_rm_step(p))
                    return 0;
                j->bsub = 2;
                if (lib_io_dirty())
                    return 0;
            }
            snprintf(q, sizeof q, "%s/%s.IX%c", j->dir, j->base, 'A' + j->slot);
            if (j->bsub == 2) {
                if (!lib_rm_step(q))
                    return 0;
                j->bsub = 3;
                if (lib_io_dirty())
                    return 0;
            }
            if (j->inc && j->bsub == 3) {  /* (and the added tracks' SONGS records' file) */
                tmp_path(p, sizeof p, 5);
                if (!lib_rm_step(p))
                    return 0;
                j->bsub = 4;
                if (lib_io_dirty())
                    return 0;
            }
            j->bsub = 0;
            tmp_path(p, sizeof p, 2);
            if (lw_open(&s_ta, p, B_TABUF, LIB_WBUF, 0))
                return -1;
            if (j->inc) {                /* the merge: the old rows and albums, the added tracks */
                tmp_path(p, sizeof p, 5);
                old_attach(j, &s_old, LS_ALBTRK, M_OLD, 1024u);
                old_attach(j, &s_oart, LS_ARTALB, M_OART, 512u);
                tmp_path(q, sizeof q, j->a1);
                if (lw_open(&s_a2, p, M_A2W, 512u, 0) || lr_open(&s_add, q, M_ARD, 1024u))
                    return -1;
                j->m_oi = j->m_af = j->m_an = j->m_have = 0;
                j->m_on = g_lib_view[j->vol].tr.sec[LS_ALBTRK].rows;
                snprintf(q, sizeof q, "%s/%s.IX%c", j->dir, j->base, 'A' + j->slot);
            }
            if (lw_open(&j->idx, q, B_IDXBUF, LIB_WBUF, 0) || (!j->inc && start_pass(j, j->s1)))
                return -1;
            sec_begin(j);
            j->rows = 0;
            j->grp_n = 0;
            j->grp_dur2 = 0;
            j->bph = B_TRK;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_TRK:
            if (s_ta.f) {                /* (the album rows' file closed in a slice of its own) */
                if ((r = cons_trk(j, &s_ta, t_end)) < 0 && j->merr)
                    return whole(j, r == -2 ? "too many tracks changed" : "the previous index did not read") ? -1 : 0;
                if (r <= 0)
                    return r;
                lr_close(&j->in);
                if (lw_close(&s_ta) || (j->inc && lw_close(&s_a2)))
                    return -1;
                lr_close(&s_old);
                lr_close(&s_oart);
                lr_close(&s_add);
                return 0;
            }
            sec_end(j, LS_ALBTRK);
            if (j->inc) {                /* the added tracks' SONGS records sorted, then merged */
                lib_log("lib: %s merged: %u tracks in, %u out (%u folders)", j->vol ? "SD" : "Internal", (unsigned)j->n_ins,
                        (unsigned)j->n_rm, (unsigned)j->n_d);
                if (start_sort(j, cmp_s2, 5, 0, 1))
                    return -1;
                j->bph = M_A2;
                return 0;
            }
            if (start_sort(j, cmp_s2, j->s1, 3, 4))
                return -1;
            j->bph = B_S2;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_S2:
            if ((r = run_sort(j, prod_s2, t_end)) <= 0)
                return r;
            j->s2 = j->srt.result;
            if (start_pass(j, 2))
                return -1;
            sec_begin(j);
            j->bph = B_ARTALB;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_ARTALB:
            if ((r = cons_albrows(j, t_end, 0)) <= 0)
                return r;
            lr_close(&j->in);
            sec_end(j, LS_ARTALB);
            if (start_pass(j, 2))
                return -1;
            sec_begin(j);
            j->bph = B_ART;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_ART:
            if ((r = cons_groups(j, t_end, 0)) <= 0)
                return r;
            lr_close(&j->in);
            j->n_art = j->rows;
            sec_end(j, LS_ARTISTS);
            if (start_sort(j, cmp_s3, 2, 0, 1))
                return -1;
            j->bph = B_S3;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_S3:
            if ((r = run_sort(j, prod_ta, t_end)) <= 0)
                return r;
            if (start_pass(j, j->srt.result))
                return -1;
            sec_begin(j);
            j->bph = B_ALBUMS;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_ALBUMS:
            if ((r = cons_albrows(j, t_end, 1)) <= 0)
                return r;
            lr_close(&j->in);
            sec_end(j, LS_ALBUMS);
            if (start_sort(j, cmp_s4, 2, 0, 1))
                return -1;
            j->bph = B_S4;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_S4:
            if ((r = run_sort(j, prod_ta, t_end)) <= 0)
                return r;
            j->s1 = j->srt.result;           /* (S1 is done with: S4 in its place) */
            if (start_pass(j, j->s1))
                return -1;
            sec_begin(j);
            j->bph = B_GENALB;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_GENALB:
            if ((r = cons_albrows(j, t_end, 0)) <= 0)
                return r;
            lr_close(&j->in);
            sec_end(j, LS_GENALB);
            if (start_pass(j, j->s1))
                return -1;
            sec_begin(j);
            j->bph = B_GENRES;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_GENRES:
            if ((r = cons_groups(j, t_end, 1)) <= 0)
                return r;
            lr_close(&j->in);
            j->n_gen = j->rows;
            sec_end(j, LS_GENRES);
            if (start_pass(j, j->inc ? j->s5 : j->s2))   /* (a merge: SONGS done, the folders sorted) */
                return -1;
            sec_begin(j);
            j->bph = j->inc ? B_FOLDERS : B_SONGS;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_SONGS:
            if ((r = cons_songs(j, t_end)) <= 0)
                return r;
            lr_close(&j->in);
            sec_end(j, LS_SONGS);
            if (start_sort(j, cmp_s5, -1, 0, 1))
                return -1;
            j->bph = B_S5;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_S5:
            if ((r = run_sort(j, prod_s5, t_end)) <= 0)
                return r;
            if (start_pass(j, j->srt.result))
                return -1;
            sec_begin(j);
            j->bph = B_FOLDERS;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case B_FOLDERS:
            if ((r = cons_folders(j, t_end)) <= 0)
                return r;
            lr_close(&j->in);
            sec_end(j, LS_FOLDERS);
            j->bph = B_TRAILER;
            return 0;                    /* (a phase begun: the rest in the next slice) */
        case M_S5:                       /* the new folders by cluster (kept for FOLDERS), the counts */
            if ((r = run_sort(j, prod_s5, t_end)) <= 0)
                return r;
            j->s5 = j->srt.result;
            if (j->n5 != j->nfolders || j->n_add > M_NINS)
                return whole(j, j->n5 != j->nfolders ? "a folder path over 255 bytes" : "too many tracks changed") ? -1 : 0;
            if (start_sort(j, cmp_s1, -2, 0, 1))
                return -1;
            j->bph = M_A1;
            return 0;
        case M_A1:                       /* the changed folders' tracks, sorted */
            if ((r = run_sort(j, prod_s1, t_end)) <= 0)
                return r;
            j->a1 = j->srt.result;
            tmp_path(p, sizeof p, j->s5);
            old_attach(j, &s_old, LS_FOLDERS, M_OLD, 1024u);
            if (lr_open(&s_add, p, M_ARD, 1024u))
                return -1;
            j->bph = M_D2;
            return 0;
        case M_D2:                       /* D, then the ALBTRK pass as a whole build's (B_S1's files) */
            if ((r = mrg_d(j, t_end)) == 0)
                return 0;
            lr_close(&s_old);
            lr_close(&s_add);
            if (r < 0)
                return whole(j, "too many folders changed") ? -1 : 0;
            j->bph = B_S1;
            j->bsub = 1;
            return 0;
        case M_A2:
            if ((r = run_sort(j, prod_ta, t_end)) <= 0)
                return r;
            tmp_path(p, sizeof p, j->srt.result);
            lr_close(&j->in);
            old_attach(j, &s_old, LS_SONGS, M_OLD, 1024u);
            if (lr_open(&s_add, p, M_ARD, 1024u))
                return -1;
            j->m_si = 0;
            j->m_sn = g_lib_view[j->vol].tr.sec[LS_SONGS].rows;
            j->m_pos = -1;
            j->m_lo = 0;
            j->m_hi = j->m_sn;
            j->m_ob = 0;
            j->m_obn = idx_letter(j->vol, LS_SONGS, 1);
            sec_begin(j);
            j->bph = M_SONGS;
            return 0;
        case M_SONGS:
            if ((r = mrg_songs(j, t_end)) < 0)
                return whole(j, "the previous index did not read") ? -1 : 0;
            if (!r)
                return 0;
            lr_close(&s_old);
            lr_close(&s_add);
            sec_end(j, LS_SONGS);
            if (start_pass(j, 2))
                return -1;
            sec_begin(j);
            j->bph = B_ARTALB;
            return 0;
        case B_TRAILER:
            if (!lib_io_ok())
                return 0;
            j->tr.magic = LIB_IDX_MAGIC;
            j->tr.version = LIB_VERSION;
            j->tr.seq = j->seq;
            j->tr.serial = lib_vol_serial(j->vol);
            j->tr.ntracks = j->ntracks;
            j->tr.nfolders = j->nfolders;
            j->tr.nalbums = j->n_alb;
            j->tr.nartists = j->n_art;
            j->tr.ngenres = j->n_gen;
            j->tr.raw_size = j->raw.pos;
            j->tr.raw_crc = j->raw_end_crc;
            j->tr.trk_size = j->trk.pos;
            j->tr.root_sig = j->root_sig;
            j->tr.flags = 0;
            j->tr.crc = lib_crc(0, &j->tr, (uint32_t)(sizeof j->tr - 4u));
            lw_put(&j->idx, &j->tr, sizeof j->tr);
            if (lw_close(&j->idx))           /* the commit */
                return -1;
            j->bph = B_DONE;                 /* (the sorter's files: removed when idle, lib_job.c) */
            return 1;
        default:
            return 1;
        }
    }
    return 0;
}

void build_abort(lib_job_t *j)
{
    srt_abort(&j->srt);
    lr_close(&j->in);
    lr_close(&s_fr);
    mrg_close();
    if (s_ta.f)
        lw_abort(&s_ta);
    if (j->idx.f)
        lw_abort(&j->idx);
}

void srt_forget(void);

void build_forget(void)              /* (their volume went: dead handles, closed to free them) */
{
    srt_forget();
    fs_close(s_ta.f), fs_close(s_fr.f), fs_close(s_a2.f), fs_close(s_add.f);
    s_ta.f = s_fr.f = s_a2.f = s_add.f = 0;
    s_old.f = s_oart.f = 0;              /* (the view's handle: lib_idx.c's to close) */
}
