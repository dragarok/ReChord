/*
 * mp3_info.h -- where the audio of an MP3 file is, and how much of it to keep.
 *
 * Used by core A (to stream only the MPEG frames to core B) and by the host
 * reference decoder (tests), so both trim exactly the same samples.
 */
#ifndef RECHORD_MP3_INFO_H
#define RECHORD_MP3_INFO_H

#include <stdint.h>

/* Reads n bytes at file offset off into buf; returns the bytes read. */
typedef uint32_t (*mp3_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

typedef struct {
    uint32_t data_off, data_end;   /* the MPEG frames: after leading tags and the Xing/Info frame,
                                      before trailing tags (ID3v1, APEv2, Lyrics3, appended ID3v2) */
    uint32_t rate, channels, layer, bitrate;   /* from the first frame; bitrate in kbit/s */
    uint32_t spf;                  /* samples per frame */
    uint32_t frames;               /* samples to play after trimming, 0 = unknown (no Xing/VBRI) */
    uint32_t skip;                 /* samples to drop at the start: LAME encoder delay + 529 */
    uint32_t tag_end;              /* end of the leading ID3v2 tag(s) (where the tags are), 0 = none */
} mp3_info_t;

#define MP3_DECODER_DELAY 529u     /* synthesis filterbank + IMDCT overlap of a libmad/mpg123 decoder */

/* 0, or -1: no MPEG audio frame found. */
int mp3_info(mp3_read_at_t rd, void *u, uint32_t size, mp3_info_t *mi);

/* Frame length in bytes from a 4-byte header, 0 if it is not a valid header. */
uint32_t mp3_frame_len(const uint8_t *h, uint32_t *rate, uint32_t *ch, uint32_t *spf);

#endif
