/*
 * lib_scan.c -- the walk: every folder of a volume, its playable files' tags, into a new RAW
 * (lib_int.h: a folders file and a tracks file), reading only what changed since the last one.
 *
 * Change detection (docs/LIBRARY.md 2): a folder's signature is an FNV-1a over its playable files
 * and subfolders as its directory entries describe them (name, slot, size, first cluster, and
 * the write time when the FS layer gives it: fs_scan_wtime). A folder whose signature matches the
 * previous walk's costs its directory sectors and nothing else: its tracks are copied as they
 * were. In a folder that changed, a file whose name, size, first cluster and write time match a
 * track it had is copied too; only new or changed files are opened for their tags.
 *
 * Order: depth first, a folder's subfolders in reverse on-disk order (a stack; the order only has
 * to be the same each time). The previous walk's folders file is read alongside: when nothing
 * moved, the next folder of the walk is the next record there (no lookups); otherwise the old
 * index's folder table (by first cluster) finds it. While every folder so far matched, nothing
 * is written ("lazy"): the first difference copies the matched prefix and the walk goes on
 * writing; a walk with no difference ends with LIB_UNCHANGED and writes nothing at all.
 *
 * Every step is bounded: a directory read until the slice ends (lib_dir.c: it goes on in the next
 * one from the entry where it stopped), one file's tags (a far read in it first walked to through
 * the FAT a slice at a time: WS_CHAIN), or records until the slice ends.
 * A writing walk checkpoints every LIB_CKPT_TAGS files read (the stack and the counters, both
 * files closed), so a power cut costs the tag reads since then: the next start resumes there.
 *
 * Scratch (32 KiB) while walking: both writers and both old readers, the tags, the batch of files
 * being read, the previous folder's tracks (a hash table), the folder stack and its names.
 */
#include <stdio.h>
#include <string.h>

#define TAGS_EXT 1
#include "tags.h"
#undef tags_read
#include "lib_int.h"
#include "dirlist.h"
#include "audio/mp3_info.h"
#include "audio/ape_info.h"

#define S8          ((uint8_t *)lib_scratch)             /* (its first LIB_JOB_SCRATCH bytes) */
#define W_FBUF      (S8 + 0)                              /* the folders writer: 1024 */
#define W_TBUF      (S8 + 1024)                           /* the tracks writer: LIB_WBUF */
#define W_OFBUF     (S8 + 5632)                           /* the old folders reader: 1024 */
#define W_OTBUF     (S8 + 6656)                           /* the old tracks reader: 1536 */
#define W_TAGS      ((tag_info_t *)(void *)(S8 + 8192))  /* 1100 */
#define W_BATCH     ((lib_bent_t *)(void *)(S8 + 9344))
#define W_NBATCH    64u                                   /* x 32 */
#define W_BPOOL     (S8 + 11392)
#define W_BPOOLCAP  1920u
#define W_MATCH     ((lib_match_t *)(void *)(S8 + 13312))
#define W_NMATCH    256u                                  /* x 16 */
#define W_PEND      ((lib_pend_t *)(void *)(S8 + 17408))  /* LIB_PEND x 32 */
#define W_NAMES     (S8 + 17408 + LIB_PEND * 32u)
#define W_NAMESCAP  (27392u - 17408u - LIB_PEND * 32u)
#define W_CURNAME   ((char *)(S8 + 27392))                /* 256 */
#define W_MP3       ((mp3_info_t *)(void *)(S8 + 27648))  /* <= 1024, to LIB_JOB_SCRATCH */

#define LIB_CKPT_TAGS 256u
#define NAME_ASK 0xFFFEu                 /* a pending folder whose name did not fit: fs_ref_name */
#define CONT     0xFFFFu                 /* a continuation: more subfolders of folder `parent` */
#define F_FIX    44u                     /* a folder record without its name */

typedef struct {
    fs_ref_t ref;
    uint32_t nh, wt;
    uint16_t name;
    uint8_t nlen, kind;
} lib_bent_t;

typedef struct {
    uint32_t nh, clus, size, off;    /* off: the old 'T' record + 1 in the old tracks file, 0 = free */
} lib_match_t;

enum { WS_POP, WS_SIG, WS_PREFIX, WS_DECIDE, WS_COPY, WS_MATCH, WS_BATCH, WS_FILES, WS_FEND, WS_CONT,
       WS_CHAIN, WS_END, WS_DONE, WS_CLEAR = 20 };

static uint32_t wtime(void)          /* (the entry lib_dscan reports) */
{
    return lib_dscan_wtime();
}

/* The tag readers: the extended one in the modes' window (startup/ap_ovl.c ap_lib_tags: -2 when the
 * USB-C earphones hold it), else the player's resident one (title, artist, album, track). */
typedef struct { char title[96], artist[64], album[64]; uint16_t track; uint8_t found; } tag_basic_t;
int tags_read(tags_read_at_t rd, void *u, uint32_t size, int kind, tag_basic_t *out);
int ap_lib_tags(tags_read_at_t rd, void *u, uint32_t size, int kind, tag_info_t *out);
int ap_lib_tags_ok(void);
#define T_PARTIAL 1u                     /* a track record's byte 27: read with the basic reader */
#define F_PARTIAL 0x80u                  /* a folder record's flags: it has such tracks */
/* (F_CHANGED, lib_int.h: its tracks are not the previous walk's as they were: the build merges) */

static uint32_t name_hash(const char *s, uint32_t n)
{
    uint32_t h = LIB_FNV0;

    for (uint32_t i = 0; i < n; i++) {   /* (case folded: FAT matches names that way) */
        uint8_t c = (uint8_t)s[i];

        h = (h ^ (c >= 'A' && c <= 'Z' ? c + 32u : c)) * 16777619u;
    }
    return h;
}

/* ---- records ---- */

static void rec_hdr(lw_t *w, int type, uint32_t len)
{
    lw_u8(w, (uint32_t)type);
    lw_u8(w, 0);
    lw_u16(w, len);
}

static uint32_t cut(const char *s, uint32_t cap)
{
    uint32_t n = (uint32_t)strlen(s);

    if (n > cap) {                       /* at a character boundary */
        n = cap;
        while (n && ((uint8_t)s[n] & 0xC0u) == 0x80u)
            n--;
    }
    return n;
}

