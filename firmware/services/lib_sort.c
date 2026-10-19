/*
 * lib_sort.c -- an external sort in the 32 KiB scratch buffer, a slice at a time (lib_int.h).
 *
 * Records are [u16 len] + bytes (len <= SRT_REC_MAX), ordered by the caller's comparator, which
 * must be total (every comparator here ends on a unique key: the order does not depend on the
 * sort being stable). Runs: the records that fit in the arena (26 KiB), heap-sorted in place
 * through an array of offsets (resumable: a slice sifts a few hundred nodes), written one after
 * another to one temp file. Merge: up to SRT_FAN runs at a time through cursors that share one
 * read handle (each keeps 1 KiB, so its current record is always contiguous), into the other
 * temp file, level by level until one run is left: that file is the result.
 *
 * Scratch while sorting: [0, 2K) the producer's reader, [2K, 6.5K) this writer, [6.5K, 11K) the
 * index writer (lib_build.c), [11K, 12K) the build's folders reader, [12K, 28K) the arena, or the
 * merge cursors' buffers and the cursors themselves.
 */
#include <string.h>

#include "lib_int.h"

#define SRT_WBUF   ((uint8_t *)lib_scratch + 2048u)        /* LIB_WBUF */
#define SRT_ARENA  ((uint8_t *)lib_scratch + 12288u)
#define SRT_ACAP   (LIB_JOB_SCRATCH - 12288u)
#define SRT_CUR    1024u
#define SRT_FAN    15                        /* x SRT_CUR within the arena, their cursors after */
#define SRT_REC_MAX 1024u

enum { PH_FILL, PH_SORT, PH_WRITE, PH_MSTART, PH_MERGE, PH_DONE, PH_ERR, PH_OPEN, PH_MCLR };

#define s_cur  ((lr_t *)(void *)(SRT_ARENA + SRT_FAN * SRT_CUR))
#define s_heap ((uint8_t *)(s_cur + SRT_FAN))
static fs_file_t *s_in;

#define OFS(s, i) (((uint16_t *)(SRT_ARENA + SRT_ACAP))[-(int)(i) - 1])

static int tmp_name(char *out, int n, int k)
{
    lib_tmp_path(out, n);
    out[strlen(out) - 5] = (char)('0' + k);          /* ".../LIBTn.TMP" */
    return 0;
}

static int open_out(lib_sort_t *s, int k)
{
    char p[48];

    tmp_name(p, sizeof p, k);
    return lw_open(&s->w, p, SRT_WBUF, LIB_WBUF, 0);
}

void srt_begin(lib_sort_t *s, lib_cmp_t cmp, int tmp_a, int tmp_b)
{
    memset(s, 0, sizeof *s);
    s->cmp = cmp;
    s->tmp_in = tmp_a;
    s->tmp_out = tmp_b;
    s->result = -1;
    s->phase = PH_OPEN;                  /* (its output removed first, a piece a pass: srt_step) */
}

int srt_wants_input(const lib_sort_t *s)
{
    return s->phase == PH_FILL && !s->done_in;
}

uint8_t *srt_room(lib_sort_t *s, uint32_t n)
{
    if (s->phase != PH_FILL || n > SRT_REC_MAX)
        return 0;
    if (s->used + n + 2u * (s->nrec + 1u) > SRT_ACAP) {
        s->phase = PH_SORT;                          /* full: sort this run first */
        s->heap_i = s->nrec / 2u;
        s->heap_n = s->nrec;
        s->heap_ph = 0;
        return 0;
    }
    return SRT_ARENA + s->used;
}

void srt_commit(lib_sort_t *s, uint32_t n)
{
    lib_put16(SRT_ARENA + s->used, n);
    OFS(s, s->nrec) = (uint16_t)s->used;
    s->nrec++;
    s->used += n;
}

