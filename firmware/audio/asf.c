/*
 * asf.c -- ASF (WMA) header and data packets (asf.h).
 *
 * Header: the File Properties object (packet size, duration, preroll), the first
 * Stream Properties object of a WMA audio stream (in the header or in an Extended
 * Stream Properties object of the Header Extension), the Data object. Encrypted
 * (DRM) files are refused. Packets: error-correction data, the payload parsing
 * information, single or multiple payloads, compressed payloads (sub-payloads),
 * padding (given, or implied by a short packet length).
 */
#include <string.h>

#include "asf.h"

static const uint8_t G_HEADER[16]   = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
static const uint8_t G_DATA[16]     = { 0x36, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                        0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
static const uint8_t G_FILE[16]     = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };
static const uint8_t G_STREAM[16]   = { 0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };
static const uint8_t G_AUDIO[16]    = { 0x40, 0x9E, 0x69, 0xF8, 0x4D, 0x5B, 0xCF, 0x11,
                                        0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B };
static const uint8_t G_HDREXT[16]   = { 0xB5, 0x03, 0xBF, 0x5F, 0x2E, 0xA9, 0xCF, 0x11,
                                        0x8E, 0xE3, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };
static const uint8_t G_EXTSTREAM[16] = { 0xCB, 0xA5, 0xE6, 0x14, 0x72, 0xC6, 0x32, 0x43,
                                         0x83, 0x99, 0xA9, 0x69, 0x52, 0x06, 0x5B, 0x5A };
static const uint8_t G_DRM[16]      = { 0xFB, 0xB3, 0x11, 0x22, 0x23, 0xBD, 0xD2, 0x11,
                                        0xB4, 0xB7, 0x00, 0xA0, 0xC9, 0x55, 0xFC, 0x6E };
static const uint8_t G_DRMEXT[16]   = { 0x14, 0xE6, 0x8A, 0x29, 0x22, 0x26, 0x17, 0x4C,
                                        0xB9, 0x35, 0xDA, 0xE0, 0x7E, 0xE9, 0x28, 0x9C };

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

typedef struct {
    asf_read_at_t rd;
    void *u;
    asf_info_t *ai;
    int have_file, have_audio, drm;
    uint64_t play, preroll_ms;
    uint32_t min_pkt, max_pkt;
} ctx_t;

static int rd_at(const ctx_t *c, uint32_t off, void *buf, uint32_t n)
{
    return c->rd(c->u, off, buf, n) == n ? 0 : -1;
}

static int is_wma(uint32_t tag)
{
    return tag >= ASF_WMAV1 && tag <= ASF_WMALOSSLESS;
}

/* A Stream Properties object's body at `off` (`len` bytes): the first WMA audio stream. */
static int stream_props(ctx_t *c, uint32_t off, uint32_t len)
{
    uint8_t b[54 + 18 + ASF_EXTRA_MAX];
    uint32_t tsd, n;
    asf_cfg_t *cfg = &c->ai->cfg;

    if (c->have_audio || len < 54 + 18)
        return 0;
    n = len < sizeof b ? len : sizeof b;
    if (rd_at(c, off, b, n))
        return -1;
    if (memcmp(b, G_AUDIO, 16))
        return 0;                                    /* not audio */
    tsd = le32(b + 40);                              /* type-specific data: WAVEFORMATEX */
    if (tsd < 18 || tsd > len - 54 || !is_wma(le16(b + 54)))
        return 0;
    if (le16(b + 48) & 0x8000u)
        c->drm = 1;                                  /* encrypted content flag */
    cfg->stream = (uint16_t)(le16(b + 48) & 0x7Fu);
    cfg->codec_id = (uint16_t)le16(b + 54);
    cfg->channels = (uint16_t)le16(b + 56);
    cfg->rate = le32(b + 58);
    cfg->bitrate = le32(b + 62) * 8u;
    cfg->block_align = (uint16_t)le16(b + 66);
    cfg->bits = (uint16_t)le16(b + 68);
    cfg->extra_len = (uint16_t)le16(b + 70);
    if (cfg->extra_len > tsd - 18)
        cfg->extra_len = (uint16_t)(tsd - 18);
    if (cfg->extra_len > ASF_EXTRA_MAX)
        cfg->extra_len = ASF_EXTRA_MAX;
    if (cfg->extra_len > n - 72)
        return -1;
    memcpy(cfg->extra, b + 72, cfg->extra_len);
    c->have_audio = 1;
    return 0;
}

