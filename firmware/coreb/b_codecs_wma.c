/* Core-B image "wma": WMA v1/v2 (Rockbox libwma, fixed point) from ASF. */
#include "b_rt.h"

int b_wma_open(void);
uint32_t b_wma_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_WMA, b_wma_open, b_wma_read },
    { 0, 0, 0 },
};
