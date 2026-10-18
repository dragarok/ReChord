/*
 * audio_shared.h -- the A/B audio contract (core A = UI + files, core B = decode).
 *
 *   core A: reads the file, strips everything but the audio (FLAC: a synthesized
 *           "fLaC" + STREAMINFO header, then the frames; WAV: raw PCM), appends it
 *           to the INPUT ring and queues a track descriptor. Gapless = the next
 *           track's bytes simply follow in the same ring.
 *   core B: decodes into the PCM ring (I2S words: 32-bit, sample left-justified,
 *           L then R), in blocks of AS_BLOCK_FRAMES.
 *   DMA (core A's controller): circular LLI chain over the PCM blocks into I2S1
 *           TXDR. Core B recycles a block when the DMA has written DONE back into
 *           its LLI; nothing on core A runs per block.
 *
 * Addresses are core-A (system) addresses; core B reaches its own RAM at
 * 0x01000000 as well as at 0. Both sides only ever poll -- every field has one
 * writer, noted per field.
 */
#ifndef RECHORD_AUDIO_SHARED_H
#define RECHORD_AUDIO_SHARED_H

#include <stdint.h>

/* Core-B RAM, as stock's decoder images use it: code (+ read-only data) in the
 * 128 KiB code RAM at 0x01000000 (core B sees it at 0), everything written in the
 * data RAM from 0x01020000 (stock: SP 0x01022000, data segments at 0x01020000):
 * this shared block, then the image's .data/.bss, its decoder heap, and its stack
 * at the top (the stack reserve is per image: coreb.ld B_STACK). The data RAM is
 * taken as 256 KiB (HDRAM0..7: SDK BBSystem.c gates 8 banks); unverified on
 * hardware. Both rings live in core-A SRAM, as RECHO's PCM ring does. */
#define AS_B_RAM          0x01000000u   /* core-B image runs here (B sees it at 0) */
#define AS_B_IMAGE_MAX    0x00020000u   /* 128 KiB: code + rodata + the .data load copy */
#define AS_SHARED         0x01020000u   /* struct audio_shared (4 KiB) */
#define AS_B_DATA         0x01021000u   /* .data, .bss, heap, stack: to AS_B_TOP */
#define AS_B_TOP          0x01060000u
#define AS_IN_RING        0x03030000u   /* compressed input ring, core-A SRAM (firmware_ap.ld) */
#define AS_IN_SIZE        0x00010000u   /* 64 KiB */
#define AS_PCM_RING       0x03090000u   /* PCM ring, core-A SRAM (RECHO uses it too) */
#define AS_PCM_BLOCKS     8u
#define AS_BLOCK_FRAMES   1024u
#define AS_BLOCK_BYTES    (AS_BLOCK_FRAMES * 8u)
#define AS_LLI            (AS_PCM_RING - 0x100u)   /* 8 x 28 B, just below the ring (SRAM ends at 0x030A0000) */

#define AS_MAGIC          0x4F445541u   /* "AUDO" */
#define AS_VERSION        13u
#define AS_QUEUE          4u

enum { AS_CODEC_FLAC = 1, AS_CODEC_WAV = 2, AS_CODEC_MP3 = 3, AS_CODEC_AIFF = 4, AS_CODEC_AAC = 5, AS_CODEC_ALAC = 6,
       AS_CODEC_VORBIS = 7, AS_CODEC_OPUS = 8, AS_CODEC_WAVF = 9 /* IEEE float32 PCM */,
       AS_CODEC_AIFF_LE = 10 /* AIFC 'sowt' */, AS_CODEC_WAVPACK = 11,
       AS_CODEC_DSD = 12 /* DSF/DFF -> 176.4 kHz */, AS_CODEC_APE = 13 /* Monkey's Audio */,
       AS_CODEC_WMA = 14 /* WMA v1/v2 (ASF) */, AS_CODEC_WMAPRO = 15 /* WMA Pro (ASF) */,
       AS_CODEC_WMALOSSLESS = 16 /* WMA Lossless (ASF) */ };

