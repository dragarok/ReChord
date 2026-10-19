/*
 * fs_target.c -- FAT16 / FAT32 (long names) / exFAT backend for fs.h on the
 * device. Volumes are registered by fs_target_mount(); paths are
 * "/<volume>/dir/file", "/" lists the volumes. Names are reported as UTF-8;
 * matching is ASCII case-insensitive (an 8.3 alias matches too).
 *
 * Writes: FAT32, FAT16 and exFAT volumes mounted with a block writer (read-only
 * mounts answer FS_ERR_UNSUPPORTED). Here, resident: the FAT writer for 8.3
 * names (create, rewrite, append, remove); existing files (long names
 * included) can be rewritten, appended and removed. The cold parts -- exFAT
 * writes, new long names, mkdir, rename -- are fs_write_ext.c, the "fsw"
 * overlay in the second window, reached through fsx_api() (fs_target_int.h).
 * Crash order: data, then the FAT chain (both copies), then the directory
 * entry, then freeing the old chain, then FSInfo -- a power cut leaves at
 * worst lost clusters (or, mid append, a chain longer than the size), never
 * an entry pointing at unwritten data or a cross-link. A rewrite builds the
 * new chain beside the old one, so the old content stays intact until the
 * entry is switched at fs_close(). fs_write_ext.c keeps the same order on
 * exFAT (the allocation bitmap where FAT has its chain).
 *
 * A volume unmounted while handles are open on it (the SD card pulled) keeps
 * them allocated but dead: reads and writes answer FS_ERR_GONE until their
 * owner closes them, so no handle silently comes back on another file. A
 * block reader answering FS_BLK_GONE (the medium is gone) shows as
 * FS_ERR_GONE too.
 *
 * Written from the FAT / exFAT on-disk format, no third-party code.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs.h"
#include "fs_target.h"

#include "fs_target_int.h"

vol_t s_vol[FS_VOLUMES];
/* Open files at once: the audio engine holds two (the track and the next), the library's job up to
 * four while it indexes (old and new folders and tracks files, or a sorter's input and output and
 * the index being written), its views one, and a passing log append, settings save or tag read. */
struct fs_file s_files[FS_HANDLES];
uint32_t s_sec[128], s_sec_lba;
const vol_t *s_sec_vol;
int s_sec_dirty;
ent_t s_ent, s_pent;
uint16_t s_u16[256];
uint32_t s_parent;                   /* resolve(): dir of the last component */
int s_leaf_missing;                  /* resolve(): only the last one missing */
static uint32_t (*s_clock)(void);
static int s_blk_err;                /* the last failed block call's answer (FS_BLK_GONE: medium gone) */
static fs_stat_t s_stat;             /* fs_scan_stat(): the entry a scan reports */
uint32_t fs_target_nrd, fs_target_nwr;   /* sectors read / written so far (the library's refresh cost) */

/* A block call's answer, the last error remembered (io_err). */
static int blk(int r)
{
    if (r)
        s_blk_err = r;
    return r;
}

/* FS_ERR_GONE when the last block call failed because the medium went away, else FS_ERR_IO. */
int io_err(void)
{
    return s_blk_err == FS_BLK_GONE ? FS_ERR_GONE : FS_ERR_IO;
}

/* Write back the cached data sector if a write left it dirty. */
int sec_flush(void)
{
    if (s_sec_dirty) {
        s_sec_dirty = 0;
        fs_target_nwr++;
        if (blk(s_sec_vol->wr(s_sec_vol->base + s_sec_lba, 1, s_sec))) {
            s_sec_vol = NULL;
            return FS_ERR_IO;
        }
    }
    return 0;
}

/* ponytail: one cached sector for everything; multi-sector reads when the
 * audio path needs throughput. lba is relative to the volume. Writers mark
 * it s_sec_dirty; it is written back before it is replaced. */
uint8_t *sector(const vol_t *v, uint32_t lba)
{
    if (v != s_sec_vol || lba != s_sec_lba) {
        if (sec_flush())
            return NULL;
        s_sec_vol = NULL;
        fs_target_nrd++;
        if (blk(v->rd(v->base + lba, 1, s_sec)))
            return NULL;
        s_sec_vol = v;
        s_sec_lba = lba;
    }
    return (uint8_t *)s_sec;
}

/* ---- clusters ---- */

/* FAT sectors get their own cache: with the data path sharing one sector, a
 * file read re-fetched the FAT sector for every 1-sector cluster. Allocation
 * dirties it; fat_flush() writes it to every FAT copy. */
/* [1]: a second FAT sector, for lookups only (fat_next): read beside the cached one while that one is
 * dirty, not over it. A file read while another is written in another part of the FAT then no longer
 * writes the FAT sector out at every turn (two blocks, one per FAT, that the writer's next
 * multi-block write waits for: m46 scale 30k, slices of 6.0-6.4 ms with 13 sectors written). It never
 * holds the sector [0] holds: [0] taking a sector drops it from [1]. */
static struct {
    uint32_t b[128], lba;
    const vol_t *v;
} s_fc[2];
#define s_fsec     s_fc[0].b
#define s_fsec_lba s_fc[0].lba
#define s_fsec_vol s_fc[0].v
int s_fsec_dirty;

/* (exFAT: its one FAT, or an allocation bitmap sector: nfats is 1) */
int fat_flush(void)
{
    if (s_fsec_dirty) {
        const vol_t *v = s_fsec_vol;

        s_fsec_dirty = 0;
        for (uint32_t k = 0; k < v->nfats; k++)
            if (fs_target_nwr++, blk(v->wr(v->base + s_fsec_lba + k * v->fatsz, 1, s_fsec))) {
                s_fsec_vol = NULL;
                return FS_ERR_IO;
            }
    }
    return 0;
}

/* ro: a lookup (into [1] while [0] is dirty with another sector) */
static uint8_t *fat_cache(const vol_t *v, uint32_t lba, int ro)
{
    int k = ro && s_fsec_dirty && (v != s_fsec_vol || lba != s_fsec_lba);

    if (v != s_fc[k].v || lba != s_fc[k].lba) {
        if (!k) {
            if (fat_flush())
                return NULL;
            if (s_fc[1].lba == lba)
                s_fc[1].v = NULL;            /* (this one is the copy that changes now) */
        }
        s_fc[k].v = NULL;
        fs_target_nrd++;
        if (blk(v->rd(v->base + lba, 1, s_fc[k].b)))
            return NULL;
        s_fc[k].v = v;
        s_fc[k].lba = lba;
    }
    return (uint8_t *)s_fc[k].b;
}

uint8_t *fat_sector(const vol_t *v, uint32_t lba)
{
    return fat_cache(v, lba, 0);
}

