/*
 * text.c -- the text engine (text.h): font tables, UTF-8, clusters and Devanagari
 * shaping, measuring, ellipsis, wrap, and the 2-bit-alpha band blit.
 *
 * Pure C99, no hardware: the firmware and the host tests compile it unchanged. The
 * shaping mirrors tools/gen_font.py's Deva.sim() step for step (the generator checks
 * that simulation against HarfBuzz; test_text.c checks this file against both).
 */
#include <stddef.h>
#include <string.h>

#include "text.h"
#include "fonts/latin_compose.h"

/* ---- fonts --------------------------------------------------------------------- */

enum { F_S8, F_B13, F_M13, F_T17, F_D28, F_COUNT };

const font_t font_s8 = { F_S8 }, font_b13 = { F_B13 }, font_m13 = { F_M13 },
             font_t17 = { F_T17 }, font_d28 = { F_D28 };

static const char k_names[F_COUNT][4] = { "S8", "B13", "M13", "T17", "D28" };

typedef struct {                      /* a glyph record, 8 bytes, as in the blob */
    uint16_t bits;                    /* offset of its rows in the bitmaps */
    uint8_t w, h;
    int8_t left, top;                 /* ink box from the pen / the baseline (y down) */
    uint8_t adv, stem;                /* advance; i-matra target (0 = 3/4 of adv) */
} grec_t;

typedef struct {
    const uint16_t *codes;
    const grec_t *recs;
    const uint8_t *bits;
    uint16_t n, fallback, ell;        /* glyphs; index of the box; of U+2026 (or 0xFFFF) */
    uint8_t px, line, base, up, down, ascii;
    /* Devanagari (shape == NULL: none) */
    const uint16_t *shape;
    const uint16_t *half, *conj, *lig, *dec;
    uint16_t n_half, n_conj, n_lig, n_dec, reph;
    const uint8_t *pos;               /* 6-byte entries: u16 base, u16 mark, s8 dx, s8 dy */
    const int16_t *imat;              /* pairs: s16 reach, u16 code */
    uint16_t n_pos, n_imat;
    int16_t imat_tol;
} fdata_t;

static fdata_t s_fd[F_COUNT];
static uint8_t s_have[F_COUNT];       /* 1 = own data, 0 = falls back to S8 */
static int s_builtin;

extern const uint32_t rechord_fonts_s8_blob[];
extern const uint32_t rechord_fonts_s8_len;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int g_index(const fdata_t *d, uint32_t c);

/* Parse one font header at h (40 bytes) of the blob [b, b+len); 0 = ok. */
static int parse_font(const uint8_t *b, uint32_t len, const uint8_t *h, fdata_t *d)
{
    uint32_t oc = rd32(h + 16), orr = rd32(h + 20), ob = rd32(h + 24), os = rd32(h + 28),
             om = rd32(h + 32), bl = rd32(h + 36);

    memset(d, 0, sizeof *d);
    d->px = h[4]; d->line = h[5]; d->base = h[6];
    d->up = h[8]; d->down = h[9]; d->ascii = h[10];
    d->n = rd16(h + 12);
    d->fallback = rd16(h + 14);
    if (!d->n || d->fallback >= d->n || (oc | orr | ob | os | om) & 3u ||
        oc + 2u * d->n > len || orr + 8u * d->n > len || ob + bl > len)
        return -1;
    d->codes = (const uint16_t *)(b + oc);
    d->recs = (const grec_t *)(b + orr);
    d->bits = b + ob;
    for (unsigned i = 0; i < d->n; i++) {           /* every bitmap inside, codes ascending */
        const grec_t *r = &d->recs[i];

        if ((uint32_t)r->bits + (uint32_t)r->h * ((r->w + 3u) >> 2) > bl)
            return -1;
        if (i && d->codes[i] <= d->codes[i - 1])
            return -1;
    }
    if (d->ascii && (d->n < 95 || d->codes[0] != 0x20 || d->codes[94] != 0x7E))
        return -1;
    if (os) {
        const uint8_t *s = b + os;
        uint32_t need;

        if (os + 16u > len)
            return -1;
        d->n_half = rd16(s); d->n_conj = rd16(s + 2); d->n_lig = rd16(s + 4);
        d->n_dec = rd16(s + 6); d->reph = rd16(s + 8);
        need = 16u + 4u * d->n_half + 10u * d->n_conj + 8u * d->n_lig + 6u * d->n_dec;
        if (os + need > len)
            return -1;
        d->shape = (const uint16_t *)s;
        d->half = d->shape + 8;
        d->conj = d->half + 2u * d->n_half;
        d->lig = d->conj + 5u * d->n_conj;
        d->dec = d->lig + 4u * d->n_lig;
        if (om) {
            const uint8_t *m = b + om;

            if (om + 8u > len)
                return -1;
            d->n_pos = rd16(m); d->n_imat = rd16(m + 2); d->imat_tol = (int16_t)rd16(m + 4);
            if (om + 8u + 6u * d->n_pos + 4u * d->n_imat > len)
                return -1;
            d->pos = m + 8;
            d->imat = (const int16_t *)(m + 8 + 6u * d->n_pos);   /* (2-aligned: 6-byte rows) */
        }
    }
    {
        int e = g_index(d, 0x2026);
        d->ell = e < 0 ? 0xFFFFu : (uint16_t)e;
    }
    return 0;
}

