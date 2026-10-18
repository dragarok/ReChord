/*
 * seek.c -- the coded unit to start a seek at (seek.h).
 */
#include <string.h>

#include "seek.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

#define CHUNK 4096u                  /* bytes read at a time while looking for a header */

/* ---- the search over a file of units that know their first sample -------------------------- */

typedef struct {
    seek_read_at_t rd;
    void *u;
    /* 1 = a unit header at p (avail bytes from p in buf), *s its first sample, *len its samples;
     * 0 = not one */
    int (*hdr)(void *ctx, const uint8_t *p, uint32_t avail, uint32_t off, uint64_t *s, uint32_t *len);
    void *ctx;
    uint32_t need;                   /* bytes a header check looks at */
    uint8_t buf[CHUNK + 32];
} search_t;

static search_t S;                   /* (one at a time: 4 KiB, not on the stack) */

/* The first unit at an offset in [from, to) whose first sample lies in [smin, smax]: 1 (*o, *s),
 * 0 none, -1 a read failed. */
static int probe(search_t *P, uint32_t from, uint32_t to, uint64_t smin, uint64_t smax, uint32_t *o, uint64_t *s,
                 uint32_t *len)
{
    while (from < to) {
        uint32_t want = to - from + P->need, n, i;

        if (want > sizeof P->buf)
            want = sizeof P->buf;
        n = P->rd(P->u, from, P->buf, want);
        if (n < P->need)
            return 0;                    /* (the end of the file, or a read that failed) */
        for (i = 0; i + P->need <= n && from + i < to; i++)
            if (P->hdr(P->ctx, P->buf + i, n - i, from + i, s, len) && *s >= smin && *s <= smax) {
                *o = from + i;
                return 1;
            }
        from += i;
        if (n < want)
            break;                       /* the end of the file */
    }
    return 0;
}

/* Bracket the target between a unit at or before it (lo) and the units after it (hi), by
 * interpolation for six probes, then by halves, until lo's unit is at most `units` units before
 * the one that holds the target (1: lo holds it). */
static int search(search_t *P, uint32_t lo, uint32_t hi, uint64_t hi_s, uint64_t t, uint32_t units,
                  uint32_t *off, uint64_t *first)
{
    uint64_t lo_s = 0, s;
    uint32_t o, lo_n, n;
    int r = probe(P, lo, hi, 0, hi_s, &o, &s, &lo_n);

    if (r <= 0 || s > t)
        return r < 0 ? -1 : -2;      /* (no unit at all, or the first one starts past the target) */
    lo = o;
    lo_s = s;
    for (int it = 0; it < 40 && t - lo_s >= (uint64_t)units * lo_n && hi - lo > 1u; it++) {
        uint64_t g = it < 6 && hi_s > lo_s ? lo + (t - lo_s) * (hi - lo) / (hi_s - lo_s) : lo + (hi - lo) / 2u;

        if (g <= lo)
            g = lo + 1u;
        if (g >= hi)
            g = hi - 1u;
        r = probe(P, (uint32_t)g, hi, lo_s + 1u, hi_s, &o, &s, &n);
        if (r < 0)
            return -1;
        if (r == 0) {                /* no unit starts in [g, hi): the first one at g or after is hi's */
            hi = (uint32_t)g;
        } else if (s > t) {          /* none in [g, o), and o's starts after the target */
            hi = (uint32_t)g;
            hi_s = s;
        } else {
            lo = o;
            lo_s = s;
            lo_n = n;
        }
    }
    *off = lo;
    *first = lo_s;
    return 0;
}

/* ---- FLAC --------------------------------------------------------------------------------------- */

uint8_t seek_crc8(const uint8_t *p, uint32_t n)
{
    uint8_t c = 0;

    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (uint8_t)(c & 0x80 ? c << 1 ^ 0x07 : c << 1);
    }
    return c;
}

typedef struct {
    uint32_t rate, channels, bits, min_bs, max_bs;
} flac_si_t;

static const uint32_t k_flac_rate[12] = { 0, 88200, 176400, 192000, 8000, 16000, 22050, 24000, 32000, 44100,
                                          48000, 96000 };
static const uint8_t k_flac_bits[8] = { 0, 8, 12, 0, 16, 20, 24, 32 };

