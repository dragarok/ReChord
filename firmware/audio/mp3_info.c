/*
 * mp3_info.c -- MPEG audio layout of a file: skip the tags at both ends and the
 * Xing/Info (or VBRI) frame, and take the gapless trim from the LAME tag.
 *
 * Trim, as ffmpeg and the LAME spec have it: play frames x spf - delay - padding
 * samples, after dropping delay + 529 (the decoder's own delay) at the start.
 */
#include <string.h>

#include "mp3_info.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }

static const uint16_t k_br[2][3][15] = {
    {   /* MPEG-1: layer I, II, III */
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384 },
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },
    }, { /* MPEG-2 / 2.5 */
        { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },
    },
};
static const uint16_t k_rate[3] = { 44100, 48000, 32000 };

uint32_t mp3_frame_len(const uint8_t *h, uint32_t *rate, uint32_t *ch, uint32_t *spf)
{
    uint32_t ver = h[1] >> 3 & 3u, lay = h[1] >> 1 & 3u, bri = h[2] >> 4, sri = h[2] >> 2 & 3u;
    uint32_t pad = h[2] >> 1 & 1u, lsf = ver != 3u, r, br, l;

    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0 || ver == 1u || lay == 0u || bri == 0u || bri == 15u || sri == 3u)
        return 0;                    /* not a header, or free format (no length to check) */
    l = 3u - lay;                    /* 0 = layer I */
    r = k_rate[sri] >> (ver == 0u ? 2 : lsf);
    br = k_br[lsf][l][bri] * 1000u;
    if (rate) *rate = r;
    if (ch) *ch = (h[3] >> 6) == 3u ? 1u : 2u;
    if (spf) *spf = l == 0u ? 384u : (l == 2u && lsf) ? 576u : 1152u;
    if (l == 0u)
        return (12u * br / r + pad) * 4u;
    return (l == 2u && lsf ? 72u : 144u) * br / r + pad;
}

/* Trailing tags, outermost first: ID3v1, Lyrics3v2, APEv2 (with or without its
 * header), appended ID3v2 (footer "3DI"); repeated while any is found. */
static uint32_t strip_tail(mp3_read_at_t rd, void *u, uint32_t lo, uint32_t end)
{
    uint8_t b[32];

    for (int guard = 0; guard < 8; guard++) {
        uint32_t was = end;

        if (end - lo >= 128u && rd(u, end - 128u, b, 3) == 3 && !memcmp(b, "TAG", 3))
            end -= 128u;
        if (end - lo >= 15u && rd(u, end - 15u, b, 15) == 15 && !memcmp(b + 6, "LYRICS200", 9)) {
            uint32_t n = 0;

            for (int i = 0; i < 6; i++)
                n = n * 10u + (uint32_t)(b[i] - '0');
            if (n + 15u <= end - lo)
                end -= n + 15u;
        }
        if (end - lo >= 32u && rd(u, end - 32u, b, 32) == 32 && !memcmp(b, "APETAGEX", 8)) {
            uint32_t n = le32(b + 12) + ((le32(b + 20) & 0x80000000u) ? 32u : 0u);

            if (n <= end - lo)
                end -= n;
        }
        if (end - lo >= 10u && rd(u, end - 10u, b, 10) == 10 && !memcmp(b, "3DI", 3)) {
            uint32_t n = syncsafe(b + 6) + 20u;

            if (n <= end - lo)
                end -= n;
        }
        if (end == was)
            break;
    }
    return end;
}

int mp3_info(mp3_read_at_t rd, void *u, uint32_t size, mp3_info_t *mi)
{
    uint8_t b[192];
    uint32_t pos = 0, len, xo;

    memset(mi, 0, sizeof *mi);
    /* leading ID3v2 tags (taggers sometimes leave two) */
    while (rd(u, pos, b, 10) == 10 && !memcmp(b, "ID3", 3) && b[3] != 0xFF && b[4] != 0xFF)
        pos += 10u + syncsafe(b + 6) + ((b[5] & 0x10) ? 10u : 0u);
    mi->tag_end = pos;
    mi->data_end = strip_tail(rd, u, pos, size);

    /* first frame: a valid header whose successor is a valid header too */
    for (uint32_t lim = pos + 0x10000u; ; pos++) {
        uint8_t n[4];

        if (pos >= lim || pos + 4u > mi->data_end)
            return -1;
        if (rd(u, pos, b, 4) != 4)
            return -1;
        len = mp3_frame_len(b, &mi->rate, &mi->channels, &mi->spf);
        if (!len)
            continue;
        if (pos + len + 4u > mi->data_end || pos + len == mi->data_end)
            break;                                   /* a one-frame file */
        if (rd(u, pos + len, n, 4) == 4 && mp3_frame_len(n, 0, 0, 0) &&
            (n[1] & 0xFE) == (b[1] & 0xFE) && (n[2] & 0x0C) == (b[2] & 0x0C))
            break;                                   /* same version, layer and rate */
    }
    mi->layer = 4u - (b[1] >> 1 & 3u);
    mi->bitrate = k_br[(b[1] >> 3 & 3u) != 3u][3u - (b[1] >> 1 & 3u)][b[2] >> 4];
    mi->data_off = pos;

    /* Xing / Info after the side information, or VBRI at a fixed 32 */
    mi->cbr = 1;                                     /* (no header: taken as constant, as players do) */
    if (rd(u, pos, b, sizeof b) != sizeof b)
        return 0;
    xo = 4u + ((b[1] & 1u) ? 0u : 2u);
    if ((b[1] >> 3 & 3u) == 3u)
        xo += mi->channels == 1u ? 17u : 32u;
    else
        xo += mi->channels == 1u ? 9u : 17u;
    if (mi->layer == 3u && xo + 8u <= sizeof b && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4))) {
        uint32_t fl = be32(b + xo + 4), p = xo + 8u, nf = 0;

        if (fl & 1u) { nf = be32(b + p); p += 4u; }
        mi->xing_off = pos;
        mi->xing_bytes = mi->data_end - pos;
        if (fl & 2u) {
            if (be32(b + p) > len && be32(b + p) <= mi->data_end - pos)
                mi->xing_bytes = be32(b + p);
            p += 4u;
        }
        if (fl & 4u) {
            if (p + 100u <= sizeof b) {
                memcpy(mi->toc, b + p, 100);
                mi->has_toc = 1;
            }
            p += 100u;
        }
        if (fl & 8u) p += 4u;
        mi->data_off = pos + len;                    /* the Info frame carries no audio */
        mi->cbr = !memcmp(b + xo, "Info", 4);
        mi->nframes = nf;
        mi->frames = nf * mi->spf;
        if (p + 24u <= sizeof b && (!memcmp(b + p, "LAME", 4) || !memcmp(b + p, "Lavf", 4) || !memcmp(b + p, "Lavc", 4))) {
            uint32_t v = (uint32_t)b[p + 21] << 16 | (uint32_t)b[p + 22] << 8 | b[p + 23];
            uint32_t delay = v >> 12, padding = v & 0xFFFu;

            mi->skip = delay + MP3_DECODER_DELAY;
            mi->frames = mi->frames > delay + padding ? mi->frames - delay - padding : 0;
        }
    } else if (mi->layer == 3u && !memcmp(b + 36, "VBRI", 4)) {
        mi->cbr = 0;
        mi->data_off = pos + len;
        mi->nframes = be32(b + 36 + 14);
        mi->frames = mi->nframes * mi->spf;
    }
    if (mi->data_off > mi->data_end)
        mi->data_off = mi->data_end;
    return 0;
}
