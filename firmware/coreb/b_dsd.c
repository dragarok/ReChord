/* Core B: DSD (DSF, DFF) to 176.4 kHz PCM (dsd/dsd_dec.c). Stream from core A:
 * "DSD1", u32 layout (0 DSF LSB first, 1 DSF MSB first, 2 DFF byte-interleaved),
 * u32 channels, u32 rate multiple (1/2/4 = DSD64/128/256), u32 DSF block size;
 * then the raw sound data. */
#include "b_rt.h"
#include "dsd/dsd_dec.h"

static dsd_t s_dsd;
static uint8_t *s_in;
static int32_t *s_out;
static uint32_t s_layout, s_block, s_pos, s_n;

#define DFF_BYTES 1024u                          /* per channel per step (DFF) */

static void *alloc(size_t n) { return b_alloc(n); }

int b_dsd_open(void)
{
    uint32_t h[5], ch, per;

    if (b_in_read(h, 20) != 20 || h[0] != 0x31445344u)       /* "DSD1" */
        return -1;
    s_layout = h[1];
    ch = h[2];
    s_block = h[4];
    if (ch != b_trk.channels || dsd_init(&s_dsd, ch, h[3], s_layout == 0, alloc))
        return -1;
    if (s_layout != 2 && (s_block < 8u || s_block > 0x10000u))
        return -1;
    per = s_layout == 2 ? DFF_BYTES : s_block;
    s_in = b_alloc(per * ch);
    s_out = b_alloc((per / s_dsd.step + 1u) * ch * 4u);
    s_pos = s_n = 0;
    b_in_need = per * ch;
    return s_in && s_out ? 0 : -1;
}

uint32_t b_dsd_read(int32_t *dst, uint32_t frames)
{
    uint32_t got = 0, ch = s_dsd.ch;

    while (got < frames) {
        if (s_pos < s_n) {
            uint32_t n = s_n - s_pos;

            if (n > frames - got)
                n = frames - got;
            memcpy(dst + got * ch, s_out + s_pos * ch, n * ch * 4u);
            s_pos += n;
            got += n;
            continue;
        }
        if (s_layout != 2) {                      /* DSF: one block of every channel */
            if (b_in_read(s_in, s_block * ch) != s_block * ch)
                break;
            s_n = dsd_run(&s_dsd, s_in, s_block, 1, s_block, s_out);
        } else {
            uint32_t n = b_in_read(s_in, DFF_BYTES * ch) / ch;

            if (!n)
                break;
            s_n = dsd_run(&s_dsd, s_in, 1, ch, n, s_out);
        }
        s_pos = 0;
    }
    return got;
}
