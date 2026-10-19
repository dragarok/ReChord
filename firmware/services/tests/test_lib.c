/*
 * test_lib.c -- the library's walk, build and index on the host: the device's own code
 * (lib_*.c, tags.c with TAGS_EXT, fs_target.c) over a disk image file. Driven by
 * tools/test_lib.py, which builds the images, changes them between runs and checks the output.
 *
 *   test_lib IMG [--vol 0|1] [--fail-after N] [--vclock US] [--io-busy K] CMD...
 *     index [full]       run the job to the end (or to the failed write); one "stats" line
 *     rows SEC           every row of a section (ALBTRK ARTALB ARTISTS ALBUMS GENALB GENRES SONGS FOLDERS)
 *     letters SEC        its letter index
 *     view               the committed slot's trailer
 *     coll A B [artist]  lib_coll(A, B)
 * --vclock US: a virtual clock, US microseconds on at every lib_now_us() (the slices end after a
 * few tests of the time, whatever the host's speed: the pieces a slice is cut into are the device's).
 * The stats line has the most sectors read in one lib_step() (maxrd).
 * --fail-after N: every block write after the N-th fails (a power cut: the image keeps what was
 * written before). --io-busy K: lib_io_ok() says no in every K-th pass (audio without headroom), and
 * lib_io_dirty() ends a loop every 7th test (a slice that queued blocks).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "services/fs.h"
#include "services/fs_target.h"
#include "services/lib_int.h"

static FILE *s_img;
static long s_fail_after = -1, s_writes, s_reads, s_io_busy, s_io_calls;
static int s_io_no;                      /* storage is off for this lib_step() (lib_io_ok() says no) */
static long s_io_viol;                   /* ... and a sector was read or written in it */
static uint32_t s_scratch[LIB_SCRATCH / 4];
uint32_t *const lib_scratch = s_scratch;
lib_status_t g_lib;
static uint32_t s_serial;

static int img_read(uint32_t lba, uint32_t count, void *buf)
{
    s_reads += count;
    s_io_viol += s_io_no;
    return fseek(s_img, (long)lba * 512L, SEEK_SET) || fread(buf, 512, count, s_img) != count;
}

static int img_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (s_fail_after >= 0 && s_writes >= s_fail_after)
        return -1;
    s_writes++;
    s_io_viol += s_io_no;
    return fseek(s_img, (long)lba * 512L, SEEK_SET) || fwrite(buf, 512, count, s_img) != count;
}

static long s_vclock;
static uint64_t s_vt;

uint64_t lib_now_us(void)
{
    struct timespec t;

    if (s_vclock)
        return s_vt += (uint64_t)s_vclock;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}

int lib_io_ok(void)
{
    return !s_io_no;
}

/* --io-busy: also a slice ended early (as by a block queued) every 7th test of a loop */
int lib_io_dirty(void)
{
    static unsigned long n;

    return (s_io_busy && ++n % 7u == 0) || lib_wdue;
}