/* B state. ENDED and RATE: B has ended the DMA chain after the last block it wrote
 * (that LLI has no successor), so the DMA stops by itself once it has played it.
 * ENDED = the stream is over; RATE = the next track needs another sample rate:
 * A re-clocks, then sends AS_CMD_RESTART. */
enum { AS_B_BOOT = 0, AS_B_READY, AS_B_PLAYING, AS_B_ENDED, AS_B_ERROR, AS_B_RATE, AS_B_PARKED,
       AS_B_FAULT /* B took a fault and stopped: A reloads it */,
       AS_B_HELD /* B holds still for a CPU point switch (b_main.c b_hold): SysTick off, interrupts
                  * masked, a loop over the shared block; A releases it (rel_seq) */ };

/* A -> B commands (cmd field, B clears it to 0 when done) */
/* PARK: B stops its SysTick, masks interrupts and idles in thread mode, so core A
 * can hold it in reset with no exception half-handled (before loading another image) */
/* HIFI_PROBE: Developer > HIFI probe (coreb/b_hifi_probe.c): B runs it with no track open (probe_*) */
/* HOLD: B holds (AS_B_HELD) until A releases it, then goes on where it was (A switches the CPU
 * point meanwhile: services/cpuclk.c). SELFTEST: sent while B holds; B runs its half of the clock
 * self-test (coreb/b_selftest.c) and answers in st_b_state. */
enum { AS_CMD_NONE = 0, AS_CMD_STOP = 1, AS_CMD_RESTART = 2, AS_CMD_PARK = 3, AS_CMD_HIFI_PROBE = 4,
       AS_CMD_HOLD = 5, AS_CMD_SELFTEST = 6 };

typedef struct {
    uint32_t seq;            /* A: track sequence number (1, 2, ...) */
    uint32_t codec;          /* AS_CODEC_* */
    uint32_t rate, bits, channels;
    uint32_t frames;         /* total frames from the header, 0 = unknown */
    uint32_t bytes;          /* bytes this track occupies in the input stream, skip included */
    uint32_t skip;           /* filler bytes before the stream (alignment for direct reads) */
    uint32_t trim;           /* decoded frames to drop at the start (MP3: encoder + decoder delay) */
    uint32_t drop;           /* frames the runtime drops after the decoder's own trim: a seek's way
                              * from the frame (or block, packet) the stream starts at to the target */
    uint32_t clk;            /* the CPU point the track plays at (drivers/clock.h CLOCK_POINT_*): A's
                              * choice at queue time (services/cpuclk.c); B asks for it at its open */
} as_track_t;

