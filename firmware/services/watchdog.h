/*
 * watchdog.h -- the SoC's watchdog, as stock arms it (watchdog.c).
 *
 * Started once the boot is done; fed by whatever proves the firmware is still moving (the key
 * scan, storage block I/O, the USB disk loop); stopped before any power-off or reboot. A hang
 * that feeds nothing for one period raises the watchdog interrupt, which records the hung code's
 * address in the boot trail; the second period resets the chip.
 */
#ifndef RECHORD_WATCHDOG_H
#define RECHORD_WATCHDOG_H

void watchdog_start(void);
void watchdog_feed(void);
void watchdog_stop(void);
int watchdog_running(void);   /* 1 = armed (a hang ends in a reset) */

#endif
