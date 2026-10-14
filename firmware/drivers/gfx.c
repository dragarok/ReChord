/*
 * gfx.c -- strip drawing and the stock bitmap fonts (gfx.h).
 *
 * Font layout in our firmware slot (tools/make_fw1.py, FONT_*): glyphs indexed
 * by BMP code point, rows u16 big-endian (MSB = leftmost pixel), then a byte
 * 0x80 | advance. SMALL: 32 bytes a glyph, 12 rows. LARGE: 33 bytes, 16 rows.
 */
#include <string.h>

#include "gfx.h"
#include "font.h"
#include "display.h"

#define FONT_LARGE_OFF   0x23C000u       /* (0x44C000 - 0x10000 * 33) & ~3 */
#define FONT_SMALL_OFF   0x22C000u       /* FONT_LARGE_OFF - 0x800 * 32 */
#define FONT_SMALL_COUNT 0x800u

static gfx_read_t s_rd;
static uint32_t s_slot;
static int s_ok;

/* 256 decoded glyphs, direct-mapped: ASCII of both fonts never collide */
typedef struct {
    uint32_t key;                        /* font << 16 | cp, + 1; 0 = empty */
    uint8_t rows[32];                    /* big-endian u16 rows as stored */
    uint8_t adv;
} gl_t;
static gl_t s_gl[256];
uint32_t gfx_glyph_misses, gfx_glyph_reads;

static uint16_t *s_buf;
static int s_y0, s_n;

/* Reads n bytes at offset off of the slot (at most 2 sectors). */
static int slot_read(uint32_t off, uint8_t *out, uint32_t n)
{
    uint32_t sec[256];                   /* 2 sectors */
    uint32_t lba = off >> 9, in = off & 511u, cnt = in + n > 512u ? 2u : 1u;

    gfx_glyph_reads++;
    if (s_rd(s_slot + lba, cnt, sec))
        return -1;
    memcpy(out, (uint8_t *)sec + in, n);
    return 0;
}

int gfx_fonts_init(gfx_read_t rd, uint32_t slot_lba)
{
    uint8_t a[33], b[32];

    s_rd = rd;
    s_slot = slot_lba;
    memset(s_gl, 0, sizeof s_gl);
    s_ok = rd && !slot_read(FONT_LARGE_OFF + 'A' * 33u, a, 33) && !slot_read(FONT_SMALL_OFF + 'A' * 32u, b, 32) &&
           (a[32] & 0x80u) && (a[32] & 0x7Fu) >= 4u && (a[32] & 0x7Fu) <= 16u &&
           (b[24] & 0x80u) && (b[24] & 0x7Fu) >= 4u && (b[24] & 0x7Fu) <= 12u;
    return s_ok;
}

int gfx_fonts_ok(void)
{
    return s_ok;
}

int gfx_font_h(int font)
{
    return s_ok ? (font == GFX_LARGE ? 16 : 12) : 8;
}

static const gl_t *glyph(int font, uint32_t cp)
{
    uint32_t key = ((uint32_t)font << 16 | cp) + 1u;
    gl_t *g = &s_gl[(cp + (uint32_t)font * 0x80u) & 0xFFu];

    if (g->key == key)
        return g;
    gfx_glyph_misses++;
    g->key = key;
    memset(g->rows, 0, sizeof g->rows);
    g->adv = 0;
    if (s_ok && (font == GFX_LARGE || cp < FONT_SMALL_COUNT)) {
        uint8_t raw[33];
        int h = font == GFX_LARGE ? 16 : 12;

        if (font == GFX_LARGE ? slot_read(FONT_LARGE_OFF + cp * 33u, raw, 33)
                              : slot_read(FONT_SMALL_OFF + cp * 32u, raw, 32)) {
            g->key = 0;                  /* I/O error: try again next time */
            return g;
        }
        if (raw[h * 2] & 0x80u) {
            memcpy(g->rows, raw, (size_t)h * 2u);
            g->adv = raw[h * 2] & 0x7Fu;
        }
        return g;
    }
    if (!s_ok && cp >= FONT_FIRST_CODE && cp <= FONT_LAST_CODE) {
        const uint8_t *r = font_glyph((uint8_t)cp);

        for (int i = 0; i < 8; i++)
            g->rows[i * 2] = r[i];
        g->adv = 8;
    }
    return g;
}

void gfx_glyph(int font, uint32_t cp, gfx_glyph_t *out)
{
    const gl_t *g = glyph(font, cp);

    out->h = (uint8_t)gfx_font_h(font);
    out->adv = g->adv;
    for (int i = 0; i < 16; i++)
        out->rows[i] = (uint16_t)(g->rows[i * 2] << 8 | g->rows[i * 2 + 1]);
}