static void put_str(lw_t *w, const char *s, uint32_t cap)
{
    uint32_t n = cut(s, cap);

    lw_u8(w, n);
    lw_put(w, s, n);
}

static void write_folder(lib_job_t *j)
{
    lib_cur_t *c = &j->cur;

    rec_hdr(&j->raw, LIB_RAW_F, F_FIX + 1u + c->nlen);
    lw_u32(&j->raw, c->ref.clus);
    lw_u32(&j->raw, c->ref.size);
    lw_u32(&j->raw, c->ref.pclus);
    lw_u32(&j->raw, c->ref.psize);
    lw_u16(&j->raw, c->ref.slot);
    lw_u8(&j->raw, (uint32_t)(c->ref.flags & ~(F_PARTIAL | F_CHANGED)) | (c->partial ? F_PARTIAL : 0u) |
                   (c->changed ? F_CHANGED : 0u));
    lw_u8(&j->raw, c->depth);
    lw_u32(&j->raw, c->sig);
    lw_u32(&j->raw, c->parent);
    lw_u32(&j->raw, c->toff);
    lw_u32(&j->raw, c->tcount);
    lw_u32(&j->raw, j->trk.pos - c->toff);
    lw_u8(&j->raw, c->nlen);
    lw_put(&j->raw, W_CURNAME, c->nlen);
}

/* ---- the stack ---- */

/* 1 pushed, 0 left out for good (too deep), -1 no room (a continuation will take it) */
static int push(lib_job_t *j, const char *name, const fs_ref_t *r, uint32_t limit)
{
    lib_pend_t *e;
    uint32_t n = (uint32_t)strlen(name);

    if (j->cur.depth + 1u > LIB_DEPTH) {
        j->warn |= LIBW_DEPTH;
        return 0;
    }
    if (j->sp >= limit)
        return -1;
    e = &W_PEND[j->sp++];
    e->ref = *r;
    e->parent = j->cur.idx;
    e->cont = 0;
    e->depth = (uint8_t)(j->cur.depth + 1u);
    e->name = (uint16_t)j->names;
    if (n > 255u || j->names + n > W_NAMESCAP) {
        e->nlen = NAME_ASK;              /* (asked of the FS when it is visited) */
        j->warn |= LIBW_NAMES;
    } else {
        e->nlen = (uint16_t)n;
        memcpy(W_NAMES + j->names, name, n);
        j->names += n;
    }
    return 1;
}

/* this scan's pushes start at base: more subfolders after `slot` wait in a continuation below them */
static void push_cont(lib_job_t *j, uint32_t base, int32_t slot)
{
    lib_pend_t *e;
    uint16_t names = base < j->sp ? W_PEND[base].name : (uint16_t)j->names;

    memmove(&W_PEND[base + 1u], &W_PEND[base], (j->sp - base) * sizeof(lib_pend_t));
    j->sp++;
    e = &W_PEND[base];
    memset(e, 0, sizeof *e);
    e->ref = j->cur.ref;
    e->parent = j->cur.idx;
    e->name = names;                     /* (the name stack's top once those above are gone) */
    e->nlen = CONT;
    e->cont = (uint16_t)(slot < 0 ? 0xFFFFu : (uint32_t)slot);
    e->depth = j->cur.depth;
    e->pad = j->cur.root;
}

/* ---- directory scans ---- */

static int skipped(const lib_job_t *j, const char *name)
{
    static const char *const k_skip[] = { "RECHORD", "System Volume Information", "$RECYCLE.BIN", 0 };

    if (name[0] == '.')
        return 1;                        /* .Trashes, .Spotlight-V100, macOS "._" files */
    if (j->cur.depth == 0)
        for (int i = 0; k_skip[i]; i++) {
            const char *a = name, *b = k_skip[i];

            while (*a && (*a | 0x20) == (*b | 0x20))
                a++, b++;
            if (!*a && !*b)
                return 1;
        }
    return 0;
}

static int sig_cb(void *u, const char *name, const fs_ref_t *r)
{
    lib_scan_t *s = u;
    lib_job_t *j = s->j;
    lib_cur_t *c = &j->cur;
    int dir = (r->flags & FS_REF_DIR) != 0, kind = dir ? 0 : dirlist_kind(name);
    uint32_t v[4];

    if (c->root && !c->ref.clus) {
        c->ref.clus = r->pclus;          /* the root's own cluster, from its first entry */
        c->ref.size = r->psize;
        c->ref.flags = (uint8_t)(FS_REF_DIR | (r->flags & FS_REF_PCONTIG ? FS_REF_CONTIG : 0));
    }
    if ((!dir && !kind) || skipped(j, name))
        return 0;
    if (!s->cont) {
        v[0] = (uint32_t)dir | (uint32_t)r->slot << 8;
        v[1] = dir ? 0u : r->size;
        v[2] = r->clus;
        v[3] = wtime();
        c->sig = lib_fnv(lib_fnv(c->sig, name, (uint32_t)strlen(name) + 1u), v, sizeof v);
        if (kind)
            c->nfiles++;
    }
    if (dir && !s->more && (!s->cont || c->cont_slot < 0 || r->slot > (uint32_t)c->cont_slot)) {
        int p = s->pushed < LIB_SUBS_MAX ? push(j, name, r, s->limit) : -1;

        if (p < 0)
            s->more = 1;                 /* the rest: a continuation */
        else if (p > 0) {
            s->pushed++;
            s->last = (int32_t)r->slot;
        }
        if (s->more && s->cont)
            return 1;                    /* (no signature to finish) */
    }
    return 0;
}

/* The folder being visited, read from entry `from` on (lib_dir.c): 0, or < 0 (it is gone). */
static int dir_start(lib_job_t *j, uint32_t from)
{
    j->ds_on = 1;
    return lib_dscan_start(&j->ds, j->cur.root ? j->root : 0, &j->cur.ref, from);
}

/* ... on from where it stopped: 1 = read to the end (or the callback stopped it), 0 = the slice is
 * over (more in the next), < 0 = gone */
