/*
 * lib_job.c -- the library's background job and where its files live (docs/LIBRARY.md 1-3).
 *
 * One job at a time: a refresh of one volume, the walk then the build, LIB_STEP_US a pass and only
 * while lib_io_ok() (audio has its headroom; the log and the settings use the same rule). Asked
 * for by events: boot (Internal when its index is missing or the last USB session wrote, the SD
 * card always: it may have been edited elsewhere), a card inserted, a USB disk session that wrote,
 * Refresh in the menu (a full walk: every file's tags read again). A card pulled out while its job
 * runs ends the job at once (its handles are gone with the volume; nothing of its slot counts).
 *
 * Places (lib_place_names): Internal's index is always on Internal. The SD card's index, the
 * favourites and the playlists go where the setting says (g_lib_data: LIB_DATA_SD by default):
 * on the card when it is in and takes writes, else on Internal (a notice in the log and on the
 * screen, once per change), the card's index then named by its serial. A folder RECHORD at the
 * volume's root holds them (fs_mkdir makes it when the FS layer can; until then, the root).
 *
 * The scratch buffer (32 KiB, services/scratch.h) is the job's while it runs; the USB disk session,
 * the updater and the BT listen test use it too, from code that holds the main loop. The job keeps
 * a canary in its last word: a slice that finds it changed starts the job again.
 */
#include <stdio.h>
#include <string.h>

#include "lib_int.h"
#include "lib.h"
#include "fs_target.h"

extern int fs_mkdir(const char *path) __attribute__((weak));     /* sdrw: FAT32/exFAT mkdir */
extern int fs_target_sync(int idx) __attribute__((weak));         /* sdrw: the card's writes on it */
/* the screens' side (ui/lib_ui.c; absent in the host test) */
extern int lib_adding(void) __attribute__((weak));
extern int lib_add_step(uint32_t us) __attribute__((weak));
extern void lib_notice(const char *s) __attribute__((weak));
extern int m3u_move_all(const char *from) __attribute__((weak));

static lib_job_t s_job;
static int s_active = -1;            /* the volume being indexed, -1 */
static uint8_t s_want[2];            /* 0, 1 refresh, 2 full */
static uint8_t s_gone[2];            /* its volume went away under it */
static uint32_t s_canary;
static uint16_t s_clean;             /* files to remove when idle: bits 0..4 LIBT0..4.TMP, 8..11 the
                                      * card's index at the other place */
static char s_clean_dir[24];         /* where the temps are */
static char s_dir[2][24];            /* data directory per volume, "" = not looked for yet */
lib_status_t g_lib;

#define CANARY (lib_scratch[LIB_SCRATCH / 4u - 1u])

static const char *vol_name(int v) { return v ? "SD" : "Internal"; }

/* The directory for library files on volume v: /V/RECHORD, made if it is not there (when the FS
 * layer can make folders), else the root. */
const char *lib_data_dir(int v)
{
    char p[24];

    if (!s_dir[v][0]) {
        snprintf(p, sizeof p, "/%s/RECHORD", vol_name(v));
        if (fs_exists(p) || (fs_mkdir && g_lib.writable[v] && fs_mkdir(p) == 0 && fs_exists(p)))
            snprintf(s_dir[v], sizeof s_dir[v], "%s", p);
        else
            snprintf(s_dir[v], sizeof s_dir[v], "/%s", vol_name(v));
    }
    return s_dir[v];
}

/* The same place, only looked for, never made: for reading what was written there before (a
 * volume that is not the library's place now gets no folder from it). */
static const char *seek_dir(int v, char *buf, int n)
{
    if (s_dir[v][0])
        return s_dir[v];
    snprintf(buf, (unsigned)n, "/%s/RECHORD", vol_name(v));
    if (!fs_exists(buf))
        snprintf(buf, (unsigned)n, "/%s", vol_name(v));
    return buf;
}

/* Where lists and the card's index go now: the chosen volume when it is in and takes writes. */
int lib_lists_vol(void)
{
    return g_lib.data == LIB_DATA_SD && g_lib.mounted[1] && g_lib.writable[1] ? 1 : 0;
}

void lib_place_names(int vol, char *dir, int dn, char *base, int bn)
{
    int on = vol == 0 ? 0 : lib_lists_vol();

    snprintf(dir, (unsigned)dn, "%s", lib_data_dir(on));
    if (vol == 0)
        snprintf(base, (unsigned)bn, "INT");
    else if (on == 1)
        snprintf(base, (unsigned)bn, "SD");
    else
        snprintf(base, (unsigned)bn, "%08X", (unsigned)lib_vol_serial(1));
}

