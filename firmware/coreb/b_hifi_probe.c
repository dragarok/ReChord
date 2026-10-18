/*
 * b_hifi_probe.c -- Developer > HIFI probe (test): what the HIFI block's FFT (type 2) and MAC (type 4)
 * functions compute, how fast, and how fast core B and the FLAC function are, measured on the silicon
 * and logged raw to ECHOLOG.TXT for tools/hifi_probe_decode.py (project repo) to read offline.
 * docs/HIFI_PLAN.md (section "The probe") has the plan, every value's source and the log format.
 *
 * Nothing here is in a decode path: core A asks for it (AS_CMD_HIFI_PROBE, audio/hifi_probe.c) with
 * the player stopped and a fresh core B, and loads a fresh core B again afterwards. The QEMU machine
 * raises a VIOLATION for any FFT or MAC access unless it runs with hifi-probe=on.
 *
 * What the SDK shows (driver/hifi/hifi_fft.c, hifi_mac.c, hifi.c, Hw_hifi.h, hifi.h) and nothing more:
 *   FFT_CFG (0x24) = FFT_FLAG (bits 27, 20) or IFFT_FLAG (0) | FFT_MODE (size 64..1024 in [31:29])
 *   MAC_CFG (0x30) = order << 18; MAC_DONE = bit 5; the result in MAC_RSH (0x38) then MAC_RSL (0x34)
 *   FFT data: imaginary at 0x1000, real at 0x2000, "max=1024" each; Hifi_Set_ACC_XFER_Disable's comment
 *   (GBK, partly lost) says FFT and MAC data are read straight from the RAM with the transfers off
 *   Hifi_Set_ACC_XFER_Disable = FUN_CFG type | count << 15; Hifi_Set_ACC_XFER_Start = the same | 3
 *   Hifi_Enable/Clear_FUN_DONE_FLAG = INTCR |= / &= ~bit 15; HIFITranData = a CPU word copy
 *   the open (HIFI_DMA_TO_register): both clocks on, both resets for 1 ms; then ACC_CLR, DMACR, INTCR
 * No firmware and no SDK decoder calls FFT or MAC, so the start itself is not shown: variant A starts
 * with XFER_Start (how every other function starts), variant B leaves the transfers off (the comment's
 * reading). A is tried first; B only if A never finishes the first job. Every wait is bounded; on a
 * timeout the block is reset and gated (stock's close), then opened again for the next job.
 */
#include "b_rt.h"
#include "b_hifi.h"

#define REG(a)        (*(volatile uint32_t *)(a))
#define HIFI          0x01060000u
#define H_DMACR       (HIFI + 0x00u)
#define H_INTCR       (HIFI + 0x04u)
#define H_INTSR       (HIFI + 0x08u)
#define H_FUN_CFG     (HIFI + 0x0Cu)
#define H_ACC_CLR     (HIFI + 0x10u)
#define H_FFT_CFG     (HIFI + 0x24u)
#define H_MAC_CFG     (HIFI + 0x30u)
#define H_MAC_RSL     (HIFI + 0x34u)
#define H_MAC_RSH     (HIFI + 0x38u)
#define W_DATI        (HIFI + 0x1000u)      /* FFT_DATI_ADD: imaginary, max 1024 */
#define W_DATR        (HIFI + 0x2000u)      /* FFT_DATR_ADD: real, max 1024 */
#define W_ALAC_COEF   (HIFI + 0x9000u)      /* ALAC_COEF_ADD, 64 words (stock's ALAC writes it) */
#define W_FLAC_COEF   (HIFI + 0xB000u)      /* FLAC_COEF_ADD, 64 words (stock's FLAC writes it) */
#define FUN_FFT       0x40000000u           /* HIfi_ACC_TYPE_FFT: 2 << 29 */
#define FUN_MAC       0x80000000u           /* HIfi_ACC_TYPE_MAC: 4 << 29 */
#define FUN_START     3u                    /* TX_Transfer_Start | RX_Transfer_Start */
#define INT_DONE      0x8000u
#define FFT_FLAG      ((1u << 27) | (1u << 20))
#define MAC_DONE      (1u << 5)

