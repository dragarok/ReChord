/*
 * eq_design.c -- the EQ's curves and their design (eq.h; the engine and the filters are eq.c). Built
 * -Os (it runs once a curve or a rate); the test model compiles this file unchanged.
 */
#include <string.h>

#include "eq.h"

#define PRE_ONE   (1 << 30)

/* ---- the curves ------------------------------------------------------------------------------- */

/* Stock's presets (MINIV390, V1.5.0..V3.9.0 identical): its 8 x 31 int16 gain rows at 0x030464C8
 * (IMG +0xC1930, 12 = 0 dB) on ISO 1/3-octave centres; RKEQAdjust 0x03089B80 picks the row (TBB
 * 0x03089C2C: Retro row 0, Bass row 5, Heavy 2, Pop 3, Jazz 4, Unique 1). Only these bands are not
 * 0 dB. Stock's filters (its 44.1 kHz coefficient set, IMG +0xC2450) are RBJ peaking sections with
 * Q = 1/sqrt 2 at the nominal centres within 0.10 dB (the 315 Hz band 0.37 dB: its int16 Q13
 * coefficients), fitted over 20 Hz..20 kHz (MERGE_NOTES_eq.md); here they are designed for each rate
 * (stock uses its 44.1 kHz set at every rate of 32 kHz and up, so its bands move with the rate). */
typedef struct { uint16_t hz; int8_t db; } eq_pt_t;
static const eq_pt_t k_preset[EQ_CUSTOM - 1][5] = {
    { { 100, 1 }, { 315, 1 }, { 1000, 1 }, { 10000, -6 } },                    /* Retro */
    { { 63, 5 }, { 125, 3 } },                                                   /* Bass */
    { { 315, 3 }, { 1000, 3 }, { 3150, -3 } },                                   /* Heavy */
    { { 100, 4 }, { 3150, -3 }, { 10000, 3 } },                                  /* Pop */
    { { 100, 4 }, { 315, -6 }, { 1000, 4 }, { 3150, 1 }, { 10000, 6 } },         /* Jazz */
    { { 100, 3 }, { 1000, -3 }, { 3150, 3 } },                                   /* Unique */
};
/* 1 / 2Q in Q30: presets Q = 1/sqrt 2 (the fit above); Custom: octave bands 1000 x 2^(k-5) Hz
 * (31.25 .. 16000; RECHO 0.10.0 has the same ten) one octave wide, Q = 1 / (2 sinh(ln 2 / 2)) = sqrt 2
 * (RBJ's bandwidth-to-Q relation, BW = 1) */
#define KQ_PRESET  759250125            /* sqrt 2 / 2 */
#define KQ_CUSTOM  379625062            /* 1 / (2 sqrt 2) */
#define ONE        (1 << 30)
#define PI_Q30     3373259426u          /* pi 2^30 */

/* A = 10^(dB/40), dB = -12..12, in Q30 (rounded; the test model checks every entry) */
static const uint32_t k_A[2 * EQ_GAIN_MAX + 1] = {
    538145694, 570032831, 603809400, 639587356, 677485290, 717628817, 760150998, 805192776, 852903448,
    903441154, 956973408, 1013677647, 1073741824, 1137365027, 1204758142, 1276144550, 1351760868,
    1431857735, 1516700640, 1606570803, 1701766107, 1802602089, 1909412977, 2022552809, 2142396597,
};

static int32_t mulq(int64_t a, int64_t b, int sh)            /* (a b) >> sh, rounded */
{
    return (int32_t)((a * b + ((int64_t)1 << (sh - 1))) >> sh);
}

static int32_t divq(int64_t num, int32_t den)                /* num / den, rounded (den > 0) */
{
    return (int32_t)((num + (num >= 0 ? den / 2 : -(den / 2))) / den);
}

