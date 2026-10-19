/*
 * lib_int.h -- the music library's insides (docs/LIBRARY.md): file formats, the job's pieces and
 * the platform hooks. Everything declared here runs in the library overlay (startup/ap_ovl.c
 * OVL_LIB, window 2) or in the host test (services/tests/test_lib.c), never from resident code:
 * resident code reaches the library through lib.h's gates only.
 *
 * Files, per indexed volume V (0 Internal, 1 SD), in its place P (lib_place_names()):
 *   <base>.RWA / .RWB   the walk's folders ("RAW"): one record per folder, in walk order
 *   <base>.RTA / .RTB   the walk's tracks: each folder's tracks together (its 'F' says where)
 *   <base>.IXA / .IXB   the index ("IDX"): the views, sorted, built from the RAW of the same slot
 *   <base>.RCA / .RCB   the walk's last checkpoint (one 'C' record), while a first walk runs
 *   LIBT0..LIBT5.TMP    the sorter's runs and results (any job; rewritten freely, removed after)
 * Two slots, A and B: a refresh writes the slot that is not current, RAW first then IDX, and the
 * IDX's trailer (written last, closed last) is the commit. A slot counts only when its IDX trailer
 * is whole (magic, CRC) and names its RAW (size and CRC). A power cut anywhere leaves the other
 * slot as it was: fs_target.c builds a rewritten file's chain beside the old one and switches the
 * directory entry at fs_close(). A walk that finds nothing changed writes nothing.
 *
 * RAW records, each [u8 type][u8 0][u16 len] then fields (little-endian, packed):
 *   'F' folder  u32 clus, size, pclus, psize; u16 slot; u8 rflags; u8 depth; u32 sig; u32 parent;
 *               u32 toff, tcount, tlen (its tracks in the tracks file); name (u8 n + bytes).
 *               Written once its tracks are: before its subfolders' (depth first, pre-order).
 *   'C' check   (its own file, .RC?) the walk so far: lib_ckpt_hdr_t, the stack, its names, then
 *               u32 len, u32 LIB_CKPT_MAGIC. After a power cut the walk goes on from it: the folders
 *               and tracks files cut back to the sizes it names (fs_open_append_at).
 *   'E' end     lib_rawend_t, then u32 len, u32 LIB_END_MAGIC
 * Tracks file: 'T' records: u32 nh, clus, size, wt; u16 slot; u8 rflags; u8 kind; u16 trk; u8 disc;
 *   u8 0; u16 year; u16 dur (s); title, artist, album, album artist, genre (u8 n + bytes each).
 *
 * IDX: sections of rows, then the trailer (lib_trailer_t). A rows section is its rows, then
 * (at blk) the offset of every 16th row (u32, relative to the section) and the letter index
 * (28 x u32: the first row of each letter bucket). Rows: lib_row_t's encodings (lib_idx.c).
 */
#ifndef RECHORD_LIB_INT_H
#define RECHORD_LIB_INT_H

#include <stdint.h>

#include "fs.h"
#include "lib.h"

#define LIB_MAX_TRACKS 65535u        /* per volume: the limit is here and nowhere else */
#define LIB_DEPTH      32u           /* folder levels below a volume's root */
#define LIB_PEND       224u          /* folders waiting to be visited (more: a continuation entry) */
#define LIB_SUBS_MAX   160u          /* subfolders pushed per directory scan (more: continued) */
#define LIB_STEP_US    3000u         /* the job's slice per main-loop pass */
#define LIB_STR        255u

/* ---- platform (lib_job.c on the device, test_lib.c on the host) ---- */
uint64_t lib_now_us(void);
int lib_io_ok(void);                 /* storage may be used now (audio has headroom) */
/* This slice queued blocks to Internal's write-behind queue: end it now (a big write after them
 * would first wait for each, ~3 ms a block). LIB_OVER: a loop's test, the clock every 16th row. */