static int dir_run(lib_job_t *j, fs_scan_cb cb, uint64_t t_end)
{
    int r = lib_dscan(&j->ds, cb, &j->sc, t_end);

    if (r == 0)
        return 0;
    j->ds_on = 0;
    return r < 0 ? r : 1;
}

/* the next batch of playable files (after last_slot) */
static int batch_cb(void *u, const char *name, const fs_ref_t *r)
{
    lib_scan_t *b = u;
    lib_job_t *j = b->j;
    lib_bent_t *e;
    uint32_t n = (uint32_t)strlen(name);
    int kind;

    if ((r->flags & FS_REF_DIR) || !(kind = dirlist_kind(name)) || skipped(j, name))
        return 0;
    if (b->any && r->slot <= j->cur.last_slot)
        return 0;
    if (j->nbatch == W_NBATCH || j->bpool + n > W_BPOOLCAP || n > 255u)
        return j->nbatch > 0;            /* full: the rest in the next batch */
    e = &W_BATCH[j->nbatch++];
    e->ref = *r;
    e->nh = name_hash(name, n);
    e->wt = wtime();
    e->name = (uint16_t)j->bpool;
    e->nlen = (uint8_t)n;
    e->kind = (uint8_t)kind;
    memcpy(W_BPOOL + j->bpool, name, n);
    j->bpool += n;
    return 0;
}

/* ---- the previous walk ---- */

/* The record at the old folders cursor, past checkpoints; NULL at its end. *at: its offset. */
static const uint8_t *old_next(lib_job_t *j, uint32_t *at)
{
    for (;;) {
        const uint8_t *p;
        uint32_t len;

        *at = lr_tell(&j->old);
        if (!(p = lr_peek(&j->old, 4)) || p[0] == LIB_RAW_E || (len = lib_le16(p + 2)) < 4u)
            return 0;
        if (p[0] == LIB_RAW_C) {
            lr_seek(&j->old, *at + len); /* (a checkpoint: bigger than the buffer, never read) */
            continue;
        }
        if (!(p = lr_peek(&j->old, len)))
            return 0;
        j->old.i += len;
        return p;
    }
}

int idx_folder_find(int vol, uint32_t clus, uint32_t *raw_off);   /* lib_idx.c */

/* The previous walk's record of this folder: at the cursor (the usual case), else from the old
 * index's folder table. Found: the cursor after it; not found: the cursor where it was. */
static void old_find(lib_job_t *j)
{
    lib_cur_t *c = &j->cur;
    uint32_t at, off;
    const uint8_t *p;

    c->old = c->at_cursor = 0;
    if (!j->old_ok)
        return;
    c->before = lr_tell(&j->old);
    p = old_next(j, &at);
    if (p && p[0] == LIB_RAW_F && lib_le32(p + 4) == c->ref.clus && (c->root ? p[23] == 0 : p[23] != 0)) {
        c->at_cursor = 1;
    } else {
        if (c->root || idx_folder_find(j->vol, c->ref.clus, &off) != 0) {
            lr_seek(&j->old, c->before);
            return;
        }
        lr_seek(&j->old, off);
        p = old_next(j, &at);
        if (!p || p[0] != LIB_RAW_F || lib_le32(p + 4) != c->ref.clus) {
            lr_seek(&j->old, c->before);
            return;
        }
    }
    c->old = 1;
    c->old_off = at;
    c->old_sig = lib_le32(p + 24);
    c->partial = (p[22] & F_PARTIAL) != 0;   /* (a copy keeps it) */
    if (c->partial && ap_lib_tags_ok())
        c->old_sig = ~c->sig;            /* (read with the basic tags: changed, to be read again) */
    c->old_toff = lib_le32(p + 32);
    c->tcount = lib_le32(p + 36);        /* (the old count: what a copy brings) */
    c->old_tlen = lib_le32(p + 40);
}

/* ---- tags ---- */

/* The tag readers' reads. One whose walk through the FAT is long (a tag at the end of a big file
 * of small clusters) is refused: the file's tags are read again once WS_CHAIN has walked there a
 * slice at a time (j->far: the farthest such offset). */
static uint32_t rd_at(void *u, uint32_t off, void *buf, uint32_t n)
{
    lib_job_t *j = u;
    long got;

    if (fs_seek(j->tf, (long)off) < 0)
        return 0;
    if (lib_chain_gap(j->tf, off) > LIB_CHAIN_FAR) {
        if (!j->far_bad && off > j->far)
            j->far = off;
        return 0;
    }
    got = fs_read(j->tf, buf, n);
    return got > 0 ? (uint32_t)got : 0u;
}

static int tag_kind(int k)
{
    switch (k) {
    case DL_FLAC: return TAGS_FLAC;
    case DL_WAV:  return TAGS_WAV;
    case DL_M4A:  return TAGS_MP4;
    case DL_OGG:
    case DL_OPUS: return TAGS_OGG;
    case DL_WMA:  return TAGS_WMA;
    case DL_DSF:  return TAGS_DSF;
    case DL_AIFF: return TAGS_AIFF;
    case DL_WV:   return TAGS_WV;
    case DL_APE:  return TAGS_APE;
    case DL_DFF:  return -1;
    default:      return TAGS_MP3;
    }
}

/* A file's tags: 0, or T_PARTIAL (the basic reader's: the walk reads it again when it can); -1 =
 * a far read was refused (j->far): the file stays open (j->tf) for WS_CHAIN, then this again. */
