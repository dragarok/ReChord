/*
 * fs_write_ext.c -- the cold half of the device's file-system writer (fs.h): exFAT writes, new long
 * names on FAT32/FAT16, mkdir and rename. It runs from the "fsw" overlay in core A's second
 * window (services/ovl.h OVL_FSW, firmware_ap.ld OVL_FS, docs/MEMORY_MAP.md); fs_target.c reaches
 * it through fsx_api() and keeps everything a plain 8.3 write on FAT needs (the log, the
 * settings), so those never wait for this code. Its .data/.bss are resident (the window holds
 * code and constants only).
 *
 * Crash order, as fs_target.c's FAT writer: data, then the allocation (FAT chain / exFAT bitmap),
 * then the directory entries, then freeing what the old content used. A power cut leaves at worst
 * lost clusters (exFAT: bitmap bits set that no entry uses), never an entry pointing at unwritten
 * data or a cross-link. An entry set (exFAT) or a long-name run (FAT) is placed inside one sector
 * whenever it fits in one (16 entries), so it reaches the card in one sector write. exFAT:
 * VolumeDirty is set on the card before the first metadata write and cleared when the last write
 * handle on the volume closes (not if the card came dirty: that is the PC's to repair). A rename
 * removes the old entry before writing the new one (a cut between them: lost clusters, as above);
 * a FAT folder moved to another parent gets its ".." changed in between, so no cut leaves an
 * entry whose ".." names the wrong folder.
 *
 * Sources (no third-party code):
 *  - exFAT: Microsoft "exFAT file system specification" (2019). Boot sector 3.1: VolumeFlags at 106
 *    (bit 1 VolumeDirty, 3.1.13), PercentInUse at 112 (0xFF = unknown, 3.1.16), both outside the
 *    boot checksum (3.4); NumberOfFats 110 (writes only with 1: TexFAT keeps two). FAT 4.1: a chain
 *    with NoFatChain is not in the FAT; 0xFFFFFFFF ends a chain. Allocation bitmap 7.1 (bit n =
 *    cluster n + 2; entry 0x81). Up-case table 7.2 (entry 0x82; compressed: 0xFFFF then a count of
 *    identity mappings). File entry 7.4 (0x85: SecondaryCount, SetChecksum over the set without
 *    bytes 2..3 (6.3.3), FileAttributes, timestamps = FAT's date << 16 | time (7.4.8)). Stream
 *    extension 7.6 (0xC0: GeneralSecondaryFlags bit 0 AllocationPossible, bit 1 NoFatChain;
 *    NameLength; NameHash over the up-cased name, 7.6.4; ValidDataLength; FirstCluster;
 *    DataLength). File name 7.7 (0xC1, 15 UTF-16 units). Directories have no "." / "..".
 *  - FAT: Microsoft "FAT: General Overview of On-Disk Format" 1.03. Long names (13 units an entry,
 *    the last ordinal | 0x40, the 8.3 name's checksum), basis-name generation and the numeric tail
 *    "~N" (the smallest unused), "." and ".." first in a new folder (".." = 0 when the parent is the
 *    root), a folder's 8.3 size 0.
 *  - The stock FS (the SDK's, firmware/rockchip; the research notes in docs/MERGE_NOTES_sdrw.md):
 *    AddFDTLong FDT.c:597 and FatCheckSum FDT.c:391 build the same long-name run; its alias,
 *    FATGetShortFromLongName file.c:137, takes the tail from SysTickCounter % 1000 with no
 *    collision check -- ours scans the folder for the tails in use. FileCreateDirW file.c:312
 *    writes "." and ".." as here. The SDK has no exFAT writer.
 */
#include <stdint.h>
#include <string.h>

#include "fs_target_int.h"

#define SET_MAX 21                   /* FAT: 20 long-name entries + the 8.3 one; exFAT: 2 + 17 */

static uint8_t s_set[SET_MAX * 32];
/* exFAT's allocation bitmap has a sector cache of its own: with the FAT in fs_target.c's, a file
 * growing as a FAT chain would write both sectors back at every cluster (they took turns in one). */
static uint32_t s_bsec[128], s_bsec_lba;
static const vol_t *s_bsec_vol;
static uint16_t s_bsec_mnt;
static int s_bsec_dirty;
static uint32_t s_set_lba[SET_MAX];
static uint16_t s_set_off[SET_MAX];
static uint16_t s_name[256], s_up[256];

static uint32_t clus_bytes(const vol_t *v)
{
    return v->spc * 512u;
}

static int bmp_flush(void)
{
    if (s_bsec_dirty) {
        s_bsec_dirty = 0;
        if (s_bsec_vol->wr(s_bsec_vol->base + s_bsec_lba, 1, s_bsec)) {
            s_bsec_vol = NULL;
            return FS_ERR_IO;
        }
    }
    return 0;
}

/* The bitmap sector holding cluster c's bit (valid for this mount only: v->mnt). */
static uint8_t *bmp_sector(const vol_t *v, uint32_t c)
{
    uint32_t lba = v->bmp + (c - 2u) / 4096u;

    if (v != s_bsec_vol || lba != s_bsec_lba || s_bsec_mnt != v->mnt) {
        if (s_bsec_vol && s_bsec_mnt == s_bsec_vol->mnt && bmp_flush())
            return NULL;                     /* (a sector from an earlier mount is dropped) */
        s_bsec_vol = NULL;
        s_bsec_dirty = 0;
        if (v->rd(v->base + lba, 1, s_bsec))
            return NULL;
        s_bsec_vol = v;
        s_bsec_lba = lba;
        s_bsec_mnt = v->mnt;
    }
    return (uint8_t *)s_bsec;
}

