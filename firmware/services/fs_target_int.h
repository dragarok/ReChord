/*
 * fs_target_int.h -- what fs_target.c (resident: mount, reads, the FAT32/FAT16 8.3 writer) shares
 * with fs_write_ext.c (the "fsw" overlay in the second window, services/ovl.h: exFAT writes, long
 * names, mkdir, rename). Not a public API: fs.h / fs_target.h are.
 *
 * The overlay calls the resident helpers below directly (the linker checks every call); resident
 * code reaches the overlay only through fsx_api() (startup/ap_ovl.c: ovl_get(OVL_FSW); the host
 * tests link the functions directly).
 */
#ifndef RECHORD_FS_TARGET_INT_H
#define RECHORD_FS_TARGET_INT_H

#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "fs_target.h"

#define T_FAT16 16
#define T_FAT32 32
#define T_EXFAT 64

#define FREE_UNKNOWN 0xFFFFFFFFu
#define FAT_EOC      0x0FFFFFFFu         /* (FAT16: 0xFFFF on disk; fat_get/fat_set translate) */

typedef struct {
    char name[12];
    fs_block_read_t rd;
    fs_block_write_t wr;             /* NULL: read-only */
    uint32_t base;                   /* volume boot sector, device LBA */
    int type;                        /* 0 = not mounted */
    uint32_t fat, data, spc, root, root_secs, nclus;
    uint32_t nfats, fatsz;
    uint32_t fsi, free, hint;        /* FSInfo sector (0: none), its two fields */
    int fsi_dirty;
    uint32_t serial;                 /* volume serial (FAT32 0x43, FAT16 0x27, exFAT 0x64) */
    uint16_t gen;                    /* +1 at every unmount: handles from before answer FS_ERR_GONE */
    uint16_t mnt;                    /* +1 at every mount: the overlay's caches of it are stale */
    uint8_t xdirty;                  /* exFAT: we set VolumeDirty on the card (cleared at the last close) */
    uint8_t xbad;                    /* exFAT: its bitmap / up-case table could not be used: read-only */
    uint32_t bmp;                    /* exFAT: allocation bitmap's first sector (volume LBA; 0 = not found yet) */
    uint32_t upc, upc_len;           /* exFAT: up-case table's first cluster, bytes */
} vol_t;

typedef struct {                     /* directory cursor */
    const vol_t *v;
    uint32_t clus, sec, off, left;   /* left: clusters, contiguous dirs only */
    int contig;
    uint32_t lba;                    /* sector of the entry last returned */
    uint32_t idx;                    /* entries returned so far (slot = idx - 1) */
} cur_t;

typedef struct {                     /* a directory entry or the root */
    char name[FS_NAME_MAX];
    int is_dir, contig;
    uint32_t clus, size;
    uint32_t slot;                   /* index of its (primary) entry in the directory */
    uint32_t dlba, doff;             /* FAT: where the 8.3 entry lives; exFAT: its 0x85 entry */
    uint32_t wtime;                  /* last write: FAT date << 16 | time (exFAT: the same layout) */
    uint8_t attr;
    uint8_t raw[11], nlfn;           /* 8.3 name; FAT: long-name entries before it; exFAT: entries in its set */
    cur_t lfn;                       /* cursor at the first of those (exFAT: at the 0x85 entry) */
} ent_t;

struct fs_file {
    const vol_t *v;
    uint32_t first, clus, cidx, size, pos;
    uint32_t mark_clus, mark_cidx;   /* fs_mark(): where backward walks restart (0: the start) */
    uint32_t dlba;                   /* 8.3 entry sector (FAT); exFAT: the 0x85 entry's */
    uint16_t doff;
    uint16_t gen;                    /* its volume's gen when opened */
    uint8_t used, contig, wr, sum;   /* wr: write handle; sum: 8.3 checksum; contig: exFAT NoFatChain */
    uint8_t cut;                     /* fs_open_append_at short of the end: old = the clusters past it */
    /* write handles: clus = cluster holding byte pos-1 (0: none yet);
     * head = first cluster allocated here, link = cluster it hangs off at
     * close (append), old = chain freed at close (rewrite). */
    uint32_t head, link, old;
    uint32_t old_size;               /* exFAT: the old chain's length in bytes, and whether contiguous */
    uint8_t old_contig, nset;        /* exFAT: entries in its set */
    cur_t ecur;                      /* exFAT: cursor at its 0x85 entry (fsx_close re-reads the set there) */
};