static int read_tags(lib_job_t *j, const lib_bent_t *e, tag_info_t *t)
{
    void *f = j;                         /* (the readers' context: rd_at's) */
    int k = tag_kind(e->kind), partial = 0;

    memset(t, 0, sizeof *t);
    if (!j->tf && !(j->tf = fs_open_ref(&e->ref))) {
        j->read_tags++;
        j->since_ckpt++;
        return 0;
    }
    j->far = 0;
    if (k >= 0 && ap_lib_tags(rd_at, f, e->ref.size, k, t) == -2) {
        tag_basic_t *b = (tag_basic_t *)(void *)t->aartist;   /* (the space of fields it lacks) */

        tags_read(rd_at, f, e->ref.size, k > TAGS_OGG ? TAGS_MP3 : k, b);
        memmove(t->title, b->title, sizeof b->title);
        memmove(t->album, b->album, sizeof b->album);
        memmove(t->artist, b->artist, sizeof b->artist);
        t->track = b->track;
        memset(t->aartist, 0, sizeof t->aartist);
        partial = 1;
        j->warn |= LIBW_PARTIAL;
    }
    if (!t->dur_ms && e->kind == DL_MP3 && !j->far) {
        mp3_info_t *mi = W_MP3;

        if (mp3_info(rd_at, f, e->ref.size, mi) == 0 && mi->rate) {
            if (mi->frames)
                t->dur_ms = (uint32_t)((uint64_t)mi->frames * 1000u / mi->rate);
            else if (mi->bitrate)
                t->dur_ms = (uint32_t)((uint64_t)(mi->data_end - mi->data_off) * 8u / mi->bitrate);
        }
    } else if (!t->dur_ms && e->kind == DL_APE && !j->far) {
        ape_info_t ai;

        if (ape_info(rd_at, f, e->ref.size, &ai) == 0 && ai.rate)
            t->dur_ms = (uint32_t)((uint64_t)ai.samples * 1000u / ai.rate);
    }
    if (j->far)
        return -1;                       /* (walked there first, then read again) */
    fs_close(j->tf);
    j->tf = 0;
    j->far_bad = 0;
    j->read_tags++;
    j->since_ckpt++;
    return partial;
}

/* A track record for e: from its tags, the file name standing in for a missing title (and its
 * leading number for a missing track number), the folder's name for a missing album. */
static void write_track(lib_job_t *j, const lib_bent_t *e, tag_info_t *t, int partial)
{
    char fn[256];
    uint32_t len;

    memcpy(fn, W_BPOOL + e->name, e->nlen);
    fn[e->nlen] = 0;
    if (!t->title[0]) {
        char *dot = strrchr(fn, '.');

        if (dot && dot != fn)
            *dot = 0;
        memcpy(t->title, fn, strlen(fn) + 1u);
    }
    if (!t->track) {
        uint32_t n = 0, d = 0;

        while (d < 3u && fn[d] >= '0' && fn[d] <= '9')
            n = n * 10u + (uint32_t)(fn[d++] - '0');
        if (d && (fn[d] == ' ' || fn[d] == '.' || fn[d] == '-' || fn[d] == '_'))
            t->track = (uint16_t)n;
    }
    if (!t->album[0] && !j->cur.root) {
        memcpy(t->album, W_CURNAME, j->cur.nlen);
        t->album[j->cur.nlen] = 0;
    }
    len = 32u + 5u + cut(t->title, 255) + cut(t->artist, 255) + cut(t->album, 255) + cut(t->aartist, 255) +
          cut(t->genre, 63);
    rec_hdr(&j->trk, LIB_RAW_T, len);
    lw_u32(&j->trk, e->nh);
    lw_u32(&j->trk, e->ref.clus);
    lw_u32(&j->trk, e->ref.size);
    lw_u32(&j->trk, e->wt);
    lw_u16(&j->trk, e->ref.slot);
    lw_u8(&j->trk, e->ref.flags);
    lw_u8(&j->trk, e->kind);
    lw_u16(&j->trk, t->track);
    lw_u8(&j->trk, t->disc);
    lw_u8(&j->trk, partial ? T_PARTIAL : 0u);
    if (partial)
        j->cur.partial = 1;
    lw_u16(&j->trk, t->year);
    lw_u16(&j->trk, t->dur_ms / 1000u > 0xFFFFu ? 0xFFFFu : t->dur_ms / 1000u);
    put_str(&j->trk, t->title, 255);
    put_str(&j->trk, t->artist, 255);
    put_str(&j->trk, t->album, 255);
    put_str(&j->trk, t->aartist, 255);
    put_str(&j->trk, t->genre, 63);
    j->ntracks++;
    j->cur.tcount++;
}

/* ---- matching a changed folder's files against the tracks it had ---- */

static lib_match_t *match_slot(uint32_t nh, uint32_t clus)
{
    uint32_t i = (nh ^ clus * 2654435761u) % W_NMATCH;

    for (uint32_t k = 0; k < W_NMATCH; k++, i = (i + 1u) % W_NMATCH) {
        lib_match_t *m = &W_MATCH[i];

        if (!m->off || (m->nh == nh && m->clus == clus))
            return m;
    }
    return 0;
}

/* ---- files ---- */

static void raw_path(char *p, int n, const char *dir, const char *base, int t, int slot)
{
    snprintf(p, (unsigned)n, "%s/%s.R%c%c", dir, base, "WTC"[t], 'A' + slot);
}

#define CK_H ((lib_ckpt_hdr_t *)(void *)&j->tr)   /* (the build's trailer: unused in the walk) */
typedef char ck_fits[sizeof(lib_ckpt_hdr_t) <= sizeof(lib_trailer_t) ? 1 : -1];

/* The walk's writers: new files (WS_CLEAR removed the slot's old ones and its checkpoint first), or,
 * resumed, each cut back to the size the checkpoint (CK_H) names. */
static int open_writers(lib_job_t *j, int resumed)
{
    char p[48];

    raw_path(p, sizeof p, j->dir, j->base, 0, j->slot);
    if (resumed ? lw_open_at(&j->raw, p, W_FBUF, 1024u, CK_H->rsize) : lw_open(&j->raw, p, W_FBUF, 1024u, 0))
        return -1;
    raw_path(p, sizeof p, j->dir, j->base, 1, j->slot);
    if (resumed ? lw_open_at(&j->trk, p, W_TBUF, LIB_WBUF, CK_H->tsize) : lw_open(&j->trk, p, W_TBUF, LIB_WBUF, 0)) {
        lw_abort(&j->raw);
        return -1;
    }
    return 0;
}

/* ---- checkpoints ---- */

/* A checkpoint over several slices (each starts with Internal's queue empty and ends at its first
 * write, lib_io_ok / lib_io_dirty): the tracks file closed, the folders file closed (both are on
 * the card to these sizes now), the checkpoint file written (the header, then the stack and its
 * names a writer's buffer at a time: up to ~16 KB), both files opened again. The header lives in
 * CK_H, the stage in j->bsub, the bytes put in j->ctr; the checkpoint's writer is j->raw's (the
 * folders file is closed meanwhile). 1 = done. */
