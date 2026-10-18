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
#define AS_VERSION        9u
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
       AS_B_FAULT /* B took a fault and stopped: A reloads it */ };

/* A -> B commands (cmd field, B clears it to 0 when done) */
/* PARK: B stops its SysTick, masks interrupts and idles in thread mode, so core A
 * can hold it in reset with no exception half-handled (before loading another image) */
enum { AS_CMD_NONE = 0, AS_CMD_STOP = 1, AS_CMD_RESTART = 2, AS_CMD_PARK = 3 };

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
} audio_shared_t;

#define AS ((audio_shared_t *)AS_SHARED)

#endif