void gfx_target(uint16_t *buf, int y0, int n)
{
    s_buf = buf;
    s_y0 = y0;
    s_n = n;
}

int gfx_strip_y0(void) { return s_y0; }
int gfx_strip_n(void) { return s_n; }

void gfx_fill(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (y0 < s_y0) y0 = s_y0;
    if (y1 >= s_y0 + s_n) y1 = s_y0 + s_n - 1;
    if (x0 < 0) x0 = 0;
    if (x1 >= DISPLAY_W) x1 = DISPLAY_W - 1;
    for (int y = y0; y <= y1; y++) {
        uint16_t *p = s_buf + (y - s_y0) * DISPLAY_W + x0;

        for (int x = x0; x <= x1; x++)
            *p++ = c;
    }
}

/* Next code point of UTF-8 text; malformed bytes come out as U+FFFD. */
static uint32_t utf8_next(const char **ps)
{
    const uint8_t *s = (const uint8_t *)*ps;
    uint32_t c = *s++;
    int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;

    if (c >= 0x80 && c < 0xC0) {
        *ps = (const char *)s;
        return 0xFFFDu;
    }
    if (n)
        c &= 0x3Fu >> n;
    for (; n; n--, s++) {
        if ((*s & 0xC0) != 0x80) {
            c = 0xFFFDu;
            break;
        }
        c = c << 6 | (*s & 0x3Fu);
    }
    *ps = (const char *)s;
    return c > 0xFFFFu ? 0xFFFDu : c;
}

static int adv_of(const gl_t *g, int font)
{
    return g->adv ? g->adv : font == GFX_LARGE ? 8 : 6;       /* missing glyph: a gap */
}

static void draw_glyph(int x, int y, const gl_t *g, int h, uint16_t fg, int xmax)
{
    int r0 = s_y0 > y ? s_y0 - y : 0, r1 = s_y0 + s_n - y < h ? s_y0 + s_n - y : h;

    for (int r = r0; r < r1; r++) {
        uint32_t bits = (uint32_t)g->rows[r * 2] << 8 | g->rows[r * 2 + 1];
        uint16_t *p = s_buf + (y + r - s_y0) * DISPLAY_W;

        for (int c = 0; bits; c++, bits = (bits << 1) & 0xFFFFu) {
            int px = x + c;

            if ((bits & 0x8000u) && px >= 0 && px < xmax && px < DISPLAY_W)
                p[px] = fg;
        }
    }
}

int gfx_text(int x, int y, int font, const char *s, uint16_t fg, int xmax)
{
    int h = gfx_font_h(font), y_off = 0;

    if (!s_ok)
        y_off = font == GFX_LARGE ? 4 : 2;           /* centre the 8x8 fallback */
    if (y + y_off >= s_y0 + s_n || y + y_off + h <= s_y0) {
        /* not in this strip: only the width matters */
        while (*s && x < xmax)
            x += adv_of(glyph(font, utf8_next(&s)), font);
        return x;
    }
    while (*s && x < xmax) {
        const gl_t *g = glyph(font, utf8_next(&s));

        draw_glyph(x, y + y_off, g, h, fg, xmax);
        x += adv_of(g, font);
    }
    return x;
}

int gfx_text_w(int font, const char *s)
{
    int w = 0;

    while (*s)
        w += adv_of(glyph(font, utf8_next(&s)), font);
    return w;
}

void gfx_text_box(int x, int y, int xmax, int font, const char *s, uint16_t fg, int align)
{
    int w = gfx_text_w(font, s), room = xmax - x;

    if (w <= room) {
        gfx_text(align == 1 ? xmax - w : align == 2 ? x + (room - w) / 2 : x, y, font, s, fg, xmax);
        return;
    }
    {
        int dots = gfx_text_w(font, "..."), end = x;
        const char *p = s;

        while (*p) {                                 /* glyphs that fit before "..." */
            const char *q = p;
            int a = adv_of(glyph(font, utf8_next(&q)), font);

            if (end + a > xmax - dots)
                break;
            end += a;
            p = q;
        }
        gfx_text(x, y, font, s, fg, end);
        gfx_text(end, y, font, "...", fg, xmax);
    }
}

void gfx_icon(int x, int y, const uint16_t *rows, uint16_t c)
{
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        uint32_t bits = rows[r];

        if (py < s_y0 || py >= s_y0 + s_n)
            continue;
        for (int i = 0; bits; i++, bits = (bits << 1) & 0xFFFFu)
            if ((bits & 0x8000u) && x + i >= 0 && x + i < DISPLAY_W)
                s_buf[(py - s_y0) * DISPLAY_W + x + i] = c;
    }
}
