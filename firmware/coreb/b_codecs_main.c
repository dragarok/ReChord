/* Core-B image "main": FLAC, WAV/AIFF, MPEG audio, ALAC. */
#include "b_rt.h"

int b_flac_open(void);
uint32_t b_flac_read(int32_t *dst, uint32_t frames);
int b_pcm_open(void);
uint32_t b_pcm_read(int32_t *dst, uint32_t frames);
int b_mp3_open(void);
uint32_t b_mp3_read(int32_t *dst, uint32_t frames);
int b_alac_open(void);
uint32_t b_alac_read(int32_t *dst, uint32_t frames);

const b_codec_t b_codecs[] = {
    { AS_CODEC_FLAC, b_flac_open, b_flac_read },
    { AS_CODEC_WAV, b_pcm_open, b_pcm_read },
    { AS_CODEC_AIFF, b_pcm_open, b_pcm_read },
    { AS_CODEC_AIFF_LE, b_pcm_open, b_pcm_read },
    { AS_CODEC_WAVF, b_pcm_open, b_pcm_read },
    { AS_CODEC_MP3, b_mp3_open, b_mp3_read },
    { AS_CODEC_ALAC, b_alac_open, b_alac_read },
    { 0, 0, 0 },
};