/* Allocation on the card: the FAT sector and the bitmap sector. */
static int alloc_flush(void)
{
    return fat_flush() || bmp_flush() ? FS_ERR_IO : 0;
}

static int bad_char(uint32_t c)
{
    return c < 0x20u || (c < 0x80u && strchr("\"*/:<>?\\|", (int)c));
}

/* UTF-8 [c, c + n) into s_name; its length in units, or -1 (too long, a code point outside the
 * BMP, a character no FAT/exFAT name may hold, "." / ".."). */
static int to_u16(const char *c, size_t n)
{
    unsigned len = 0;

    for (size_t i = 0; i < n; ) {
        uint32_t u = (uint8_t)c[i++];

        if (u >= 0xE0u && u < 0xF0u) {
            if (i + 2 > n)
                return -1;
            u = (u & 0x0Fu) << 12 | ((uint8_t)c[i] & 0x3Fu) << 6 | ((uint8_t)c[i + 1] & 0x3Fu);
            i += 2;
        } else if (u >= 0xC0u && u < 0xE0u) {
            if (i + 1 > n)
                return -1;
            u = (u & 0x1Fu) << 6 | ((uint8_t)c[i++] & 0x3Fu);
        } else if (u >= 0x80u) {
            return -1;
        }
        if (bad_char(u) || len == 255u)
            return -1;
        s_name[len++] = (uint16_t)u;
    }
    if (!len || (len <= 2u && s_name[0] == '.' && s_name[len - 1u] == '.'))
        return -1;
    return (int)len;
}

static void leaf_of(const char *path, const char **leaf, size_t *ln)
{
    size_t n;
    const char *c;

    *leaf = path;
    *ln = 0;
    for (c = component(path, &n); n; c = component(c + n, &n)) {
        *leaf = c;
        *ln = n;
    }
}

/* A write handle open on v other than `self`. */
static int writer_open(const vol_t *v, const struct fs_file *self)
{
    for (unsigned i = 0; i < sizeof s_files / sizeof s_files[0]; i++) {
        const struct fs_file *f = &s_files[i];

        if (f != self && f->used && f->wr && f->v == v && f->gen == v->gen)
            return 1;
    }
    return 0;
}

/* ---- entry runs: read, find, write ---------------------------------------------------------- */

/* n entries from cursor c (a copy) into s_set, with where each lives. 0, or -1. */
static int run_load(cur_t c, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = cur_next(&c);

        if (!p)
            return -1;
        memcpy(s_set + i * 32u, p, 32);
        s_set_lba[i] = c.lba;
        s_set_off[i] = (uint16_t)(c.off - 32u);
    }
    return 0;
}

/* s_set's n entries back where run_load found them: the last first (the primary or the 8.3 entry
 * reaches the card last), or the first first (a removal: the primary goes first). */
static int run_store(const vol_t *v, unsigned n, int first_first)
{
    for (unsigned k = 0; k < n; k++) {
        unsigned i = first_first ? k : n - 1u - k;
        uint8_t *b = sector(v, s_set_lba[i]);

        if (!b)
            return FS_ERR_IO;
        memcpy(b + s_set_off[i], s_set + i * 32u, 32);
        s_sec_dirty = 1;
    }
    return sec_flush();
}

static uint16_t set_sum(unsigned n)
{
    uint16_t c = 0;

    for (unsigned i = 0; i < n * 32u; i++)
        if (i != 2u && i != 3u)
            c = (uint16_t)(((c & 1u) ? 0x8000u : 0u) + (c >> 1) + s_set[i]);
    return c;
}

/* exFAT's FAT entry (spec 4.1: 32 bits, 0xFFFFFFFF ends a chain; FAT_EOC as fs_target.c's). */
static int x_fat_set(const vol_t *v, uint32_t c, uint32_t val)
{
    uint8_t *b = fat_sector(v, v->fat + c / 128u);

    if (!b)
        return FS_ERR_IO;
    put32(b + c % 128u * 4u, val == FAT_EOC ? 0xFFFFFFFFu : val);
    s_fsec_dirty = 1;
    return 0;
}

static int x_free(vol_t *v, uint32_t c, uint32_t size, int contig);
static int x_mark(vol_t *v, uint32_t c, int on);
static uint32_t x_alloc(vol_t *v, uint32_t from);
static int x_store_dir_size(vol_t *v, ent_t *dir, uint32_t add);

/* A zeroed cluster c on the card (a new folder or a folder's new cluster); `first` (32 B entries)
 * at the start of its first sector, if any. */
static int clus_zero(vol_t *v, uint32_t c, const uint8_t *first, unsigned nfirst)
{
    if (sec_flush())
        return FS_ERR_IO;
    memset(s_sec, 0, 512);
    s_sec_vol = NULL;                        /* (the cache holds no sector now) */
    for (uint32_t s = v->spc; s-- > 0; ) {   /* the first sector last */
        if (!s && first)
            memcpy(s_sec, first, nfirst * 32u);
        if (v->wr(v->base + clus_lba(v, c) + s, 1, s_sec))
            return io_err();
    }
    return 0;
}