/* Next cluster in the chain, 0 at the end or on error. */
uint32_t fat_next(const vol_t *v, uint32_t c)
{
    const uint8_t *b;

    if (v->type == T_FAT16) {
        if (!(b = fat_cache(v, v->fat + c / 256u, 1)))
            return 0;
        c = le16(b + c % 256u * 2u);
    } else {
        if (!(b = fat_cache(v, v->fat + c / 128u, 1)))
            return 0;
        c = le32(b + c % 128u * 4u);
        if (v->type == T_FAT32)
            c &= 0x0FFFFFFFu;
    }
    return c >= 2u && c < v->nclus + 2u ? c : 0u;
}

/* ---- FAT writes ---- */

/* FAT entry as FAT32's 28 bits (FAT16 0xFFF7.. come back as 0x0FFFFFF7..: bad / end of chain
 * alike); 0xFFFFFFFF on I/O error. FAT only: exFAT's writer keeps its own (fs_write_ext.c). */
uint32_t fat_get(const vol_t *v, uint32_t c)
{
    const uint8_t *b;
    uint32_t e;

    if (v->type == T_FAT16) {
        if (!(b = fat_sector(v, v->fat + c / 256u)))
            return 0xFFFFFFFFu;
        e = le16(b + c % 256u * 2u);
        return e >= 0xFFF7u ? e | 0x0FFF0000u : e;
    }
    if (!(b = fat_sector(v, v->fat + c / 128u)))
        return 0xFFFFFFFFu;
    return le32(b + c % 128u * 4u) & 0x0FFFFFFFu;
}

/* val as fat_get gives it (FAT_EOC = end of chain on both types); FAT only, as fat_get. */
int fat_set(const vol_t *v, uint32_t c, uint32_t val)
{
    uint8_t *b;

    if (v->type == T_FAT16) {
        if (!(b = fat_sector(v, v->fat + c / 256u)))
            return FS_ERR_IO;
        put16(b + c % 256u * 2u, val);
    } else {
        if (!(b = fat_sector(v, v->fat + c / 128u)))
            return FS_ERR_IO;
        b += c % 128u * 4u;
        put32(b, (le32(b) & 0xF0000000u) | val);
    }
    s_fsec_dirty = 1;
    return 0;
}

static void free_add(vol_t *v, int d)
{
    if (v->free != FREE_UNKNOWN) {
        v->free += (uint32_t)d;
        if (v->free > v->nclus)
            v->free = FREE_UNKNOWN;
    }
    v->fsi_dirty = 1;
}

/* First free cluster from `from` on (wrapping), marked end-of-chain and
 * hung off `prev` (0: none). 0 when the volume is full or on I/O error.
 * prev is linked before c is marked: when they sit in different FAT
 * sectors, prev's sector is then written back once, not twice. */
uint32_t clus_alloc(vol_t *v, uint32_t from, uint32_t prev)
{
    uint32_t c = from;

    for (uint32_t i = 0; i < v->nclus; i++, c++) {
        uint32_t e;

        if (c < 2u || c >= v->nclus + 2u)
            c = 2u;
        if ((e = fat_get(v, c)) == 0xFFFFFFFFu)
            return 0;
        if (e == 0u) {
            if ((prev && fat_set(v, prev, c)) || fat_set(v, c, FAT_EOC))
                return 0;
            v->hint = c + 1u < v->nclus + 2u ? c + 1u : 2u;
            free_add(v, -1);
            return c;
        }
    }
    return 0;
}

int chain_free(vol_t *v, uint32_t c)
{
    for (uint32_t i = 0; c >= 2u && c < v->nclus + 2u && i < v->nclus; i++) {
        uint32_t next = fat_get(v, c);

        if (next == 0xFFFFFFFFu || fat_set(v, c, 0))
            return FS_ERR_IO;
        free_add(v, 1);
        c = next;
    }
    return fat_flush();
}

int fsi_flush(vol_t *v)
{
    uint8_t *b;

    if (!v->fsi_dirty || !v->fsi)
        return 0;
    v->fsi_dirty = 0;
    if (!(b = sector(v, v->fsi)))
        return FS_ERR_IO;
    put32(b + 488, v->free);
    put32(b + 492, v->hint);
    s_sec_dirty = 1;
    return sec_flush();
}

int writable(const vol_t *v)
{
    return v->wr && !v->xbad;
}

/* ---- mount ---- */

static int is_boot(const uint8_t *b)
{
    return (b[0] == 0xEB || b[0] == 0xE9) &&
           (memcmp(b + 3, "EXFAT   ", 8) == 0 || le16(b + 0x0B) == 512u);
}

static void drop_caches(void)
{
    s_sec_vol = s_fc[0].v = s_fc[1].v = NULL;
    s_sec_dirty = s_fsec_dirty = 0;
}

