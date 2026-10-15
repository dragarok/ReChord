/* Core-B image "wmall": WMA Lossless, mono and stereo, 16/24 bit (FFmpeg's decoder) from ASF. */
#include "b_rt.h"

int b_wmall_open(void);
uint32_t b_wmall_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_WMALOSSLESS, b_wmall_open, b_wmall_read },
    { 0, 0, 0 },
};
