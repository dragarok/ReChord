/*
 * eq_design.c -- the EQ's curves and their design (eq.h; the engine and the filters are eq.c). Built
 * -Os (it runs once a curve or a rate); the test model compiles this file unchanged.
 *
 * What runs is planned first (plan()): at 32 kHz and up each preset is its fewest sections (eq.h
 * EQ_FIT_TABLE, stock's curve fitted within 0.5 dB), below that stock's bands themselves; Custom is its
 * non-zero bands, with runs of equal neighbours at +-1 or +-2 dB merged into one section while the
 * merges' summed worst errors stay within 0.5 dB (k_run). The peak (the preamp) is looked for at the
 * bands as set, whatever runs.
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
/* Above EQ_FIT_RATE each preset runs its fewest sections instead (eq.h EQ_FIT_TABLE: tests/eqfit.c) */
typedef struct { uint8_t mode; uint16_t hz4; uint32_t kq, A; } eq_fit_t;     /* kq = 1/2Q, A: Q30 */
#define EQ_FIT_ROW_(a, mode, hz4, kq, A)  { mode, hz4, kq, A },
static const eq_fit_t k_fit[] = { EQ_FIT_TABLE(EQ_FIT_ROW_, 0) };

/* Custom: a run of L equal neighbours of gain g = +-1 or +-2 dB (octave bands; the top one at most
 * k_run's top, 4 kHz) as one section at the run's centre: Q and A for g = 1 and 2 (a cut: 1 / A, the
 * cookbook's peaking EQ at -g being the exact inverse of +g), and the worst error a dB of g over every
 * such position and every rate of 32 kHz and up (tests/eqfit.c runtab 7). Merges are taken left to
 * right while their errors add up to at most EQ_MERGE_MDB, the tolerance: the curve stays within it of
 * the bands' own (tests/eqref.c checks). Only at EQ_FIT_RATE and up. */
