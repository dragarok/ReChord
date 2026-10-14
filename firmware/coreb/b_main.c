/*
 * Core B: the decoder core (firmware/audio/audio_shared.h has the contract).
 *
 * Loop: while the PCM ring has a free block and input is available, decode into
 * it; otherwise sleep (WFI). SysTick (100 Hz) recycles blocks the DMA finished
 * (DONE written back into their LLIs), keeps the heartbeat and measures the
 * busy share, from which the core picks its own clock divider.
 *
 * Gapless: when a track ends mid-block, the next queued track continues filling
 * the same block, so the PCM stream never has a seam.
 */
#include <stddef.h>
#include <stdint.h>

#include "audio/audio_shared.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_SIMD
#define DR_FLAC_NO_CRC
#define DRFLAC_ASSERT(x) ((void)0)
#define DRFLAC_MALLOC(sz)      ((void *)0)        /* every allocation goes to the arena */
#define DRFLAC_REALLOC(p, sz)  ((void *)0)
#define DRFLAC_FREE(p)         ((void)0)
#include "player/codecs/dr_flac/dr_flac.h"

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define TICK_HZ   100u

#define LLI_WORDS 7u
#define LLI_DONE  (1u << 12)

/* ---- time base, idle accounting, clock choice ---------------------------- */

static uint32_t s_reload;
static volatile uint32_t s_idle_cyc, s_win;

static void systick_start(void)
{
    s_reload = AS->b_hz / TICK_HZ - 1u;
    SYST_CSR = 0;
    SYST_RVR = s_reload;
    SYST_CVR = 0;
    SYST_CSR = 7u;
}

static void recycle_blocks(void)
{
    volatile uint32_t *lli = (volatile uint32_t *)AS_LLI;

    for (unsigned n = 0; n < AS_PCM_BLOCKS; n++) {
        volatile uint32_t *ctlh = &lli[(AS->pcm_played % AS_PCM_BLOCKS) * LLI_WORDS + 4];

        if (!(*ctlh & LLI_DONE))
            break;
        *ctlh &= ~LLI_DONE;
        AS->pcm_played++;
    }
}

/* B picks its divider from its own load; core A applies it (b_div_req). */
static void choose_clock(unsigned load_pm)
{
    unsigned div = AS->b_div;

    if (load_pm > 600u && div > 1u)
        div = div > 2u ? div / 2u : 1u;          /* well short of time: double */
    else if (load_pm < 200u && div < 8u)
        div = div * 2u > 8u ? 8u : div * 2u;     /* plenty of slack: halve */
    AS->b_div_req = div;
}

void SysTick_Handler(void)
{
    AS->ticks++;
    recycle_blocks();
    if (++s_win == TICK_HZ) {
        uint32_t cyc = (s_reload + 1u) * TICK_HZ, idle = s_idle_cyc;

        s_win = 0;
        s_idle_cyc = 0;
        AS->b_load_pm = idle >= cyc ? 0u : 1000u - (uint32_t)((uint64_t)idle * 1000u / cyc);
        choose_clock(AS->b_load_pm);
    }
}

static void cpu_idle(void)
{
    uint32_t v0, v1, t0, t1;

    __asm__ volatile ("cpsid i" ::: "memory");
    t0 = AS->ticks;
    v0 = SYST_CVR;
    __asm__ volatile ("dsb\n wfi" ::: "memory");
    v1 = SYST_CVR;
    t1 = AS->ticks;
    /* down-counter: same tick -> v0 - v1; a pending wrap (IRQ not yet taken) -> + reload */
    s_idle_cyc += v0 >= v1 && t0 == t1 ? v0 - v1 : v0 + (s_reload + 1u) - v1;
    __asm__ volatile ("cpsie i" ::: "memory");
}

/* ---- decoder input: the track's span of the input ring ------------------- */

static as_track_t s_trk;
static uint32_t s_trk_start, s_trk_end;
static drflac *s_flac;
static int s_open;

static uint32_t in_avail(void)
{
    uint32_t a = AS->in_wr - AS->in_rd, t = s_trk_end - AS->in_rd;

    return a < t ? a : t;
}

