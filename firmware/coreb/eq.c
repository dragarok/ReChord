/*
 * eq.c -- the EQ (eq.h). Core B runs it on the decoder's words before they go into the PCM ring;
 * the test model (snowsky_echo_mini tests/eqref.c) compiles this file unchanged.
 *
 * Filters: the RBJ Audio EQ Cookbook's peaking EQ (R. Bristow-Johnson, "Cookbook formulae for audio
 * EQ biquad filter coefficients", peakingEQ: b0 = 1 + alpha A, b1 = -2 cos w0, b2 = 1 - alpha A,
 * a0 = 1 + alpha / A, a1 = -2 cos w0, a2 = 1 - alpha / A; A = 10^(dB/40), alpha = sin w0 / 2Q).
 * Its b1 equals a1 and b0 + b2 = a0 + a2, so after dividing by a0 a section is
 *     y = x + d (x - x2) + a1 (x1 - y1) + a2 (x2 - y2),   d = b0 - 1
 * three multiplies a sample instead of five, and exactly 0 dB at DC and Nyquist whatever the
 * coefficients round to (stock's own tables are up to 1.1 dB off at DC: MINIV390 bands 0 and 10).
 * A band at 0 dB has d = 0: the identity, so it is left out (no work, nothing changes).
 *
 * Fixed point (core B is a Cortex-M3: no FPU, SMLAL 64-bit multiply-accumulate): the signal is Q28
 * inside (the word >> 3: 3 bits of headroom over full scale, 28 bits below it), the coefficients
 * Q3.29, each section one 64-bit accumulator with first-order error feedback (the 29 bits a sample
 * drops go into the next sample's sum: the rounding noise is shaped by (1 - z^-1), so a 31 Hz band at
 * 192 kHz, whose poles sit close to z = 1, does not lift it). The words that come out are saturated
 * (never wrapped). Direct form I, run as its correction w = y - x (STEP below: the same words, two
 * operations a sample fewer): the state is the section's own input and correction, so a section's
 * coefficients never meet a state that is not theirs. Each section is one pass over the frames (the
 * first reads the words through the preamp, the last writes them saturated: run1), three samples a
 * turn with no state moved between them.
 *
 * Headroom: a preamp before the first section, 0.9 / the peak of the cascade's magnitude (the peak of
 * the coefficients as rounded, sampled at every centre, three points between neighbouring centres
 * and a quarter-octave ladder an octave past each end), at most 1 (0 dB): rechord_dsp_core.c's rule
 * (0.9 / the largest gain, after stock's RockEQReduce9dB), on the whole curve instead of one band.
 * The sections run cuts first, then boosts, so a steady tone never grows past the output's level
 * inside the chain; transients have the 3 bits.
 *
 * Changes are crossfaded: the old curve and the new one both run for 2^n frames (~21 ms at every
 * rate) and the output moves linearly from one to the other; the new one starts from the steady state
 * of the first frame it sees. Bypass (Off, or nothing to do at this rate) runs nothing at all.
 *
 * The design is integer arithmetic too (Q30, 64-bit products and quotients; no floating point on
 * core B): A = 10^(dB/40) from a table, sin and cos by their series, each coefficient one rounded
 * quotient. The test model checks it against the cookbook in double precision.
 */
#include <string.h>
#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

#include "eq.h"

#define Q28_MAX   0x0FFFFFFF
#define Q28_MIN   (-0x10000000)
#define PRE_ONE   (1 << 30)

/* ---- the engine ------------------------------------------------------------------------------- */

static uint8_t ramp_log2(uint32_t rate)
{
    return rate <= 48000u ? 10 : rate <= 96000u ? 11 : rate <= 192000u ? 12 : 13;   /* 21.3..23.2 ms */
}