/* the place the card's index is not in now (to find an index written before a change) */
static void other_place(char *dir, int dn, char *base, int bn)
{
    if (lib_lists_vol() == 1) {
        snprintf(dir, (unsigned)dn, "%s", lib_data_dir(0));
        snprintf(base, (unsigned)bn, "%08X", (unsigned)lib_vol_serial(1));
    } else {
        char b[24];

        snprintf(dir, (unsigned)dn, "%s", g_lib.mounted[1] ? seek_dir(1, b, sizeof b) : "");
        snprintf(base, (unsigned)bn, "SD");
    }
}

const char *lib_job_dir(void)
{
    return s_job.dir;
}

void lib_tmp_path(char *out, int n)
{
    snprintf(out, (unsigned)n, "%s/LIBT0.TMP", s_job.dir);
}

/* ---- the job ---- */

enum { J_IDLE, J_SCAN, J_BUILD };

/* The phase's code: the walk (J_SCAN) or the build (J_BUILD), each an overlay of its own in the
 * phase window (lib.h): loaded here, at a phase's start or when it is not in the window, never
 * while code in it runs (nothing there calls back into this file). NULL: refused (damaged). */
#ifdef LIB_HOST
static const lib_phase_api_t k_phase[2] = {
    { scan_start, scan_step, scan_abort, 0 }, { build_start, build_step, build_abort, build_forget } };
#define phase(b) (&k_phase[b])
#else
#include "ovl.h"
#define phase(b) ((const lib_phase_api_t *)ovl_get((b) ? OVL_LBUILD : OVL_LWALK))
#endif

static void job_end(int ok)
{
    lib_job_t *j = &s_job;
    uint32_t ms = (uint32_t)((lib_now_us() - j->t_start) / 1000u);

    if (ok) {
        lib_log("lib: %s indexed: %u tracks, %u folders, %u albums, %u artists, %u genres; %u files read%s, "
                "%u reused, %u copied", vol_name(j->vol),
                (unsigned)j->ntracks, (unsigned)j->nfolders, (unsigned)j->n_alb, (unsigned)j->n_art,
                (unsigned)j->n_gen, (unsigned)(j->read_tags - j->reads0), j->reads0 ? " (more before a power cut)" : "",
                (unsigned)j->reused, (unsigned)j->copied);
        lib_log("lib: %s cost: %u ms (%u ms busy, %u slices, worst %u us, %u passes deferred); "
                "%u sectors read, %u written", vol_name(j->vol),
                (unsigned)ms, (unsigned)(j->t_busy / 1000u), (unsigned)j->steps, (unsigned)j->worst_us,
                (unsigned)j->deferred, (unsigned)j->nrd0, (unsigned)j->nwr0);
    }
    j->state = J_IDLE;
    s_active = -1;
    g_lib.busy = 0;
    g_lib.changed++;
}

static void job_abort(const char *why)
{
    lib_job_t *j = &s_job;
    const lib_phase_api_t *ph;

    if (s_active < 0)
        return;
    if (s_gone[strncmp(j->dir, "/SD", 3) == 0]) {   /* the job's files were there: dead handles
                                                     * (FS_ERR_GONE), closed only to free them */
        fs_close(j->raw.f), fs_close(j->trk.f), fs_close(j->idx.f), fs_close(j->in.f), fs_close(j->srt.w.f);
        j->raw.f = j->trk.f = j->idx.f = j->in.f = j->srt.w.f = 0;
        if (j->state == J_BUILD && (ph = phase(1)))   /* (the walk holds no handles of its own) */
            ph->forget();
    }
    if (s_gone[strncmp(g_lib_view[j->vol].dir, "/SD", 3) == 0]) {
        fs_close(j->old.f), fs_close(j->otrk.f);
        j->old.f = j->otrk.f = 0;        /* (the previous walk's, read from the view's place) */
    }
    if ((j->state == J_SCAN || j->state == J_BUILD) && (ph = phase(j->state == J_BUILD)))
        ph->abort(j);
    lib_log("lib: %s indexing stopped: %s", vol_name(j->vol), why);
    j->state = J_IDLE;
    s_active = -1;
    g_lib.busy = 0;
}

