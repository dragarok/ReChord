/*
 * wma_wfx.h -- the stream description the vendored WMA decoders take (Rockbox's
 * asf_waveformatex_t, libasf/asf.h), filled by firmware/coreb/b_wma.c from what
 * core A's ASF demuxer (firmware/audio/asf.c) streams ahead of the packets.
 */
#ifndef RECHORD_WMA_WFX_H
#define RECHORD_WMA_WFX_H

#include <stdint.h>

#define ASF_CODEC_ID_WMAV1      0x160
#define ASF_CODEC_ID_WMAV2      0x161
#define ASF_CODEC_ID_WMAPRO     0x162
#define ASF_CODEC_ID_WMALOSSLESS 0x163

struct asf_waveformatex_s {
    uint32_t packet_size;
    int audiostream;
    uint16_t codec_id;
    uint16_t channels;
    uint32_t rate;
    uint32_t bitrate;
    uint16_t blockalign;
    uint16_t bitspersample;
    uint16_t datalen;
    uint64_t numpackets;
    uint8_t data[46];
};
typedef struct asf_waveformatex_s asf_waveformatex_t;

#endif
