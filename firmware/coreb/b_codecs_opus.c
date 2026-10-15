/* Core-B image "opus": Ogg Opus. */
#include "b_rt.h"

int b_opus_open(void);
uint32_t b_opus_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_OPUS, b_opus_open, b_opus_read },
    { 0, 0, 0 },
};
