/*
 * mp4.c -- MP4/M4A audio track: codec setup, sample tables, gapless trim (mp4.h).
 *
 * Gapless: iTunes writes "iTunSMPB" (encoder delay, padding, valid length, hex,
 * in the track's time scale -- the AAC core rate for HE-AAC); otherwise an edit
 * list (elst media_time + segment duration); otherwise the whole track. AAC: the
 * output rate follows FAAD2's rules (explicit SBR, or implicit up-sampling of
 * streams at 24 kHz or less), and the trims scale with it.
 */
#include <string.h>

#include "mp4.h"

#define T(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

typedef struct {
    mp4_read_at_t rd;
    void *u;
} io_t;

static int rd_at(const io_t *io, uint32_t off, void *buf, uint32_t n)
{
    return io->rd(io->u, off, buf, n) == n ? 0 : -1;
}

/* The child box `type` of [off, end): its body and end. 0 = found. */
static int find(const io_t *io, uint32_t off, uint32_t end, uint32_t type, uint32_t *body, uint32_t *bend)
{
    uint8_t h[16];

    while (off + 8u <= end) {
        uint32_t size, hdr = 8;

        if (rd_at(io, off, h, 8))
            return -1;
        size = be32(h);
        if (size == 1) {                         /* 64-bit size: only small ones are of use */
            if (rd_at(io, off + 8, h + 8, 8) || be32(h + 8))
                return -1;
            size = be32(h + 12);
            hdr = 16;
        } else if (size == 0) {
            size = end - off;
        }
        if (size < hdr || size > end - off)
            return -1;
        if (be32(h + 4) == type) {
            *body = off + hdr;
            *bend = off + size;
            return 0;
        }
        off += size;
    }
    return -1;
}

/* A path of nested boxes, e.g. "mdiaminfstbl" (4 characters each). */
static int path(const io_t *io, uint32_t off, uint32_t end, const char *p, uint32_t *body, uint32_t *bend)
{
    for (; *p; p += 4) {
        if (find(io, off, end, T(p[0], p[1], p[2], p[3]), body, bend))
            return -1;
        off = *body;
        end = *bend;
    }
    return 0;
}

/* ---- AAC AudioSpecificConfig (as FAAD2 reads it) ------------------------ */

typedef struct { const uint8_t *p; uint32_t n, pos; } bits_t;