#define CRU_CLKGATE3  0x4018008Cu           /* bit 9: HCLK_HIFI */
#define CRU_CLKGATE4  0x40180090u           /* bit 11: HCLK_HIGH_DMA (DMA2) */
#define CRU_SOFTRST2  0x401800C8u           /* bit 3: HIFI_SRST */
#define CRU_SOFTRST3  0x401800CCu           /* bit 7: DMA2_SRST */
#define SYST_RVR      (*(volatile uint32_t *)0xE000E014u)

#define NMAX          1024u
#define FLAC_N        4608u                 /* B_HIFI_MAX_COUNT: stock's longest FLAC job */
#define TXT_CAP       (52u * 1024u)

/* ---- the text core A logs ------------------------------------------------- */

static char *s_txt;
static uint32_t s_len, s_lost;

static void tc(char c)
{
    if (s_len < TXT_CAP - 1u)
        s_txt[s_len++] = c;
    else
        s_lost++;
}

static void ts(const char *s)
{
    while (*s)
        tc(*s++);
}

static void tu(uint32_t v)
{
    char t[10];
    int n = 0;

    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        tc(t[--n]);
}

static void td(int32_t v)
{
    if (v < 0) {
        tc('-');
        tu(0u - (uint32_t)v);
    } else {
        tu((uint32_t)v);
    }
}

static void tx(uint32_t v)                   /* 8 hex digits */
{
    for (int k = 28; k >= 0; k -= 4)
        tc("0123456789abcdef"[v >> k & 15u]);
}

static void txm(uint32_t v)                  /* hex, no leading zeros */
{
    int k = 28;

    while (k > 0 && !(v >> k & 15u))
        k -= 4;
    for (; k >= 0; k -= 4)
        tc("0123456789abcdef"[v >> k & 15u]);
}

static uint32_t s_line;                      /* where the line being written starts */

static void tl(const char *head)             /* a new line (core A logs each, <= 160 bytes with its time) */
{
    if (s_len && s_txt[s_len - 1] != '\n')
        tc('\n');
    AS->probe_len = s_len;                   /* (the lines so far: core A logs them after a stall too) */
    s_line = s_len;
    ts("hfp: ");
    ts(head);
}

static void tsu(const char *k, uint32_t v) { tc(' '); ts(k); tc(' '); tu(v); }
static void tsx(const char *k, uint32_t v) { tc(' '); ts(k); tc(' '); tx(v); }

/* A dump of n words (8 hex digits each; "w*k": k equal words in a row), lines of at most ~130 bytes:
 * "hfp: d <tag> <index of the first>: w w ...". */
static void dump(const char *tag, const int32_t *w, uint32_t n)
{
    uint32_t i = 0;
    int fresh = 1;

    while (i < n) {
        uint32_t k = 1;

        while (i + k < n && w[i + k] == w[i])
            k++;
        if (fresh || s_len - s_line > 112u) {
            tl("d ");
            ts(tag);
            tc(' ');
            tu(i);
            tc(':');
            fresh = 0;
        }
        tc(' ');
        tx((uint32_t)w[i]);
        if (k > 2) {
            tc('*');
            tu(k);
        } else {
            k = 1;
        }
        i += k;
    }
}

/* CRC-32 (IEEE, reflected) of n words as little-endian bytes. */
static uint32_t crc_words(uint32_t c, const int32_t *w, uint32_t n)
{
    c = ~c;
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t b = 0; b < 32u; b++) {
            uint32_t bit = ((uint32_t)w[i] >> b ^ c) & 1u;

            c = c >> 1 ^ (bit ? 0xEDB88320u : 0u);
        }
    return ~c;
}

/* ---- vectors: xorshift32, as tools/hifi_probe_decode.py regenerates them ---- */

static uint32_t s_rng;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int32_t rnd_bits(uint32_t bits)
{
    return (int32_t)(rnd() << (32u - bits)) >> (32u - bits);
}

/* FFT vector v of size n: 0 impulse at 0, 1 impulse at 1, 2 random +-2^14, 3 random +-2^22, 4 DC 32767. */
static void fft_vec(uint32_t n, uint32_t v, int32_t *re, int32_t *im)
{
    s_rng = 0x9E3779B9u ^ (n << 8) ^ v;
    for (uint32_t i = 0; i < n; i++) {
        re[i] = v == 2 ? rnd_bits(15) : v == 3 ? rnd_bits(23) : v == 4 ? 32767 : 0;
        im[i] = v == 2 ? rnd_bits(15) : v == 3 ? rnd_bits(23) : 0;
    }
    if (v == 0)
        re[0] = 16384;
    if (v == 1)
        re[1] = 16384;
}

