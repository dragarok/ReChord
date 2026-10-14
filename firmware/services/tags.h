/*
 * tags.h -- title, artist, album and track number of an audio file, from the
 * tags RECHO reads: ID3v2.2/2.3/2.4, ID3v1, APEv2, appended ID3v2, FLAC Vorbis
 * comments, WAV LIST/INFO and the WAV "id3 " chunk.
 *
 * Works through a read-at callback only, with one 512-byte window on the stack
 * (under 800 bytes of stack in all on Cortex-M3), no malloc, no static data
 * but const name tables. Covers, lyrics and padding are seeked over, never
 * read: a file with a 2 MB cover costs a handful of small reads.
 */
#ifndef RECHORD_SERVICES_TAGS_H
#define RECHORD_SERVICES_TAGS_H

#include <stdint.h>

/* File kinds, from the extension. Any other value is read like TAGS_MP3. */
enum { TAGS_MP3, TAGS_FLAC, TAGS_WAV };

/* tag_info_t.found: the sources that gave at least one field. */
#define TAGS_SRC_ID3V2      0x01u   /* ID3v2 in front of the file, or the WAV "id3 " chunk */
#define TAGS_SRC_APE        0x02u   /* APEv2 (or v1) at the end */
#define TAGS_SRC_ID3V2_END  0x04u   /* appended ID3v2 (footer "3DI") at the end */
#define TAGS_SRC_ID3V1      0x08u
#define TAGS_SRC_VORBIS     0x10u   /* FLAC VORBIS_COMMENT block */
#define TAGS_SRC_RIFF       0x20u   /* WAV LIST/INFO */

/* Strings are UTF-8, NUL-terminated, cut at a character boundary; "" and
 * track 0 when not found. */
typedef struct {
    char title[96], artist[64], album[64];
    uint16_t track;
    uint8_t found;                 /* TAGS_SRC_* bitmask */
} tag_info_t;

/* Reads n bytes at file offset off into buf; returns the bytes read. */
typedef uint32_t (*tags_read_at_t)(void *u, uint32_t off, void *buf, uint32_t n);

/*
 * When a file has several tags the first one found wins, field by field, in
 * this order:
 *   MP3 (and any other kind): the ID3v2 tags in front, in file order; then at
 *     the end APEv2, appended ID3v2, ID3v1 (Lyrics3v2 is skipped over).
 *   FLAC: ID3v2 tags in front (some taggers add one), then VORBIS_COMMENT.
 *   WAV:  LIST/INFO and "id3 "/"ID3 " chunks, in file order.
 * Reading stops as soon as all four fields are known.
 *
 * Returns 0 if any field was found, -1 if none was.
 */
int tags_read(tags_read_at_t rd, void *u, uint32_t size, int kind, tag_info_t *out);

#endif