int fs_target_mount_rw(int idx, const char *name, fs_block_read_t rd,
                       fs_block_write_t wr, uint32_t base)
{
    vol_t *v;
    const uint8_t *b;

    if (idx < 0 || idx >= FS_VOLUMES || !rd)
        return FS_ERR_ARG;
    v = &s_vol[idx];
    /* the volume may have changed under us (USB host): forget cached
     * sectors, and turn write handles on it into dead ones (fs_close then
     * only releases them). Read handles stay: their owners still hold them. */
    uint16_t gen = v->gen, mnt = v->mnt;

    for (unsigned i = 0; i < sizeof s_files / sizeof s_files[0]; i++)
        if (s_files[i].used && s_files[i].v == v)
            s_files[i].wr = 0;
    drop_caches();
    memset(v, 0, sizeof *v);
    v->gen = gen;
    v->mnt = (uint16_t)(mnt + 1u);
    v->rd = rd;
    v->base = base;
    if (!(b = sector(v, 0)))
        return FS_ERR_IO;
    if (!is_boot(b)) {
        if (b[0x1FE] != 0x55 || b[0x1FF] != 0xAA || le32(b + 0x1C6) == 0u)
            return FS_ERR_UNSUPPORTED;
        v->base += le32(b + 0x1C6);          /* MBR partition 0 */
        s_sec_vol = NULL;
        if (!(b = sector(v, 0)))
            return FS_ERR_IO;
        if (!is_boot(b))
            return FS_ERR_UNSUPPORTED;
    }
    if (memcmp(b + 3, "EXFAT   ", 8) == 0) {
        if (b[0x6C] != 9u || b[0x6D] > 16u)  /* 512-byte sectors only */
            return FS_ERR_UNSUPPORTED;
        v->fat = le32(b + 0x50);
        v->fatsz = le32(b + 0x54);
        v->data = le32(b + 0x58);
        v->nclus = le32(b + 0x5C);
        v->root = le32(b + 0x60);
        v->serial = le32(b + 0x64);
        v->spc = 1u << b[0x6D];
        v->type = T_EXFAT;
        v->nfats = 1;                        /* (writes: one FAT only; TexFAT's second stays read-only) */
        if (wr && b[0x6E] == 1u)
            v->wr = wr;
    } else {
        uint32_t rsvd = le16(b + 0x0E), nf = b[0x10], fatsz, tot;

        if (b[0x0D] == 0 || nf == 0)
            return FS_ERR_UNSUPPORTED;
        fatsz = le16(b + 0x16) ? le16(b + 0x16) : le32(b + 0x24);
        tot = le16(b + 0x13) ? le16(b + 0x13) : le32(b + 0x20);
        v->spc = b[0x0D];
        v->fat = rsvd;
        v->nfats = nf;
        v->fatsz = fatsz;
        v->root_secs = (le16(b + 0x11) * 32u + 511u) / 512u;
        v->data = rsvd + nf * fatsz + v->root_secs;
        if (tot <= v->data)
            return FS_ERR_UNSUPPORTED;
        v->nclus = (tot - v->data) / v->spc;
        if (v->nclus < 4085u)                /* FAT12 */
            return FS_ERR_UNSUPPORTED;
        v->type = v->nclus < 65525u ? T_FAT16 : T_FAT32;
        v->root = v->type == T_FAT32 ? le32(b + 0x2C) : rsvd + nf * fatsz;
        v->serial = le32(b + (v->type == T_FAT32 ? 0x43 : 0x27));
        if (wr && (v->type == T_FAT16 ? v->nclus <= fatsz * 256u - 2u :
                   !(b[0x28] & 0x80u) && v->nclus <= (fatsz * 128u) - 2u)) {   /* FATs mirrored */
            uint32_t fsi = v->type == T_FAT32 ? le16(b + 0x30) : 0u;

            v->wr = wr;
            v->free = FREE_UNKNOWN;
            v->hint = 2;
            if (fsi && fsi < rsvd && (b = sector(v, fsi)) && le32(b) == 0x41615252u &&
                le32(b + 484) == 0x61417272u && le32(b + 508) == 0xAA550000u) {
                v->fsi = fsi;
                v->free = le32(b + 488) <= v->nclus ? le32(b + 488) : FREE_UNKNOWN;
                if (le32(b + 492) >= 2u && le32(b + 492) < v->nclus + 2u)
                    v->hint = le32(b + 492);
            }
        }
    }
    snprintf(v->name, sizeof v->name, "%s", name);
    return 0;
}

int fs_target_mount(int idx, const char *name, fs_block_read_t rd, uint32_t base)
{
    return fs_target_mount_rw(idx, name, rd, NULL, base);
}

/* Handles still open on it stay allocated, dead (gen), until their owners close them. */
void fs_target_unmount(int idx)
{
    if (idx >= 0 && idx < FS_VOLUMES) {
        (void)fs_target_sync(idx);
        s_vol[idx].gen++;
        s_vol[idx].type = 0;
        drop_caches();
    }
}

/* The cached sectors of the volume on the disk; exFAT: VolumeDirty cleared when no write handle
 * is open on it (fsx clean: only the overlay ever sets it). */
int fs_target_sync(int idx)
{
    vol_t *v;
    const fsx_api_t *x;
    int r;

    if (idx < 0 || idx >= FS_VOLUMES || !(v = &s_vol[idx])->type)
        return FS_ERR_ARG;
    r = sec_flush() | fat_flush() | fsi_flush(v);    /* (whatever is dirty: one volume writes at a time) */
    if (v->xdirty)
        r |= (x = fsx_api()) ? x->clean(v) : FS_ERR_IO;
    return r ? FS_ERR_IO : 0;
}

uint32_t fs_target_serial(int idx)
{
    return idx >= 0 && idx < FS_VOLUMES && s_vol[idx].type ? s_vol[idx].serial : 0u;
}

const char *fs_target_type(int idx)
{
    if (idx < 0 || idx >= FS_VOLUMES)
        return NULL;
    switch (s_vol[idx].type) {
    case T_FAT16: return "FAT16";
    case T_FAT32: return "FAT32";
    case T_EXFAT: return "exFAT";
    default:      return NULL;
    }
}

int fs_target_writable(int idx)
{
    return idx >= 0 && idx < FS_VOLUMES && s_vol[idx].type && writable(&s_vol[idx]);
}

void fs_target_set_clock(uint32_t (*now)(void))
{
    s_clock = now;
}

/* FAT date << 16 | time; 2026-01-01 12:00:00 without a clock. */
uint32_t now_dt(void)
{
    return s_clock ? s_clock() : (uint32_t)((46u << 9 | 1u << 5 | 1u) << 16 | 12u << 11);
}

/* ---- directories ---- */

void cur_init(cur_t *d, const vol_t *v, const ent_t *dir)
{
    uint32_t cl = v->spc * 512u;

    d->v = v;
    d->clus = dir->clus;
    d->sec = d->off = 0;
    d->contig = dir->contig;
    d->left = dir->contig ? (dir->size + cl - 1u) / cl : 0u;
    d->idx = 0;
}

/* Next 32-byte entry; NULL at the end of the directory (d->clus then still
 * names its last cluster) or on error. */
uint8_t *cur_next(cur_t *d)
{
    uint8_t *b;

    if (d->off == 512u) {
        d->off = 0;
        d->sec++;
    }
    if (d->clus == 0u) {                     /* FAT16 fixed root */
        if (d->sec >= d->v->root_secs)
            return NULL;
        d->lba = d->v->root + d->sec;
    } else {
        if (d->sec == d->v->spc) {
            if (d->contig) {
                if (d->left <= 1u)
                    return NULL;
                d->left--;
                d->clus++;
            } else {
                uint32_t nc = fat_next(d->v, d->clus);

                if (!nc)
                    return NULL;
                d->clus = nc;
            }
            d->sec = 0;
        }
        d->lba = clus_lba(d->v, d->clus) + d->sec;
    }
    if (!(b = sector(d->v, d->lba)))
        return NULL;
    b += d->off;
    d->off += 32u;
    d->idx++;
    return b;
}

static void put_utf8(char *out, const uint16_t *u, unsigned n)
{
    char *end = out + FS_NAME_MAX - 4;

    for (unsigned i = 0; i < n && u[i] && out < end; i++) {
        uint32_t c = u[i];

        if (c >= 0xD800u && c < 0xE000u)     /* surrogate: not decoded */
            c = '?';
        if (c < 0x80u) {
            *out++ = (char)c;
        } else if (c < 0x800u) {
            *out++ = (char)(0xC0u | c >> 6);
            *out++ = (char)(0x80u | (c & 0x3Fu));
        } else {
            *out++ = (char)(0xE0u | c >> 12);
            *out++ = (char)(0x80u | (c >> 6 & 0x3Fu));
            *out++ = (char)(0x80u | (c & 0x3Fu));
        }
    }
    *out = '\0';
}

uint8_t sfn_sum(const uint8_t *p)
{
    uint8_t s = 0;

    for (int i = 0; i < 11; i++)
        s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + p[i]);
    return s;
}