int lib_io_dirty(void);
extern uint32_t lib_slice_wr;        /* fs_target_nwr when the slice began (lib_job.c) */
#define LIB_OVER(k, t_end) ((((k) & 15) == 15 && lib_now_us() >= (t_end)) || lib_io_dirty())
#ifdef LIB_HOST
void lib_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#else
#include "services/log.h"
#define lib_log log_printf
#endif
void lib_serial_forget(void);        /* a card in or out */
uint32_t lib_vol_serial(int vol);    /* volume serial (FAT/exFAT boot sector), 0 if unknown */
extern uint32_t *const lib_scratch;  /* LIB_SCRATCH bytes, word aligned (services/scratch.h) */
#define LIB_SCRATCH 32768u
/* The scratch buffer while the library has it (the USB disk session, the updater and the BT
 * listen test take it whole, from code that holds the main loop: lib_job.c's canary in the last
 * word says so, and the library then drops what it kept here):
 *   [0, 28 KiB)       the job: the walk's or the build's working set (lib_scan.c, lib_build.c)
 *   [28 KiB, 32 KiB)  the views and the lists: the index's sector cache, the lists' buffers */
#define LIB_JOB_SCRATCH 28672u
#define LIB_WBUF 4608u               /* the job's main writers: 9 sectors, past emmc_write_async's
                                      * queue (more than half of it: one CMD25) */
#define LIB_TAIL(off) ((uint8_t *)lib_scratch + LIB_JOB_SCRATCH + (off))
#define LT_IDXC   LIB_TAIL(0)        /* 4 x 512: lib_idx.c's cache */
#define LT_RBUF   LIB_TAIL(2048)     /* 512: lib_m3u.c's reader */
#define LT_WBUF   LIB_TAIL(2560)     /* 512: its writer */
#define LT_LINE   ((char *)LIB_TAIL(3072))   /* 300: a list's line */
#define LT_INFO   ((char *)LIB_TAIL(3372))   /* 300: its #EXTINF */
#define LT_RBUF2  LIB_TAIL(3672)     /* 416: a second reader (lists moved) */
#define LT_END    4088u              /* (the canary: the last word) */
void idx_cache_drop(void);           /* the scratch was taken: what lib_idx.c kept there is gone */
uint32_t lib_crc(uint32_t crc, const void *p, uint32_t n);   /* the updater's CRC-32 (fw_update.h fwu_crc) */

/* ---- formats ---- */
#define F_CHANGED 0x40u              /* a folder record's flags (byte 22): its tracks were read or matched
                                      * again by the walk that wrote it, not copied as the previous
                                      * walk had them (lib_build.c merges only those) */
