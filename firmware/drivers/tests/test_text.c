/*
 * test_text.c -- host tests for the text engine (text.c) and the icons (icons.c):
 *   1. UTF-8 decoding (valid, malformed, overlong, surrogate, truncated)
 *   2. the goldens of tools/gen_font.py: per case the width, the shaped glyphs, the
 *      ellipsis cut and the rendered band, byte for byte against its reference renderer
 *      (which draws Latin exactly as tools/ui_mockups.py does), also drawn in 5-row bands
 *   3. Devanagari against HarfBuzz (fonts/deva_hb.txt): the agreement must equal what the
 *      generator's simulation of this shaper reached
 *   4. text_fit / text_wrap / clipping / band edges / fonts not loaded
 *   5. icons
 *   6. speed: a 320 x 16 band of B13 text
 *
 * Usage: test_text <golden dir> <deva_hb.txt> [out dir for the rendered PPMs]
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "text.h"
#include "icons.h"
#include "test_util.h"

extern const uint32_t rechord_fonts_blob[];
extern const uint32_t rechord_fonts_len;

static const font_t *font_by_name(const char *n)
{
    if (!strcmp(n, "S8")) return &font_s8;
    if (!strcmp(n, "B13")) return &font_b13;
    if (!strcmp(n, "M13")) return &font_m13;
    if (!strcmp(n, "T17")) return &font_t17;
    if (!strcmp(n, "D28")) return &font_d28;
    return NULL;
}

static int unhex(const char *h, char *out, int cap)
{
    int n = 0;

    while (h[0] && h[1] && n < cap - 1) {
        unsigned v;

        if (sscanf(h, "%2x", &v) != 1)
            break;
        out[n++] = (char)v;
        h += 2;
    }
    out[n] = 0;
    return n;
}

static void test_utf8(void)
{
    struct { const char *s; int n; uint32_t cp[6]; int ncp; } t[] = {
        { "A", 1, { 0x41 }, 1 },
        { "\xC3\xA9", 2, { 0xE9 }, 1 },
        { "\xE2\x80\x99", 3, { 0x2019 }, 1 },
        { "\xF0\x9F\x8E\xB5", 4, { 0x1F3B5 }, 1 },
        { "\xFF" "a", 2, { 0xFFFD, 0x61 }, 2 },
        { "\xC0\xAF", 2, { 0xFFFD, 0xFFFD }, 2 },              /* overlong '/' */
        { "\xE0\x80\xAF", 3, { 0xFFFD, 0xFFFD, 0xFFFD }, 3 },  /* overlong */
        { "\xED\xA0\x80", 3, { 0xFFFD, 0xFFFD, 0xFFFD }, 3 },  /* surrogate */
        { "\xE2\x82", 2, { 0xFFFD, 0xFFFD }, 2 },              /* truncated */
        { "\xF4\x90\x80\x80", 4, { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD }, 4 },  /* > U+10FFFF */
    };

    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        const char *p = t[i].s, *e = t[i].s + t[i].n;
        int k = 0;

        while (p < e && k < 6) {
            uint32_t c = text_utf8_next(&p, e);

            CHECK(c == t[i].cp[k]);
            k++;
        }
        CHECK(k == t[i].ncp && p == e);
    }
}

/* Render a whole line into a band of the font's line height, or in 5-row bands. */
static void render(const font_t *f, const char *s, int n, uint16_t fg, uint16_t bg, uint16_t *buf, int h, int step)
{
    for (int i = 0; i < 320 * h; i++)
        buf[i] = bg;
    for (int y = 0; y < h; y += step) {
        int bh = h - y < step ? h - y : step;

        text_draw_band(f, buf + y * 320, y, bh, 4, text_base(f), 0, 320, s, n, fg);
    }
}