void eq_restart(eq_t *e, uint32_t rate, unsigned mode, const int8_t *gain)
{
    e->mode = mode < EQ_MODES ? mode : EQ_OFF;
    memcpy(e->gain, gain, EQ_BANDS);
    e->rate = rate;
    e->ramp = 0;
    e->pos = 0;
    e->spos = 0;
    e->loud = 0;
    e->ramp_log2 = ramp_log2(rate);
    e->on = eq_design(&e->bank[e->cur], e->mode, e->gain, rate) > 0;
}

int eq_set(eq_t *e, unsigned mode, const int8_t *gain)
{
    eq_bank_t *nb;

    if (e->ramp)
        return -1;                              /* (one at a time: again after this crossfade) */
    if (mode >= EQ_MODES)
        mode = EQ_OFF;
    if (mode == e->mode && (mode != EQ_CUSTOM || !memcmp(gain, e->gain, EQ_BANDS)))
        return 0;
    e->mode = mode;
    memcpy(e->gain, gain, EQ_BANDS);
    if (!e->rate)
        return 1;                               /* (designed when the stream starts) */
    nb = &e->bank[e->cur ^ 1];
    if (!eq_design(nb, mode, gain, e->rate) && !e->on)
        return 1;                               /* bypass, and still bypass */
    if (!e->on) {                               /* from bypass: the bank playing is the identity */
        memset(&e->bank[e->cur], 0, sizeof e->bank[0]);
        e->bank[e->cur].rate = e->rate;
        e->bank[e->cur].pre = PRE_ONE;
    }
    nb->prime = 1;                              /* (its state from its first frame: run()) */
    e->ramp = 1;
    e->pos = 0;
    e->on = 1;
    return 1;
}

/* One sample of a section. The cookbook's y = x + d (x - x2) + a1 (x1 - y1) + a2 (x2 - y2) is run
 * as its correction w = y - x alone (the state keeps x and w, not y):
 *     w = d (x - x2) - a1 w1 - a2 w2,   y = x + w
 * the same sums (x1 - y1 = -w1 exactly), so the same words as the direct form, bit for bit, with two
 * subtractions and the accumulator's x 2^29 term fewer a sample. First-order error feedback: the
 * accumulator starts at e, the fraction (29 bits) the last sample dropped, which shapes the rounding
 * noise by (1 - z^-1) before the poles see it. Measured (tests/eqref.c): one +12 dB section's noise
 * -145 dBFS rms at worst (31.25 Hz at 384 kHz), -155 at 44.1 kHz; without it -43 and -80. Second order
 * ((1 - z^-1)^2) gains ~25 dB more and costs a fourth multiply or spills here: not worth it under the
 * 24-bit floor. na1, na2 = -a1, -a2 (the M3 has SMLAL, no SMLSL). */
#define STEP(w, x, x2, w1, w2) do { \
        int64_t acc_ = (int64_t)e; \
        acc_ += (int64_t)d * ((x) - (x2)); \
        acc_ += (int64_t)na1 * (w1); \
        acc_ += (int64_t)na2 * (w2); \
        (w) = (int32_t)(acc_ >> 29); \
        e = (uint32_t)acc_ & 0x1FFFFFFFu; \
    } while (0)

/* a word (sample left-justified) into Q28 through the preamp (Q30): (w pre) >> 33; at 0 dB (pre =
 * 2^30) that is w >> 3 exactly, so one form serves both */
#define IN(w, pre)  ((int32_t)(((int64_t)(w) * (pre)) >> 33))

static int32_t word(int32_t y)            /* Q28 -> the word, saturated (never wrapped) */
{
#if defined(__ARM_FEATURE_SAT)            /* (core B: one SSAT #29, the same clamp; the host the C) */
    return __ssat(y, 29) * 8;
#else
    return (y > Q28_MAX ? Q28_MAX : y < Q28_MIN ? Q28_MIN : y) * 8;
#endif
}
/* One section over one channel (every other word), in place, three samples a turn: sample k's x and
 * w go into the registers of sample k - 3's, which are dead by then, so no state moves between
 * samples (gcc -O2: 37 instructions for three, Cortex-M3; two a turn needed 29 for two). The bank's
 * first section reads the words through the preamp (in_w) and its last one writes words (out_w), so a
 * curve is one pass over the frames a section, none for the conversions. */
