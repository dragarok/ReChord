/*
 * screenshot.h -- save what the panel shows as /Internal/SHOTn.BMP.
 */
#ifndef RECHORD_SCREENSHOT_H
#define RECHORD_SCREENSHOT_H

/* Writes the screen (DISPLAY_W x DISPLAY_H, 16-bit RGB565 BMP, top-down)
 * as /Internal/SHOTn.BMP with the first free n in 1..999. Rendered strip by
 * strip from the display list (no framebuffer). Returns n, or a negative
 * FS_ERR_* (FS_ERR_OPEN: no free name / volume not writable). */
int screenshot_save(void);

#endif