void lib_log(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

uint32_t lib_vol_serial(int vol) { return s_serial; }

/* startup/ap_ovl.c on the device: the extended reader in the modes' window; --tags-busy: the
 * USB-C earphones hold it (the walk falls back to the basic reader) */
static int s_tags_busy;
int tags_read_ext(uint32_t (*rd)(void *, uint32_t, void *, uint32_t), void *u, uint32_t size, int kind, void *out);
int ap_lib_tags_ok(void) { return !s_tags_busy; }
int ap_lib_tags(uint32_t (*rd)(void *, uint32_t, void *, uint32_t), void *u, uint32_t size, int kind, void *out)
{
    return s_tags_busy ? -2 : tags_read_ext(rd, u, size, kind, out);
}
void lib_serial_forget(void) { }

uint32_t fwu_crc(uint32_t crc, const uint8_t *p, uint32_t n)
{
    static uint32_t tab[256];

    if (!tab[1])
        for (uint32_t i = 0; i < 256u; i++) {
            uint32_t r = i << 24;
            for (int k = 0; k < 8; k++)
                r = r & 0x80000000u ? r << 1 ^ 0x04C10DB7u : r << 1;
            tab[i] = r;
        }
    while (n--)
        crc = crc << 8 ^ tab[(crc >> 24 ^ *p++) & 0xFFu];
    return crc;
}

static const char *const k_secs[LS_N] = { "ALBTRK", "ARTALB", "ARTISTS", "ALBUMS", "GENALB", "GENRES", "SONGS", "FOLDERS" };

static int sec_of(const char *s)
{
    for (int i = 0; i < LS_N; i++)
        if (!strcmp(s, k_secs[i]))
            return i;
    return -1;
}

static void print_row(int vol, int sec, uint32_t i)
{
    static uint8_t buf[1024];
    lib_row_t r;

    if (idx_row(vol, sec, i, &r, buf, sizeof buf)) {
        printf("%u ERR\n", (unsigned)i);
        return;
    }
    switch (sec) {
    case LS_ALBTRK:
    case LS_SONGS:
        printf("%u trk=%u disc=%u dur=%u year=%u kind=%u clus=%u size=%u slot=%u title=%s artist=%s\n", (unsigned)i,
               r.trk, r.disc, r.dur, r.year, r.kind, (unsigned)r.ref.clus, (unsigned)r.ref.size, r.ref.slot, r.s1, r.s2);
        break;
    case LS_ARTALB:
    case LS_ALBUMS:
    case LS_GENALB:
        printf("%u first=%u n=%u year=%u dur=%u album=%s artist=%s genre=%s\n", (unsigned)i, (unsigned)r.a,
               (unsigned)r.b, r.year, (unsigned)r.c, r.s1, r.s2, r.s3);
        break;
    case LS_ARTISTS:
        printf("%u first=%u n=%u alb=%u nalb=%u name=%s\n", (unsigned)i, (unsigned)r.a, (unsigned)r.b,
               (unsigned)r.c, (unsigned)r.d, r.s1);
        break;
    case LS_GENRES:
        printf("%u alb=%u nalb=%u name=%s\n", (unsigned)i, (unsigned)r.c, (unsigned)r.d, r.s1);
        break;
    case LS_FOLDERS:
        printf("%u clus=%u raw=%u path=%s\n", (unsigned)i, (unsigned)r.a, (unsigned)r.b, r.s1);
        break;
    }
}

int main(int argc, char **argv)
{
    int vol = 1, a = 1;
    const char *names[2] = { "Internal", "SD" };

    if (argc < 3)
        return 2;
    if (strcmp(argv[1], "coll") == 0 && argc >= 4) {
        int r = lib_coll((const uint8_t *)argv[2], (unsigned)strlen(argv[2]), (const uint8_t *)argv[3],
                         (unsigned)strlen(argv[3]), argc > 4 ? LC_ARTIST : 0);

        printf("%d %d\n", r < 0 ? -1 : r > 0, lib_letter((const uint8_t *)argv[2], (unsigned)strlen(argv[2]), argc > 4 ? LC_ARTIST : 0));
        return 0;
    }
    if (!(s_img = fopen(argv[a++], "r+b")))
        return 2;
    for (; a < argc && argv[a][0] == '-' && argv[a][1] == '-'; a += 2) {
        if (!strcmp(argv[a], "--vol"))
            vol = atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "--fail-after"))
            s_fail_after = atol(argv[a + 1]);
        else if (!strcmp(argv[a], "--vclock"))
            s_vclock = atol(argv[a + 1]);
        else if (!strcmp(argv[a], "--io-busy"))
            s_io_busy = atol(argv[a + 1]);
        else if (!strcmp(argv[a], "--tags-busy"))
            s_tags_busy = atoi(argv[a + 1]);
        else if (!strcmp(argv[a], "--serial"))
            s_serial = (uint32_t)strtoul(argv[a + 1], 0, 16);
    }
    if (fs_target_mount_rw(vol, names[vol], img_read, img_write, 0)) {
        printf("mount failed\n");
        return 2;
    }
    g_lib.data = LIB_DATA_SD;
    g_lib.mounted[vol] = 1;
    g_lib.writable[vol] = (uint8_t)fs_target_writable(vol);
    lib_boot();
    for (; a < argc; a++) {
        if (!strcmp(argv[a], "index")) {
            int full = a + 1 < argc && !strcmp(argv[a + 1], "full");
            long r0 = s_reads, w0 = s_writes, maxrd = 0;
            unsigned long steps = 0;

            if (full)
                a++;
            lib_request(vol, full);
            for (;;) {
                int more;

                /* --io-busy K: storage off for every K-th pass (audio without headroom): the
                 * harness says so, not the job, so a pass that never asks is caught too */
                long rs = s_reads;

                s_io_no = s_io_busy && ++s_io_calls % (unsigned long)s_io_busy == 0;
                more = lib_step();
                s_io_no = 0;
                if (s_reads - rs > maxrd)
                    maxrd = s_reads - rs;
                if (!more)
                    break;
                steps++;
            }
            printf("stats reads=%ld writes=%ld steps=%lu ok=%d tracks=%u seq=%u slot=%c io_viol=%ld maxrd=%ld\n", s_reads - r0,
                   s_writes - w0, steps, g_lib_view[vol].ok, (unsigned)g_lib_view[vol].tr.ntracks, (unsigned)g_lib_view[vol].tr.seq,
                   'A' + g_lib_view[vol].slot, s_io_viol, maxrd);
        } else if (!strcmp(argv[a], "rows") && a + 1 < argc) {
            int sec = sec_of(argv[++a]);

            if (sec < 0)
                return 2;
            for (uint32_t i = 0; i < idx_rows(vol, sec); i++)
                print_row(vol, sec, i);
        } else if (!strcmp(argv[a], "letters") && a + 1 < argc) {
            int sec = sec_of(argv[++a]);

            for (int l = 0; l < LIB_LETTERS; l++)
                printf("%u%c", (unsigned)idx_letter(vol, sec, l), l + 1 < LIB_LETTERS ? ' ' : '\n');
        } else if (!strcmp(argv[a], "view")) {
            lib_trailer_t *t = &g_lib_view[vol].tr;

            printf("view ok=%d slot=%c dir=%s base=%s seq=%u tracks=%u folders=%u albums=%u artists=%u genres=%u raw=%u\n",
                   g_lib_view[vol].ok, 'A' + g_lib_view[vol].slot, g_lib_view[vol].dir, g_lib_view[vol].base,
                   (unsigned)t->seq, (unsigned)t->ntracks, (unsigned)t->nfolders, (unsigned)t->nalbums,
                   (unsigned)t->nartists, (unsigned)t->ngenres, (unsigned)t->raw_size);
        }
    }
    fflush(stdout);
    fclose(s_img);
    return 0;
}