void srt_end_input(lib_sort_t *s)
{
    s->done_in = 1;
    if (s->phase == PH_FILL) {
        s->phase = PH_SORT;
        s->heap_i = s->nrec / 2u;
        s->heap_n = s->nrec;
        s->heap_ph = 0;
    }
}

static int less(lib_sort_t *s, uint32_t a, uint32_t b)
{
    return s->cmp(SRT_ARENA + OFS(s, a), SRT_ARENA + OFS(s, b)) < 0;
}

static void sift(lib_sort_t *s, uint32_t i, uint32_t n)
{
    for (;;) {
        uint32_t c = 2u * i + 1u, m = i;
        uint16_t t;

        if (c < n && less(s, m, c))
            m = c;
        if (c + 1u < n && less(s, m, c + 1u))
            m = c + 1u;
        if (m == i)
            return;
        t = OFS(s, i);
        OFS(s, i) = OFS(s, m);
        OFS(s, m) = t;
        i = m;
    }
}

/* the heap of merge cursors: the one with the smallest record on top */
static int cur_less(lib_sort_t *s, int a, int b)
{
    int c = s->cmp(s_cur[a].buf + s_cur[a].i, s_cur[b].buf + s_cur[b].i);

    return c < 0 || (c == 0 && a < b);
}

static void heap_down(lib_sort_t *s, int i, int n)
{
    for (;;) {
        int c = 2 * i + 1, m = i;
        uint8_t t;

        if (c < n && cur_less(s, s_heap[c], s_heap[m]))
            m = c;
        if (c + 1 < n && cur_less(s, s_heap[c + 1], s_heap[m]))
            m = c + 1;
        if (m == i)
            return;
        t = s_heap[i];
        s_heap[i] = s_heap[m];
        s_heap[m] = t;
        i = m;
    }
}

/* cursor c's next record in its buffer, or 0 at its run's end */
static int cur_ready(lr_t *r)
{
    const uint8_t *p = lr_peek(r, 2);

    return p && lr_peek(r, lib_le16(p)) != 0;
}

/* A merge level: its output removed (PH_MCLR, a piece a pass), then input and output opened. */
static int level_start(lib_sort_t *s)
{
    char p[48];

    tmp_name(p, sizeof p, s->tmp_in);
    if (!(s_in = fs_open_read(p)))
        return -1;
    if (open_out(s, s->tmp_out)) {
        fs_close(s_in);
        return -1;
    }
    s->grp = 0;
    s->nrun2 = 0;
    s->k = 0;
    return 0;
}

static int group_start(lib_sort_t *s)
{
    uint32_t end = s->grp + SRT_FAN < s->nruns ? s->grp + SRT_FAN : s->nruns;
    int n = 0;

    for (uint32_t i = s->grp; i < end; i++) {
        int c = (int)(i - s->grp);

        lr_attach(&s_cur[c], s_in, s->run[i], s->run[i + 1], SRT_ARENA + (uint32_t)c * SRT_CUR, SRT_CUR);
        if (c)                           /* (its chain walked on from the run before it, not from the
                                          * file's start: 15 such walks were a 6.4 ms slice at 30k) */
            memcpy(s_cur[c].pl, s_cur[c - 1].pl, sizeof s_cur[c].pl);
        if (cur_ready(&s_cur[c]))
            s_heap[n++] = (uint8_t)c;
    }
    s->k = n;
    for (int i = n / 2; i-- > 0; )
        heap_down(s, i, n);
    s->grp = end;
    return n;
}