static int ppm_equal(const uint16_t *buf, int h, const char *path, const char *save)
{
    FILE *fp = fopen(path, "rb");
    static uint8_t want[320 * 64 * 3 + 64], got[320 * 64 * 3 + 64];
    int hl, n, wn;

    if (!fp)
        return 0;
    wn = (int)fread(want, 1, sizeof want, fp);
    fclose(fp);
    hl = sprintf((char *)got, "P6\n320 %d\n255\n", h);
    n = hl;
    for (int i = 0; i < 320 * h; i++) {
        unsigned v = buf[i], r = v >> 11, g = v >> 5 & 63, b = v & 31;

        got[n++] = (uint8_t)(r << 3 | r >> 2);
        got[n++] = (uint8_t)(g << 2 | g >> 4);
        got[n++] = (uint8_t)(b << 3 | b >> 2);
    }
    if (save) {
        FILE *o = fopen(save, "wb");

        if (o) {
            fwrite(got, 1, (size_t)n, o);
            fclose(o);
        }
    }
    return wn == n && !memcmp(want, got, (size_t)n);
}

static void test_goldens(const char *gdir, const char *outdir)
{
    char path[512], line[8192];
    FILE *fp;
    int cases = 0;

    snprintf(path, sizeof path, "%s/text_cases.txt", gdir);
    fp = fopen(path, "r");
    CHECK(fp != NULL);
    if (!fp)
        return;
    while (fgets(line, sizeof line, fp)) {
        char id[64], fn[8], hex[1024], text[512], fit[512], fitwant[512], *bar, *fitp;
        int fitw, width, n;
        unsigned fg, bg;
        const font_t *f;
        text_glyph_t sh[128];
        static uint16_t buf[320 * 64], buf2[320 * 64];
        int h, ok_shape = 1, ns, k = 0;

        if (line[0] == '#')
            continue;
        if (sscanf(line, "%63s %7s %d %x %x %1023s", id, fn, &fitw, &fg, &bg, hex) != 6)
            continue;
        f = font_by_name(fn);
        n = unhex(hex, text, sizeof text);
        bar = strchr(line, '|');
        width = atoi(bar + 1);
        CHECK(text_width(f, text, n) == width);
        if (text_width(f, text, n) != width)
            printf("  %s: width %d, want %d\n", id, text_width(f, text, n), width);
        fitp = strstr(line, "| fit ");
        if (fitp)
            unhex(fitp + 6, fitwant, sizeof fitwant);
        /* shaped glyphs */
        ns = text_shape(f, text, n, sh, 128);
        bar = strchr(bar + 1, '|');
        for (char *t = strtok(bar + 1, " \n"); t && strcmp(t, "|"); t = strtok(NULL, " \n")) {
            unsigned code;
            int x, y, adv;

            if (sscanf(t, "%x:%d:%d:%d", &code, &x, &y, &adv) != 4)
                break;
            if (k >= ns || sh[k].code != code || sh[k].x != x || sh[k].y != y || sh[k].adv != adv) {
                if (ok_shape)
                    printf("  %s: glyph %d: got %04X:%d:%d:%d want %04X:%d:%d:%d\n", id, k,
                           k < ns ? sh[k].code : 0, k < ns ? sh[k].x : 0, k < ns ? sh[k].y : 0,
                           k < ns ? sh[k].adv : 0, code, x, y, adv);
                ok_shape = 0;
            }
            k++;
        }
        CHECK(ok_shape && k == ns);
        /* the ellipsis cut */
        if (fitw) {
            char got[512], *want = fitwant;

            CHECK(fitp != NULL);
            if (fitp) {
                int wn = (int)strlen(want), gn = text_fit(f, text, n, fitw, got, sizeof got);

                CHECK(gn == wn && !memcmp(got, want, (size_t)wn));
                CHECK(text_width(f, got, gn) <= fitw);
                memcpy(fit, got, (size_t)gn + 1);
                n = gn;
                memcpy(text, fit, (size_t)n + 1);
            }
        }
        /* the band, whole and in 5-row bands */
        h = text_line_h(f);
        render(f, text, n, (uint16_t)fg, (uint16_t)bg, buf, h, h);
        snprintf(path, sizeof path, "%s/%s.ppm", gdir, id);
        {
            char save[512];

            snprintf(save, sizeof save, "%s/%s.ppm", outdir ? outdir : ".", id);
            CHECK(ppm_equal(buf, h, path, outdir ? save : NULL));
            if (!ppm_equal(buf, h, path, NULL))
                printf("  %s: band differs from %s\n", id, path);
        }
        render(f, text, n, (uint16_t)fg, (uint16_t)bg, buf2, h, 5);
        CHECK(!memcmp(buf, buf2, sizeof(uint16_t) * 320 * (size_t)h));
        cases++;
    }
    fclose(fp);
    CHECK(cases >= 10);
    printf("goldens: %d cases\n", cases);
}