/* ---- the block: stock's open and close (b_hifi.c hifi_up/hifi_down, for another function) ---- */

static uint32_t s_timeouts, s_jobs;

static void wait_cycles(uint32_t n)
{
    uint32_t t0 = b_cycles();

    while (b_cycles() - t0 < n)
        ;
}

static void block_up(uint32_t fun)
{
    REG(CRU_CLKGATE3) = 0x02000000u;
    REG(CRU_CLKGATE4) = 0x08000000u;
    REG(CRU_SOFTRST3) = 0x00800080u;
    REG(CRU_SOFTRST2) = 0x00080008u;
    wait_cycles((SYST_RVR + 1u) / 10u);      /* 1 ms (stock DelayMs2(1)) */
    REG(CRU_SOFTRST3) = 0x00800000u;
    REG(CRU_SOFTRST2) = 0x00080000u;
    REG(H_FUN_CFG) = fun;
    REG(H_ACC_CLR) = 3u;
    REG(H_DMACR) = 0x01080108u;              /* the SDK's Hifi_Set_ACC_Dmacr */
    REG(H_INTCR) = 0x00830083u;              /* the SDK's Hifi_Set_ACC_Intcr */
}

static void block_down(void)
{
    REG(CRU_SOFTRST3) = 0x00800080u;
    REG(CRU_SOFTRST2) = 0x00080008u;
    wait_cycles((SYST_RVR + 1u) / 10u);
    REG(CRU_SOFTRST3) = 0x00800000u;
    REG(CRU_SOFTRST2) = 0x00080000u;
    REG(CRU_CLKGATE3) = 0x02000200u;
    REG(CRU_CLKGATE4) = 0x08000800u;
}

/* The wait: until INTSR bit 15 (or, for MAC, MAC_CFG's MAC_DONE), at most lim cycles.
 * Returns 0 timed out, 1 INTSR, 2 MAC_CFG, 3 both. */
static uint32_t wait_done(uint32_t lim, int mac, uint32_t *cyc)
{
    uint32_t t0 = b_cycles(), r = 0;

    for (;;) {
        r = (REG(H_INTSR) & INT_DONE ? 1u : 0u) | (mac && (REG(H_MAC_CFG) & MAC_DONE) ? 2u : 0u);
        *cyc = b_cycles() - t0;
        if (r || *cyc > lim)
            return r;
    }
}

static void copy_in(uint32_t win, const int32_t *src, uint32_t n)      /* the SDK's HIFITranData */
{
    for (uint32_t i = 0; i < n; i++)
        REG(win + 4u * i) = (uint32_t)src[i];
}

static void copy_out(uint32_t win, int32_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = (int32_t)REG(win + 4u * i);
}

/* ---- one FFT job ----------------------------------------------------------------------------- */

static int32_t *s_work;                     /* 3 x FLAC_N words: FLAC's in/out/software, or the FFT's 4 x NMAX */
static int32_t *s_re, *s_im, *s_ore, *s_oim;
static int s_fft_var = -1;                   /* -1: not known yet; 0 = A, 1 = B; 2 = neither finished */

static uint32_t mode_of(uint32_t n)
{
    uint32_t k = 0;

    while ((64u << k) < n)
        k++;
    return k << 29;                          /* FFT_MODE_64..1024 */
}