/* The directory s_pent grows by one zeroed cluster. */
static int dir_grow(vol_t *v, uint32_t last)
{
    uint32_t c;
    int r;

    if (!last)
        return FS_ERR_FULL;                  /* FAT16's root: fixed */
    if (v->type != T_EXFAT) {
        if (fat_get(v, last) < 0x0FFFFFF8u || !(c = clus_alloc(v, last + 1u, 0)))
            return FS_ERR_FULL;
        if (clus_zero(v, c, NULL, 0) || fat_flush() || fat_set(v, last, c) || fat_flush() || fsi_flush(v))
            return FS_ERR_IO;
        return 0;
    }
    if (!(c = x_alloc(v, last + 1u)))
        return FS_ERR_FULL;
    if ((r = clus_zero(v, c, NULL, 0)))
        return r;
    if (s_pent.contig && c != last + 1u) {   /* NoFatChain no more: its chain into the FAT */
        for (uint32_t x = s_pent.clus; x < last; x++)
            if (x_fat_set(v, x, x + 1u))
                return FS_ERR_IO;
        s_pent.contig = 0;
    }
    if (!s_pent.contig && (x_fat_set(v, last, c) || x_fat_set(v, c, FAT_EOC)))
        return FS_ERR_IO;
    if (alloc_flush())
        return FS_ERR_IO;
    if (s_pent.nlfn)                         /* (the root has no entry: its chain is its size) */
        return x_store_dir_size(v, &s_pent, clus_bytes(v));
    return 0;
}

/* n free entries in a row in the directory s_pent, the first one's cursor in *run; the directory
 * grows when it has none. A run that fits in a sector is kept inside one (one sector write puts it
 * on the card); when that moves it past the end marker, the unused entries before it become
 * deleted ones (FAT 0xE5, exFAT 0x05) so readers go on to it (fs_target.c dir_slot does the same).
 * FAT: a lone 8.3 entry never right after a live long-name entry (it would take that orphan name). */
static int find_run(vol_t *v, unsigned n, cur_t *run)
{
    for (int pass = 0; pass < 2; pass++) {
        cur_t d;
        unsigned have = 0;
        int ended = 0, prev_lfn = 0, r;
        uint32_t end_lba = 0, end_off = 0;
        uint8_t *p;

        cur_init(&d, v, &s_pent);
        while ((p = cur_next(&d))) {
            int fre;

            if (!ended && p[0] == 0) {       /* the end marker: everything after it is unused */
                ended = 1;
                end_lba = d.lba;
                end_off = d.off - 32u;
            }
            fre = ended || (v->type == T_EXFAT ? p[0] < 0x80u : p[0] == 0xE5u);
            if (fre && !have && ((n <= 16u && (d.off - 32u) / 32u + n > 16u) || (n == 1u && prev_lfn)))
                fre = 0;                     /* (not here: from the next sector / entry on) */
            prev_lfn = v->type != T_EXFAT && !ended && p[11] == 0x0F && p[0] != 0xE5u;
            if (!fre) {
                have = 0;
                continue;
            }
            if (!have) {
                *run = d;
                run->off -= 32u;
            }
            if (++have < n)
                continue;
            if (ended && (run->lba != end_lba || run->off != end_off)) {
                uint8_t *b = sector(v, end_lba);

                if (!b)
                    return FS_ERR_IO;
                for (uint32_t o = end_off; o < 512u; o += 32u)
                    b[o] = v->type == T_EXFAT ? 0x05u : 0xE5u;
                s_sec_dirty = 1;
                if (sec_flush())
                    return FS_ERR_IO;
            }
            return 0;
        }
        if (pass || (r = dir_grow(v, d.clus)))
            return pass ? FS_ERR_FULL : r;
    }
    return FS_ERR_FULL;
}

/* ---- exFAT ----------------------------------------------------------------------------------- */

/* The allocation bitmap and the up-case table, from the root directory (once per mount). */
static int x_locate(vol_t *v)
{
    ent_t root;
    cur_t d;
    uint8_t *p;
    uint32_t bmp = 0, blen = 0;

    if (v->bmp || v->xbad)
        return v->xbad ? FS_ERR_UNSUPPORTED : 0;
    memset(&root, 0, sizeof root);
    root.clus = v->root;
    cur_init(&d, v, &root);
    while ((p = cur_next(&d)) && p[0]) {
        if (p[0] == 0x81 && !(p[1] & 1u)) {  /* the (first) allocation bitmap */
            bmp = le32(p + 20);
            blen = le32(p + 24);
        } else if (p[0] == 0x82) {
            v->upc = le32(p + 20);
            v->upc_len = le32(p + 24);
        }
    }
    /* the bitmap's clusters must follow one another (we address it as one run of sectors) */
    if (bmp >= 2u && blen >= (v->nclus + 7u) / 8u && v->upc >= 2u) {
        uint32_t c = bmp, k = (blen + clus_bytes(v) - 1u) / clus_bytes(v);

        while (--k && fat_next(v, c) == c + 1u)
            c++;
        if (!k) {
            v->bmp = clus_lba(v, bmp);
            v->hint = 2;
            return 0;
        }
    }
    v->xbad = 1;
    return FS_ERR_UNSUPPORTED;
}

static int x_bit(vol_t *v, uint32_t c)
{
    const uint8_t *b = bmp_sector(v, c);

    return b ? b[(c - 2u) % 4096u / 8u] >> ((c - 2u) % 8u) & 1 : -1;
}

static int x_mark(vol_t *v, uint32_t c, int on)
{
    uint8_t *b = bmp_sector(v, c);
    uint8_t m = (uint8_t)(1u << ((c - 2u) % 8u));

    if (!b)
        return FS_ERR_IO;
    b += (c - 2u) % 4096u / 8u;
    *b = (uint8_t)(on ? *b | m : *b & ~m);
    s_bsec_dirty = 1;
    return 0;
}

/* The first free cluster from `from` on (wrapping), marked used; 0 when full or on I/O error. */
static uint32_t x_alloc(vol_t *v, uint32_t from)
{
    uint32_t c = from;

    for (uint32_t i = 0; i < v->nclus; i++, c++) {
        int b;

        if (c < 2u || c >= v->nclus + 2u)
            c = 2u;
        if ((b = x_bit(v, c)) < 0)
            return 0;
        if (!b) {
            if (x_mark(v, c, 1))
                return 0;
            v->hint = c + 1u;
            return c;
        }
    }
    return 0;
}

