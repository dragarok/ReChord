/*
 * tags.c -- title, artist, album and track from the tags of an audio file.
 *
 * Everything goes through one window: get() returns a pointer to n bytes at a
 * file offset, reading TAGS_WIN bytes from there when they are not in the
 * window yet (at() places the window around them, for the probes walking back
 * from the end of a file). Parsers only ask
 * for headers and for the first TEXT_MAX bytes of a wanted text value; covers,
 * lyrics and padding are stepped over by their sizes. Every size taken from the
 * file is checked against the bytes left before it is used.
 *
 * Not supported, like RECHO: Lyrics3 fields, compressed or encrypted ID3v2
 * frames, whole-tag unsynchronisation in ID3v2.2/2.3 (the tag is skipped).
 * ID3v2.4 unsynchronised frames are undone in place.
 *
 * Text: ID3 Latin-1 is converted; UTF-16 takes its byte order from the BOM
 * (big-endian without one); UTF-8 (ID3, Vorbis, APE) is copied, and so is RIFF
 * INFO where it is valid UTF-8 -- any byte that is not part of a valid UTF-8
 * sequence is taken as Latin-1. Control characters become spaces, trailing
 * spaces are dropped, and the result is cut at a character boundary.
 */
#include <string.h>

#include "tags.h"

#define TAGS_WIN  512u                 /* the window; every get() asks for <= this */
#define TEXT_MAX  256u                 /* bytes of a value we look at: fills title[] from UTF-16 */
#define NONE      0xFFFFFFFFu

#ifdef TAGS_EXT
enum { F_TITLE, F_ARTIST, F_ALBUM, F_TRACK, F_AARTIST, F_GENRE, F_DISC, F_YEAR, NF };
#else
enum { F_TITLE, F_ARTIST, F_ALBUM, F_TRACK, NF };
#endif
#define ALL       ((1u << NF) - 1u)    /* every field known */
enum { E_LATIN1, E_UTF8, E_UTF16BE, E_UTF16LE };

typedef struct {
    tags_read_at_t rd;
    void *u;
    tag_info_t *out;
    uint32_t size;
    uint32_t woff, wlen;               /* the window: file offset, bytes valid */
    uint16_t have;                     /* 1 << F_*: fields known */
    uint16_t src;                      /* TAGS_SRC_* of the tag being read */
    uint8_t win[TAGS_WIN];             /* last, with no padding after it: ASan sees an overrun */
} ctx_t;

#ifdef TAGS_EXT
/* Lower-case names per source in F_* order ("" where a source has none), NULL-terminated; an
 * alias after them starts with its field + 1 (a control character). */
static const char *const k_id3v22[] = { "tt2", "tp1", "tal", "trk", "tp2", "tco", "tpa", "tye", 0 };
static const char *const k_id3v23[] = { "tit2", "tpe1", "talb", "trck", "tpe2", "tcon", "tpos", "tyer",
                                        "\x08tdrc", 0 };
static const char *const k_ape[] = { "title", "artist", "album", "track", "album artist", "genre", "disc",
                                     "year", "\x05albumartist", 0 };
static const char *const k_vorbis[] = { "title", "artist", "album", "tracknumber", "albumartist", "genre",
                                        "discnumber", "date", "\x05album artist", "\x08year", 0 };
static const char *const k_riff[] = { "inam", "iart", "iprd", "itrk", "", "ignr", "", "icrd", "\x04iprt", 0 };
#else
/* Lower-case names per source, NULL-terminated; an index past F_TRACK is the track. */
static const char *const k_id3v22[] = { "tt2", "tp1", "tal", "trk", 0 };
static const char *const k_id3v23[] = { "tit2", "tpe1", "talb", "trck", 0 };
static const char *const k_ape[] = { "title", "artist", "album", "track", 0 };
static const char *const k_vorbis[] = { "title", "artist", "album", "tracknumber", 0 };
static const char *const k_riff[] = { "inam", "iart", "iprd", "itrk", "iprt", 0 };
#endif

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | be24(p + 1); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t syncsafe(const uint8_t *p) { return (uint32_t)(p[0] & 0x7F) << 21 | (uint32_t)(p[1] & 0x7F) << 14 | (uint32_t)(p[2] & 0x7F) << 7 | (p[3] & 0x7F); }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }

/* n bytes at off, or NULL past the end of the file. On a miss the window is
 * loaded from off; with back (n <= 128, walking back from the end of a file)
 * it ends 384 bytes past off + n instead, or at the end of the file: what is
 * in front of the probe, and the tag a probe finds, come with it. */
static uint8_t *at(ctx_t *c, uint32_t off, uint32_t n, int back)
{
    uint32_t start, len;

    if (off > c->size || n > c->size - off)
        return 0;
    if (off < c->woff || off - c->woff > c->wlen || n > c->wlen - (off - c->woff)) {
        start = off;
        if (back) {
            start = c->size - (off + n) > TAGS_WIN - 128u ? off + n + TAGS_WIN - 128u : c->size;
            start = start > TAGS_WIN ? start - TAGS_WIN : 0;
        }
        len = min32(TAGS_WIN, c->size - start);
        c->woff = start;
        c->wlen = c->rd(c->u, start, c->win, len);
        if (c->wlen > len)
            c->wlen = 0;
        if (off - start > c->wlen || n > c->wlen - (off - start))
            return 0;
    }
    return c->win + (off - c->woff);
}
static uint8_t *get(ctx_t *c, uint32_t off, uint32_t n) { return at(c, off, n, 0); }

