/* Host harness for the SD card's writer (fs_target.c + fs_write_ext.c): mounts a card image (a bare
 * volume or MBR partition 0) as the writable volume "SD", runs the commands given as arguments, one
 * result line each. Driven by snowsky_echo_mini tests/fs_sdrw.py, which checks the image with
 * tools/fsck_card.py (and macOS fsck_msdos) afterwards.
 *
 *   test_fs_sdrw IMG [--cut N] [--ro] CMD...
 *     w:PATH:SIZE:SEED:CHUNK[:MIS]  create/truncate, SIZE pattern bytes in CHUNK-byte fs_write calls
 *     a:PATH:SIZE:SEED:CHUNK[:MIS]  same, append mode
 *     rm:PATH  ex:PATH  cat:PATH  md:PATH  mv:FROM:TO (a ':' in names: use '|' instead)
 *     st:PATH  (size, write time, attributes)    ls:PATH (every name, in order)
 *     sync     (fs_target_sync)                    busy:PATH (second write open must fail)
 *     gone     (the medium gone: every block call answers FS_BLK_GONE from now on)
 *     unmount  (fs_target_unmount; the open handles go dead)
 *     hold:PATH  keep a read handle open on PATH (for gone / unmount); rd: read from it
 *   --cut N: every block write after the first N fails (a power cut). --ro: mount read-only.
 * Pattern byte i of seed s: (s * 31 + i * 7 + (i >> 9)) & 0xFF. Clock: 2026-10-19 12:34:56. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/fs.h"
#include "services/fs_target.h"

static FILE *s_img;
static long s_cut = -1;
static int s_gone;
static unsigned long s_wcalls, s_wsecs, s_rcalls;
static fs_file_t *s_hold;

static void need_aligned(const void *buf)
{
    if ((uintptr_t)buf & 3u) {
        fprintf(stderr, "unaligned block buffer %p\n", buf);
        exit(3);
    }
}

static int img_read(uint32_t lba, uint32_t count, void *buf)
{
    need_aligned(buf);
    if (s_gone)
        return FS_BLK_GONE;
    s_rcalls++;
    return fseek(s_img, (long)lba * 512L, SEEK_SET) || fread(buf, 512, count, s_img) != count;
}

static int img_write(uint32_t lba, uint32_t count, const void *buf)
{
    need_aligned(buf);
    if (s_gone)
        return FS_BLK_GONE;
    if (s_cut >= 0 && (long)s_wcalls >= s_cut)
        return -1;
    s_wcalls++;
    s_wsecs += count;
    if (getenv("FSW_TRACE"))
        fprintf(stderr, "W %u+%u\n", (unsigned)lba, (unsigned)count);
    return fseek(s_img, (long)lba * 512L, SEEK_SET) || fwrite(buf, 512, count, s_img) != count || fflush(s_img);
}

static uint32_t clock_now(void)
{
    return (uint32_t)((46u << 9 | 10u << 5 | 19u) << 16 | 12u << 11 | 34u << 5 | 28u);
}

static uint8_t pat(unsigned seed, unsigned long i)
{
    return (uint8_t)(seed * 31u + i * 7u + (i >> 9));
}

static char *unbar(char *p)
{
    for (char *q = p; *q; q++)
        if (*q == '|')
            *q = ':';
    return p;
}

static void do_write(char *arg, int append)
{
    char *path = unbar(strtok(arg, ":")), *p;
    unsigned long size = strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned seed = (unsigned)strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned long chunk = strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned long mis = (p = strtok(NULL, ":")) ? strtoul(p, NULL, 0) : 0;
    unsigned long done = 0, w0 = s_wcalls;
    uint32_t *mem = malloc(chunk + 8);
    uint8_t *buf = (uint8_t *)mem + mis;
    fs_file_t *f = append ? fs_open_append(path) : fs_open_write(path);
    long w = 0;
    int c;

    if (!f) {
        printf("%c %s open-fail\n", append ? 'a' : 'w', path);
        free(mem);
        return;
    }
    while (done < size) {
        unsigned long n = size - done < chunk ? size - done : chunk;

        for (unsigned long i = 0; i < n; i++)
            buf[i] = pat(seed, done + i);
        if ((w = fs_write(f, buf, n)) != (long)n)
            break;
        done += n;
    }
    c = fs_close(f);
    printf("%c %s wrote %lu of %lu close %d calls %lu%s\n", append ? 'a' : 'w', path, done, size, c,
           s_wcalls - w0, done < size ? (w == FS_ERR_GONE ? " gone" : " short") : "");
    free(mem);
}

static void do_cat(const char *path)
{
    static uint32_t mem[1100];
    fs_file_t *f = fs_open_read(path);
    uint32_t h = 2166136261u;
    long r, got = 0, step = 1;

    if (!f) {
        printf("cat %s missing\n", path);
        return;
    }
    while ((r = fs_read(f, (uint8_t *)mem + (step & 1), step % 2 ? 777 : 4096)) > 0) {
        for (long k = 0; k < r; k++)
            h = (h ^ ((uint8_t *)mem + (step & 1))[k]) * 16777619u;
        got += r;
        step++;
    }
    printf("cat %s %ld %08x%s\n", path, fs_file_size(f), h, r < 0 || got != fs_file_size(f) ? " ERR" : "");
    fs_close(f);
}

static int ls_cb(const char *name, int is_dir)
{
    printf("  %s%s\n", name, is_dir ? "/" : "");
    return 0;
}

int main(int argc, char **argv)
{
    int i = 2, ro = 0;

    if (argc < 2 || !(s_img = fopen(argv[1], "r+b")))
        return 2;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--cut") && i + 1 < argc)
            s_cut = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--ro"))
            ro = 1;
    }
    fs_target_set_clock(clock_now);
    printf("mount %d %s writable %d serial %08x\n",
           fs_target_mount_rw(1, "SD", img_read, ro ? NULL : img_write, 0), fs_target_type(1),
           fs_target_writable(1), (unsigned)fs_target_serial(1));
    for (; i < argc; i++) {
        char *a = argv[i];

        if (!strncmp(a, "w:", 2)) {
            do_write(a + 2, 0);
        } else if (!strncmp(a, "a:", 2)) {
            do_write(a + 2, 1);
        } else if (!strncmp(a, "rm:", 3)) {
            printf("rm %s %d\n", a + 3, fs_remove(unbar(a + 3)));
        } else if (!strncmp(a, "ex:", 3)) {
            printf("ex %s %d\n", a + 3, fs_exists(unbar(a + 3)));
        } else if (!strncmp(a, "cat:", 4)) {
            do_cat(unbar(a + 4));
        } else if (!strncmp(a, "md:", 3)) {
            printf("md %s %d\n", a + 3, fs_mkdir(unbar(a + 3)));
        } else if (!strncmp(a, "mv:", 3)) {
            char *from = strtok(a + 3, ":"), *to = strtok(NULL, ":");

            unbar(from);
            unbar(to);
            printf("mv %s %s %d\n", from, to, fs_rename(from, to));
        } else if (!strncmp(a, "st:", 3)) {
            fs_stat_t st;
            int r = fs_stat(unbar(a + 3), &st);

            if (r)
                printf("st %s %d\n", a + 3, r);
            else
                printf("st %s size %u wtime %08x attr %02x dir %d\n", a + 3, (unsigned)st.size,
                       (unsigned)st.wtime, st.attr, st.is_dir);
        } else if (!strncmp(a, "ls:", 3)) {
            printf("ls %s %d\n", a + 3, fs_list_dir_each(unbar(a + 3), ls_cb));
        } else if (!strcmp(a, "sync")) {
            printf("sync %d\n", fs_target_sync(1));
        } else if (!strcmp(a, "gone")) {
            s_gone = 1;
            printf("gone\n");
        } else if (!strcmp(a, "unmount")) {
            fs_target_unmount(1);
            printf("unmount\n");
        } else if (!strncmp(a, "hold:", 5)) {
            s_hold = fs_open_read(unbar(a + 5));
            printf("hold %s %d\n", a + 5, s_hold != NULL);
        } else if (!strcmp(a, "rd")) {
            static uint32_t b[256];

            printf("rd %ld\n", fs_read(s_hold, b, sizeof b));
        } else if (!strcmp(a, "close")) {
            printf("close %d\n", fs_close(s_hold));
            s_hold = NULL;
        } else if (!strncmp(a, "busy:", 5)) {
            fs_file_t *f = fs_open_append(unbar(a + 5)), *g = fs_open_write(a + 5);

            printf("busy %s %d %d %d %d\n", a + 5, f != NULL, g == NULL, fs_remove(a + 5),
                   fs_rename(a + 5, "/SD/zz-renamed"));
            fs_close(g);
            fs_close(f);
        } else {
            printf("bad command %s\n", a);
            return 2;
        }
    }
    printf("total writes %lu sectors %lu reads %lu\n", s_wcalls, s_wsecs, s_rcalls);
    fclose(s_img);
    return 0;
}