static uint32_t getb(bits_t *b, int k)
{
    uint32_t v = 0;

    while (k--) {
        uint32_t bit = b->pos < b->n * 8u ? (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1u : 0u;

        v = v << 1 | bit;
        b->pos++;
    }
    return v;
}

static const uint32_t k_sr[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                   16000, 12000, 11025, 8000, 7350 };

static uint32_t sr_of(bits_t *b)
{
    uint32_t i = getb(b, 4);

    return i == 15 ? getb(b, 24) : i < 13 ? k_sr[i] : 0;
}

static uint32_t aot_of(bits_t *b)
{
    uint32_t a = getb(b, 5);

    return a == 31 ? 32u + getb(b, 6) : a;
}

static int asc_parse(mp4_info_t *mi)
{
    bits_t b = { mi->cfg, mi->cfg_len, 0 };
    uint32_t aot = aot_of(&b), rate = sr_of(&b), ch = getb(&b, 4), core_sri, sbr = 2;   /* 2 = not said */

    if (!rate || !ch || ch > 7)
        return -1;                                   /* (a PCE layout: not supported) */
    core_sri = rate;
    if (aot == 5 || aot == 29) {                     /* explicit, hierarchical SBR */
        uint32_t ext = sr_of(&b);

        sbr = ext != core_sri;                       /* same index: down-sampled SBR */
        rate = ext;
        aot = aot_of(&b);
    }
    if (aot != 2)
        return -1;                                   /* AAC LC (+SBR, +PS) only */
    getb(&b, 1);                                     /* frameLengthFlag */
    if (getb(&b, 1))                                 /* dependsOnCoreCoder */
        getb(&b, 14);
    getb(&b, 1);                                     /* extensionFlag (0 for LC) */
    if (sbr == 2 && mi->cfg_len * 8u - b.pos >= 16u && getb(&b, 11) == 0x2B7) {
        uint32_t eaot = aot_of(&b);

        if ((eaot == 5 || eaot == 29) && getb(&b, 1)) {   /* backward-compatible SBR */
            uint32_t ext = sr_of(&b);

            sbr = ext != core_sri;
            rate = ext;
        } else if (eaot == 5 || eaot == 29) {
            sbr = 0;
        }
    }
    if (sbr == 2 && rate <= 24000u) {                /* implicit: FAAD2 up-samples */
        rate *= 2u;
        sbr = 1;
    } else if (sbr == 2) {
        sbr = 0;
    }
    mi->aot = 2;
    mi->sbr = sbr;
    mi->rate = rate;
    mi->channels = 2;                                /* b_aac.c always writes stereo */
    mi->bits = 16;
    return 0;
}

/* esds: ES_Descriptor (3) > DecoderConfigDescriptor (4, object type 0x40) >
 * DecoderSpecificInfo (5) = the AudioSpecificConfig. */
static int esds_parse(const io_t *io, uint32_t off, uint32_t end, mp4_info_t *mi)
{
    uint8_t d[96];
    uint32_t n = end - off, i = 4, len;

    if (n > sizeof d)
        n = sizeof d;
    if (rd_at(io, off, d, n))
        return -1;
    for (;;) {
        uint32_t tag;

        if (i + 2 > n)
            return -1;
        tag = d[i++];
        len = 0;
        for (int k = 0; k < 4 && i < n; k++) {
            uint8_t c = d[i++];

            len = len << 7 | (c & 0x7Fu);
            if (!(c & 0x80))
                break;
        }
        if (tag == 3) {
            uint8_t fl = d[i + 2];

            i += 3 + ((fl & 0x80) ? 2 : 0);          /* ES_ID, flags, dependsOn_ES_ID */
            if (fl & 0x40)
                i += 1u + d[i];                      /* URL */
            if (fl & 0x20)
                i += 2;                              /* OCR_ES_Id */
        } else if (tag == 4) {
            if (d[i] != 0x40)                        /* MPEG-4 audio */
                return -1;
            i += 13;
        } else if (tag == 5) {
            if (len > sizeof mi->cfg || i + len > n)
                return -1;
            memcpy(mi->cfg, d + i, len);
            mi->cfg_len = len;
            return 0;
        } else {
            return -1;
        }
    }
}

/* ---- iTunSMPB ------------------------------------------------------------ */

static uint32_t hexval(const uint8_t **p, const uint8_t *end, uint32_t *hi)
{
    uint64_t v = 0;

    while (*p < end && **p == ' ')
        (*p)++;
    for (; *p < end; (*p)++) {
        uint8_t c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'f' ? (c | 0x20) - 'a' + 10 : -1;

        if (d < 0)
            break;
        v = v << 4 | (uint32_t)d;
    }
    if (hi)
        *hi = (uint32_t)(v >> 32);
    return (uint32_t)v;
}

/* 1 = found: delay, padding, length (in the track's time scale). */
static int smpb(const io_t *io, uint32_t ilst, uint32_t end, uint32_t *delay, uint32_t *pad, uint32_t *len)
{
    uint8_t h[8], v[160];

    while (ilst + 8u <= end) {
        uint32_t size, b, e, nb, ne, db, de;

        if (rd_at(io, ilst, h, 8))
            return 0;
        size = be32(h);
        if (size < 8 || size > end - ilst)
            return 0;
        if (be32(h + 4) == T('-', '-', '-', '-') &&
            !find(io, ilst + 8, ilst + size, T('n', 'a', 'm', 'e'), &nb, &ne) && ne - nb == 12 &&
            !rd_at(io, nb, v, 12) && !memcmp(v + 4, "iTunSMPB", 8) &&
            !find(io, ilst + 8, ilst + size, T('d', 'a', 't', 'a'), &db, &de)) {
            uint32_t n = de - db, hi;
            const uint8_t *p = v + 8, *q;

            if (n > sizeof v)
                n = sizeof v;
            if (rd_at(io, db, v, n))
                return 0;
            q = v + n;
            hexval(&p, q, NULL);                     /* reserved */
            *delay = hexval(&p, q, NULL);
            *pad = hexval(&p, q, NULL);
            *len = hexval(&p, q, &hi);
            return !hi && *len;
        }
        b = ilst + size;
        e = b;
        (void)e;
        ilst = b;
    }
    return 0;
}

/* ---- the track ----------------------------------------------------------- */

static int trak_parse(const io_t *io, uint32_t tb, uint32_t te, mp4_info_t *mi, uint32_t *ts, uint32_t *dur,
                      uint32_t *elst_mt, uint32_t *elst_dur)
{
    uint8_t h[64];
    uint32_t b, e, sb, se, eb, ee, cb, ce;

    if (path(io, tb, te, "mdiahdlr", &b, &e) || rd_at(io, b, h, 12) || be32(h + 8) != T('s', 'o', 'u', 'n'))
        return -1;
    if (path(io, tb, te, "mdiamdhd", &b, &e) || rd_at(io, b, h, 32))
        return -1;
    if (h[0] == 1) {                                 /* version 1: 64-bit times */
        *ts = be32(h + 20);
        *dur = be32(h + 24) ? 0xFFFFFFFFu : be32(h + 28);
    } else {
        *ts = be32(h + 12);
        *dur = be32(h + 16);
    }
    if (path(io, tb, te, "mdiaminfstbl", &sb, &se))
        return -1;
    if (find(io, sb, se, T('s', 't', 's', 'd'), &b, &e) || rd_at(io, b, h, 8 + 36))
        return -1;
    {
        uint32_t entry = b + 8, fmt = be32(h + 12), ver = be16(h + 8 + 16);
        uint32_t kids = entry + 36 + (ver == 1 ? 16u : ver == 2 ? 36u : 0u), eend = entry + be32(h + 8);

        if (fmt == T('m', 'p', '4', 'a')) {
            if (find(io, kids, eend, T('e', 's', 'd', 's'), &cb, &ce) || esds_parse(io, cb, ce, mi) ||
                asc_parse(mi))
                return -2;
            mi->codec = MP4_AAC;
        } else if (fmt == T('a', 'l', 'a', 'c')) {
            if (find(io, kids, eend, T('a', 'l', 'a', 'c'), &cb, &ce) || ce - cb < 28 ||
                rd_at(io, cb + 4, mi->cfg, 24))
                return -2;
            mi->cfg_len = 24;
            mi->codec = MP4_ALAC;
            mi->bits = mi->cfg[5];
            mi->channels = mi->cfg[9];
            mi->rate = be32(mi->cfg + 20);
            if (mi->channels < 1 || mi->channels > 2 || (mi->bits != 16 && mi->bits != 20 &&
                mi->bits != 24 && mi->bits != 32) || !mi->rate)
                return -2;
        } else {
            return -2;
        }
    }
    if (find(io, sb, se, T('s', 't', 's', 'z'), &b, &e) || rd_at(io, b, h, 12))
        return -3;
    mi->stsz_fixed = be32(h + 4);
    mi->nsamples = be32(h + 8);
    mi->stsz_off = b + 12;
    if (!mi->stsz_fixed && (e - mi->stsz_off) / 4u < mi->nsamples)
        return -3;
    if (find(io, sb, se, T('s', 't', 's', 'c'), &b, &e) || rd_at(io, b, h, 8))
        return -3;
    mi->stsc_n = be32(h + 4);
    mi->stsc_off = b + 8;
    if (!mi->stsc_n || (e - mi->stsc_off) / 12u < mi->stsc_n)
        return -3;
    if (!find(io, sb, se, T('s', 't', 'c', 'o'), &b, &e))
        mi->co64 = 0;
    else if (!find(io, sb, se, T('c', 'o', '6', '4'), &b, &e))
        mi->co64 = 1;
    else
        return -3;
    if (rd_at(io, b, h, 8))
        return -3;
    mi->stco_n = be32(h + 4);
    mi->stco_off = b + 8;
    if ((e - mi->stco_off) / (mi->co64 ? 8u : 4u) < mi->stco_n)
        return -3;
    *elst_mt = 0xFFFFFFFFu;
    if (!path(io, tb, te, "edtselst", &eb, &ee) && !rd_at(io, eb, h, 20) && be32(h + 4) >= 1) {
        if (h[0] == 0) {                             /* first edit (an empty one = a delay: ignored) */
            *elst_dur = be32(h + 8);
            *elst_mt = be32(h + 12);
        }
    }
    return 0;
}

int mp4_info(mp4_read_at_t rd, void *u, uint32_t size, mp4_info_t *mi)
{
    io_t io = { rd, u };
    uint32_t mb, me, tb, te, ts = 0, dur = 0, mt, ed, mvts = 0, off;
    uint32_t delay, pad, len;
    uint8_t h[24];
    int r = -2;

    memset(mi, 0, sizeof *mi);
    if (find(&io, 0, size, T('m', 'o', 'o', 'v'), &mb, &me))
        return -1;
    if (!find(&io, mb, me, T('m', 'v', 'h', 'd'), &tb, &te) && !rd_at(&io, tb, h, 24))
        mvts = h[0] == 1 ? be32(h + 20) : be32(h + 12);
    for (off = mb; !find(&io, off, me, T('t', 'r', 'a', 'k'), &tb, &te); off = te) {
        memset(mi, 0, sizeof *mi);
        if ((r = trak_parse(&io, tb, te, mi, &ts, &dur, &mt, &ed)) == 0)
            break;
    }
    if (r)
        return r;
    {
        uint32_t ub, ue, mb2, me2;

        if (!find(&io, mb, me, T('u', 'd', 't', 'a'), &ub, &ue) && !find(&io, ub, ue, T('m', 'e', 't', 'a'), &mb2, &me2) &&
            !find(&io, mb2 + 4, me2, T('i', 'l', 's', 't'), &ub, &ue)) {
            mi->ilst_off = ub;
            mi->ilst_len = ue - ub;
        }
    }
    /* trims, in output frames. FAAD2 outputs nothing for the first AAC frame (its
     * overlap), so that frame is already off the encoder delay; with SBR its QMF
     * banks add 962 output samples (measured against the encoder's input). */
    {
        uint32_t f = mi->codec == MP4_AAC && ts && mi->rate >= ts * 2u ? 2u : 1u;   /* SBR: core-rate tables */
        uint32_t eaten = mi->codec == MP4_AAC ? 1024u * f : 0u;

        if (mi->ilst_off && smpb(&io, mi->ilst_off, mi->ilst_off + mi->ilst_len, &delay, &pad, &len)) {
            mi->trim = delay * f;
            mi->frames = len * f;
        } else if (mt != 0xFFFFFFFFu && mvts) {
            mi->trim = mt * f;
            mi->frames = (uint32_t)((uint64_t)ed * ts / mvts) * f;
        } else {
            mi->frames = dur == 0xFFFFFFFFu ? 0 : dur * f;
        }
        mi->trim = mi->trim > eaten ? mi->trim - eaten : 0;
        if (mi->codec == MP4_AAC && mi->sbr)
            mi->trim += 962;                         /* FAAD2's SBR filterbank delay (output samples) */
    }
    /* total payload, for the stream length */
    if (mi->stsz_fixed) {
        mi->data_bytes = mi->stsz_fixed * mi->nsamples;
    } else {
        uint8_t buf[256];

        for (uint32_t i = 0; i < mi->nsamples; ) {
            uint32_t k = mi->nsamples - i > 64u ? 64u : mi->nsamples - i;

            if (rd_at(&io, mi->stsz_off + i * 4u, buf, k * 4u))
                return -1;
            for (uint32_t j = 0; j < k; j++)
                mi->data_bytes += be32(buf + j * 4);
            i += k;
        }
    }
    /* A cut (or damaged) file whose tables still name samples past its end: keep the
     * samples that lie inside it, and play whatever they decode to (frames unknown).
     * One read when the last chunk starts inside the file; a walk only otherwise. */
    {
        uint8_t b[8];
        uint32_t last, w = mi->co64 ? 8u : 4u;

        if (!mi->stco_n || rd_at(&io, mi->stco_off + (mi->stco_n - 1u) * w, b, w))
            return -3;
        last = mi->co64 ? (be32(b) ? 0xFFFFFFFFu : be32(b + 4)) : be32(b);
        if (last >= size || mi->data_bytes > size) {
            mp4_cur_t c;
            uint32_t n = 0, bytes = 0, o, sz;

            mp4_cursor(&c);
            while (mp4_next(rd, u, mi, &c, &o, &sz) == 1 && o <= size && sz <= size - o) {
                n++;
                bytes += sz;
            }
            if (!n)
                return -3;
            mi->nsamples = n;
            mi->data_bytes = bytes;
            mi->frames = 0;
        }
    }
    return 0;
}

/* ---- walking the samples --------------------------------------------------- */

void mp4_cursor(mp4_cur_t *c)
{
    memset(c, 0, sizeof *c);
    c->sz_base = 0xFFFFFFFFu;
}

int mp4_next(mp4_read_at_t rd, void *u, const mp4_info_t *mi, mp4_cur_t *c, uint32_t *off, uint32_t *size)
{
    io_t io = { rd, u };
    uint8_t b[12];

    if (c->sample >= mi->nsamples)
        return 0;
    if (!c->left_in_chunk) {                         /* next chunk: its offset, its sample count */
        if (c->chunk >= mi->stco_n)
            return -1;
        if (c->chunk == 0 || c->chunk + 1u == c->next_first) {
            if (c->chunk == 0)
                c->stsc_i = 0;
            else
                c->stsc_i++;
            if (rd_at(&io, mi->stsc_off + c->stsc_i * 12u, b, 12))
                return -1;
            c->per_chunk = be32(b + 4);
            c->next_first = 0xFFFFFFFFu;
            if (c->stsc_i + 1u < mi->stsc_n) {
                if (rd_at(&io, mi->stsc_off + (c->stsc_i + 1u) * 12u, b, 4))
                    return -1;
                c->next_first = be32(b);
            }
        }
        if (mi->co64) {
            if (rd_at(&io, mi->stco_off + c->chunk * 8u, b, 8) || be32(b))
                return -1;
            c->off = be32(b + 4);
        } else {
            if (rd_at(&io, mi->stco_off + c->chunk * 4u, b, 4))
                return -1;
            c->off = be32(b);
        }
        c->chunk++;
        c->left_in_chunk = c->per_chunk;
        if (!c->left_in_chunk)
            return -1;
    }
    if (mi->stsz_fixed) {
        *size = mi->stsz_fixed;
    } else {
        uint32_t i = c->sample;

        if (c->sz_base == 0xFFFFFFFFu || i < c->sz_base || i >= c->sz_base + 64u) {
            uint32_t k = mi->nsamples - i > 64u ? 64u : mi->nsamples - i;
            uint8_t buf[256];

            if (rd_at(&io, mi->stsz_off + i * 4u, buf, k * 4u))
                return -1;
            for (uint32_t j = 0; j < k; j++)
                c->sz[j] = be32(buf + j * 4);
            c->sz_base = i;
        }
        *size = c->sz[i - c->sz_base];
    }
    *off = c->off;
    c->off += *size;
    c->left_in_chunk--;
    c->sample++;
    return 1;
}

/* Sizes of samples [a, b) added up (stsz read 256 entries at a time). */
static int sizes(const io_t *io, const mp4_info_t *mi, uint32_t a, uint32_t b, uint32_t *sum)
{
    static uint8_t buf[1024];

    *sum = 0;
    if (mi->stsz_fixed) {
        *sum = mi->stsz_fixed * (b - a);
        return 0;
    }
    while (a < b) {
        uint32_t k = b - a > 256u ? 256u : b - a;

        if (rd_at(io, mi->stsz_off + a * 4u, buf, k * 4u))
            return -1;
        for (uint32_t j = 0; j < k; j++)
            *sum += be32(buf + j * 4);
        a += k;
    }
    return 0;
}

int mp4_seek(mp4_read_at_t rd, void *u, const mp4_info_t *mi, mp4_cur_t *c, uint32_t k, uint32_t *before)
{
    io_t io = { rd, u };
    uint8_t b[12];
    uint32_t acc = 0, in_chunk;

    if (k >= mi->nsamples)
        return -1;
    mp4_cursor(c);
    if (sizes(&io, mi, 0, k, before))
        return -1;
    for (uint32_t i = 0; i < mi->stsc_n; i++) {
        uint32_t first, per, next, run;

        if (rd_at(&io, mi->stsc_off + i * 12u, b, 12))
            return -1;
        first = be32(b);
        per = be32(b + 4);
        next = mi->stco_n + 1u;
        if (i + 1u < mi->stsc_n) {
            if (rd_at(&io, mi->stsc_off + (i + 1u) * 12u, b, 4))
                return -1;
            next = be32(b);
        }
        if (!first || next < first || !per)
            return -3;
        run = (next - first) * per;
        if (k - acc >= run) {
            acc += run;
            continue;
        }
        {   /* chunk (0-based) first - 1 + (k - acc) / per, the sample's place in it */
            uint32_t ch = first - 1u + (k - acc) / per, j = (k - acc) % per;

            if (ch >= mi->stco_n)
                return -3;
            if (mi->co64) {
                if (rd_at(&io, mi->stco_off + ch * 8u, b, 8) || be32(b))
                    return -1;
                c->off = be32(b + 4);
            } else {
                if (rd_at(&io, mi->stco_off + ch * 4u, b, 4))
                    return -1;
                c->off = be32(b);
            }
            if (sizes(&io, mi, k - j, k, &in_chunk))
                return -1;
            c->off += in_chunk;
            c->sample = k;
            c->chunk = ch + 1u;              /* (the next chunk mp4_next loads) */
            c->left_in_chunk = per - j;
            c->stsc_i = i;
            c->per_chunk = per;
            c->next_first = next == mi->stco_n + 1u && i + 1u >= mi->stsc_n ? 0xFFFFFFFFu : next;
            return 0;
        }
    }
    return -3;
}
