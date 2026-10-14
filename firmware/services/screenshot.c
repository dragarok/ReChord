/*
 * screenshot.c -- the screen as a BMP file (see screenshot.h).
 *
 * Layout: BITMAPINFOHEADER with BI_BITFIELDS (565 masks) and a negative
 * height (rows top-down, as the display renders them), pixel data at 512
 * so every 8-row strip (5120 bytes = 10 sectors) lands sector-aligned and
 * goes from the display's strip buffer straight to the eMMC. 109312 bytes.
 * RAM: the 512-byte header on the stack, nothing static.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "drivers/display.h"
#include "services/fs.h"
#include "services/screenshot.h"

#define PIX_OFF   512u
#define ROW_BYTES (DISPLAY_W * 2u)

static void put16(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); }
static void put32(uint8_t *p, uint32_t x) { put16(p, x); put16(p + 2, x >> 16); }

int screenshot_save(void)
{
    uint32_t hdr[PIX_OFF / 4u];
    uint8_t *h = (uint8_t *)hdr;
    char path[24];
    fs_file_t *f;
    int n, r = 0;

    for (n = 1; n <= 999; n++) {
        snprintf(path, sizeof path, "/Internal/SHOT%d.BMP", n);
        if (!fs_exists(path))
            break;
    }
    if (n > 999 || !(f = fs_open_write(path)))
        return FS_ERR_OPEN;
    memset(hdr, 0, sizeof hdr);
    h[0] = 'B';
    h[1] = 'M';
    put32(h + 2, PIX_OFF + ROW_BYTES * DISPLAY_H);      /* file size */
    put32(h + 10, PIX_OFF);                             /* pixel data */
    put32(h + 14, 40);                                  /* BITMAPINFOHEADER */
    put32(h + 18, DISPLAY_W);
    put32(h + 22, (uint32_t)-DISPLAY_H);                /* top-down */
    put16(h + 26, 1);                                   /* planes */
    put16(h + 28, 16);                                  /* bpp */
    put32(h + 30, 3);                                   /* BI_BITFIELDS */
    put32(h + 34, ROW_BYTES * DISPLAY_H);
    put32(h + 38, 2835);                                /* 72 dpi */
    put32(h + 42, 2835);
    put32(h + 54, 0xF800);                              /* R, G, B masks */
    put32(h + 58, 0x07E0);
    put32(h + 62, 0x001F);
    if (fs_write(f, hdr, PIX_OFF) != (long)PIX_OFF)
        r = FS_ERR_IO;
    for (int y = 0; !r && y < DISPLAY_H; y += DISPLAY_STRIP_ROWS) {
        int rows = DISPLAY_H - y < DISPLAY_STRIP_ROWS ? DISPLAY_H - y : DISPLAY_STRIP_ROWS;
        long len = (long)rows * (long)ROW_BYTES;

        if (fs_write(f, display_render_rows(y, rows), (unsigned long)len) != len)
            r = FS_ERR_IO;
    }
    if (fs_close(f) && !r)
        r = FS_ERR_IO;
    if (r)
        (void)fs_remove(path);                          /* no half pictures */
    return r ? r : n;
}
