/* Core-B image "dsd": DSF / DFF (DSD64/128/256) to 176.4 kHz PCM. */
#include "b_rt.h"

int b_dsd_open(void);
uint32_t b_dsd_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_DSD, b_dsd_open, b_dsd_read },
    { 0, 0, 0 },
};
