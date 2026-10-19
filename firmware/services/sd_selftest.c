/*
 * sd_selftest.c -- Settings > System > Developer > SD card test (the dev overlay: startup/ap_ovl.c).
 *
 * The device's own writes to the card, end to end, timed: what QEMU cannot tell (the real card's
 * write and busy times) is in its log line for the next flash. On the mounted card (volume 1,
 * services/sdcard.c), in /SD/RECHORD (created if missing, left in place: the library keeps its
 * files there):
 *   1 MiB written in 32 KiB pieces to a file with a long, non-ASCII name; 100 KiB appended; all of
 *   it read back and compared; its size and write time read (fs_stat); renamed; 20 small files
 *   with long names written and removed; the big file removed; the card synced (exFAT: VolumeDirty
 *   clear). Pattern byte i: (seed * 31 + i * 7 + (i >> 9)) & 0xFF.
 * One log line: "sdtest: ok: ..." with the times, or "sdtest: FAIL <step> (<error>)"; a toast.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fs.h"
#include "fs_target.h"
#include "log.h"
#include "scratch.h"
#include "sdcard.h"
#include "drivers/clock.h"
#include "ui/frame.h"
#include "ui/nav.h"

#define DIR   "/SD/RECHORD"
#define BIG   DIR "/SD test \xe2\x80\x94 Gr\xc3\xb6\xc3\x9f" "e.bin"   /* "SD test — Größe.bin" */
#define MOVED DIR "/sdtest renamed.bin"
#define CHUNK 32768u
#define BIG_N (1024u * 1024u)
#define APP_N (100u * 1024u)

static void fill(uint8_t *b, uint32_t n, unsigned seed, uint32_t at)
{
    for (uint32_t i = 0; i < n; i++, at++)
        b[i] = (uint8_t)(seed * 31u + at * 7u + (at >> 9));
}

static int put(const char *path, int append, uint32_t n, unsigned seed, uint32_t at)
{
    fs_file_t *f = append ? fs_open_append(path) : fs_open_write(path);
    uint8_t *b = (uint8_t *)g_scratch;
    int r = 0;

    if (!f)
        return FS_ERR_OPEN;
    for (uint32_t done = 0; done < n && !r; ) {
        uint32_t k = n - done < CHUNK ? n - done : CHUNK;

        fill(b, k, seed, at + done);
        if (fs_write(f, b, k) != (long)k)
            r = FS_ERR_IO;
        done += k;
    }
    return fs_close(f) ? FS_ERR_IO : r;
}

void sd_selftest(void)
{
    static const char *step;
    uint8_t *b = (uint8_t *)g_scratch, *c = b + CHUNK / 2u;
    uint32_t t0, tw, ta, tr, tf, tx;
    fs_stat_t st;
    fs_file_t *f;
    char path[96];
    int r = 0;

    if (!sdcard_mounted() || !fs_target_writable(1)) {
        log_printf("sdtest: no writable card (%s)", sdcard_text());
        nav_toast(ICON_WARN, TOAST_WARN, "SD test: no writable card");
        return;
    }
    log_printf("sdtest: start: %s", sdcard_text());
    step = "mkdir";
    r = fs_mkdir(DIR);
    if (r == FS_ERR_EXIST)
        r = 0;
    t0 = time_ms();
    if (!r && !(r = put(BIG, 0, BIG_N, 7, 0)))
        step = "append";
    tw = time_ms() - t0;
    t0 = time_ms();
    if (!r && !(r = put(BIG, 1, APP_N, 7, BIG_N)))
        step = "read back";
    ta = time_ms() - t0;
    t0 = time_ms();
    if (!r && !(f = fs_open_read(BIG)))
        r = FS_ERR_OPEN;
    for (uint32_t at = 0; !r && at < BIG_N + APP_N; ) {   /* half the scratch read, half the pattern */
        uint32_t k = BIG_N + APP_N - at < CHUNK / 2u ? BIG_N + APP_N - at : CHUNK / 2u;

        if (fs_read(f, b, k) != (long)k) {
            r = FS_ERR_IO;
            break;
        }
        fill(c, k, 7, at);
        if (memcmp(b, c, k)) {
            r = FS_ERR_IO;
            step = "compare";
        }
        at += k;
    }
    if (step[0] == 'r' && !r)
        fs_close(f);
    tr = time_ms() - t0;
    if (!r) {
        step = "stat";
        r = fs_stat(BIG, &st);
        if (!r && (st.size != BIG_N + APP_N || st.is_dir || !st.wtime))
            r = FS_ERR_IO;
    }
    if (!r) {
        step = "rename";
        r = fs_rename(BIG, MOVED);
        if (!r && (fs_exists(BIG) || !fs_exists(MOVED)))
            r = FS_ERR_IO;
    }
    t0 = time_ms();
    for (unsigned i = 0; !r && i < 20u; i++) {
        step = "small files";
        snprintf(path, sizeof path, DIR "/playlist %02u with a long name.m3u8", i);
        r = put(path, 0, 200u + i, 30u + i, 0);
    }
    for (unsigned i = 0; !r && i < 20u; i++) {
        step = "remove";
        snprintf(path, sizeof path, DIR "/playlist %02u with a long name.m3u8", i);
        r = fs_remove(path);
    }
    tf = time_ms() - t0;
    t0 = time_ms();
    if (!r) {
        step = "remove big";
        r = fs_remove(MOVED);
    }
    if (!r) {
        step = "sync";
        r = fs_target_sync(1);
    }
    tx = time_ms() - t0;
    if (r) {
        log_printf("sdtest: FAIL %s (%d)", step, r);
        nav_toast(ICON_WARN, TOAST_WARN, "SD test: failed (%s)", step);
        return;
    }
    log_printf("sdtest: ok: w 1024 KB %u ms (%u KB/s), +100 KB %u ms, r %u ms (%u KB/s), 20 files %u ms, rm+sync %u ms",
               (unsigned)tw, (unsigned)(1024000u / (tw ? tw : 1u)), (unsigned)ta, (unsigned)tr,
               (unsigned)(1124000u / (tr ? tr : 1u)), (unsigned)tf, (unsigned)tx);
    nav_toast(ICON_CHECK, TOAST_TEXT, "SD test: ok, %u KB/s", (unsigned)(1024000u / (tw ? tw : 1u)));
}
