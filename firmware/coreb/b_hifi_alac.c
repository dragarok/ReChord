/*
 * b_hifi_alac.c -- ALAC's adaptive LPC on the HIFI block (type 3), the way stock MINIV390's ALAC
 * decoder (core-B image seg79 lpc_prediction 0x0100459A, inlined again at 0x01004B40; = the SDK lib
 * RkNanoD_BHALAC_20160926 hifi_alac_alac.o 0x1D6, ffmpeg's alac.c) drives it; docs/HIFI.md section 3c
 * has every value with its source.
 *
 *   order 0 (a copy) and 31 (the first-order filter) stay in software, as does quant >= 12 (stock's
 *   Alac_Decode_Lpc); otherwise the CPU computes out[0..order] (the warm-up) and writes them back into
 *   the residual buffer, then:
 *     FUN_CFG = 0x60000000 | n << 15, INTCR |= 0x8000, ALAC_CFG = bps << 24 | order << 16 | quant << 8,
 *     the `order` coefficients to 0x9000 in ffmpeg's order (the bitstream's reversed: Apple's coefs[k]
 *     multiplies lag k + 1, the block's word j lag order - j), FUN_CFG |= 3,
 *     DMA2 residuals -> TX, RX -> out[], n words, the RX wait, INTSR bit 15, INTCR &= ~0x8000.
 *   the decoder's open (Alac_decode_init 0x010025CC): FUN_CFG = 0x60000000, ACC_CLR = 3, DMACR, INTCR.
 * The block adapts the coefficients itself sample by sample: a job is checked by running Apple's
 * unpc_block over its first samples (the warm-up and ALAC_CHECK predicted ones); hifi=verify runs it
 * over the whole channel. A difference, or a job that times out: the channel is redone by unpc_block
 * from the residuals (kept: the job runs out of place; the warm-up words restored).
 *
 * Beyond stock: the self-test (per boot, before the first use), the check, the bounded wait, the
 * software behind it, and a lower limit: quant 1..11 (stock: 0..11; Apple's code shifts by -1 at 0)
 * and blocks with something to predict (n > order + 1; stock sends any n > 1).
 */
#include "b_rt.h"
#include "b_hifi.h"
#include "b_hifi_fn.h"
#include "alac/dplib.h"

#define FUN_ALAC     0x60000000u                  /* HIfi_ACC_TYPE_ALAC: 3 << 29 */
#define ALAC_MAX     4096u                        /* stock's buffers: 0x18400-byte pool, 6 x 4096 words [inferred] */
#define ALAC_CHECK   32u                          /* predicted samples a job's check recomputes */

static struct {
    int on;                                       /* this track's LPC goes to the block */
    int32_t *ver;                                 /* hifi=verify: a channel's software result (frame_len) */
    uint32_t chans, hw;                           /* channel-frames with LPC; those the block restored */
    uint32_t orders;                              /* bit n: an LPC of order n */
} s;

/* One channel: in[] (out[0..order] already in place of the residuals), the coefficients in the
 * block's order, out[] = the samples. 0, or < 0: timed out (the block off; out[] undefined). */
static int alac_job(const int32_t *in, int32_t *out, uint32_t n, const int16_t *coefs, uint32_t order,
                    uint32_t chanbits, uint32_t den)
{
    uint32_t t0;

    b_hifi_fn_busy();
    HIFI_REG(HIFI_R_FUN_CFG) = FUN_ALAC | n << 15;         /* Hifi_Set_ACC_XFER_Disable */
    HIFI_REG(HIFI_R_INTCR) |= HIFI_INT_DONE;               /* Hifi_Enable_FUN_DONE_FLAG */
    HIFI_REG(HIFI_R_ALAC_CFG) = chanbits << 24 | order << 16 | den << 8;   /* Alac_Set_CFG */
    for (uint32_t j = 0; j < order; j++)                   /* HIFITranData: ffmpeg's array order */
        HIFI_REG(HIFI_R_ALAC_COEF + 4u * j) = (uint32_t)(int32_t)coefs[order - 1u - j];
    HIFI_REG(HIFI_R_FUN_CFG) = FUN_ALAC | n << 15 | HIFI_FUN_START;
    t0 = b_cycles();
    b_hifi_dma(in, out, n);
    /* the limit: 10 ms, plus 16 cycles a tap (each tap also adapts) */
    return b_hifi_fn_wait(t0, (SYST_RVR + 1u) + order * n * 16u, 1, "ALAC", n);
}

/* Apple's warm-up (unpc_block's first loop): out[0..order], also into in[] (what the block takes;
 * stock does the same, seg79 0x01004620). The residuals it replaces go to save[]. */
static void warm_up(int32_t *in, int32_t *out, uint32_t order, uint32_t chanbits, int32_t *save)
{
    uint32_t sh = 32u - chanbits;

    out[0] = in[0];
    for (uint32_t j = 1; j <= order; j++) {
        save[j] = in[j];
        out[j] = in[j] = (int32_t)((uint32_t)(in[j] + out[j - 1]) << sh) >> sh;
    }
}