/* Clusters of a chain back to free: contiguous ones by count, others along the FAT. */
static int x_free(vol_t *v, uint32_t c, uint32_t size, int contig)
{
    uint32_t n = (size + clus_bytes(v) - 1u) / clus_bytes(v);

    if (size == 0xFFFFFFFFu)
        return FS_ERR_IO;                    /* (over 4 GiB: never ours to free) */
    for (uint32_t i = 0; c >= 2u && c < v->nclus + 2u && i < n; i++) {
        uint32_t next = contig ? c + 1u : fat_next(v, c);

        if (x_mark(v, c, 0))
            return FS_ERR_IO;
        c = next;
    }
    return alloc_flush();
}

/* VolumeDirty on, PercentInUse unknown, before the first metadata write (spec 3.1.13, 3.1.16). */
static int x_dirty(vol_t *v)
{
    uint8_t *b;

    if (v->xdirty)
        return 0;
    if (x_locate(v) || !(b = sector(v, 0)))
        return FS_ERR_IO;
    if (b[106] & 2u) {
        v->xdirty = 2;                       /* dirty already: the PC repairs it, we leave it set */
        return 0;
    }
    b[106] |= 2u;
    b[112] = 0xFF;
    s_sec_dirty = 1;
    v->xdirty = 1;
    return sec_flush();
}

/* VolumeDirty off again once nothing writes (self: the handle being closed, not counted). */
static int x_clean(vol_t *v, const struct fs_file *self)
{
    uint8_t *b;

    if ((s_bsec_vol == v && bmp_flush()) || fat_flush() || sec_flush())
        return FS_ERR_IO;
    if (v->xdirty != 1 || writer_open(v, self))
        return 0;
    if (!(b = sector(v, 0)))
        return FS_ERR_IO;
    b[106] &= (uint8_t)~2u;
    s_sec_dirty = 1;
    v->xdirty = 0;
    return sec_flush();
}

int fsx_clean(vol_t *v)
{
    return x_clean(v, NULL);
}

/* s_name[0..len) up-cased into s_up through the volume's up-case table (7.2): one pass over the
 * table up to the largest code in the name (an ASCII name reads its first sector only). */
static void x_upcase(vol_t *v, unsigned len)
{
    uint32_t top = 0, idx = 0, pos = 0, c = v->upc, cl = clus_bytes(v);
    int run = 0;

    for (unsigned i = 0; i < len; i++) {
        s_up[i] = s_name[i];
        if (s_name[i] > top)
            top = s_name[i];
    }
    while (pos + 1u < v->upc_len && idx <= top && c >= 2u) {
        const uint8_t *b = sector(v, clus_lba(v, c) + pos % cl / 512u);
        uint32_t w;

        if (!b)
            return;
        w = le16(b + pos % 512u);
        pos += 2u;
        if (pos % cl == 0u)
            c = fat_next(v, c);
        if (run) {
            idx += w;
            run = 0;
        } else if (w == 0xFFFFu) {
            run = 1;
        } else {
            for (unsigned i = 0; i < len; i++)
                if (s_name[i] == idx)
                    s_up[i] = (uint16_t)w;
            idx++;
        }
    }
}

static uint16_t x_hash(unsigned len)
{
    uint16_t h = 0;

    for (unsigned i = 0; i < len; i++) {
        h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (h >> 1) + (s_up[i] & 0xFFu));
        h = (uint16_t)(((h & 1u) ? 0x8000u : 0u) + (h >> 1) + (s_up[i] >> 8));
    }
    return h;
}

/* A new entry set in s_pent for s_name[0..len): attributes, the stream's flags / first cluster /
 * size, and the file entry's bytes 8..24 (timestamps) from `times` or now. s_ent then names it. */
static int x_create(vol_t *v, unsigned len, uint8_t attr, uint8_t flags, uint32_t clus, uint32_t size,
                    const uint8_t *times)
{
    unsigned n = 2u + (len + 14u) / 15u;
    uint32_t dt = now_dt();
    cur_t run;
    int r;

    if ((r = x_dirty(v)) || (r = find_run(v, n, &run)))
        return r;
    x_upcase(v, len);
    if (run_load(run, n))
        return FS_ERR_IO;
    memset(s_set, 0, n * 32u);
    s_set[0] = 0x85;
    s_set[1] = (uint8_t)(n - 1u);
    put16(s_set + 4, attr);
    if (times) {
        memcpy(s_set + 8, times, 17);
    } else {
        put32(s_set + 8, dt);                /* created, modified, accessed; 10 ms 0, UTC offsets unused */
        put32(s_set + 12, dt);
        put32(s_set + 16, dt);
    }
    s_set[32] = 0xC0;
    s_set[33] = flags;
    s_set[35] = (uint8_t)len;
    put16(s_set + 36, x_hash(len));
    put32(s_set + 40, size);                 /* ValidDataLength (64-bit: the high word 0) */
    put32(s_set + 52, clus);
    put32(s_set + 56, size);                 /* DataLength */
    for (unsigned i = 0; i < len; i++) {
        uint8_t *e = s_set + (2u + i / 15u) * 32u;

        e[0] = 0xC1;
        put16(e + 2 + i % 15u * 2u, s_name[i]);
    }
    for (unsigned k = 2; k < n; k++)
        s_set[k * 32u] = 0xC1;
    put16(s_set + 2, set_sum(n));
    if ((r = run_store(v, n, 0)))
        return r;
    s_ent.lfn = run;
    s_ent.nlfn = (uint8_t)n;
    s_ent.dlba = s_set_lba[0];
    s_ent.doff = s_set_off[0];
    s_ent.clus = clus;
    s_ent.size = size;
    s_ent.contig = (flags & 2u) != 0;
    s_ent.attr = attr;
    s_ent.is_dir = (attr & 0x10u) != 0;
    s_ent.raw[0] = 0;
    return 0;
}

