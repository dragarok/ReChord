/*
 * board.h -- board variant support (Echo Mini 8 GB vs 512 MB).
 *
 * The variant is chosen at build time (make BOARD=512m | 8g; echo512 builds
 * 512m by default). The two boards differ in a few pins and in the stock
 * update-image names; everything else (panel, keys on ADC ch1, power key,
 * eMMC layout parsing) is shared. board_check() guards against running a
 * build on the other board: it compares the eMMC size with the variant.
 */
#ifndef RECHORD_BOARD_H
#define RECHORD_BOARD_H

#include <stdint.h>

#define BOARD_8G   1
#define BOARD_512M 2

#ifndef BOARD
#define BOARD BOARD_512M
#endif

/* Pin setup the stock firmware of this board does first thing at reset.
 * Call before any other driver touches the GPIOs. */
void board_early_init(void);

/* 1 if the eMMC size matches the built variant (512 MB board: < 4 GiB). */
int board_check(uint32_t emmc_sectors);

/* "512M" / "8G" */
const char *board_name(void);

/* 512 MB board: SAR-ADC channel 2 (second key ladder, purpose unconfirmed --
 * stock's 512M build scans it, RECHO only logs it). Last conversion, 1023 =
 * idle; -1 on the 8 GB board. */
int board_adc_ch2(void);

#endif
