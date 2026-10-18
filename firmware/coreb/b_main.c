/*
 * Core B: the decoder core (firmware/audio/audio_shared.h has the contract).
 *
 * Loop: while the PCM ring has a free block and input is available, decode into
 * it; otherwise sleep (WFI). SysTick (100 Hz) recycles blocks the DMA finished
 * (DONE written back into their LLIs), keeps the heartbeat and measures the
 * busy share, from which the core picks its own clock divider.
 *
 * Gapless: when a track ends mid-block, the next queued track continues filling
 * the same block, so the PCM stream never has a seam. When the next track needs
 * another sample rate, or a decoder this image does not have, the DMA chain ends
 * after the last block and core A re-clocks / loads the other image (state RATE).
 *
 * The decoders (b_codecs_*.c) only see b_rt.h.
 */
#include <stddef.h>
#include <stdint.h>

#include <string.h>

#include "b_rt.h"

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define TICK_HZ   100u

#define LLI_WORDS 7u
#define LLI_DONE  (1u << 12)

/* ---- time base, idle accounting, clock choice ---------------------------- */

static uint32_t s_reload;
static void stack_check(void);
static volatile uint32_t s_idle_cyc, s_win;

static void systick_start(void)
{
    s_reload = AS->b_hz / TICK_HZ - 1u;
    SYST_CSR = 0;
    SYST_RVR = s_reload;
    SYST_CVR = 0;
    SYST_CSR = 7u;
}

static volatile uint32_t s_resync;      /* SysTick moved pcm_wr on: drop the block being written */

/* A block the DMA has played is zeroed, so if B falls behind, the DMA (which
 * goes round the ring regardless) plays silence, never old audio. If it finished
 * the block B was still writing (an underrun), B gives that block and the next one
 * up (the DMA is already in it) and goes on two blocks later: what it had in the
 * block is lost, the rest stays in order. */