static int job_start(int vol, int full)
{
    lib_job_t *j = &s_job;
    lib_view_t *v = &g_lib_view[vol];
    const lib_phase_api_t *ph = phase(0);
    int r;

    if (!ph) {
        lib_log("lib: %s: the walk's code is not usable (ovl.c says why)", vol_name(vol));
        return -1;
    }
    memset(j, 0, sizeof *j);
    j->vol = vol;
    j->full = full;
    snprintf(j->root, sizeof j->root, "/%s", vol_name(vol));
    lib_place_names(vol, j->dir, sizeof j->dir, j->base, sizeof j->base);
    j->seq = (v->ok ? v->tr.seq : 0u) + 1u;
    j->slot = v->ok && !strcmp(v->dir, j->dir) && !strcmp(v->base, j->base) ? 1 - v->slot : 0;
    j->t_start = lib_now_us();
    j->nrd0 = j->nwr0 = 0;              /* (the job's own sectors: counted slice by slice) */
    s_active = vol;
    g_lib.busy = 1;
    g_lib.busy_vol = (uint8_t)vol;
    g_lib.pct = 0;
    r = ph->start(j);
    if (r == LIB_WALKED) {               /* (the build starts in the next slice: its code loaded then) */
        j->state = J_BUILD;
        j->bph = -1;
    } else if (r) {
        lib_log("lib: %s: cannot write its index in %s", vol_name(vol), j->dir);
        s_active = -1;
        g_lib.busy = 0;
        return -1;
    } else {
        j->state = J_SCAN;
    }
    lib_log("lib: %s indexing%s into %s/%s.*%c (seq %u)", vol_name(vol), full ? " (full)" : "", j->dir,
            j->base, 'A' + j->slot, (unsigned)j->seq);
    return 0;
}

/* The index just committed: the views switch to it; one at the other place is now stale. */
static void committed(void)
{
    lib_job_t *j = &s_job;
    char dir[24], base[12], p[48];

    if (idx_load(j->vol))
        lib_log("lib: %s: the new index does not read back", vol_name(j->vol));
    snprintf(s_clean_dir, sizeof s_clean_dir, "%s", j->dir);
    s_clean |= 0x1Fu;
    if (j->vol == 1)
        s_clean |= 0xF00u;               /* (an index the card had at its other place is stale now) */
    (void)dir, (void)base, (void)p;
    if (j->vol == 0) {
        snprintf(p, sizeof p, "%s/INT.DRT", j->dir);
        if (fs_exists(p))
            fs_remove(p);
    }
}

void lib_request(int vol, int full)
{
    if (vol < 0 || vol > 1)
        return;
    if (s_want[vol] < (full ? 2 : 1))
        s_want[vol] = (uint8_t)(full ? 2 : 1);
    if (s_active == vol && full)
        job_abort("a full refresh asked for");
}

/* Where the lists are now, and the one-line notice when that is not where the setting says. */
static uint8_t s_was_lists = 0xFF;

static void lists_place(void)
{
    int now = lib_lists_vol();

    g_lib.lists_vol = (uint8_t)now;
    if (now == s_was_lists)
        return;
    if (g_lib.data == LIB_DATA_SD && now == 0) {
        const char *why = !g_lib.mounted[1] ? "no SD card" : "the SD card is read-only";

        lib_log("lib: library data on Internal: %s", why);
        if (lib_notice && s_was_lists != 0xFF)
            lib_notice(!g_lib.mounted[1] ? "Lists on Internal: no SD card" : "Lists on Internal: SD read-only");
    } else if (s_was_lists != 0xFF) {
        lib_log("lib: library data on %s", now ? "the SD card" : "Internal");
    }
    /* back on the card (the setting's place): what was kept on Internal meanwhile goes there */
    if (now == 1 && m3u_move_all)
        m3u_move_all(lib_data_dir(0));
    s_was_lists = (uint8_t)now;
}

/* Settings > Library data changed: the lists to the new place, the card's index there next. */
void lib_data_changed(void)
{
    int was = s_was_lists;

    lists_place();
    if (was == 1 && g_lib.lists_vol == 0 && m3u_move_all && g_lib.mounted[1]) {
        char b[24];

        m3u_move_all(seek_dir(1, b, sizeof b));   /* (to Internal: the card's lists come along) */
    }
    if (g_lib.mounted[1]) {
        if (s_active == 1)
            job_abort("the library's place changed");
        if (idx_load(1))
            lib_view_other(1);
        lib_request(1, 0);
    }
    g_lib.changed++;
}

uint32_t lib_slice_wr;