static void in_copy(void *out, uint32_t n)
{
    uint32_t off = AS->in_rd % AS_IN_SIZE, k = AS_IN_SIZE - off;
    const uint8_t *ring = (const uint8_t *)AS_IN_RING;

    if (k > n)
        k = n;
    memcpy(out, ring + off, k);
    if (n > k)
        memcpy((uint8_t *)out + k, ring, n - k);
    AS->in_rd += n;
}

static size_t flac_read(void *u, void *out, size_t n)
{
    uint32_t a;

    while ((a = in_avail()) < n && AS->in_rd + a < s_trk_end && !AS->cmd)
        cpu_idle();                      /* core A refills every 10 ms */
    if (n > a)
        n = a;
    in_copy(out, n);
    return n;
}

static drflac_bool32 flac_seek(void *u, int offset, drflac_seek_origin origin)
{
    uint8_t sink[64];
    uint32_t cur = AS->in_rd - s_trk_start, to;

    to = origin == DRFLAC_SEEK_SET ? (uint32_t)offset : cur + (uint32_t)offset;
    if (to < cur || to > s_trk_end - s_trk_start)
        return DRFLAC_FALSE;             /* forward only: the stream has no seek table to use */
    while (cur < to) {
        uint32_t k = to - cur > sizeof sink ? sizeof sink : to - cur;

        if (flac_read(u, sink, k) != k)
            return DRFLAC_FALSE;
        cur += k;
    }
    return DRFLAC_TRUE;
}

static drflac_bool32 flac_tell(void *u, drflac_int64 *pos)
{
    *pos = AS->in_rd - s_trk_start;
    return DRFLAC_TRUE;
}

/* dr_flac allocates once per open; a bump arena reset at close is enough. */
static uint32_t s_arena_used;

static void *arena_malloc(size_t n, void *u)
{
    void *p = (void *)(AS_ARENA + s_arena_used);

    n = (n + 7u) & ~7u;
    if (s_arena_used + n > AS_ARENA_SIZE)
        return NULL;
    s_arena_used += n;
    return p;
}

static void *arena_realloc(void *p, size_t n, void *u)
{
    void *q = arena_malloc(n, u);

    if (q && p)
        memcpy(q, p, n);                 /* only growth happens; over-copy stays in the arena */
    return q;
}

static void arena_free(void *p, void *u) { }

static const drflac_allocation_callbacks s_alloc = { NULL, arena_malloc, arena_realloc, arena_free };

/* ---- tracks ------------------------------------------------------------- */

static int track_open(void)
{
    s_trk = AS->q[AS->q_rd % AS_QUEUE];
    s_trk_start = AS->in_rd;
    s_trk_end = s_trk_start + s_trk.bytes;
    AS->cur_seq = s_trk.seq;
    AS->cur_frames = 0;
    s_arena_used = 0;
    s_flac = NULL;
    if (s_trk.codec == AS_CODEC_FLAC) {
        s_flac = drflac_open(flac_read, flac_seek, flac_tell, NULL, &s_alloc);
        if (!s_flac || s_flac->channels > 2) {
            AS->err = 1;
            return -1;
        }
    } else if (s_trk.codec != AS_CODEC_WAV || (s_trk.bits != 16 && s_trk.bits != 24) ||
               s_trk.channels < 1 || s_trk.channels > 2) {
        AS->err = 2;
        return -1;
    }
    s_open = 1;
    return 0;
}

static void track_close(void)
{
    s_open = 0;
    s_flac = NULL;
    AS->in_rd = s_trk_end;               /* skip whatever the decoder left unread */
    AS->done_seq = s_trk.seq;
    AS->q_rd++;
}

