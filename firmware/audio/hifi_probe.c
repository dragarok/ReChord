/*
 * hifi_probe.c -- Developer > HIFI probe (test), core A's side (in the dev overlay: firmware_ap.ld).
 *
 * Core B runs the probe (coreb/b_hifi_probe.c): the HIFI block's FFT and MAC functions on fixed
 * vectors, the FLAC function and core B's own instructions timed. Four passes, each on a fresh core B:
 * the whole probe at PLL / 1 (96 MHz), then the timing jobs at / 2, / 4, / 8 (does the block follow
 * core B's clock?). Every line core B writes ("hfp: ...") goes to ECHOLOG.TXT as it is;
 * tools/hifi_probe_decode.py (project repo) turns the log into answers. docs/HIFI_PLAN.md has the plan.
 *
 * Bounded: a pass that takes over 60 s, a core B that stops (no heartbeat for 1 s) or faults ends the
 * probe, logged with the step it was in (b_hifi_probe.c: AS->probe_step). Core B marks the block busy
 * while it touches it, so a stall turns the FLAC use of the block off until power-off (b_hifi.c). A
 * fresh core B is loaded at the end in any case: the probe leaves the block gated.
 */
#include <stdint.h>
#include <stdio.h>

#include "audio/audio.h"
#include "audio/audio_shared.h"
#include "drivers/clock.h"
#include "drivers/display.h"
#include "player.h"
#include "services/log.h"
#include "services/watchdog.h"

#define PASS_MS 20000u                           /* (the whole probe takes about a second) */

static void row(int r, const char *s)
{
    display_fill_rect(0, 8 + r * 14, DISPLAY_W - 1, 8 + r * 14 + 13, 0x0000);
    display_draw_text(8, 8 + r * 14, s, 0xFFFF, 0x0000);
    display_flush_rows(8 + r * 14, 8 + r * 14 + 13);
}

static void drain_b_msg(void)
{
    while (AS->b_msg_rd != AS->b_msg_wr)
        log_printf("%s", AS->b_msg[AS->b_msg_rd++ % 4u]);
}

/* B's text from `from` to probe_len, one log line each, the log written out as it fills. */
static uint32_t log_text(uint32_t from)
{
    const char *t = (const char *)AS->probe_text;
    uint32_t len = AS->probe_len, i = from, lines = 0;

    if (!t || len > 0x40000u)
        return 0;
    while (i < len) {
        uint32_t j = i;

        while (j < len && t[j] != '\n')
            j++;
        if (j == len)
            break;                           /* (a line still being written) */
        {
            char ln[148];                    /* (core B's lines are shorter; the libc has no %.*s) */
            uint32_t k = 0;

            for (; i + k < j && k < sizeof ln - 1u; k++)
                ln[k] = t[i + k];
            ln[k] = 0;
            if (log_bytes() > 3000u)
                log_flush(NULL);
            log_printf("%s", ln);
        }
        lines++;
        i = j + 1;
        watchdog_feed();
    }
    return lines;
}

void hifi_probe_test(void)
{
    char buf[48];
    const char *why = NULL;
    uint32_t lines = 0;
    unsigned pass;

    player_stop();
    display_fill_rect(0, 0, DISPLAY_W - 1, DISPLAY_H - 1, 0x0000);
    row(0, "HIFI probe (test)");
    row(1, "FFT, MAC, FLAC, core B timing");
    log_printf("hfp: start (4 passes: the probe at B / 1, timing at / 2, / 4, / 8)");
    for (pass = 0; pass < 4 && !why; pass++) {
        uint32_t t0, t_tick, ticks, shown = 0;
        int r = audio_b_fresh(1u << pass);

        if (r != 0) {
            log_printf("hfp: pass %u: no fresh core B at / %u (%d)", pass, 1u << pass, r);
            why = "core B did not start";
            break;
        }
        snprintf(buf, sizeof buf, "pass %u of 4: core B at %u MHz", pass + 1, (unsigned)(AS->b_hz / 1000000u));
        row(2, buf);
        AS->probe_state = AS_PROBE_NONE;
        AS->probe_step = 0;
        AS->probe_len = 0;
        AS->probe_text = 0;
        AS->probe_arg = pass ? 1u : 0u;
        __asm__ volatile ("dmb" ::: "memory");
        AS->cmd = AS_CMD_HIFI_PROBE;
        t0 = t_tick = time_ms();
        ticks = AS->ticks;
        while (AS->cmd == AS_CMD_HIFI_PROBE) {
            uint32_t now = time_ms();

            drain_b_msg();
            if (AS->state == AS_B_FAULT) {
                why = "faulted";
                break;
            }
            if (AS->ticks != ticks) {
                ticks = AS->ticks;
                t_tick = now;
            } else if (now - t_tick > 1000u) {
                why = "stopped (no heartbeat)";
                break;
            }
            if (now - t0 > PASS_MS) {
                why = "took over 20 s";
                break;
            }
            if (now - shown >= 500u) {
                shown = now;
                snprintf(buf, sizeof buf, "step %05x", (unsigned)AS->probe_step);
                row(3, buf);
            }
            watchdog_feed();
            cpu_wait_tick();
        }
        drain_b_msg();
        lines += log_text(0);
        if (why) {
            log_printf("hfp: core B %s in step %05x (pass %u, state %u, pc %08x lr %08x)", why,
                       (unsigned)AS->probe_step, pass, (unsigned)AS->state, (unsigned)AS->fault_pc,
                       (unsigned)AS->fault_lr);
            break;
        }
        if (AS->probe_state != AS_PROBE_DONE) {
            log_printf("hfp: pass %u: core B answered %u (%s)", pass, (unsigned)AS->probe_state,
                       AS->probe_state == AS_PROBE_NOIMAGE ? "no probe in its image" :
                       AS->probe_state == AS_PROBE_NOMEM ? "no memory" : "?");
            why = "did not run it";
            break;
        }
    }
    if (audio_b_fresh(1) != 0)                   /* the player's core B: the block set up again by its FLAC */
        log_printf("hfp: no fresh core B after the probe");
    log_printf("hfp: %s, %u lines", why ? "stopped" : "done", (unsigned)lines);
    log_flush("hfp");
    row(3, why ? why : "done");
    snprintf(buf, sizeof buf, "%u lines in ECHOLOG.TXT", (unsigned)lines);
    row(4, buf);
    for (uint32_t t0 = time_ms(); time_ms() - t0 < 2000u; cpu_wait_tick())
        watchdog_feed();
}