static int parse_blob(const void *blob, uint32_t len, int take)
{
    const uint8_t *b = blob;
    uint32_t n;
    int got = 0;
    fdata_t tmp[F_COUNT];
    uint8_t have[F_COUNT] = { 0 };

    if (!b || len < 16u || memcmp(b, "RCFONT2", 8) || rd32(b + 8) > len)
        return -1;
    n = rd32(b + 12);
    if (n > 16u || 16u + 4u * n > len)
        return -1;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t o = rd32(b + 16 + 4 * i);
        int id = -1;

        if (o + 40u > len)
            return -1;
        for (int k = 0; k < F_COUNT; k++)
            if (!memcmp(b + o, k_names[k], 4))
                id = k;
        if (id < 0)
            continue;                                  /* a font this build does not know */
        if (parse_font(b, len, b + o, &tmp[id]))
            return -1;
        have[id] = 1;
        got++;
    }
    if (take)
        for (int k = 0; k < F_COUNT; k++)
            if (have[k]) {
                s_fd[k] = tmp[k];
                s_have[k] = 1;
            }
    return got;
}

static void builtin(void)
{
    if (s_builtin)
        return;
    s_builtin = 1;
    parse_blob(rechord_fonts_s8_blob, rechord_fonts_s8_len, 1);
}

int text_fonts_init(const void *blob, uint32_t len)
{
    builtin();
    if (parse_blob(blob, len, 0) < 0)
        return -1;
    return parse_blob(blob, len, 1);
}

int text_font_loaded(const font_t *f)
{
    builtin();
    return f && f->id < F_COUNT && s_have[f->id];
}

static const fdata_t *fd(const font_t *f)
{
    builtin();
    if (f && f->id < F_COUNT && s_have[f->id])
        return &s_fd[f->id];
    return &s_fd[F_S8];
}

int text_line_h(const font_t *f) { return fd(f)->line; }
int text_base(const font_t *f) { return fd(f)->base; }
int text_ink_up(const font_t *f) { return fd(f)->up; }
int text_ink_down(const font_t *f) { return fd(f)->down; }