static int fat_dir_read(cur_t *d, ent_t *e)
{
    static const uint8_t k_lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    int next_ord = -1;                       /* -1: no long name pending */
    uint8_t sum = 0;
    const uint8_t *p;

    while ((p = cur_next(d)) && p[0] != 0) {
        if (p[0] == 0xE5) {
            next_ord = -1;
            continue;
        }
        if (p[11] == 0x0F) {
            unsigned ord = p[0] & 0x3Fu;

            if (p[0] & 0x40u) {
                memset(s_u16, 0, sizeof s_u16);
                next_ord = (int)ord;
                sum = p[13];
                e->lfn = *d;                 /* re-reads this entry */
                e->lfn.off -= 32u;
                e->nlfn = (uint8_t)ord;
            }
            if (next_ord != (int)ord || p[13] != sum || ord == 0u || ord > 19u) {
                next_ord = -1;
                continue;
            }
            for (int i = 0; i < 13; i++)
                s_u16[(ord - 1u) * 13u + (unsigned)i] = (uint16_t)le16(p + k_lfn_off[i]);
            next_ord--;
            continue;
        }
        if ((p[11] & 0x08) || p[0] == '.') {  /* volume label, "." / ".." */
            next_ord = -1;
            continue;
        }
        if (next_ord == 0 && sfn_sum(p) != sum)
            next_ord = -1;
        if (next_ord == 0) {
            for (unsigned i = 0; i < 256u; i++)
                if (s_u16[i] == 0xFFFFu)
                    s_u16[i] = 0;
            put_utf8(e->name, s_u16, 256);
        } else {                             /* 8.3, NT lower-case flags */
            char *o = e->name;
            for (int i = 0; i < 8 && p[i] != ' '; i++)
                *o++ = (char)((p[12] & 0x08) && p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
            if (p[0] == 0x05)
                e->name[0] = (char)0xE5;
            if (p[8] != ' ')
                *o++ = '.';
            for (int i = 8; i < 11 && p[i] != ' '; i++)
                *o++ = (char)((p[12] & 0x10) && p[i] >= 'A' && p[i] <= 'Z' ? p[i] + 32 : p[i]);
            *o = '\0';
            e->nlfn = 0;
        }
        e->is_dir = (p[11] & 0x10) != 0;
        e->attr = p[11];
        e->wtime = le32(p + 22);             /* write time, date */
        e->contig = 0;
        e->clus = (d->v->type == T_FAT32 ? le16(p + 20) << 16 : 0u) | le16(p + 26);
        e->size = le32(p + 28);
        e->dlba = d->lba;
        e->doff = d->off - 32u;
        e->slot = d->idx - 1u;
        memcpy(e->raw, p, 11);
        if (e->is_dir && e->clus == 0u)      /* ".." to root: not reported */
            continue;
        return 1;
    }
    return 0;
}

static int exfat_dir_read(cur_t *d, ent_t *e)
{
    const uint8_t *p;

    e->raw[0] = 0;                           /* no 8.3 alias */
    while ((p = cur_next(d)) && p[0] != 0) {
        unsigned n2, len, got = 0;
        uint32_t attr;

        if (p[0] != 0x85 || p[1] < 2u)
            continue;
        e->slot = d->idx - 1u;
        e->lfn = *d;                         /* the set starts here (fs_write_ext.c re-reads it) */
        e->lfn.off -= 32u;
        e->dlba = d->lba;
        e->doff = d->off - 32u;
        n2 = p[1];
        e->nlfn = (uint8_t)(n2 + 1u);
        attr = le16(p + 4);
        e->attr = (uint8_t)attr;
        e->wtime = le32(p + 12);             /* LastModifiedTimestamp: FAT's date << 16 | time */
        if (!(p = cur_next(d)) || p[0] != 0xC0)
            continue;
        len = p[3];
        e->contig = (p[1] & 2u) != 0;
        e->clus = le32(p + 20);
        e->size = le32(p + 28) ? 0xFFFFFFFFu : le32(p + 24);   /* > 4 GiB: clamp */
        memset(s_u16, 0, sizeof s_u16);
        for (unsigned k = 1; k < n2; k++) {
            if (!(p = cur_next(d)))
                return 0;
            if (p[0] != 0xC1)
                break;
            for (unsigned i = 0; i < 15u && got < len; i++)
                s_u16[got++] = (uint16_t)le16(p + 2 + 2 * i);
        }
        put_utf8(e->name, s_u16, got);
        e->is_dir = (attr & 0x10u) != 0;
        return 1;
    }
    return 0;
}

int dir_read(cur_t *d, ent_t *e)
{
    return d->v->type == T_EXFAT ? exfat_dir_read(d, e) : fat_dir_read(d, e);
}

/* ---- paths ---- */

static int name_eq(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !y)
            return 0;
    }
    return b[n] == '\0';
}

const char *component(const char *p, size_t *n)
{
    while (*p == '/' || *p == '\\')
        p++;
    *n = 0;
    while (p[*n] && p[*n] != '/' && p[*n] != '\\')
        (*n)++;
    return p;
}

/* Upper-case 8.3 directory name of c[0..n), 0 if it is not a plain 8.3 name. */
static int sfn_make(const char *c, size_t n, uint8_t out[11])
{
    unsigned i = 0, k = 0;

    memset(out, ' ', 11);
    for (size_t j = 0; j < n; j++) {
        char ch = c[j];

        if (ch == '.') {
            if (k >= 8u || i == 0u)          /* second dot, or a leading one */
                return 0;
            k = 8u;
            i = 8u;
            continue;
        }
        if (ch >= 'a' && ch <= 'z')
            ch -= 32;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("!#$%&'()-@^_`{}~", ch)))
            return 0;
        if (i >= (k ? 11u : 8u))
            return 0;
        out[i++] = (uint8_t)ch;
    }
    return out[0] != ' ' && (k == 0u || i > 8u);
}

/* Resolves path into s_ent / *pv. 1 = the virtual root "/", 0 = found,
 * FS_ERR_* otherwise; s_leaf_missing says only the last component was
 * missing (its directory: s_parent). */