/* Extended Stream Properties: an optional Stream Properties object at its end. */
static int ext_stream_props(ctx_t *c, uint32_t off, uint32_t end)
{
    uint8_t b[24];
    uint32_t p = off + 64, names, exts;

    if (p > end || rd_at(c, off + 60, b, 4))
        return -1;
    names = le16(b);
    exts = le16(b + 2);
    while (names--) {                                /* language index u16, length u16, name */
        if (p + 4 > end || rd_at(c, p, b, 4))
            return -1;
        p += 4 + le16(b + 2);
    }
    while (exts--) {                                 /* GUID, data size u16, info length u32, info */
        if (p + 22 > end || rd_at(c, p, b, 22))
            return -1;
        p += 22 + le32(b + 18);
    }
    if (p + 24 <= end) {
        uint64_t sz;

        if (rd_at(c, p, b, 24))
            return -1;
        sz = le64(b + 16);
        if (!memcmp(b, G_STREAM, 16) && sz >= 24 && sz <= end - p)
            return stream_props(c, p + 24, (uint32_t)sz - 24);
    }
    return 0;
}

/* The objects in [off, end); `ext`: inside the Header Extension. */
static int objects(ctx_t *c, uint32_t off, uint32_t end, int ext)
{
    uint8_t b[104];

    while (off + 24 <= end) {
        uint64_t sz;
        uint32_t body;

        if (rd_at(c, off, b, 24))
            return -1;
        sz = le64(b + 16);
        if (sz < 24 || sz > end - off)
            return -4;
        body = off + 24;
        if (!memcmp(b, G_FILE, 16) && !ext) {
            if (sz < 24 + 80 || rd_at(c, body, b, 80))
                return -1;
            c->play = le64(b + 40);                  /* 100 ns units, preroll included */
            c->preroll_ms = le64(b + 56);
            c->min_pkt = le32(b + 68);
            c->max_pkt = le32(b + 72);
            c->have_file = 1;
        } else if (!memcmp(b, G_STREAM, 16)) {
            if (stream_props(c, body, (uint32_t)sz - 24))
                return -1;
        } else if (!memcmp(b, G_HDREXT, 16) && !ext) {
            if (sz >= 24 + 22) {
                int r = objects(c, body + 22, off + (uint32_t)sz, 1);

                if (r)
                    return r;
            }
        } else if (!memcmp(b, G_EXTSTREAM, 16) && ext) {
            if (ext_stream_props(c, body, off + (uint32_t)sz))
                return -1;
        } else if (!memcmp(b, G_DRM, 16) || !memcmp(b, G_DRMEXT, 16)) {
            c->drm = 1;
        }
        off += (uint32_t)sz;
    }
    return 0;
}

int asf_info(asf_read_at_t rd, void *u, uint32_t size, asf_info_t *ai)
{
    ctx_t c;
    uint8_t b[50];
    uint64_t hsz, dsz, n, ms, announced;
    uint32_t doff;
    int r;

    memset(ai, 0, sizeof *ai);
    memset(&c, 0, sizeof c);
    c.rd = rd;
    c.u = u;
    c.ai = ai;
    if (size < 30 + 50 || rd(u, 0, b, 30) != 30 || memcmp(b, G_HEADER, 16))
        return -1;
    hsz = le64(b + 16);
    if (hsz < 30 || hsz > size - 50)
        return -1;
    r = objects(&c, 30, (uint32_t)hsz, 0);
    if (r)
        return r;
    if (!c.have_audio)
        return -2;
    if (c.drm)
        return -3;
    if (!c.have_file || !c.min_pkt || c.min_pkt != c.max_pkt || c.min_pkt > 0x10000u)
        return -4;                                   /* variable packet sizes: a stream, not a file */
    ai->cfg.packet_size = c.min_pkt;
    doff = (uint32_t)hsz;
    if (rd(u, doff, b, 50) != 50 || memcmp(b, G_DATA, 16))
        return -4;
    dsz = le64(b + 16);
    n = announced = le64(b + 40);                    /* total data packets (0: unknown, broadcast) */
    ai->data_off = doff + 50;
    if (dsz >= 50 && (dsz - 50) / c.min_pkt < n)
        n = (dsz - 50) / c.min_pkt;
    if (!n || n > (size - ai->data_off) / c.min_pkt)
        n = (size - ai->data_off) / c.min_pkt;       /* cut short, or no count given */
    ai->npackets = (uint32_t)n;
    ai->data_bytes = ai->npackets * c.min_pkt;
    ms = c.play / 10000u;
    ms = ms > c.preroll_ms ? ms - c.preroll_ms : 0;
    ai->ms = (uint32_t)ms;
    ai->frames = c.play > c.preroll_ms * 10000u ?
                 (uint32_t)((c.play - c.preroll_ms * 10000u) * ai->cfg.rate / 10000000u) : 0;
    if (announced > ai->npackets) {                  /* a cut file: its share of the duration */
        ai->ms = (uint32_t)((uint64_t)ai->ms * ai->npackets / announced);
        ai->frames = (uint32_t)((uint64_t)ai->frames * ai->npackets / announced);
    }
    if (!ai->cfg.channels || !ai->cfg.rate || !ai->npackets)
        return -2;
    return 0;
}