/* Glyph index of code c, -1 if the font lacks it. */
static int g_index(const fdata_t *d, uint32_t c)
{
    int lo = 0, hi = (int)d->n - 1;

    if (d->ascii && c - 0x20u < 95u)
        return (int)(c - 0x20u);
    if (c > 0xFFFFu)
        return -1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = d->codes[mid];

        if (v == c)
            return mid;
        if (v < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

static int g_adv(const fdata_t *d, uint32_t c)
{
    int i = g_index(d, c);

    return d->recs[i < 0 ? d->fallback : i].adv;
}

/* ---- UTF-8 ----------------------------------------------------------------------- */

uint32_t text_utf8_next(const char **pp, const char *end)
{
    const uint8_t *p = (const uint8_t *)*pp, *e = (const uint8_t *)end;
    uint32_t c = *p, min;
    int n;

    if (c < 0x80u) {
        *pp = (const char *)(p + 1);
        return c;
    }
    if (c >= 0xC2u && c <= 0xDFu) { n = 1; c &= 0x1Fu; min = 0x80u; }
    else if (c >= 0xE0u && c <= 0xEFu) { n = 2; c &= 0x0Fu; min = 0x800u; }
    else if (c >= 0xF0u && c <= 0xF4u) { n = 3; c &= 0x07u; min = 0x10000u; }
    else { *pp = (const char *)(p + 1); return 0xFFFDu; }
    if (e - p <= n) {
        *pp = (const char *)(p + 1);
        return 0xFFFDu;
    }
    for (int i = 1; i <= n; i++) {
        if ((p[i] & 0xC0u) != 0x80u) {
            *pp = (const char *)(p + 1);
            return 0xFFFDu;
        }
        c = c << 6 | (p[i] & 0x3Fu);
    }
    if (c < min || c > 0x10FFFFu || (c >= 0xD800u && c <= 0xDFFFu)) {
        *pp = (const char *)(p + 1);
        return 0xFFFDu;
    }
    *pp = (const char *)(p + n + 1);
    return c;
}

/* ---- clusters and shaping ----------------------------------------------------- */

#define VIRAMA 0x094Du
#define NUKTA  0x093Cu
#define RA     0x0930u
#define IMATRA 0x093Fu
#define ZWNJ   0x200Cu
#define ZWJ    0x200Du
#define SG_MAX 32
#define CL_MAX 24

typedef struct { uint16_t idx; int8_t dx, dy; } sg_t;

static int is_cons(uint32_t c)
{
    return (c >= 0x0915u && c <= 0x0939u) || (c >= 0x0958u && c <= 0x095Fu) || (c >= 0x0978u && c <= 0x097Fu);
}

static int is_mark(uint32_t c)
{
    return (c >= 0x0900u && c <= 0x0903u) || (c >= 0x093Au && c <= 0x094Fu && c != 0x093Du) ||
           (c >= 0x0951u && c <= 0x0957u) || c == 0x0962u || c == 0x0963u;
}

static int is_deva(uint32_t c) { return c >= 0x0900u && c <= 0x097Fu; }

static int zero_width(uint32_t c)
{
    return c < 0x20u || (c >= 0x200Bu && c <= 0x200Fu) || c == 0x2060u || c == 0xFEFFu || c == 0x00ADu ||
           (c >= 0x7Fu && c < 0xA0u);
}

/* Binary search in a table of rows of `w` u16, keyed by (row[0] << 16 | row[1]). */
static const uint16_t *find2(const uint16_t *t, unsigned n, unsigned w, uint32_t a, uint32_t b)
{
    uint32_t key = a << 16 | b;
    int lo = 0, hi = (int)n - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const uint16_t *r = t + (unsigned)mid * w;
        uint32_t k = (uint32_t)r[0] << 16 | r[1];

        if (k == key)
            return r;
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

static uint16_t half_of(const fdata_t *d, uint32_t c)
{
    int lo = 0, hi = (int)d->n_half - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = d->half[2 * mid];

        if (k == c)
            return d->half[2 * mid + 1];
        if (k < c)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static int pos_of(const fdata_t *d, uint32_t base, uint32_t mark, int8_t *dx, int8_t *dy)
{
    uint32_t key = base << 16 | mark;
    int lo = 0, hi = (int)d->n_pos - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        const uint8_t *r = d->pos + 6 * mid;
        uint32_t k = (uint32_t)rd16(r) << 16 | rd16(r + 2);

        if (k == key) {
            *dx = (int8_t)r[4];
            *dy = (int8_t)r[5];
            return 1;
        }
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* The i-matra variant for cluster glyphs o[0..n) (gen_font.py Deva.pick_imat). */
static uint16_t pick_imatra(const fdata_t *d, const uint16_t *o, int n)
{
    int last = -1, pre = 0, t;
    uint16_t v;

    for (int i = 0; i < n; i++)
        if (g_adv(d, o[i]) > 0) {
            if (last >= 0)
                pre += g_adv(d, o[last]);
            last = i;
        }
    if (last < 0 || !d->n_imat)
        return IMATRA;
    {
        int gi = g_index(d, o[last]);
        const grec_t *r = &d->recs[gi < 0 ? d->fallback : gi];

        t = pre + (r->stem ? r->stem : r->adv * 3 / 4) + d->imat_tol;
    }
    v = (uint16_t)d->imat[1];
    for (unsigned i = 0; i < d->n_imat; i++)
        if (d->imat[2 * i] <= t)
            v = (uint16_t)d->imat[2 * i + 1];
    return v;
}

/* Shape one Devanagari syllable cps[0..n) into codes + mark offsets (gen_font.py Deva.sim). */
static int deva_shape(const fdata_t *d, const uint32_t *cps, int n, sg_t *g)
{
    uint16_t tmp[SG_MAX], o[SG_MAX + 4];
    uint32_t rest[CL_MAX];
    int k = 0, reph = 0, no = 0, nr = 0, pre = 0, h;

    if (n >= 3 && cps[0] == RA && cps[1] == VIRAMA && is_cons(cps[2])) {
        reph = 1;
        k = 2;
    }
    if (k < n && is_cons(cps[k])) {
        uint16_t cons[CL_MAX];
        int nc = 0;

        for (;;) {
            uint32_t c = cps[k++];

            if (k < n && cps[k] == NUKTA) {
                const uint16_t *l = find2(d->lig, d->n_lig, 4, c, NUKTA);

                if (l) {
                    c = l[2];
                    k++;
                }
            }
            cons[nc++] = (uint16_t)c;
            if (k + 1 < n && cps[k] == VIRAMA && is_cons(cps[k + 1])) {
                k++;
                continue;
            }
            if (k + 2 < n && cps[k] == VIRAMA && (cps[k + 1] == ZWJ || cps[k + 1] == ZWNJ) && is_cons(cps[k + 2])) {
                k += 2;
                continue;
            }
            break;
        }
        h = SG_MAX - 1;
        tmp[h] = cons[nc - 1];
        for (int i = nc - 2; i >= 0 && h >= 4; i--) {
            const uint16_t *r = find2(d->conj, d->n_conj, 5, cons[i], tmp[h]);
            uint16_t hf;

            if (r) {
                int m = 0;

                while (m < 3 && r[2 + m])
                    m++;
                h++;                                   /* the head is replaced */
                while (m > 0)
                    tmp[--h] = r[2 + --m];
            } else if ((hf = half_of(d, cons[i])) != 0) {
                tmp[--h] = hf;
            } else {
                tmp[--h] = (uint16_t)VIRAMA;
                tmp[--h] = cons[i];
            }
        }
        for (int i = h; i < SG_MAX; i++)
            o[no++] = tmp[i];
    } else if (k < n) {
        const uint16_t *r = NULL;
        int lo = 0, hi = (int)d->n_dec - 1;

        while (lo <= hi) {
            int mid = (lo + hi) >> 1;

            if (d->dec[3 * mid] == cps[k]) {
                r = d->dec + 3 * mid;
                break;
            }
            if (d->dec[3 * mid] < cps[k])
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        if (r) {
            o[no++] = r[1];
            o[no++] = r[2];
        } else {
            o[no++] = (uint16_t)cps[k];
        }
        k++;
    }
    for (; k < n; k++) {
        if (cps[k] == ZWJ || cps[k] == ZWNJ)
            continue;
        if (cps[k] == IMATRA && !pre)
            pre = 1;
        else if (nr < CL_MAX - 1)
            rest[nr++] = cps[k];
    }
    if (pre) {
        memmove(o + 1, o, (size_t)no * sizeof o[0]);
        o[0] = pick_imatra(d, o + 1, no);
        no++;
    }
    if (reph)
        rest[nr++] = d->reph;
    for (int i = 0; i < nr && no < SG_MAX; i++) {
        const uint16_t *l = no ? find2(d->lig, d->n_lig, 4, o[no - 1], rest[i]) : NULL;

        if (l) {
            o[no - 1] = l[2];
            if (l[3])
                o[no++] = l[3];
        } else {
            o[no++] = (uint16_t)rest[i];
        }
    }
    if (no > SG_MAX)
        no = SG_MAX;
    {
        uint32_t base = 0xFFFFu;

        for (int i = 0; i < no; i++) {
            int gi = g_index(d, o[i]);
            const grec_t *r = &d->recs[gi < 0 ? d->fallback : gi];

            g[i].idx = (uint16_t)(gi < 0 ? d->fallback : gi);
            g[i].dx = g[i].dy = 0;
            if (r->adv == 0) {
                if (d->n_pos)
                    pos_of(d, base, o[i], &g[i].dx, &g[i].dy);
            } else {
                base = o[i];
            }
        }
    }
    return no;
}

static int one(const fdata_t *d, uint32_t c, sg_t *g)
{
    int i = g_index(d, c);

    g->idx = (uint16_t)(i < 0 ? d->fallback : i);
    g->dx = g->dy = 0;
    return 1;
}

static uint32_t compose(uint32_t base, uint32_t mark)
{
    int lo = 0, hi = LATIN_COMPOSE_N - 1;
    uint32_t key = base << 16 | mark;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t k = (uint32_t)latin_compose[mid][0] << 16 | latin_compose[mid][1];

        if (k == key)
            return latin_compose[mid][2];
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

/* The next cluster of [*p, end): its glyphs into g (up to SG_MAX), *p moved past it.
 * Returns the glyph count (0: nothing to draw). */
static int next_cluster(const fdata_t *d, const char **p, const char *end, sg_t *g)
{
    const char *q;
    uint32_t c = text_utf8_next(p, end);

    if (c < 0x80u && c >= 0x20u && c != 0x7Fu &&           /* the common case: ASCII, no accent */
        (*p >= end || ((uint8_t)**p != 0xCCu && (uint8_t)**p != 0xCDu) || c < 0x41u))
        return one(d, c, g);
    if (c == '\t')
        return one(d, ' ', g);
    if (zero_width(c))
        return 0;
    if (is_deva(c) && d->shape) {
        uint32_t cps[CL_MAX], x;
        int n = 0;

        cps[n++] = c;
        if (is_cons(c)) {
            for (;;) {
                q = *p;
                if (q < end && (x = text_utf8_next(&q, end)) == NUKTA && n < CL_MAX) {
                    cps[n++] = x;
                    *p = q;
                }
                q = *p;
                if (q < end && text_utf8_next(&q, end) == VIRAMA && q < end && n + 2 <= CL_MAX) {
                    const char *q2 = q;
                    uint32_t y = text_utf8_next(&q2, end);

                    if (is_cons(y)) {
                        cps[n++] = VIRAMA;
                        cps[n++] = y;
                        *p = q2;
                        continue;
                    }
                    if ((y == ZWJ || y == ZWNJ) && q2 < end && n + 3 <= CL_MAX) {
                        const char *q3 = q2;
                        uint32_t z = text_utf8_next(&q3, end);

                        if (is_cons(z)) {
                            cps[n++] = VIRAMA;
                            cps[n++] = y;
                            cps[n++] = z;
                            *p = q3;
                            continue;
                        }
                    }
                }
                break;
            }
        }
        for (;;) {                                       /* matras, signs, joiners */
            q = *p;
            if (q >= end || n >= CL_MAX)
                break;
            x = text_utf8_next(&q, end);
            if (!is_mark(x) && x != ZWJ && x != ZWNJ)
                break;
            cps[n++] = x;
            *p = q;
        }
        return deva_shape(d, cps, n, g);
    }
    if (c >= 0x41u && c < 0x180u) {                      /* a Latin letter + combining accents */
        for (;;) {
            uint32_t m, k;

            q = *p;
            if (q >= end)
                break;
            m = text_utf8_next(&q, end);
            if (m < 0x300u || m > 0x36Fu)
                break;
            *p = q;
            if ((k = compose(c, m)) != 0)
                c = k;                                   /* (an accent it lacks is dropped) */
        }
    } else if (c >= 0x300u && c <= 0x36Fu) {
        return 0;                                        /* a stray accent */
    }
    return one(d, c, g);
}

/* ---- measuring --------------------------------------------------------------- */

static int clen(const char *s, int nbytes)
{
    return nbytes < 0 ? (int)strlen(s) : nbytes;
}

static int cluster_w(const fdata_t *d, const sg_t *g, int n)
{
    int w = 0;

    for (int i = 0; i < n; i++)
        w += d->recs[g[i].idx].adv;
    return w;
}

int text_width(const font_t *f, const char *utf8, int nbytes)
{
    const fdata_t *d = fd(f);
    const char *p = utf8, *end;
    sg_t g[SG_MAX];
    int w = 0;

    if (!utf8)
        return 0;
    end = utf8 + clen(utf8, nbytes);
    while (p < end) {
        uint8_t c = (uint8_t)*p;

        if (c >= 0x20u && c < 0x7Fu && d->ascii) {      /* ASCII without a cluster around it */
            const char *q = p + 1;

            if (q >= end || ((uint8_t)*q != 0xCCu && (uint8_t)*q != 0xCDu) || c < 0x41u) {
                w += d->recs[c - 0x20u].adv;
                p++;
                continue;
            }
        }
        w += cluster_w(d, g, next_cluster(d, &p, end, g));
    }
    return w;
}

int text_shape(const font_t *f, const char *utf8, int nbytes, text_glyph_t *out, int max)
{
    const fdata_t *d = fd(f);
    const char *p = utf8, *end;
    sg_t g[SG_MAX];
    int pen = 0, k = 0;

    if (!utf8)
        return 0;
    end = utf8 + clen(utf8, nbytes);
    while (p < end) {
        int n = next_cluster(d, &p, end, g);

        for (int i = 0; i < n; i++) {
            const grec_t *r = &d->recs[g[i].idx];

            if (k < max) {
                out[k].code = d->codes[g[i].idx];
                out[k].x = (int16_t)(pen + g[i].dx);
                out[k].y = g[i].dy;
                out[k].adv = r->adv;
            }
            k++;
            pen += r->adv;
        }
    }
    return k < max ? k : max;
}

int text_fit(const font_t *f, const char *utf8, int nbytes, int max_w, char *out, int out_cap)
{
    const fdata_t *d = fd(f);
    const char *p, *end;
    sg_t g[SG_MAX];
    int n, w = 0, cut = 0, room, ell, total;

    if (!out || out_cap < 1)
        return 0;
    out[0] = 0;
    if (!utf8)
        return 0;
    n = clen(utf8, nbytes);
    end = utf8 + n;
    total = text_width(f, utf8, n);
    room = out_cap - 1;
    if (total <= max_w && n <= room) {
        memcpy(out, utf8, (size_t)n);
        out[n] = 0;
        return n;
    }
    ell = d->ell != 0xFFFFu ? d->recs[d->ell].adv : d->recs[d->fallback].adv;
    if (total <= max_w)
        ell = 0;                                         /* only the buffer is short: no "…" */
    if (ell > max_w)
        return 0;
    for (p = utf8; p < end;) {
        const char *q = p;
        int cw = cluster_w(d, g, next_cluster(d, &q, end, g));

        if (w + cw + ell > max_w || (q - utf8) + (ell ? 3 : 0) > room)
            break;
        w += cw;
        p = q;
        cut = (int)(p - utf8);
    }
    while (cut > 0 && utf8[cut - 1] == ' ')
        cut--;
    memcpy(out, utf8, (size_t)cut);
    if (ell) {
        memcpy(out + cut, "\xE2\x80\xA6", 3);
        cut += 3;
    }
    out[cut] = 0;
    return cut;
}

static int is_space_at(const char *p, const char *end)
{
    return p < end && *p == ' ';
}

/* A cluster after which a line may break (it stays on the line): - / – — */
static int breaks_after(const char *s, int len)
{
    return (len == 1 && (*s == '-' || *s == '/')) ||
           (len == 3 && (uint8_t)s[0] == 0xE2u && (uint8_t)s[1] == 0x80u &&
            ((uint8_t)s[2] == 0x93u || (uint8_t)s[2] == 0x94u));
}

int text_wrap(const font_t *f, const char *utf8, int nbytes, int max_w, int max_lines, text_line_t *lines)
{
    const fdata_t *d = fd(f);
    const char *end, *pos;
    sg_t g[SG_MAX];
    int nl = 0;

    if (!utf8 || !lines || max_lines <= 0)
        return 0;
    end = utf8 + clen(utf8, nbytes);
    pos = utf8;
    while (is_space_at(pos, end))
        pos++;
    while (pos < end && nl < max_lines) {
        const char *p = pos, *brk = NULL, *brk_next = NULL, *stop = end, *next = end;
        int w = 0, brk_w = 0, stop_w = -1;

        while (p < end) {
            const char *q = p;
            int cw = cluster_w(d, g, next_cluster(d, &q, end, g));

            if (*p == ' ') {
                if (p > pos && p[-1] != ' ') {           /* a word ends here */
                    brk = p;
                    brk_w = w;
                }
                brk_next = q;
                while (is_space_at(brk_next, end))
                    brk_next++;
                w += cw;
                p = q;
                continue;
            }
            if (w + cw > max_w && p > pos && nl < max_lines - 1) {
                if (brk) {
                    stop = brk; stop_w = brk_w; next = brk_next;
                } else {
                    stop = p; stop_w = w; next = p;         /* a word longer than the line */
                }
                break;
            }
            w += cw;
            if (breaks_after(p, (int)(q - p))) {
                brk = q;
                brk_w = w;
                brk_next = q;
                while (is_space_at(brk_next, end))
                    brk_next++;
            }
            p = q;
        }
        if (stop_w < 0) {                                /* the rest fits (or is the last line) */
            const char *e = end;

            while (e > pos && e[-1] == ' ')
                e--;
            stop = e;
            stop_w = text_width(f, pos, (int)(e - pos));
            next = end;
        }
        lines[nl].off = (uint16_t)(pos - utf8);
        lines[nl].len = (uint16_t)(stop - pos);
        lines[nl].w = (int16_t)stop_w;
        nl++;
        pos = next;
    }
    return nl;
}

/* ---- drawing -------------------------------------------------------------------- */

/* 2-bit alpha over bg, exactly as tools/ui_mockups.py mix(): both colours widened to
 * 8 bits, bg + (fg - bg) * a/3 rounded, then back to RGB565. */
static uint16_t mix565(uint16_t bg, uint16_t fg, unsigned a)
{
    unsigned br = bg >> 11, bgc = bg >> 5 & 63u, bb = bg & 31u;
    unsigned fr = fg >> 11, fgc = fg >> 5 & 63u, fb = fg & 31u;
    int r8 = (int)(br << 3 | br >> 2), g8 = (int)(bgc << 2 | bgc >> 4), b8 = (int)(bb << 3 | bb >> 2);
    int R8 = (int)(fr << 3 | fr >> 2), G8 = (int)(fgc << 2 | fgc >> 4), B8 = (int)(fb << 3 | fb >> 2);
    unsigned r = (unsigned)(r8 * 3 + (R8 - r8) * (int)a + 1) / 3u;
    unsigned gg = (unsigned)(g8 * 3 + (G8 - g8) * (int)a + 1) / 3u;
    unsigned b = (unsigned)(b8 * 3 + (B8 - b8) * (int)a + 1) / 3u;

    return (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | b >> 3);
}

static uint16_t s_cbg, s_cfg, s_c[4];
static int s_cok;

/* The two blended colours for (bg, fg): one pair cached, text sits on fills. */
static __attribute__((noinline)) void blend_fill(uint16_t bg, uint16_t fg)
{
    s_c[1] = mix565(bg, fg, 1);
    s_c[2] = mix565(bg, fg, 2);
    s_cbg = bg;
    s_cfg = fg;
    s_cok = 1;
}

/* One pixel of alpha a (0..3) in fg; cbg/pal hold the cached pair in locals. */
#define PUT(p, a)                                                          \
    do {                                                                   \
        if ((a) == 3u)                                                     \
            *(p) = fg;                                                     \
        else if (a) {                                                      \
            uint16_t bg_ = *(p);                                           \
            if (bg_ != cbg) {                                              \
                blend_fill(bg_, fg);                                       \
                cbg = pal[0] = bg_; pal[1] = s_c[1]; pal[2] = s_c[2];      \
            }                                                              \
            *(p) = pal[a];                                                 \
        }                                                                  \
    } while (0)

static void blit(const fdata_t *d, const grec_t *r, uint16_t *band, int band_y, int band_h,
                 int cx0, int cx1, int gx, int gy, uint16_t fg)
{
    int stride = (r->w + 3) >> 2;
    int y0 = gy > band_y ? gy : band_y, y1 = gy + r->h < band_y + band_h ? gy + r->h : band_y + band_h;
    int x0 = gx > cx0 ? gx : cx0, x1 = gx + r->w < cx1 ? gx + r->w : cx1;
    const uint8_t *src;
    uint16_t *row, cbg, pal[4];

    if (y0 >= y1 || x0 >= x1)
        return;
    if (!s_cok || s_cfg != fg)
        blend_fill(s_cbg, fg);
    cbg = pal[0] = s_cbg;
    pal[1] = s_c[1];
    pal[2] = s_c[2];
    pal[3] = fg;
    src = d->bits + r->bits + (y0 - gy) * stride;
    row = band + (y0 - band_y) * TEXT_BAND_W;
    if (x0 == gx && x1 == gx + r->w) {                   /* not clipped: four pixels a byte */
        for (int y = y0; y < y1; y++, src += stride, row += TEXT_BAND_W) {
            uint16_t *p = row + gx;

            for (int i = 0; i < stride; i++, p += 4) {
                unsigned b = src[i], a;

                if (!b)
                    continue;
                if (b == 0xFFu) {
                    p[0] = p[1] = p[2] = p[3] = fg;
                    continue;
                }
                a = b >> 6;      PUT(p, a);               /* (padding pixels are 0: never written) */
                a = b >> 4 & 3u; PUT(p + 1, a);
                a = b >> 2 & 3u; PUT(p + 2, a);
                a = b & 3u;      PUT(p + 3, a);
            }
        }
        return;
    }
    for (int y = y0; y < y1; y++, src += stride, row += TEXT_BAND_W) {
        uint16_t *p = row + x0;

        for (int x = x0 - gx; x < x1 - gx; x++, p++) {
            unsigned a = src[x >> 2] >> (6 - 2 * (x & 3)) & 3u;

            PUT(p, a);
        }
    }
}

int text_draw_band(const font_t *f, uint16_t *band, int band_y, int band_h,
                   int x, int baseline, int clip_x0, int clip_x1,
                   const char *utf8, int nbytes, uint16_t fg)
{
    const fdata_t *d = fd(f);
    const char *p = utf8, *end;
    sg_t g[SG_MAX];
    int pen = x;

    if (!utf8)
        return 0;
    end = utf8 + clen(utf8, nbytes);
    if (clip_x0 < 0)
        clip_x0 = 0;
    if (clip_x1 > TEXT_BAND_W)
        clip_x1 = TEXT_BAND_W;
    if (!band || band_h <= 0 || baseline - d->up - 4 >= band_y + band_h || baseline + d->down + 4 <= band_y ||
        clip_x0 >= clip_x1)
        return text_width(f, utf8, (int)(end - utf8));   /* nothing of it in this band */
    while (p < end) {
        int n = next_cluster(d, &p, end, g);

        for (int i = 0; i < n; i++) {
            const grec_t *r = &d->recs[g[i].idx];

            if (r->w && pen < clip_x1 + 8)
                blit(d, r, band, band_y, band_h, clip_x0, clip_x1,
                     pen + g[i].dx + r->left, baseline + g[i].dy + r->top, fg);
            pen += r->adv;
        }
    }
    return pen - x;
}
