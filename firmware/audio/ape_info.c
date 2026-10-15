/*
 * ape_info.c -- Monkey's Audio file layout (as MAC SDK 13.28 APEHeader.cpp reads it).
 *
 *   [ID3v2 ...] "MAC " descriptor (3.98+) | old header (3.97)
 *   header, seek table, stored WAV header, frames, WAV tail, [APEv2] [ID3v1]
 *
 * Only the frames go to core B; their 32-bit word grouping starts at the first
 * frame, which is where core B's decoder starts reading.
 */
#include <string.h>

#include "ape_info.h"

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

#define FLAG_8_BIT        (1u << 0)
#define FLAG_PEAK_LEVEL   (1u << 2)
#define FLAG_24_BIT       (1u << 3)
#define FLAG_SEEK_ELEMS   (1u << 4)
#define FLAG_CREATE_WAV   (1u << 5)
#define FLAG_FLOAT        (1u << 12)

/* Where the trailing tags (APEv2, ID3v1) start. */
static uint32_t tags_start(ape_read_at_t rd, void *u, uint32_t end)
{
    uint8_t b[32];

    if (end >= 128 && rd(u, end - 128, b, 3) == 3 && !memcmp(b, "TAG", 3))
        end -= 128;
    if (end >= 32 && rd(u, end - 32, b, 32) == 32 && !memcmp(b, "APETAGEX", 8)) {
        uint32_t len = le32(b + 12) + ((le32(b + 20) & 0x80000000u) ? 32u : 0u);

        if (len <= end)
            end -= len;
    }
    return end;
}

int ape_info(ape_read_at_t rd, void *u, uint32_t size, ape_info_t *ai)
{
    uint8_t b[76];
    uint32_t junk = 0, flags, end;

    memset(ai, 0, sizeof *ai);
    for (;;) {                                       /* leading ID3v2 tag(s) */
        if (rd(u, junk, b, 10) != 10)
            return -1;
        if (memcmp(b, "ID3", 3))
            break;
        junk += 10u + syncsafe(b + 6) + ((b[5] & 0x10) ? 10u : 0u);
    }
    if (rd(u, junk, b, sizeof b) != sizeof b || memcmp(b, "MAC ", 4))
        return -1;                                   /* "MACF" (float) is not supported either */
    ai->version = le16(b + 4);
    if (ai->version < APE_VERSION_MIN || ai->version > APE_VERSION_MAX)
        return -2;
    end = tags_start(rd, u, size);
    if (ai->version >= 3980) {
        uint32_t desc = le32(b + 8), hdr = le32(b + 12), seek = le32(b + 16), wavhdr = le32(b + 20);
        const uint8_t *h;

        if (desc < 52 || desc + 24 > sizeof b || hdr < 24 || le32(b + 28))
            return -2;                               /* (frame data > 4 GiB: not on FAT32) */
        h = b + desc;
        ai->level = le16(h);
        flags = le16(h + 2);
        ai->blocks_per_frame = le32(h + 4);
        ai->final_blocks = le32(h + 8);
        ai->total_frames = le32(h + 12);
        ai->bits = le16(h + 16);
        ai->channels = le16(h + 18);
        ai->rate = le32(h + 20);
        ai->data_off = junk + desc + hdr + seek + wavhdr;
        ai->data_end = ai->data_off + le32(b + 24);
    } else {                                         /* 3.97: APE_HEADER_OLD */
        uint32_t hdr = 32, seek;

        ai->level = le16(b + 6);
        flags = le16(b + 8);
        ai->channels = le16(b + 10);
        ai->rate = le32(b + 12);
        ai->total_frames = le32(b + 24);
        ai->final_blocks = le32(b + 28);
        seek = ai->total_frames;
        if (flags & FLAG_PEAK_LEVEL)
            hdr += 4;
        if (flags & FLAG_SEEK_ELEMS) {
            seek = le32(b + hdr);
            hdr += 4;
        }
        ai->bits = (flags & FLAG_8_BIT) ? 8u : (flags & FLAG_24_BIT) ? 24u : 16u;
        ai->blocks_per_frame = 73728u * 4u;          /* every version >= 3.95 */
        ai->data_off = junk + hdr + ((flags & FLAG_CREATE_WAV) ? 0u : le32(b + 16)) + seek * 4u;
        ai->data_end = end;
    }
    if (ai->data_end > end)
        ai->data_end = end;
    if ((flags & FLAG_FLOAT) || (ai->bits != 8 && ai->bits != 16 && ai->bits != 24) ||
        ai->channels < 1 || ai->channels > 2 || !ai->rate || !ai->total_frames ||
        !ai->blocks_per_frame || ai->final_blocks > ai->blocks_per_frame ||
        (ai->level != 1000 && ai->level != 2000 && ai->level != 3000 && ai->level != 4000 && ai->level != 5000) ||
        ai->data_off >= ai->data_end)
        return -2;
    ai->samples = (ai->total_frames - 1u) * ai->blocks_per_frame + ai->final_blocks;
    return 0;
}

void ape_stream_header(const ape_info_t *ai, uint8_t out[APE_STREAM_HDR])
{
    const uint32_t v[5] = { ai->version | ai->level << 16, ai->blocks_per_frame, ai->final_blocks,
                            ai->total_frames, ai->bits | ai->channels << 16 };

    memcpy(out, "MACb", 4);
    for (int i = 0; i < 5; i++) {
        out[4 + 4 * i] = (uint8_t)v[i];
        out[5 + 4 * i] = (uint8_t)(v[i] >> 8);
        out[6 + 4 * i] = (uint8_t)(v[i] >> 16);
        out[7 + 4 * i] = (uint8_t)(v[i] >> 24);
    }
}
