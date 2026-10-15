/* Core-B image "wv": WavPack. */
#include "b_rt.h"

int b_wavpack_open(void);
uint32_t b_wavpack_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_WAVPACK, b_wavpack_open, b_wavpack_read },
    { 0, 0, 0 },
};
