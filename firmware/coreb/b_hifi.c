/*
 * b_hifi.c -- the HIFI accelerator's FLAC LPC function, driven the way stock MINIV390's core-B FLAC
 * decoder drives it (docs/HIFI.md has each value with its source; the addresses below are stock's),
 * plus what stock does without: a self-test before the first use, a bounded wait on every job, and
 * the decoder's own software whenever either fails. Stock waits forever (its done poll at 0x01004330
 * and its DMA wait at 0x010052DC have no limit).
 *
 * A job (stock decode_subframe_lpc 0x0100423E, = SDK hifi_flacdec.o 0x5F8):
 *   FUN_CFG = FLAC | n << 15, INTCR |= 0x8000, FLAC_CFG = order << 16 | shift << 8,
 *   the coefficients reversed into 0xB000, FUN_CFG |= 3 (TX and RX start),
 *   DMA2 ch1: memory -> TX FIFO, ch0: RX FIFO -> memory, n words,
 *   wait for ch0's transfer and INTSR bit 15, INTCR &= ~0x8000.
 * Unlike stock (in place) the job runs out of place: the residuals stay as they were, so a job that
 * fails is redone in software from them.
 *
 * The block's clock is core B's HCLK (the SDK's HCLK_HIFI gate is in core B's group) [inferred]: it
 * slows with B's divider, which is why waits count as busy time (B polls, it never sleeps on it).
 */
#include <stdarg.h>

#include "b_rt.h"
#include "b_hifi.h"

#define REG(a)        (*(volatile uint32_t *)(a))
#define HIFI          0x01060000u           /* SDK hw_memap.h HIFIACC_BASE */
#define HIFI_DMACR    (HIFI + 0x00u)
#define HIFI_INTCR    (HIFI + 0x04u)
#define HIFI_INTSR    (HIFI + 0x08u)
#define HIFI_FUN_CFG  (HIFI + 0x0Cu)
#define HIFI_ACC_CLR  (HIFI + 0x10u)
#define HIFI_FLAC_CFG (HIFI + 0x2Cu)
#define HIFI_TX       (HIFI + 0x40u)
#define HIFI_RX       (HIFI + 0xA0u)
#define HIFI_COEF     (HIFI + 0xB000u)      /* "flac initial coef address max=64x32bit" */
#define FUN_FLAC      0xA0000000u           /* HIfi_ACC_TYPE_FLAC: 5 << 29 */
#define FUN_START     3u                    /* TX_Transfer_Start | RX_Transfer_Start */
#define INT_DONE      0x8000u               /* INTCR: Function_DONE_INT_ENABLE, INTSR: done */

/* DMA2 (SDK DMA_BASE2, DW_ahb_dmac, Hw_dma.h): channel n at n * 0x58 */
#define DMA2          0x01070000u
#define CH(c, o)      (DMA2 + (c) * 0x58u + (o))
#define CH_SAR 0x00u
#define CH_DAR 0x08u
#define CH_LLP 0x10u
#define CH_CTL 0x18u
#define CH_SIZE 0x1Cu
#define CH_CFGL 0x40u
#define CH_CFGH 0x44u
#define DMA2_RAW_TFR   (DMA2 + 0x2C0u)
#define DMA2_MASK_TFR  (DMA2 + 0x310u)
#define DMA2_CLEAR_TFR (DMA2 + 0x338u)
#define DMA2_CFG       (DMA2 + 0x398u)
#define DMA2_CHEN      (DMA2 + 0x3A0u)
#define CH_RX 0u                            /* stock HIFI_DMA_FROM_CH (data 0x010220F0) */
#define CH_TX 1u                            /* HIFI_DMA_TO_CH (0x010220EC) */
/* stock's DmaConfig2 table (0x01005C58), = the SDK's DMA_CTLL/CFGL/CFGH_HIFI_WR / _RD macros */
#define CTL_TX  0x02109124u                 /* 32-bit, burst 8, memory (master 1, +) -> peripheral (fixed) */
#define CFGL_TX 0x00000800u
#define CFGH_TX 0x00000807u                 /* DST_PER(1) */
#define CTL_RX  0x00A09424u                 /* peripheral (fixed) -> memory (master 1, +) */
#define CFGL_RX 0x00000400u
#define CFGH_RX 0x00000007u                 /* SRC_PER(0) */
#define CTL_INT_EN  1u                      /* stock sets it on every block (DmaConfig2 0x01000E78) */
#define CTL_LLP_EN  0x18000000u             /* LLP_SRC_EN | LLP_DST_EN */
#define DMA_BLOCK   4088u                   /* stock DMA_MAX_BLOCK_SIZE = LLP_BLOCK_SIZE (0x01000D9C) */