/* A frame header (sync 0xFFF8/9, the fields STREAMINFO fixes, CRC-8) at p: 1 and its first sample. */
static int flac_hdr(void *ctx, const uint8_t *p, uint32_t avail, uint32_t off, uint64_t *s, uint32_t *len)
{
    const flac_si_t *si = ctx;
    uint32_t bs = p[2] >> 4, sr = p[2] & 15u, ca = p[3] >> 4, ss = p[3] >> 1 & 7u, i = 5, extra;
    uint64_t num;

    (void)off;
    if (p[0] != 0xFF || (p[1] & 0xFE) != 0xF8 || !bs || sr == 15u || ca > 10u || ss == 3u || (p[3] & 1u))
        return 0;
    if ((ca < 8u ? ca + 1u : 2u) != si->channels || (ss && k_flac_bits[ss] != si->bits) ||
        (sr && sr < 12u && k_flac_rate[sr] != si->rate))
        return 0;
    /* the frame (fixed blocking) or sample (variable) number, UTF-8 coded */
    if (p[4] < 0x80) {
        num = p[4];
        extra = 0;
    } else {
        uint32_t lead = p[4];

        extra = lead >= 0xFE ? 6u : lead >= 0xFC ? 5u : lead >= 0xF8 ? 4u : lead >= 0xF0 ? 3u : lead >= 0xE0 ? 2u :
                lead >= 0xC0 ? 1u : 9u;
        if (extra == 9u || (lead == 0xFF))
            return 0;
        num = lead & (0x3Fu >> extra);
        for (uint32_t k = 0; k < extra; k++) {
            if ((p[5 + k] & 0xC0) != 0x80)
                return 0;
            num = num << 6 | (p[5 + k] & 0x3Fu);
        }
    }
    i += extra;
    {
        uint32_t size = bs == 1u ? 192u : bs <= 5u ? 576u << (bs - 2u) : bs >= 8u ? 256u << (bs - 8u) : 0u;

        if (bs == 6u)
            size = p[i++] + 1u;
        else if (bs == 7u)
            size = be16(p + i) + 1u, i += 2;
        if (sr == 12u)
            i++;
        else if (sr == 13u || sr == 14u)
            i += 2;
        if (i + 1u > avail || seek_crc8(p, i) != p[i] || !size || size > 65535u)
            return 0;
        *len = size;
        if (p[1] & 1u)
            *s = num;                                    /* variable blocking: the sample number */
        else
            *s = num * (si->min_bs == si->max_bs ? si->max_bs : size);
    }
    return 1;
}

int seek_flac(seek_read_at_t rd, void *u, uint32_t lo, uint32_t hi, const uint8_t *si, uint64_t t,
              uint32_t *off, uint64_t *first)
{
    static flac_si_t f;
    uint64_t total;

    f.min_bs = be16(si);
    f.max_bs = be16(si + 2);
    f.rate = (uint32_t)si[10] << 12 | (uint32_t)si[11] << 4 | si[12] >> 4;
    f.channels = (si[12] >> 1 & 7u) + 1u;
    f.bits = ((si[12] & 1u) << 4 | si[13] >> 4) + 1u;
    total = (uint64_t)(si[13] & 15u) << 32 | (uint32_t)si[14] << 24 | (uint32_t)si[15] << 16 | (uint32_t)si[16] << 8 | si[17];
    memset(&S, 0, sizeof S - sizeof S.buf);
    S.rd = rd;
    S.u = u;
    S.hdr = flac_hdr;
    S.ctx = &f;
    S.need = 16;                     /* the longest header: 4 + 7 + 2 + 2 + CRC */
    return search(&S, lo, hi, total ? total : ~(uint64_t)0 >> 1, t, 2u, off, first);
}

/* ---- WavPack -------------------------------------------------------------------------------------- */

typedef struct {
    seek_read_at_t rd;
    void *u;
    uint32_t hi;
} wv_ctx_t;

#define WV_INITIAL 0x800u

/* A block header ("wvpk", version, an initial block with samples) followed by another block header
 * or the end of the stream. */
static int wv_hdr(void *ctx, const uint8_t *p, uint32_t avail, uint32_t off, uint64_t *s, uint32_t *len)
{
    const wv_ctx_t *c = ctx;
    uint32_t size = le32(p + 4), ver = le16(p + 8), n = le32(p + 20), fl = le32(p + 24), next;
    uint8_t b[4];

    (void)avail;
    if (memcmp(p, "wvpk", 4) || ver < 0x402u || ver > 0x410u || !n || n > 0x100000u || !(fl & WV_INITIAL) ||
        size < 24u || size > 0x200000u || (size & 1u))
        return 0;
    next = off + 8u + size;
    if (next + 4u <= c->hi && (c->rd(c->u, next, b, 4) != 4 || memcmp(b, "wvpk", 4)))
        return 0;
    *s = (uint64_t)p[10] << 32 | le32(p + 16);
    *len = n;
    return 1;
}

