/*
 * bootguard.h -- the player is never left without a way to take new firmware.
 *
 * A fault (HardFault, MemManage, BusFault, UsageFault, an unexpected interrupt) does not
 * hang: its PC and LR go into a crash record in SRAM that survives a reset, and the core
 * resets (SYSRESETREQ). The next boot sees the record and opens the recovery menu before
 * anything else runs (ap_ui.c): install an update from the disk, USB disk to copy one,
 * start normally, power off. After BOOTGUARD_MAX_CRASHES crash resets in a row the player
 * powers off instead of looping; the record is still there at the next power-on. VOL+ held
 * at power-on opens the same menu. A record is cleared once the player has run normally
 * for BOOTGUARD_STABLE_MS.
 */
#ifndef RECHORD_BOOTGUARD_H
#define RECHORD_BOOTGUARD_H

#include <stdint.h>

#define BOOTGUARD_MAX_CRASHES 3u
#define BOOTGUARD_STABLE_MS   60000u

typedef struct {
    uint32_t magic, count, pc, lr, check;
} bootguard_rec_t;

/* The fault handlers' end (frame: the stacked r0-r3, r12, lr, pc, xpsr). Does not return. */
void bootguard_fault(const uint32_t *frame, uint32_t exc);
/* The record from before this boot: 0 = none (then *rec is untouched). */
int bootguard_crashed(bootguard_rec_t *rec);
/* Every main-loop pass: clears the record once the player has run BOOTGUARD_STABLE_MS. */
void bootguard_poll(void);
void bootguard_clear(void);

#endif