static int ckpt_step(lib_job_t *j)
{
    lib_ckpt_hdr_t *h = CK_H;
    uint32_t len = 4u + (uint32_t)sizeof *h + j->sp * (uint32_t)sizeof(lib_pend_t) + j->names + 8u;
    char p[48];

    switch (j->bsub) {
    case 0:
        memset(h, 0, sizeof *h);
        h->seq = j->seq;
        h->nfolders = j->nfolders;
        h->ntracks = j->ntracks;
        h->old_off = j->old_ok ? lr_tell(&j->old) : 0u;
        h->crc = j->raw.crc;
        h->rsize = j->raw.pos;
        h->tsize = j->trk.pos;
        h->tcrc = j->trk.crc;
        h->read_tags = j->read_tags;
        h->root_sig = j->root_sig;
        h->sp = (uint16_t)j->sp;
        h->names = (uint16_t)j->names;
        if (lw_close(&j->trk))
            goto bad;
        j->bsub = 1;
        return 0;
    case 1:
        if (lw_close(&j->raw))
            goto bad;
        j->bsub = 2;
        return 0;
    case 2:
        raw_path(p, sizeof p, j->dir, j->base, 2, j->slot);
        if (lw_open(&j->raw, p, W_FBUF, 1024u, 0))
            goto bad;
        rec_hdr(&j->raw, LIB_RAW_C, len);
        lw_put(&j->raw, h, sizeof *h);
        j->ctr = 0;
        j->bsub = 3;
        return 0;
    case 3: {                            /* the stack, then its names: one buffer a slice */
        uint32_t ps = j->sp * (uint32_t)sizeof(lib_pend_t), all = ps + j->names;
        uint32_t k = j->raw.cap - j->raw.n;

        if (k > all - j->ctr)
            k = all - j->ctr;
        while (k) {
            uint32_t o = j->ctr, n = o < ps ? (ps - o < k ? ps - o : k) : k;

            lw_put(&j->raw, o < ps ? (const uint8_t *)W_PEND + o : (const uint8_t *)W_NAMES + (o - ps), n);
            j->ctr += n;
            k -= n;
        }
        if (j->ctr < all)
            return 0;
        lw_u32(&j->raw, len);
        lw_u32(&j->raw, LIB_CKPT_MAGIC);
        j->bsub = 4;
        return 0;
    }
    case 4:
        if (lw_close(&j->raw))
            goto bad;
        j->bsub = 5;
        return 0;
    case 5:
        raw_path(p, sizeof p, j->dir, j->base, 1, j->slot);
        if (lw_open(&j->trk, p, W_TBUF, LIB_WBUF, 1))
            goto bad;
        j->trk.crc = h->tcrc;
        j->bsub = 6;
        return 0;
    default:
        raw_path(p, sizeof p, j->dir, j->base, 0, j->slot);
        if (lw_open(&j->raw, p, W_FBUF, 1024u, 1))
            goto bad;
        j->raw.crc = h->crc;
        j->bsub = 0;
        j->since_ckpt = 0;
        lib_log("lib: %s checkpoint: %u folders, %u tracks, %u read", j->base, (unsigned)j->nfolders,
                (unsigned)j->ntracks, (unsigned)j->read_tags);
        return 1;
    }
bad:
    j->raw.err = FS_ERR_IO;
    return 0;
}

extern fs_file_t *fs_open_append_at(const char *path, uint32_t at) __attribute__((weak));

/* A walk of this seq cut short (power): its last checkpoint, into CK_H (open_writers cuts the
 * files back to it). 1 = resumed, 0 = start afresh. */
static int ckpt_resume(lib_job_t *j)
{
    lib_ckpt_hdr_t h;
    lr_t r;
    uint8_t t[8];
    uint32_t size, len;
    char p[48];
    int ok = 0;

    raw_path(p, sizeof p, j->dir, j->base, 2, j->slot);
    if (lr_open(&r, p, W_OFBUF, 1024u))
        return 0;
    size = r.end;
    if (size < 12u + sizeof h)
        goto out;
    lr_seek(&r, size - 8u);
    if (lr_get(&r, t, 8) || lib_le32(t + 4) != LIB_CKPT_MAGIC || (len = lib_le32(t)) != size)
        goto out;
    lr_seek(&r, 4u);
    if (lr_get(&r, &h, sizeof h) || h.seq != j->seq || h.sp > LIB_PEND || h.names > W_NAMESCAP ||
        len != 12u + sizeof h + h.sp * (uint32_t)sizeof(lib_pend_t) + h.names ||
        lr_get(&r, W_PEND, h.sp * (uint32_t)sizeof(lib_pend_t)) || lr_get(&r, W_NAMES, h.names))
        goto out;
    lr_close(&r);
    for (int k = 0; k < 2; k++) {        /* (each file as long as it names, at least) */
        fs_file_t *f;
        long n;

        raw_path(p, sizeof p, j->dir, j->base, k, j->slot);
        if (!(f = fs_open_read(p)))
            return 0;
        n = fs_file_size(f);
        fs_close(f);
        if (n < 0 || (uint32_t)n < (k ? h.tsize : h.rsize))
            return 0;
    }
    j->nfolders = h.nfolders;
    j->ntracks = h.ntracks;
    j->read_tags = h.read_tags;
    j->root_sig = h.root_sig;
    j->sp = h.sp;
    j->names = h.names;
    j->raw.crc = h.crc;
    j->trk.crc = h.tcrc;
    *CK_H = h;
    if (j->old_ok && h.old_off)
        lr_seek(&j->old, h.old_off);
    return fs_open_append_at != 0;
out:
    lr_close(&r);
    return ok;
}

