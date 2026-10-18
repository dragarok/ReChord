/*
 * b_hifi_ape.c -- APE's NN filters on the HIFI block (types 0 = left, 1 = right), the way stock
 * MINIV390's APE decoder (core-B image seg80, = the SDK lib RkNanoD_BHAPE_20160806, ffmpeg's decoder)
 * drives it; docs/HIFI.md section 3b has every value with its source.
 *
 *   a frame starts (stock Get_Frame_Info 0x01003926), levels above 1000 only:
 *     FUN_CFG = 0 | 4096 << 15, APE_CFG = cfg, APE_CFG |= 0x200000 (APE_Clear), ACC_CLR = 3,
 *     DMACR = 0x01080108, INTCR = 0x00830083, FUN_CFG = 0 | 4096 << 15 | 3     (no data)
 *   each channel of each chunk (ape_apply_filters 0x0100341E):
 *     FUN_CFG = type << 29 | n << 15, APE_CFG = cfg, FUN_CFG |= 3, DMA2 n words in place, the RX wait
 *   cfg (APE_Set_CFG 0x01002502): 0x80000000 from version 3980 | normal 0x040B0000, high 0x100B0000,
 *     extra high 0x280AD000, insane 0x440BDF00.
 * The block runs every filter of the level on the channel (16 / 64 / 32 + 256 / 16 + 256 + 1280 taps)
 * and keeps their state (coefficients, history, adapt values, average) inside itself from job to job
 * until the next clear: the software cannot check a job without having run the frame itself.
 *   each track (stock's decoder open 0x0100261A -> HIFI_DMA_TO_register 0x01000228, its close
 *     0x0100274E -> 0x01000278): both clocks on and the block and DMA2 through a 1 ms reset at the open,
 *     the clocks gated at the close. So stock's block only ever runs one configuration (one level and
 *     version class) between two resets. ReChord does the same: every APE track (and the self-test of
 *     its level, run at the open) starts on a freshly reset block, and the close gates it. Without it
 *     (round3) the device failed exactly where the configuration changed with no reset in between
 *     (ECHOLOG_20261018_round3: the level 4000 self-test after a level 3000 track, "sample 2: hifi -193,
 *     software -151"; a level 2000 24-bit track after that self-test, "sample 3: hifi -12387, software
 *     -12481"); docs/HIFI.md 3b.5.
 *
 * So, beyond stock:
 *   - a self-test per level (and version class) before its first use: two frames of jobs on synthetic
 *     residuals, both channels interleaved as the decoder sends them, every output compared with
 *     libdemac's own filters (each comparison on the same state, so all equal = the block computes
 *     libdemac's filters for that level); the first frame includes a run of large constant input that
 *     drives the 32-bit dot product past its range (Monkey's Audio wraps it);
 *   - each frame's first job per channel checked against libdemac (its software state then is exactly
 *     the block's, from the frame's reset: its first APE_CHECK_PART samples, the rest of the job only if
 *     they differ), and in a track's first frame, whole, its first APE_TRACK_TAPS / taps jobs (16
 *     chunks of 256 at normal and high, 14 at extra high, 2 at insane: what shows only once the
 *     block's history has filled; about 1 M taps of software a channel): a difference and the software's samples play,
 *     the level is off until power-off, the rest of the frame continues in software, exactly;
 *   - hifi=verify: every job checked (the software runs the whole frame alongside; its samples play);
 *   - a bounded wait; a job that times out: the block off until power-off; if the software has the
 *     frame's state (a checked job) it redoes the chunk exactly; otherwise the filter state of the rest
 *     of that frame is gone with the block and those blocks play as silence (the next frame decodes in
 *     software). Stock waits forever;
 *   - for the next device run (measurement only, nothing plays from it): a failing self-test or check
 *     logs the job's hifi - software differences (samples s..s+11 from the first bad one, and 32..39,
 *     where the candidate semantics of docs/HIFI.md 3b.5 differ), and a failing self-test runs once more
 *     after another reset with 20 ms after each clear start (stock leaves ~15 ms [trace] between the
 *     clear and the first job: it entropy-decodes a 4096-block chunk in between; ReChord 0.02-0.2 ms).
 *     The level stays in software either way.
 */