/* sin and cos of h in [0, pi/2) (Q30) by their series: 7 terms each, the next under 2^-32 */
static void sincos_h(int32_t h, int32_t *s, int32_t *c)
{
    int32_t h2 = mulq(h, h, 30), ts = h, tc = ONE, sv = h, cv = ONE;

    for (int k = 1; k <= 7; k++) {
        ts = -mulq(ts, h2, 30) / (2 * k * (2 * k + 1));
        tc = -mulq(tc, h2, 30) / ((2 * k - 1) * 2 * k);
        sv += ts;
        cv += tc;
    }
    *s = sv;
    *c = cv;
}

/* half the angle of hz4 / 4 Hz at rate: pi f / rate = pi hz4 / (4 rate), Q30, rounded (hz4 < 2 rate) */
static int32_t half_angle(uint32_t hz4, uint32_t rate)
{
    return (int32_t)(((uint64_t)PI_Q30 * hz4 + 2u * rate) / (4u * (uint64_t)rate));
}

/* the RBJ peaking section, normalized by a0, as eq.h's three coefficients (Q3.29):
 * w0 = 2h; cos w0 = 1 - 2 sin^2 h, sin w0 = 2 sin h cos h; alpha = sin w0 / 2Q */
static void section(eq_coef_t *c, uint32_t hz4, int32_t kq, int db, uint32_t rate)
{
    int32_t s, ch, sw, al, aA, aiA, a0, A = (int32_t)k_A[db + EQ_GAIN_MAX], iA = (int32_t)k_A[EQ_GAIN_MAX - db];

    sincos_h(half_angle(hz4, rate), &s, &ch);
    sw = mulq(s, ch, 29);                                    /* 2 s c */
    al = mulq(sw, kq, 30);
    aA = mulq(al, A, 30);
    aiA = mulq(al, iA, 30);
    a0 = ONE + aiA;
    c->d = divq((int64_t)(aA - aiA) << 29, a0);               /* b0 - 1 = alpha (A - 1/A) / a0 */
    c->a1 = divq(((int64_t)-2 * ONE + 4 * (int64_t)mulq(s, s, 30)) << 29, a0);   /* -2 cos w0 / a0 */
    c->a2 = divq((int64_t)(ONE - aiA) << 29, a0);
}

/* |H(e^jw)|^2 of the cascade as rounded, Q24. A section is H = 1 + d (1 - z^-2) / A(z); times e^jw
 * on both sides of the fraction: A e^jw = P + jQ, P = (1 + a2) cos w + a1, Q = (1 - a2) sin w, and
 * (1 - z^-2) e^jw = 2j sin w, so |H|^2 = (P^2 + (Q + 2 d sin w)^2) / (P^2 + Q^2) (no difference of
 * large terms near the centre, where P -> 0). P, Q in Q28 (squares below 2^60). */
