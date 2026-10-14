/* Host harness for the fs_target.c write path: mounts a disk image file as
 * the writable volume "Internal" (and the same image read-only as "SD"),
 * then runs the commands given as arguments, one result line each. Driven
 * by tools/test_fs_write.py, which checks the image with its own FAT32
 * reader afterwards.
 *
 *   test_fs_write IMG [--cut N] CMD...
 *     w:PATH:SIZE:SEED:CHUNK[:MIS]  create/truncate, SIZE pattern bytes in
 *                                   CHUNK-byte fs_write calls from a buffer
 *                                   offset by MIS bytes (misaligned source)
 *     a:PATH:SIZE:SEED:CHUNK[:MIS]  same, append mode
 *     rm:PATH   ex:PATH   cat:PATH   busy:PATH (second write open must fail)
 *   --cut N: every block write after the first N fails (a power cut: the
 *   image keeps exactly what reached it).
 * Pattern byte i of seed s: (s * 31 + i * 7 + (i >> 9)) & 0xFF.
 * FSW_TRACE=1 in the environment logs every block write to stderr. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "services/fs.h"
#include "services/fs_target.h"

static FILE *s_img;
static long s_cut = -1;
static unsigned long s_wcalls, s_wsecs, s_rcalls;

static void need_aligned(const void *buf)
{
    if ((uintptr_t)buf & 3u) {               /* the eMMC FIFO moves words */
        fprintf(stderr, "unaligned block buffer %p\n", buf);
        exit(3);
    }
}

static int img_read(uint32_t lba, uint32_t count, void *buf)
{
    need_aligned(buf);
    s_rcalls++;
    return fseek(s_img, (long)lba * 512L, SEEK_SET) ||
           fread(buf, 512, count, s_img) != count;
}

static int img_write(uint32_t lba, uint32_t count, const void *buf)
{
    need_aligned(buf);
    if (s_cut >= 0 && (long)s_wcalls >= s_cut)
        return -1;
    s_wcalls++;
    s_wsecs += count;
    if (getenv("FSW_TRACE"))
        fprintf(stderr, "W %u+%u\n", (unsigned)lba, (unsigned)count);
    return fseek(s_img, (long)lba * 512L, SEEK_SET) ||
           fwrite(buf, 512, count, s_img) != count || fflush(s_img);
}

static uint8_t pat(unsigned seed, unsigned long i)
{
    return (uint8_t)(seed * 31u + i * 7u + (i >> 9));
}

static void do_write(char *arg, int append)
{
    char *path = strtok(arg, ":"), *p;
    unsigned long size = strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned seed = (unsigned)strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned long chunk = strtoul(strtok(NULL, ":"), NULL, 0);
    unsigned long mis = (p = strtok(NULL, ":")) ? strtoul(p, NULL, 0) : 0;
    unsigned long done = 0, w0 = s_wcalls, s0 = s_wsecs;
    uint32_t *mem = malloc(chunk + 8);
    uint8_t *buf = (uint8_t *)mem + mis;
    fs_file_t *f = append ? fs_open_append(path) : fs_open_write(path);
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
        if (fs_write(f, buf, n) != (long)n)
            break;
        done += n;
    }
    c = fs_close(f);
    printf("%c %s wrote %lu of %lu close %d calls %lu secs %lu\n", append ? 'a' : 'w', path,
           done, size, c, s_wcalls - w0, s_wsecs - s0);
    free(mem);
}

static void do_cat(const char *path)
{
    static uint32_t mem[1100];               /* odd chunks + aligned bulk */
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

int main(int argc, char **argv)
{
    int i = 2;

    if (argc < 2 || !(s_img = fopen(argv[1], "r+b")))
        return 2;
    if (argc > 3 && !strcmp(argv[2], "--cut")) {
        s_cut = strtol(argv[3], NULL, 0);
        i = 4;
    }
    printf("mount %d %d\n", fs_target_mount_rw(0, "Internal", img_read, img_write, 0),
           fs_target_mount(1, "SD", img_read, 0));
    printf("writable %d %d\n", fs_target_writable(0), fs_target_writable(1));
    for (; i < argc; i++) {
        char *a = argv[i];

        if (!strncmp(a, "w:", 2)) {
            do_write(a + 2, 0);
        } else if (!strncmp(a, "a:", 2)) {
            do_write(a + 2, 1);
        } else if (!strncmp(a, "rm:", 3)) {
            printf("rm %s %d\n", a + 3, fs_remove(a + 3));
        } else if (!strncmp(a, "ex:", 3)) {
            printf("ex %s %d\n", a + 3, fs_exists(a + 3));
        } else if (!strncmp(a, "cat:", 4)) {
            do_cat(a + 4);
        } else if (!strncmp(a, "busy:", 5)) {
            fs_file_t *f = fs_open_append(a + 5), *g = fs_open_write(a + 5);

            printf("busy %s %d %d %d\n", a + 5, f != NULL, g == NULL, fs_remove(a + 5));
            fs_close(g);
            fs_close(f);
        } else {
            printf("bad command %s\n", a);
            return 2;
        }
    }
    printf("total writes %lu reads %lu\n", s_wcalls, s_rcalls);
    fclose(s_img);
    return 0;
}
