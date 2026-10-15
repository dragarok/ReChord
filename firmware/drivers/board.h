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

/* What the player draws from USB power (512 MB board, non-0x66666666 board only;
 * stock MINIV390's "UAC charging" setting, USB handler seg09 0x0307CFAE):
 *   NORMAL:  GPIO0.14 = 1 (charge enable), GPIO2.1 = !strap (the boot state);
 *   BATTERY: GPIO0.14 = 0, GPIO2.1 = 0: no charging and no supply path from VBUS --
 *            the player runs on its battery (stock's default "Turn off charging
 *            power supply" for its USB DAC);
 *   VBUS:    GPIO0.14 = 0, GPIO2.1 = 1: the player runs from VBUS, not charging (stock's
 *            "Power only, no charging", which it uses below 4000 mV of battery).
 * 0, or -1 where the board cannot (the 0x66666666 board, GPIO2.9 strap low: stock
 * keeps both pins high there; the 8 GB board). */
enum { BOARD_SUPPLY_NORMAL = 0, BOARD_SUPPLY_BATTERY = 1, BOARD_SUPPLY_VBUS = 2 };
int board_usb_supply(int mode);

/* Reboot into the boot ROM's USB loader (rockusb, 2207:262d): the way back that needs none of our
 * drivers -- the ROM drives the storage itself, and a computer reinstalls with rkdeveloptool
 * (wl 0 MINIV390.IMG). The SDK's SysReboot(…, 0x02) with stock MINIV390's values (seg09 literal pool
 * 0x0307D620..0x0307D650): clocks to the crystal, every clock gate open, soft resets released,
 * pins back to GPIO inputs (that drops the power hold: on battery the player switches off, so
 * only call this with USB power in), ROM remapped to 0, VTOR 0, PMU_SYS_REG3 = 0x18AF, reset. */
void board_reboot_rockusb(void) __attribute__((noreturn));

/* With USB power in: 1 = the battery is charged, 0 = charging (GPIO2.3). */
int board_charge_done(void);

#endif