#include "b_rt.h"
#include "b_hifi.h"
#include "b_hifi_fn.h"
#include "demac/demac.h"

void demac_filters_init(int compressiontype);
void demac_filters_sw(struct ape_ctx_t *ctx, int ch, int32_t *data, int count);

#define APE_CLC     0x00200000u                   /* SDK APE_CLC: APE_Clear */
#define APE_V3980   0x80000000u                   /* APE_VERSION_EQUAL_OR_GREAT_3980 */
#define APE_CHUNK   256u                          /* b_ape.c's blocks per decode_chunk() */
#define CLR_COUNT   4096u                         /* stock's clear start: 4096 << 15 (0x01003930) */
#define APE_TRACK_TAPS 4096u                      /* a track's first frame: this / taps jobs checked (<= 16) */
#define APE_CHECK_PART 64u                        /* samples of a later frame's first job checked */

static const uint32_t k_cfg[4] = { 0x040B0000u, 0x100B0000u, 0x280AD000u, 0x440BDF00u };   /* levels 2..5 */
static const uint16_t k_taps[4] = { 16, 64, 32 + 256, 16 + 256 + 1280 };

static struct {
    int on;                                       /* this track's filters go to the block */
    uint32_t check[2];                            /* this channel's next jobs checked (the frame's first) */
    int lost;                                     /* the rest of this frame is silence */
    int lost_chunk;                               /* the last decode_chunk() was silenced */
    uint32_t cfg, taps, k;                        /* APE_CFG, taps a sample, its hifi_ape bit */
    int32_t *chk;                                 /* APE_CHUNK words: a checked job's input, then the software's */
    uint32_t frames;                              /* frames started on the block */
    int powered;                                  /* this track's open reset the block: its close gates it */
    uint32_t gap;                                 /* cycles to wait after a clear start (the retry only) */
} s;

/* ---- the block -------------------------------------------------------------------------------- */

/* Stock's APE_Set_CFG (0x01002502), write for write [trace]: the version bit alone, then with the
 * level's bits (normal: the value in a register; the others OR-ed into a re-read of the register). */
static void ape_set_cfg(void)
{
    HIFI_REG(HIFI_R_APE_CFG) = s.cfg & APE_V3980;
    if ((s.cfg & ~APE_V3980) == k_cfg[0])
        HIFI_REG(HIFI_R_APE_CFG) = s.cfg;
    else
        HIFI_REG(HIFI_R_APE_CFG) = HIFI_REG(HIFI_R_APE_CFG) | (s.cfg & ~APE_V3980);
}

static void ape_clear(void)
{
    b_hifi_fn_busy();
    HIFI_REG(HIFI_R_FUN_CFG) = CLR_COUNT << 15;            /* Hifi_Set_ACC_XFER_Disable(0, 4096, 0) */
    ape_set_cfg();
    HIFI_REG(HIFI_R_APE_CFG) |= APE_CLC;                   /* APE_Clear */
    HIFI_REG(HIFI_R_ACC_CLR) = 3u;                         /* Hifi_Set_ACC_clear */
    HIFI_REG(HIFI_R_DMACR) = HIFI_DMACR_STOCK;             /* Hifi_Set_ACC_Dmacr */
    HIFI_REG(HIFI_R_INTCR) = HIFI_INTCR_STOCK;             /* Hifi_Set_ACC_Intcr */
    HIFI_REG(HIFI_R_FUN_CFG) = CLR_COUNT << 15 | HIFI_FUN_START;   /* Hifi_Set_ACC_XFER_Start(0, 4096, 0) */
    AS->hifi_busy = 0;
    if (s.gap) {                                           /* the retry: stock's time before the first job */
        uint32_t t0 = b_cycles();

        while (b_cycles() - t0 < s.gap)
            ;
    }
}

/* One channel's filters over n blocks, in place. 0, or < 0: timed out (the block is off; d[] lost). */
static int ape_job(int ch, int32_t *d, uint32_t n)
{
    uint32_t fun = (ch ? 1u : 0u) << 29 | n << 15, t0;

    b_hifi_fn_busy();
    HIFI_REG(HIFI_R_FUN_CFG) = fun;
    ape_set_cfg();
    HIFI_REG(HIFI_R_FUN_CFG) = fun | HIFI_FUN_START;
    t0 = b_cycles();
    b_hifi_dma(d, d, n);
    /* the limit: 10 ms, plus 8 cycles a tap (the block's speed is not measured) */
    return b_hifi_fn_wait(t0, (SYST_RVR + 1u) + s.taps * n * 8u, 0, "APE", n);
}