/* Index of the name k[0..n) in names (ASCII, case-insensitive), -1 if none. */
static int field(const uint8_t *k, uint32_t n, const char *const *names)
{
    for (int f = 0; names[f]; f++) {
        const char *s = names[f];
        uint32_t i = 0;

#ifdef TAGS_EXT
        if (f >= NF)
            s++;
#endif
        while (i < n && s[i] && (k[i] | 0x20) == (uint8_t)s[i])
            i++;
#ifdef TAGS_EXT
        if (i == n && !s[i] && n)      /* ("" placeholders never match) */
            return f >= NF ? names[f][0] - 1 : f;
#else
        if (i == n && !s[i])
            return f > F_TRACK ? F_TRACK : f;
#endif
    }
    return -1;
}

/* Length of the valid UTF-8 sequence at p (code point in *cp), 0 if invalid. */
static uint32_t utf8_seq(const uint8_t *p, uint32_t n, uint32_t *cp)
{
    uint32_t k, v = p[0];

    if (v < 0x80u) {
        *cp = v;
        return 1;
    }
    if (v < 0xC2u || v > 0xF4u)
        return 0;
    k = v < 0xE0u ? 2u : v < 0xF0u ? 3u : 4u;
    if (n < k)
        return 0;
    v &= 0x7Fu >> k;
    for (uint32_t i = 1; i < k; i++) {
        if ((p[i] & 0xC0u) != 0x80u)
            return 0;
        v = v << 6 | (p[i] & 0x3Fu);
    }
    if ((k == 3u && v < 0x800u) || (k == 4u && (v < 0x10000u || v > 0x10FFFFu)) || (v >= 0xD800u && v < 0xE000u))
        return 0;
    *cp = v;
    return k;
}

/* Text p[0..n) in encoding enc into dst (cap bytes with the NUL), as UTF-8. */
static void put_text(char *dst, uint32_t cap, const uint8_t *p, uint32_t n, int enc)
{
    uint32_t len = 0;

    while (n) {
        uint32_t cp, k = 1, w;

        if (enc == E_LATIN1) {
            cp = p[0];
        } else if (enc == E_UTF8) {
            k = utf8_seq(p, n, &cp);
            if (!k) {
                cp = p[0];
                k = 1;
            }
        } else {
            if (n < 2u)
                break;
            cp = enc == E_UTF16BE ? be16(p) : (uint32_t)p[1] << 8 | p[0];
            k = 2;
            if (cp >= 0xD800u && cp < 0xDC00u && n >= 4u) {
                uint32_t lo = enc == E_UTF16BE ? be16(p + 2) : (uint32_t)p[3] << 8 | p[2];

                if (lo >= 0xDC00u && lo < 0xE000u) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    k = 4;
                }
            }
            if (cp >= 0xD800u && cp < 0xE000u)
                cp = 0xFFFDu;          /* unpaired surrogate */
        }
        if (!cp)
            break;
        if (cp < 0x20u || cp == 0x7Fu)
            cp = ' ';
        w = cp < 0x80u ? 1u : cp < 0x800u ? 2u : cp < 0x10000u ? 3u : 4u;
        if (len + w >= cap)
            break;
        if (w == 1u) {
            dst[len++] = (char)cp;
        } else {
            dst[len++] = (char)((0xF0u << (4u - w)) | cp >> (6u * (w - 1u)));
            for (uint32_t i = w - 1u; i--; )
                dst[len++] = (char)(0x80u | (cp >> (6u * i) & 0x3Fu));
        }
        p += k;
        n -= k;
    }
    while (len && dst[len - 1] == ' ')
        len--;
    dst[len] = 0;
}

static void set_track(ctx_t *c, uint32_t t)
{
    if (!t || (c->have & 1u << F_TRACK))
        return;
    c->out->track = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
    c->have |= 1u << F_TRACK;
    c->out->found |= c->src;
}

#ifdef TAGS_EXT
/* ID3v1's genre numbers 0..79 (ID3v2 TCON "(17)", MP4 gnre, ID3v1's byte); Winamp's later ones
 * (80 and up) are left as their number's text would be: not used */
static const char *const k_genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop",
    "Vocal", "Jazz+Funk", "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
    "Sound Clip", "Gospel", "Noise", "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative",
    "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic", "Darkwave", "Techno-Industrial",
    "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave",
    "Psychedelic", "Rave", "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka",
    "Retro", "Musical", "Rock & Roll", "Hard Rock",
};

static void set_genre_num(ctx_t *c, uint32_t g)
{
    const char *s = g < sizeof k_genres / sizeof k_genres[0] ? k_genres[g] : 0;

    if (s && !(c->have & 1u << F_GENRE)) {
        put_text(c->out->genre, sizeof c->out->genre, (const uint8_t *)s, (uint32_t)strlen(s), E_LATIN1);
        c->have |= 1u << F_GENRE;
        c->out->found |= c->src;
    }
}

/* Field f from text p[0..n): first source wins; empty text and numbers 0 do not count. Years:
 * the first number from 1000 on ("2003-05-01"); genres "(17)" / "17" by the ID3v1 table. */