int resolve(const char *path, const vol_t **pv)
{
    const vol_t *v = NULL;
    size_t n;
    const char *c;
    cur_t d;

    s_leaf_missing = 0;
    if (!path)
        return FS_ERR_ARG;
    c = component(path, &n);
    if (n == 0u)
        return 1;
    for (int i = 0; i < FS_VOLUMES; i++)
        if (s_vol[i].type && name_eq(c, s_vol[i].name, n))
            v = &s_vol[i];
    if (!v)
        return FS_ERR_OPEN;
    *pv = v;
    memset(&s_ent, 0, sizeof s_ent);        /* the root: no entry of its own (nlfn 0) */
    s_ent.is_dir = 1;
    s_ent.attr = 0x10;
    s_ent.clus = v->type == T_FAT16 ? 0u : v->root;
    for (c = component(c + n, &n); n; c = component(c + n, &n)) {
        int found = 0, alias;
        uint8_t sfn[11];
        size_t rest;

        if (!s_ent.is_dir)
            return FS_ERR_OPEN;
        alias = v->type != T_EXFAT && sfn_make(c, n, sfn);
        s_parent = s_ent.clus;
        s_pent = s_ent;
        cur_init(&d, v, &s_ent);
        while (dir_read(&d, &s_ent) == 1)
            if ((found = name_eq(c, s_ent.name, n) || (alias && !memcmp(sfn, s_ent.raw, 11))))
                break;
        if (!found) {
            component(c + n, &rest);
            s_leaf_missing = rest == 0u;
            return FS_ERR_OPEN;
        }
    }
    return 0;
}

/* Handle already open on the entry s_ent names? */
int busy(const vol_t *v)
{
    for (unsigned i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
        const struct fs_file *f = &s_files[i];

        if (f->used && f->v == v && f->dlba == s_ent.dlba && f->doff == s_ent.doff)
            return 1;
    }
    return 0;
}

static struct fs_file *handle_new(const vol_t *v)
{
    for (unsigned i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
        struct fs_file *f = &s_files[i];

        if (f->used)
            continue;
        memset(f, 0, sizeof *f);
        f->used = 1;
        f->v = v;
        f->gen = v->gen;
        f->dlba = s_ent.dlba;
        f->doff = (uint16_t)s_ent.doff;
        return f;
    }
    return NULL;
}

/* Free entry for a new 8.3 entry in FAT32 directory `dir`: a deleted entry
 * or the end marker -- not one right after a live long-name entry, which
 * would adopt that orphan name. An end marker after one is turned into a
 * deleted entry so the slot behind it becomes visible. A full directory
 * grows by a zeroed cluster. */
static int dir_slot(vol_t *v, uint32_t dir, uint32_t *lba, uint32_t *off)
{
    cur_t d = { v, dir, 0, 0, 0, 0, 0, 0 };
    uint32_t end_lba = 0, end_off = 0, c, e;
    int prev_lfn = 0;
    uint8_t *p;

    while ((p = cur_next(&d))) {
        if (end_lba || ((p[0] == 0 || p[0] == 0xE5) && !prev_lfn)) {
            *lba = d.lba;
            *off = d.off - 32u;
            goto unblock;
        }
        if (p[0] == 0) {
            end_lba = d.lba;
            end_off = d.off - 32u;
        }
        prev_lfn = p[11] == 0x0F && p[0] != 0xE5;
    }
    /* end of the chain (d.clus is its last cluster) -- or a read error; FAT16's root is fixed */
    if (!d.clus)
        return FS_ERR_FULL;
    if ((e = fat_get(v, d.clus)) < 0x0FFFFFF8u || !(c = clus_alloc(v, d.clus + 1u, 0)))
        return FS_ERR_IO;
    if (sec_flush())
        return FS_ERR_IO;
    memset(s_sec, 0, sizeof s_sec);
    s_sec_vol = NULL;
    for (uint32_t s = 0; s < v->spc; s++)
        if (fs_target_nwr++, v->wr(v->base + clus_lba(v, c) + s, 1, s_sec))
            return FS_ERR_IO;
    s_sec_vol = v;                           /* the cache holds a zero sector */
    s_sec_lba = *lba = clus_lba(v, c);
    *off = 0;
    if (fat_flush() || fat_set(v, d.clus, c) || fat_flush() || fsi_flush(v))
        return FS_ERR_IO;
unblock:
    if (end_lba) {
        if (!(p = sector(v, end_lba)))
            return FS_ERR_IO;
        p[end_off] = 0xE5;
        s_sec_dirty = 1;
        return sec_flush();
    }
    return 0;
}

/* ---- fs.h ---- */

int fs_list_dir(const char *path, uint32_t index, fs_dirent_t *out,
                uint32_t max, uint32_t *out_count, uint32_t *out_total)
{
    const vol_t *v;
    uint32_t total = 0, count = 0;
    int r = resolve(path, &v);
    cur_t d;

    if (out_count) *out_count = 0;
    if (out_total) *out_total = 0;
    if (r < 0)
        return r;
    if (r == 1) {
        for (int i = 0; i < FS_VOLUMES; i++) {
            if (!s_vol[i].type)
                continue;
            if (total >= index && count < max && out) {
                snprintf(out[count].name, FS_NAME_MAX, "%s", s_vol[i].name);
                out[count++].is_dir = 1;
            }
            total++;
        }
    } else {
        if (!s_ent.is_dir)
            return FS_ERR_OPEN;
        cur_init(&d, v, &s_ent);
        while (dir_read(&d, &s_ent) == 1) {
            if (total >= index && count < max && out) {
                memcpy(out[count].name, s_ent.name, FS_NAME_MAX);
                out[count++].is_dir = s_ent.is_dir;
            }
            total++;
        }
    }
    if (out_count) *out_count = count;
    if (out_total) *out_total = total;
    return 0;
}

int fs_list_dir_each(const char *path, fs_dirent_cb cb)
{
    static fs_dirent_t one;
    uint32_t n, total, i = 0;

    if (!cb)
        return FS_ERR_ARG;
    do {
        int r = fs_list_dir(path, i, &one, 1, &n, &total);
        if (r)
            return r;
        if (n && cb(one.name, one.is_dir))
            return (int)i + 1;
        i += n;
    } while (n);
    return (int)i;
}

int fs_exists(const char *path)
{
    const vol_t *v;

    return resolve(path, &v) >= 0;
}

static void stat_of(fs_stat_t *st, const ent_t *e)
{
    st->size = e->size;
    st->wtime = e->wtime;
    st->clus = e->clus;
    st->is_dir = (uint8_t)e->is_dir;
    st->attr = e->attr;
}

const fs_stat_t *fs_scan_stat(void)
{
    return &s_stat;
}

/* ---- references: a file or folder found by a scan, opened without its path ---- */

static void ref_of(fs_ref_t *r, const vol_t *v, const ent_t *dir, const ent_t *e)
{
    r->clus = e->clus;
    r->size = e->size;
    r->pclus = dir->clus;
    r->psize = dir->size;
    r->slot = (uint16_t)e->slot;
    r->vol = (uint8_t)(v - s_vol);
    r->flags = (uint8_t)((e->is_dir ? FS_REF_DIR : 0) | (e->contig ? FS_REF_CONTIG : 0) |
                         (dir->contig ? FS_REF_PCONTIG : 0));
}

