/*
 * sdcard.c -- the microSD card: detect, debounce, mount, unmount, events (sdcard.h).
 *
 * Detect: GPIO0.11, pulled up, low = a card in (drivers/emmc_rknanoc.c sd_present; stock
 * SDA_CheckCard 0x03057ad8 and RECHO 0x03062c34 read the same pin). Debounce: RECHO 0.9.2's main
 * loop (0x03051238..0x030512fe): the pin read when 100 ms have passed since the last sample
 * (0x03051242 cmp #99), a reading that differs from the last one restarts the count
 * (0x0305125e), the state changes when the same new reading came twice more (0x030512e0 cmp #1;
 * ble): three samples, 200..300 ms after the card moved. Stock has no time debounce (it reads the
 * pin twice back to back); the brief's "two equal reads 100 ms apart" is the same rule counted
 * from the second sample. At boot the first reading counts at once (stock mounts a card that is in
 * at power-on: Reset_Handler around 0x03062f30).
 *
 * In: sd_init (drivers/emmc_rknanoc.c), volume 1 mounted read-write (fs_target_mount_rw), one log
 * line, SD_EV_INSERTED. Out: if what plays or is queued comes from the card, playback stops and
 * the queue is cleared (its file refs name the card's clusters); volume 1 unmounted (handles still
 * open on it go dead: FS_ERR_GONE until closed); one log line; SD_EV_REMOVED. While the pin says
 * "out" and the debounce has not answered yet, the driver refuses every SD command at once
 * (SD_ERR_NOCARD, no bus traffic) and the audio refill waits instead of filling zeros, so a glitch
 * on the detect contact shorter than the debounce costs nothing. (If a glitch also dropped the
 * card's supply, its reads fail until it is pushed in again: a device question, MERGE_NOTES_sdrw.)
 */
#include <stdio.h>
#include <string.h>

#include "sdcard.h"
#include "fs.h"
#include "fs_target.h"
#include "log.h"
#include "queue.h"
#include "drivers/clock.h"
#include "drivers/emmc.h"
#include "player/player.h"

static uint8_t s_state = 0xFF;       /* debounced: 1 in, 0 out, 0xFF not sampled yet (boot) */
static uint8_t s_last, s_same, s_mounted, s_ev;
static uint32_t s_t, s_serial, s_ev_serial, s_wr0;
static char s_text[28];

/* One slot: an event not read yet is replaced by a newer one (whoever reads them compares serials:
 * an INSERTED says which card is in now, whatever happened before it). */
static void post(int ev)
{
    s_ev = (uint8_t)ev;
    s_ev_serial = s_serial;
}

int sd_event(uint32_t *serial)
{
    int ev = s_ev;

    if (serial)
        *serial = ev ? s_ev_serial : 0u;
    s_ev = SD_EV_NONE;
    return ev;
}

int sdcard_mounted(void) { return s_mounted; }
uint32_t sdcard_serial(void) { return s_mounted ? s_serial : 0u; }
const char *sdcard_text(void) { return s_text[0] ? s_text : "no card"; }

/* (The card initialised and) volume 1 mounted on it; the menu row says how. */
static void mount(int init)
{
    int r = init ? sd_init() : sd_sectors() ? 0 : -1;

    s_mounted = 0;
    if (r) {
        snprintf(s_text, sizeof s_text, r == -1 ? "" : "init %d", r);
        return;
    }
    r = fs_target_mount_rw(1, "SD", sd_read, sd_write, 0);
    s_mounted = r == 0;
    s_serial = fs_target_serial(1);
    snprintf(s_text, sizeof s_text, "%uM %s %db %s", (unsigned)(sd_sectors() >> 11), sd_is_hc() ? "HC" : "SC",
             sd_bus_width(), r ? (r == FS_ERR_IO ? "rd err" : "no fs") : fs_target_type(1));
}

static void gone(void)
{
    int from_sd = queue_count() && queue_ref(0)->vol == 1;
    int stop = from_sd && player_get_state() != PLAYER_STOPPED;

    if (stop)
        player_stop();
    if (from_sd)
        queue_clear();                   /* (refs into the card that went) */
    fs_target_unmount(1);
    sd_init();                           /* (no card: sd_sectors() 0, the USB disk shows no medium) */
    log_printf("sd: card removed%s", stop ? ": playback stopped" : "");
    s_text[0] = 0;
    if (s_mounted)
        post(SD_EV_REMOVED);
    s_mounted = 0;
}

int sdcard_tick(void)
{
    uint32_t now = time_ms();
    int in;

    if (s_state != 0xFF && now - s_t < 100u)
        return 0;
    s_t = now;
    in = sd_present();
    if (s_state != 0xFF) {
        if (in != s_last) {              /* a new reading: the count starts again */
            s_last = (uint8_t)in;
            s_same = 0;
            return 0;
        }
        if (in == s_state)               /* steady (a glitch between two samples: commands refused */
            return 0;                    /* for its length, the audio waited) */
        if (++s_same < 2u)
            return 0;
    }
    s_state = s_last = (uint8_t)in;
    if (!in) {
        if (s_mounted || sd_sectors())
            gone();
        return 1;
    }
    mount(1);
    log_printf("sd: card in: %s, serial %08x", s_text, (unsigned)s_serial);
    if (s_mounted)
        post(SD_EV_INSERTED);
    return 1;
}

void sdcard_usb_begin(void)
{
    if (s_mounted)
        fs_target_unmount(1);            /* (synced first: the PC sees the card whole) */
    s_wr0 = sd_written();
}

void sdcard_usb_end(void)
{
    uint32_t wrote = sd_written() - s_wr0;

    if (s_state != 1u)
        return;
    mount(0);                            /* (the PC may have formatted it: mounted if it can be now) */
    log_printf("sd: USB disk wrote %u sectors; %s", (unsigned)wrote, s_text);
    if (wrote && s_mounted)
        post(SD_EV_CHANGED);
}
