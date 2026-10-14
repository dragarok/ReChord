/*
 * gfx.h -- drawing into the display's strip buffer, with the stock bitmap fonts.
 *
 * There is no framebuffer: display_flush_rows() renders the screen 8 rows at a
 * time, calling the screen's render function once per strip (display.h). The
 * render function draws the whole screen with these calls; each one clips to
 * the current strip, so what lies outside it costs a comparison.
 *
 * Fonts: the stock firmware's two bitmap fonts, read from our firmware slot on
 * the eMMC (tools/make_fw1.py packs them), through a 256-glyph cache. SMALL is
 * 12 px high (U+0000..07FF), LARGE 16 px (the whole BMP: CJK included). Without
 * them (an image built without --fonts) both fall back to the built-in 8x8 font.
 */
#ifndef RECHORD_GFX_H
#define RECHORD_GFX_H

#include <stdint.h>

enum { GFX_SMALL = 0, GFX_LARGE = 1 };

typedef int (*gfx_read_t)(uint32_t lba, uint32_t count, void *buf);

/* slot_lba: first sector of the running firmware image. 1 = stock fonts found. */
int gfx_fonts_init(gfx_read_t rd, uint32_t slot_lba);
int gfx_fonts_ok(void);

/* The strip being rendered: buf holds rows [y0, y0 + n) of the screen. */
void gfx_target(uint16_t *buf, int y0, int n);
int gfx_strip_y0(void);
int gfx_strip_n(void);

void gfx_fill(int x0, int y0, int x1, int y1, uint16_t c);          /* inclusive */
int gfx_font_h(int font);
/* UTF-8 text, top-left at (x, y), transparent background, clipped at xmax
 * (exclusive). Returns the x after the last glyph drawn. */
int gfx_text(int x, int y, int font, const char *s, uint16_t fg, int xmax);
int gfx_text_w(int font, const char *s);
/* Text in [x, xmax): left (align 0), right (1) or centred (2); when it does not
 * fit it is cut and ends in "..." */
void gfx_text_box(int x, int y, int xmax, int font, const char *s, uint16_t fg, int align);
/* 16x16 one-bit icon (16 u16 rows, MSB = left). */
void gfx_icon(int x, int y, const uint16_t *rows, uint16_t c);

/* Decoded glyph, for the screenshot/test tools: rows MSB = left, h rows. */
typedef struct { uint16_t rows[16]; uint8_t h, adv; } gfx_glyph_t;
void gfx_glyph(int font, uint32_t cp, gfx_glyph_t *g);

/* Counters for the debug pages. */
extern uint32_t gfx_glyph_misses, gfx_glyph_reads;

#endif