static int scan(const vol_t *v, ent_t *dir, fs_scan_cb cb, void *u)
{
    ent_t here = *dir;                       /* s_ent is reused by the reader */
    cur_t d;
    fs_ref_t r;
    int n = 0;

    cur_init(&d, v, &here);
    while (dir_read(&d, &s_ent) == 1) {
        ref_of(&r, v, &here, &s_ent);
        stat_of(&s_stat, &s_ent);
        n++;
        if (cb(u, s_ent.name, &r))
            break;
    }
    return n;
}

int fs_scan(const char *path, fs_scan_cb cb, void *u)
{
    const vol_t *v;
    int r = resolve(path, &v);

    if (r != 0)
        return r < 0 ? r : FS_ERR_ARG;       /* "/": the volume list has no refs */
    if (!s_ent.is_dir)
        return FS_ERR_OPEN;
    return scan(v, &s_ent, cb, u);
}

static int ref_vol(const fs_ref_t *r, const vol_t **pv)
{
    if (!r || r->vol >= FS_VOLUMES || !s_vol[r->vol].type)
        return FS_ERR_ARG;
    *pv = &s_vol[r->vol];
    return 0;
}

int fs_scan_ref(const fs_ref_t *dir, fs_scan_cb cb, void *u)
{
    const vol_t *v;
    ent_t e;

    if (ref_vol(dir, &v) || !(dir->flags & FS_REF_DIR))
        return FS_ERR_ARG;
    memset(&e, 0, sizeof e);
    e.is_dir = 1;
    e.clus = dir->clus;
    e.size = dir->size;
    e.contig = (dir->flags & FS_REF_CONTIG) != 0;
    return scan(v, &e, cb, u);
}

fs_file_t *fs_open_ref(const fs_ref_t *r)
{
    const vol_t *v;
    struct fs_file *f;

    if (ref_vol(r, &v) || (r->flags & FS_REF_DIR))
        return NULL;
    memset(&s_ent, 0, sizeof s_ent);         /* no directory entry: read-only use */
    if (!(f = handle_new(v)))
        return NULL;
    f->first = f->clus = r->clus;
    f->size = r->size;
    f->contig = (uint8_t)((r->flags & FS_REF_CONTIG) != 0);
    return f;
}

int fs_ref_name(const fs_ref_t *r, char *out, unsigned long n)
{
    const vol_t *v;
    ent_t dir;
    cur_t d;

    if (ref_vol(r, &v) || !n)
        return FS_ERR_ARG;
    memset(&dir, 0, sizeof dir);
    dir.is_dir = 1;
    dir.clus = r->pclus;
    dir.size = r->psize;
    dir.contig = (r->flags & FS_REF_PCONTIG) != 0;
    cur_init(&d, v, &dir);
    while (dir_read(&d, &s_ent) == 1)
        if (s_ent.slot == r->slot && s_ent.clus == r->clus) {
            snprintf(out, n, "%s", s_ent.name);
            return 0;
        }
    return FS_ERR_OPEN;
}

fs_file_t *fs_open_read(const char *path)
{
    const vol_t *v;
    struct fs_file *f;

    if (resolve(path, &v) != 0 || s_ent.is_dir || !(f = handle_new(v)))
        return NULL;
    f->first = f->clus = s_ent.clus;
    f->size = s_ent.size;
    f->contig = (uint8_t)s_ent.contig;
    return f;
}

long fs_read(fs_file_t *f, void *buf, unsigned long cap)
{
    uint8_t *o = buf;
    unsigned long n = 0;

    if (!f || !f->used || (!buf && cap))
        return FS_ERR_ARG;
    if (f->gen != f->v->gen)
        return FS_ERR_GONE;
    if (f->wr)
        return FS_ERR_UNSUPPORTED;
    while (n < cap && f->pos < f->size) {
        uint32_t cl = f->v->spc * 512u, want = f->pos / cl, off = f->pos % 512u, k;
        const uint8_t *b;

        if (want < f->cidx || f->clus < 2u) {
            if (f->mark_clus && want >= f->mark_cidx) {
                f->clus = f->mark_clus;
                f->cidx = f->mark_cidx;
            } else {
                f->clus = f->first;
                f->cidx = 0;
            }
        }
        while (f->cidx < want) {
            f->clus = f->contig ? f->clus + 1u : fat_next(f->v, f->clus);
            f->cidx++;
            if (!f->clus)
                goto err;
        }
        if (f->clus < 2u)
            goto err;
        /* whole sectors into a word-aligned buffer: one multi-block read per run
         * of contiguous clusters (at most 64 KiB), no copy through the cache */
        if (off == 0u && !((uintptr_t)(o + n) & 3u) && cap - n >= 512u && f->size - f->pos >= 512u) {
            uint32_t want = (uint32_t)((cap - n) / 512u), left = (f->size - f->pos) / 512u;
            uint32_t secs = (cl - f->pos % cl) / 512u, c = f->clus, adv = 0;

            if (want > left)
                want = left;
            if (want > 128u)
                want = 128u;
            while (secs < want) {
                uint32_t nc = f->contig ? c + 1u : fat_next(f->v, c);

                if (nc != c + 1u)
                    break;
                c = nc;
                adv++;
                secs += f->v->spc;
            }
            if (secs > want)
                secs = want;
            if (s_sec_dirty && sec_flush())  /* a writer's sector may be in range */
                goto err;
            fs_target_nrd += secs;
            if (blk(f->v->rd(f->v->base + clus_lba(f->v, f->clus) + f->pos % cl / 512u, secs, o + n)))
                goto err;
            n += secs * 512u;
            f->pos += secs * 512u;
            f->clus = c;
            f->cidx += adv;
            continue;
        }
        if (!(b = sector(f->v, clus_lba(f->v, f->clus) + f->pos % cl / 512u)))
            goto err;
        k = 512u - off;
        if (k > cap - n)
            k = (uint32_t)(cap - n);
        if (k > f->size - f->pos)
            k = f->size - f->pos;
        memcpy(o + n, b + off, k);
        n += k;
        f->pos += k;
    }
    return (long)n;
err:                                         /* what was read stands (f->pos is past it): the error */
    return n ? (long)n : io_err();           /* comes on the next call, which starts there */
}

long fs_file_size(fs_file_t *f)
{
    return f && f->used ? (long)f->size : FS_ERR_ARG;
}

long fs_seek(fs_file_t *f, long offset)
{
    if (f == NULL || !f->used || offset < 0)
        return FS_ERR_ARG;
    if (f->wr)
        return FS_ERR_UNSUPPORTED;
    if (offset > (long)f->size)
        offset = (long)f->size;   /* legal; reads past end report EOF */
    /* the cluster cursor stays: fs_read walks on from it, or restarts (from the
     * mark, else the first cluster) when the new position is behind it */
    f->pos = (uint32_t)offset;
    return offset;
}

/* A reader's place in the cluster chain, kept by a caller that shares one handle between several
 * streams (the library's merge cursors): fs_read walks on from a place at or before the position
 * instead of from the start (a backward seek on a long chain of small clusters reads its FAT). */