static void set(ctx_t *c, int f, const uint8_t *p, uint32_t n, int enc)
{
    char *dst;
    uint32_t cap;

    if (c->have & 1u << f)
        return;
    if (f == F_TRACK || f == F_DISC || f == F_YEAR) {
        char num[24];
        uint32_t t = 0;
        const char *q = num;

        put_text(num, sizeof num, p, n, enc);
        for (;;) {
            while (*q && (*q < '0' || *q > '9'))
                q++;
            for (t = 0; *q >= '0' && *q <= '9' && t < 0x10000u; q++)
                t = t * 10u + (uint32_t)(*q - '0');
            if (f != F_YEAR || !*q || (t >= 1000u && t < 3000u))
                break;
        }
        if (f == F_YEAR && (t < 1000u || t >= 3000u))
            return;
        if (f == F_TRACK && num[0] != ' ' && (num[0] < '0' || num[0] > '9'))
            return;                    /* (a track "A1" is not a number) */
        if (!t)
            return;
        if (f == F_TRACK)
            c->out->track = (uint16_t)(t > 0xFFFFu ? 0xFFFFu : t);
        else if (f == F_DISC)
            c->out->disc = (uint8_t)(t > 255u ? 255u : t);
        else
            c->out->year = (uint16_t)t;
        c->have |= 1u << f;
        c->out->found |= c->src;
        return;
    }
    dst = f == F_TITLE ? c->out->title : f == F_ARTIST ? c->out->artist : f == F_ALBUM ? c->out->album :
          f == F_AARTIST ? c->out->aartist : c->out->genre;
    cap = f == F_GENRE ? sizeof c->out->genre : sizeof c->out->title;
    put_text(dst, cap, p, n, enc);
    if (f == F_GENRE && (dst[0] == '(' || (dst[0] >= '0' && dst[0] <= '9'))) {
        const char *q = dst + (dst[0] == '(');
        uint32_t g = 0;

        while (*q >= '0' && *q <= '9' && g < 1000u)
            g = g * 10u + (uint32_t)(*q++ - '0');
        if ((dst[0] == '(' && *q == ')' && !q[1]) || (dst[0] != '(' && !*q)) {
            dst[0] = 0;                /* "(17)" or "17": the number's name */
            set_genre_num(c, g);
            return;
        }
        if (dst[0] == '(' && *q == ')' && q[1]) {   /* "(17)Rock": the text */
            memmove(dst, q + 1, strlen(q + 1) + 1);
        }
    }
    if (*dst) {
        c->have |= 1u << f;
        c->out->found |= c->src;
    }
}
#else
/* Field f from text p[0..n): first source wins; empty text and track 0 do not count. */
static void set(ctx_t *c, int f, const uint8_t *p, uint32_t n, int enc)
{
    char *dst = f == F_TITLE ? c->out->title : f == F_ARTIST ? c->out->artist : c->out->album;
    uint32_t cap = f == F_TITLE ? sizeof c->out->title : sizeof c->out->artist;

    if (c->have & 1u << f)
        return;
    if (f == F_TRACK) {                /* "3", " 03/12" */
        char num[12];
        uint32_t t = 0;
        const char *s = num;

        put_text(num, sizeof num, p, n, enc);
        while (*s == ' ')
            s++;
        while (*s >= '0' && *s <= '9' && t < 0x10000u)
            t = t * 10u + (uint32_t)(*s++ - '0');
        set_track(c, t);
        return;
    }
    put_text(dst, cap, p, n, enc);
    if (*dst) {
        c->have |= 1u << f;
        c->out->found |= c->src;
    }
}
#endif

/* ID3v2 text frame body: encoding byte, then the (first) string. */
static void id3_text(ctx_t *c, int f, const uint8_t *p, uint32_t n)
{
    int enc = p[0] == 0 ? E_LATIN1 : p[0] == 3 ? E_UTF8 : E_UTF16BE;

    if (p[0] > 3)
        return;
    p++, n--;
    if (p[-1] == 1 && n >= 2u && (be16(p) == 0xFFFEu || be16(p) == 0xFEFFu)) {
        enc = p[0] == 0xFF ? E_UTF16LE : E_UTF16BE;
        p += 2, n -= 2;
    }
    set(c, f, p, n, enc);
}

/* Undo ID3 unsynchronisation (FF 00 -> FF) in place; returns the new length. */
static uint32_t unsync(uint8_t *p, uint32_t n)
{
    uint32_t o = 0;

    for (uint32_t i = 0; i < n; i++) {
        p[o++] = p[i];
        if (p[i] == 0xFF && i + 1u < n && !p[i + 1])
            i++;
    }
    return o;
}

/* The ID3v2 tag at pos, within [pos, lim). Returns its whole length (header,
 * footer), 0 if there is no tag at pos. */
static uint32_t id3v2(ctx_t *c, uint32_t pos, uint32_t lim, uint8_t src)
{
    uint8_t *h = get(c, pos, 10);
    uint32_t ver, fl, len, total, p, end, hl;

    if (!h || lim - pos < 10u || memcmp(h, "ID3", 3) || h[3] < 2 || h[3] > 4 || ((h[6] | h[7] | h[8] | h[9]) & 0x80))
        return 0;
    ver = h[3];
    fl = h[5];
    len = syncsafe(h + 6);
    total = 10u + len + (ver == 4u && (fl & 0x10u) ? 10u : 0u);
    end = pos + 10u + min32(len, lim - pos - 10u);
    p = pos + 10u;
    if ((ver < 4u && (fl & 0x80u)) || (ver == 2u && (fl & 0x40u)))
        return total;                  /* whole-tag unsync, v2.2 compression: not supported */
    if (fl & 0x40u) {                  /* extended header */
        if (!(h = get(c, p, 4)))
            return total;
        len = ver == 3u ? be32(h) + 4u : syncsafe(h);
        p = len < 4u || len > end - p ? end : p + len;
    }
    hl = ver == 2u ? 6u : 10u;
    c->src = src;
    while (c->have != ALL && end - p >= hl) {
        uint32_t fsz, ffl, skip = 0, idl = ver == 2u ? 3u : 4u;
        int f;

        if (!(h = get(c, p, hl)))
            break;
        for (uint32_t i = 0; i < idl; i++)
            if (!((h[i] >= 'A' && h[i] <= 'Z') || (h[i] >= '0' && h[i] <= '9')))
                return total;          /* padding, or garbage */
        fsz = ver == 2u ? be24(h + 3) : ver == 3u ? be32(h + 4) : syncsafe(h + 4);
        ffl = ver == 2u ? 0u : be16(h + 8);
        f = field(h, idl, ver == 2u ? k_id3v22 : k_id3v23);
        p += hl;
        if (fsz > end - p)
            break;
        if (f >= 0 && !(c->have & 1u << f)) {
            int un = 0;

            if (ver == 3u) {
                if (ffl & 0x00C0u)     /* compressed, encrypted */
                    f = -1;
                skip = ffl & 0x0020u ? 1u : 0u;
            } else if (ver == 4u) {
                if (ffl & 0x000Cu)
                    f = -1;
                skip = (ffl & 0x0040u ? 1u : 0u) + (ffl & 0x0001u ? 4u : 0u);
                un = (ffl & 0x0002u) || (fl & 0x80u);
            }
            if (f >= 0 && fsz > skip) {
                uint32_t n = min32(fsz - skip, TEXT_MAX);
                uint8_t *d = get(c, p + skip, n);

                if (d) {
                    if (un) {
                        n = unsync(d, n);
                        c->wlen = 0;   /* the window no longer matches the file */
                    }
                    id3_text(c, f, d, n);
                }
            }
        }
        p += fsz;
    }
    return total;
}