#define RD(v)      (in_w ? IN(v, pre) : (v))
#define WR(x, w)   (out_w ? word((x) + (w)) : (x) + (w))

/* the last n % 3 samples of a run1 (0..2), one at a time: one copy for every kind (its flags at run
 * time: two samples a call at most) */
static void __attribute__((noinline)) tail(const eq_coef_t *c, eq_state_t *st, int32_t *p, uint32_t n, int32_t pre,
                                           int in_w, int out_w)
{
    int32_t x2 = st->x2, x1 = st->x1, w2 = st->w2, w1 = st->w1, x, w;
    const int32_t d = c->d, na1 = -c->a1, na2 = -c->a2;
    uint32_t e = st->e;

    for (; n; n--, p += 2) {
        x = RD(p[0]);
        STEP(w, x, x2, w1, w2);
        p[0] = WR(x, w);
        x2 = x1;
        x1 = x;
        w2 = w1;
        w1 = w;
    }
    st->x2 = x2;
    st->x1 = x1;
    st->w2 = w2;
    st->w1 = w1;
    st->e = e;
}

static inline __attribute__((always_inline)) void run1(const eq_coef_t *c, eq_state_t *st, int32_t *p, uint32_t n,
                                                       int32_t pre, int in_w, int out_w)
{
    int32_t xa = st->x2, xb = st->x1, xc, wa = st->w2, wb = st->w1, wc;
    const int32_t d = c->d, na1 = -c->a1, na2 = -c->a2;
    uint32_t e = st->e;
    int32_t *end = p + 6 * (n / 3);

    for (; p != end; p += 6) {          /* (x2, x1) = (xa, xb), (w2, w1) = (wa, wb) on entry and exit */
        xc = RD(p[0]); STEP(wc, xc, xa, wb, wa); p[0] = WR(xc, wc);
        xa = RD(p[2]); STEP(wa, xa, xb, wc, wb); p[2] = WR(xa, wa);
        xb = RD(p[4]); STEP(wb, xb, xc, wa, wc); p[4] = WR(xb, wb);
    }
    st->x2 = xa;
    st->x1 = xb;
    st->w2 = wa;
    st->w1 = wb;
    st->e = e;
    if (n % 3)
        tail(c, st, p, n % 3, pre, in_w, out_w);
}

typedef void run1_fn(const eq_coef_t *, eq_state_t *, int32_t *, uint32_t, int32_t);
static void run1_mid(const eq_coef_t *c, eq_state_t *s, int32_t *p, uint32_t n, int32_t pre) { run1(c, s, p, n, pre, 0, 0); }
static void run1_in(const eq_coef_t *c, eq_state_t *s, int32_t *p, uint32_t n, int32_t pre) { run1(c, s, p, n, pre, 1, 0); }
static void run1_out(const eq_coef_t *c, eq_state_t *s, int32_t *p, uint32_t n, int32_t pre) { run1(c, s, p, n, pre, 0, 1); }
static void run1_io(const eq_coef_t *c, eq_state_t *s, int32_t *p, uint32_t n, int32_t pre) { run1(c, s, p, n, pre, 1, 1); }

/* the bank over n frames of words in place: words in; words out (out_w) or Q28 (a crossfade's mix) */
static void run(eq_bank_t *b, int32_t *x, uint32_t n, int out_w)
{
    if (!b->n) {                                /* the identity (a crossfade from or to bypass) */
        for (uint32_t i = 0; i < 2 * n; i++)
            x[i] = out_w ? word(IN(x[i], b->pre)) : IN(x[i], b->pre);
        return;
    }
    if (b->prime) {                             /* every section at the steady state of this frame */
        b->prime = 0;
        for (int ch = 0; ch < 2; ch++) {
            int32_t x0 = IN(x[ch], b->pre);

            for (int i = 0; i < b->n; i++) {
                eq_state_t *s = &b->s[ch][i];

                s->x1 = s->x2 = x0;             /* (unity at DC: every section's input is x0, w = 0) */
                s->w1 = s->w2 = 0;
                s->e = 0;
            }
        }
    }
    for (int i = 0; i < b->n; i++) {
        run1_fn *f = i == 0 ? (out_w && b->n == 1 ? run1_io : run1_in) : out_w && i == b->n - 1 ? run1_out : run1_mid;

        f(&b->c[i], &b->s[0][i], x, n, b->pre);
        f(&b->c[i], &b->s[1][i], x + 1, n, b->pre);
    }
}