/* ---- what a failure logs (for the next device run) ---------------------------------------------- */

/* Room for two lines in core A's log ring (4 lines; b_msg drops a line when it is full): up to 100 ms. */
static void ape_msg_room(void)
{
    uint32_t t0 = b_cycles();

    while (AS->b_msg_wr - AS->b_msg_rd > 2u && b_cycles() - t0 < 10u * (SYST_RVR + 1u))
        ;
}

/* hifi - software over the job's samples first..first+11 and 32..39 (0 where the job is shorter):
 * where the candidate semantics of docs/HIFI.md 3b.5 differ (sample 3 and on: one 13-bit filter against
 * a 3970-style 10-bit one; 35 and on: the 256-tap filter against a 32-tap one). */
static void ape_dump(const int32_t *hw, const int32_t *sw, uint32_t n, uint32_t first, uint32_t k0)
{
    int32_t v[20];

    for (uint32_t i = 0; i < 20u; i++) {
        uint32_t j = i < 12u ? first + i : 32u + i - 12u;

        v[i] = j < n ? hw[j] - sw[j] : 0;
    }
    b_msg("hifi: APE diff hifi-sw from sample %u: %d %d %d %d %d %d %d %d %d %d %d %d", (unsigned)(k0 + first),
          (int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5], (int)v[6], (int)v[7], (int)v[8],
          (int)v[9], (int)v[10], (int)v[11]);
    b_msg("hifi: APE diff hifi-sw at samples %u..%u: %d %d %d %d %d %d %d %d", (unsigned)(k0 + 32u),
          (unsigned)(k0 + 39u), (int)v[12], (int)v[13], (int)v[14], (int)v[15], (int)v[16], (int)v[17],
          (int)v[18], (int)v[19]);
}

/* ---- self-test -------------------------------------------------------------------------------- */

static uint32_t s_rng[2];

static int32_t residual(int ch, uint32_t k, int hot)
{
    uint32_t r = s_rng[ch];

    r ^= r << 13;
    r ^= r >> 17;
    r ^= r << 5;
    s_rng[ch] = r;
    if (hot)
        return 30000;                             /* a loud constant: the dot product leaves 32 bits */
    if (k % 7u == 3u)
        return 0;
    if (k % 61u == 5u)
        return (int32_t)(r << 13) >> 13;          /* +-2^18 */
    return (int32_t)(r << 20) >> 20;              /* +-2^11 */
}

/* Two frames: 8 + 2 jobs per channel, interleaved left/right as the decoder sends them. 0 = passed. */
static int ape_selftest(struct ape_ctx_t *ctx, int32_t *hw, uint32_t *t_us, int retry)
{
    static const uint16_t k_len[2][8] = { { 256, 256, 256, 256, 256, 256, 256, 256 }, { 200, 56 } };
    static const uint8_t k_jobs[2] = { 8, 2 };
    int32_t *sw = s.chk;
    uint32_t t = 0;

    s_rng[0] = 0x9E3779B9u;
    s_rng[1] = 0x7F4A7C15u;
    for (int f = 0; f < 2; f++) {
        uint32_t k[2] = { 0, 0 };

        demac_filters_init(ctx->compressiontype);
        ape_clear();
        for (int j = 0; j < k_jobs[f]; j++) {
            for (int ch = 0; ch < 2; ch++) {
                uint32_t n = k_len[f][j], t0;

                for (uint32_t i = 0; i < n; i++, k[ch]++)
                    sw[i] = hw[i] = residual(ch, k[ch], f == 0 && ch == 0 && k[ch] >= 600u && k[ch] < 800u);
                t0 = b_cycles();
                if (ape_job(ch, hw, n) < 0)
                    return -1;                    /* timed out: off and logged */
                t += b_cycles() - t0;
                demac_filters_sw(ctx, ch, sw, (int)n);
                for (uint32_t i = 0; i < n; i++) {
                    if (hw[i] != sw[i]) {
                        ape_msg_room();
                        b_msg("hifi: APE self-test%s (level %u, version %s3980) frame %d job %d channel %d sample %u: "
                              "hifi %d, software %d", retry ? " retry" : "", (unsigned)ctx->compressiontype,
                              ctx->fileversion >= 3980 ? ">= " : "< ", f, j, ch, (unsigned)(k[ch] - n + i),
                              (int)hw[i], (int)sw[i]);
                        ape_msg_room();
                        ape_dump(hw, sw, n, i, k[ch] - n);
                        return 1;
                    }
                }
            }
        }
    }
    *t_us = b_us(t);
    return 0;
}