static void recycle_blocks(void)
{
    volatile uint32_t *lli = (volatile uint32_t *)AS_LLI;

    for (unsigned n = 0; n < AS_PCM_BLOCKS; n++) {
        uint32_t i = AS->pcm_played % AS_PCM_BLOCKS;
        volatile uint32_t *ctlh = &lli[i * LLI_WORDS + 4];
        uint32_t *p = (uint32_t *)(AS_PCM_RING + i * AS_BLOCK_BYTES);

        if (!(*ctlh & LLI_DONE))
            break;
        *ctlh &= ~LLI_DONE;
        for (uint32_t k = 0; k < AS_BLOCK_BYTES / 4u; k += 8u)
            p[k] = p[k + 1] = p[k + 2] = p[k + 3] = p[k + 4] = p[k + 5] = p[k + 6] = p[k + 7] = 0;
        if (AS->pcm_played == AS->pcm_wr) {
            AS->pcm_wr += 2u;
            AS->underruns++;
            s_resync = 1;
        }
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
    /* falling behind (the PCM ring under half full while the DMA runs): full speed now, not at the
     * next 1 s load window (a slow clock kept from a pause or a quiet passage drained the ring
     * first; OURS_AUDIT 3.3a). Core A applies it from its own SysTick. */
    if (AS->state == AS_B_PLAYING && AS->b_div > 1u && AS->pcm_wr >= AS_PCM_BLOCKS &&
        AS->pcm_wr - AS->pcm_played < AS_PCM_BLOCKS / 2u)
        AS->b_div_req = 1;
    if (++s_win == TICK_HZ) {
        stack_check();
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

as_track_t b_trk;
uint32_t b_trk_start, b_trk_end;
uint32_t b_in_need;
static const b_codec_t *s_dec;           /* NULL: no track open */
static uint32_t s_drop;                  /* frames still to drop at the track's start (b_trk.drop) */

uint32_t b_in_avail(void)
{
    uint32_t a = AS->in_wr - AS->in_rd, t = b_trk_end - AS->in_rd;

    return a < t ? a : t;
}

void b_in_copy(void *out, uint32_t n)
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

uint32_t b_in_read(void *out, uint32_t n)
{
    uint8_t *o = out;
    uint32_t got = 0;

    /* copy what is there and wait for more: a read may be larger than the ring
     * (a WavPack block at 24/96 is ~80 KiB) */
    while (got < n) {
        uint32_t a = b_in_avail();

        if (a) {
            if (a > n - got)
                a = n - got;
            b_in_copy(o + got, a);
            got += a;
            continue;
        }
        if (AS->in_rd >= b_trk_end || AS->cmd)
            break;                       /* the end of the track, or A wants us */
        cpu_idle();                      /* core A refills every 10 ms */
    }
    return got;
}

uint32_t b_in_pos(void)
{
    return AS->in_rd - b_trk_start;
}

/* Decoder heap: the arena from the end of .bss to the stack reserve (coreb.ld),
 * emptied when a track opens. First fit over an implicit list of blocks (8-byte
 * header: size incl. header, bit 0 = used); adjacent free blocks merge while
 * malloc walks. Decoders allocate a few dozen blocks, so the walk is short. */
extern uint8_t _b_heap[], _b_stack_limit[];
static uint32_t s_heap_used;

static void heap_reset(void)
{
    uint32_t *h = (uint32_t *)_b_heap;

    h[0] = (uint32_t)(_b_stack_limit - _b_heap) & ~7u;     /* one free block */
    s_heap_used = 0;
}

/* ---- giving up on a track from inside a decoder ------------------------- */

/* setjmp/longjmp of our own (no libc here): r4-r11, sp, lr. */
typedef uint32_t b_jmp_buf[10];

__attribute__((naked, returns_twice)) static int b_setjmp(b_jmp_buf b)
{
    (void)b;
    __asm__ volatile ("mov ip, sp\n stmia r0!, {r4-r11, ip, lr}\n movs r0, #0\n bx lr");
}

__attribute__((naked, noreturn)) static void b_longjmp(b_jmp_buf b, int v)
{
    (void)b;
    (void)v;
    __asm__ volatile ("ldmia r0!, {r4-r11, ip, lr}\n mov sp, ip\n movs r0, r1\n bne 1f\n movs r0, #1\n"
                      "1: bx lr");
}

static b_jmp_buf s_jmp;
static volatile int s_jmp_armed;

void b_abort(uint32_t code)
{
    AS->aborts++;
    AS->abort_code = code;
    if (s_jmp_armed) {
        s_jmp_armed = 0;
        b_longjmp(s_jmp, 1);
    }
    for (;;)                              /* outside a decoder call: A's supervisor reloads B */
        __asm__ volatile ("wfi");
}

void *b_alloc(size_t n)
{
    uint8_t *p = _b_heap, *end = _b_heap + ((uint32_t)(_b_stack_limit - _b_heap) & ~7u);
    uint32_t need = ((uint32_t)n + 8u + 7u) & ~7u;

    while (p < end) {
        uint32_t *h = (uint32_t *)p, sz = h[0] & ~7u;

        if (!(h[0] & 1u)) {
            while (p + sz < end && !(*(uint32_t *)(p + sz) & 1u))   /* merge free neighbours */
                sz += *(uint32_t *)(p + sz) & ~7u;
            h[0] = sz;
            if (sz >= need) {
                if (sz - need >= 16u) {                              /* split */
                    *(uint32_t *)(p + need) = sz - need;
                    sz = need;
                }
                h[0] = sz | 1u;
                s_heap_used += sz;
                if (s_heap_used > AS->b_heap_peak)
                    AS->b_heap_peak = s_heap_used;
                return p + 8;
            }
        }
        p += sz;
    }
    b_abort(0xA110Cu);                           /* out of heap: never hand a decoder NULL */
}

void b_free(void *q)
{
    if (q) {
        uint32_t *h = (uint32_t *)((uint8_t *)q - 8);

        s_heap_used -= h[0] & ~7u;
        h[0] &= ~1u;
    }
}

void *b_realloc(void *q, size_t n)
{
    uint32_t have = q ? (((uint32_t *)((uint8_t *)q - 8))[0] & ~7u) - 8u : 0u;
    void *r;

    if (q && n <= have)
        return q;
    r = b_alloc(n);
    if (r && q) {
        memcpy(r, q, have);
        b_free(q);
    }
    return r;
}

/* Stack low-water mark: b_reset painted the reserve; count untouched words. */
#define STACK_PAINT 0x5A5A5A5Au

static void stack_check(void)
{
    const uint32_t *p = (const uint32_t *)_b_stack_limit;
    uint32_t n = 0;

    while (p[n] == STACK_PAINT)
        n++;
    AS->b_stack_free = n * 4u;
}

void *b_calloc(size_t n)
{
    void *p = b_alloc(n);

    if (p)
        memset(p, 0, n);
    return p;
}

static const b_codec_t *codec_find(uint32_t codec)
{
    for (const b_codec_t *c = b_codecs; c->codec; c++)
        if (c->codec == codec)
            return c;
    return NULL;
}

/* ---- tracks ------------------------------------------------------------- */

/* The decoder's open/read, from which b_abort() returns here: -1 = gave up. */
static int guarded_open(void)
{
    int r;

    if (b_setjmp(s_jmp))
        return -1;
    s_jmp_armed = 1;
    r = s_dec->open();
    s_jmp_armed = 0;
    return r;
}

static int guarded_read(int32_t *dst, uint32_t want, uint32_t *got)
{
    if (b_setjmp(s_jmp))
        return -1;
    s_jmp_armed = 1;
    *got = s_dec->read(dst, want);
    s_jmp_armed = 0;
    return 0;
}

static int track_open(void)
{
    b_trk = AS->q[AS->q_rd % AS_QUEUE];
    b_trk_end = AS->in_rd + b_trk.bytes;
    AS->in_rd += b_trk.skip;                 /* alignment filler */
    b_trk_start = AS->in_rd;
    AS->cur_seq = b_trk.seq;
    AS->cur_frames = 0;
    s_drop = b_trk.drop;
    heap_reset();
    s_dec = codec_find(b_trk.codec);
    b_in_need = 0x8000u;
    if (!s_dec || b_trk.channels < 1 || b_trk.channels > 2 || guarded_open() != 0) {
        AS->err = b_trk.codec;
        s_dec = NULL;
        return -1;
    }
    return 0;
}

static uint32_t s_discard_seq, s_discard_to;   /* dropping the rest of a track's input */

/* The decoder is done with the track: skip whatever it left unread. If A has not
 * written all of it yet, drop it as it comes (produce()) and ask A to stop reading. */
static void discard_rest(void)
{
    if (AS->in_wr - AS->in_rd >= b_trk_end - AS->in_rd) {
        AS->in_rd = b_trk_end;
        return;
    }
    s_discard_seq = b_trk.seq;
    s_discard_to = b_trk_end;
    AS->abandon_seq = b_trk.seq;
}

/* 1 while the rest of an abandoned track is still being dropped. */
static int discarding(void)
{
    uint32_t end, avail, need;

    if (!s_discard_seq)
        return 0;
    end = AS->cut_seq == s_discard_seq ? AS->cut_end : s_discard_to;
    avail = AS->in_wr - AS->in_rd;
    need = end - AS->in_rd;
    AS->in_rd += avail < need ? avail : need;
    if (AS->in_rd != end)
        return 1;
    s_discard_seq = 0;
    return 0;
}

static void track_close(void)
{
    s_dec = NULL;
    discard_rest();
    AS->done_seq = b_trk.seq;
    AS->q_rd++;
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
 * successor. The DMA plays it and stops; nothing stale or padded is ever heard.
 * A track that ends exactly on a block boundary leaves its last block full and
 * still open (produce()), so it is that block that ends the chain, not an empty one. */
static void end_chain(void)
{
    volatile uint32_t *l;

    __asm__ volatile ("cpsid i" ::: "memory");
    if (s_resync) {
        s_resync = 0;
        s_fill = 0;
    }
    if (!s_fill && AS->pcm_wr - AS->pcm_played >= 4u) {
        /* nothing in this block (a stream of unknown length ended on a block boundary):
         * end at the previous one instead. The DMA is at least three blocks short of it
         * (pcm_played lags the DMA by up to two at 192 kHz), so it has not fetched that
         * LLI yet. */
        l = (volatile uint32_t *)AS_LLI + ((AS->pcm_wr - 1u) % AS_PCM_BLOCKS) * LLI_WORDS;
        l[2] = 0;
        l[3] &= ~(3u << 27);
        AS->pcm_end = AS->pcm_wr;
        __asm__ volatile ("cpsie i" ::: "memory");
        return;
    }
    l = (volatile uint32_t *)AS_LLI + (AS->pcm_wr % AS_PCM_BLOCKS) * LLI_WORDS;
    if (!s_fill) {
        cur_block()[0] = cur_block()[1] = 0;
        s_fill = 1;
    }
    l[2] = 0;                                        /* LLP */
    l[3] &= ~(3u << 27);                             /* LLP_DST_EN, LLP_SRC_EN */
    l[4] = s_fill * 2u;                              /* block size in words, DONE clear */
    block_done();
    AS->pcm_end = AS->pcm_wr;
    __asm__ volatile ("cpsie i" ::: "memory");
}

/* Enough input to decode without stalling mid-frame: the rest of the track, or
 * what the decoder said one step needs (32 KiB: a 4096-frame 24-bit FLAC frame). */
static int input_ready(void)
{
    uint32_t want = b_trk_end - AS->in_rd;

    return b_in_avail() >= (want < b_in_need ? want : b_in_need);
}

/* One step of work; 0 = nothing to do now. */
static int produce(void)
{
    uint32_t want, got, raw;
    int32_t *dst;

    if (!AS->run)
        return 0;
    if (discarding())
        return 0;                                    /* (until A has written, or cut, the rest) */
    if (AS->pcm_wr - AS->pcm_played >= AS_PCM_BLOCKS)
        return 0;                                    /* ring full */
    if (!s_dec) {
        if (AS->state == AS_B_ENDED || AS->state == AS_B_RATE)
            return 0;                                /* waiting for A */
        if (AS->q_rd != AS->q_wr) {
            const as_track_t *n = &AS->q[AS->q_rd % AS_QUEUE];

            if ((AS->rate && n->rate != AS->rate) || !codec_find(n->codec)) {
                end_chain();                         /* A drains, re-clocks or swaps the image */
                AS->state = AS_B_RATE;
                return 1;
            }
            if (s_fill == AS_BLOCK_FRAMES) {         /* the last track's full last block: the */
                __asm__ volatile ("cpsid i" ::: "memory");   /* stream goes on, so commit it */
                if (s_resync)
                    s_resync = 0;
                else
                    block_done();
                s_fill = 0;
                __asm__ volatile ("cpsie i" ::: "memory");
                return 1;
            }
            if (AS->in_wr - AS->in_rd < 64u && n->bytes > 64u)
                return 0;                            /* header not there yet */
            if (track_open() != 0) {                 /* unplayable: skip it, keep the stream */
                AS->q_rd++;
                discard_rest();
                AS->done_seq = b_trk.seq;
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
    AS->in_need = b_in_need;
    if (!input_ready())
        return 0;
    if (s_resync) {                                  /* SysTick moved pcm_wr on since the last step */
        __asm__ volatile ("cpsid i" ::: "memory");
        s_resync = 0;
        s_fill = 0;
        __asm__ volatile ("cpsie i" ::: "memory");
    }
    dst = cur_block() + 2 * s_fill;
    want = AS_BLOCK_FRAMES - s_fill;
    if (b_trk.frames && b_trk.frames - AS->cur_frames < want)
        want = b_trk.frames - AS->cur_frames;
    if (guarded_read(dst, want, &got) != 0) {       /* the decoder gave up: end the track */
        track_close();
        return 1;
    }
    raw = got;
    if (s_drop && got) {                             /* a seek: the frames before its target */
        uint32_t k = got < s_drop ? got : s_drop;

        memmove(dst, dst + k * b_trk.channels, (got - k) * b_trk.channels * 4u);
        s_drop -= k;
        got -= k;
    }
    if (b_trk.channels == 1) {                       /* mono: L = R, expand in place */
        for (uint32_t i = got; i-- > 0; )
            dst[2 * i] = dst[2 * i + 1] = dst[i];
    }
    AS->cur_frames += got;
    if (raw == 0 || (b_trk.frames && AS->cur_frames >= b_trk.frames))
        track_close();                               /* the next track fills the same block */
    __asm__ volatile ("cpsid i" ::: "memory");
    if (s_resync) {                                  /* the DMA passed this block meanwhile */
        s_resync = 0;
        s_fill = 0;
    } else {
        s_fill += got;
        if (s_fill == AS_BLOCK_FRAMES && s_dec)
            block_done();                            /* (at a track's end: next step decides) */
    }
    __asm__ volatile ("cpsie i" ::: "memory");
    return 1;
}

static void stop_all(void)
{
    if (s_dec)
        track_close();
    AS->q_rd = AS->q_wr;
    AS->in_rd = AS->in_wr;
    s_fill = s_resync = 0;
    s_discard_seq = 0;
    AS->state = AS_B_READY;
}

int b_main(void)
{
    unsigned div_seen = AS->b_div;

    heap_reset();
    systick_start();
    AS->b_div_ack = div_seen;
    AS->state = AS_B_READY;
    for (;;) {
        if (AS->cmd == AS_CMD_STOP) {
            stop_all();
            AS->cmd = AS_CMD_NONE;
        } else if (AS->cmd == AS_CMD_PARK) {
            SYST_CSR = 0;
            __asm__ volatile ("cpsid i" ::: "memory");
            AS->state = AS_B_PARKED;
            AS->cmd = AS_CMD_NONE;
            for (;;)
                __asm__ volatile ("wfi");            /* until core A resets us */
        } else if (AS->cmd == AS_CMD_RESTART) {        /* A re-clocked / swapped images, DMA re-armed */
            AS->pcm_wr = AS->pcm_played = AS->pcm_end = 0;
            s_fill = s_resync = 0;
            AS->state = AS_B_PLAYING;
            AS->cmd = AS_CMD_NONE;
        }
        if (AS->b_div != div_seen) {                 /* core A changed our clock */
            div_seen = AS->b_div;
            systick_start();
            AS->b_div_ack = div_seen;                /* (A may now drop a faster clock's wait state) */
        }
        if (!produce())
            cpu_idle();
    }
}
