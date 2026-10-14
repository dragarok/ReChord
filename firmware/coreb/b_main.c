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
#include "mad.h"

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

/* libmad's two allocations (main data, IMDCT overlap) come from the arena as well */
void *malloc(size_t n) { return arena_malloc(n, NULL); }
void *calloc(size_t k, size_t n)
{
    void *p = arena_malloc(k * n, NULL);

    if (p)
        memset(p, 0, k * n);
    return p;
}
void free(void *p) { }

/* ---- MP3 (libmad, fixed point) -------------------------------------------- */

#define MP3_BUF 4096u                    /* > 2 frames of 320 kbit/s; refilled as frames are used */

typedef struct {
    struct mad_stream st;
    struct mad_frame fr;
    struct mad_synth sy;
    uint32_t pos;                        /* next sample of sy.pcm to hand out */
    uint32_t trim;                       /* decoded samples still to drop */
    int final;                           /* the buffer holds the end of the track (+ guard) */
    uint8_t buf[MP3_BUF + MAD_BUFFER_GUARD];
} mp3_t;

static mp3_t *s_mp3;

static int mp3_open(void)
{
    s_mp3 = arena_malloc(sizeof *s_mp3, NULL);
    if (!s_mp3)
        return -1;
    mad_stream_init(&s_mp3->st);
    mad_frame_init(&s_mp3->fr);
    mad_synth_init(&s_mp3->sy);
    s_mp3->pos = s_mp3->sy.pcm.length = 0;
    s_mp3->trim = s_trk.trim;
    s_mp3->final = 0;
    return 0;
}

/* Keep the undecoded tail, append what the ring has of this track. */
static void mp3_fill(void)
{
    mp3_t *m = s_mp3;
    uint32_t keep = 0, n;

    if (m->st.buffer && m->st.next_frame) {
        keep = (uint32_t)(m->st.bufend - m->st.next_frame);
        memmove(m->buf, m->st.next_frame, keep);
    }
    n = in_avail();
    if (n > MP3_BUF - keep)
        n = MP3_BUF - keep;
    in_copy(m->buf + keep, n);
    keep += n;
    if (AS->in_rd == s_trk_end) {        /* the end: libmad wants zero guard bytes after it */
        memset(m->buf + keep, 0, MAD_BUFFER_GUARD);
        keep += MAD_BUFFER_GUARD;
        m->final = 1;
    }
    mad_stream_buffer(&m->st, m->buf, keep);
}

static int32_t mp3_word(mad_fixed_t v)
{
    if (v >= MAD_F_ONE)
        v = MAD_F_ONE - 1;
    else if (v < -MAD_F_ONE)
        v = -MAD_F_ONE;
    return (int32_t)((uint32_t)v << 3);  /* Q28 -> left-justified 32-bit word */
}

static uint32_t mp3_read(int32_t *dst, uint32_t want)
{
    mp3_t *m = s_mp3;
    uint32_t got = 0, ch = s_trk.channels;

    while (got < want) {
        struct mad_pcm *pcm = &m->sy.pcm;

        if (m->pos < pcm->length) {
            uint32_t n = pcm->length - m->pos, r = pcm->channels > 1;

            if (m->trim) {
                if (n > m->trim)
                    n = m->trim;
                m->trim -= n;
                m->pos += n;
                continue;
            }
            if (n > want - got)
                n = want - got;
            for (uint32_t i = 0; i < n; i++, m->pos++) {
                dst[(got + i) * ch] = mp3_word(pcm->samples[0][m->pos]);
                if (ch > 1)
                    dst[(got + i) * ch + 1] = mp3_word(pcm->samples[r][m->pos]);
            }
            got += n;
            continue;
        }
        if (!m->st.buffer || m->st.error == MAD_ERROR_BUFLEN) {
            if (m->final)
                break;                   /* decoded everything */
            mp3_fill();
            m->st.error = MAD_ERROR_NONE;
        }
        if (mad_frame_decode(&m->fr, &m->st)) {
            if (MAD_RECOVERABLE(m->st.error) || m->st.error == MAD_ERROR_BUFLEN)
                continue;
            m->final = 1;                /* unrecoverable: end the track */
            m->st.error = MAD_ERROR_BUFLEN;
            continue;
        }
        mad_synth_frame(&m->sy, &m->fr);
        m->pos = 0;
    }
    return got;
}

/* ---- tracks ------------------------------------------------------------- */

static int track_open(void)
{
    s_trk = AS->q[AS->q_rd % AS_QUEUE];
    s_trk_end = AS->in_rd + s_trk.bytes;
    AS->in_rd += s_trk.skip;                 /* alignment filler */
    s_trk_start = AS->in_rd;
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
    } else if (s_trk.codec == AS_CODEC_MP3) {
        if (mp3_open() != 0 || s_trk.channels < 1 || s_trk.channels > 2) {
            AS->err = 3;
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

/* End the DMA chain after the block being written: shorten it to what it holds
 * (one silent frame if it is empty, as a block cannot be empty) and give its LLI no
 * successor. The DMA plays it and stops; nothing stale or padded is ever heard. */
static void end_chain(void)
{
    volatile uint32_t *l = (volatile uint32_t *)AS_LLI + (AS->pcm_wr % AS_PCM_BLOCKS) * LLI_WORDS;

    if (!s_fill) {
        cur_block()[0] = cur_block()[1] = 0;
        s_fill = 1;
    }
    l[2] = 0;                                        /* LLP */
    l[3] &= ~(3u << 27);                             /* LLP_DST_EN, LLP_SRC_EN */
    l[4] = s_fill * 2u;                              /* block size in words, DONE clear */
    block_done();
    AS->pcm_end = AS->pcm_wr;
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
        if (AS->state == AS_B_ENDED || AS->state == AS_B_RATE)
            return 0;                                /* waiting for A */
        if (AS->q_rd != AS->q_wr) {
            if (AS->rate && AS->q[AS->q_rd % AS_QUEUE].rate != AS->rate) {
                end_chain();                         /* rate change: A drains and re-clocks */
                AS->state = AS_B_RATE;
                return 1;
            }
            if (AS->in_wr - AS->in_rd < 64u && AS->q[AS->q_rd % AS_QUEUE].bytes > 64u)
                return 0;                            /* header not there yet */
            if (track_open() != 0) {                 /* unplayable: skip it, keep the stream */
                AS->q_rd++;
                AS->in_rd = s_trk_end;
                AS->done_seq = s_trk.seq;
                return 1;
            }
            AS->state = AS_B_PLAYING;
        } else if (AS->in_closed) {
            end_chain();                             /* end of stream */
            AS->state = AS_B_ENDED;
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
    } else if (s_trk.codec == AS_CODEC_MP3) {
        got = mp3_read(dst, want);
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
        } else if (AS->cmd == AS_CMD_RESTART) {        /* A re-clocked and re-armed the DMA */
            AS->pcm_wr = AS->pcm_played = AS->pcm_end = 0;
            s_fill = 0;
            AS->state = AS_B_PLAYING;
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
