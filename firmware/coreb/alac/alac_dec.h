/*
 * alac_dec.h -- ALAC frame decoder (a C port of Apple's ALACDecoder.cpp over its
 * own ag_dec/dp_dec/bit-utility C sources, Apache-2.0). Output: interleaved
 * 32-bit words, sample left-justified (the I2S word format).
 */
#ifndef RECHORD_ALAC_DEC_H
#define RECHORD_ALAC_DEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t frame_len;
    uint8_t bits, pb, mb, kb, channels;
    uint16_t max_run;
    int32_t *u, *v, *pred;               /* frame_len each */
    uint16_t *shift;                     /* 2 x frame_len */
} alac_t;

/* cfg = the 24-byte ALACSpecificConfig ("magic cookie"). 0, or < 0. */
int alac_init(alac_t *a, const uint8_t *cfg, void *(*alloc)(size_t));
/* One packet -> *n frames in out (frame_len x channels words). 0, or < 0. */
int alac_frame(alac_t *a, uint8_t *pkt, uint32_t len, int32_t *out, uint32_t *n);

#endif
