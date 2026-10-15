/* Core B: raw PCM (WAV: little-endian, AIFF: big-endian; 8 = unsigned, 16, 24, 32 bit)
 * to left-justified 32-bit I2S words. */
#include "b_rt.h"

int b_pcm_open(void)
{
    uint32_t b = b_trk.bits;

    return (b == 8 || b == 16 || b == 24 || b == 32) && b_trk.channels >= 1 && b_trk.channels <= 2 ? 0 : -1;
}

uint32_t b_pcm_read(int32_t *dst, uint32_t frames)
{
    uint32_t bps = b_trk.bits / 8u, ch = b_trk.channels, fsz = bps * ch, n = b_in_avail() / fsz;
    int be = b_trk.codec == AS_CODEC_AIFF;
    uint8_t buf[192];

    if (n > frames)
        n = frames;
    for (uint32_t done = 0; done < n; ) {
        uint32_t k = n - done > sizeof buf / fsz ? sizeof buf / fsz : n - done;

        b_in_copy(buf, k * fsz);
        for (uint32_t i = 0; i < k * ch; i++) {
            const uint8_t *p = buf + i * bps;
            uint32_t v = 0;

            for (uint32_t j = 0; j < bps; j++)           /* most significant byte first */
                v = v << 8 | p[be ? j : bps - 1u - j];
            v <<= 32u - 8u * bps;
            if (bps == 1)
                v ^= 0x80000000u;                       /* 8-bit WAV is unsigned */
            *dst++ = (int32_t)v;
        }
        done += k;
    }
    return n;
}