static int64_t mag2(const eq_bank_t *b, uint32_t hz4)
{
    int32_t s, c, sw, cw;
    int64_t m = (int64_t)1 << 24;

    sincos_h(half_angle(hz4, b->rate), &s, &c);
    sw = mulq(s, c, 29);
    cw = ONE - 2 * mulq(s, s, 30);
    for (int i = 0; i < b->n; i++) {
        const eq_coef_t *k = &b->c[i];
        int64_t P = (((int64_t)(1 << 29) + k->a2) * cw >> 31) + (k->a1 >> 1);    /* Q28 */
        int64_t Q = ((int64_t)(1 << 29) - k->a2) * sw >> 31;
        int64_t R = Q + ((int64_t)k->d * sw >> 30);
        int64_t num = P * P + R * R, den = P * P + Q * Q;

        while (den > 0x7FFFFFFF) {                            /* (|H|^2 <= 16: num < 2^35 then) */
            den >>= 1;
            num >>= 1;
        }
        if (den <= 0)
            return (int64_t)1 << 40;                          /* (cannot happen: the safe side) */
        m = (m * ((num << 24) / den)) >> 24;
    }
    return m;
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = 1ull << 62;

    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

int eq_design(eq_bank_t *b, unsigned mode, const int8_t *gain, uint32_t rate)
{
    uint32_t hz4[EQ_BANDS];
    int db[EQ_BANDS], n = 0;
    int64_t peak = 0;

    memset(b, 0, sizeof *b);
    b->rate = rate;
    b->pre = PRE_ONE;
    b->pre_milli = 1000;
    if (!rate)
        return 0;
    /* the bands below 0.45 rate (hz4 < 1.8 rate), in rising order */
    if (mode == EQ_CUSTOM) {
        for (int k = 0; k < EQ_BANDS; k++)
            if (gain[k] && gain[k] >= -EQ_GAIN_MAX && gain[k] <= EQ_GAIN_MAX && (125u << k) * 5u < 9u * rate) {
                hz4[n] = 125u << k;                           /* 31.25 Hz x 2^k, in quarter Hz */
                db[n++] = gain[k];
            }
    } else if (mode > EQ_OFF && mode < EQ_CUSTOM) {
        for (int k = 0; k < 5 && k_preset[mode - 1][k].hz; k++)
            if (k_preset[mode - 1][k].hz * 20u < 9u * rate) {
                hz4[n] = 4u * k_preset[mode - 1][k].hz;
                db[n++] = k_preset[mode - 1][k].db;
            }
    }
    if (!n)
        return 0;
    /* sections: cuts first, then boosts (a stable insertion sort by gain) */
    {
        int ord[EQ_BANDS];

        for (int i = 0; i < n; i++) {
            int j = i;

            while (j > 0 && db[ord[j - 1]] > db[i]) {
                ord[j] = ord[j - 1];
                j--;
            }
            ord[j] = i;
        }
        for (int i = 0; i < n; i++)
            section(&b->c[i], hz4[ord[i]], mode == EQ_CUSTOM ? KQ_CUSTOM : KQ_PRESET, db[ord[i]], rate);
        b->n = n;
    }
    /* the peak: each centre, three points between neighbours (a quarter, a half, three quarters of the
     * way, geometrically), and a quarter-octave ladder an octave down from the lowest and up from the
     * highest (below Nyquist). 2^(k/4), Q16: 65536, 77936, 92682, 110218. */
    {
        static const uint32_t k_q4[4] = { 65536, 77936, 92682, 110218 };

        for (int i = 0; i < n; i++) {
            int64_t m = mag2(b, hz4[i]);

            peak = m > peak ? m : peak;
            if (i + 1 < n) {                              /* lo (hi/lo)^(k/4), k = 1..3 */
                uint32_t lo = hz4[i], hi = hz4[i + 1];
                uint64_t r2 = isqrt64(((uint64_t)hi << 32) / lo), r4 = isqrt64(r2 << 16);   /* Q16 */

                for (int k = 1; k < 4; k++) {
                    uint64_t r = hi == 2 * lo ? k_q4[k] : k == 1 ? r4 : k == 2 ? r2 : (r2 * r4) >> 16;

                    m = mag2(b, (uint32_t)(((uint64_t)lo * r) >> 16));
                    peak = m > peak ? m : peak;
                }
            }
        }
        for (int k = 1; k <= 4; k++) {
            uint32_t r = k == 4 ? 131072u : k_q4[k];
            uint32_t f = (uint32_t)(((uint64_t)hz4[0] << 16) / r), g = (uint32_t)(((uint64_t)hz4[n - 1] * r) >> 16);
            int64_t m = mag2(b, f);

            peak = m > peak ? m : peak;
            if (g < 2u * rate) {                              /* (below Nyquist: hz4 < 4 rate / 2) */
                m = mag2(b, g);
                peak = m > peak ? m : peak;
            }
        }
    }
    /* the preamp: 0.9 / sqrt(peak) when that is under 1 (peak in Q24; sqrt(peak << 20) is Q22) */
    if (peak > (int64_t)13589545) {                           /* 0.81 x 2^24 */
        uint32_t r = isqrt64((uint64_t)peak << 20);

        b->pre = (int32_t)(4053239664633446ull / r);          /* 0.9 x 2^52 / r: Q30 */
    }
    b->pre_milli = (int)(((int64_t)b->pre * 1000) >> 30);
    return n;
}