/* 1 = done (results in s_ore/s_oim), 0 = timed out (the block was reset, gated and opened again). */
static int fft_job(uint32_t n, int inv, uint32_t v, uint32_t var, int full)
{
    uint32_t sr[4], t0, c_in, c_run, c_out = 0, by, lim, lg = 0;
    char tag[12];

    while ((1u << lg) < n)
        lg++;
    AS->probe_step = 0x20000u | n << 4 | (inv ? 8u : 0u) | v;
    fft_vec(n, v, s_re, s_im);
    REG(H_FUN_CFG) = FUN_FFT | n << 15;                  /* Hifi_Set_ACC_XFER_Disable */
    sr[0] = REG(H_INTSR);
    REG(H_INTCR) |= INT_DONE;                            /* Hifi_Enable_FUN_DONE_FLAG */
    REG(H_FFT_CFG) = (inv ? 0u : FFT_FLAG) | mode_of(n); /* FFT_Set_CFG */
    t0 = b_cycles();
    copy_in(W_DATR, s_re, n);
    copy_in(W_DATI, s_im, n);
    c_in = b_cycles() - t0;
    sr[1] = REG(H_INTSR);
    lim = (SYST_RVR + 1u) + 32u * n * lg;                /* 10 ms + 32 cycles a butterfly-point */
    REG(H_FUN_CFG) = FUN_FFT | n << 15 | (var ? 0u : FUN_START);
    by = wait_done(lim, 0, &c_run);
    sr[2] = REG(H_INTSR);
    if (by) {
        t0 = b_cycles();
        copy_out(W_DATR, s_ore, n);
        copy_out(W_DATI, s_oim, n);
        c_out = b_cycles() - t0;
    }
    REG(H_INTCR) &= ~INT_DONE;                           /* Hifi_Clear_FUN_DONE_FLAG */
    sr[3] = REG(H_INTSR);
    s_jobs++;
    tl("fft");
    tsu("n", n);
    ts(inv ? " i" : " f");
    tsu("v", v);
    ts(var ? " var B" : " var A");
    ts(by ? " st done" : " st timeout");
    ts(" sr ");
    for (int k = 0; k < 4; k++) {
        txm(sr[k]);
        tc(k < 3 ? ',' : ' ');
    }
    ts("in ");
    tu(c_in);
    tsu("run", c_run);
    tsu("out", c_out);
    if (!by) {
        s_timeouts++;
        block_down();
        block_up(FUN_FFT);
        return 0;
    }
    tsx("crc", crc_words(crc_words(0, s_ore, n), s_oim, n));
    ts(" o");
    for (int k = 0; k < 2; k++) {
        tc(' ');
        td(s_ore[k]);
        tc(' ');
        td(s_oim[k]);
    }
    if (full) {
        tag[0] = 'f';
        {
            uint32_t m = n, p = 1;
            char t[6];
            int q = 0;

            while (m) {
                t[q++] = (char)('0' + m % 10u);
                m /= 10u;
            }
            while (q)
                tag[p++] = t[--q];
            tag[p++] = inv ? 'i' : 'f';
            tag[p++] = (char)('0' + v);
            tag[p++] = 'r';
            tag[p] = 0;
            dump(tag, s_ore, n);
            tag[p - 1] = 'i';
            dump(tag, s_oim, n);
        }
    }
    return 1;
}

static void fft_all(int timing_only)
{
    static const uint8_t k_full_v[5] = { 1, 0, 0, 0, 1 };   /* impulse at 0, DC: always (they compress) */

    block_up(FUN_FFT);
    for (uint32_t n = 64; n <= NMAX; n *= 2) {
        for (int inv = 0; inv < 2; inv++) {
            for (uint32_t v = 0; v < 5; v++) {
                int full = n == 64u || (n == 512u && v == 2u) || k_full_v[v];

                if (timing_only && !(n == 256u && v == 2u))
                    continue;
                if (s_fft_var < 0) {                     /* the first job finds the start */
                    if (fft_job(n, inv, v, 0, full))
                        s_fft_var = 0;
                    else if (fft_job(n, inv, v, 1, full))
                        s_fft_var = 1;
                    else
                        s_fft_var = 2;
                    continue;
                }
                if (s_fft_var == 2)
                    return;
                (void)fft_job(n, inv, v, (uint32_t)s_fft_var, full);
            }
        }
    }
}

/* ---- MAC -------------------------------------------------------------------------------------- */

static const int32_t k_mac_a[4] = { 1, 3, 7, 15 };   /* DATR, DATI, ALAC coef, FLAC coef: every pair's
                                                      * sum of products differs (a_j * a_k * sum i^2) */
static int s_mac_var = -1;