/* The overlay's entry points (fs_write_ext.c), reached through fsx_api() (NULL: refused). */
enum { FSX_REMOVE, FSX_MKDIR, FSX_RENAME, FSX_STAT };
typedef struct {
    int (*create)(vol_t *v, const char *leaf, size_t n, uint8_t attr, uint32_t clus);
    int (*grow)(struct fs_file *f);
    int (*close)(struct fs_file *f);
    int (*open)(struct fs_file *f, int mode);   /* exFAT write handle on s_ent: 0 rewrite, 1 append,
                                                 * 2 just created (FAT: nothing to do) */
    int (*path)(int op, const char *a, const char *b, fs_stat_t *st);   /* FSX_*: fs_remove .. fs_stat */
    int (*clean)(vol_t *v);
} fsx_api_t;
const fsx_api_t *fsx_api(void);

/* ---- shared state and helpers (fs_target.c) ---- */
#define s_vol        fst_vol
#define s_files      fst_files
#define s_sec        fst_sec
#define s_sec_lba    fst_sec_lba
#define s_sec_vol    fst_sec_vol
#define s_sec_dirty  fst_sec_dirty
#define s_fsec_dirty fst_fsec_dirty
#define s_ent        fst_ent
#define s_pent       fst_pent
#define s_u16        fst_u16
#define s_parent     fst_parent
#define s_leaf_missing fst_leaf_missing

extern vol_t s_vol[FS_VOLUMES];
#ifndef FS_HANDLES
#define FS_HANDLES 8                 /* (fs_target.c: who holds them) */
#endif
extern struct fs_file s_files[FS_HANDLES];
extern uint32_t s_sec[128], s_sec_lba;
extern const vol_t *s_sec_vol;
extern int s_sec_dirty, s_fsec_dirty;
extern ent_t s_ent, s_pent;          /* resolve(): the entry found; the directory it was looked up in */
extern uint16_t s_u16[256];
extern uint32_t s_parent;            /* resolve(): dir of the last component */
extern int s_leaf_missing;           /* resolve(): only the last one missing */

static inline uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static inline void put16(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static inline void put32(uint8_t *p, uint32_t x) { put16(p, x); put16(p + 2, x >> 16); }

int fst_sec_flush(void);
uint8_t *fst_sector(const vol_t *v, uint32_t lba);
int fst_fat_flush(void);
uint8_t *fst_fat_sector(const vol_t *v, uint32_t lba);
uint32_t fst_fat_next(const vol_t *v, uint32_t c);
uint32_t fst_fat_get(const vol_t *v, uint32_t c);
int fst_fat_set(const vol_t *v, uint32_t c, uint32_t val);
uint32_t fst_clus_alloc(vol_t *v, uint32_t from, uint32_t prev);
int fst_chain_free(vol_t *v, uint32_t c);
int fst_writable(const vol_t *v);
int fst_fsi_flush(vol_t *v);
uint32_t fst_now_dt(void);
void fst_cur_init(cur_t *d, const vol_t *v, const ent_t *dir);
uint8_t *fst_cur_next(cur_t *d);
int fst_dir_read(cur_t *d, ent_t *e);   /* the next entry into e: 1, or 0 at the end (lib_dir.c too) */
uint8_t fst_sfn_sum(const uint8_t *p);
const char *fst_component(const char *p, size_t *n);
int fst_resolve(const char *path, const vol_t **pv);
int fst_busy(const vol_t *v);
int fst_io_err(void);

/* the short names both files use */
#define sec_flush   fst_sec_flush
#define sector      fst_sector
#define fat_flush   fst_fat_flush
#define fat_sector  fst_fat_sector
#define fat_next    fst_fat_next
#define fat_get     fst_fat_get
#define fat_set     fst_fat_set
#define clus_alloc  fst_clus_alloc
#define chain_free  fst_chain_free
#define free_add    fst_free_add
#define fsi_flush   fst_fsi_flush
#define now_dt      fst_now_dt
#define cur_init    fst_cur_init
#define cur_next    fst_cur_next
#define dir_read    fst_dir_read
#define sfn_sum     fst_sfn_sum
#define sfn_make    fst_sfn_make
#define component   fst_component
#define resolve     fst_resolve
#define busy        fst_busy
#define writable    fst_writable
#define io_err      fst_io_err

static inline uint32_t clus_lba(const vol_t *v, uint32_t c)
{
    return v->data + (c - 2u) * v->spc;
}

#endif