int lib_step(void)
{
    lib_job_t *j = &s_job;
    uint64_t t0, t_end, us;
    int r;

    if (CANARY != s_canary) {            /* someone else had the scratch buffer (it is ours now) */
        idx_cache_drop();
        if (s_active >= 0) {
            job_abort("the scratch buffer was used (USB disk, update): again");
            s_want[j->vol] = 1;
        }
        s_canary = CANARY = 0x4C49425Au ^ (uint32_t)lib_now_us();
    }

    if (lib_adding && lib_adding()) {    /* (songs being added to a list: a slice of their own) */
        lib_add_step(LIB_STEP_US);
        return 1;
    }
    if (s_active < 0) {
        int vol = s_want[1] && g_lib.mounted[1] ? 1 : s_want[0] && g_lib.mounted[0] ? 0 : -1;

        if ((s_clean || vol >= 0 || lib_sd_wrote) && !lib_io_ok())
            return 1;                    /* (a job's start and the clean-up use storage too) */
        if (lib_sd_wrote) {              /* the index or a list written on the card: all of it there */
            lib_sd_wrote = 0;
            if (fs_target_sync && g_lib.mounted[1])
                fs_target_sync(1);
            return 1;
        }
        if (s_clean) {    /* one file a pass: the sorter's, an index left behind */
            char p[48], dir[24], base[12];
            int k = 0;

            while (!(s_clean >> k & 1u))
                k++;
            s_clean &= (uint16_t)~(1u << k);
            if (k < 8)
                snprintf(p, sizeof p, "%s/LIBT%d.TMP", s_clean_dir, k);
            else {
                other_place(dir, sizeof dir, base, sizeof base);
                snprintf(p, sizeof p, "%s/%s.R%c%c", dir, base, k < 10 ? 'W' : 'T', 'A' + (k & 1));
                if (!dir[0] || !g_lib.mounted[1])
                    p[0] = 0;
                if (k == 8) {            /* (its IDX first: then no slot there counts) */
                    char q[48];

                    snprintf(q, sizeof q, "%s/%s.IXA", dir, base);
                    if (p[0] && (!lib_rm_step(q) || (q[strlen(q) - 1] = 'B', !lib_rm_step(q)))) {
                        s_clean |= (uint16_t)(1u << k);
                        return 1;
                    }
                }
            }
            if (p[0] && !lib_rm_step(p))
                s_clean |= (uint16_t)(1u << k);  /* (a big file: a piece a pass) */
            return 1;
        }
        if (vol < 0)
            return 0;
        r = s_want[vol] == 2;
        s_want[vol] = 0;
        if (job_start(vol, r))
            return 0;
        return 1;
    }

    if (!lib_io_ok()) {
        j->deferred++;                   /* (audio without headroom, or a block being programmed) */
        return 1;
    }
    t0 = lib_now_us();
    t_end = t0 + LIB_STEP_US;
    {
        uint32_t rd0 = fs_target_nrd, wr0 = lib_slice_wr = fs_target_nwr;

        const lib_phase_api_t *ph = phase(j->state == J_BUILD);

        r = !ph ? -1 : j->state == J_SCAN ? ph->step(j, t_end) : j->bph < 0 ? (ph->start(j) ? -1 : 0) : ph->step(j, t_end);
        j->nrd0 += fs_target_nrd - rd0;
        j->nwr0 += fs_target_nwr - wr0;
    }
    if (j->state == J_SCAN) {
        if (r == LIB_UNCHANGED) {        /* nothing to write, nothing to build */
            us = lib_now_us() - t0;
            j->t_busy += us;
            j->steps++;
            lib_log("lib: %s unchanged: %u tracks, %u folders, %u ms (%u ms busy, %u slices, worst %u us, %u passes "
                    "deferred); %u sectors read, %u written", vol_name(j->vol), (unsigned)j->ntracks,
                    (unsigned)j->nfolders, (unsigned)((lib_now_us() - j->t_start) / 1000u), (unsigned)(j->t_busy / 1000u),
                    (unsigned)j->steps, (unsigned)(us > j->worst_us ? us : j->worst_us), (unsigned)j->deferred,
                    (unsigned)j->nrd0, (unsigned)j->nwr0);
            if (j->vol == 0) {
                char p[48];

                snprintf(p, sizeof p, "%s/INT.DRT", j->dir);
                if (fs_exists(p))
                    fs_remove(p);
            }
            j->state = J_IDLE;
            s_active = -1;
            g_lib.busy = 0;
            return 1;
        }
        if (r == 1)
            j->state = J_BUILD, j->bph = -1;     /* (the build starts in the next slice) */
        g_lib.pct = (uint8_t)(g_lib_view[j->vol].ok && g_lib_view[j->vol].tr.nfolders ?
                              60u * (j->nfolders < g_lib_view[j->vol].tr.nfolders ? j->nfolders : g_lib_view[j->vol].tr.nfolders) /
                              g_lib_view[j->vol].tr.nfolders : j->nfolders % 50u);
        if (r == 1)
            r = 0;
    } else {
        g_lib.pct = (uint8_t)(60u + (unsigned)j->bph * 40u / 15u);
    }
    us = lib_now_us() - t0;
    j->t_busy += us;
    j->steps++;
    if (us > j->worst_us)
        j->worst_us = (uint32_t)us;
    if (us > 2u * LIB_STEP_US)           /* (a slice over its time: where, and its sectors, for the log and the tests) */
        lib_log("lib: slice %u us in %s %d, %u sectors written", (unsigned)us, j->state == J_SCAN ? "walk" : "build",
                j->state == J_SCAN ? j->wst : j->bph, (unsigned)(fs_target_nwr - lib_slice_wr));
    if (r < 0) {
        job_abort(j->state == J_SCAN ? "the walk failed (I/O)" : "the build failed (I/O)");
        return 1;
    }
    if (r == 1 && j->state == J_BUILD) {
        committed();
        job_end(1);
        return 1;
    }
    return 1;
}