/* ---- the decoder's side ----------------------------------------------------------------------- */

int b_hifi_ape_open(struct ape_ctx_t *ctx)
{
    uint32_t lv = ctx->compressiontype / 1000u, t_us = 0, bit;
    int r;

    memset(&s, 0, sizeof s);
    memset(&b_hifi_fn_stats, 0, sizeof b_hifi_fn_stats);
    if (AS->hifi_mode == AS_HIFI_OFF) {
        b_hifi_fn_off();
        return 0;
    }
    if (lv < 2u || lv > 5u || ctx->compressiontype % 1000u)
        return 0;                                 /* fast (1000) has no filters: stock's "> 1000" */
    s.k = lv - 2u + (ctx->fileversion >= 3980 ? 4u : 0u);
    bit = 1u << s.k;
    if (AS->hifi_ape & bit << 8 || !b_hifi_fn_ready())
        return 0;                                 /* failed this boot, or the block is off */
    s.cfg = k_cfg[lv - 2u] | (ctx->fileversion >= 3980 ? APE_V3980 : 0u);
    s.taps = k_taps[lv - 2u];
    s.chk = b_try_alloc(APE_CHUNK * 4u);
    if (!s.chk)
        return 0;
    /* stock's decoder open, at every track (seg80 0x0100261A -> HIFI_DMA_TO_register 0x01000228): both
     * clocks on, the block and DMA2 through a 1 ms reset, so the block runs one configuration (this
     * track's level and version class, its self-test included) from a reset, as stock's always does */
    b_hifi_power();
    s.powered = 1;
    if (!(AS->hifi_ape & bit)) {
        int32_t *hw = b_try_alloc(APE_CHUNK * 4u);

        if (!hw)
            return 0;
        r = ape_selftest(ctx, hw, &t_us, 0);
        if (r > 0) {
            /* measurement only: once more from another reset, with stock's time between each clear
             * start and the first job (~15 ms [trace]); the level stays in software either way */
            uint32_t t2 = 0;
            int r2;

            b_hifi_power();
            s.gap = 2u * (SYST_RVR + 1u);         /* 20 ms */
            r2 = ape_selftest(ctx, hw, &t2, 1);
            s.gap = 0;
            ape_msg_room();
            b_msg("hifi: APE self-test retry (another reset, 20 ms after each clear start): %s",
                  r2 < 0 ? "timed out" : r2 ? "failed" : "passed");
            if (r2 < 0)
                r = -1;
        }
        b_free(hw);
        memset(&b_hifi_fn_stats, 0, sizeof b_hifi_fn_stats);   /* (the self-test's jobs are not the track's) */
        if (r < 0)
            return 0;                             /* timed out: dead, logged */
        if (r) {
            AS->hifi_ape |= bit << 8;
            ape_msg_room();
            b_msg("hifi: APE self-test failed: level %u in software", (unsigned)ctx->compressiontype);
            return 0;
        }
        AS->hifi_ape |= bit;
        b_msg("hifi: APE self-test passed (level %u, version %s3980, 20 jobs, %u us): NN filters on HIFI%s",
              (unsigned)ctx->compressiontype, ctx->fileversion >= 3980 ? ">= " : "< ",
              (unsigned)t_us, AS->hifi_mode == AS_HIFI_VERIFY ? ", every sample verified" : "");
    }
    s.on = 1;
    return 1;
}