static void id3v1(ctx_t *c, uint32_t pos)
{
    const uint8_t *t = get(c, pos, 128);

    if (!t)
        return;
    c->src = TAGS_SRC_ID3V1;
    set(c, F_TITLE, t + 3, 30, E_LATIN1);
    set(c, F_ARTIST, t + 33, 30, E_LATIN1);
    set(c, F_ALBUM, t + 63, 30, E_LATIN1);
    if (!t[125] && t[126])             /* ID3v1.1 */
        set_track(c, t[126]);
#ifdef TAGS_EXT
    set(c, F_YEAR, t + 93, 4, E_LATIN1);
    set_genre_num(c, t[127]);
#endif
}

/* APEv2 (or v1): cnt items from p up to the footer at foot, each a value
 * length, flags, a NUL-terminated key and the value. */
static void ape(ctx_t *c, uint32_t p, uint32_t foot, uint32_t cnt)
{
    uint8_t *b;

    c->src = TAGS_SRC_APE;
    while (cnt-- && c->have != ALL && foot - p > 9u) {
        uint32_t m = min32(foot - p, 8u + 256u), n, k = 8;
        int f;

        if (!(b = get(c, p, m)))
            return;
        n = le32(b);
        while (k < m && b[k])
            k++;
        if (k == m)
            return;                    /* no key */
        f = (le32(b + 4) & 6u) ? -1 : field(b + 8, k - 8u, k_ape);   /* text items only */
        p += k + 1u;
        if (n > foot - p)
            return;
        if (f >= 0 && !(c->have & 1u << f) && (b = get(c, p, min32(n, TEXT_MAX))))
            set(c, f, b, min32(n, TEXT_MAX), E_UTF8);
        p += n;
    }
}

/* MP3: tags at the end, found walking back from it, read in priority order.
 * Each probe asks for bytes that end at end, so one window serves them all. */
static void tail(ctx_t *c, uint32_t lo)
{
    uint32_t end = c->size, was, v1 = NONE, foot = NONE, items = 0, cnt = 0, app = NONE, n;
    const uint8_t *b;

    do {
        was = end;
        if (v1 == NONE && end - lo >= 128u && (b = at(c, end - 128u, 128, 1)) && !memcmp(b, "TAG", 3))
            v1 = end -= 128u;
        if (end - lo >= 15u && (b = at(c, end - 15u, 15, 1)) && !memcmp(b + 6, "LYRICS200", 9)) {
            n = 0;
            for (int i = 0; i < 6 && b[i] >= '0' && b[i] <= '9'; i++)
                n = n * 10u + (uint32_t)(b[i] - '0');
            if (n >= 11u && n <= end - lo - 15u)
                end -= n + 15u;         /* Lyrics3v2: skipped, not read */
        }
        if (foot == NONE && end - lo >= 32u && (b = at(c, end - 32u, 32, 1)) && !memcmp(b, "APETAGEX", 8)) {
            n = le32(b + 12);
            if (n >= 32u && n <= end - lo) {
                foot = end - 32u;
                cnt = le32(b + 16);
                items = end -= n;
                if ((le32(b + 20) & 0x80000000u) && end - lo >= 32u)
                    end -= 32u;         /* its header */
            }
        }
        if (app == NONE && end - lo >= 20u && (b = at(c, end - 10u, 10, 1)) && !memcmp(b, "3DI", 3) && !((b[6] | b[7] | b[8] | b[9]) & 0x80)) {
            n = syncsafe(b + 6);
            if (n <= end - lo - 20u)
                app = end -= n + 20u;
        }
    } while (end != was);

    if (foot != NONE && c->have != ALL)
        ape(c, items, foot, cnt);
    if (app != NONE && c->have != ALL)
        id3v2(c, app, c->size, TAGS_SRC_ID3V2_END);
    if (v1 != NONE && c->have != ALL)
        id3v1(c, v1);
}

/* VORBIS_COMMENT block body [p, end): vendor string, count, then the
 * comments, each a length and "KEY=value". */