void lib_vol_event(int vol, int ev)
{
    if (vol < 0 || vol > 1)
        return;
    switch (ev) {
    case LIB_EV_GONE:
        if (vol == 1)
            lib_serial_forget();
        s_gone[vol] = 1;
        if (s_active == vol || (s_active == 1 && vol == 0 && lib_lists_vol() == 0))
            job_abort(vol ? "the card was removed" : "Internal went away");
        lib_forget_handles_vol(vol);
        g_lib.mounted[vol] = 0;
        g_lib.writable[vol] = 0;
        s_dir[vol][0] = 0;
        g_lib.changed++;
        s_gone[vol] = 0;
        lists_place();
        break;
    case LIB_EV_MOUNTED:
        if (vol == 1)
            lib_serial_forget();
        s_dir[vol][0] = 0;
        lists_place();
        idx_load(vol);
        if (vol == 1 && !g_lib_view[1].ok)
            lib_view_other(1);
        lib_request(vol, 0);
        g_lib.changed++;
        break;
    case LIB_EV_WROTE:                /* a USB disk session wrote to it */
        if (s_active == vol)
            job_abort("the computer wrote to it");
        if (g_lib.mounted[1] && idx_load(1))
            lib_view_other(1);
        {
            char p[48];
            fs_file_t *f;

            snprintf(p, sizeof p, "%s/INT.DRT", lib_data_dir(0));
            if (vol == 0 && (f = fs_open_write(p)))
                fs_close(f);
        }
        idx_load(vol);
        lib_request(vol, 0);
        g_lib.changed++;
        break;
    case LIB_EV_SUSPEND:              /* storage is about to be the computer's */
        if (s_active >= 0) {
            int v = s_active;

            job_abort("a USB disk session");
            s_want[v] = 1;
        }
        idx_drop(0);
        idx_drop(1);
        break;
    case LIB_EV_RESUME:               /* back from a session that wrote nothing */
        for (int v = 0; v < 2; v++)
            if (g_lib.mounted[v] && idx_load(v) && v == 1)
                lib_view_other(1);
        g_lib.changed++;
        break;
    }
}

/* At boot, after the volumes are mounted and the settings read. */
void lib_boot(void)
{
    char p[48];

    s_dir[0][0] = s_dir[1][0] = 0;
    if (g_lib.mounted[0]) {
        idx_load(0);
        snprintf(p, sizeof p, "%s/INT.DRT", lib_data_dir(0));
        if (!g_lib_view[0].ok || fs_exists(p))
            s_want[0] = 1;
    }
    if (g_lib.mounted[1]) {
        idx_load(1);
        if (!g_lib_view[1].ok)
            lib_view_other(1);
        s_want[1] = 1;
    }
    lists_place();
    g_lib.changed++;
}

/* The card's index may be at the other place (written before the setting or the card's write
 * protection changed): the views read it from there until the next refresh writes the new place. */
int lib_view_other(int vol)
{
    char dir[24], base[12];

    if (vol != 1)
        return -1;
    other_place(dir, sizeof dir, base, sizeof base);
    return dir[0] ? idx_load_at(1, dir, base) : -1;
}

int lib_busy(int *vol, int *pct)
{
    if (vol)
        *vol = s_active;
    if (pct)
        *pct = g_lib.pct;
    return s_active >= 0;
}