int seek_wavpack(seek_read_at_t rd, void *u, uint32_t lo, uint32_t hi, uint64_t total, uint64_t t,
                 uint32_t *off, uint64_t *first)
{
    static wv_ctx_t c;

    c.rd = rd;
    c.u = u;
    c.hi = hi;
    memset(&S, 0, sizeof S - sizeof S.buf);
    S.rd = rd;
    S.u = u;
    S.hdr = wv_hdr;
    S.ctx = &c;
    S.need = 32;
    return search(&S, lo, hi, total ? total : ~(uint64_t)0 >> 1, t, 1u, off, first);   /* (blocks are long) */
}

/* ---- MPEG audio ----------------------------------------------------------------------------------- */

/* Three valid headers chained from p, with the first frame's version, layer and rate. */
static int mp3_chain(seek_read_at_t rd, void *u, const mp3_info_t *mi, const uint8_t *ref, uint32_t at)
{
    uint8_t h[4];

    for (int k = 0; k < 3; k++) {
        uint32_t len;

        if (at + 4u > mi->data_end)
            return k > 0;            /* the stream's last frame */
        if (rd(u, at, h, 4) != 4)
            return 0;
        len = mp3_frame_len(h, 0, 0, 0);
        if (!len || (h[1] & 0xFE) != (ref[1] & 0xFE) || (h[2] & 0x0C) != (ref[2] & 0x0C))
            return 0;
        at += len;
    }
    return 1;
}

int seek_mp3(seek_read_at_t rd, void *u, const mp3_info_t *mi, uint32_t want, uint32_t *off, uint32_t *index)
{
    uint8_t *b = S.buf, ref[4];
    uint32_t span = mi->data_end - mi->data_off, est, from, n, best = 0, bestd = 0xFFFFFFFFu;
    uint64_t avg;                    /* bytes a frame, Q16 */
    int toc = !mi->cbr && mi->has_toc && mi->nframes && mi->xing_bytes;

    if (!want) {
        *off = mi->data_off;
        *index = 0;
        return 0;
    }
    if (rd(u, mi->data_off, ref, 4) != 4 || !mp3_frame_len(ref, 0, 0, 0))
        return -1;
    if (mi->nframes)
        avg = ((uint64_t)span << 16) / mi->nframes;
    else if (mi->rate)               /* (CBR without a frame count: spf / 8 x the bit rate / the rate) */
        avg = ((uint64_t)mi->spf * mi->bitrate * 1000u << 16) / (8u * mi->rate);
    else
        avg = 0;
    if (!avg)
        return -1;
    if (toc && want < mi->nframes) {
        /* percent of the time -> the TOC's byte fraction, interpolated between its points */
        uint64_t pq = (uint64_t)want * 100u * 256u / mi->nframes;        /* percent, Q8 */
        uint32_t i = (uint32_t)(pq >> 8), fr = (uint32_t)(pq & 255u);
        uint32_t a = mi->toc[i < 100u ? i : 99u], z = i + 1u < 100u ? mi->toc[i + 1u] : 256u;

        /* (+ half a TOC step: its points are rounded down, which put the estimate 1-2 frames early) */
        est = mi->xing_off + (uint32_t)(((uint64_t)a * 256u + (uint64_t)(z - a) * fr + 128u) * mi->xing_bytes >> 16);
    } else {
        est = mi->data_off + (uint32_t)(((uint64_t)want * avg) >> 16);
    }
    if (est < mi->data_off)
        est = mi->data_off;
    if (est >= mi->data_end)
        return -1;
    /* the valid frame start nearest est, within a window either side */
    from = est > mi->data_off + CHUNK / 2u ? est - CHUNK / 2u : mi->data_off;
    n = rd(u, from, b, CHUNK);
    for (uint32_t i = 0; i + 4u <= n; i++) {
        uint32_t at = from + i, d = at > est ? at - est : est - at;

        if (b[i] != 0xFF || d >= bestd || !mp3_frame_len(b + i, 0, 0, 0) || (b[i + 1] & 0xFE) != (ref[1] & 0xFE) ||
            (b[i + 2] & 0x0C) != (ref[2] & 0x0C) || !mp3_chain(rd, u, mi, ref, at))
            continue;
        best = at;
        bestd = d;
    }
    if (bestd == 0xFFFFFFFFu)
        return -2;
    *off = best;
    if (toc)
        *index = want;               /* (VBR: the TOC's estimate) */
    else
        *index = (uint32_t)((((uint64_t)(best - mi->data_off) << 16) + avg / 2u) / avg);
    return 0;
}
