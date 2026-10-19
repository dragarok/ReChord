/*
 * lib_plat.c -- the library's platform on the device (lib_int.h): time, storage headroom, the
 * scratch buffer, a card's serial. The host test (services/tests/test_lib.c) has its own.
 */
#include <string.h>

#include "lib_int.h"
#include "scratch.h"
#include "audio/audio.h"
#include "drivers/clock.h"
#include "drivers/emmc.h"
#include "fs_target.h"

uint32_t *const lib_scratch = g_scratch;

uint64_t lib_now_us(void)
{
    return time_us();
}

/* Storage may be used now: audio has its headroom (as the log and the settings, ap_bg.c) and
 * Internal's write-behind queue is empty. Then nothing in the slice waits for a block being
 * programmed (SD NAND: ~3 ms a block; a read waits for the one in flight, a big write first drains
 * the queue): the blocks a slice queues are programmed between slices, by the main loop. The job's
 * big writes (LIB_WBUF) go to the card as one CMD25 of their own (emmc_write_async). With up to 4
 * blocks allowed queued the slices that closed a file took 8-13 ms (m44 underrun). */
int lib_io_ok(void)
{
    return audio_write_ok() && !emmc_busy() && !emmc_wq_pending();
}

/* The slice wrote: end it. Internal's blocks wait in its queue (a big write next would wait for each);
 * the card's writes are synchronous, each command ~3 ms (QEMU's figure; the sd-rw driver's). */
int lib_io_dirty(void)
{
    return emmc_wq_pending() != 0u || fs_target_nwr != lib_slice_wr || lib_wdue;
}

static uint32_t s_serial;
static int s_serial_ok;

void lib_serial_forget(void)
{
    s_serial_ok = 0;
}

/* The card's volume serial (FAT32 0x43, FAT16 0x27, exFAT 0x64) from its boot sector: an MBR's
 * first partition, or a superfloppy. Read once per insertion. */
extern uint32_t fs_target_serial(int idx) __attribute__((weak));   /* sd-rw: the mounted volume's */

uint32_t lib_vol_serial(int vol)
{
    uint32_t b[128];
    const uint8_t *p = (const uint8_t *)b;
    uint32_t base = 0;

    if (vol != 1)
        return 0;
    if (fs_target_serial)
        return fs_target_serial(1);
    if (s_serial_ok)
        return s_serial;
    s_serial = 0;
    if (sd_read(0, 1, b) == 0) {
        int boot = (p[0] == 0xEB || p[0] == 0xE9) && (!memcmp(p + 3, "EXFAT   ", 8) || (p[11] | p[12] << 8) == 512);

        if (!boot && p[510] == 0x55 && p[511] == 0xAA) {
            base = lib_le32(p + 0x1C6);
            if (sd_read(base, 1, b))
                return 0;
        }
        if (!memcmp(p + 3, "EXFAT   ", 8))
            s_serial = lib_le32(p + 0x64);
        else if ((p[11] | p[12] << 8) == 512)
            s_serial = lib_le16(p + 0x16) ? lib_le32(p + 0x27) : lib_le32(p + 0x43);
        s_serial_ok = 1;
    }
    return s_serial;
}
