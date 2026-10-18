/* Core B: ALAC. Stream from core A: u32 length + the 24-byte ALACSpecificConfig,
 * then u32 length + one frame (packet) each. The adaptive LPC goes to the HIFI block
 * when RECHORD.CFG hifi= allows it (b_hifi_alac.c, docs/HIFI.md); each track ends with
 * a line in core A's log: core B's cycles per second of audio, the block's share. */
#include "b_rt.h"
#include "b_hifi.h"
#include "b_hifi_fn.h"
#include "alac/alac_dec.h"

void b_hifi_alac_counts(uint32_t *chans, uint32_t *hw, uint32_t *orders);

static alac_t s_alac;
static uint8_t *s_pkt;
static int32_t *s_out;
static uint32_t s_cap, s_pos, s_n, s_trim;
static uint32_t s_cyc, s_frames;             /* decode cycles, frames (the track's line) */

static void *alloc(size_t n) { return b_alloc(n); }

int b_alac_open(void)
{
    uint8_t cfg[24];
    uint32_t len;

    if (b_in_read(&len, 4) != 4 || len != 24 || b_in_read(cfg, 24) != 24 || alac_init(&s_alac, cfg, alloc))
        return -1;
    if (s_alac.channels != b_trk.channels)
        return -1;
    /* maxFrameBytes from the config, or the worst case: a verbatim frame */
    s_cap = (uint32_t)cfg[12] << 24 | (uint32_t)cfg[13] << 16 | (uint32_t)cfg[14] << 8 | cfg[15];
    if (!s_cap || s_cap > s_alac.frame_len * s_alac.channels * 4u + 64u)
        s_cap = s_alac.frame_len * s_alac.channels * 4u + 64u;
    s_pkt = b_alloc(s_cap + 8u);
    s_out = b_alloc(s_alac.frame_len * s_alac.channels * 4u);
    s_pos = s_n = 0;
    s_trim = b_trk.trim;
    b_in_need = 4u + s_cap;                  /* one packet */
    if (!s_pkt || !s_out)
        return -1;
    s_cyc = s_frames = 0;
    b_hifi_alac_open(s_alac.frame_len);
    return 0;
}

uint32_t b_alac_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s_alac.channels, t0 = b_cycles();

    while (got < frames) {
        if (s_pos < s_n) {
            uint32_t n = s_n - s_pos;

            if (s_trim) {
                if (n > s_trim)
                    n = s_trim;
                s_trim -= n;
                s_pos += n;
                continue;
            }
            if (n > frames - got)
                n = frames - got;
            memcpy(dst + got * ch, s_out + s_pos * ch, n * ch * 4u);
            s_pos += n;
            got += n;
            continue;
        }
        {
            uint32_t len;

            if (b_in_read(&len, 4) != 4 || len > s_cap || b_in_read(s_pkt, len) != len)
                break;                       /* end of the track (or a broken one) */
            memset(s_pkt + len, 0, 8);       /* the bit reader may look a little past the end */
            if (alac_frame(&s_alac, s_pkt, len, s_out, &s_n)) {
                s_n = s_pos = 0;
                AS->err = 0x60u;
                continue;
            }
            s_pos = 0;
        }
    }
    s_cyc += b_cycles() - t0;
    s_frames += got;
    return got;
}

void b_alac_close(void)
{
    const b_hifi_fn_stats_t *h = &b_hifi_fn_stats;
    uint32_t mhz100, wait100, chans, hw, orders;

    if (!s_frames || !b_trk.rate)
        return;
    b_hifi_alac_counts(&chans, &hw, &orders);
    mhz100 = (uint32_t)((uint64_t)s_cyc * b_trk.rate / s_frames / 10000u);
    wait100 = (uint32_t)((uint64_t)h->wait_cyc * b_trk.rate / s_frames / 10000u);
    b_msg("alac: track %u, %u-bit: core B %u.%02u M cycles/s, LPC %u of %u channels on HIFI (wait %u.%02u M/s), "
          "orders %x", (unsigned)b_trk.seq, (unsigned)b_trk.bits, (unsigned)(mhz100 / 100u), (unsigned)(mhz100 % 100u),
          (unsigned)hw, (unsigned)chans, (unsigned)(wait100 / 100u), (unsigned)(wait100 % 100u), (unsigned)orders);
    b_hifi_alac_close();
}