static void vorbis(ctx_t *c, uint32_t p, uint32_t end)
{
    const uint8_t *b = get(c, p, 4);
    uint32_t cnt;

    if (!b || end - p < 8u || le32(b) > end - p - 8u)
        return;
    p += 4u + le32(b);
    if (!(b = get(c, p, 4)))
        return;
    cnt = le32(b);
    p += 4u;
    c->src = TAGS_SRC_VORBIS;
    while (cnt-- && c->have != ALL && end - p >= 4u && (b = get(c, p, 4)) != 0) {
        uint32_t n = le32(b), m, k = 0;
        const uint8_t *s;

        p += 4u;
        if (n > end - p)
            return;
        m = min32(n, 12u + TEXT_MAX);  /* "TRACKNUMBER=" is the longest key we want */
        if (m && (s = get(c, p, m)) != 0) {
            while (k < m && k < 12u && s[k] != '=')
                k++;
            if (k < m && s[k] == '=') {
                int f = field(s, k, k_vorbis);

                if (f >= 0)
                    set(c, f, s + k + 1u, m - k - 1u, E_UTF8);
            }
        }
        p += n;
    }
}

/* FLAC metadata blocks from pos ("fLaC"); only VORBIS_COMMENT (4) is read. */
static void flac(ctx_t *c, uint32_t pos)
{
    const uint8_t *b = get(c, pos, 4);

    if (!b || memcmp(b, "fLaC", 4))
        return;
    for (pos += 4u; (b = get(c, pos, 4)) != 0; ) {
        uint32_t len = be24(b + 1), type = b[0] & 0x7Fu, last = b[0] & 0x80u;

        pos += 4u;
        if (len > c->size - pos)
            return;
#ifdef TAGS_EXT
        if (type == 0u && len >= 18u && (b = get(c, pos, 18)) != 0) {    /* STREAMINFO */
            uint32_t rate = (uint32_t)b[10] << 12 | (uint32_t)b[11] << 4 | b[12] >> 4;
            uint64_t n = (uint64_t)(b[13] & 0x0Fu) << 32 | be32(b + 14);

            if (rate)
                c->out->dur_ms = (uint32_t)(n * 1000u / rate);
        }
#endif
        if (type == 4u) {
            vorbis(c, pos, pos + len);
            return;
        }
        if (last || type == 127u)
            return;
        pos += len;
    }
}

/* WAV: RIFF chunks in file order; LIST/INFO and "id3 " are read. */
static void wav(ctx_t *c)
{
    const uint8_t *b = get(c, 0, 12);
    uint32_t end, p = 12;

    if (!b || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4))
        return;
    end = le32(b + 4) >= 4u && le32(b + 4) <= c->size - 8u ? le32(b + 4) + 8u : c->size;
#ifdef TAGS_EXT
    {
        uint32_t bps = 0;

    while (end - p >= 8u && (b = get(c, p, 8)) != 0) {    /* (every chunk: the length is at "data") */
        uint32_t n = le32(b + 4), q, e;
        int list = !memcmp(b, "LIST", 4), id3 = !memcmp(b, "id3 ", 4) || !memcmp(b, "ID3 ", 4);

        if (!memcmp(b, "data", 4) && bps)
            c->out->dur_ms = (uint32_t)((uint64_t)min32(n, end - p - 8u) * 1000u / bps);
        else if (!memcmp(b, "fmt ", 4) && n >= 16u && (b = get(c, p + 8u, 16)) != 0)
            bps = le32(b + 8);
        p += 8u;
        if (n > end - p)
            n = end - p;                /* a truncated last chunk */
#else
    while (c->have != ALL && end - p >= 8u && (b = get(c, p, 8)) != 0) {
        uint32_t n = le32(b + 4), q, e;
        int list = !memcmp(b, "LIST", 4), id3 = !memcmp(b, "id3 ", 4) || !memcmp(b, "ID3 ", 4);

        p += 8u;
        if (n > end - p)
            n = end - p;                /* a truncated last chunk */
#endif
        if (list && n >= 4u && (b = get(c, p, 4)) && !memcmp(b, "INFO", 4)) {
            c->src = TAGS_SRC_RIFF;
            for (q = p + 4u, e = p + n; c->have != ALL && e - q >= 8u && (b = get(c, q, 8)) != 0; ) {
                uint32_t len = le32(b + 4);
                int f = field(b, 4, k_riff);

                q += 8u;
                if (len > e - q)
                    break;
                if (f >= 0 && !(c->have & 1u << f) && (b = get(c, q, min32(len, TEXT_MAX))))
                    set(c, f, b, min32(len, TEXT_MAX), E_UTF8);
                if (len + (len & 1u) >= e - q)
                    break;
                q += len + (len & 1u);
            }
        } else if (id3) {
            id3v2(c, p, p + n, TAGS_SRC_ID3V2);
        }
        if (n + (n & 1u) >= end - p)
            break;
        p += n + (n & 1u);
    }
#ifdef TAGS_EXT
    }
#endif
}

/* ---- MP4 (M4A, ALAC): moov/udta/meta/ilst ------------------------------ */

/* Box [p, end) children: the first child of type t, as its body [*b, *e). */
static int mp4_child(ctx_t *c, uint32_t p, uint32_t end, const char *t, uint32_t *b, uint32_t *e)
{
    const uint8_t *h;

    while (end - p >= 8u && (h = get(c, p, 16)) != 0) {
        uint32_t n = be32(h), hl = 8;

        if (n == 1u) {                 /* 64-bit size: only boxes under 4 GiB are walked */
            if (end - p < 16u || be32(h + 8))
                return -1;
            n = be32(h + 12);
            hl = 16;
        } else if (n == 0u) {
            n = end - p;               /* to the end */
        }
        if (n < hl || n > end - p)
            return -1;
        if (!memcmp(h + 4, t, 4)) {
            *b = p + hl;
            *e = p + n;
            return 0;
        }
        p += n;
    }
    return -1;
}