static void mac_load(uint32_t set)
{
    static const uint32_t k_win[4] = { W_DATR, W_DATI, W_ALAC_COEF, W_FLAC_COEF };

    for (uint32_t w = 0; w < 4; w++) {
        uint32_t n = w < 2 ? NMAX : 64u;

        s_rng = 0x85EBCA6Bu ^ w;
        for (uint32_t i = 0; i < n; i++)
            s_re[i] = set == 1 ? (int32_t)rnd() : k_mac_a[w] * (int32_t)(i + 1u);
        copy_in(k_win[w], s_re, n);
    }
}

static int mac_job(uint32_t order, uint32_t set, uint32_t var)
{
    uint32_t sr0, sr1, cfg0, cfg1, c_run, by, rsh = 0, rsl = 0;

    AS->probe_step = 0x40000u | order << 4 | set;
    REG(H_FUN_CFG) = FUN_MAC | order << 15;              /* Hifi_Set_ACC_XFER_Disable */
    REG(H_INTCR) |= INT_DONE;
    REG(H_MAC_CFG) = order << 18;                        /* MAC_Set_CFG */
    cfg0 = REG(H_MAC_CFG);
    sr0 = REG(H_INTSR);
    REG(H_FUN_CFG) = FUN_MAC | order << 15 | (var ? 0u : FUN_START);
    by = wait_done((SYST_RVR + 1u) + 32u * order, 1, &c_run);
    sr1 = REG(H_INTSR);
    cfg1 = REG(H_MAC_CFG);
    if (by) {
        rsh = REG(H_MAC_RSH);                            /* MAC_Get_result: RSH, then RSL */
        rsl = REG(H_MAC_RSL);
    }
    REG(H_INTCR) &= ~INT_DONE;
    s_jobs++;
    tl("mac");
    tsu("o", order);
    tsu("set", set);
    ts(var ? " var B" : " var A");
    ts(by ? " st done by " : " st timeout");
    if (by)
        ts(by == 1 ? "intsr" : by == 2 ? "cfg" : "both");
    ts(" sr ");
    txm(sr0);
    tc(',');
    txm(sr1);
    ts(" cfg ");
    txm(cfg0);
    tc(',');
    txm(cfg1);
    tsu("run", c_run);
    if (!by) {
        s_timeouts++;
        block_down();
        block_up(FUN_MAC);
        return 0;
    }
    tsx("rsh", rsh);
    tsx("rsl", rsl);
    return 1;
}

static void mac_all(int timing_only)
{
    static const uint16_t k_ord[2][7] = { { 1, 2, 3, 8, 64, 256, 1024 }, { 1, 8, 1024, 0, 0, 0, 0 } };

    block_up(FUN_MAC);
    for (uint32_t set = 0; set < 3; set++) {
        if (timing_only && set)
            break;
        mac_load(set == 2 ? 0u : set);
        for (int k = 0; k < (set == 2 ? 2 : 7); k++) {
            uint32_t o = set == 2 ? 8u : k_ord[set][k];

            if (!o || (timing_only && o != 1024u))
                continue;
            if (s_mac_var < 0) {
                if (mac_job(o, set, 0))
                    s_mac_var = 0;
                else if (mac_job(o, set, 1))
                    s_mac_var = 1;
                else
                    s_mac_var = 2;
                continue;
            }
            if (s_mac_var == 2)
                return;
            (void)mac_job(o, set, (uint32_t)s_mac_var);
        }
    }
}

/* ---- the windows: do they read back what was written, and what does a word cost? -------------- */

static int window_test(uint32_t win, uint32_t off)
{
    uint32_t t0, wc, rc, bad = 0, first = 0, w0 = 0, r0 = 0, diff = 0;

    AS->probe_step = 0x10000u | off;
    for (uint32_t i = 0; i < NMAX; i++)
        s_re[i] = (int32_t)(0xA5C30000u ^ (i * 0x9E3779B9u));
    t0 = b_cycles();
    copy_in(win, s_re, NMAX);
    wc = b_cycles() - t0;
    t0 = b_cycles();
    copy_out(win, s_ore, NMAX);
    rc = b_cycles() - t0;
    for (uint32_t i = 0; i < NMAX; i++) {
        uint32_t d = (uint32_t)(s_re[i] ^ s_ore[i]);

        diff |= d;
        if (d && !bad++) {
            first = i;
            w0 = (uint32_t)s_re[i];
            r0 = (uint32_t)s_ore[i];
        }
    }
    tl("win");
    tsx("off", off);
    tsu("n", NMAX);
    tsu("bad", bad);
    tsu("first", first);
    tsx("w", w0);
    tsx("r", r0);
    tsx("or", diff);
    tsu("wcyc", wc);
    tsu("rcyc", rc);
    return bad == 0;
}