/* ---- data packets ------------------------------------------------------- */

#ifndef RECHORD_AP_BUILD                /* the packet walk is core B's (b_asf.c): not in core A's image */
/* A field of length type t (0: none, 1: byte, 2: word, 3: dword); NULL past `end`. */
static const uint8_t *field(const uint8_t *p, const uint8_t *end, unsigned t, uint32_t *v)
{
    static const uint8_t len[4] = { 0, 1, 2, 4 };
    unsigned n = len[t & 3u];

    if (!p || p + n > end)
        return NULL;
    *v = n == 0 ? 0 : n == 1 ? p[0] : n == 2 ? le16(p) : le32(p);
    return p + n;
}

int asf_pkt_begin(asf_pkt_t *it, const uint8_t *pkt, uint32_t size)
{
    const uint8_t *p = pkt, *end = pkt + size;
    uint32_t plen, seq, pad, flags;

    memset(it, 0, sizeof *it);
    if (size < 8)
        return -1;
    if (p[0] & 0x80u) {                              /* error correction data */
        if (p[0] & 0x60u)
            return -1;                               /* its length must be a plain count */
        p += 1 + (p[0] & 0x0Fu);
    }
    if (p + 2 > end)
        return -1;
    flags = p[0];
    it->prop = p[1];
    p += 2;
    p = field(p, end, flags >> 5, &plen);            /* packet length */
    p = field(p, end, flags >> 1, &seq);             /* sequence (unused) */
    p = field(p, end, flags >> 3, &pad);             /* padding length */
    if (!p || p + 6 > end)
        return -1;
    p += 6;                                          /* send time, duration */
    if (!((flags >> 5) & 3u) || plen > size)
        plen = size;                                 /* none given: the header's size */
    if (pad > plen || pkt + plen - pad < p)
        return -1;
    it->end = pkt + plen - pad;                      /* a short packet length means more padding */
    it->multiple = flags & 1u;
    if (it->multiple) {
        if (p >= it->end)
            return -1;
        it->left = p[0] & 0x3Fu;
        it->len_type = p[0] >> 6;
        p++;
    } else {
        it->left = 1;
    }
    it->p = p;
    return 0;
}

int asf_pkt_next(asf_pkt_t *it, asf_payload_t *pl)
{
    const uint8_t *p = it->p;
    uint32_t mon, moff, rlen, len;

    if (it->sub && it->sub < it->sub_end) {          /* compressed payload: next sub-payload */
        len = it->sub[0];
        if (it->sub + 1 + len > it->sub_end)
            return -1;
        *pl = it->sub_pl;
        pl->data = it->sub + 1;
        pl->len = pl->obj_size = len;
        it->sub_pl.obj++;
        it->sub += 1 + len;
        return 1;
    }
    it->sub = NULL;
    if (!it->left)
        return 0;
    it->left--;
    if (!p || p >= it->end)
        return -1;
    pl->stream = p[0] & 0x7Fu;
    pl->key = p[0] >> 7;
    p++;
    p = field(p, it->end, it->prop >> 4, &mon);      /* media object number */
    p = field(p, it->end, it->prop >> 2, &moff);     /* offset in it (compressed: presentation time) */
    p = field(p, it->end, it->prop, &rlen);          /* replicated data length */
    if (!p || rlen > (uint32_t)(it->end - p))
        return -1;
    pl->obj = mon;
    pl->obj_off = moff;
    pl->obj_size = rlen >= 8 ? le32(p) : 0;          /* replicated data: object size, time, ... */
    p += rlen;
    if (it->multiple) {
        p = field(p, it->end, it->len_type ? it->len_type : 2u, &len);
        if (!p || len > (uint32_t)(it->end - p))
            return -1;
    } else {
        len = (uint32_t)(it->end - p);
    }
    it->p = p + len;
    if (rlen == 1) {                                 /* compressed: sub-payloads, each a whole object */
        it->sub = p;
        it->sub_end = p + len;
        it->sub_pl = *pl;
        it->sub_pl.obj_off = 0;
        return asf_pkt_next(it, pl);
    }
    pl->data = p;
    pl->len = len;
    if (!pl->obj_size)
        pl->obj_size = moff + len;
    return 1;
}
#endif