static void test_deva(const char *path)
{
    FILE *fp = fopen(path, "r");
    char line[2048];
    const font_t *f = NULL;
    int want_ok = -1, want_pos = -1, ok = 0, okpos = 0, words = 0;

    CHECK(fp != NULL);
    if (!fp)
        return;
    for (;;) {
        char *got = fgets(line, sizeof line, fp);

        if (!got || !strncmp(line, "# font ", 7)) {
            if (f) {
                printf("devanagari %s: %d words, glyphs as HarfBuzz %d (sim %d), with mark offsets %d (sim %d)\n",
                       f == &font_b13 ? "B13" : f == &font_m13 ? "M13" : "T17", words, ok, want_ok, okpos, want_pos);
                CHECK(ok == want_ok && okpos == want_pos);
                CHECK(ok * 100 >= words * 80);
            }
            if (!got)
                break;
            {
                char fn[8];

                sscanf(line + 7, "%7s agree %d %d", fn, &want_ok, &want_pos);
                f = font_by_name(fn);
                ok = okpos = words = 0;
            }
            continue;
        }
        if (line[0] == '#' || !f)
            continue;
        {
            char *tab = strchr(line, '\t');
            text_glyph_t sh[64];
            int ns, k = 0, same = 1, samepos = 1, pen = 0;

            if (!tab)
                continue;
            ns = text_shape(f, line, (int)(tab - line), sh, 64);
            for (char *t = strtok(tab + 1, " \n"); t; t = strtok(NULL, " \n")) {
                unsigned code;
                int dx, dy;

                if (sscanf(t, "%x:%d:%d", &code, &dx, &dy) != 3)
                    break;
                if (k >= ns || sh[k].code != code)
                    same = 0;
                else if (sh[k].x - pen != dx || sh[k].y != dy)
                    samepos = 0;
                if (k < ns)
                    pen += sh[k].adv;
                k++;
            }
            if (k != ns)
                same = 0;
            words++;
            ok += same;
            okpos += same && samepos;
        }
    }
    fclose(fp);
}

