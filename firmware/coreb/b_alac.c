/* Core B: ALAC. Stream from core A: u32 length + the 24-byte ALACSpecificConfig,
 * then u32 length + one frame (packet) each. */
#include "b_rt.h"
#include "alac/alac_dec.h"

static alac_t s_alac;
static uint8_t *s_pkt;
static int32_t *s_out;
static uint32_t s_cap, s_pos, s_n, s_trim;

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
    return s_pkt && s_out ? 0 : -1;
}

uint32_t b_alac_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s_alac.channels;

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
    return got;
}
