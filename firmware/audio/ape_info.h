/*
 * ape_info.h -- where the frames of a Monkey's Audio (.ape) file are, and the
 * header core B's decoder needs to decode them.
 *
 * Used by core A (to stream only the frames, behind a small synthesized header)
 * and by the host reference decoder (tests/aperef.c), so both feed core B's
 * b_ape.c exactly the same bytes.
 */
#ifndef RECHORD_APE_INFO_H
#define RECHORD_APE_INFO_H

#include <stdint.h>

/* Reads n bytes at file offset off into buf; returns the bytes read. */
typedef uint32_t (*ape_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

typedef struct {
    uint32_t data_off, data_end;    /* the frames: after leading ID3v2 tags, descriptor, header,
                                       seek table and stored WAV header; before the WAV tail and
                                       the APEv2 / ID3v1 tags */
    uint32_t version;               /* file version x 1000 (3990 = 3.99) */
    uint32_t level;                 /* compression level: 1000 fast .. 5000 insane */
    uint32_t blocks_per_frame, final_blocks, total_frames;
    uint32_t bits, channels, rate;
    uint32_t samples;               /* total frames (blocks) of audio */
} ape_info_t;

/* The versions libdemac decodes (3.97 .. 3.99) */
#define APE_VERSION_MIN 3970u
#define APE_VERSION_MAX 3990u

/* Core B's stream: this header, then the file's frame bytes [data_off, data_end).
 *   0  "MACb"
 *   4  u16 version, u16 level
 *   8  u32 blocks per frame
 *  12  u32 blocks in the final frame
 *  16  u32 total frames
 *  20  u16 bits per sample, u16 channels                     (all little-endian) */
#define APE_STREAM_HDR 24u

/* 0; -1: not a Monkey's Audio file; -2: one this decoder does not support
 * (version, bit depth, channels, float, or a damaged header). */
int ape_info(ape_read_at_t rd, void *u, uint32_t size, ape_info_t *ai);

void ape_stream_header(const ape_info_t *ai, uint8_t out[APE_STREAM_HDR]);

#endif
