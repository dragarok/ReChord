/* Core-B image "ape": Monkey's Audio (libdemac). */
#include "b_rt.h"

int b_ape_open(void);
uint32_t b_ape_read(int32_t *dst, uint32_t frames);
void b_ape_close(void);

const b_codec_t b_codecs[] = {
    { AS_CODEC_APE, b_ape_open, b_ape_read, b_ape_close },
    { 0, 0, 0 },
};
