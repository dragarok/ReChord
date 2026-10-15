/*
 * dsd_dec.h -- DSD (1-bit, 2.8224 / 5.6448 / 11.2896 MHz) to 176.4 kHz PCM:
 * one linear-phase FIR (dsd_fir.h) decimating by 16 / 32 / 64, run on whole
 * bytes through tables of 8-tap partial sums (one lookup per byte and output).
 * Output: interleaved 32-bit words, sample left-justified.
 */
#ifndef RECHORD_DSD_DEC_H
#define RECHORD_DSD_DEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t ch, nt, step;           /* channels, tables (= taps / 8), bytes per output */
    uint32_t shift;                  /* table scale: entries = sum(+-h) >> shift (Q30 h) */
    int16_t (*tab)[256];             /* nt tables */
    uint8_t *hist;                   /* ch x nt bytes, newest at pos */
    uint32_t pos, phase;
    uint8_t lsb_first;
} dsd_t;

/* rate_mult: 1 (DSD64), 2 (DSD128), 4 (DSD256). lsb_first: DSF's bit order. 0 / -1. */
int dsd_init(dsd_t *d, uint32_t ch, uint32_t rate_mult, int lsb_first, void *(*alloc)(size_t));
/* Push n bytes of each channel (channel c's i-th byte at in[c * cstride + i * istride]);
 * writes the frames they complete to out (room: n * 8 / (8 * step) + 1 frames). Returns frames. */
uint32_t dsd_run(dsd_t *d, const uint8_t *in, uint32_t cstride, uint32_t istride, uint32_t n, int32_t *out);

#endif