void fs_place_get(fs_file_t *f, uint32_t pl[2])
{
    pl[0] = f->clus;
    pl[1] = f->cidx;
}

void fs_place_set(fs_file_t *f, const uint32_t pl[2])
{
    if (pl[0] >= 2u && pl[1] <= f->pos / (f->v->spc * 512u)) {
        f->clus = pl[0];
        f->cidx = pl[1];
    }
}

int fs_mark(fs_file_t *f)
{
    uint8_t b;

    if (f == NULL || !f->used || f->wr)
        return FS_ERR_ARG;
    if (f->pos < f->size && fs_read(f, &b, 1) == 1)   /* walks the cursor to pos */
        f->pos--;
    f->mark_clus = f->clus;
    f->mark_cidx = f->cidx;
    return 0;
}

/* Data, new chain, link, entry, old chain, FSInfo -- see the file header. */
static int close_write(struct fs_file *f)
{
    vol_t *v = (vol_t *)f->v;
    uint32_t dt = now_dt();
    uint8_t *b;
    int r;

    if (v->type == T_EXFAT) {
        const fsx_api_t *x = fsx_api();

        return x ? x->close(f) : FS_ERR_IO;
    }
    r = sec_flush() || fat_flush();

    if (!r && f->link && f->head)
        r = fat_set(v, f->link, f->head) || fat_flush();
    else if (!r && f->cut)                   /* (cut short, nothing new: its last cluster ends it) */
        r = fat_set(v, f->clus, 0x0FFFFFFFu) || fat_flush();
    if (!r) {
        if (!(b = sector(v, f->dlba)) || sfn_sum(b + f->doff) != f->sum) {
            r = 1;
        } else {
            b += f->doff;
            put16(b + 20, f->first >> 16);
            put16(b + 26, f->first);
            put32(b + 28, f->size);
            put16(b + 22, dt);               /* modified */
            put16(b + 24, dt >> 16);
            put16(b + 18, dt >> 16);         /* accessed */
            b[11] |= 0x20u;                  /* archive */
            s_sec_dirty = 1;
            r = sec_flush();
        }
    }
    if (!r && f->old)
        r = chain_free(v, f->old);
    if (r) {
        v->free = FREE_UNKNOWN;
        v->fsi_dirty = 1;
    }
    return fsi_flush(v) || r ? FS_ERR_IO : 0;
}

int fs_close(fs_file_t *f)
{
    int r = 0;

    if (!f || !f->used)
        return f ? FS_ERR_ARG : 0;
    if (f->wr)
        r = f->gen == f->v->gen ? close_write(f) : FS_ERR_GONE;
    f->used = 0;
    return r;
}

/* An 8.3 name exactly as it is stored (no lower case): the resident writer's own. */
static int sfn_upper(const char *c, size_t n, uint8_t out[11])
{
    for (size_t i = 0; i < n; i++)
        if (c[i] >= 'a' && c[i] <= 'z')
            return 0;
    return sfn_make(c, n, out);
}

/* append: 0 rewrite, 1 at the end, 2 at byte `at` (fs_open_append_at). */
static fs_file_t *open_write(const char *path, int append, uint32_t at)
{
    const vol_t *cv;
    vol_t *v;
    struct fs_file *f;
    int r = resolve(path, &cv);
    uint32_t dt = now_dt();

    const fsx_api_t *x;

    if (r == 1 || (r < 0 && !s_leaf_missing) || !writable(cv))
        return NULL;
    v = (vol_t *)cv;
    if (r == 0) {
        if (s_ent.is_dir || busy(v) || !(f = handle_new(v)))
            return NULL;
        f->sum = sfn_sum(s_ent.raw);
        if (append != 2)
            at = s_ent.size;
        if (at > s_ent.size) {
            f->used = 0;
            return NULL;
        }
        if (v->type == T_EXFAT) {            /* (exFAT appends at the end only: fs_open_append_at short
                                              * of it answers NULL, the library then starts over) */
            if ((append == 2 && at != s_ent.size) || !(x = fsx_api()) || x->open(f, append != 0)) {
                f->used = 0;
                return NULL;
            }
        } else if (append && at && s_ent.clus) {
            uint32_t cl = v->spc * 512u, c = s_ent.clus;

            for (uint32_t i = (at - 1u) / cl; i && c; i--)
                c = fat_next(v, c);
            if (!c) {
                f->used = 0;
                return NULL;
            }
            f->first = s_ent.clus;
            f->clus = c;
            f->size = f->pos = at;
            if (at < s_ent.size)             /* the clusters past `at`: cut off and freed at close */
                f->old = fat_next(v, c), f->cut = 1;
        } else {
            f->old = s_ent.clus;             /* freed once the entry is switched */
        }
    } else {
        uint8_t sfn[11], *b;
        const char *leaf = path, *c;
        size_t n, ln = 0;

        for (c = component(path, &n); n; c = component(c + n, &n)) {
            leaf = c;
            ln = n;
        }
        if (v->type == T_EXFAT || !sfn_upper(leaf, ln, sfn)) {   /* exFAT, a long or lower-case name */
            if ((x = fsx_api())) {
                if (x->create(v, leaf, ln, 0x20, 0) || !(f = handle_new(v)))
                    return NULL;
                f->sum = sfn_sum(s_ent.raw);
                x->open(f, 2);               /* (exFAT: NoFatChain while it stays contiguous) */
                f->wr = 1;
                return f;
            }
            if (v->type == T_EXFAT)
                return NULL;
        }
        /* (without the overlay a lower-case 8.3 name is stored upper-case, as before) */
        if (!sfn_make(leaf, ln, sfn) ||
            dir_slot(v, s_parent, &s_ent.dlba, &s_ent.doff) ||
            !(b = sector(v, s_ent.dlba)))
            return NULL;
        b += s_ent.doff;
        memset(b, 0, 32);
        memcpy(b, sfn, 11);
        b[11] = 0x20;                        /* archive */
        put16(b + 14, dt);                   /* created */
        put16(b + 16, dt >> 16);
        put16(b + 18, dt >> 16);
        put16(b + 22, dt);
        put16(b + 24, dt >> 16);
        s_sec_dirty = 1;
        if (sec_flush() || !(f = handle_new(v)))
            return NULL;
        f->sum = sfn_sum(sfn);
    }
    f->wr = 1;
    return f;
}

fs_file_t *fs_open_write(const char *path)
{
    return open_write(path, 0, 0);
}

fs_file_t *fs_open_append(const char *path)
{
    return open_write(path, 1, 0);
}

fs_file_t *fs_open_append_at(const char *path, uint32_t at)
{
    return open_write(path, 2, at);
}