/* The walk of this seq ended before a power cut, the build did not: 1, its counts restored. */
static int raw_whole(lib_job_t *j)
{
    lib_rawend_t e;
    lr_t r;
    uint8_t t[8];
    uint32_t size;
    char p[48];
    int ok = 0;

    raw_path(p, sizeof p, j->dir, j->base, 0, j->slot);
    if (lr_open(&r, p, W_OFBUF, 1024u))
        return 0;
    size = r.end;
    lr_seek(&r, size >= 8u ? size - 8u : 0u);
    if (size >= 40u && lr_get(&r, t, 8) == 0 && lib_le32(t + 4) == LIB_END_MAGIC && lib_le32(t) == 40u) {
        lr_seek(&r, size - 36u);
        if (lr_get(&r, &e, sizeof e) == 0 && e.seq == j->seq) {
            lr_close(&r);
            raw_path(p, sizeof p, j->dir, j->base, 1, j->slot);
            if (lr_open(&r, p, W_OTBUF, 1536u))
                return 0;
            if (r.end == e.tsize) {
                j->raw.pos = size;
                j->raw_end_crc = e.crc;
                j->trk.pos = e.tsize;
                j->nfolders = e.nfolders;
                j->ntracks = e.ntracks;
                j->root_sig = e.root_sig;
                ok = 1;
            }
        }
    }
    lr_close(&r);
    return ok;
}

/* ---- the walk ---- */

static void scan_close(lib_job_t *j);

int scan_start(lib_job_t *j)
{
    char p[48];
    lib_view_t *v = &g_lib_view[j->vol];
    int resumed = 0;

    j->nfolders = j->ntracks = j->read_tags = j->reused = j->copied = j->since_ckpt = 0;
    j->sp = j->names = 0;
    j->warn = 0;
    j->old_ok = 0;
    j->t_sync = 0;
    if (!j->full && v->ok) {             /* the previous walk, to compare with and copy from */
        raw_path(p, sizeof p, v->dir, v->base, 0, v->slot);
        if (lr_open(&j->old, p, W_OFBUF, 1024u) == 0) {
            raw_path(p, sizeof p, v->dir, v->base, 1, v->slot);
            if (j->old.end == v->tr.raw_size && lr_open(&j->otrk, p, W_OTBUF, 1536u) == 0) {
                if (j->otrk.end == v->tr.trk_size)
                    j->old_ok = 1;
                else
                    lr_close(&j->otrk);
            }
            if (!j->old_ok)
                lr_close(&j->old);
        }
    }
    if (!j->full && raw_whole(j)) {      /* cut short in the build: the walk is done */
        scan_close(j);
        lib_log("lib: %s build resumed: the walk of seq %u was whole", j->base, (unsigned)j->seq);
        return LIB_WALKED;
    }
    if (!j->full)
        resumed = ckpt_resume(j);
    j->reads0 = j->read_tags;
    j->lazy = j->old_ok && !resumed;
    if (resumed) {
        uint32_t fc = j->raw.crc, tc = j->trk.crc;

        if (open_writers(j, 1)) {
            if (j->old_ok)
                lr_close(&j->old), lr_close(&j->otrk);
            return -1;
        }
        j->raw.crc = fc;
        j->trk.crc = tc;
    }
    if (resumed) {
        j->wst = WS_POP;
        lib_log("lib: %s walk resumed: %u folders, %u tracks so far, %u to visit", j->base,
                (unsigned)j->nfolders, (unsigned)j->ntracks, (unsigned)j->sp);
        return 0;
    }
    memset(&j->cur, 0, sizeof j->cur);
    j->cur.root = 1;
    j->wst = j->lazy ? WS_SIG : WS_CLEAR;
    j->wclr = WS_SIG;
    j->bsub = 0;
    return 0;
}

static void scan_close(lib_job_t *j)
{
    if (j->tf)
        fs_close(j->tf);
    j->tf = 0;
    j->ds_on = 0;
    if (j->old_ok) {
        lr_close(&j->old);
        lr_close(&j->otrk);
    }
    j->old_ok = 0;
}

/* Leave the lazy walk at the first difference: what matched so far is copied, then it writes. */
static int unlazy(lib_job_t *j, uint32_t fend, int next)
{
    j->lazy = 0;
    j->pfx = j->tpfx = 0;
    j->pfx_end = fend;
    j->tpfx_end = j->t_sync;
    j->wnext = (uint32_t)next;
    j->wst = WS_CLEAR;                   /* (the slot's old files removed, the writers opened) */
    j->wclr = WS_PREFIX;
    j->bsub = 0;
    return 0;
}

/* Copy the old tracks file from *off to end into the new one, a slice at a time: 1 when done. */
static int copy_trk(lib_job_t *j, uint32_t *off, uint32_t end, uint64_t t_end)
{
    while (*off < end) {
        uint32_t k = end - *off < 1024u ? end - *off : 1024u;
        const uint8_t *p;

        lr_seek(&j->otrk, *off);
        if (!(p = lr_peek(&j->otrk, k)))
            return -1;
        lw_put(&j->trk, p, k);
        *off += k;
        if (lib_now_us() >= t_end || lib_io_dirty())
            return *off >= end;
    }
    return 1;
}