static void mp4(ctx_t *c)
{
#ifdef TAGS_EXT
    static const char k_item[9][5] = { "\xa9nam", "\xa9" "ART", "\xa9" "alb", "trkn", "aART", "\xa9gen",
                                       "disk", "\xa9" "day", "gnre" };
#define MP4_ITEMS 9
#else
    static const char k_item[4][5] = { "\xa9nam", "\xa9" "ART", "\xa9" "alb", "trkn" };
#define MP4_ITEMS 4
#endif
    uint32_t b, e, p, end;
    const uint8_t *h;

#ifdef TAGS_EXT
    if (!mp4_child(c, 0, c->size, "moov", &b, &e)) {
        uint32_t mb, me;

        if (!mp4_child(c, b, e, "mvhd", &mb, &me) && me - mb >= 32u && (h = get(c, mb, 32)) != 0) {
            uint32_t ts = h[0] == 1 ? be32(h + 20) : be32(h + 12);
            uint64_t d = h[0] == 1 ? (uint64_t)be32(h + 24) << 32 | be32(h + 28) : be32(h + 16);

            if (ts)
                c->out->dur_ms = (uint32_t)(d * 1000u / ts);
        }
    }
#endif
    if (mp4_child(c, 0, c->size, "moov", &b, &e) || mp4_child(c, b, e, "udta", &b, &e) ||
        mp4_child(c, b, e, "meta", &b, &e) || e - b < 4u ||
        mp4_child(c, b + 4u, e, "ilst", &p, &end))   /* meta is a full box: 4 bytes of version/flags */
        return;
    c->src = TAGS_SRC_MP4;
    while (c->have != ALL && end - p >= 8u && (h = get(c, p, 8)) != 0) {
        uint32_t n = be32(h), db, de;
        int f = -1;

        if (n < 8u || n > end - p)
            return;
        for (int i = 0; i < MP4_ITEMS; i++)
            if (!memcmp(h + 4, k_item[i], 4))
                f = i;
        /* the value: a "data" box, 8 bytes of type and locale, then the bytes */
        if (f >= 0 && !mp4_child(c, p + 8u, p + n, "data", &db, &de) && de - db > 8u) {
            uint32_t len = min32(de - db - 8u, TEXT_MAX);
            const uint8_t *v = get(c, db + 8u, len);

            if (v && f == F_TRACK && len >= 4u)
                set_track(c, be16(v + 2));                 /* 0, track, total */
#ifdef TAGS_EXT
            else if (v && f == F_DISC && len >= 4u && be16(v + 2) && !(c->have & 1u << F_DISC)) {
                c->out->disc = (uint8_t)min32(be16(v + 2), 255u);
                c->have |= 1u << F_DISC;
            } else if (v && f == 8 && len >= 2u)
                set_genre_num(c, be16(v) - 1u);           /* gnre: ID3v1 + 1 */
            else if (v && f != F_TRACK && f < NF && f != F_DISC)
#else
            else if (v && f != F_TRACK)
#endif
                set(c, f, v, len, E_UTF8);
        }
        p += n;
    }
}

/* ---- Ogg (Vorbis, Opus): the comment header, the second packet ----------- */

/* The packet can span pages: its pieces (file offset, length) in order, read
 * through a virtual offset space so vorbis() can walk it as one block. */
#define OGG_SPANS 24u

typedef struct {
    tags_read_at_t rd;
    void *u;
    uint32_t n, off[OGG_SPANS], len[OGG_SPANS];
#ifdef TAGS_EXT
    uint32_t size;                     /* the file's (c->size is the packet's while it is read) */
#endif
} ogg_map_t;

static uint32_t ogg_rd(void *u, uint32_t off, void *buf, uint32_t n)
{
    ogg_map_t *m = (ogg_map_t *)u;
    uint32_t got = 0, base = 0;

    for (uint32_t i = 0; i < m->n && got < n; base += m->len[i++]) {
        uint32_t want, r;

        if (off + got >= base + m->len[i])
            continue;
        want = min32(n - got, base + m->len[i] - (off + got));
        r = m->rd(m->u, m->off[i] + (off + got - base), (uint8_t *)buf + got, want);
        got += r;
        if (r != want)
            break;
    }
    return got;
}

#ifdef TAGS_EXT
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p + 4) << 32 | le32(p); }

/* The last page of the stream: its granule position is the length (in samples of rate). */
static void ogg_length(ctx_t *c, uint32_t serial, uint32_t rate, uint32_t skip)
{
    for (uint32_t back = 0; back < 2u * (TAGS_WIN - 32u) && rate; back += TAGS_WIN - 32u) {
        uint32_t lo = c->size > back + TAGS_WIN ? c->size - back - TAGS_WIN : 0;
        const uint8_t *w = get(c, lo, min32(TAGS_WIN, c->size - lo));

        for (uint32_t i = w ? min32(TAGS_WIN, c->size - lo) : 0u; i >= 27u + 1u; i--) {
            const uint8_t *g = w + i - 28u;

            if (!memcmp(g, "OggS", 4) && le32(g + 14) == serial) {
                uint64_t n = le64(g + 6);

                if (n != ~(uint64_t)0 && n > skip)
                    c->out->dur_ms = (uint32_t)((n - skip) * 1000u / rate);
                return;
            }
        }
        if (!lo)
            return;
    }
}
#endif

