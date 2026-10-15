/* Core-B image "wmapro": WMA Pro, mono and stereo (Rockbox libwmapro, fixed point) from ASF. */
#include "b_rt.h"

int b_wmapro_open(void);
uint32_t b_wmapro_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_WMAPRO, b_wmapro_open, b_wmapro_read },
    { 0, 0, 0 },
};
