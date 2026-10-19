/*
 * lib_dir.c -- the walk's reads that take more than a slice, in pieces (lib_int.h): a directory
 * read that stops and goes on from where it stopped, and a file's cluster chain walked toward a far
 * offset. In the walk's overlay (OVL_LWALK); it keeps the file system's own cursors
 * (fs_target_int.h) between slices.
 *
 * A directory: fs_scan reads a whole folder in one call. A folder of hundreds of entries with long
 * names is dozens of sectors and a callback per entry: 9-12 ms in one slice on the 10k/30k-track
 * cards of m46 scale. Here the cursor (cluster, sector, entry) is the scan's state: a slice reads
 * until its time is up, the next goes on from that entry, nothing is read twice. A continuation
 * (more subfolders than the walk's stack takes at once) starts at its entry by cluster arithmetic.
 *
 * Between two slices the volume may change: its gen says (a pull, a remount: the walk stops). Our
 * own files may be written meanwhile (on a volume without a RECHORD folder, the root's): a new entry
 * goes into a free slot or at the end, so an entry already read is not moved and none is read
 * twice; such files are not music and not counted.
 */
#include <string.h>

#include "fs_target_int.h"
#include "lib_int.h"

typedef struct {
    cur_t d;                         /* the next entry */
    uint32_t pclus, psize;           /* the directory itself (the refs' parent) */
    uint16_t gen;                    /* its volume's, at the start */
    uint8_t vol, pcontig, end, pad;
} ds_t;

typedef char ds_fits[sizeof(ds_t) <= sizeof(lib_dscan_t) ? 1 : -1];

int lib_dscan_start(lib_dscan_t *s_, const char *root, const fs_ref_t *dir, uint32_t from)
{
    ds_t *s = (ds_t *)(void *)s_;
    const vol_t *v;
    ent_t e;

    memset(s, 0, sizeof *s);
    if (root) {
        if (resolve(root, &v) != 0 || !s_ent.is_dir)
            return FS_ERR_OPEN;
        e = s_ent;
    } else {
        if (!dir || dir->vol >= FS_VOLUMES || !s_vol[dir->vol].type || !(dir->flags & FS_REF_DIR))
            return FS_ERR_ARG;
        v = &s_vol[dir->vol];
        memset(&e, 0, sizeof e);
        e.is_dir = 1;
        e.clus = dir->clus;
        e.size = dir->size;
        e.contig = (dir->flags & FS_REF_CONTIG) != 0;
    }
    cur_init(&s->d, v, &e);
    s->pclus = e.clus;
    s->psize = e.size;
    s->pcontig = (uint8_t)e.contig;
    s->vol = (uint8_t)(v - s_vol);
    s->gen = v->gen;
    if (from) {                      /* entry `from`: whole clusters skipped through the FAT */
        uint32_t byte = from * 32u, cl = v->spc * 512u;

        if (s->d.clus == 0u) {       /* (FAT16's fixed root: no chain) */
            s->d.sec = byte / 512u;
        } else {
            for (uint32_t k = byte / cl; k && !s->end; k--) {
                if (!s->d.contig)
                    s->end = !(s->d.clus = fat_next(v, s->d.clus));
                else if (s->d.left <= 1u)
                    s->end = 1;      /* (the directory ends before `from`) */
                else
                    s->d.left--, s->d.clus++;
            }
            s->d.sec = byte % cl / 512u;
        }
        s->d.off = byte % 512u;
        s->d.idx = from;
    }
    return 0;
}

int lib_dscan(lib_dscan_t *s_, fs_scan_cb cb, void *u, uint64_t t_end)
{
    ds_t *s = (ds_t *)(void *)s_;
    const vol_t *v = s->d.v;
    unsigned n = 0;

    if (!v || !v->type || v->gen != s->gen)
        return FS_ERR_GONE;
    while (!s->end) {
        cur_t keep = s->d;
        fs_ref_t r;

        if ((++n & 7u) == 0u && lib_now_us() >= t_end)
            return 0;
        if (dir_read(&s->d, &s_ent) != 1) {
            s->end = 1;
            break;
        }
        r.clus = s_ent.clus;
        r.size = s_ent.size;
        r.pclus = s->pclus;
        r.psize = s->psize;
        r.slot = (uint16_t)s_ent.slot;
        r.vol = s->vol;
        r.flags = (uint8_t)((s_ent.is_dir ? FS_REF_DIR : 0) | (s_ent.contig ? FS_REF_CONTIG : 0) |
                            (s->pcontig ? FS_REF_PCONTIG : 0));
        if (cb(u, s_ent.name, &r)) {
            s->d = keep;             /* (that entry again, next time) */
            return 2;
        }
    }
    return 1;
}

uint32_t lib_dscan_wtime(void)
{
    return s_ent.wtime;
}

/* A folder's name from its entry (fs_ref_name reads its directory from the start: in a folder of
 * hundreds of subfolders, each name asked of it cost dozens of sectors). Read from just before its
 * slot: on FAT its long name's entries come first (at most 20 of them), on exFAT after it. */
static int name_cb(void *u, const char *name, const fs_ref_t *r)
{
    const fs_ref_t *want = ((const fs_ref_t *const *)u)[0];
    char *out = ((char *const *)u)[1];

    if (r->slot < want->slot)
        return 0;
    if (r->slot == want->slot && r->clus == want->clus) {
        size_t n = strlen(name);

        memcpy(out, name, n > 255u ? 255u : n);
        out[n > 255u ? 255u : n] = 0;
    }
    return 1;
}

int lib_ref_name(const fs_ref_t *r, char *out)
{
    fs_ref_t d;
    lib_dscan_t s;
    const void *u[2] = { r, out };
    const vol_t *v = r->vol < FS_VOLUMES ? &s_vol[r->vol] : 0;

    if (!v || !v->type)
        return FS_ERR_ARG;
    memset(&d, 0, sizeof d);
    d.clus = r->pclus;
    d.size = r->psize;
    d.vol = r->vol;
    d.flags = (uint8_t)(FS_REF_DIR | (r->flags & FS_REF_PCONTIG ? FS_REF_CONTIG : 0));
    out[0] = 0;
    if (lib_dscan_start(&s, 0, &d, v->type == T_EXFAT || r->slot < 20u ? (v->type == T_EXFAT ? r->slot : 0u) : r->slot - 20u) ||
        lib_dscan(&s, name_cb, (void *)u, ~0ull) < 0)
        return FS_ERR_IO;
    return out[0] ? 0 : FS_ERR_OPEN;
}
