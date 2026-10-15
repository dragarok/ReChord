/* wmalldec.h -- WMA Lossless decoder (FFmpeg's, made standalone: wmalldec.c). */
#ifndef RECHORD_WMALLDEC_H
#define RECHORD_WMALLDEC_H

#include <stddef.h>
#include <stdint.h>
#include "wma_wfx.h"

/* 0, or < 0 (unsupported stream, out of memory). `alloc` gets ~64 KiB + the frame buffers. */
int wmall_init(const asf_waveformatex_t *wfx, void *(*alloc)(size_t));
/* Decode from the rest of a WMA packet (size 0 at the end of the stream: the last
 * frame). Returns the bytes used (< 0: a bad packet); *nb_samples per channel. */
int wmall_decode(const uint8_t *buf, int size, int *nb_samples);
/* The decoded samples of channel ch: 16-bit values, or 24-bit values << 8. */
const int32_t *wmall_samples(int ch);
int wmall_bits(void);

#endif