/* Cluster for the byte at f->pos (a cluster boundary): the next free one
 * from the last, so files stay contiguous when they can (when the next one is taken, from the
 * volume's hint: the cluster after the last allocation). New clusters form
 * their own chain; it is hung onto the file's old chain only at close. */
static int grow(struct fs_file *f)
{
    vol_t *v = (vol_t *)f->v;
    uint32_t c;

    if (v->type == T_EXFAT) {
        const fsx_api_t *x = fsx_api();

        return x ? x->grow(f) : FS_ERR_IO;
    }
    c = f->clus ? f->clus + 1u : v->hint;
    if (c != v->hint && fat_get(v, c) != 0u)
        c = v->hint;                         /* taken (another file grew there meanwhile: the library
                                              * writes two at once): the next free one after the last
                                              * allocation, not a scan over everything after this file
                                              * (147 FAT sectors in one slice, m46 scale 30k) */
    c = clus_alloc(v, c, f->head ? f->clus : 0u);

    if (!c)
        return FS_ERR_IO;
    if (!f->head) {
        f->head = c;
        f->link = f->clus;
        if (!f->link)
            f->first = c;
    }
    f->clus = c;
    return 0;
}

long fs_write(fs_file_t *f, const void *buf, unsigned long len)
{
    const uint8_t *s = buf;
    unsigned long n = 0;
    const vol_t *v;
    uint32_t cl;
    int ahead = 0;                           /* f->clus already holds pos */

    if (!f || !f->used || (!buf && len))
        return FS_ERR_ARG;
    if (f->gen != f->v->gen)
        return FS_ERR_GONE;
    if (!f->wr)
        return FS_ERR_UNSUPPORTED;
    v = f->v;
    cl = v->spc * 512u;
    if (len > 0xFFFFFFFFu - f->pos)
        len = 0xFFFFFFFFu - f->pos;          /* FAT32 4 GiB - 1 limit */
    while (n < len) {
        uint32_t off = f->pos % cl, lba, k;
        uint8_t *b;

        if (off == 0u && !ahead && grow(f))
            break;
        ahead = 0;
        lba = clus_lba(v, f->clus) + off / 512u;
        /* whole sectors from a word-aligned buffer: one multi-block write per
         * run of contiguous clusters, straight from the caller's buffer. A
         * cluster taken for the run that is not contiguous starts the next
         * run instead (ahead). */
        if (f->pos % 512u == 0u && !((uintptr_t)(s + n) & 3u) && len - n >= 512u) {
            uint32_t want = (uint32_t)((len - n) / 512u), secs = (cl - off) / 512u;

            if (want > 128u)
                want = 128u;
            while (secs < want) {
                uint32_t prev = f->clus;

                if (grow(f))
                    break;
                if (f->clus != prev + 1u) {
                    ahead = 1;
                    break;
                }
                secs += v->spc;
            }
            if (secs > want)
                secs = want;
            if (s_sec_vol == v && s_sec_lba - lba < secs) {
                s_sec_vol = NULL;            /* cached copy is stale now */
                s_sec_dirty = 0;
            }
            fs_target_nwr += secs;
            if (blk(v->wr(v->base + lba, secs, s + n)))
                break;
            k = secs * 512u;
        } else {
            if (s_sec_vol == v && s_sec_lba == lba) {
                b = (uint8_t *)s_sec;
            } else if (f->pos % 512u == 0u) {    /* past the end: nothing to keep */
                if (sec_flush())
                    break;
                b = (uint8_t *)s_sec;
                memset(b, 0, 512);
                s_sec_vol = v;
                s_sec_lba = lba;
            } else if (!(b = sector(v, lba))) {
                break;
            }
            k = 512u - f->pos % 512u;
            if (k > len - n)
                k = (uint32_t)(len - n);
            memcpy(b + f->pos % 512u, s + n, k);
            s_sec_dirty = 1;
        }
        n += k;
        f->pos += k;
        if (f->pos > f->size)
            f->size = f->pos;
    }
    return n || !len ? (long)n : io_err();
}

/* Remove a file a piece at a time: its first `max` clusters cut off (the entry moved to the next
 * cluster and made smaller first, then the piece freed: a cut in between loses at most that piece),
 * or, when that is all of it, the file removed. 1 = gone, 0 = smaller, < 0 = error. Freeing a big
 * chain at once writes a FAT sector (both copies) per 128 clusters: 32 block writes for 1 MB of
 * 512-byte clusters, in one call (the library's job keeps each pass short with this). */
int fs_remove_part(const char *path, uint32_t max)
{
    const vol_t *cv;
    vol_t *v;
    int r = resolve(path, &cv);
    uint32_t cl, head, c, n = 0;
    uint8_t *b;

    if (r)
        return r == 1 ? FS_ERR_ARG : r;
    if (!writable(cv) || s_ent.is_dir || busy(cv))
        return FS_ERR_ARG;
    v = (vol_t *)cv;
    cl = v->spc * 512u;
    if (v->type == T_EXFAT || !max || !s_ent.clus || s_ent.size <= max * cl)   /* (exFAT: its bitmap frees
                                                                  * 4096 clusters a sector: whole) */
        return fs_remove(path) == 0 ? 1 : FS_ERR_IO;
    for (head = c = s_ent.clus; n < max; n++)
        if (!(c = fat_next(v, c)))
            return FS_ERR_IO;
    if (!(b = sector(v, s_ent.dlba)) || memcmp(b + s_ent.doff, s_ent.raw, 11))
        return FS_ERR_IO;
    b += s_ent.doff;
    put16(b + 20, c >> 16);
    put16(b + 26, c);
    put32(b + 28, s_ent.size - max * cl);
    s_sec_dirty = 1;
    if (sec_flush())
        return FS_ERR_IO;
    for (; n; n--) {                         /* the piece: head .. the cluster before c */
        uint32_t next = fat_get(v, head);

        if (next == 0xFFFFFFFFu || fat_set(v, head, 0))
            return FS_ERR_IO;
        free_add(v, 1);
        head = next;
    }
    return fat_flush() || fsi_flush(v) ? FS_ERR_IO : 0;
}

/* ---- the overlay's own calls (fs_write_ext.c) ---- */

/* The calls with a path only (fs_remove: the screenshot's error path is its only caller; fs_stat:
 * the library's) run in fs_write_ext.c. */
static int path_op(int op, const char *a, const char *b, fs_stat_t *st)
{
    const fsx_api_t *x = fsx_api();

    return x ? x->path(op, a, b, st) : FS_ERR_UNSUPPORTED;
}

int fs_remove(const char *path) { return path_op(FSX_REMOVE, path, NULL, NULL); }
int fs_mkdir(const char *path) { return path_op(FSX_MKDIR, path, NULL, NULL); }
int fs_rename(const char *from, const char *to) { return path_op(FSX_RENAME, from, to, NULL); }
int fs_stat(const char *path, fs_stat_t *st) { return path_op(FSX_STAT, path, NULL, st); }