static void ogg(ctx_t *c)
{
    ogg_map_t m;
    uint32_t pos = 0, packet = 0, total = 0, serial = 0;
    int pages = 0;
    const uint8_t *h;
#ifdef TAGS_EXT
    uint32_t rate = 0, skip = 0;
#endif

    m.rd = c->rd;
    m.u = c->u;
    m.n = 0;
#ifdef TAGS_EXT
    m.size = c->size;
#endif
    while (packet < 2u && pages++ < 64 && (h = get(c, pos, 27)) != 0 && !memcmp(h, "OggS", 4)) {
        uint32_t nseg = h[26], data, sz = 0;
        const uint8_t *lace;

        if (pages == 1)
            serial = le32(h + 14);
        else if (le32(h + 14) != serial)
            break;                     /* another logical stream */
        if (!(lace = get(c, pos + 27u, nseg)))
            break;
        data = pos + 27u + nseg;
#ifdef TAGS_EXT
        if (pages == 1) {
            const uint8_t *id = get(c, data, 19);

            if (id && !memcmp(id, "\x01vorbis", 7))
                rate = le32(id + 12);
            else if (id && !memcmp(id, "OpusHead", 8)) {
                rate = 48000u;
                skip = (uint32_t)id[10] | (uint32_t)id[11] << 8;
            }
            if (!(lace = get(c, pos + 27u, nseg)))
                break;
        }
#endif
        for (uint32_t i = 0; i < nseg; sz += lace[i++]) {
            if (lace[i] > c->size - data || sz > c->size - data - lace[i]) {
                packet = 2u;           /* the page runs past the end of the file */
                break;
            }
            if (packet == 1u && lace[i]) {             /* a piece of the comment packet */
                if (m.n && m.off[m.n - 1] + m.len[m.n - 1] == data + sz)
                    m.len[m.n - 1] += lace[i];
                else if (m.n < OGG_SPANS) {
                    m.off[m.n] = data + sz;
                    m.len[m.n++] = lace[i];
                } else {
                    packet = 2u;       /* enough of it (a large cover follows the text) */
                }
                if (packet == 1u)
                    total += lace[i];
            }
            if (packet < 2u && lace[i] < 255u)
                packet++;
        }
        pos = data + sz;
    }
    if (!m.n) {
#ifdef TAGS_EXT
        ogg_length(c, serial, rate, skip);
#endif
        return;
    }
    /* the packet: "\x03vorbis" or "OpusTags", then the Vorbis comment structure */
    c->rd = ogg_rd;
    c->u = &m;
    c->size = total;
    c->woff = c->wlen = 0;
    if ((h = get(c, 0, 8)) != 0 && (!memcmp(h, "\x03vorbis", 7) || !memcmp(h, "OpusTags", 8))) {
        uint32_t skip = h[0] == 3 ? 7u : 8u;

        vorbis(c, skip, total);
        if (c->out->found & TAGS_SRC_VORBIS)
            c->out->found = (uint8_t)((c->out->found & ~TAGS_SRC_VORBIS) | TAGS_SRC_OGG);
    }
    c->rd = m.rd;
    c->u = m.u;
#ifdef TAGS_EXT
    c->size = m.size;
    c->woff = c->wlen = 0;
    ogg_length(c, serial, rate, skip);
#endif
}

#ifdef TAGS_EXT
/* ---- the library's other kinds: WMA (ASF), DSF, AIFF; WavPack's length --------------------- */

static int asf_name(const uint8_t *p, uint32_t n, const char *a)   /* UTF-16LE name == ASCII a */
{
    uint32_t i = 0;

    for (; a[i] && 2u * i + 1u < n; i++)
        if (p[2u * i] != (uint8_t)a[i] || p[2u * i + 1u])
            return 0;
    return !a[i] && (2u * i == n || (2u * i + 2u == n && !p[2u * i] && !p[2u * i + 1u]));
}

static void asf(ctx_t *c)
{
    static const uint8_t k_hdr[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA,
                                       0x00, 0x62, 0xCE, 0x6C };
    static const uint8_t k_file[4] = { 0xA1, 0xDC, 0xAB, 0x8C }, k_cd[4] = { 0x33, 0x26, 0xB2, 0x75 },
                         k_ecd[4] = { 0x40, 0xA4, 0xD0, 0xD2 };
    static const char *const k_wm[] = { 0, 0, "WM/AlbumTitle", "WM/TrackNumber", "WM/AlbumArtist", "WM/Genre",
                                        "WM/PartOfSet", "WM/Year" };
    const uint8_t *h = get(c, 0, 30);
    uint32_t p = 30, end;

    if (!h || memcmp(h, k_hdr, 16) || le32(h + 20))
        return;
    end = min32(le32(h + 16), c->size);
    c->src = TAGS_SRC_ID3V2;
    while (end - p >= 24u && (h = get(c, p, 24)) != 0) {
        uint32_t n = le32(h + 16);

        if (le32(h + 20) || n < 24u || n > end - p)
            return;
        if (!memcmp(h, k_file, 4) && n >= 88u && (h = get(c, p + 64u, 24)) != 0) {
            uint64_t d = (uint64_t)le32(h + 4) << 32 | le32(h), pre = le32(h + 16);

            c->out->dur_ms = d / 10000u > pre ? (uint32_t)(d / 10000u - pre) : 0u;
        } else if (!memcmp(h, k_cd, 4) && n >= 34u && (h = get(c, p + 24u, 10)) != 0) {
            uint32_t tl = h[0] | h[1] << 8, al = h[2] | h[3] << 8, q = p + 34u;
            const uint8_t *v;

            if (tl && tl <= n - 34u && (v = get(c, q, min32(tl, TEXT_MAX))) != 0)
                set(c, F_TITLE, v, min32(tl, TEXT_MAX), E_UTF16LE);
            if (al && tl + al <= n - 34u && (v = get(c, q + tl, min32(al, TEXT_MAX))) != 0)
                set(c, F_ARTIST, v, min32(al, TEXT_MAX), E_UTF16LE);
        } else if (!memcmp(h, k_ecd, 4) && n >= 26u && (h = get(c, p + 24u, 2)) != 0) {
            uint32_t cnt = h[0] | h[1] << 8, q = p + 26u, e = p + n;

            while (cnt-- && e - q >= 6u && (h = get(c, q, 2)) != 0) {
                uint32_t nl = h[0] | h[1] << 8, t, vl;
                int f = -1;
                const uint8_t *k, *v;

                if (nl > e - q - 6u || nl > 64u || !(k = get(c, q + 2u, nl + 4u)))
                    return;
                t = k[nl] | k[nl + 1] << 8;
                vl = k[nl + 2] | k[nl + 3] << 8;
                for (int i = 2; i < NF; i++)
                    if (asf_name(k, nl, k_wm[i]))
                        f = i;
                q += 2u + nl + 4u;
                if (vl > e - q)
                    return;
                if (f >= 0 && !(c->have & 1u << f) && (v = get(c, q, min32(vl, TEXT_MAX))) != 0) {
                    if (t == 0u)
                        set(c, f, v, min32(vl, TEXT_MAX), E_UTF16LE);
                    else if ((t == 3u && vl >= 4u) || (t == 5u && vl >= 2u)) {
                        uint32_t x = t == 3u ? le32(v) : (uint32_t)(v[0] | v[1] << 8);
                        char d[12];
                        int k2 = 11;

                        d[11] = 0;
                        do d[--k2] = (char)('0' + x % 10u); while ((x /= 10u) && k2);
                        set(c, f, (const uint8_t *)d + k2, (uint32_t)(11 - k2), E_LATIN1);
                    }
                }
                q += vl;
            }
        }
        p += n;
    }
}