typedef struct {
    uint32_t magic, version;

    /* input ring: bytes, absolute counters (wrap at 2^32) */
    volatile uint32_t in_wr;         /* A */
    volatile uint32_t in_rd;         /* B */
    volatile uint32_t in_closed;     /* A: 1 = no more tracks will follow */

    /* track queue */
    as_track_t q[AS_QUEUE];          /* A writes q[q_wr % AS_QUEUE] then bumps q_wr */
    volatile uint32_t q_wr;          /* A */
    volatile uint32_t q_rd;          /* B */

    /* PCM ring progress, in blocks */
    volatile uint32_t pcm_wr;        /* B: blocks written */
    volatile uint32_t pcm_played;    /* B: blocks the DMA has finished (from LLI DONE) */
    volatile uint32_t pcm_end;       /* B: blocks after which only silence follows, 0 = open */
    volatile uint32_t underruns;     /* B */

    /* status, B -> A */
    volatile uint32_t state;         /* B: AS_B_* */
    volatile uint32_t cur_seq;       /* B: track now being decoded */
    volatile uint32_t cur_frames;    /* B: frames of cur_seq decoded so far */
    volatile uint32_t done_seq;      /* B: last track fully decoded */
    /* A track B stopped decoding early (damaged data) while A still reads it: B asks
     * A to stop reading (abandon_seq); A answers where its input now ends (cut_end,
     * then cut_seq). B drops input only as far as A has written, never past it. */
    volatile uint32_t abandon_seq;   /* B */
    volatile uint32_t cut_end;       /* A: absolute input position, valid when cut_seq matches */
    volatile uint32_t cut_seq;       /* A */
    volatile uint32_t aborts;        /* B: tracks a decoder gave up on mid-call (out of memory, ...) */
    volatile uint32_t abort_code;    /* B: why the last one */
    volatile uint32_t fault_pc, fault_lr;   /* B: where its fault happened (state AS_B_FAULT) */
    volatile uint32_t in_need;       /* B: input bytes its decoder wants before a step */
    volatile uint32_t err;           /* B: decoder error code */
    volatile uint32_t b_hz;          /* B: its clock; A writes the boot value */
    volatile uint32_t b_load_pm;     /* B: busy share last second, 0..1000 */
    volatile uint32_t b_div;         /* A: PLL divider in use (A applies it in the CRU) */
    volatile uint32_t b_div_req;     /* B: divider B wants, from its load */
    volatile uint32_t ticks;         /* B: heartbeat */
    volatile uint32_t b_heap_peak;   /* B: most of its arena in use, bytes */
    volatile uint32_t b_stack_free;  /* B: stack never touched, bytes (low-water mark) */

    volatile uint32_t rate;          /* A: sample rate the output runs at */
    volatile uint32_t cmd;           /* A -> B */
    volatile uint32_t run;           /* A: 1 = decode; 0 = B leaves every counter alone */
    volatile uint32_t b_div_ack;     /* B: the b_div its SysTick now runs at (A: a slower clock's
                                      * memory wait state comes off after this) */

    /* HIFI accelerator (coreb/b_hifi.c, docs/HIFI.md). The state outlives B's image reloads (A
     * clears the block once a boot), so a block that failed its self-test, timed out or stalled
     * core B's bus is not tried again until the next power-on. */
    volatile uint32_t hifi_mode;     /* A: RECHORD.CFG hifi=, AS_HIFI_* */
    volatile uint32_t hifi_state;    /* B: AS_HIFI_UNTESTED .. AS_HIFI_DEAD */
    volatile uint32_t hifi_busy;     /* B: 1 while B touches the block (A reloading B with it set:
                                      * the access stalled core B's bus; B's next boot marks it DEAD) */
    /* Lines for core A's log, written by B (text, NUL-terminated): B fills b_msg[b_msg_wr % 4] and
     * bumps b_msg_wr; A logs every line up to it and bumps b_msg_rd. B drops a line when all 4 are
     * still unread. */
    volatile uint32_t b_msg_wr;      /* B */
    volatile uint32_t b_msg_rd;      /* A */
    char b_msg[4][128];              /* B */
    /* The HIFI block's APE and ALAC functions (coreb/b_hifi_fn.c), each on its own: hifi_state stays
     * FLAC's, but DEAD there stops every function. hifi_ape: bit k passed, bit 8 + k failed (its
     * self-test or a check: software until power-off), k = level / 1000 - 2 (+ 4 from version 3980) */
    volatile uint32_t hifi_ape;      /* B */
    volatile uint32_t hifi_alac;     /* B: AS_HIFI_UNTESTED, AS_HIFI_OK or AS_HIFI_FAILED */
    /* Developer > HIFI probe (coreb/b_hifi_probe.c, audio/hifi_probe.c, docs/HIFI_PLAN.md): A sets
     * probe_arg, then cmd = AS_CMD_HIFI_PROBE; B writes its lines into its own heap and answers in
     * probe_state. A logs them and loads a fresh core B after. probe_step names a stalled step. */
    volatile uint32_t probe_arg;     /* A: 0 = the whole probe, 1 = the timing jobs only */
    volatile uint32_t probe_state;   /* B: AS_PROBE_* */
    volatile uint32_t probe_step;    /* B: the step it is in (b_hifi_probe.c) */
    volatile uint32_t probe_text;    /* B: system address of its text ("hfp: ..." lines, '\n' each) */
    volatile uint32_t probe_len;     /* B: bytes of it */

    /* CPU point switches (services/cpuclk.c, docs/CPU_CLOCK.md). B, opening a track whose clk is not
     * clk_cur, sets clk_req and holds (b_hold: hold_seq + 1, state HELD); A (its SysTick) switches if
     * it may, writes clk_cur, b_hz, b_div, and releases B (rel_seq = hold_seq); B restarts its SysTick
     * at the new clock and acks (b_ack_seq). A also holds B itself (AS_CMD_HOLD) to go back to the UI
     * point when playback stops. */
    volatile uint32_t clk_cur;       /* A: the point in effect */
    volatile uint32_t clk_req;       /* B: the point the next track wants */
    volatile uint32_t hold_seq;      /* B: holds entered */
    volatile uint32_t rel_seq;       /* A: the hold released */
    volatile uint32_t b_ack_seq;     /* B: running again after that hold */
    /* the clock self-test (selftest_kernel.h): A sets the inputs, then cmd = AS_CMD_SELFTEST while B
     * holds; B runs its half (st_b_state 1, then 2) and leaves its results */
    volatile uint32_t st_code_words; /* A: words of B's image it loaded (at 0x01000000) */
    volatile uint32_t st_b_state;    /* B: 0, 1 running, 2 done */
    volatile uint32_t st_b_cyc;      /* B: its SysTick's cycles over st_b_ticks of the 24 MHz timer */
    volatile uint32_t st_b_ticks;
    volatile uint32_t st_b_sig;      /* B: st_kernel()'s signature */
    volatile uint32_t st_b_high;     /* B: st_march2() over its free heap (0 = clean) */
    volatile uint32_t st_b_high_at, st_b_high_words;
    volatile uint32_t st_b_low;      /* B: st_march2() over its LOW window */
    volatile uint32_t st_b_code;     /* B: st_sum() of its image */
    volatile uint32_t st_b_unlock;   /* B: times it read the PLL lock bit 0 */
    /* the track B last finished: its decode cycles (B's SysTick: b_hz cycles a second), for core A's
     * per-track line (audio.c "clock: track") */
    volatile uint32_t trk_seq, trk_frames, trk_rate, trk_hz, trk_point;
    volatile uint32_t trk_cyc_lo, trk_cyc_hi;
} audio_shared_t;