static void test_fit_wrap(void)
{
    char out[128];
    text_line_t ln[4];
    const char *s = "Khamoshiyan Unplugged Version";
    int n, w = text_width(&font_b13, s, -1);

    /* fits: copied whole */
    n = text_fit(&font_b13, s, -1, w, out, sizeof out);
    CHECK(n == (int)strlen(s) && !strcmp(out, s));
    /* one px short: cut with "…" */
    n = text_fit(&font_b13, s, -1, w - 1, out, sizeof out);
    CHECK(n > 3 && !strcmp(out + n - 3, "\xE2\x80\xA6") && text_width(&font_b13, out, n) <= w - 1);
    /* a cut after a space drops the space: "Tum Hi Ho…" */
    {
        int ww = text_width(&font_b13, "Tum Hi Ho ", -1) + text_width(&font_b13, "\xE2\x80\xA6", -1);

        n = text_fit(&font_b13, "Tum Hi Ho Unplugged", -1, ww, out, sizeof out);
        CHECK(n == 12 && !strcmp(out, "Tum Hi Ho\xE2\x80\xA6"));
    }
    /* too narrow for anything */
    CHECK(text_fit(&font_b13, s, -1, 2, out, sizeof out) == 0 && out[0] == 0);
    /* a tiny buffer: whole clusters, NUL-terminated */
    n = text_fit(&font_b13, s, -1, 1000, out, 6);
    CHECK(n == 5 && out[5] == 0);
    /* never inside a Devanagari syllable: every cut ends at a cluster boundary */
    {
        const char *h = "\xE0\xA4\x95\xE0\xA4\xBF\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xAE\xE0\xA4\xA4"; /* किस्मत */
        int hw = text_width(&font_b13, h, -1);

        for (int mw = 0; mw < hw; mw++) {
            n = text_fit(&font_b13, h, -1, mw, out, sizeof out);
            CHECK(n == 0 || n == 3 + 6 || n == 3 + 15 || n == 3);
            CHECK(text_width(&font_b13, out, n) <= mw);
        }
    }
    /* wrap: two lines on a word boundary, the rest on the last */
    n = text_wrap(&font_t17, "Khamoshiyan Unplugged Version (Live at the Royal Albert Hall)", -1, 150, 2, ln);
    CHECK(n == 2);
    CHECK(ln[0].off == 0 && ln[0].w <= 150);
    CHECK(ln[1].off == ln[0].len + 1);
    CHECK(ln[1].off + ln[1].len == (int)strlen("Khamoshiyan Unplugged Version (Live at the Royal Albert Hall)"));
    n = text_wrap(&font_b13, "Tum Hi Ho", -1, 300, 3, ln);
    CHECK(n == 1 && ln[0].len == 9);
    /* a word longer than the line breaks inside it */
    n = text_wrap(&font_b13, "Supercalifragilisticexpialidocious", -1, 60, 3, ln);
    CHECK(n == 3 && ln[0].w <= 60 && ln[1].w <= 60 && ln[0].len > 0 && ln[1].off == ln[0].len);
    /* a hyphen gives way: the line ends after it */
    {
        const char *a = "Arijit Singh-Shreya Ghoshal";
        int wh = text_width(&font_b13, "Arijit Singh-", -1);

        n = text_wrap(&font_b13, a, -1, wh + 5, 2, ln);
        CHECK(n == 2 && ln[0].len == (int)strlen("Arijit Singh-") && ln[0].w == wh && ln[1].off == ln[0].len);
    }
    CHECK(text_wrap(&font_b13, "", -1, 100, 2, ln) == 0);
    CHECK(text_wrap(&font_b13, "   ", -1, 100, 2, ln) == 0);
}

