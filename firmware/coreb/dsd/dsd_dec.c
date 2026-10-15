/*
 * dsd_dec.c -- DSD to PCM (dsd_dec.h). Table k holds, for every value of the k-th
 * newest byte, the sum of +-h over its 8 taps (bit = 1: +h); an output sample is
 * the sum of nt lookups. The filter is symmetric, so tap order within the window
 * only has to be consistent: tap 8k + j is bit j of byte k counted from the newest
 * sample (bit 0 of an MSB-first byte is its newest).
 */
#include <string.h>

#include "dsd_dec.h"
#include "dsd_fir.h"

static int32_t tap(const int32_t *half, uint32_t n, uint32_t i)
{
    return half[i < n / 2 ? i : n - 1 - i];
}

int dsd_init(dsd_t *d, uint32_t ch, uint32_t rate_mult, int lsb_first, void *(*alloc)(size_t))
{
    const int32_t *h = rate_mult == 1 ? k_dsd_fir256 : rate_mult == 2 ? k_dsd_fir512 :
                       rate_mult == 4 ? k_dsd_fir1024 : 0;
    uint32_t n = 256u * rate_mult, maxsum = 0;

    memset(d, 0, sizeof *d);
    if (!h || ch < 1 || ch > 2)
        return -1;
    d->ch = ch;
    d->nt = n / 8u;
    d->step = rate_mult == 4u ? 16u : 2u * rate_mult;   /* 16 / 32 bits -> 176.4k; DSD256: 128 -> 88.2k */
    d->lsb_first = (uint8_t)lsb_first;
    for (uint32_t k = 0; k < d->nt; k++) {        /* scale: the largest entry fits int16 */
        uint32_t s = 0;

        for (uint32_t j = 0; j < 8; j++) {
            int32_t v = tap(h, n, k * 8 + j);

            s += (uint32_t)(v < 0 ? -v : v);
        }
        if (s > maxsum)
            maxsum = s;
    }
    while ((maxsum >> d->shift) > 32767u)
        d->shift++;
    d->tab = alloc(d->nt * sizeof *d->tab);
    d->hist = alloc(ch * d->nt * 2u);            /* mirrored: a window is always contiguous */
    if (!d->tab || !d->hist)
        return -1;
    for (uint32_t k = 0; k < d->nt; k++)
        for (uint32_t v = 0; v < 256; v++) {
            int32_t s = 0;

            for (uint32_t j = 0; j < 8; j++)
                s += (v >> j & 1u) ? tap(h, n, k * 8 + j) : -tap(h, n, k * 8 + j);
            d->tab[k][v] = (int16_t)((s + (1 << d->shift >> 1)) >> d->shift);
        }
    memset(d->hist, 0x69, ch * d->nt * 2u);      /* DSD silence pattern: 0x69 averages to 0 */
    return 0;
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
    b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
    return (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
}

uint32_t dsd_run(dsd_t *d, const uint8_t *in, uint32_t cstride, uint32_t istride, uint32_t n, int32_t *out)
{
    uint32_t frames = 0, nt = d->nt, sh = 1u + d->shift;   /* sum is Q(30 - shift); word Q31 */
    int32_t lim = 0x7FFFFFFF >> sh;

    for (uint32_t i = 0; i < n; i++) {
        d->pos = d->pos ? d->pos - 1u : nt - 1u;            /* newest byte goes to pos (and pos + nt) */
        for (uint32_t c = 0; c < d->ch; c++) {
            uint8_t b = in[c * cstride + i * istride], *h = d->hist + c * 2u * nt;

            if (d->lsb_first)
                b = rev8(b);
            h[d->pos] = h[d->pos + nt] = b;
        }
        if (++d->phase < d->step)
            continue;
        d->phase = 0;
        for (uint32_t c = 0; c < d->ch; c++) {
            const uint8_t *hb = d->hist + c * 2u * nt + d->pos;
            const int16_t *t = &d->tab[0][0];
            int32_t s = 0;

            for (uint32_t k = 0; k < nt; k += 4, hb += 4, t += 4 * 256) {   /* nt is a multiple of 32 */
                s += t[hb[0]];
                s += t[256 + hb[1]];
                s += t[512 + hb[2]];
                s += t[768 + hb[3]];
            }
            s = s > lim ? lim : s < -lim ? -lim : s;
            out[frames * d->ch + c] = (int32_t)((uint32_t)s << sh);
        }
        frames++;
    }
    return frames;
}