/* WAV: raw little-endian PCM to left-justified 32-bit I2S words. */
static uint32_t wav_read(int32_t *dst, uint32_t frames)
{
    uint32_t bps = s_trk.bits / 8u, fsz = bps * s_trk.channels, n = in_avail() / fsz;
    uint8_t buf[192];

    if (n > frames)
        n = frames;
    for (uint32_t done = 0; done < n; ) {
        uint32_t k = n - done > sizeof buf / fsz ? sizeof buf / fsz : n - done;

        in_copy(buf, k * fsz);
        for (uint32_t i = 0; i < k * s_trk.channels; i++) {
            const uint8_t *p = buf + i * bps;
            int32_t v = bps == 2 ? (int32_t)((uint32_t)p[0] << 16 | (uint32_t)p[1] << 24)
                                 : (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            dst[(done + i / s_trk.channels) * s_trk.channels + i % s_trk.channels] = v;
        }
        done += k;
    }
    return n;
}

/* ---- PCM ring ----------------------------------------------------------- */

static uint32_t s_fill;                  /* frames already in the block being written */

static int32_t *cur_block(void)
{
    return (int32_t *)(AS_PCM_RING + (AS->pcm_wr % AS_PCM_BLOCKS) * AS_BLOCK_BYTES);
}

static void block_done(void)
{
    s_fill = 0;
    AS->pcm_wr++;
}

static void pad_silence(void)
{
    int32_t *b = cur_block();

    memset(b + 2 * s_fill, 0, (AS_BLOCK_FRAMES - s_fill) * 8u);
    block_done();
}

/* Enough input to decode without stalling mid-frame: the rest of the track,
 * or 32 KiB (more than a 4096-frame 24-bit stereo FLAC frame). */
static int input_ready(void)
{
    uint32_t want = s_trk_end - AS->in_rd;

    return in_avail() >= (want < 0x8000u ? want : 0x8000u);
}

/* One step of work; 0 = nothing to do now. */
static int produce(void)
{
    uint32_t want, got;
    int32_t *dst;

    if (!AS->run || AS->pcm_wr - AS->pcm_played >= AS_PCM_BLOCKS)
        return 0;                                    /* stopped, or ring full */
    if (!s_open) {
        if (AS->q_rd != AS->q_wr) {
            if (AS->in_wr - AS->in_rd < 64u && AS->q[AS->q_rd % AS_QUEUE].bytes > 64u)
                return 0;                            /* header not there yet */
            if (track_open() != 0) {
                AS->state = AS_B_ERROR;
                AS->q_rd++;
                AS->in_rd = s_trk_end;
                return 1;
            }
            AS->state = AS_B_PLAYING;
        } else if (AS->in_closed) {
            if (s_fill || !AS->pcm_end) {            /* end of stream: finish the block */
                pad_silence();
                if (!AS->pcm_end)
                    AS->pcm_end = AS->pcm_wr;
                return 1;
            }
            pad_silence();                           /* keep the ring silent until A stops */
            if (AS->pcm_played >= AS->pcm_end)
                AS->state = AS_B_DRAINED;
            return 1;
        } else {
            return 0;
        }
    }
    if (!input_ready())
        return 0;
    dst = cur_block() + 2 * s_fill;
    want = AS_BLOCK_FRAMES - s_fill;
    if (s_trk.frames && s_trk.frames - AS->cur_frames < want)
        want = s_trk.frames - AS->cur_frames;
    if (s_trk.codec == AS_CODEC_FLAC) {
        got = (uint32_t)drflac_read_pcm_frames_s32(s_flac, want, (drflac_int32 *)dst);
    } else {
        got = wav_read(dst, want);
    }
    if (s_trk.channels == 1) {                       /* mono: L = R, expand in place */
        for (uint32_t i = got; i-- > 0; )
            dst[2 * i] = dst[2 * i + 1] = dst[i];
    }
    s_fill += got;
    AS->cur_frames += got;
    if (got == 0 || (s_trk.frames && AS->cur_frames >= s_trk.frames))
        track_close();                               /* the next track fills the same block */
    if (s_fill == AS_BLOCK_FRAMES)
        block_done();
    return 1;
}

static void stop_all(void)
{
    if (s_open)
        track_close();
    AS->q_rd = AS->q_wr;
    AS->in_rd = AS->in_wr;
    s_fill = 0;
    AS->state = AS_B_READY;
}

int b_main(void)
{
    unsigned div_seen = AS->b_div;

    systick_start();
    AS->state = AS_B_READY;
    for (;;) {
        if (AS->cmd == AS_CMD_STOP) {
            stop_all();
            AS->cmd = AS_CMD_NONE;
        }
        if (AS->b_div != div_seen) {                 /* core A changed our clock */
            div_seen = AS->b_div;
            systick_start();
        }
        if (!produce())
            cpu_idle();
    }
}