static void test_clip_and_edges(void)
{
    static uint16_t band[320 * 16];
    int w;

    /* clip window: nothing outside [100, 140) */
    for (int i = 0; i < 320 * 16; i++)
        band[i] = 0x1083;
    w = text_draw_band(&font_b13, band, 100, 16, 90, 112, 100, 140, "MMMMMMMMMMMMMM", -1, 0xFFFF);
    CHECK(w == text_width(&font_b13, "MMMMMMMMMMMMMM", -1));
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 320; x++)
            if (x < 100 || x >= 140)
                CHECK(band[y * 320 + x] == 0x1083);
    {
        int inked = 0;

        for (int i = 0; i < 320 * 16; i++)
            inked += band[i] != 0x1083;
        CHECK(inked > 100);
    }
    /* a band the text does not reach: untouched, the advance still returned */
    for (int i = 0; i < 320 * 16; i++)
        band[i] = 0;
    CHECK(text_draw_band(&font_b13, band, 0, 16, 0, 100, 0, 320, "Hello", -1, 0xFFFF) ==
          text_width(&font_b13, "Hello", -1));
    for (int i = 0; i < 320 * 16; i++)
        CHECK(band[i] == 0);
    /* text off the right and left edges, negative x: no writes outside the band */
    {
        static uint16_t big[320 * 18];

        for (int i = 0; i < 320 * 18; i++)
            big[i] = 0xAAAA;
        text_draw_band(&font_t17, big + 320, 0, 16, -50, 13, -100, 1000, "Kun Faya Kun Kun Faya Kun Kun Faya Kun", -1, 0);
        text_draw_band(&font_t17, big + 320, 0, 16, 280, 13, 0, 320, "Kun Faya Kun", -1, 0);
        for (int x = 0; x < 320; x++)
            CHECK(big[x] == 0xAAAA && big[17 * 320 + x] == 0xAAAA);
    }
    /* missing glyphs: a box, never '?'; zero-width controls: nothing */
    {
        text_glyph_t g[4];

        CHECK(text_shape(&font_b13, "\xE4\xBD\xA0", -1, g, 4) == 1 && g[0].code == 0xFFFD);   /* 你 */
        CHECK(text_shape(&font_b13, "a\xE2\x80\x8D" "b", -1, g, 4) == 2);                    /* ZWJ */
        CHECK(text_shape(&font_b13, "\xC2\xAD", -1, g, 4) == 0);                              /* SHY */
    }
    /* NULs and NULL */
    CHECK(text_width(&font_b13, NULL, 3) == 0);
    CHECK(text_width(&font_b13, "", -1) == 0);
}

static void test_icons(void)
{
    static uint16_t band[320 * 16];

    CHECK(ICON_COUNT == 42);
    CHECK(icon_width(ICON_CHEV) == 7 && icon_width(ICON_FOLDER) == 12 && icon_width(-1) == 0);
    for (int i = 0; i < 320 * 16; i++)
        band[i] = 0;
    /* the folder icon at (10, 2) in a band of rows 0..15: its pixels, and nothing else */
    CHECK(icon_draw_band(ICON_FOLDER, band, 0, 16, 10, 2, 0xFFFF) == 12);
    {
        int n = 0;

        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 320; x++)
                if (band[y * 320 + x]) {
                    n++;
                    CHECK(x >= 10 && x < 22 && y >= 2 && y < 14);
                }
        {
            int want = 0;

            for (int r = 0; r < ICON_H; r++)
                for (unsigned v = icon_table[ICON_FOLDER].rows[r]; v; v &= v - 1)
                    want++;
            CHECK(n == want && want > 30);
        }
    }
    /* clipped by the band: drawn in two bands it equals one */
    {
        static uint16_t a[320 * 20], b[320 * 20];

        memset(a, 0, sizeof a);
        memset(b, 0, sizeof b);
        for (int id = 0; id < ICON_COUNT; id++) {
            icon_draw_band(id, a, 0, 20, id * 7 % 300, 3 + id % 5, (uint16_t)(0x1111 * (id % 7 + 1)));
            icon_draw_band(id, b, 0, 9, id * 7 % 300, 3 + id % 5, (uint16_t)(0x1111 * (id % 7 + 1)));
            icon_draw_band(id, b + 9 * 320, 9, 11, id * 7 % 300, 3 + id % 5, (uint16_t)(0x1111 * (id % 7 + 1)));
        }
        CHECK(!memcmp(a, b, sizeof a));
    }
    /* the right screen edge */
    icon_draw_band(ICON_PLAY, band, 0, 16, 314, 0, 0x1234);
    CHECK(icon_draw_band(ICON_COUNT, band, 0, 16, 0, 0, 0) == 0);
}

