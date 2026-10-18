/*
 * dsd_dec.c -- DSD to PCM (dsd_dec.h). Table k holds, for every value of the k-th
 * newest byte, the sum of +-h over its 8 taps (bit = 1: +h); an output sample is
 * the sum of nt lookups. The filter is symmetric, so tap order within the window
 * only has to be consistent: tap 8k + j is bit j of byte k counted from the newest
 * sample (bit 0 of an MSB-first byte is its newest).
 *
 * ReChord 2026-10-18 (docs/HIFI_PLAN.md): the same sums in fewer instructions. An LSB-first stream's
 * tables are built for its own bit order (tab[k][v] = the MSB-first table at rev8(v)), so its bytes
 * go into the history as they come; and a window starts at a multiple of the step (pos and phase
 * start at 0 and move together), so it is read 4 bytes a load (DSD128, DSD256) or 2 (DSD64).
 * Integer sums of the same table entries: every output word is what it was.
 */
#include <string.h>

#include "dsd_dec.h"
#include "dsd_fir.h"

static int32_t tap(const int32_t *half, uint32_t n, uint32_t i)
{
    return half[i < n / 2 ? i : n - 1 - i];
}

static uint8_t rev8(uint8_t b)
{
    b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
    b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
    return (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
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
            uint32_t b = lsb_first ? rev8((uint8_t)v) : v;    /* (the byte as an MSB-first one) */
            int32_t s = 0;

            for (uint32_t j = 0; j < 8; j++)
                s += (b >> j & 1u) ? tap(h, n, k * 8 + j) : -tap(h, n, k * 8 + j);
            d->tab[k][v] = (int16_t)((s + (1 << d->shift >> 1)) >> d->shift);
        }
    /* DSD silence pattern: 0x69 averages to 0 (as an MSB-first byte; an LSB-first stream's history
     * holds its bytes as they come, so its 0x69 is 0x96) */
    memset(d->hist, lsb_first ? 0x96 : 0x69, ch * d->nt * 2u);
    return 0;
}


/* The window's sum: nt table lookups, its bytes read 4 (or 2) at a time (little-endian: byte 0 first).
 * On core B (Cortex-M3), one table pointer per byte of the word, so a lookup is 3 instructions (extract,
 * load, add); GCC folds the pointers back into one plus an offset add (4, and 5.25 a lookup in all).
 * The host build (tests) takes the C, which is the same sum. */
static int32_t sum4(const uint8_t *hb, const int16_t *t, uint32_t nt)
{
#if defined(__arm__) && defined(__thumb2__)
    int32_t s = 0;
    uint32_t w, x, n = nt / 4u;
    const int16_t *t1 = t + 256, *t2 = t + 512, *t3 = t + 768;

    __asm__ volatile (
        "1: ldr %[w], [%[hb]], #4\n"
        "   uxtb %[x], %[w]\n"
        "   ldrsh %[x], [%[t0], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   ubfx %[x], %[w], #8, #8\n"
        "   ldrsh %[x], [%[t1], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   ubfx %[x], %[w], #16, #8\n"
        "   ldrsh %[x], [%[t2], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   lsr %[x], %[w], #24\n"
        "   ldrsh %[x], [%[t3], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   add %[t0], %[t0], #2048\n"
        "   add %[t1], %[t1], #2048\n"
        "   add %[t2], %[t2], #2048\n"
        "   add %[t3], %[t3], #2048\n"
        "   subs %[n], %[n], #1\n"
        "   bne 1b\n"
        : [s] "+r" (s), [w] "=&r" (w), [x] "=&r" (x), [hb] "+r" (hb), [n] "+r" (n),
          [t0] "+r" (t), [t1] "+r" (t1), [t2] "+r" (t2), [t3] "+r" (t3)
        :
        : "cc", "memory");
    return s;
#else
    const uint32_t *w = (const uint32_t *)hb;
    int32_t s = 0;

    for (uint32_t k = 0; k < nt; k += 4, t += 4 * 256) {
        uint32_t v = *w++;

        s += t[v & 255u];
        s += t[256 + (v >> 8 & 255u)];
        s += t[512 + (v >> 16 & 255u)];
        s += t[768 + (v >> 24)];
    }
    return s;
#endif
}

