/*
 * lib_fsx.c -- what the library needs of a handle that fs.h does not say (in the library's
 * overlay, for the walk and the build alike; fs_target_int.h's names stay out of the other files):
 * its cluster size, and its cluster chain walked toward a far offset a piece at a time (a file of
 * small clusters: one FAT sector per 128 clusters; lib_int.h LIB_CHAIN_FAR).
 */
#include "fs_target_int.h"
#include "lib_int.h"

uint32_t lib_clus_bytes(fs_file_t *f)        /* its volume's cluster, in bytes */
{
    return f && f->v ? f->v->spc * 512u : 512u;
}

/* ---- a file's chain ---- */

uint32_t lib_chain_gap(fs_file_t *f, uint32_t off)
{
    uint32_t want, from;

    if (!f || f->contig || f->first < 2u)
        return 0;
    want = off / (f->v->spc * 512u);
    if (f->mark_clus && want >= f->mark_cidx && (f->clus < 2u || f->cidx < f->mark_cidx || want < f->cidx)) {
        f->clus = f->mark_clus;      /* (the mark is nearer: fs_read walks on from there) */
        f->cidx = f->mark_cidx;
    }
    from = f->clus >= 2u && want >= f->cidx ? f->cidx : 0u;
    return want - from;
}

int lib_chain_walk(fs_file_t *f, uint32_t off, uint64_t t_end)
{
    uint32_t want;

    if (!f || f->first < 2u)
        return -1;
    if (f->contig)
        return 1;
    want = off / (f->v->spc * 512u);
    /* the mark half a far walk before it: the reads a tag reader makes around it (a footer, then
     * the tag before it) all walk on from the mark, none from the start again */
    want = want > LIB_CHAIN_FAR / 2u ? want - LIB_CHAIN_FAR / 2u : 0u;
    (void)lib_chain_gap(f, want * f->v->spc * 512u);
    if (f->clus < 2u || want < f->cidx) {
        f->clus = f->first;
        f->cidx = 0;
    }
    while (f->cidx < want) {
        for (uint32_t k = 0; k < LIB_CHAIN_STEP && f->cidx < want; k++) {
            if (!(f->clus = fat_next(f->v, f->clus)))
                return -1;
            f->cidx++;
        }
        if (f->cidx < want && lib_now_us() >= t_end)
            return 0;                /* (the cursor stays: the next slice goes on from it) */
    }
    f->mark_clus = f->clus;          /* reads behind it restart here, not at the start (fs_read) */
    f->mark_cidx = f->cidx;
    return 1;
}

/* The places (cluster, its index) where the parts of f begin, each part bytes long (pl[k]: at or after
 * k * part, the first of its clusters), walked a step at a time from the last one known (none: the
 * first cluster). f's own cursor is not moved. 1 = all n known, 0 = more (t_end), < 0 = a short chain. */
int lib_chain_places(fs_file_t *f, uint32_t (*pl)[2], int n, uint32_t part, uint64_t t_end)
{
    uint32_t cl = lib_clus_bytes(f), c, i;
    int k = 1;

    if (!f || f->first < 2u)
        return -1;
    if (!pl[0][0]) {
        pl[0][0] = f->first;
        pl[0][1] = 0;
    }
    while (k < n && pl[k][0])
        k++;
    c = pl[k - 1][0];
    i = pl[k - 1][1];
    for (uint32_t s = 0; k < n; ) {
        if ((i + 1u) * cl >= f->size)
            return 1;                    /* (the file ends before part k) */
        if (f->contig)
            c++;
        else if (!(c = fat_next(f->v, c)))
            return -1;
        i++;
        if (i * cl >= (uint32_t)k * part) {
            pl[k][0] = c;
            pl[k][1] = i;
            k++;
        }
        if (++s % LIB_CHAIN_STEP == 0u && lib_now_us() >= t_end)
            return 0;
    }
    return 1;
}
