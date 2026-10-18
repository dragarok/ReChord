/*
 * b_hifi_fn.c -- what the HIFI block's APE and ALAC functions share (b_hifi_fn.h): the per-track
 * readiness check, the bounded wait, the track's counters and its log line. The block itself (power,
 * DMA2 transfers) is b_hifi.c's; each function's own sequence is in b_hifi_ape.c / b_hifi_alac.c.
 *
 * The wait: stock's APE and ALAC decoders sleep (WFI) until DMA2 ch0's transfer interrupt, ALAC then
 * polls INTSR bit 15 (seg79 0x01004698), both without a limit (seg80 HIFI_DMA_TO_ACC 0x01004F28, seg79
 * the same). ReChord polls DMA2's RawTfr (the interrupt latched but masked) with a limit: the wait
 * counts as busy time for the load governor (the block runs on core B's clock [inferred]).
 */
#include "b_rt.h"
#include "b_hifi.h"
#include "b_hifi_fn.h"

#define REG(a)         (*(volatile uint32_t *)(a))
#define DMA2_RAW_TFR   0x010702C0u               /* DW_ahb_dmac RawTfr */
#define DMA2_CLEAR_TFR 0x01070338u
#define DMA2_CH_RX     0u                        /* stock HIFI_DMA_FROM_CH */

b_hifi_fn_stats_t b_hifi_fn_stats;
static uint32_t s_gated;                         /* hifi=off: gated once by this core B */

int b_hifi_fn_ready(void)
{
    if (AS->hifi_busy) {                         /* the last core B was reloaded mid-access */
        b_msg("hifi: core B was reloaded while it used the block (a stalled bus?): the block off until "
              "power-off");
        b_hifi_down(AS_HIFI_DEAD);
        return 0;
    }
    return AS->hifi_mode != AS_HIFI_OFF && AS->hifi_state != AS_HIFI_DEAD;
}

void b_hifi_fn_off(void)
{
    if (!s_gated && AS->hifi_state != AS_HIFI_DEAD) {
        s_gated = 1;
        b_hifi_gate();
    }
}

void b_hifi_fn_busy(void)
{
    AS->hifi_busy = 1;
}

int b_hifi_fn_wait(uint32_t t0, uint32_t lim, int done, const char *what, uint32_t n)
{
    b_hifi_fn_stats_t *st = &b_hifi_fn_stats;
    uint32_t t;

    while (!(REG(DMA2_RAW_TFR) & (1u << DMA2_CH_RX)) || (done && !(HIFI_REG(HIFI_R_INTSR) & HIFI_INT_DONE))) {
        st->polls++;
        if (b_cycles() - t0 > lim) {
            uint32_t rx = REG(DMA2_RAW_TFR) & (1u << DMA2_CH_RX);    /* (before its clock goes off) */

            t = b_cycles() - t0;
            b_hifi_down(AS_HIFI_DEAD);
            b_msg("hifi: %s job %u (%u samples) not done after %u us (RX %s): the block off until power-off",
                  what, (unsigned)st->jobs, (unsigned)n, (unsigned)b_us(t), rx ? "done" : "not done");
            return -1;
        }
    }
    t = b_cycles() - t0;
    if (done)
        HIFI_REG(HIFI_R_INTCR) &= ~HIFI_INT_DONE;   /* stock's done-flag clear (seg79 0x010046B0) */
    REG(DMA2_CLEAR_TFR) = 3u;
    AS->hifi_busy = 0;
    st->jobs++;
    st->samples += n;
    st->wait_cyc += t;
    if (t > st->wait_max_cyc)
        st->wait_max_cyc = t;
    return 0;
}

void b_hifi_fn_track_line(const char *fn, const char *state)
{
    static const char *const k_mode[] = { "auto", "off", "verify" };
    const b_hifi_fn_stats_t *h = &b_hifi_fn_stats;

    b_msg("hifi: track %u: %s hifi=%s, %s, %u jobs, %u checked, %u mismatched, %u redone, %u silenced, "
          "longest %u us, %u polls", (unsigned)b_trk.seq, fn, k_mode[AS->hifi_mode % 3u], state,
          (unsigned)h->jobs, (unsigned)h->checked, (unsigned)h->mismatch, (unsigned)h->redone, (unsigned)h->lost,
          (unsigned)b_us(h->wait_max_cyc), (unsigned)h->polls);
}