/* The set at e (s_ent / s_pent as dir_read left it) into s_set, checked: 0x85, then 0xC0, the
 * count as recorded. */
static int x_load(const ent_t *e)
{
    if (e->nlfn < 3u || e->nlfn > SET_MAX || run_load(e->lfn, e->nlfn) || s_set[0] != 0x85 ||
        s_set[1] + 1u != e->nlfn || s_set[32] != 0xC0)
        return FS_ERR_IO;
    return 0;
}

/* A folder's set: its size (and flags) after it grew by `add` bytes. */
static int x_store_dir_size(vol_t *v, ent_t *dir, uint32_t add)
{
    unsigned n = dir->nlfn;
    int r;

    if ((r = x_load(dir)))
        return r;
    dir->size += add;
    s_set[33] = (uint8_t)(1u | (dir->contig ? 2u : 0u));
    put32(s_set + 40, dir->size);
    put32(s_set + 56, dir->size);
    put16(s_set + 2, set_sum(n));
    return run_store(v, n, 0);
}

/* ---- FAT: long names and their 8.3 alias ------------------------------------------------------ */

static int sfn_char(uint32_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c < 0x80u && c > 0x20u && strchr("!#$%&'()-@^_`{}~", (int)c));
}

/* The 8.3 alias of s_name[0..len) in raw[11] (MS FAT spec, basis-name generation): upper case,
 * spaces and leading dots dropped, other characters an 8.3 name cannot hold as '_'; the base to 8
 * and the extension (after the last dot) to 3. Returns 1 if it needs a numeric tail (lossy or cut). */
