/* Core-B image "aac": AAC LC, HE-AAC v1/v2 (FAAD2 needs the whole code RAM). */
#include "b_rt.h"

int b_aac_open(void);
uint32_t b_aac_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_AAC, b_aac_open, b_aac_read },
    { 0, 0, 0 },
};
