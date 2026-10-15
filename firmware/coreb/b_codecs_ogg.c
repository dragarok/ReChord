/* Core-B image "ogg": Ogg Vorbis. */
#include "b_rt.h"

int b_vorbis_open(void);
uint32_t b_vorbis_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_VORBIS, b_vorbis_open, b_vorbis_read },
    { 0, 0, 0 },
};