#define LIB_RAW_F 'F'
#define LIB_RAW_T 'T'
#define LIB_RAW_C 'C'
#define LIB_RAW_E 'E'
#define LIB_CKPT_MAGIC  0x504B4352u  /* "RCKP" */
#define LIB_END_MAGIC   0x4E454352u  /* "RCEN" */
#define LIB_IDX_MAGIC   0x584C4352u  /* "RCLX" */
#define LIB_VERSION     2u           /* 2: track rows carry their genre when it is not their album's
                                      * (kind's bits 6, 7: R_NOGENRE, R_GENRE); ties in ALBTRK by folder
                                      * and slot. Version 1 indexes are read as before (no such rows) */
#define R_NOGENRE 0x40u              /* a track row's kind byte: the track has no genre */
#define R_GENRE   0x80u              /* ... its genre (u8 n + bytes) follows its artist */
#define R_KIND    0x3Fu

enum { LS_ALBTRK, LS_ARTALB, LS_ARTISTS, LS_ALBUMS, LS_GENALB, LS_GENRES, LS_SONGS, LS_FOLDERS, LS_N };
#define LIB_LETTERS 28               /* '#', A..Z, other scripts */
#define LIB_BLK     16u              /* rows per block index entry */

typedef struct {
    uint32_t off, len, rows, blk, crc;   /* crc: of len bytes from off (rows, block and letter index) */
} lib_sec_t;

typedef struct {
    uint32_t magic, version, seq, serial;
    uint32_t ntracks, nfolders, nalbums, nartists, ngenres;
    uint32_t raw_size, raw_crc, root_sig, flags, trk_size;
    lib_sec_t sec[LS_N];
    uint32_t crc;                    /* of everything above */
} lib_trailer_t;

typedef struct {
    uint32_t seq, nfolders, ntracks, root_sig, crc;   /* crc: of every byte before this record */
    uint32_t tsize, tcrc;                             /* the tracks file */
} lib_rawend_t;

/* A folder waiting on the walk's stack. */
typedef struct {
    fs_ref_t ref;
    uint32_t parent;                 /* its parent's folder index */
    uint16_t name, nlen;             /* in the name stack; nlen 0xFFFF: a continuation of folder `parent` */
    uint16_t cont;                   /* continuation: subfolders after this slot */
    uint8_t depth, pad;
} lib_pend_t;

typedef struct {
    uint32_t seq, nfolders, ntracks, old_off, crc, tsize, tcrc, read_tags, root_sig;
    uint32_t rsize;                  /* the folders file's size (crc: its CRC) */
    uint16_t sp, names;
} lib_ckpt_hdr_t;

/* ---- buffered streams (lib_io.c) ---- */
typedef struct {
    fs_file_t *f;
    uint8_t *buf;
    uint32_t cap, n, pos, crc;
    int err, due;                    /* due: within LIB_WSLACK of full (lib_wdue said so) */
} lw_t;
/* A big writer (cap > 8 sectors: its flush is one multi-block write, past the queue) within a record of
 * full ends the slice (lib_io_dirty): its flush then comes early in the next slice, with the queue empty,
 * not after a slice's 3 ms of work (that write, and the FAT blocks its clusters queued before it, then
 * took the slice past 6 ms: m46 scale 30k). Cleared at each slice's start (lib_job.c). */
#define LIB_WSLACK 1152u             /* the longest record (a track's: 5 strings) */
extern uint8_t lib_wdue;
int lw_open(lw_t *w, const char *path, void *buf, uint32_t cap, int append);
int lw_open_at(lw_t *w, const char *path, void *buf, uint32_t cap, uint32_t at);   /* append from `at` */
extern uint8_t lib_sd_wrote;
#define LIB_RM_CLUS 96u
int lib_rm_step(const char *path);   /* 1 = the file is gone, 0 = a piece removed (more passes) */         /* lw_open* on the card since lib_step's last fs_target_sync */
void lw_put(lw_t *w, const void *p, uint32_t n);
void lw_u8(lw_t *w, uint32_t v);
void lw_u16(lw_t *w, uint32_t v);
void lw_u32(lw_t *w, uint32_t v);
int lw_flush(lw_t *w);
int lw_close(lw_t *w);              /* 0, or < 0: the file is not what was written */
void lw_abort(lw_t *w);

typedef struct {
    fs_file_t *f;                    /* own handle, or shared (merge cursors) */
    uint8_t *buf;
    uint32_t cap, n, i;              /* buf[i..n) are file bytes [base + i, base + n) */
    uint32_t base, end;              /* end: where this stream stops */
    int own;
    uint32_t pl[2];                  /* shared handle: this stream's place in the chain (fs_place_*) */
} lr_t;
int lr_open(lr_t *r, const char *path, void *buf, uint32_t cap);
int lr_open_ref(lr_t *r, const fs_ref_t *ref, void *buf, uint32_t cap);
void lr_attach(lr_t *r, fs_file_t *f, uint32_t start, uint32_t end, void *buf, uint32_t cap);
void lr_seek(lr_t *r, uint32_t off);
uint32_t lr_tell(const lr_t *r);
const uint8_t *lr_peek(lr_t *r, uint32_t n);  /* n contiguous bytes (n <= cap), NULL at the end */
int lr_get(lr_t *r, void *dst, uint32_t n);
void lr_close(lr_t *r);
/* The next record ([u8 type][u8][u16 len]...): a pointer to all len bytes, NULL at the end. */
const uint8_t *lr_rec(lr_t *r);
const uint8_t *lr_rec16(lr_t *r);   /* a sort record: [u16 len]... */
const uint8_t *lr_rec_raw(lr_t *r); /* a RAW record, checkpoints passed over */

static inline uint32_t lib_le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t lib_le32(const uint8_t *p) { return lib_le16(p) | lib_le16(p + 2) << 16; }
static inline void lib_put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void lib_put32(uint8_t *p, uint32_t v) { lib_put16(p, v); lib_put16(p + 2, v >> 16); }

/* ---- collation (lib_coll.c) ---- */
#define LC_ARTIST 1                  /* a leading "The " does not count */
/* < 0, 0, > 0: case and Latin accents folded, numbers by value, non-Latin by code point; an empty
 * string after everything. 0 means "the same name" (grouping). */
int lib_coll(const uint8_t *a, unsigned la, const uint8_t *b, unsigned lb, int flags);
/* The letter bucket of a name: 0 '#', 1..26 A..Z, 27 other. */
int lib_letter(const uint8_t *s, unsigned n, int flags);
uint32_t lib_fnv(uint32_t h, const void *p, uint32_t n);
#define LIB_FNV0 2166136261u

/* ---- the sorter (lib_sort.c): variable-length records [u16 len]..., any comparator ---- */
typedef int (*lib_cmp_t)(const uint8_t *a, const uint8_t *b);
#define LIB_RUNS 256u
typedef struct {
    lib_cmp_t cmp;
    int phase, tmp_in, tmp_out, result;   /* result: the temp file holding the sorted records */
    uint32_t nrec, used, nofs;            /* the run being filled: records, bytes, entries */
    uint32_t heap_i, heap_n, heap_ph;     /* the resumable heapsort */
    uint32_t emit;                        /* next sorted record to write */
    uint32_t run[LIB_RUNS + 1], nruns;    /* run starts in tmp_in (run[nruns] = end) */
    uint32_t nrun2, grp;                  /* merge: runs written this level, next group */
    uint32_t total;                       /* records sorted */
    lw_t w;
    int k;                                /* cursors in the group being merged */
    uint32_t done_in;                     /* the input is over */
} lib_sort_t;
void srt_begin(lib_sort_t *s, lib_cmp_t cmp, int tmp_a, int tmp_b);
/* Room for a record of n bytes in the run being filled: a pointer, or NULL (the run is full:
 * call srt_step until it asks for input again). */
uint8_t *srt_room(lib_sort_t *s, uint32_t n);
void srt_commit(lib_sort_t *s, uint32_t n);
void srt_end_input(lib_sort_t *s);
/* Work until t_end: 1 = wants input (after srt_begin or a run written), 2 = done (s->result),
 * 0 = more to do, < 0 = I/O error. */
int srt_step(lib_sort_t *s, uint64_t t_end);
int srt_wants_input(const lib_sort_t *s);
void srt_abort(lib_sort_t *s);

/* ---- places and temp files (lib_job.c) ---- */
void lib_tmp_path(char *out, int n);         /* "<dir>/LIBTn.TMP" in the current job's place */
const char *lib_job_dir(void);
int lib_tmp_ref(int n, fs_ref_t *ref);       /* the temp file's ref after its writer closed */

/* ---- directory reads (lib_dir.c, in the walk's overlay) and chain walks (lib_fsx.c) in pieces ---- */
/* A directory read over several slices: the file system's own cursor, kept between them, so a
 * folder of thousands of entries is read a slice's worth at a time and never again from its start. */
typedef struct { void *p; uint32_t w[11]; } lib_dscan_t;   /* (lib_dir.c: the cursor and its folder) */
/* Its start: a volume's root by path (root), else the folder dir, from entry index `from` on (the
 * entries before it skipped by cluster arithmetic, not read). 0, or < 0 (the volume is gone). */
int lib_dscan_start(lib_dscan_t *s, const char *root, const fs_ref_t *dir, uint32_t from);
/* Entries from where it stopped, each to cb as fs_scan's: 2 = cb said stop (that entry is read
 * again by the next call), 1 = the directory's end, 0 = t_end came (more), < 0 = the volume went. */
int lib_dscan(lib_dscan_t *s, fs_scan_cb cb, void *u, uint64_t t_end);
uint32_t lib_dscan_wtime(void);      /* inside its callback: the entry's write time */
int lib_ref_name(const fs_ref_t *r, char *out);   /* its name (<= 255 bytes): 0, or < 0 (gone) */
/* A file of small clusters is read near its end (tags at the end: APE, MP3's ID3v1, WavPack): the
 * cursor has to get there through the FAT, one FAT sector per 128 clusters (16.5 MB at 512-byte
 * clusters: 252 sectors, 133 ms on SD NAND in one slice). A read whose walk is longer than
 * LIB_CHAIN_FAR clusters is refused by the walk's reader and done first a slice at a time. */
#define LIB_CHAIN_FAR  1024u         /* clusters: 8 FAT sectors */
#define LIB_CHAIN_STEP 512u          /* clusters between tests of the slice's end */
/* Seeked to off: the clusters a read there walks through the FAT (0: contiguous, or near); the
 * cursor first moved to the file's mark when that is nearer. */
uint32_t lib_chain_gap(fs_file_t *f, uint32_t off);
/* The cursor walked toward off and marked there: 1 = there, 0 = more (t_end), < 0 = the chain ended. */
int lib_chain_walk(fs_file_t *f, uint32_t off, uint64_t t_end);

/* ---- the walk (lib_scan.c) and the build (lib_build.c) ---- */
typedef struct {                     /* a directory scan of the walk's (its callback's state) */
    struct lib_job *j;
    uint32_t base, pushed, limit;
    int32_t last;                    /* the last pushed subfolder's slot */
    int cont, more, any;             /* a continuation's scan; subfolders left for one; files after last_slot */
} lib_scan_t;
typedef struct {                     /* the folder being visited */
    fs_ref_t ref;
    uint32_t idx, parent, sig, old_off, old_sig, old_toff, old_tlen, toff, tcount, before;
    uint16_t nlen, last_slot, nfiles;
    uint8_t depth, old, cont, root, done_files, at_cursor, partial, changed;
    int32_t cont_slot;
} lib_cur_t;

struct lib_job {
    int vol, full, slot, state;
    uint32_t seq;                    /* of the slot being written */
    char root[12];                   /* "/Internal", "/SD" */
    char dir[24];                    /* its place (lib_place): "/Internal/RECHORD", or a root */
    char base[10];                   /* "INT", "SD", or the card's serial in hex */
    /* the walk */
    lw_t raw, trk;                   /* the new RAW: folders, tracks */
    lr_t old, otrk;                  /* the previous slot's (old_ok) */
    uint32_t old_ok, lazy, pfx, pfx_end, tpfx, tpfx_end, t_sync, wnext;
    uint32_t f_left;                 /* the build's S1 producer: tracks of its folder still to read */
    uint32_t f_skip;                 /* ... a merge's: where they start + 1, its chain still to walk */
    fs_ref_t f_ref;                  /* ... and that folder */
    uint32_t nfolders, ntracks, read_tags, reused, copied, since_ckpt, root_sig;
    uint32_t sp, names;              /* the pending stack, the name stack's top */
    int wst;                         /* walk state */
    int wclr;                        /* WS_CLEAR: the state after the slot's files are removed */
    lib_cur_t cur;
    uint32_t nbatch, bi, bpool;      /* files of the folder being read: count, next, name bytes */
    uint32_t nmatch;
    uint32_t mslot;                  /* the reuse table: the slot of the last old track in it */
    int mret, mmore;                 /* ... the state after filling it; old tracks left for it */
    uint32_t warn;                   /* LIBW_* seen (logged once) */
    lib_scan_t sc;                   /* the directory scan in progress (ds_on), over slices */
    lib_dscan_t ds;
    int ds_on;
    fs_file_t *tf;                   /* the file whose tags are read (kept while its chain is walked) */
    uint32_t far;                    /* ... the far offset its reader asked for (WS_CHAIN walks there) */
    int far_bad;                     /* ... its chain ended early: far reads fail */
    /* the build */
    int bph, bsub;
    lib_sort_t srt;
    lw_t idx;
    lr_t in;
    uint32_t rows, nblk, letter[LIB_LETTERS];
    uint32_t n_alb, n_art, n_gen, sec_start;
    lib_trailer_t tr;
    int s1, s2, ta;                  /* temp files: S1 sorted, S2 sorted, album rows */
    uint32_t grp_first, grp_n, grp_first2, grp_dur, grp_dur2, grp_year;
    uint32_t ctr, sec_crc, raw_end_crc;   /* records counted by a producer, a section's CRC, the RAW's */
    uint16_t path_off[LIB_DEPTH + 2];
    /* an edit merged into the previous index (lib_build.c, j->inc) */
    int inc, s5, a1, merr;
    uint32_t n5, n_add, n_d, n_rm, n_ins;   /* folders sorted, tracks to add, D, removed rows, added rows */
    uint32_t m_oi, m_on, m_af, m_an, m_have;   /* the old rows: next, count; its album's range; one waiting */
    uint32_t m_si, m_sn, m_lo, m_hi, m_ob, m_obn;   /* SONGS: next old entry, count; the search; its letter */
    int m_pos;                              /* the waiting added entry's place in the old SONGS (-1: to find) */
    uint8_t grp_kl[3];               /* the group being collected: its names' lengths (lib_build.c B_KEY) */
    int warm;                        /* the view's chain places known (idx_warm) */
    /* stats for the log and the tests */
    uint64_t t_start, t_busy;
    uint32_t steps, worst_us, reads0, deferred, nrd0, nwr0;
};
typedef struct lib_job lib_job_t;
#define LIBW_DEPTH 1u
#define LIBW_PEND  2u
#define LIBW_MAX   4u
#define LIBW_NAMES 8u
#define LIBW_PARTIAL 16u
/* scan_step: the walk found nothing changed (no RAW written, no build) */
#define LIB_UNCHANGED 2
/* scan_start: the walk of this seq is already whole (a cut in the build): build it */
#define LIB_WALKED 3
int scan_start(lib_job_t *j);
int scan_step(lib_job_t *j, uint64_t t_end);   /* 0 more, 1 done, LIB_UNCHANGED, < 0 failed */
void scan_abort(lib_job_t *j);
int build_start(lib_job_t *j);
int build_step(lib_job_t *j, uint64_t t_end);  /* 0 more, 1 committed, < 0 failed */

/* ---- reading an index (lib_idx.c) ---- */
typedef struct {
    uint8_t kind, disc, flags;
    uint16_t trk, dur, year;         /* dur: seconds */
    fs_ref_t ref;
    uint32_t a, b, c, d;             /* album/artist/genre rows: first, count, first2, count2 */
    const char *s1, *s2, *s3;        /* title / name, artist, album (genre for albums) */
    uint8_t l1, l2, l3;
} lib_row_t;

/* One volume's committed index, as the views read it. */
typedef struct {
    int ok, slot;
    char dir[24], base[12];          /* where it was read from (lib_place_names, or the other place) */
    uint32_t size;
    lib_trailer_t tr;
} lib_view_t;
extern lib_view_t g_lib_view[2];
int idx_load(int vol);              /* the newest whole slot into g_lib_view[vol]: 0, or < 0 */
void idx_drop(int vol);
uint32_t idx_rows(int vol, int sec);
int idx_row(int vol, int sec, uint32_t i, lib_row_t *r, uint8_t *buf, uint32_t cap);
uint32_t idx_letter(int vol, int sec, int letter);   /* first row of that bucket */
/* The folder path (volume-relative, "" for its root) of the folder whose first cluster is clus. */
int idx_folder_path(int vol, uint32_t clus, char *out, int n);
/* Read len bytes at off of volume vol's IDX (cached). */
int idx_read(int vol, uint32_t off, void *dst, uint32_t len);
fs_file_t *idx_file(int vol);       /* the view's IDX handle (shared: streams keep their places) */
uint32_t lib_clus_bytes(fs_file_t *f);   /* lib_fsx.c */
int lib_chain_places(fs_file_t *f, uint32_t (*pl)[2], int n, uint32_t part, uint64_t t_end);
/* The view's chain places for every part of its IDX, a step a slice (a job's first slices): then a
 * read anywhere in it walks at most a part's clusters. 1 = done (or nothing to do), 0 = more. */
int idx_warm(int vol, uint64_t t_end);

/* Track rows: encode / decode (lib_build.c writes, lib_idx.c reads). */
uint32_t row_track_put(uint8_t *o, const lib_row_t *r);
#define ROW_REF_BYTES 19
void ref_put(uint8_t *o, const fs_ref_t *r);
void ref_get(const uint8_t *p, fs_ref_t *r, int vol);

#endif
