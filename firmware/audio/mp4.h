/*
 * mp4.h -- the audio track of an MP4/M4A file: codec setup, where every sample
 * (= coded frame) is, and the gapless trim. Used by core A to stream the frames
 * to core B, and by the host reference decoder (tests), so both cut alike.
 *
 * Only the sample tables are used (stsz, stsc, stco/co64); they are read through
 * the callback a little at a time, so a long file costs no RAM.
 */
#ifndef RECHORD_MP4_H
#define RECHORD_MP4_H

#include <stdint.h>

typedef uint32_t (*mp4_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

enum { MP4_AAC = 1, MP4_ALAC = 2 };

typedef struct {
    uint32_t codec;                  /* MP4_AAC / MP4_ALAC */
    uint32_t rate, channels, bits;   /* of the decoded output (AAC: 16 bits, 2 channels always) */
    uint32_t frames;                 /* output frames to play (0 = unknown) */
    uint32_t trim;                   /* output frames to drop first */
    uint8_t cfg[64];                 /* AAC AudioSpecificConfig / ALAC magic cookie (24 B) */
    uint32_t cfg_len;
    uint32_t nsamples;               /* coded frames */
    uint32_t stsz_off, stsz_fixed;   /* table of sizes, or one size for all */
    uint32_t stsc_off, stsc_n;
    uint32_t stco_off, stco_n, co64;
    uint32_t data_bytes;             /* sum of the sample sizes (mp4_info counts them) */
    uint32_t ilst_off, ilst_len;     /* iTunes tags, 0 = none */
    uint32_t aot, sbr;               /* AAC: audio object type, SBR output (rate x 2) */
} mp4_info_t;

/* 0, or < 0: -1 not MP4 / I/O, -2 no audio track we can play, -3 bad tables. */
int mp4_info(mp4_read_at_t rd, void *u, uint32_t size, mp4_info_t *mi);

/* Walks the samples in order. */
typedef struct {
    uint32_t sample, chunk, left_in_chunk, off;
    uint32_t stsc_i, next_first, per_chunk;
    uint32_t sz_base, sz[64];        /* window on stsz */
} mp4_cur_t;

void mp4_cursor(mp4_cur_t *c);
/* Next sample: 1 (off, size set), 0 at the end, < 0 on a read error. */
int mp4_next(mp4_read_at_t rd, void *u, const mp4_info_t *mi, mp4_cur_t *c, uint32_t *off, uint32_t *size);

#endif
