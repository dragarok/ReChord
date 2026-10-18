/* Core B: raw PCM (WAV, AIFC 'sowt': little-endian; AIFF: big-endian; 8 = unsigned
 * in WAV, signed in AIFF; 16, 24, 32 bit; 32-bit IEEE float) to left-justified
 * 32-bit I2S words. */
#include "b_rt.h"

/* IEEE single in [-1, 1) -> Q31 word, by its bits (core B has no FPU); clipped. */
static int32_t float_word(uint32_t f)
{
    uint32_t e = f >> 23 & 0xFFu, m = (f & 0x7FFFFFu) | 0x800000u;
    int32_t v;

    if (e >= 127u)                                  /* |x| >= 1 (and inf, NaN) */
        v = 0x7FFFFFFF;
    else if (e < 127u - 31u)
        v = 0;
    else
        v = (int32_t)(((uint64_t)m << 7) >> (126u - e));  /* m * 2^(e-150) * 2^31 */
    if ((f >> 31) && v)
        v = v == 0x7FFFFFFF ? (int32_t)0x80000000u : -v;
    return v;
}

int b_pcm_open(void)
{
    uint32_t b = b_trk.bits;

    b_in_need = 0x400u;
    if (b_trk.codec == AS_CODEC_WAVF && b != 32)
        return -1;
    return (b == 8 || b == 16 || b == 24 || b == 32) && b_trk.channels >= 1 && b_trk.channels <= 2 ? 0 : -1;
}

/* 16- and 24-bit little-endian (WAV, AIFC 'sowt'), the common case: straight from the input ring, no
 * copy, a sample in a few instructions (the general loop below: ~45 a sample through a 192-byte
 * buffer). The words are the general loop's, bit for bit. Returns the frames done (a frame across
 * the ring's wrap is left to the general loop). */
static uint32_t pcm_le_fast(int32_t *dst, uint32_t n, uint32_t bps, uint32_t ch)
{
    uint32_t fsz = bps * ch, done = 0;

    while (done < n) {
        uint32_t off = AS->in_rd % AS_IN_SIZE, k = (AS_IN_SIZE - off) / fsz, m;
        const uint8_t *p = (const uint8_t *)AS_IN_RING + off;

        if (k > n - done)
            k = n - done;
        if (!k)
            break;
        m = k * ch;
        if (bps == 2) {
            for (uint32_t i = 0; i < m; i++, p += 2)
                *dst++ = (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24);
        } else {
            for (uint32_t i = 0; i < m; i++, p += 3)
                *dst++ = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
        }
        AS->in_rd += k * fsz;
        done += k;
    }
    return done;
}

uint32_t b_pcm_read(int32_t *dst, uint32_t frames)
{
    uint32_t bps = b_trk.bits / 8u, ch = b_trk.channels, fsz = bps * ch, n = b_in_avail() / fsz;
    int be = b_trk.codec == AS_CODEC_AIFF, fl = b_trk.codec == AS_CODEC_WAVF;
    uint8_t buf[192];
    uint32_t done = 0;

    if (n > frames)
        n = frames;
    if (!be && !fl && (bps == 2 || bps == 3)) {
        done = pcm_le_fast(dst, n, bps, ch);
        dst += done * ch;
    }
    for (; done < n; ) {
        uint32_t k = n - done > sizeof buf / fsz ? sizeof buf / fsz : n - done;

        b_in_copy(buf, k * fsz);
        for (uint32_t i = 0; i < k * ch; i++) {
            const uint8_t *p = buf + i * bps;
            uint32_t v = 0;

            for (uint32_t j = 0; j < bps; j++)           /* most significant byte first */
                v = v << 8 | p[be ? j : bps - 1u - j];
            if (fl) {
                *dst++ = float_word(v);
                continue;
            }
            v <<= 32u - 8u * bps;
            if (bps == 1 && !be)
                v ^= 0x80000000u;                       /* 8-bit WAV is unsigned (AIFF signed) */
            *dst++ = (int32_t)v;
        }
        done += k;
    }
    return n;
}
