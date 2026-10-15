/*
 * b_asf.h -- core B's side of the ASF stream (audio/asf.h): the asf_cfg_t header
 * core A sends, then whole media objects of the audio stream, from the data
 * packets that follow (fragments put back together). Shared by the WMA images;
 * only b_rt.h is used, so the host reference builds it as it is.
 */
#ifndef RECHORD_B_ASF_H
#define RECHORD_B_ASF_H

#include "b_rt.h"
#include "audio/asf.h"

#define B_ASF_PAD 32u                    /* zeros after every buffer: bit readers look past the end */

typedef struct {
    asf_cfg_t cfg;
    uint8_t *pkt;                        /* the packet being parsed */
    asf_pkt_t it;
    int in_pkt;
    uint8_t *obj;                        /* a fragmented object, put back together */
    uint32_t obj_cap, obj_have, obj_size, obj_num;
    int obj_on;
    uint32_t errors;                     /* bad packets, lost fragments */
} b_asf_t;

/* Read the header, check it against b_trk, allocate the packet buffer: 0, or < 0. */
int b_asf_open(b_asf_t *a);
/* The next whole media object of the audio stream: 1 (data, len), 0 at the track end.
 * It stays valid until the next call. */
int b_asf_object(b_asf_t *a, const uint8_t **data, uint32_t *len);

#endif