/* ---- core B itself: cycles for 8000 instructions of each kind (QEMU counts instructions) ---- */

static uint32_t cpi_run(int kind, volatile uint32_t *mem)
{
    uint32_t t0 = b_cycles(), n = 1000u;

    switch (kind) {
    case 0:                                      /* 8 ALU instructions a turn */
        __asm__ volatile ("1: adds r0, r0, #1\n adds r1, r1, #1\n adds r2, r2, #1\n adds r3, r3, #1\n"
                          " adds r0, r0, #1\n adds r1, r1, #1\n adds r2, r2, #1\n subs %0, %0, #1\n bne 1b"
                          : "+r" (n) :: "r0", "r1", "r2", "r3", "cc");
        break;
    case 1:                                      /* 7 loads a turn */
        __asm__ volatile ("1: ldr r0, [%1]\n ldr r1, [%1, #4]\n ldr r2, [%1, #8]\n ldr r3, [%1, #12]\n"
                          " ldr r0, [%1, #16]\n ldr r1, [%1, #20]\n ldr r2, [%1, #24]\n subs %0, %0, #1\n bne 1b"
                          : "+r" (n) : "r" (mem) : "r0", "r1", "r2", "r3", "cc", "memory");
        break;
    default:                                     /* 7 stores a turn */
        __asm__ volatile ("1: str %0, [%1]\n str %0, [%1, #4]\n str %0, [%1, #8]\n str %0, [%1, #12]\n"
                          " str %0, [%1, #16]\n str %0, [%1, #20]\n str %0, [%1, #24]\n subs %0, %0, #1\n bne 1b"
                          : "+r" (n) : "r" (mem) : "cc", "memory");
        break;
    }
    return b_cycles() - t0;                      /* 9 instructions a turn: 9000 (+ a few) */
}

static void cpi_all(void)
{
    AS->probe_step = 0x08000u;
    tl("cpi");
    tsu("alu", cpi_run(0, NULL));
    tsu("ldr", cpi_run(1, (volatile uint32_t *)s_re));
    tsu("str", cpi_run(2, (volatile uint32_t *)s_re));
    tsu("ldra", cpi_run(1, (volatile uint32_t *)AS_IN_RING));   /* core A's SRAM: the input ring */
    ts(" (9000 instructions each)");
}

/* ---- FLAC: the block against the same restore in software ------------------------------------ */

static int32_t predict32(const int32_t *s, uint32_t order, uint32_t shift, const int32_t *c)
{
    int32_t acc = 0;

    for (uint32_t j = 0; j < order; j++)
        acc += c[j] * s[-1 - (int32_t)j];
    return acc >> shift;
}

static int32_t predict64(const int32_t *s, uint32_t order, uint32_t shift, const int32_t *c)
{
    int64_t acc = 0;

    for (uint32_t j = 0; j < order; j++)
        acc += (int64_t)c[j] * s[-1 - (int32_t)j];
    return (int32_t)(acc >> shift);
}

