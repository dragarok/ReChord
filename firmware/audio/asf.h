/*
 * asf.h -- ASF (WMA) audio: the header (core A, through a read-at callback, like
 * mp4.h) and the data packets' payloads (core B and the host reference, on a
 * packet in memory).
 *
 * Core A streams to core B "u32 length + asf_cfg_t", then the data packets as
 * they are in the file: they all have the one size the header gives (ASF pads
 * every packet to it), so their count fixes the stream length up front and the
 * bytes go straight from the eMMC into the ring. Core B parses each packet
 * (asf_pkt_*), reassembles fragmented media objects and decodes them.
 */
#ifndef RECHORD_ASF_H
#define RECHORD_ASF_H

#include <stdint.h>

typedef uint32_t (*asf_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

enum { ASF_WMAV1 = 0x160, ASF_WMAV2 = 0x161, ASF_WMAPRO = 0x162, ASF_WMALOSSLESS = 0x163 };

#define ASF_EXTRA_MAX 32u

/* What core B needs: the stream's WAVEFORMATEX and how to cut the packets. */
typedef struct {
    uint16_t codec_id;               /* ASF_WMA* (the WAVEFORMATEX format tag) */
    uint16_t channels;
    uint32_t rate;
    uint32_t bitrate;                /* bits per second (average bytes/s x 8) */
    uint16_t block_align;            /* bytes per codec packet (WMA: a superframe) */
    uint16_t bits;                   /* bits per sample (Pro / Lossless: of the output) */
    uint16_t stream;                 /* ASF stream number of this audio stream */
    uint16_t extra_len;              /* codec-specific data (cbSize), up to ASF_EXTRA_MAX */
    uint32_t packet_size;            /* every data packet's size */
    uint8_t extra[ASF_EXTRA_MAX];
} asf_cfg_t;

typedef struct {
    asf_cfg_t cfg;
    uint32_t data_off;               /* file offset of the first data packet */
    uint32_t npackets;               /* data packets present (a cut file: fewer than announced) */
    uint32_t data_bytes;             /* npackets x packet_size */
    uint32_t frames;                 /* play duration less the preroll, in frames (a cut file: pro rata); 0 = unknown */
    uint32_t ms;                     /* the same in milliseconds */
} asf_info_t;

/* 0, or < 0: -1 not ASF / I/O, -2 no WMA audio stream, -3 DRM, -4 bad layout. */
int asf_info(asf_read_at_t rd, void *u, uint32_t size, asf_info_t *ai);

/* ---- data packets ---- */

typedef struct {
    uint8_t stream;                  /* stream number */
    uint8_t key;                     /* key frame flag */
    uint32_t obj;                    /* media object number (mod 2^8/2^16/2^32 as coded) */
    uint32_t obj_off;                /* offset of these bytes in the object */
    uint32_t obj_size;               /* the object's size (whole payload if not given) */
    const uint8_t *data;
    uint32_t len;
} asf_payload_t;

typedef struct {
    const uint8_t *p, *end;          /* next payload header; end of the payload data */
    uint32_t left;                   /* payloads left to parse */
    uint8_t multiple, prop, len_type;
    const uint8_t *sub, *sub_end;    /* compressed payload: next sub-payload */
    asf_payload_t sub_pl;            /* its template */
} asf_pkt_t;

/* Start on one data packet of `size` bytes: 0, or < 0 if its header is bad. */
int asf_pkt_begin(asf_pkt_t *it, const uint8_t *pkt, uint32_t size);
/* Next payload: 1 (pl set), 0 at the end of the packet, < 0 if the packet is bad. */
int asf_pkt_next(asf_pkt_t *it, asf_payload_t *pl);

#endif
