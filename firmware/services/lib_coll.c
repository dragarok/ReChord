/*
 * lib_coll.c -- how the library orders names (lib_int.h lib_coll): one function, tested on its own
 * (services/tests/test_lib.c "coll").
 *
 *   - case is ignored; Latin-1 and Latin Extended-A letters count as their base letter
 *     (é = e, Ł = l), with ß = ss, æ = ae, œ = oe, þ = th, ĳ = ij;
 *   - Greek and Cyrillic capitals count as their small letters; every other script by code point,
 *     after all Latin letters;
 *   - a run of digits is a number: "Track 2" before "Track 10", "007" = "7";
 *   - leading spaces and punctuation are skipped ("'Til Tuesday" under T, "(What's the Story)"
 *     under W), and for artists a leading "The " ("The Beatles" under B);
 *   - spaces and punctuation inside a name sort before digits, digits before letters;
 *   - an empty name (the "Unknown" buckets) after everything.
 * 0 means "the same name": "Beatles" and "beatles" are one artist. Callers break such ties by the
 * bytes when they need a fixed order.
 */
#include <string.h>

#include "lib_int.h"

#define W_PUNCT  0x00000100u
#define W_NUM    0x00010000u
#define W_LETTER 0x00020000u
#define W_OTHER  0x00100000u

/* U+00C0..U+00FF and U+0100..U+017F: the base letter, or 1 ae, 2 not a letter, 3 th, 4 ss,
 * 5 ij, 6 oe */
static const char k_lat1[64] __attribute__((nonstring)) = "aaaaaa\1ceeeeiiiidnooooo\2ouuuuy\3\4aaaaaa\1ceeeeiiiidnooooo\2ouuuuy\3y";
static const char k_latA[128] __attribute__((nonstring)) = "aaaaaacccccccc" "dddd" "eeeeeeeeee" "gggggggg" "hhhh" "iiiiiiiiii" "\5\5" "jj"
                                "kkk" "llllllllll" "nnnnnnnnn" "oooooo" "\6\6" "rrrrrr" "ssssssss" "tttttt"
                                "uuuuuuuuuuuu" "ww" "yyy" "zzzzzz" "s";

static uint32_t utf8_next(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    uint32_t c = *p++, k, v;

    if (c >= 0xC2u && c <= 0xF4u) {
        k = c < 0xE0u ? 1u : c < 0xF0u ? 2u : 3u;
        if ((uint32_t)(end - p) >= k) {
            v = c & (0x3Fu >> k);
            for (uint32_t i = 0; i < k; i++) {
                if ((p[i] & 0xC0u) != 0x80u)
                    goto latin1;
                v = v << 6 | (p[i] & 0x3Fu);
            }
            *pp = p + k;
            return v;
        }
    }
latin1:                                       /* not UTF-8: the byte as Latin-1 */
    *pp = p;
    return c;
}

/* The weight of code point c; *exp: a second letter that follows (ß = s s). */
static uint32_t weight(uint32_t c, uint32_t *exp)
{
    char b = 0;

    *exp = 0;
    if (c >= 'A' && c <= 'Z')
        c += 32;
    if (c >= 'a' && c <= 'z')
        return W_LETTER + (c - 'a' + 1u);
    if (c >= '0' && c <= '9')
        return W_NUM;
    if (c < 0xC0u)                            /* ASCII and Latin-1 punctuation */
        return W_PUNCT + c;
    if (c < 0x100u)
        b = k_lat1[c - 0xC0u];
    else if (c < 0x180u)
        b = k_latA[c - 0x100u];
    if (b >= 'a') return W_LETTER + (uint32_t)(b - 'a' + 1);
    switch (b) {
    case 1: *exp = W_LETTER + 5u; return W_LETTER + 1u;        /* ae */
    case 2: return W_PUNCT + c;                                 /* x, / */
    case 3: *exp = W_LETTER + 8u; return W_LETTER + 20u;       /* th */
    case 4: *exp = W_LETTER + 19u; return W_LETTER + 19u;      /* ss */
    case 5: *exp = W_LETTER + 10u; return W_LETTER + 9u;       /* ij */
    case 6: *exp = W_LETTER + 5u; return W_LETTER + 15u;       /* oe */
    }
    if (c >= 0x2000u && c < 0x2070u)          /* general punctuation: quotes, dashes */
        return W_PUNCT + 0xC0u + (c - 0x2000u);
    if ((c >= 0x391u && c <= 0x3A9u) || (c >= 0x410u && c <= 0x42Fu))
        c += 32u;                             /* Greek, Cyrillic capitals */
    else if (c >= 0x400u && c <= 0x40Fu)
        c += 80u;
    return W_OTHER + c;
}