/* CRU (hiword write masks). SDK Hw_cru.h ids: HCLK_HIFI_GATE 0x39, HCLK_HIGH_DMA_GATE 0x4B,
 * HIFI_SRST 0x23, DMA2_SRST 0x37 (register = id / 16, bit = id % 16); stock 0x01000228. */
#define CRU_CLKGATE3  0x4018008Cu           /* bit 9: HIFI */
#define CRU_CLKGATE4  0x40180090u           /* bit 11: DMA2 */
#define CRU_SOFTRST2  0x401800C8u           /* bit 3: HIFI */
#define CRU_SOFTRST3  0x401800CCu           /* bit 7: DMA2 */

#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)

b_hifi_stats_t b_hifi_stats;
static uint32_t s_lli[2][2][6];             /* per channel: SAR, DAR, LLP, CTL, SIZE, DSTAT (SDK DMA_LLP) */
static uint32_t s_jobs;                     /* this boot of core B */
static int s_told;                          /* a mismatch was logged */

/* ---- time ------------------------------------------------------------------ */

uint32_t b_cycles(void)
{
    uint32_t t, v;

    do {
        t = AS->ticks;
        v = SYST_CVR;
    } while (t != AS->ticks);
    return t * (SYST_RVR + 1u) + (SYST_RVR - v);
}

uint32_t b_us(uint32_t cyc)
{
    return (uint32_t)((uint64_t)cyc * 10000u / (SYST_RVR + 1u));   /* SysTick: 100 Hz */
}

/* ---- core A's log --------------------------------------------------------- */

