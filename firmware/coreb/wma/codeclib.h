/*
 * codeclib.h -- ReChord stand-in for Rockbox's lib/rbcodec/codecs/lib/codeclib.h:
 * the transforms the WMA decoders use and av_log2 (CLZ: Cortex-M3 and host alike).
 * No allocator: the vendored decoders keep their buffers static.
 */
#ifndef RECHORD_WMA_CODECLIB_H
#define RECHORD_WMA_CODECLIB_H

#include "codecs.h"
#include "mdct.h"
#include "fft.h"

extern void ff_imdct_half(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_imdct_calc(unsigned int nbits, int32_t *output, const int32_t *input);
extern void ff_fft_calc_c(int nbits, FFTComplex *z);

/* floor(log2(v)), 0 for v = 0 (Rockbox's bs_generic(v, BS_0_0)) */
static inline unsigned int rc_log2(unsigned int v)
{
    return v ? 31u - (unsigned int)__builtin_clz(v) : 0u;
}
#define av_log2(v) rc_log2(v)

#endif