/* Digital silence: at the end of a whole aligned chunk (EQ_CHUNK frames of the stream) that was all
 * zeros, a bank whose every state is within 2^8 of zero (-120 dBFS) is set to zero; from there zeros
 * come out of it, without running it, until a word is not zero. So the error feedback's idle pattern
 * (a few LSB) never reaches the DAC, and silence costs nothing. Decided at aligned chunk ends only,
 * for frames after them: the words never depend on how the frames arrive. */
#define SETTLED 256
static int loud(const int32_t *w, uint32_t k)
{
    for (uint32_t i = 0; i < k; i++)
        if (w[i])
            return 1;
    return 0;
}

static void settle(eq_bank_t *b)
{
    for (int ch = 0; ch < 2; ch++)
        for (int i = 0; i < b->n; i++) {
            const eq_state_t *s = &b->s[ch][i];

            if ((uint32_t)(s->x1 + SETTLED) > 2 * SETTLED || (uint32_t)(s->x2 + SETTLED) > 2 * SETTLED ||
                (uint32_t)(s->x1 + s->w1 + SETTLED) > 2 * SETTLED || (uint32_t)(s->x2 + s->w2 + SETTLED) > 2 * SETTLED)
                return;
        }
    memset(b->s, 0, sizeof b->s);
    b->zero = 1;
}

void eq_process(eq_t *e, int32_t *w, uint32_t frames)
{
    while (frames && e->on) {
        uint32_t n = EQ_CHUNK - e->spos % EQ_CHUNK, k;

        n = frames < n ? frames : n;
        k = 2u * n;
        e->spos += n;
        int32_t *a = e->buf[0];

        if (!e->loud)
            e->loud = loud(w, k);               /* (music: its first word) */
        if (!e->ramp) {
            eq_bank_t *b = &e->bank[e->cur];

            if (!b->zero || e->loud) {
                b->zero = 0;
                run(b, w, n, 1);                /* (in place: one pass a section) */
            }
            if (e->spos % EQ_CHUNK == 0 && !e->loud && !b->zero)
                settle(b);
        } else {
            eq_bank_t *nb = &e->bank[e->cur ^ 1], *ob = &e->bank[e->cur];
            int32_t *o = e->buf[1];
            uint32_t len = 1u << e->ramp_log2, sh = 15u - e->ramp_log2;

            memcpy(a, w, k * 4u);
            run(nb, a, n, 0);
            memcpy(o, w, k * 4u);
            run(ob, o, n, 0);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t p = e->pos + i;
                int64_t g = (int64_t)((p < len ? p : len) << sh);   /* 0 .. 32768 */

                w[2 * i] = word(o[2 * i] + (int32_t)((((int64_t)a[2 * i] - o[2 * i]) * g) >> 15));
                w[2 * i + 1] = word(o[2 * i + 1] + (int32_t)((((int64_t)a[2 * i + 1] - o[2 * i + 1]) * g) >> 15));
            }
            e->pos += n;
            if (e->pos >= len) {                /* the new curve alone from here (bypass if it is flat) */
                e->ramp = 0;
                e->cur ^= 1;
                e->on = e->bank[e->cur].n > 0;
            }
        }
        if (e->spos % EQ_CHUNK == 0)
            e->loud = 0;
        w += k;
        frames -= n;
    }
}