static void restore(int32_t *in, uint32_t order, const int32_t *save)
{
    for (uint32_t j = 1; j <= order; j++)
        in[j] = save[j];
}

/* ---- self-test -------------------------------------------------------------------------------- */

static uint32_t s_rng;

static int32_t rnd_bits(uint32_t bits)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (int32_t)(s_rng << (32u - bits)) >> (32u - bits);
}

typedef struct {
    uint16_t n;
    uint8_t order, den, chanbits, res_bits, coef_bits;
} alac_vec_t;

/* Apple's encoder's own settings first (orders 4 and 8, denshift 9, 16/17-bit), then the edges stock
 * sends: order 1 and 30, quant 1 and 11, 24/25-bit and 32-bit channels, a 4096-word job (two LLIs). */
static const alac_vec_t k_vec[] = {
    { 4096, 8, 9, 17, 12, 11 },     /* stereo 16-bit: Apple's numactive 8, denshift 9 */
    { 1500, 4, 9, 16, 10, 12 },
    { 1000, 30, 11, 25, 20, 9 },    /* the highest order and quant, a 24-bit side channel */
    {  700, 1, 1, 32, 26, 15 },
    {  900, 13, 5, 20, 16, 14 },
};

static int alac_selftest(int32_t *res, int32_t *hw, int32_t *sw, uint32_t *t_us)
{
    int16_t c0[32], c1[32];
    int32_t save[32];
    uint32_t t = 0;

    for (unsigned v = 0; v < sizeof k_vec / sizeof k_vec[0]; v++) {
        const alac_vec_t *k = &k_vec[v];
        uint32_t t0;

        s_rng = 0x2545F491u ^ v << 9;
        for (uint32_t j = 0; j < k->order; j++)
            c0[j] = (int16_t)rnd_bits(k->coef_bits);
        for (uint32_t i = 0; i < k->n; i++)
            res[i] = i % 97u == 11u ? 0 : rnd_bits(i < 64u ? k->chanbits : k->res_bits);
        memcpy(c1, c0, sizeof c0);
        unpc_block(res, sw, (int32_t)k->n, c1, (int32_t)k->order, k->chanbits, k->den);   /* (res unchanged) */
        warm_up(res, hw, k->order, k->chanbits, save);
        t0 = b_cycles();
        if (alac_job(res, hw, k->n, c0, k->order, k->chanbits, k->den) < 0)
            return -1;                            /* timed out: off and logged */
        t += b_cycles() - t0;
        for (uint32_t i = 0; i < k->n; i++) {
            if (hw[i] != sw[i]) {
                b_msg("hifi: ALAC self-test job %u (order %u, quant %u, %u-bit) sample %u: hifi %d, software %d",
                      v, k->order, k->den, k->chanbits, (unsigned)i, (int)hw[i], (int)sw[i]);
                return (int)v + 1;
            }
        }
    }
    *t_us = b_us(t);
    return 0;
}

/* ---- the decoder's side ----------------------------------------------------------------------- */

int b_hifi_alac_open(uint32_t frame_len)
{
    uint32_t t_us = 0;
    int r;

    memset(&s, 0, sizeof s);
    memset(&b_hifi_fn_stats, 0, sizeof b_hifi_fn_stats);
    if (AS->hifi_mode == AS_HIFI_OFF) {
        b_hifi_fn_off();
        return 0;
    }
    if (AS->hifi_alac == AS_HIFI_FAILED || frame_len > ALAC_MAX || !b_hifi_fn_ready())
        return 0;
    if (AS->hifi_mode == AS_HIFI_VERIFY && !(s.ver = b_try_alloc(frame_len * 4u)))
        return 0;                                 /* (verify without room to verify: software) */
    if (!b_hifi_is_up())
        b_hifi_power();                           /* stock's decoder open: clocks on, 1 ms reset */
    b_hifi_fn_busy();                             /* Alac_decode_init's set-up */
    HIFI_REG(HIFI_R_FUN_CFG) = FUN_ALAC;
    HIFI_REG(HIFI_R_ACC_CLR) = 3u;
    HIFI_REG(HIFI_R_DMACR) = HIFI_DMACR_STOCK;
    HIFI_REG(HIFI_R_INTCR) = HIFI_INTCR_STOCK;
    AS->hifi_busy = 0;
    if (AS->hifi_alac == AS_HIFI_UNTESTED) {
        int32_t *res = b_try_alloc(ALAC_MAX * 4u), *hw = res ? b_try_alloc(ALAC_MAX * 4u) : NULL,
                *sw = hw ? b_try_alloc(ALAC_MAX * 4u) : NULL;

        if (!sw) {
            b_free(hw);
            b_free(res);
            return 0;                             /* no room now: tried at the next track */
        }
        r = alac_selftest(res, hw, sw, &t_us);
        memset(&b_hifi_fn_stats, 0, sizeof b_hifi_fn_stats);   /* (the self-test's jobs are not the track's) */
        b_free(sw);
        b_free(hw);
        b_free(res);
        if (r < 0)
            return 0;                             /* timed out: dead, logged */
        if (r) {
            AS->hifi_alac = AS_HIFI_FAILED;
            b_msg("hifi: ALAC self-test failed: its LPC in software");
            return 0;
        }
        AS->hifi_alac = AS_HIFI_OK;
        b_msg("hifi: ALAC self-test passed (%u jobs, %u us on the block): its LPC on HIFI%s",
              (unsigned)(sizeof k_vec / sizeof k_vec[0]), (unsigned)t_us,
              AS->hifi_mode == AS_HIFI_VERIFY ? ", every sample verified" : "");
    }
    s.on = 1;
    return 1;
}