static void dsf(ctx_t *c)
{
    const uint8_t *h = get(c, 0, 72);

    if (!h || memcmp(h, "DSD ", 4) || memcmp(h + 28, "fmt ", 4))
        return;
    if (le32(h + 56))
        c->out->dur_ms = (uint32_t)(((uint64_t)le32(h + 68) << 32 | le32(h + 64)) * 1000u / le32(h + 56));
    if (!le32(h + 24) && le32(h + 20) && le32(h + 20) < c->size)
        id3v2(c, le32(h + 20), c->size, TAGS_SRC_ID3V2);
}

static void aiff(ctx_t *c)
{
    const uint8_t *b = get(c, 0, 12);
    uint32_t p = 12, end;

    if (!b || memcmp(b, "FORM", 4) || (memcmp(b + 8, "AIFF", 4) && memcmp(b + 8, "AIFC", 4)))
        return;
    end = be32(b + 4) <= c->size - 8u ? be32(b + 4) + 8u : c->size;
    while (end - p >= 8u && (b = get(c, p, 8)) != 0) {
        uint32_t n = be32(b + 4);

        p += 8u;
        if (n > end - p)
            n = end - p;
        if (!memcmp(b, "COMM", 4) && n >= 18u && (b = get(c, p, 18)) != 0) {
            int sh = 16383 + 31 - (int)(be16(b + 8) & 0x7FFFu);
            uint32_t rate = sh >= 0 && sh < 32 ? be32(b + 10) >> sh : 0u;

            if (rate)
                c->out->dur_ms = (uint32_t)((uint64_t)be32(b + 2) * 1000u / rate);
        } else if (!memcmp(b, "ID3 ", 4) || !memcmp(b, "id3 ", 4)) {
            id3v2(c, p, p + n, TAGS_SRC_ID3V2);
        }
        if (n + (n & 1u) >= end - p)
            break;
        p += n + (n & 1u);
    }
}

static void wv_length(ctx_t *c, uint32_t pos)
{
    static const uint32_t k_rates[15] = { 6000, 8000, 9600, 11025, 12000, 16000, 22050, 24000, 32000, 44100,
                                          48000, 64000, 88200, 96000, 192000 };
    const uint8_t *b = get(c, pos, 32);
    uint32_t r;

    if (b && !memcmp(b, "wvpk", 4) && le32(b + 12) != 0xFFFFFFFFu && (r = le32(b + 24) >> 23 & 15u) < 15u)
        c->out->dur_ms = (uint32_t)((uint64_t)le32(b + 12) * 1000u / k_rates[r]);
}
#endif

int tags_read(tags_read_at_t rd, void *u, uint32_t size, int kind, tag_info_t *out)
{
    ctx_t c;

    memset(out, 0, sizeof *out);
    c.rd = rd;
    c.u = u;
    c.size = size;
    c.woff = c.wlen = 0;
    c.out = out;
    c.have = 0;
    c.src = 0;
    if (kind == TAGS_WAV) {
        wav(&c);
    } else if (kind == TAGS_MP4) {
        mp4(&c);
    } else if (kind == TAGS_OGG) {
        ogg(&c);
#ifdef TAGS_EXT
    } else if (kind == TAGS_WMA) {
        asf(&c);
    } else if (kind == TAGS_DSF) {
        dsf(&c);
    } else if (kind == TAGS_AIFF) {
        aiff(&c);
#endif
    } else {
        uint32_t pos = 0, n;

        for (int i = 0; i < 8 && c.have != ALL && (n = id3v2(&c, pos, size, TAGS_SRC_ID3V2)) != 0 && n <= size - pos; i++)
            pos += n;                  /* taggers sometimes leave two */
#ifdef TAGS_EXT
        if (kind == TAGS_WV)
            wv_length(&c, pos);
#endif
        if (kind == TAGS_FLAC)
            flac(&c, pos);
        else if (c.have != ALL)
            tail(&c, pos);
    }
    return c.have ? 0 : -1;
}
