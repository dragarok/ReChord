/*
 * text_load.c -- the fonts module into its RAM window at boot (target only).
 *
 * tools/make_fw1.py stores drivers/fonts/rechord_fonts.bin in the image's module table
 * as "fonts" (Makefile cfw-img). It is read from the firmware slot on the eMMC into the
 * fonts window [__fonts_ram_start, __fonts_ram_end) of firmware_ap.ld: core-A SRAM
 * between the most the code image may grow to (the end of FW_CODE) and the audio LLI
 * below the PCM ring. Nothing else lives there once the loader has run.
 */
#include "text.h"
#include "services/modules.h"
#include "services/log.h"

extern uint8_t __fonts_ram_start[], __fonts_ram_end[];

int text_fonts_load(void)
{
    uint32_t room = (uint32_t)(__fonts_ram_end - __fonts_ram_start) & ~511u;   /* whole sectors */
    int n = module_load("fonts", __fonts_ram_start, room), k;

    if (n < 0) {
        log_printf("text: no fonts module (%d): S8 only", n);
        return n;
    }
    k = text_fonts_init(__fonts_ram_start, (uint32_t)n);
    log_printf("text: %d fonts, %d B", k, n);
    return k;
}
