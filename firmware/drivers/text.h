/*
 * text.h -- ReChord's text engine: UTF-8, the baked 2-bit-alpha fonts, Devanagari
 * shaping, measuring, ellipsis, word wrap, and drawing into a row band.
 *
 * Fonts (DESIGN.md 5.2), baked by tools/gen_font.py (project repo) from Noto Sans,
 * Noto Sans Devanagari and Noto Sans Math (SIL OFL 1.1: drivers/fonts/OFL.txt):
 *   font_s8   the 5x7 pixel font, proportional (line 9)   key bar, badges, numbers
 *   font_b13  Noto Sans Regular 13 px (line 22)           list text, values, body
 *   font_m13  Noto Sans SemiBold 13 px (line 22)          headers, prompts
 *   font_t17  Noto Sans SemiBold 17 px (line 20)          Now Playing title
 *   font_d28  Noto Sans SemiBold 28 px (line 32)          big values: digits, A-Z, "dB"
 * B13/M13/T17 hold ASCII, Latin-1, Latin Extended-A, general punctuation, arrows and a
 * few signs, and Devanagari (U+0900..097F plus the half forms, conjuncts and matra
 * variants HarfBuzz produces for Hindi). S8 is compiled in (fonts/fonts_s8.c); the others
 * come in one blob (fonts/rechord_fonts.bin): the firmware loads it from its slot at boot
 * (text_fonts_load), the host tests link fonts/fonts_blob.c. A font that is not loaded
 * draws with S8.
 *
 * Text: UTF-8; an invalid byte is one U+FFFD. A code point the font lacks draws as a dim
 * box (never '?'); zero-width controls (ZWJ, ZWNJ, U+200B, soft hyphen, BOM) draw nothing;
 * a Latin letter + combining accent is drawn precomposed when Latin-1/Ext-A has it.
 * No kerning: a glyph's advance is its own.
 *
 * Devanagari: syllables are shaped with tables precomputed from HarfBuzz (no OpenType
 * engine here): nukta forms, half forms, conjunct ligatures, rakar, reph, the pre-base
 * i-matra (moved before its cluster, its loop length chosen by the cluster's stem),
 * matra/anusvara ligatures and mark offsets. Known limits: i-matra + reph and
 * i-matra + anusvara ligatures are drawn as two glyphs; a few clusters of 3+ consonants
 * differ from HarfBuzz (gen_font.py prints the agreement on its corpus).
 *
 * Geometry: x and baseline are panel coordinates; the baseline is the row the letters
 * sit on. text_line_h()/text_base() give a font's line box: a line whose top is at y
 * has its baseline at y + text_base(f).
 */
#ifndef RECHORD_TEXT_H
#define RECHORD_TEXT_H

#include <stdint.h>

typedef struct font font_t;
struct font { uint8_t id; };          /* a handle: the data lives in the loaded tables */

extern const font_t font_s8, font_b13, font_m13, font_t17, font_d28;

#define TEXT_BAND_W 320                /* a band row is 320 RGB565 pixels */

/*
 * Draw UTF-8 text (nbytes < 0: up to the NUL) with its pen starting at x and its
 * baseline at `baseline`, into the band: band holds panel rows band_y .. band_y+band_h-1,
 * 320 pixels each (row 0 = panel row band_y). Only columns clip_x0 <= x < clip_x1
 * (exclusive end; clamped to 0..320) are written. Pixels are alpha-blended over what
 * the band already holds (2-bit alpha: 0, 1/3, 2/3, 1). Returns the advance in px
 * (the same as text_width), whether or not anything fell inside the band.
 */
int text_draw_band(const font_t *f, uint16_t *band, int band_y, int band_h,
                   int x, int baseline, int clip_x0, int clip_x1,
                   const char *utf8, int nbytes, uint16_t fg);

/* Width in px of the text as drawn (the sum of the advances). */
int text_width(const font_t *f, const char *utf8, int nbytes);

/*
 * The text cut to max_w px: if it fits it is copied whole; otherwise it is cut at a
 * cluster boundary (never inside a Devanagari syllable), trailing spaces dropped, and
 * "…" (U+2026) appended, so that text_width(out) <= max_w. When not even the ellipsis
 * fits, out is "". out is always NUL-terminated (out_cap >= 1). Returns the bytes
 * written (without the NUL).
 */
int text_fit(const font_t *f, const char *utf8, int nbytes, int max_w, char *out, int out_cap);

/*
 * Word wrap into at most max_lines lines of max_w px: breaks after spaces (and after
 * '-', '/', '–', '—' when a word must give way), a word longer than a line is broken
 * at a cluster boundary. Spaces at a break are dropped. The last line allowed holds
 * the rest of the text and may be wider than max_w (text_fit it). Returns the number
 * of lines (0 for empty text).
 */
typedef struct { uint16_t off, len; int16_t w; } text_line_t;
int text_wrap(const font_t *f, const char *utf8, int nbytes, int max_w, int max_lines,
              text_line_t *lines);

/* The font's line box and ink extents (px). */
int text_line_h(const font_t *f);      /* line pitch */
int text_base(const font_t *f);        /* baseline from the top of the line box */
int text_ink_up(const font_t *f);      /* most ink above the baseline */
int text_ink_down(const font_t *f);    /* most ink below the baseline */

/*
 * Shaped glyphs, for tests and tools: up to max of {code, x, y}, where code is the
 * font's glyph code (a code point, or U+E000.. for a glyph without one, 0xFFFD for the
 * box) and x/y the glyph origin relative to the pen start and the baseline. Returns the
 * number of glyphs.
 */
typedef struct { uint16_t code; int16_t x; int8_t y; uint8_t adv; } text_glyph_t;
int text_shape(const font_t *f, const char *utf8, int nbytes, text_glyph_t *out, int max);

/* Next code point of UTF-8 in [*p, end): advances *p; malformed bytes give U+FFFD. */
uint32_t text_utf8_next(const char **p, const char *end);

/*
 * Fonts: text_fonts_init() takes a blob of the RCFONT2 format (tools/gen_font.py) that
 * stays valid while in use; fonts named in it replace the built-in ones. Returns the
 * number of fonts taken, < 0 if the blob is malformed (nothing taken).
 * text_fonts_load() (target only, text_load.c) loads the "fonts" module from the
 * firmware slot to the fonts window (firmware_ap.ld) and calls text_fonts_init.
 * text_font_loaded(f): 1 when f has its own data, 0 when it falls back to S8.
 */
int text_fonts_init(const void *blob, uint32_t len);
int text_fonts_load(void);
int text_font_loaded(const font_t *f);

#endif /* RECHORD_TEXT_H */