typedef struct {
    const uint8_t *p, *end;
    uint32_t pend;                            /* an expansion's second letter */
} cur_t;

/* Where a name starts for ordering: past leading spaces and punctuation, and "The ". */
static const uint8_t *start(const uint8_t *s, const uint8_t *end, int flags)
{
    const uint8_t *p = s;

    for (;;) {
        const uint8_t *q = p;
        uint32_t e, w;

        if (q >= end)
            return s;                         /* nothing but punctuation: as it is */
        w = weight(utf8_next(&q, end), &e);
        if (w < W_PUNCT || w >= W_NUM)        /* a letter, a digit, another script */
            break;
        p = q;
    }
    if ((flags & LC_ARTIST) && end - p > 4 && (p[0] | 0x20) == 't' && (p[1] | 0x20) == 'h' &&
        (p[2] | 0x20) == 'e' && p[3] == ' ')
        p += 4;
    return p;
}

/* Next unit: a weight, or W_NUM with the number's digits at *d (n of them, no leading zeros). */
static uint32_t unit(cur_t *c, const uint8_t **d, uint32_t *n)
{
    uint32_t w, e;

    if (c->pend) {
        w = c->pend;
        c->pend = 0;
        return w;
    }
    if (*c->p >= '0' && *c->p <= '9') {
        const uint8_t *q = c->p;

        while (q < c->end && *q == '0')
            q++;
        *d = q;
        while (q < c->end && *q >= '0' && *q <= '9')
            q++;
        *n = (uint32_t)(q - *d);
        c->p = q;
        return W_NUM;
    }
    w = weight(utf8_next(&c->p, c->end), &e);
    c->pend = e;
    return w;
}

int lib_coll(const uint8_t *a, unsigned la, const uint8_t *b, unsigned lb, int flags)
{
    cur_t x, y;

    if (!la || !lb)
        return (int)!la - (int)!lb;           /* empty: last */
    x.end = a + la;
    y.end = b + lb;
    x.p = start(a, x.end, flags);
    y.p = start(b, y.end, flags);
    x.pend = y.pend = 0;
    while ((x.p < x.end || x.pend) && (y.p < y.end || y.pend)) {
        const uint8_t *dx = 0, *dy = 0;
        uint32_t nx = 0, ny = 0, wx = unit(&x, &dx, &nx), wy = unit(&y, &dy, &ny);

        if (wx != wy)
            return wx < wy ? -1 : 1;
        if (wx == W_NUM) {
            int d;

            if (nx != ny)
                return nx < ny ? -1 : 1;
            if ((d = memcmp(dx, dy, nx)) != 0)
                return d < 0 ? -1 : 1;
        }
    }
    return (x.p < x.end || x.pend) - (y.p < y.end || y.pend);
}

int lib_letter(const uint8_t *s, unsigned n, int flags)
{
    const uint8_t *end = s + n, *p;
    uint32_t w, e;

    if (!n)
        return LIB_LETTERS - 1;
    p = start(s, end, flags);
    w = weight(utf8_next(&p, end), &e);
    if (w > W_LETTER && w <= W_LETTER + 26u)
        return (int)(w - W_LETTER);
    return w >= W_OTHER ? LIB_LETTERS - 1 : 0;
}

uint32_t lib_fnv(uint32_t h, const void *p, uint32_t n)
{
    const uint8_t *s = p;

    while (n--)
        h = (h ^ *s++) * 16777619u;
    return h;
}

uint32_t fwu_crc(uint32_t crc, const uint8_t *p, uint32_t n);   /* services/fw_update.c */

uint32_t lib_crc(uint32_t crc, const void *p, uint32_t n)
{
    return fwu_crc(crc, (const uint8_t *)p, n);
}