void demac_rechord_frame(struct ape_ctx_t *ctx)
{
    s.lost = 0;
    if (!s.on)
        return;
    ape_clear();
    if (s.frames++)
        s.check[0] = s.check[1] = 1;
    else {                                        /* the track's first frame: more of it */
        uint32_t k = APE_TRACK_TAPS / s.taps;

        s.check[0] = s.check[1] = k > 16u ? 16u : k < 1u ? 1u : k;
    }
}

/* The block fell over mid-frame: the decoder plays silence for what is left of it. */
static void ape_lose(void)
{
    s.on = 0;
    s.lost = 1;
}

int demac_rechord_filters(struct ape_ctx_t *ctx, int ch, int32_t *d, int count)
{
    uint32_t n = (uint32_t)count;
    int check;

    if (s.lost) {
        s.lost_chunk = 1;
        return 1;                                 /* (silenced: the filters' state is gone) */
    }
    if (!s.on)
        return 0;
    if (n > APE_CHUNK) {
        b_hifi_fn_stats.sw++;                     /* (never: b_ape.c decodes APE_CHUNK at most) */
        return 0;
    }
    check = s.check[ch] || AS->hifi_mode == AS_HIFI_VERIFY;
    if (s.check[ch])
        s.check[ch]--;
    if (check)
        memcpy(s.chk, d, n * 4u);
    if (ape_job(ch, d, n) < 0) {
        s.on = 0;
        if (!check) {
            ape_lose();
            s.lost_chunk = 1;
            return 1;
        }
        demac_filters_sw(ctx, ch, s.chk, count);  /* the software has this frame's state: exact */
        memcpy(d, s.chk, n * 4u);
        b_hifi_fn_stats.redone++;
        return 1;
    }
    if (check) {
        /* a later frame's first job: its first APE_CHECK_PART samples (the rest only if they differ) */
        uint32_t m = s.frames > 1u && AS->hifi_mode != AS_HIFI_VERIFY && n > APE_CHECK_PART ? APE_CHECK_PART : n;

        demac_filters_sw(ctx, ch, s.chk, (int)m);
        b_hifi_fn_stats.checked++;
        for (uint32_t i = 0; i < m; i++) {
            if (d[i] != s.chk[i]) {
                if (!b_hifi_fn_stats.mismatch)
                    b_msg("hifi: %s APE job %u (level %u, channel %d, frame %u) sample %u: hifi %d, software %d%s",
                          AS->hifi_mode == AS_HIFI_VERIFY ? "verify:" : "check:", (unsigned)(b_hifi_fn_stats.jobs - 1u),
                          (unsigned)ctx->compressiontype, ch, (unsigned)s.frames, (unsigned)i, (int)d[i],
                          (int)s.chk[i], AS->hifi_mode == AS_HIFI_VERIFY ? " (counting on)" : ": level in software");
                b_hifi_fn_stats.mismatch++;
                b_hifi_fn_stats.redone++;
                if (m < n)
                    demac_filters_sw(ctx, ch, s.chk + m, (int)(n - m));
                if (b_hifi_fn_stats.mismatch == 1u)
                    ape_dump(d, s.chk, n, i, 0);      /* (dropped if core A's ring is full: no wait here) */
                memcpy(d, s.chk, n * 4u);         /* the software's samples play */
                if (AS->hifi_mode != AS_HIFI_VERIFY) {
                    AS->hifi_ape |= 1u << (s.k + 8u);
                    s.on = 0;                     /* (its state is the frame's: the rest goes on exactly) */
                }
                break;
            }
        }
    }
    return 1;
}

int b_hifi_ape_lost(void)
{
    int l = s.lost_chunk;

    s.lost_chunk = 0;
    return l;
}

void b_hifi_ape_close(void)
{
    uint32_t k = s.k;

    if (b_hifi_fn_stats.jobs || AS->hifi_mode != AS_HIFI_AUTO || AS->hifi_ape)
        b_hifi_fn_track_line("APE", AS->hifi_state == AS_HIFI_DEAD ? "dead" : AS->hifi_ape & 1u << (k + 8u) ?
                             "failed" : AS->hifi_ape & 1u << k ? "ok" : "untested");
    s.on = 0;
    if (s.powered && AS->hifi_state != AS_HIFI_DEAD)
        b_hifi_gate();                            /* stock's decoder close (0x0100274E -> 0x01000278) */
    s.powered = 0;
}
