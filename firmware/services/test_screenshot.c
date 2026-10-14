/* Host harness for screenshot.c: a disk image as the writable "Internal"
 * volume, the display driver over the panel simulator, a known screen, two
 * screenshots. Driven by tools/test_fs_write.py (which decodes the BMPs). */
#include <stdint.h>
#include <stdio.h>

#include "drivers/display.h"
#include "services/fs.h"
#include "services/fs_target.h"
#include "services/screenshot.h"

static FILE *s_img;
static unsigned long s_wcalls;

static int img_read(uint32_t lba, uint32_t count, void *buf)
{
    return ((uintptr_t)buf & 3u) || fseek(s_img, (long)lba * 512L, SEEK_SET) ||
           fread(buf, 512, count, s_img) != count;
}

static int img_write(uint32_t lba, uint32_t count, const void *buf)
{
    s_wcalls++;
    return ((uintptr_t)buf & 3u) || fseek(s_img, (long)lba * 512L, SEEK_SET) ||
           fwrite(buf, 512, count, s_img) != count;
}

int main(int argc, char **argv)
{
    int a, b;
    unsigned long w;

    if (argc != 2 || !(s_img = fopen(argv[1], "r+b")))
        return 2;
    if (fs_target_mount_rw(0, "Internal", img_read, img_write, 0))
        return 3;
    display_init();
    display_clear(0x0000);
    display_fill_rect(0, 0, 9, 9, 0xF800);                 /* red, top left */
    display_fill_rect(DISPLAY_W - 10, DISPLAY_H - 10, DISPLAY_W - 1, DISPLAY_H - 1, 0x07E0);
    display_fill_rect(100, 80, 199, 89, 0x001F);           /* blue bar */
    display_draw_text(8, 40, "ReChord", 0xFFFF, 0x0000);
    display_flush_rows(0, DISPLAY_H - 1);
    w = s_wcalls;
    a = screenshot_save();
    w = s_wcalls - w;
    b = screenshot_save();
    printf("shots %d %d writes %lu\n", a, b, w);
    fclose(s_img);
    return 0;
}