int scan_step(lib_job_t *j, uint64_t t_end)
{
    lib_cur_t *c = &j->cur;

    while (lib_now_us() < t_end && !lib_io_dirty()) {
        if (j->raw.err || j->trk.err) {
            scan_abort(j);
            return -1;
        }
        switch (j->wst) {
        case WS_POP: {
            lib_pend_t *e;

            if (!j->lazy && j->since_ckpt >= LIB_CKPT_TAGS) {
                ckpt_step(j);
                return 0;                /* (each step a slice of its own) */
            }
            if (!j->sp) {
                j->wst = WS_END;
                break;
            }
            e = &W_PEND[--j->sp];
            memset(c, 0, sizeof *c);
            c->ref = e->ref;
            c->depth = e->depth;
            j->names = e->name;
            if (e->nlen == CONT) {       /* more subfolders of a folder already visited */
                c->idx = e->parent;
                c->root = e->pad;
                c->cont = 1;
                c->cont_slot = e->cont == 0xFFFFu ? -1 : (int32_t)e->cont;
                j->wst = WS_CONT;
                break;
            }
            if (e->nlen == NAME_ASK) {
                if (lib_ref_name(&c->ref, W_CURNAME) != 0)
                    break;               /* (gone since: skip it) */
                c->nlen = (uint16_t)cut(W_CURNAME, 255);
            } else {
                c->nlen = e->nlen;
                memcpy(W_CURNAME, W_NAMES + e->name, e->nlen);
            }
            c->parent = e->parent;
            j->wst = WS_SIG;
            break;
        }
        case WS_SIG: {                    /* the folder's directory, read until the slice ends */
            lib_scan_t *s = &j->sc;
            int r = 0;

            if (!j->ds_on) {
                c->idx = j->nfolders;
                c->sig = LIB_FNV0;
                memset(s, 0, sizeof *s);
                s->j = j;
                s->base = j->sp;
                s->last = -1;
                s->limit = LIB_PEND - 2u;
                r = dir_start(j, 0);
            }
            if (r >= 0 && (r = dir_run(j, sig_cb, t_end)) == 0)
                return 0;                /* (more of it in the next slice) */
            if (r < 0) {
                j->ds_on = 0;
                if (c->root)
                    return -2;           /* the volume itself: gone */
                j->sp = s->base;         /* (a folder that vanished: skip it) */
                j->wst = WS_POP;
                break;
            }
            if (s->more)
                push_cont(j, s->base, s->last);
            if (c->root)
                j->root_sig = c->sig;
            j->nfolders++;
            old_find(j);
            if (j->lazy) {
                if (c->old && c->at_cursor && c->sig == c->old_sig) {
                    j->ntracks += c->tcount;     /* the same as last time: nothing to write */
                    j->copied += c->tcount;
                    j->t_sync = c->old_toff + c->old_tlen;
                    j->wst = WS_POP;
                    break;
                }
                if (unlazy(j, c->before, WS_DECIDE))
                    return -1;
                break;
            }
            j->wst = WS_DECIDE;
            break;
        }
        case WS_PREFIX: {                 /* the matched folders' records, then their tracks */
            uint32_t at;
            const uint8_t *p;

            if (j->pfx < j->pfx_end) {
                lr_seek(&j->old, j->pfx);
                if (!(p = old_next(j, &at)) || at >= j->pfx_end) {
                    j->pfx = j->pfx_end;
                    break;
                }
                lw_put(&j->raw, p, 22);  /* (as it was, but not changed by this walk) */
                lw_u8(&j->raw, p[22] & ~F_CHANGED);
                lw_put(&j->raw, p + 23, lib_le16(p + 2) - 23u);
                j->pfx = lr_tell(&j->old);
                break;
            }
            {
                int r = copy_trk(j, &j->tpfx, j->tpfx_end, t_end);

                if (r < 0)
                    return -1;
                if (!r)
                    return 0;
            }
            if (j->wnext == WS_DECIDE && c->old)  /* the old cursor where this folder left it */
                lr_seek(&j->old, c->old_off), old_next(j, &at);
            else if (j->wnext == WS_DECIDE)
                lr_seek(&j->old, c->before);
            j->wst = (int)j->wnext;
            break;
        }
        case WS_DECIDE:
            c->toff = j->trk.pos;
            j->pfx = c->old_toff;
            if (c->old && c->old_sig == c->sig) {
                j->wst = WS_COPY;
            } else {
                c->tcount = 0;
                c->partial = 0;
                c->changed = 1;
                j->nmatch = 0;
                memset(W_MATCH, 0, W_NMATCH * sizeof(lib_match_t));
                c->last_slot = 0;
                c->done_files = 0;
                j->mmore = 0;
                j->mret = c->nfiles ? WS_BATCH : WS_FEND;
                j->wst = c->old ? WS_MATCH : j->mret;
                if (c->old && fs_seek(j->otrk.f, (long)c->old_toff) >= 0)
                    fs_mark(j->otrk.f);  /* (its reused tracks are read back from here: no walk from
                                          * the old tracks file's first cluster) */
            }
            break;
        case WS_COPY: {                   /* an unchanged folder: its tracks as they were */
            int r = copy_trk(j, &j->pfx, c->old_toff + c->old_tlen, t_end);

            if (r < 0)
                return -1;
            if (!r)
                return 0;
            j->ntracks += c->tcount;
            j->copied += c->tcount;
            j->wst = WS_FEND;
            break;
        }
        case WS_MATCH: {                  /* a changed folder: what it had, to match file by file */
            const uint8_t *p;
            lib_match_t *m;

            if (j->pfx >= c->old_toff + c->old_tlen) {
                j->mmore = 0;
                j->wst = j->mret;
                break;
            }
            if (j->nmatch >= W_NMATCH * 3u / 4u) {   /* the table is full: the rest when the files */
                j->mmore = 1;                       /* get past the last one in it (WS_FILES) */
                j->wst = j->mret;
                break;
            }
            lr_seek(&j->otrk, j->pfx);
            if (!(p = lr_rec(&j->otrk)) || p[0] != LIB_RAW_T) {
                j->pfx = c->old_toff + c->old_tlen;
                break;
            }
            if ((m = match_slot(lib_le32(p + 4), lib_le32(p + 8))) && !m->off) {
                m->nh = lib_le32(p + 4);
                m->clus = lib_le32(p + 8);
                m->size = lib_le32(p + 12);
                m->off = j->pfx + 1u;
                j->nmatch++;
            }
            j->mslot = lib_le16(p + 20);
            j->pfx += lib_le16(p + 2);
            break;
        }
        case WS_BATCH: {                  /* the next files to read: the directory on from the last batch */
            int r = 0;

            if (!j->ds_on) {
                if (!c->done_files)      /* (the folder's first batch: its directory from the start;
                                          * the next ones go on from where this one stops) */
                    r = dir_start(j, 0);
                j->nbatch = j->bi = j->bpool = 0;
                memset(&j->sc, 0, sizeof j->sc);
                j->sc.j = j;
                j->ds_on = 1;
            }
            j->sc.any = c->done_files;
            if (r >= 0 && (r = dir_run(j, batch_cb, t_end)) == 0)
                return 0;
            j->ds_on = 0;
            if (r < 0 || !j->nbatch) {
                j->wst = WS_FEND;
                break;
            }
            j->wst = WS_FILES;
            break;
        }
        case WS_FILES: {                  /* one file: copied, or its tags read */
            lib_bent_t *e = &W_BATCH[j->bi];
            lib_match_t *m;
            int reused = 0;

            if (j->mmore && e->ref.slot > j->mslot) {
                /* past the table's last old track: the next ones in its place. A folder's tracks
                 * are in slot order and a file that did not change kept its slot, so what the
                 * files before this one could still match is behind it. */
                j->nmatch = 0;
                memset(W_MATCH, 0, W_NMATCH * sizeof(lib_match_t));
                j->mret = WS_FILES;
                j->wst = WS_MATCH;
                break;
            }
            m = j->nmatch ? match_slot(e->nh, e->ref.clus) : 0;

            if (m && m->off && m->size == e->ref.size) {
                const uint8_t *p;

                lr_seek(&j->otrk, m->off - 1u);
                p = lr_rec(&j->otrk);
                if (p && p[0] == LIB_RAW_T && lib_le32(p + 16) == e->wt && !((p[27] & T_PARTIAL) && ap_lib_tags_ok())) {
                    uint8_t hd[24];

                    memcpy(hd, p, sizeof hd);
                    lib_put16(hd + 20, e->ref.slot);
                    lw_put(&j->trk, hd, sizeof hd);
                    lw_put(&j->trk, p + sizeof hd, lib_le16(p + 2) - (uint32_t)sizeof hd);
                    j->ntracks++;
                    c->tcount++;
                    j->reused++;
                    reused = 1;
                    if (p[27] & T_PARTIAL)
                        c->partial = 1;
                }
            }
            if (!reused) {
                int p;

                if ((int64_t)(t_end - lib_now_us()) < 1000)
                    return 0;            /* (a file's tags take ~1-2 ms: in the next slice) */
                if ((p = read_tags(j, e, W_TAGS)) < 0) {
                    j->wst = WS_CHAIN;   /* (its far read walked to first, then this file again) */
                    break;
                }
                write_track(j, e, W_TAGS, p);
            }
            c->last_slot = e->ref.slot;
            c->done_files = 1;
            if (++j->bi == j->nbatch)
                j->wst = WS_BATCH;
            if (j->ntracks >= LIB_MAX_TRACKS) {
                j->warn |= LIBW_MAX;
                j->sp = 0;
                j->wst = WS_FEND;
            }
            break;
        }
        case WS_FEND:                     /* the folder's record, now that its tracks are out */
            write_folder(j);
            j->wst = WS_POP;
            break;
        case WS_CONT: {                   /* more subfolders: the directory from the entry after the last pushed */
            lib_scan_t *s = &j->sc;
            int r = 0;

            if (!j->ds_on) {
                memset(s, 0, sizeof *s);
                s->j = j;
                s->base = j->sp;
                s->last = c->cont_slot;
                s->cont = 1;
                s->limit = LIB_PEND - 1u;
                r = dir_start(j, c->cont_slot < 0 ? 0u : (uint32_t)c->cont_slot + 1u);
            }
            if (r >= 0 && (r = dir_run(j, sig_cb, t_end)) == 0)
                return 0;
            j->ds_on = 0;
            if (r >= 0 && s->more)
                push_cont(j, s->base, s->last);
            j->wst = WS_POP;
            break;
        }
        case WS_CHAIN: {                  /* a far read of the file at W_BATCH[bi]: its chain walked to */
            int r = lib_chain_walk(j->tf, j->far, t_end);

            if (r == 0)
                return 0;
            if (r < 0)
                j->far_bad = 1;          /* (a short chain: those reads fail, the rest stands) */
            j->wst = WS_FILES;
            break;
        }
        case WS_END: {
            uint32_t at, crc;

            if (j->lazy) {               /* nothing differed so far: and nothing after? */
                if (old_next(j, &at)) {  /* (folders gone at the end: written without them) */
                    if (unlazy(j, at, WS_END))
                        return -1;
                    break;
                }
                scan_close(j);
                j->wst = WS_DONE;
                return LIB_UNCHANGED;
            }
            if (lw_close(&j->trk))       /* (first: its last big write before anything queued) */
                return -1;
            crc = j->raw_end_crc = j->raw.crc;
            rec_hdr(&j->raw, LIB_RAW_E, 4u + (uint32_t)sizeof(lib_rawend_t) + 8u);
            lw_u32(&j->raw, j->seq);
            lw_u32(&j->raw, j->nfolders);
            lw_u32(&j->raw, j->ntracks);
            lw_u32(&j->raw, j->root_sig);
            lw_u32(&j->raw, crc);
            lw_u32(&j->raw, j->trk.pos);
            lw_u32(&j->raw, j->trk.crc);
            lw_u32(&j->raw, 4u + (uint32_t)sizeof(lib_rawend_t) + 8u);
            lw_u32(&j->raw, LIB_END_MAGIC);
            scan_close(j);
            j->wst = WS_DONE + 1;        /* the folders file: closed in the next slice */
            return 0;
        }
        case WS_CLEAR: {                  /* the slot's folders, tracks, checkpoint files: a piece a pass */
            char p[48];

            raw_path(p, sizeof p, j->dir, j->base, j->bsub, j->slot);
            if (!lib_rm_step(p))
                return 0;
            if (++j->bsub < 3)
                break;
            j->bsub = 0;
            if (open_writers(j, 0))
                return -1;
            j->wst = j->wclr;
            break;
        }
        case WS_DONE + 1:
            if (lw_close(&j->raw))
                return -1;
            j->wst = WS_DONE + 2;        /* its checkpoint: removed in the next slice */
            return 0;
        case WS_DONE + 2: {
            char p[48];

            raw_path(p, sizeof p, j->dir, j->base, 2, j->slot);
            if (!lib_rm_step(p))
                return 0;
            if (j->warn)
                lib_log("lib: %s walk:%s%s%s%s", j->base, j->warn & LIBW_DEPTH ? " folders deeper than 32 left out" : "",
                        j->warn & LIBW_NAMES ? " (some folder names asked of the FS again)" : "",
                        j->warn & LIBW_MAX ? " over 65535 tracks: the rest not indexed" : "",
                        j->warn & LIBW_PARTIAL ? " basic tags only for some (the USB-C earphones had the window)" : "");
            j->wst = WS_DONE;
            return 1;
        }
        default:
            return 1;
        }
    }
    return 0;
}

void scan_abort(lib_job_t *j)
{
    scan_close(j);
    if (j->raw.f)
        lw_abort(&j->raw);
    if (j->trk.f)
        lw_abort(&j->trk);
}