static int32_t sum2(const uint8_t *hb, const int16_t *t, uint32_t nt)
{
#if defined(__arm__) && defined(__thumb2__)
    int32_t s = 0;
    uint32_t w, x, n = nt / 4u;                              /* 4 lookups a turn: two halfwords */
    const int16_t *t1 = t + 256, *t2 = t + 512, *t3 = t + 768;

    __asm__ volatile (
        "1: ldrh %[w], [%[hb]], #2\n"
        "   uxtb %[x], %[w]\n"
        "   ldrsh %[x], [%[t0], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   lsr %[x], %[w], #8\n"
        "   ldrsh %[x], [%[t1], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   ldrh %[w], [%[hb]], #2\n"
        "   uxtb %[x], %[w]\n"
        "   ldrsh %[x], [%[t2], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   lsr %[x], %[w], #8\n"
        "   ldrsh %[x], [%[t3], %[x], lsl #1]\n"
        "   add %[s], %[s], %[x]\n"
        "   add %[t0], %[t0], #2048\n"
        "   add %[t1], %[t1], #2048\n"
        "   add %[t2], %[t2], #2048\n"
        "   add %[t3], %[t3], #2048\n"
        "   subs %[n], %[n], #1\n"
        "   bne 1b\n"
        : [s] "+r" (s), [w] "=&r" (w), [x] "=&r" (x), [hb] "+r" (hb), [n] "+r" (n),
          [t0] "+r" (t), [t1] "+r" (t1), [t2] "+r" (t2), [t3] "+r" (t3)
        :
        : "cc", "memory");
    return s;
#else
    const uint16_t *w = (const uint16_t *)hb;
    int32_t s = 0;

    for (uint32_t k = 0; k < nt; k += 2, t += 2 * 256) {
        uint32_t v = *w++;

        s += t[v & 255u];
        s += t[256 + (v >> 8)];
    }
    return s;
#endif
}

static int32_t sum1(const uint8_t *hb, const int16_t *t, uint32_t nt)
{
    int32_t s = 0;

    for (uint32_t k = 0; k < nt; k++, t += 256)
        s += t[hb[k]];
    return s;
}

uint32_t dsd_run(dsd_t *d, const uint8_t *in, uint32_t cstride, uint32_t istride, uint32_t n, int32_t *out)
{
    const uint32_t nt = d->nt, nch = d->ch, step = d->step, sh = 1u + d->shift;   /* sum is Q(30 - shift); word Q31 */
    const int32_t lim = 0x7FFFFFFF >> sh;
    const int16_t *tab = &d->tab[0][0];
    uint8_t *hist = d->hist;
    uint32_t frames = 0, pos = d->pos, phase = d->phase;

    for (uint32_t i = 0; i < n; i++) {
        pos = pos ? pos - 1u : nt - 1u;                      /* newest byte goes to pos (and pos + nt) */
        for (uint32_t c = 0; c < nch; c++) {
            uint8_t b = in[c * cstride + i * istride], *h = hist + c * 2u * nt;

            h[pos] = h[pos + nt] = b;
        }
        if (++phase < step)
            continue;
        phase = 0;
        for (uint32_t c = 0; c < nch; c++) {
            const uint8_t *hb = hist + c * 2u * nt + pos;
            int32_t s = ((uintptr_t)hb & 3u) == 0 ? sum4(hb, tab, nt) :
                        ((uintptr_t)hb & 1u) == 0 ? sum2(hb, tab, nt) : sum1(hb, tab, nt);

            s = s > lim ? lim : s < -lim ? -lim : s;
            out[frames * nch + c] = (int32_t)((uint32_t)s << sh);
        }
        frames++;
    }
    d->pos = pos;
    d->phase = phase;
    return frames;
}