static void test_not_loaded(void)
{
    /* before text_fonts_init every font draws with the built-in S8 */
    CHECK(text_font_loaded(&font_s8));
    CHECK(!text_font_loaded(&font_b13));
    CHECK(text_width(&font_b13, "ABC", -1) == text_width(&font_s8, "ABC", -1));
    CHECK(text_line_h(&font_d28) == 9);
    /* a damaged blob is refused whole */
    {
        static uint32_t bad[64];

        memcpy(bad, rechord_fonts_blob, sizeof bad);
        ((uint8_t *)bad)[12] = 99;                       /* nfonts */
        CHECK(text_fonts_init(bad, sizeof bad) < 0);
        CHECK(!text_font_loaded(&font_b13));
    }
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void bench(void)
{
    static uint16_t band[320 * 16];
    const char *s = "Arijit Singh - Tum Hi Ho (Unplugged) \xC2\xB7 King Of Bollywood 4:22";
    const char *h = "\xE0\xA4\xA4\xE0\xA5\x81\xE0\xA4\xAE \xE0\xA4\xB9\xE0\xA5\x80 \xE0\xA4\xB9\xE0\xA5\x8B "
                    "\xE0\xA4\x95\xE0\xA4\xBF\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xAE\xE0\xA4\xA4 "
                    "\xE0\xA4\xB9\xE0\xA4\xBF\xE0\xA4\xA8\xE0\xA5\x8D\xE0\xA4\xA6\xE0\xA5\x81\xE0\xA4\xB8"
                    "\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA4\xBE\xE0\xA4\xA8 \xE0\xA4\xA6\xE0\xA4\xBF\xE0\xA4\xB2";
    int iters = 20000, px = 0;
    double t0, t1, t2, tf;

    for (int i = 0; i < 320 * 16; i++)
        band[i] = 0x1083;
    text_draw_band(&font_b13, band, 0, 16, 2, 13, 0, 320, s, -1, 0xEF7E);
    for (int i = 0; i < 320 * 16; i++)
        px += band[i] != 0x1083;
    /* each pass fills the band first, as the renderer does; the fill alone is timed and taken off */
#define FILL() for (int j = 0; j < 320 * 16; j++) band[j] = 0x1083
    t0 = now();
    for (int i = 0; i < iters; i++) {
        FILL();
        text_draw_band(&font_b13, band, 0, 16, 2, 13, 0, 320, s, -1, 0xEF7E);
    }
    t1 = now();
    for (int i = 0; i < iters; i++) {
        FILL();
        text_draw_band(&font_b13, band, 0, 16, 2, 13, 0, 320, h, -1, 0xEF7E);
    }
    t2 = now();
    for (int i = 0; i < iters; i++) {
        FILL();
        __asm__ volatile("" : : "r"(band) : "memory");
    }
    tf = now() - t2;
    t1 -= tf;
    t2 -= 2 * tf;
    printf("bench: a 320x16 band of B13 Latin (%d px wide, %d px inked): %.2f us; Devanagari line: %.2f us "
           "(host; on the Cortex-M3 about 36,000 and 30,000 instructions, tests/text_m3 in the project repo)\n", text_width(&font_b13, s, -1), px, (t1 - t0) / iters * 1e6, (t2 - t1) / iters * 1e6);
}

int main(int argc, char **argv)
{
    const char *gdir = argc > 1 ? argv[1] : "golden";
    const char *deva = argc > 2 ? argv[2] : "../fonts/deva_hb.txt";
    const char *outdir = argc > 3 ? argv[3] : NULL;

    test_utf8();
    test_not_loaded();
    CHECK(text_fonts_init(rechord_fonts_blob, rechord_fonts_len) == 4);
    CHECK(text_font_loaded(&font_b13) && text_font_loaded(&font_m13) && text_font_loaded(&font_t17) &&
          text_font_loaded(&font_d28));
    CHECK(text_line_h(&font_b13) == 22 && text_base(&font_b13) == 15 && text_line_h(&font_t17) == 20);
    test_goldens(gdir, outdir);
    test_deva(deva);
    test_fit_wrap();
    test_clip_and_edges();
    test_icons();
    bench();
    TEST_END("test_text");
}