static void flac_one(uint32_t n, uint32_t bits, uint32_t order, uint32_t shift, uint32_t prec)
{
    int32_t coef[32], *in = s_work, *out = s_work + FLAC_N, *sw = s_work + 2u * FLAC_N;
    uint32_t t0, c_hw, c32 = 0, c64, bad = 0;
    int r;

    AS->probe_step = 0x30000u | order << 8 | bits;
    s_rng = 0x2545F491u ^ order ^ bits << 8;
    for (uint32_t j = 0; j < order; j++)
        coef[j] = rnd_bits(prec) >> 2;
    for (uint32_t i = 0; i < n; i++)
        sw[i] = rnd_bits(bits) >> 1;
    for (uint32_t i = 0; i < n; i++)                 /* the residuals an encoder would send */
        in[i] = i < order ? sw[i] : (int32_t)((uint32_t)sw[i] - (uint32_t)predict64(sw + i, order, shift, coef));
    t0 = b_cycles();
    r = b_hifi_flac(in, out, n, order, shift, coef);
    c_hw = b_cycles() - t0;
    if (!b_hifi_wide(bits, order, coef)) {
        t0 = b_cycles();
        for (uint32_t i = order; i < n; i++)
            sw[i] = in[i] + predict32(sw + i, order, shift, coef);
        c32 = b_cycles() - t0;
    }
    t0 = b_cycles();
    for (uint32_t i = order; i < n; i++)
        sw[i] = (int32_t)((uint32_t)in[i] + (uint32_t)predict64(sw + i, order, shift, coef));
    c64 = b_cycles() - t0;
    for (uint32_t i = 0; r == 0 && i < n; i++)
        bad += out[i] != (i < order ? in[i] : sw[i]);
    tl("flac");
    tsu("o", order);
    tsu("s", shift);
    tsu("n", n);
    tsu("b", bits);
    tsu("hw", c_hw);
    tsu("sw32", c32);
    tsu("sw64", c64);
    ts(r ? " st timeout" : bad ? " st bad" : " st ok");
    if (bad)
        tsu("bad", bad);
}

static void flac_all(int timing_only)
{
    if (!b_hifi_open() || AS->hifi_state != AS_HIFI_OK) {
        tl("flac skipped:");
        tsu("state", AS->hifi_state);
        tsu("mode", AS->hifi_mode);
        return;
    }
    flac_one(4096, 16, 8, 11, 13);                   /* flac -5's CD16 shape */
    if (timing_only)
        return;
    flac_one(4096, 16, 12, 12, 14);
    flac_one(4096, 24, 8, 14, 15);
    flac_one(4096, 24, 32, 15, 15);
    flac_one(256, 16, 8, 11, 13);                    /* a short job: the per-job overhead */
    flac_one(4608, 16, 3, 9, 12);
}

/* ---- the run ---------------------------------------------------------------------------------- */

void b_probe(void)
{
    uint32_t t0 = b_cycles(), arg = AS->probe_arg;
    int timing_only = arg != 0;

    AS->probe_len = 0;
    s_txt = b_try_alloc(TXT_CAP);
    s_work = b_try_alloc(3u * 4u * FLAC_N);
    if (!s_txt || !s_work) {
        b_free(s_txt);
        AS->probe_state = AS_PROBE_NOMEM;
        return;
    }
    s_re = s_work;
    s_im = s_re + NMAX;
    s_ore = s_im + NMAX;
    s_oim = s_ore + NMAX;
    s_len = s_lost = s_timeouts = s_jobs = 0;
    s_fft_var = s_mac_var = -1;
    AS->probe_text = (uint32_t)s_txt < AS_B_RAM ? (uint32_t)s_txt + AS_B_RAM : (uint32_t)s_txt;
    AS->probe_state = AS_PROBE_RUNNING;
    tl("v1");
    tsu("arg", arg);
    tsu("b_hz", AS->b_hz);
    tsu("div", AS->b_div);
    tsu("rvr", SYST_RVR + 1u);
    cpi_all();
    flac_all(timing_only);                           /* (b_hifi.c: its own open, self-test and close) */
    AS->hifi_busy = 1;                               /* a stall from here on: FLAC on HIFI off (b_hifi_open) */
    block_up(FUN_FFT);
    if (window_test(W_DATR, 0x2000u) + window_test(W_DATI, 0x1000u) < 2)
        tl("win: the FFT windows do not read back as written (FFT results unreadable)");
    fft_all(timing_only);
    mac_all(timing_only);
    block_down();                                    /* gated: stock's state between tracks */
    AS->hifi_busy = 0;
    AS->probe_step = 0xFFFFFu;
    tl("end");
    tsu("jobs", s_jobs);
    tsu("timeouts", s_timeouts);
    tsu("us", b_us(b_cycles() - t0));
    tsu("lost", s_lost);
    ts(" block gated");
    tc('\n');
    AS->probe_len = s_len;
    __asm__ volatile ("dmb" ::: "memory");
    AS->probe_state = AS_PROBE_DONE;                 /* (the buffers stay: core A reads the text, then
                                                      * loads a fresh core B) */
}
