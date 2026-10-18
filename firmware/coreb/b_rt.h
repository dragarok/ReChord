/*
 * b_rt.h -- what core B's runtime (b_main.c) offers its decoders.
 *
 * A core-B image is the runtime plus one family of decoders (b_codecs_*.c lists
 * them). Each decoder reads its track's bytes from the input ring with the
 * calls below and writes 32-bit I2S words (sample left-justified), interleaved
 * in the track's channel count; the runtime does mono expansion, gapless joins
 * and the PCM ring.
 */
#ifndef RECHORD_B_RT_H
#define RECHORD_B_RT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "audio/audio_shared.h"

typedef struct {
    uint32_t codec;                              /* AS_CODEC_* */
    int (*open)(void);                           /* 0, or < 0: unplayable */
    uint32_t (*read)(int32_t *dst, uint32_t frames);   /* frames written, 0 = end of track */
    void (*close)(void);                         /* the track is over (may be NULL) */
} b_codec_t;

extern const b_codec_t b_codecs[];               /* this image's decoders, codec 0 ends it */

extern as_track_t b_trk;                         /* the track being decoded */
extern uint32_t b_trk_start, b_trk_end;          /* its span of the input stream */

uint32_t b_in_avail(void);                       /* bytes of this track in the ring now */
void b_in_copy(void *out, uint32_t n);           /* n <= b_in_avail() */
/* n bytes, waiting (WFI) for core A's refills (n may exceed the ring); short only
 * at the track end or when core A sends a command. */
uint32_t b_in_read(void *out, uint32_t n);
uint32_t b_in_pos(void);                         /* bytes of the track consumed */
/* Input one decode step may need (a frame or packet); the runtime decodes only once
 * this much of the track (or all of the rest) is in the ring. Set by open(), else 32 KiB. */
extern uint32_t b_in_need;

/* Heap over the decoder arena, emptied when a track opens (8-byte aligned). */
/* Out of heap: the decoder call is abandoned (b_abort), never handed NULL. */
void *b_alloc(size_t n);
void *b_try_alloc(size_t n);                     /* NULL when it does not fit: the track goes on */
/* Give up on the track from inside a decoder call: back in b_main, the track ends. */
void b_abort(uint32_t code) __attribute__((noreturn));
void *b_calloc(size_t n);
void b_free(void *p);
void *b_realloc(void *p, size_t n);

/* Developer > HIFI probe (b_hifi_probe.c in the main image; elsewhere a stub that says so). */
void b_probe(void);

#endif