static int basis(unsigned len, uint8_t raw[11])
{
    int lossy = 0, dot = -1;
    unsigned k = 0, i = 0;

    memset(raw, ' ', 11);
    for (unsigned j = 0; j < len; j++)
        if (s_name[j] == '.')
            dot = (int)j;
    while (i < len && s_name[i] == '.')
        i++;
    lossy = i > 0u;
    for (; i < len && (dot < 0 || i < (unsigned)dot); i++) {
        uint32_t c = s_name[i];

        if (c == ' ' || c == '.') {
            lossy = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z')
            c -= 32u;
        if (!sfn_char(c)) {
            c = '_';
            lossy = 1;
        }
        if (k == 8u) {
            lossy = 1;
            break;
        }
        raw[k++] = (uint8_t)c;
    }
    if (dot >= 0) {
        k = 8;
        for (i = (unsigned)dot + 1u; i < len; i++) {
            uint32_t c = s_name[i];

            if (c == ' ')
                continue;
            if (c >= 'a' && c <= 'z')
                c -= 32u;
            if (!sfn_char(c)) {
                c = '_';
                lossy = 1;
            }
            if (k == 11u) {
                lossy = 1;
                break;
            }
            raw[k++] = (uint8_t)c;
        }
    }
    if (raw[0] == ' ') {                     /* nothing left of the base */
        raw[0] = '_';
        lossy = 1;
    }
    return lossy;
}

/* raw with the numeric tail ~n (1..999999), the base cut to make room. */
static void put_tail(uint8_t raw[11], const uint8_t base[8], uint32_t n)
{
    char t[8];
    unsigned tl = 0, bl = 0;

    for (uint32_t x = n; x; x /= 10u)
        tl++;
    for (unsigned j = tl; j; j--, n /= 10u)
        t[j] = (char)('0' + n % 10u);
    t[0] = '~';
    while (bl < 8u && base[bl] != ' ')
        bl++;
    if (bl > 7u - tl)
        bl = 7u - tl;
    memcpy(raw, base, bl);
    memcpy(raw + bl, t, tl + 1u);
    memset(raw + bl + tl + 1u, ' ', 8u - bl - tl - 1u);
}

/* Which tails 1..32 of `base` with this extension are taken in s_pent (bit n-1), and whether the
 * exact raw name is; one pass over the folder. */
static uint32_t tails_used(vol_t *v, const uint8_t raw[11], const uint8_t base[8], int *exact)
{
    cur_t d;
    uint8_t *p;
    uint32_t used = 0;

    *exact = 0;
    cur_init(&d, v, &s_pent);
    while ((p = cur_next(&d)) && p[0]) {
        unsigned i = 0;
        uint32_t n = 0;

        if (p[0] == 0xE5u || p[11] == 0x0F || memcmp(p + 8, raw + 8, 3))
            continue;
        if (!memcmp(p, raw, 8))
            *exact = 1;
        while (i < 8u && p[i] != '~' && p[i] == base[i])
            i++;
        if (i == 8u || p[i] != '~')
            continue;
        for (i++; i < 8u && p[i] >= '0' && p[i] <= '9'; i++)
            n = n * 10u + (p[i] - '0');
        if ((i == 8u || p[i] == ' ') && n >= 1u && n <= 32u)
            used |= 1u << (n - 1u);
    }
    return used;
}

/* A long-name run + 8.3 alias (or a plain 8.3 entry for a name that is one) in s_pent for
 * s_name[0..len); attr, cluster, size, and the 8.3 entry's bytes 13..25 (times) from `times` or
 * now. s_ent then names it. */
static int f_create(vol_t *v, unsigned len, uint8_t attr, uint32_t clus, uint32_t size, const uint8_t *times)
{
    uint8_t raw[11], base[8];
    unsigned nl = 0, n;
    uint32_t dt = now_dt();
    int exact, tail = basis(len, raw);
    cur_t run;
    int r;

    memcpy(base, raw, 8);
    /* no long name for a name its alias spells exactly (upper case, fits): fs_target.c's 8.3 case */
    {
        uint16_t spelt[13];
        unsigned k = 0;

        for (unsigned i = 0; i < 8u && raw[i] != ' '; i++)
            spelt[k++] = raw[i];
        if (raw[8] != ' ')
            spelt[k++] = '.';
        for (unsigned i = 8; i < 11u && raw[i] != ' '; i++)
            spelt[k++] = raw[i];
        if (tail || k != len || memcmp(spelt, s_name, len * 2u))
            nl = (len + 12u) / 13u;
    }
    {
        uint32_t used = tails_used(v, raw, base, &exact);

        if (tail || exact) {
            uint32_t k = 1;

            while (k <= 32u && (used >> (k - 1u) & 1u))
                k++;
            if (k > 32u) {                   /* 32 taken: the tail from the name's hash (as Windows) */
                uint16_t h = 0;
                static const char hex[] = "0123456789ABCDEF";

                for (unsigned i = 0; i < len; i++)
                    h = (uint16_t)(h * 37u + s_name[i]);
                base[2] = (uint8_t)hex[h >> 12];
                base[3] = (uint8_t)hex[h >> 8 & 15u];
                base[4] = (uint8_t)hex[h >> 4 & 15u];
                base[5] = (uint8_t)hex[h & 15u];
                if (base[0] == ' ' || base[1] == ' ')
                    base[0] = base[1] = '_';
                used = tails_used(v, raw, base, &exact);
                for (k = 1; k <= 32u && (used >> (k - 1u) & 1u); k++)
                    ;
                if (k > 32u)
                    return FS_ERR_FULL;
            }
            put_tail(raw, base, k);
            if (!nl)
                nl = (len + 12u) / 13u;
        }
    }
    n = nl + 1u;
    if ((r = find_run(v, n, &run)) || run_load(run, n))
        return r ? r : FS_ERR_IO;
    memset(s_set, 0, n * 32u);
    for (unsigned k = 0; k < nl; k++) {     /* ordinals nl .. 1, the first written is the last part */
        static const uint8_t off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        uint8_t *e = s_set + k * 32u;
        unsigned ord = nl - k;

        e[0] = (uint8_t)(ord | (k == 0u ? 0x40u : 0u));
        e[11] = 0x0F;
        e[13] = sfn_sum(raw);
        for (unsigned i = 0; i < 13u; i++) {
            unsigned u = (ord - 1u) * 13u + i;

            put16(e + off[i], u < len ? s_name[u] : u == len ? 0u : 0xFFFFu);
        }
    }
    {
        uint8_t *e = s_set + nl * 32u;

        memcpy(e, raw, 11);
        e[11] = attr;
        if (times) {
            memcpy(e + 13, times, 13);
        } else {
            put16(e + 14, dt);               /* created */
            put16(e + 16, dt >> 16);
            put16(e + 18, dt >> 16);         /* accessed */
            put16(e + 22, dt);               /* written */
            put16(e + 24, dt >> 16);
        }
        put16(e + 20, v->type == T_FAT32 ? clus >> 16 : 0u);
        put16(e + 26, clus);
        put32(e + 28, size);
    }
    if ((r = run_store(v, n, 0)))
        return r;
    s_ent.lfn = run;
    s_ent.nlfn = (uint8_t)nl;
    s_ent.dlba = s_set_lba[nl];
    s_ent.doff = s_set_off[nl];
    memcpy(s_ent.raw, raw, 11);
    s_ent.clus = clus;
    s_ent.size = size;
    s_ent.attr = attr;
    s_ent.is_dir = (attr & 0x10u) != 0;
    s_ent.contig = 0;
    return 0;
}

/* ---- the API (fs_target_int.h fsx_api_t) ----------------------------------------------------- */

/* A new file entry named leaf[0..n) in s_pent (the directory resolve() left): fs_open_write. */
int fsx_create(vol_t *v, const char *leaf, size_t n, uint8_t attr, uint32_t clus)
{
    int len = to_u16(leaf, n);

    if (len < 0)
        return FS_ERR_ARG;
    if (v->type == T_EXFAT)
        return x_create(v, (unsigned)len, attr, 1u, clus, 0, NULL);
    return f_create(v, (unsigned)len, attr, clus, 0, NULL);
}

/* An existing exFAT file opened for writing (s_ent): append from its end, or a rewrite whose old
 * clusters are freed at close. */
int fsx_open(struct fs_file *f, int append)
{
    vol_t *v = (vol_t *)f->v;
    uint32_t cl = clus_bytes(v);

    if (x_locate(v) || s_ent.size == 0xFFFFFFFFu || s_ent.nlfn < 3u)
        return FS_ERR_UNSUPPORTED;
    if (append && s_ent.size && s_ent.clus) {
        uint32_t c = s_ent.clus;

        for (uint32_t i = (s_ent.size - 1u) / cl; i && c; i--)
            c = s_ent.contig ? c + 1u : fat_next(v, c);
        if (!c)
            return FS_ERR_IO;
        f->first = s_ent.clus;
        f->clus = c;
        f->contig = (uint8_t)s_ent.contig;
        f->size = f->pos = s_ent.size;
    } else {
        f->old = s_ent.clus;
        f->old_size = s_ent.size;
        f->old_contig = (uint8_t)s_ent.contig;
        f->contig = 1;
    }
    return 0;
}

/* The cluster for the byte at f->pos (a cluster boundary): the next one while it is free (the
 * file stays NoFatChain), else any free one, the chain then put into the FAT. */
int fsx_grow(struct fs_file *f)
{
    vol_t *v = (vol_t *)f->v;
    uint32_t want = f->clus ? f->clus + 1u : v->hint, c;
    int b;

    if (x_dirty(v))
        return FS_ERR_IO;
    if (f->clus && f->contig && want < v->nclus + 2u && (b = x_bit(v, want)) == 0) {
        if (x_mark(v, want, 1))
            return FS_ERR_IO;
        c = want;
    } else {
        if (!(c = x_alloc(v, want)))
            return FS_ERR_FULL;
        if (f->clus && f->contig) {          /* contiguous no more: its clusters into the FAT */
            for (uint32_t x = f->first; x < f->clus; x++)
                if (x_fat_set(v, x, x + 1u))
                    return FS_ERR_IO;
            f->contig = 0;
        }
        if (f->clus && !f->contig && x_fat_set(v, f->clus, c))
            return FS_ERR_IO;
        if (!f->contig && x_fat_set(v, c, FAT_EOC))
            return FS_ERR_IO;
    }
    if (!f->head) {
        f->head = c;
        if (!f->clus)
            f->first = c;
    }
    f->clus = c;
    return 0;
}

/* Data and allocation on the card, then the entry set (size, first cluster, NoFatChain, time),
 * then the old content's clusters freed. */
int fsx_close(struct fs_file *f)
{
    vol_t *v = (vol_t *)f->v;
    uint32_t dt = now_dt();
    ent_t e;
    int r = sec_flush() || alloc_flush();

    memset(&e, 0, sizeof e);
    e.lfn = f->ecur;
    e.nlfn = f->nset;
    if (!r && (x_load(&e) || s_set_lba[0] != f->dlba || s_set_off[0] != f->doff))
        r = 1;                               /* (the set is not where it was opened) */
    if (!r) {
        uint32_t first = f->size ? f->first : 0u;

        s_set[33] = (uint8_t)(1u | (first && f->contig ? 2u : 0u));
        put32(s_set + 40, f->size);
        put32(s_set + 44, 0);
        put32(s_set + 52, first);
        put32(s_set + 56, f->size);
        put32(s_set + 60, 0);
        put16(s_set + 4, le16(s_set + 4) | 0x20u);   /* archive */
        put32(s_set + 12, dt);               /* modified, accessed */
        put32(s_set + 16, dt);
        s_set[21] = 0;
        put16(s_set + 2, set_sum(f->nset));
        r = run_store(v, f->nset, 0) != 0;
    }
    if (!r && f->old)
        r = x_free(v, f->old, f->old_size, f->old_contig) != 0;
    if (!r && f->size == 0 && f->head)       /* (clusters taken, nothing written in them) */
        r = x_free(v, f->head, clus_bytes(v), 1) != 0;
    if (!r)
        r = x_clean(v, f) != 0;
    return r ? FS_ERR_IO : 0;
}

/* A file (not a folder, not one that is open) removed. FAT (fs_target.c's writer before it moved
 * here): its long-name entries first (they may run over sectors and clusters; the cache writes each
 * sector back as the cursor leaves it), then the 8.3 entry -- a cut in between leaves the file
 * under its 8.3 name -- then its chain, then FSInfo. exFAT: its set not in use any more (the
 * primary first), then its clusters free. */
int fsx_remove(const char *path)
{
    const vol_t *cv;
    vol_t *v;
    uint8_t *p;
    unsigned n;
    int r = resolve(path, &cv);

    if (r)
        return r == 1 ? FS_ERR_ARG : r;
    if (!writable(cv))
        return FS_ERR_UNSUPPORTED;
    if (s_ent.is_dir || busy(cv))
        return FS_ERR_ARG;
    v = (vol_t *)cv;
    n = s_ent.nlfn;
    if (v->type != T_EXFAT) {
        for (unsigned i = 0; i < n; i++) {
            if (!(p = cur_next(&s_ent.lfn)) || p[11] != 0x0F)
                return FS_ERR_IO;
            p[0] = 0xE5;
            s_sec_dirty = 1;
        }
        if (!(p = sector(v, s_ent.dlba)) || memcmp(p + s_ent.doff, s_ent.raw, 11))
            return FS_ERR_IO;
        p[s_ent.doff] = 0xE5;
        s_sec_dirty = 1;
        if (sec_flush() || chain_free(v, s_ent.clus) || fsi_flush(v))
            return FS_ERR_IO;
        return 0;
    }
    if ((r = x_dirty(v)) || (r = x_load(&s_ent)))
        return r;
    for (unsigned i = 0; i < n; i++)
        s_set[i * 32u] &= 0x7Fu;
    if ((r = run_store(v, n, 1)))
        return r;
    if (s_ent.clus && (r = x_free(v, s_ent.clus, s_ent.size, s_ent.contig)))
        return r;
    return x_clean(v, NULL);
}

int fsx_mkdir(const char *path)
{
    const vol_t *cv;
    vol_t *v;
    const char *leaf;
    size_t ln;
    uint32_t c, dt = now_dt();
    int len, r = resolve(path, &cv);

    if (r >= 0)
        return FS_ERR_EXIST;
    if (!s_leaf_missing)
        return r;
    v = (vol_t *)cv;
    if (!writable(v))
        return FS_ERR_UNSUPPORTED;
    leaf_of(path, &leaf, &ln);
    if ((len = to_u16(leaf, ln)) < 0)
        return FS_ERR_ARG;
    if (v->type == T_EXFAT) {
        if ((r = x_dirty(v)))
            return r;
        if (!(c = x_alloc(v, v->hint)))
            return FS_ERR_FULL;
        if ((r = clus_zero(v, c, NULL, 0)) || (r = alloc_flush()) ||
            (r = x_create(v, (unsigned)len, 0x10, 3u, c, clus_bytes(v), NULL)))
            return r;
        return x_clean(v, NULL);
    }
    {
        uint8_t dots[64];

        if (!(c = clus_alloc(v, v->hint, 0)))
            return FS_ERR_FULL;
        memset(dots, 0, sizeof dots);
        memset(dots, ' ', 11);
        dots[0] = '.';
        memset(dots + 32, ' ', 11);
        dots[32] = dots[33] = '.';
        for (unsigned k = 0; k < 2u; k++) {
            uint8_t *e = dots + k * 32u;
            uint32_t to = k ? (s_parent == v->root && v->type == T_FAT32 ? 0u : s_parent) : c;

            e[11] = 0x10;
            put16(e + 14, dt);
            put16(e + 16, dt >> 16);
            put16(e + 18, dt >> 16);
            put16(e + 22, dt);
            put16(e + 24, dt >> 16);
            put16(e + 20, v->type == T_FAT32 ? to >> 16 : 0u);
            put16(e + 26, to);
        }
        if ((r = clus_zero(v, c, dots, 2)) || (r = fat_flush()) || (r = fsi_flush(v)))
            return r;
        return f_create(v, (unsigned)len, 0x10, c, 0, NULL);
    }
}

/* Case-insensitive (ASCII) "b is a or is inside a": a folder may not move into itself. */
static int inside(const char *a, const char *b)
{
    size_t i = 0;

    for (; a[i]; i++) {
        char x = a[i], y = b[i];

        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x != y)
            return 0;
    }
    return b[i] == '\0' || b[i] == '/' || b[i] == '\\';
}