int srt_step(lib_sort_t *s, uint64_t t_end)
{
    uint32_t tick = 0;

    for (;;) {
        if (((++tick & 15u) == 0 && lib_now_us() >= t_end) || lib_io_dirty())
            return 0;
        switch (s->phase) {
        case PH_FILL:
            if (!s->done_in)
                return 1;
            s->phase = PH_SORT;
            break;
        case PH_SORT:
            if (s->heap_ph == 0) {
                if (s->heap_i) {
                    s->heap_i--;
                    sift(s, s->heap_i, s->nrec);
                    break;
                }
                s->heap_ph = 1;
            }
            if (s->heap_n > 1u) {
                uint16_t t = OFS(s, 0);

                s->heap_n--;
                OFS(s, 0) = OFS(s, s->heap_n);
                OFS(s, s->heap_n) = t;
                sift(s, 0, s->heap_n);
                break;
            }
            s->phase = PH_WRITE;
            s->emit = 0;
            break;
        case PH_WRITE:
            if (s->emit < s->nrec) {
                const uint8_t *r = SRT_ARENA + OFS(s, s->emit++);

                lw_put(&s->w, r, lib_le16(r));
                if (s->w.err)
                    return s->phase = PH_ERR, -1;
                break;
            }
            if (s->nrec) {
                if (s->nruns == LIB_RUNS)
                    return s->phase = PH_ERR, -2;    /* (LIB_RUNS x 26 KiB: never for 65535 tracks) */
                s->run[++s->nruns] = s->w.pos;
            }
            s->total += s->nrec;
            s->nrec = s->used = 0;
            if (!s->done_in) {
                s->phase = PH_FILL;
                return 1;
            }
            s->phase = PH_MSTART;        /* (its close in a slice of its own: its last big write */
            return 0;                    /* then comes first, not after this slice's work) */
        case PH_OPEN: {                  /* the first output: the old file of that name removed */
            char p[48];

            tmp_name(p, sizeof p, s->tmp_in);
            if (!lib_rm_step(p))
                return 0;
            if (open_out(s, s->tmp_in))
                return s->phase = PH_ERR, -1;
            s->phase = PH_FILL;
            return 1;
        }
        case PH_MSTART:                  /* the level's output written */
            if (lw_close(&s->w))
                return s->phase = PH_ERR, -1;
            if (s->nruns <= 1u) {
                s->result = s->tmp_in;
                return s->phase = PH_DONE, 2;
            }
            s->phase = PH_MCLR;
            return 0;
        case PH_MCLR: {
            char p[48];

            tmp_name(p, sizeof p, s->tmp_out);
            if (!lib_rm_step(p) || lib_io_dirty())
                return 0;                /* (the level's start, its 15 cursors' first reads, in a slice
                                          * after the removal's writes: 6.4 ms together at 30k) */
            if (level_start(s) < 0)
                return s->phase = PH_ERR, -1;
            s->phase = PH_MERGE;
            group_start(s);
            break;
        }
        case PH_MERGE:
            if (s->k) {
                int c = s_heap[0];
                lr_t *r = &s_cur[c];
                const uint8_t *p = r->buf + r->i;
                uint32_t len = lib_le16(p);

                lw_put(&s->w, p, len);
                r->i += len;
                if (s->w.err)
                    return s->phase = PH_ERR, -1;
                if (!cur_ready(r))
                    s_heap[0] = s_heap[--s->k];
                heap_down(s, 0, s->k);
                break;
            }
            s->run[++s->nrun2] = s->w.pos;           /* this group's run is written */
            if (s->grp < s->nruns) {
                group_start(s);
                break;
            }
            fs_close(s_in);
            s_in = 0;
            s->nruns = s->nrun2;
            {
                int t = s->tmp_in;

                s->tmp_in = s->tmp_out;
                s->tmp_out = t;
            }
            s->phase = PH_MSTART;        /* (likewise) */
            return 0;
        case PH_DONE:
            return 2;
        default:
            return -1;
        }
    }
}

void srt_abort(lib_sort_t *s)
{
    if (s_in)
        fs_close(s_in);
    s_in = 0;
    if (s->w.f)
        lw_abort(&s->w);
    s->phase = PH_ERR;
}

void srt_forget(void)               /* (its volume went: a dead handle, closed to free it) */
{
    fs_close(s_in);
    s_in = 0;
}