static char *put_u(char *d, char *e, uint32_t v, unsigned base, int width)
{
    char t[12];
    int n = 0;

    do {
        t[n++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    while (n < width && n < 12)
        t[n++] = '0';
    while (n && d < e)
        *d++ = t[--n];
    return d;
}

void b_msg(const char *fmt, ...)
{
    uint32_t w = AS->b_msg_wr;
    char *d, *e;
    int width;
    va_list ap;

    if (w - AS->b_msg_rd >= 4u)
        return;                              /* core A has not caught up: dropped */
    d = AS->b_msg[w % 4u];
    e = d + sizeof AS->b_msg[0] - 1;
    va_start(ap, fmt);
    for (; *fmt && d < e; fmt++) {
        if (*fmt != '%' || !fmt[1]) {
            *d++ = *fmt;
            continue;
        }
        width = 0;
        if (fmt[1] == '0' && fmt[2] >= '1' && fmt[2] <= '9') {   /* %0Nu, %0Nx */
            width = fmt[2] - '0';
            fmt += 2;
        }
        switch (*++fmt) {
        case 'u': d = put_u(d, e, va_arg(ap, unsigned), 10, width); break;
        case 'x': d = put_u(d, e, va_arg(ap, unsigned), 16, width); break;
        case 'd': {
            int v = va_arg(ap, int);

            if (v < 0 && d < e)
                *d++ = '-';
            d = put_u(d, e, v < 0 ? 0u - (unsigned)v : (unsigned)v, 10, 0);
            break;
        }
        case 's':
            for (const char *s = va_arg(ap, const char *); *s && d < e; )
                *d++ = *s++;
            break;
        default:
            *d++ = *fmt;
        }
    }
    va_end(ap);
    *d = 0;
    __asm__ volatile ("dmb" ::: "memory");
    AS->b_msg_wr = w + 1u;
}

/* ---- the block ------------------------------------------------------------ */

static uint32_t sysaddr(const void *p)       /* DMA2 takes system addresses, never B's 0 alias */
{
    uint32_t a = (uint32_t)p;

    return a < AS_B_RAM ? a + AS_B_RAM : a;
}

static void delay_cycles(uint32_t n)
{
    uint32_t t0 = b_cycles();

    while (b_cycles() - t0 < n)
        ;
}

static int s_up;                            /* this core B has powered the block (not gated since) */

/* Stock's decoder open (HIFI_DMA_TO_register 0x01000228, FLAC_decode_init 0x01002D00): both clocks
 * on, both blocks through a 1 ms reset (the same in its ALAC and APE images: hifi_power), then the
 * block's set-up. */
static void hifi_power(void)
{
    s_up = 1;
    REG(CRU_CLKGATE3) = 0x02000000u;
    REG(CRU_CLKGATE4) = 0x08000000u;
    REG(CRU_SOFTRST3) = 0x00800080u;
    REG(CRU_SOFTRST2) = 0x00080008u;
    delay_cycles((SYST_RVR + 1u) / 10u);     /* 1 ms (stock DelayMs2(1)) */
    REG(CRU_SOFTRST3) = 0x00800000u;
    REG(CRU_SOFTRST2) = 0x00080000u;
}

static void hifi_up(void)
{
    hifi_power();
    REG(HIFI_FUN_CFG) = FUN_FLAC;
    REG(HIFI_ACC_CLR) = 3u;                  /* TX and RX logic clear */
    REG(HIFI_DMACR) = 0x01080108u;           /* both FIFOs on DMA requests, levels 8 */
    REG(HIFI_INTCR) = 0x00830083u;           /* stock's (SDK Hifi_Set_ACC_Intcr) */
}

/* Off for good this boot: DMA2's channels stopped, both blocks reset and their clocks gated
 * (stock's decoder close, HIFI_DMA_TO_Unregister 0x01000278, gates them too). */
static void hifi_gate(void)
{
    if (s_up || AS->hifi_busy) {             /* (powered by this core B: b_hifi_fn.c's functions too) */
        REG(DMA2_CHEN) = 0x0300u;            /* ch0, ch1 off (write enables 8, 9) */
        REG(CRU_SOFTRST3) = 0x00800080u;
        REG(CRU_SOFTRST2) = 0x00080008u;
        delay_cycles((SYST_RVR + 1u) / 10u);
        REG(CRU_SOFTRST3) = 0x00800000u;
        REG(CRU_SOFTRST2) = 0x00080000u;
    }
    REG(CRU_CLKGATE3) = 0x02000200u;
    REG(CRU_CLKGATE4) = 0x08000800u;
    s_up = 0;
}

static void hifi_down(uint32_t state)
{
    hifi_gate();
    AS->hifi_state = state;
    AS->hifi_busy = 0;
}

/* One DMA2 transfer of n words (stock DmaConfig2 0x01000D5E): one block, or a chain of 4088-word
 * blocks; the transfer interrupt latched (INT_EN) but masked: B polls RawTfr. */
static void dma2_start(uint32_t ch, uint32_t src, uint32_t dst, uint32_t n, uint32_t ctl,
                       uint32_t cfgl, uint32_t cfgh)
{
    REG(DMA2_CLEAR_TFR) = 1u << ch;
    REG(DMA2_MASK_TFR) = 0x100u << ch;
    REG(CH(ch, CH_SAR)) = src;
    REG(CH(ch, CH_DAR)) = dst;
    REG(CH(ch, CH_CFGL)) = cfgl;
    REG(CH(ch, CH_CFGH)) = cfgh;
    if (n > DMA_BLOCK) {
        uint32_t *l = s_lli[ch][0], *m = s_lli[ch][1];
        uint32_t step = DMA_BLOCK * 4u;

        l[0] = src;
        l[1] = dst;
        l[2] = sysaddr(m);
        l[3] = ctl | CTL_INT_EN | CTL_LLP_EN;
        l[4] = DMA_BLOCK;
        m[0] = ch == CH_TX ? src + step : src;
        m[1] = ch == CH_RX ? dst + step : dst;
        m[2] = 0;
        m[3] = ctl | CTL_INT_EN | CTL_LLP_EN;
        m[4] = n - DMA_BLOCK;
        l[5] = m[5] = 0;
        REG(CH(ch, CH_CTL)) = ctl | CTL_INT_EN | CTL_LLP_EN;
        REG(CH(ch, CH_LLP)) = sysaddr(l);
    } else {
        REG(CH(ch, CH_CTL)) = ctl | CTL_INT_EN;
        REG(CH(ch, CH_SIZE)) = n;
        REG(CH(ch, CH_LLP)) = 0;
    }
    REG(DMA2_CFG) = 1u;
    REG(DMA2_CHEN) = 0x101u << ch;
}

int b_hifi_flac(const int32_t *in, int32_t *out, uint32_t n, uint32_t order, uint32_t shift,
                const int32_t *coef)
{
    uint32_t t0, t, lim;

    AS->hifi_busy = 1;
    REG(HIFI_FUN_CFG) = FUN_FLAC | n << 15;
    REG(HIFI_INTCR) |= INT_DONE;
    REG(HIFI_FLAC_CFG) = order << 16 | shift << 8;
    for (uint32_t j = 0; j < order; j++)
        REG(HIFI_COEF + 4u * j) = (uint32_t)coef[order - 1u - j];   /* the x[n-1] coefficient last */
    REG(HIFI_FUN_CFG) = FUN_FLAC | n << 15 | FUN_START;
    t0 = b_cycles();
    dma2_start(CH_TX, sysaddr(in), HIFI_TX, n, CTL_TX, CFGL_TX, CFGH_TX);     /* stock: TX first */
    dma2_start(CH_RX, HIFI_RX, sysaddr(out), n, CTL_RX, CFGL_RX, CFGH_RX);
    /* the limit: 10 ms, plus 8 cycles a tap (the block's own speed is not measured: docs/HIFI.md; in
     * QEMU, core A's share of the round robin has held a job's end back 3 ms) */
    lim = (SYST_RVR + 1u) + order * n * 8u;
    while (!(REG(DMA2_RAW_TFR) & (1u << CH_RX)) || !(REG(HIFI_INTSR) & INT_DONE)) {
        b_hifi_stats.polls++;
        if (b_cycles() - t0 > lim) {
            uint32_t rx = REG(DMA2_RAW_TFR) & (1u << CH_RX);     /* (before its clock goes off) */

            t = b_cycles() - t0;
            hifi_down(AS_HIFI_DEAD);
            b_msg("hifi: job %u (order %u, %u samples) not done after %u us (RX %s): FLAC LPC in "
                  "software until power-off", (unsigned)s_jobs, (unsigned)order, (unsigned)n,
                  (unsigned)b_us(t), rx ? "done" : "not done");
            return -1;
        }
    }
    t = b_cycles() - t0;
    REG(HIFI_INTCR) &= ~INT_DONE;            /* stock's done-flag clear (0x01004342) */
    REG(DMA2_CLEAR_TFR) = 3u;
    AS->hifi_busy = 0;
    s_jobs++;
    b_hifi_stats.jobs++;
    b_hifi_stats.samples += n;
    b_hifi_stats.wait_cyc += t;
    if (t > b_hifi_stats.wait_max_cyc)
        b_hifi_stats.wait_max_cyc = t;
    return 0;
}

/* ---- self-test ------------------------------------------------------------ */

/* The reference: the 64-bit sum (SDK Flac_Decode_Lpc, libFLAC, dr_flac's wide path). */
static int32_t predict(const int32_t *s, uint32_t order, uint32_t shift, const int32_t *coef)
{
    int64_t acc = 0;

    for (uint32_t j = 0; j < order; j++)
        acc += (int64_t)coef[j] * s[-1 - (int32_t)j];
    return (int32_t)(acc >> shift);
}

typedef struct {
    uint16_t n;
    uint8_t bits, prec, order, shift, wide;
} vec_t;

/* Narrow and wide by b_hifi_wide(); each has one sum at the largest its class allows (every term
 * at full scale, the signs lined up): just under 2^31 for narrow, about 2^42 for wide. */
static const vec_t k_vec[] = {
    { 4608, 16, 12, 12, 11, 0 },    /* stock's longest job: a two-block DMA chain */
    {  300, 16, 14,  3,  0, 0 },    /* the lowest order the block gets, no shift */
    { 4096, 24, 15, 32, 15, 1 },    /* 24-bit: the highest order, precision and shift */
    { 1000, 25, 15,  8, 14, 1 },    /* a 24-bit file's side channel */
};

static uint32_t s_rng;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int32_t rnd_bits(uint32_t bits)       /* uniform, signed, `bits` wide */
{
    return (int32_t)(rnd() << (32u - bits)) >> (32u - bits);
}

/* First sample where out[] is not the software's restore of in[] (in order, so each check uses
 * samples already proven), or -1. */
static int32_t first_bad(const int32_t *in, const int32_t *out, uint32_t n, uint32_t order,
                         uint32_t shift, const int32_t *coef)
{
    for (uint32_t i = 0; i < n; i++) {
        int32_t want = i < order ? in[i] : (int32_t)((uint32_t)in[i] + (uint32_t)predict(out + i, order, shift, coef));

        if (out[i] != want)
            return (int32_t)i;
    }
    return -1;
}

/* 0 = all passed; else the first failing vector's index + 1 for the class it failed. */
static int self_test(int32_t *in, int32_t *out, uint32_t *t_us, uint32_t *fail_class)
{
    int32_t coef[32];
    uint32_t t = 0, bad_wide = 0;

    for (unsigned v = 0; v < sizeof k_vec / sizeof k_vec[0]; v++) {
        const vec_t *k = &k_vec[v];
        int32_t big = (1 << (k->bits - 1)) - 1, i0 = k->n / 2;
        uint32_t t0;
        int32_t bad;

        s_rng = 0x9E3779B9u ^ (v << 8);
        for (uint32_t j = 0; j < k->order; j++)
            coef[j] = rnd_bits(k->prec);
        if (b_hifi_wide(k->bits, k->order, coef) != k->wide)
            return -2;                       /* (a vector in the wrong class: a build error) */
        for (uint32_t i = 0; i < k->n; i++)
            out[i] = rnd_bits(k->bits);
        for (uint32_t j = 0; j < k->order; j++)   /* one sum at its largest */
            out[i0 - 1 - (int32_t)j] = coef[j] < 0 ? -big : big;
        for (uint32_t i = 0; i < k->n; i++)  /* the residuals an encoder would send */
            in[i] = i < k->order ? out[i] : (int32_t)((uint32_t)out[i] - (uint32_t)predict(out + i, k->order,
                                                                                           k->shift, coef));
        t0 = b_cycles();
        if (b_hifi_flac(in, out, k->n, k->order, k->shift, coef) != 0)
            return -1;                       /* timed out: already off and logged */
        t += b_cycles() - t0;
        bad = first_bad(in, out, k->n, k->order, k->shift, coef);
        if (bad >= 0) {
            int32_t want = (uint32_t)bad < k->order ? in[bad] :
                           (int32_t)((uint32_t)in[bad] + (uint32_t)predict(out + bad, k->order, k->shift, coef));

            b_msg("hifi: self-test job %u (%u-bit, order %u, shift %u, %s) sample %d: hifi %d, software %d",
                  v, k->bits, k->order, k->shift, k->wide ? "wide" : "narrow", (int)bad, (int)out[bad], (int)want);
            if (!k->wide) {
                *fail_class = 0;
                return (int)v + 1;
            }
            bad_wide = v + 1;
        }
    }
    *t_us = b_us(t);
    *fail_class = 1;
    return (int)bad_wide;
}

/* ---- the decoder's side ---------------------------------------------------- */

uint32_t b_hifi_mode(void)
{
    uint32_t st = AS->hifi_state;

    return st == AS_HIFI_OK || st == AS_HIFI_NARROW ? AS->hifi_mode : AS_HIFI_OFF;
}

int b_hifi_wants(uint32_t n, uint32_t order)
{
    uint32_t st = AS->hifi_state;

    if (AS->hifi_mode == AS_HIFI_OFF || (st != AS_HIFI_OK && st != AS_HIFI_NARROW))
        return 0;
    return order >= 3u && order <= 32u && n > order && n <= B_HIFI_MAX_COUNT;
}

/* Every partial sum is at most sum |c| * (2^(bits-1)) in size (each sample of a valid stream fits
 * `bits`): below 2^31 a 32-bit accumulator cannot wrap. (dr_flac's own split, bits + precision +
 * log2(order) > 32, is coarser: it calls nearly every 16-bit subframe of flac -5..-8 wide.) */
int b_hifi_wide(uint32_t bits, uint32_t order, const int32_t *coef)
{
    uint64_t sum = 0;

    for (uint32_t j = 0; j < order; j++)
        sum += (uint32_t)(coef[j] < 0 ? -coef[j] : coef[j]);
    return bits > 32u || (sum << (bits - 1u)) >= 0x80000000ull;
}

int b_hifi_class_ok(int wide)
{
    return !wide || AS->hifi_state == AS_HIFI_OK;
}

void b_hifi_mismatch(uint32_t i, int32_t hw, int32_t sw, uint32_t order, uint32_t shift, int wide)
{
    b_hifi_stats.mismatch_jobs++;
    if (!s_told) {
        s_told = 1;
        b_msg("hifi: %s job %u (order %u, shift %u, %s) sample %u: hifi %d, software %d%s",
              AS->hifi_mode == AS_HIFI_VERIFY ? "verify:" : "check:", (unsigned)(s_jobs - 1u), (unsigned)order,
              (unsigned)shift, wide ? "wide" : "narrow", (unsigned)i, (int)hw, (int)sw,
              AS->hifi_mode == AS_HIFI_VERIFY ? " (counting on)" : ": that class in software from now on");
    }
    if (AS->hifi_mode != AS_HIFI_VERIFY) {
        if (wide && AS->hifi_state == AS_HIFI_OK)
            AS->hifi_state = AS_HIFI_NARROW;
        else
            hifi_down(AS_HIFI_FAILED);
    }
}

int b_hifi_open(void)
{
    uint32_t st = AS->hifi_state, t_us = 0, cls = 0;
    int32_t *in, *out;
    int r;

    if (AS->hifi_busy) {                     /* the last core B was reloaded mid-access */
        AS->hifi_busy = 0;
        b_msg("hifi: core B was reloaded while it used the block (a stalled bus?): FLAC LPC in software "
              "until power-off");
        hifi_down(AS_HIFI_DEAD);
        return 0;
    }
    if (AS->hifi_mode == AS_HIFI_OFF) {
        if (st == AS_HIFI_UNTESTED) {
            hifi_down(AS_HIFI_UNTESTED);     /* not used: its clocks off (stock's state between tracks) */
            AS->hifi_state = AS_HIFI_UNTESTED;
        }
        return 0;
    }
    if (st == AS_HIFI_OK || st == AS_HIFI_NARROW) {
        hifi_up();                           /* stock's FLAC open at every track (seg78 0x01002B76 ->
                                              * 0x01000228): reset, then the set-up; ALAC or APE may have
                                              * used the block since (b_hifi_alac.c) */
        return 1;
    }
    if (st != AS_HIFI_UNTESTED)
        return 0;                            /* failed or dead this boot */
    in = b_try_alloc(B_HIFI_MAX_COUNT * 4u);
    out = in ? b_try_alloc(B_HIFI_MAX_COUNT * 4u) : NULL;
    if (!out) {
        b_free(in);
        return 0;                            /* no room now: tried at the next track */
    }
    AS->hifi_busy = 1;
    hifi_up();
    AS->hifi_state = AS_HIFI_OK;             /* (b_hifi_flac's own failure path reads it) */
    r = self_test(in, out, &t_us, &cls);
    b_free(out);
    b_free(in);
    if (r == -2) {
        b_msg("hifi: self-test vector in the wrong class: FLAC LPC in software");
        hifi_down(AS_HIFI_FAILED);
        return 0;
    }
    if (r < 0)
        return 0;                            /* timed out: DEAD, logged */
    if (r && !cls) {
        b_msg("hifi: self-test failed: FLAC LPC in software");
        hifi_down(AS_HIFI_FAILED);
        return 0;
    }
    AS->hifi_busy = 0;
    AS->hifi_state = r ? AS_HIFI_NARROW : AS_HIFI_OK;
    b_msg("hifi: self-test %s (4 jobs, %u us on the block): FLAC LPC on HIFI%s%s", r ? "failed for wide sums" :
          "passed", (unsigned)t_us, r ? " for narrow subframes only" : "",
          AS->hifi_mode == AS_HIFI_VERIFY ? ", every sample verified" : "");
    return 1;
}

/* ---- shared with the APE and ALAC functions (b_hifi_fn.c, b_hifi_ape.c, b_hifi_alac.c) ----------- */

void b_hifi_power(void)
{
    hifi_power();
}

int b_hifi_is_up(void)
{
    return s_up;
}

void b_hifi_gate(void)
{
    hifi_gate();
}

void b_hifi_down(uint32_t state)
{
    hifi_down(state);
}

/* One job's two transfers, as stock starts them: TX (in[] -> the TX FIFO) first, then RX. */
void b_hifi_dma(const int32_t *in, int32_t *out, uint32_t n)
{
    dma2_start(CH_TX, sysaddr(in), HIFI_TX, n, CTL_TX, CFGL_TX, CFGH_TX);
    dma2_start(CH_RX, HIFI_RX, sysaddr(out), n, CTL_RX, CFGL_RX, CFGH_RX);
}

uint32_t b_hifi_sysaddr(const void *p)
{
    return sysaddr(p);
}