int fsx_rename(const char *from, const char *to)
{
    static ent_t src;
    const vol_t *cv, *cv2;
    vol_t *v;
    const char *leaf;
    size_t ln;
    uint8_t keep[24];                        /* exFAT: the file entry's 8..24; FAT: the 8.3's 13..25 */
    uint8_t xflags = 0;
    uint32_t src_parent;
    cur_t probe;
    int len, r = resolve(from, &cv), same = 0;

    if (r)
        return r == 1 ? FS_ERR_ARG : r;
    v = (vol_t *)cv;
    if (!writable(v))
        return FS_ERR_UNSUPPORTED;
    if (fst_busy(v))
        return FS_ERR_ARG;                   /* open */
    src = s_ent;
    src_parent = s_parent;
    if (src.is_dir && inside(from, to) && strlen(to) > strlen(from))
        return FS_ERR_ARG;                   /* a folder into itself */
    r = resolve(to, &cv2);
    if (cv2 != cv)
        return FS_ERR_ARG;
    if (r == 0) {
        if (s_ent.dlba != src.dlba || s_ent.doff != src.doff)
            return FS_ERR_EXIST;
        same = 1;                            /* the same entry: a change of case */
    } else if (r < 0 && !s_leaf_missing) {
        return r;
    } else if (r == 1) {
        return FS_ERR_ARG;
    }
    (void)same;                              /* (s_pent: the folder the name goes in, either way) */
    leaf_of(to, &leaf, &ln);
    if ((len = to_u16(leaf, ln)) < 0)
        return FS_ERR_ARG;
    if (v->type == T_EXFAT) {
        if ((r = x_dirty(v)) || (r = x_load(&src)))
            return r;
        memcpy(keep, s_set + 8, 17);
        xflags = s_set[33];
        if ((r = find_run(v, 2u + ((unsigned)len + 14u) / 15u, &probe)))   /* room first */
            return r;
        if ((r = x_load(&src)))
            return r;
        for (unsigned i = 0; i < src.nlfn; i++)
            s_set[i * 32u] &= 0x7Fu;
        if ((r = run_store(v, src.nlfn, 1)))
            return r;
        if ((r = x_create(v, (unsigned)len, src.attr, xflags, src.clus, src.size, keep)))
            return r;
        return x_clean(v, NULL);
    }
    {
        unsigned nl = src.nlfn;
        uint8_t *p;

        if (!(p = sector(v, src.dlba)) || memcmp(p + src.doff, src.raw, 11))
            return FS_ERR_IO;
        memcpy(keep, p + src.doff + 13, 13);
        if ((r = find_run(v, ((unsigned)len + 12u) / 13u + 1u, &probe)))  /* room first */
            return r;
        /* the old name gone: long-name entries, then the 8.3 entry (fs_target.c fs_remove's order) */
        for (unsigned i = 0; i < nl; i++) {
            if (!(p = cur_next(&src.lfn)) || p[11] != 0x0F)
                return FS_ERR_IO;
            p[0] = 0xE5;
            s_sec_dirty = 1;
        }
        if (!(p = sector(v, src.dlba)))
            return FS_ERR_IO;
        p[src.doff] = 0xE5;
        s_sec_dirty = 1;
        if (sec_flush())
            return FS_ERR_IO;
        if (src.is_dir && s_parent != src_parent) {   /* moved: its ".." names the new parent first */
            uint32_t to_c = s_parent == v->root && v->type == T_FAT32 ? 0u : s_parent;

            if (!(p = sector(v, clus_lba(v, src.clus))) || memcmp(p + 32, "..", 2))
                return FS_ERR_IO;
            put16(p + 32 + 20, v->type == T_FAT32 ? to_c >> 16 : 0u);
            put16(p + 32 + 26, to_c);
            s_sec_dirty = 1;
            if (sec_flush())
                return FS_ERR_IO;
        }
        return f_create(v, (unsigned)len, src.attr, src.clus, src.size, keep);
    }
}
