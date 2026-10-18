/* Core B: FLAC with dr_flac (32-bit output = the I2S word format already).
 *
 * LPC subframes of order 3..32 (stock's split: its orders 1-2 stay in software too) go to the HIFI
 * accelerator when RECHORD.CFG hifi= allows it (b_hifi.c, docs/HIFI.md): dr_flac decodes the warm-up
 * and the residuals into s_res, the block restores the samples into dr_flac's buffer, and dr_flac's own
 * prediction checks them: three samples a job (hifi=auto), or every one (hifi=verify, where the
 * software's sample is what plays). A job that fails, or a sample that disagrees, and the subframe is
 * redone in software from s_res. Each track ends with a line in core A's log: core B's cycles per
 * second of audio, and the HIFI's share (the device's measure of the benefit). */
#include "b_rt.h"
#include "b_hifi.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#define DRFLAC_ASSERT(x) ((void)0)
#define DRFLAC_MALLOC(sz)      ((void *)0)        /* every allocation goes to the arena */
#define DRFLAC_REALLOC(p, sz)  ((void *)0)
#define DRFLAC_FREE(p)         ((void)0)
#define DRFLAC_RECHORD_LPC
#include "player/codecs/dr_flac/dr_flac.h"

static drflac *s_flac;
static drflac_int32 *s_res;                      /* the subframe's warm-up + residuals: HIFI's input */
static uint32_t s_res_cap;
static struct {
    uint32_t cyc, frames;                        /* decode cycles, frames */
    uint32_t lpc, hw;                            /* LPC subframes of order >= 3; those the block restored */
    uint32_t orders;                             /* bit n: an LPC subframe of order n (n >= 3; 32 = bit 0) */
} s_trk;

static size_t flac_read_cb(void *u, void *out, size_t n)
{
    return b_in_read(out, (uint32_t)n);
}

