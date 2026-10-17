/*
 * ap_bg.c -- the storage work that used to stop the main loop, a slice per pass (ap_bg_poll from
 * the main loop and from every screen that runs its own loop). Nothing here waits for the card
 * to program a block (OURS_AUDIT 1.6 "rest 604", DESIGN 4.3):
 *   - the write-behind queue (drivers/emmc.h): one block at a time, and while music plays only with
 *     audio headroom (audio_write_ok: the card is not readable while it programs);
 *   - the log: written when it has grown or aged, only with the queue empty (its own reads then
 *     wait for nothing); its blocks go through the queue;
 *   - the settings: a save asked for while music plays waits for the pause or the stop
 *     (DESIGN 4.7; bug 7 of the audit: a volume key used to rewrite RECHORD.CFG mid-track).
 */
#include <stddef.h>
#include <stdint.h>

#include "drivers/clock.h"
#include "drivers/emmc.h"
#include "audio/audio.h"
#include "player.h"
#include "services/bootcrumb.h"
#include "services/config.h"
#include "services/log.h"

#define LOG_SOON_BYTES   2048u      /* this much unsaved: written at the next chance */
#define LOG_IDLE_MS      10000u     /* anything unsaved, nothing playing: within 10 s */
#define LOG_PLAYING_MS   30000u     /* while playing: within 30 s */

static uint32_t s_log_at;           /* time_ms() of the last log write */
static uint32_t s_stats_at;
static emmc_wq_stats_t s_stats_shown;

void ap_bg_poll(void)
{
    int playing = player_get_state() == PLAYER_PLAYING;
    uint32_t now = time_ms();

    if (!emmc_wq_step(audio_write_ok()))
        log_durable();                   /* (the log written so far is on the disk) */
    config_poll(playing);
    if (!emmc_wq_pending() && log_bytes() &&
        (log_bytes() > LOG_SOON_BYTES || now - s_log_at >= (playing ? LOG_PLAYING_MS : LOG_IDLE_MS))) {
        int rc;

        s_log_at = now;
        rc = log_flush(NULL);
        if (rc != 0 || time_ms() - now >= 20u) {
            log_printf("log: flush %d in %u ms", rc, (unsigned)(time_ms() - now));
            bootcrumb_kv(BCK_LOG_FLUSH, (uint32_t)(time_ms() - now) | (uint32_t)(rc & 0xFF) << 24);
        }
    }
    if (now - s_stats_at >= 30000u) {   /* the queue's figures, when they moved */
        const emmc_wq_stats_t *st = emmc_wq_stats();

        s_stats_at = now;
        if (st->written != s_stats_shown.written || st->errors != s_stats_shown.errors) {
            log_printf("storage: %u blocks queued, %u written, %u merged, peak %u, waits %u (last %u ms, "
                       "from %08x), syncs %u, errors %u (%d), stuck %u", (unsigned)st->queued,
                       (unsigned)st->written, (unsigned)st->merged, (unsigned)st->peak, (unsigned)st->waits,
                       (unsigned)st->wait_ms, (unsigned)st->wait_lr, (unsigned)st->syncs, (unsigned)st->errors,
                       st->last_error, (unsigned)st->stuck);
            s_stats_shown = *st;
        }
    }
}

/* Everything on the card now: before power off, a USB session, a reboot. */
void ap_bg_sync(void)
{
    if (emmc_sync())
        log_printf("storage: a queued write failed (%d)", emmc_wq_stats()->last_error);
}