/* hifi_mode: auto = the HIFI block for FLAC LPC, APE's NN filters and ALAC's LPC, each once its
 * self-test passed; off = software only; verify = HIFI, then every sample checked in software (the
 * software result is played; mismatches counted) */
enum { AS_HIFI_AUTO = 0, AS_HIFI_OFF = 1, AS_HIFI_VERIFY = 2 };
/* hifi_state */
enum { AS_HIFI_UNTESTED = 0, AS_HIFI_OK = 1 /* narrow and wide */, AS_HIFI_NARROW = 2 /* wide failed */,
       AS_HIFI_FAILED = 3 /* self-test or a check failed: software */, AS_HIFI_DEAD = 4 /* timed out or
       stalled B's bus: never touched again this boot */ };

/* probe_state */
enum { AS_PROBE_NONE = 0, AS_PROBE_RUNNING = 1, AS_PROBE_DONE = 2, AS_PROBE_NOIMAGE = 3 /* not in this
       core-B image */, AS_PROBE_NOMEM = 4 };

#define AS ((audio_shared_t *)AS_SHARED)

/* Core B's wake-up while it holds (b_main.c b_hold: WFI with its interrupts masked, so a PLL switch
 * runs with B executing nothing): the mailbox's A2B channel 0 (0x40110000, port 0: +0x00 IntEn, +0x04
 * status, write 1 to clear, +0x0C data 0, whose write sets status bit 0), core B's NVIC IRQ 9 (SDK
 * INT_ID25_MAILBOX0). Stock holds and releases core B through the mailbox too (BB_HOLD / BB_HOLD_EXIT,
 * FreqSetPrevProcess 0x03068678 [code]). Core A pings after writing rel_seq or cmd. */
#define AS_MBOX_A2B_EN    (*(volatile uint32_t *)0x40110000u)
#define AS_MBOX_A2B_ST    (*(volatile uint32_t *)0x40110004u)
#define AS_MBOX_A2B_DATA0 (*(volatile uint32_t *)0x4011000Cu)
#define AS_MBOX_B_IRQ     9u
static inline void as_ping_b(void)
{
    __asm__ volatile ("dsb" ::: "memory");
    AS_MBOX_A2B_DATA0 = 1u;
}

#endif