static drflac_bool32 flac_seek_cb(void *u, int offset, drflac_seek_origin origin)
{
    uint8_t sink[64];
    uint32_t cur = b_in_pos(), to;

    to = origin == DRFLAC_SEEK_SET ? (uint32_t)offset : cur + (uint32_t)offset;
    if (to < cur || to > b_trk_end - b_trk_start)
        return DRFLAC_FALSE;             /* forward only: the stream has no seek table to use */
    while (cur < to) {
        uint32_t k = to - cur > sizeof sink ? sizeof sink : to - cur;

        if (b_in_read(sink, k) != k)
            return DRFLAC_FALSE;
        cur += k;
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_tell_cb(void *u, drflac_int64 *pos)
{
    *pos = b_in_pos();
    return DRFLAC_TRUE;
}

static void *a_malloc(size_t n, void *u) { return b_alloc(n); }
static void *a_realloc(void *p, size_t n, void *u) { return b_realloc(p, n); }
static void a_free(void *p, void *u) { b_free(p); }
static const drflac_allocation_callbacks s_alloc = { NULL, a_malloc, a_realloc, a_free };

/* ---- LPC on the HIFI accelerator (dr_flac's DRFLAC_RECHORD_LPC hook) ---------- */

static drflac_int32 *drflac_rechord_lpc_buf(drflac_uint32 n, drflac_uint32 bps, drflac_uint32 order,
                                            drflac_uint32 prec)
{
    if (order < 3)
        return NULL;
    s_trk.lpc++;
    s_trk.orders |= 1u << (order & 31u);
    if (!s_res || n > s_res_cap || !b_hifi_wants(n, order))
        return NULL;
    return s_res;
}

/* dr_flac's own prediction for sample i (the software path's arithmetic, narrow or wide). */
static drflac_int32 sw_sample(const drflac_int32 *res, drflac_int32 *out, drflac_uint32 i, drflac_uint32 order,
                              drflac_int32 shift, const drflac_int32 *coef, int wide)
{
    return (drflac_int32)((drflac_uint32)res[i] + (drflac_uint32)(wide ?
        drflac__calculate_prediction_64(order, shift, coef, out + i) :
        drflac__calculate_prediction_32(order, shift, coef, out + i)));
}

static void drflac_rechord_lpc_restore(const drflac_int32 *res, drflac_int32 *out, drflac_uint32 n,
                                       drflac_uint32 bps, drflac_uint32 order, drflac_int32 shift,
                                       drflac_uint32 prec, const drflac_int32 *coef)
{
    int wide = drflac__use_64_bit_prediction(bps, order, prec);       /* dr_flac's arithmetic */
    int cls = b_hifi_wide(bps, order, (const int32_t *)coef);          /* the block's class */
    drflac_uint32 i;

    if (b_hifi_class_ok(cls) &&
        b_hifi_flac((const int32_t *)res, (int32_t *)out, n, order, (uint32_t)shift, (const int32_t *)coef) == 0) {
        if (b_hifi_mode() == AS_HIFI_VERIFY) {
            /* every sample, in order: each check stands on samples already checked (or corrected),
             * so the output is the software's whatever the block did */
            int told = 0;

            for (i = 0; i < n; i++) {
                drflac_int32 want = i < order ? res[i] : sw_sample(res, out, i, order, shift, coef, wide);

                if (out[i] != want) {
                    if (!told++)
                        b_hifi_mismatch(i, out[i], want, order, (uint32_t)shift, cls);
                    out[i] = want;
                }
            }
            s_trk.hw += !told;
            return;
        } else {
            /* the last warm-up sample, the first predicted one, one in the middle, the last */
            const drflac_uint32 at[4] = { order - 1, order, order + (n - order) / 2, n - 1 };
            int k;

            for (k = 0; k < 4; k++) {
                drflac_int32 want = at[k] < order ? res[at[k]] : sw_sample(res, out, at[k], order, shift, coef, wide);

                if (out[at[k]] != want) {
                    b_hifi_mismatch(at[k], out[at[k]], want, order, (uint32_t)shift, cls);
                    break;
                }
            }
            if (k == 4) {
                s_trk.hw++;
                return;
            }
        }
    }
    b_hifi_stats.sw_subframes += b_hifi_class_ok(cls);   /* the block failed it: the software from res[] */
    for (i = 0; i < order; i++)
        out[i] = res[i];
    for (; i < n; i++)
        out[i] = sw_sample(res, out, i, order, shift, coef, wide);
}

/* ---- the codec ------------------------------------------------------------ */

int b_flac_open(void)
{
    memset(&s_trk, 0, sizeof s_trk);
    s_res = NULL;
    s_flac = drflac_open(flac_read_cb, flac_seek_cb, flac_tell_cb, NULL, &s_alloc);
    if (!s_flac || s_flac->channels > 2)
        return -1;
    if (b_hifi_open()) {                         /* (its self-test's buffers are free again here) */
        s_res_cap = s_flac->maxBlockSizeInPCMFrames;
        if (s_res_cap == 0 || s_res_cap > B_HIFI_MAX_COUNT)
            s_res_cap = B_HIFI_MAX_COUNT;
        s_res = b_try_alloc(s_res_cap * sizeof *s_res);
    }
    memset(&b_hifi_stats, 0, sizeof b_hifi_stats);   /* (the self-test's jobs are not the track's) */
    return 0;
}

uint32_t b_flac_read(int32_t *dst, uint32_t frames)
{
    uint32_t t0 = b_cycles(), got;

    got = (uint32_t)drflac_read_pcm_frames_s32(s_flac, frames, (drflac_int32 *)dst);
    s_trk.cyc += b_cycles() - t0;
    s_trk.frames += got;
    return got;
}

/* The track's line: core B's decode cycles per second of audio (the clock FLAC needs), and what
 * the block did. */
void b_flac_close(void)
{
    static const char *const k_mode[] = { "auto", "off", "verify" };
    static const char *const k_state[] = { "untested", "ok", "narrow only", "failed", "dead" };
    const b_hifi_stats_t *h = &b_hifi_stats;
    uint32_t mhz100, wait100;

    if (!s_trk.frames || !b_trk.rate)
        return;
    mhz100 = (uint32_t)((uint64_t)s_trk.cyc * b_trk.rate / s_trk.frames / 10000u);
    wait100 = (uint32_t)((uint64_t)h->wait_cyc * b_trk.rate / s_trk.frames / 10000u);
    b_msg("flac: track %u, %u-bit: core B %u.%02u M cycles/s, LPC %u of %u subframes on HIFI (wait %u.%02u M/s), "
          "orders %x", (unsigned)b_trk.seq, (unsigned)b_trk.bits, (unsigned)(mhz100 / 100u),
          (unsigned)(mhz100 % 100u), (unsigned)s_trk.hw, (unsigned)s_trk.lpc, (unsigned)(wait100 / 100u),
          (unsigned)(wait100 % 100u), (unsigned)s_trk.orders);
    if (h->jobs || AS->hifi_mode != AS_HIFI_AUTO)
        b_msg("hifi: track %u: hifi=%s, block %s, %u jobs, %u mismatched, %u redone in software, "
              "longest job %u us, %u polls", (unsigned)b_trk.seq, k_mode[AS->hifi_mode % 3u],
              k_state[AS->hifi_state % 5u], (unsigned)h->jobs, (unsigned)h->mismatch_jobs,
              (unsigned)h->sw_subframes, (unsigned)b_us(h->wait_max_cyc), (unsigned)h->polls);
}