/* alac_dec.c's hook (ALAC_RECHORD_HIFI): what unpc_block(pc1, out, num, coefs, numactive, chanbits,
 * denshift) computes, on the block when stock would send it there. */
void alac_rechord_unpc(int32_t *pc1, int32_t *out, int32_t num, int16_t *coefs, int32_t numactive,
                       uint32_t chanbits, uint32_t denshift)
{
    uint32_t n = (uint32_t)num, order = (uint32_t)numactive, p;
    int32_t save[32], ref[32 + ALAC_CHECK + 1];
    int16_t c0[32], c1[32];
    int bad = -1;

    if (order >= 1u && order <= 31u) {
        s.chans++;
        s.orders |= 1u << order;
    }
    if (!s.on || order < 1u || order > 30u || denshift < 1u || denshift > 11u || n <= order + 1u ||
        n > ALAC_MAX || chanbits > 32u) {
        unpc_block(pc1, out, num, coefs, numactive, chanbits, denshift);
        return;
    }
    memcpy(c0, coefs, order * sizeof c0[0]);      /* (unpc_block adapts its coefficients in place) */
    warm_up(pc1, out, order, chanbits, save);
    if (alac_job(pc1, out, n, c0, order, chanbits, denshift) < 0) {
        s.on = 0;
        restore(pc1, order, save);
        unpc_block(pc1, out, num, coefs, numactive, chanbits, denshift);
        b_hifi_fn_stats.redone++;
        return;
    }
    b_hifi_fn_stats.checked++;
    restore(pc1, order, save);
    if (s.ver) {                                  /* verify: the whole channel */
        memcpy(c1, c0, order * sizeof c1[0]);
        unpc_block(pc1, s.ver, num, c1, numactive, chanbits, denshift);
        for (uint32_t i = 0; i < n && bad < 0; i++)
            if (out[i] != s.ver[i])
                bad = (int)i;
    } else {                                      /* the warm-up and the first ALAC_CHECK predicted */
        p = n < order + 1u + ALAC_CHECK ? n : order + 1u + ALAC_CHECK;
        memcpy(c1, c0, order * sizeof c1[0]);
        unpc_block(pc1, ref, (int32_t)p, c1, numactive, chanbits, denshift);
        for (uint32_t i = 0; i < p && bad < 0; i++)
            if (out[i] != ref[i])
                bad = (int)i;
    }
    if (bad < 0) {
        s.hw++;
        return;
    }
    if (!b_hifi_fn_stats.mismatch)
        b_msg("hifi: %s ALAC job %u (order %u, quant %u, %u-bit) sample %d: hifi %d, software %d%s",
              s.ver ? "verify:" : "check:", (unsigned)(b_hifi_fn_stats.jobs - 1u), (unsigned)order,
              (unsigned)denshift, (unsigned)chanbits, bad, (int)out[bad], (int)(s.ver ? s.ver[bad] : ref[bad]),
              s.ver ? " (counting on)" : ": ALAC's LPC in software from now on");
    b_hifi_fn_stats.mismatch++;
    b_hifi_fn_stats.redone++;
    if (s.ver) {
        memcpy(out, s.ver, n * 4u);               /* the software's samples play */
    } else {
        AS->hifi_alac = AS_HIFI_FAILED;
        s.on = 0;
        unpc_block(pc1, out, num, coefs, numactive, chanbits, denshift);
    }
}

void b_hifi_alac_close(void)
{
    if (b_hifi_fn_stats.jobs || AS->hifi_mode != AS_HIFI_AUTO || AS->hifi_alac)
        b_hifi_fn_track_line("ALAC", AS->hifi_state == AS_HIFI_DEAD ? "block dead" :
                             AS->hifi_alac == AS_HIFI_FAILED ? "failed" :
                             AS->hifi_alac == AS_HIFI_OK ? "ok" : "untested");
    s.on = 0;
}

/* For b_alac.c's track line. */
void b_hifi_alac_counts(uint32_t *chans, uint32_t *hw, uint32_t *orders)
{
    *chans = s.chans;
    *hw = s.hw;
    *orders = s.orders;
}