#define EQ_MERGE_MDB  500                /* milli-dB */
#define EQ_RUN_TOP    7                  /* the highest band a merged run may hold (4 kHz) */
typedef struct { uint32_t kq, A1, A2; uint16_t mdb; } eq_run_t;
static const eq_run_t k_run[] = {        /* L = 2 .. */
    {  756891927, 1150534151, 1232818545, 258 },   /* L 2: Q 0.7093, gain 1.2000 g */
    { 1391025328, 1159850514, 1252864688, 232 },   /* L 3: Q 0.3860, gain 1.3401 g */
    { 2049767192, 1163229475, 1260175194, 263 },   /* L 4: Q 0.2619, gain 1.3906 g */
    { 2849440972, 1167073593, 1268517945, 279 },   /* L 5: Q 0.1884, gain 1.4479 g */
    { 3755378361, 1170352698, 1275656221, 320 },   /* L 6: Q 0.1430, gain 1.4967 g */
};
#define EQ_RUN_MAX  (1 + (int)(sizeof k_run / sizeof k_run[0]))

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
static void section(eq_coef_t *c, uint32_t hz4, uint32_t kq, uint32_t A, uint32_t iA, uint32_t rate)
{
    int32_t s, ch, sw, al, aA, aiA, a0;

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

/* What a curve runs at a rate: its sections (centre, 1/2Q, A and 1/A) and the points its peak is looked
 * for at (the bands as the user or stock set them, rising) */
typedef struct {
    int n, np;
    uint32_t hz4[EQ_BANDS], kq[EQ_BANDS], A[EQ_BANDS], iA[EQ_BANDS], pk[EQ_BANDS];
} eq_plan_t;

static void add(eq_plan_t *p, uint32_t hz4, uint32_t kq, uint32_t A, uint32_t iA)
{
    p->hz4[p->n] = hz4;
    p->kq[p->n] = kq;
    p->A[p->n] = A;
    p->iA[p->n++] = iA;
}

static uint32_t inv(uint32_t A)                                /* 2^60 / A, rounded: Q30 */
{
    return (uint32_t)((((uint64_t)1 << 60) + A / 2) / A);
}

static int below(uint32_t hz4, uint32_t rate) { return hz4 * 5u < 9u * rate; }   /* hz < 0.45 rate */

static int plan(eq_plan_t *p, unsigned mode, const int8_t *gain, uint32_t rate)
{
    p->n = p->np = 0;
    if (!rate)
        return 0;
    if (mode == EQ_CUSTOM) {
        unsigned mdb = 0;                                         /* the merges' errors so far */

        for (int k = 0; k < EQ_BANDS; ) {
            int g = gain[k], L = 1;

            if (!g || g < -EQ_GAIN_MAX || g > EQ_GAIN_MAX || !below(125u << k, rate)) {
                k++;
                continue;
            }
            while (k + L < EQ_BANDS && L < EQ_RUN_MAX && gain[k + L] == g && below(125u << (k + L), rate))
                L++;                                              /* (a longer run: the rest is the next) */
            for (int j = 0; j < L; j++)
                p->pk[p->np++] = 125u << (k + j);                 /* 31.25 Hz x 2^k, in quarter Hz */
            if (L >= 2 && rate >= EQ_FIT_RATE && k + L - 1 <= EQ_RUN_TOP && (g == 1 || g == -1 || g == 2 || g == -2) &&
                mdb + (unsigned)(g < 0 ? -g : g) * k_run[L - 2].mdb <= EQ_MERGE_MDB) {
                const eq_run_t *r = &k_run[L - 2];
                uint32_t A = g == 1 || g == -1 ? r->A1 : r->A2, c = 125u << (k + (L - 1) / 2);

                mdb += (unsigned)(g < 0 ? -g : g) * r->mdb;
                if (!(L & 1))                                     /* (an even run: half an octave up) */
                    c = (uint32_t)(((uint64_t)c * 1518500250u + (1u << 29)) >> 30);   /* sqrt 2, Q30 */
                if (g > 0)
                    add(p, c, r->kq, A, inv(A));
                else
                    add(p, c, r->kq, inv(A), A);
            } else {
                for (int j = 0; j < L; j++)
                    add(p, 125u << (k + j), KQ_CUSTOM, k_A[g + EQ_GAIN_MAX], k_A[EQ_GAIN_MAX - g]);
            }
            k += L;
        }
    } else if (mode > EQ_OFF && mode < EQ_CUSTOM) {
        for (int k = 0; k < 5 && k_preset[mode - 1][k].hz; k++) {
            uint32_t hz4 = 4u * k_preset[mode - 1][k].hz;
            int db = k_preset[mode - 1][k].db;

            if (!below(hz4, rate))
                continue;
            p->pk[p->np++] = hz4;
            if (rate < EQ_FIT_RATE)
                add(p, hz4, KQ_PRESET, k_A[db + EQ_GAIN_MAX], k_A[EQ_GAIN_MAX - db]);
        }
        if (rate >= EQ_FIT_RATE)
            for (unsigned i = 0; i < sizeof k_fit / sizeof k_fit[0]; i++)
                if (k_fit[i].mode == mode)
                    add(p, k_fit[i].hz4, k_fit[i].kq, k_fit[i].A, inv(k_fit[i].A));
        /* (the peak's points rising: stock's rows are) */
    }
    return p->n;
}

int eq_count(unsigned mode, const int8_t *gain, uint32_t rate)
{
    eq_plan_t p;

    return plan(&p, mode, gain, rate);
}

int eq_design(eq_bank_t *b, unsigned mode, const int8_t *gain, uint32_t rate)
{
    eq_plan_t pl;
    const uint32_t *hz4 = pl.pk;
    int n = plan(&pl, mode, gain, rate), np = pl.np;
    int64_t peak = 0;

    memset(b, 0, sizeof *b);
    b->rate = rate;
    b->pre = PRE_ONE;
    b->pre_milli = 1000;
    if (!n)
        return 0;
    /* sections: cuts first, then boosts (a stable insertion sort by A) */
    {
        int ord[EQ_BANDS];

        for (int i = 0; i < n; i++) {
            int j = i;

            while (j > 0 && pl.A[ord[j - 1]] > pl.A[i]) {
                ord[j] = ord[j - 1];
                j--;
            }
            ord[j] = i;
        }
        for (int i = 0; i < n; i++)
            section(&b->c[i], pl.hz4[ord[i]], pl.kq[ord[i]], pl.A[ord[i]], pl.iA[ord[i]], rate);
        b->n = n;
    }
    /* the peak: each centre, three points between neighbours (a quarter, a half, three quarters of the
     * way, geometrically), and a quarter-octave ladder an octave down from the lowest and up from the
     * highest (below Nyquist). 2^(k/4), Q16: 65536, 77936, 92682, 110218. */
    {
        static const uint32_t k_q4[4] = { 65536, 77936, 92682, 110218 };

        for (int i = 0; i < np; i++) {
            int64_t m = mag2(b, hz4[i]);

            peak = m > peak ? m : peak;
            if (i + 1 < np) {                              /* lo (hi/lo)^(k/4), k = 1..3 */
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
            uint32_t f = (uint32_t)(((uint64_t)hz4[0] << 16) / r), g = (uint32_t)(((uint64_t)hz4[np - 1] * r) >> 16);
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

