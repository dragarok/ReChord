/*
 * bootcrumb.h -- a boot trail on the eMMC from the first line of Main (bootcrumb.c).
 *
 * Each bootcrumb(step) persists "this step started" before the step runs, so a boot that stops
 * dead leaves the step it stopped in. A fault is recorded too. Decoded by tools/bootlog.py in the
 * snowsky_echo_mini repo (rockusb `rl 0x10EA0 0x50`).
 */
#ifndef RECHORD_BOOTCRUMB_H
#define RECHORD_BOOTCRUMB_H

#include <stdint.h>

enum {
    BC_MAIN = 1, BC_BOARD, BC_CLOCK, BC_LOG, BC_DISPLAY, BC_SPLASH, BC_UI, BC_EMMC, BC_SLOTS,
    BC_RECOVERY, BC_UPDATE, BC_PLAYER, BC_CONFIG, BC_POWER, BC_UI_INIT, BC_LOOP,
    /* after boot: sessions that take over the main loop (back to BC_LOOP when they end) */
    BC_DAC = 20, BC_DAC_AUDIO, BC_DAC_VOLUME, BC_DAC_USB, BC_DAC_WAIT,
    BC_DAC_STEP = 40,       /* + drivers/dac_cs43131.c DAC_STEP_*: inside dac_init (a reset there
                             * names the step as the next boot's "previous step") */
    BC_USBDET = 60,         /* + the USB detect test's state under test */
    BC_DISPLAY_TRIAL = 80,  /* the panel moved to the VOP (drivers/panel_io.h): its first 5 s */
};

/* event keys (< 0x1000; others are register addresses) */
enum { BCK_CLOCK_RC = 0x200, BCK_CORE_HZ, BCK_EMMC_RC, BCK_EMMC_SECTORS, BCK_EMMC_WIDTH,
       BCK_CLOCK_HANGS, BCK_PREV_STEP, BCK_CLOCK_POINT, BCK_PREV_STAGE,
       BCK_PREV_FAULT_PC = 0x220, BCK_PREV_FAULT_LR, BCK_PREV_FAULT_EXC,   /* the last boot's last fault */
       BCK_ADC_DIAG = 0x210, /* .. + ADC_DIAG_N (drivers/panel_io.h) */
       /* 0x230..0x23F: kept with bootcrumb_set() and carried into the next boot as + 0x80 */
       BCK_LOG_FLUSH = 0x230,  /* the last slow or failed log flush: ms | rc << 24 */
       BCK_PLAY,               /* the last play: rc & 0xFFFF | plays << 16 */
       BCK_PLAY_FAIL,          /* the last failed play: audio_play's code & 0xFFFF | DAC step << 16 */
       BCK_SLOW_PASS,          /* the slowest main-loop pass: ms | screen index << 24 */
       BCK_USB_HWCFG3,         /* GHWCFG3 (FIFO depth in the high half) */
       BCK_USB_HWCFG4,
       BCK_USB_SNPSID,
       BCK_USBDET_RESULT,      /* the USB detect test: per state, bit 2n plug seen, 2n+1 unplug seen */
       BCK_PREV = 0x80 };

void bootcrumb_init(void);
void bootcrumb(uint32_t step);
void bootcrumb_kv(uint32_t key, uint32_t val);
/* The key's value replaced (or added), and on the eMMC at once. */
void bootcrumb_set(uint32_t key, uint32_t val);
void bootcrumb_fault(uint32_t pc, uint32_t lr, uint32_t exc);
/* The step this boot is in, and the one the previous boot was in when it ended (0 = unknown). */
uint32_t bootcrumb_step(void);
uint32_t bootcrumb_prev_step(void);
/* Boots of this build that stopped in clock_init (Main then keeps the ROM's clocks). */
uint32_t bootcrumb_clock_hangs(void);
/* clock_init_at()'s trace: the stage inside the clock sequence, persisted (sector 1 word 4). */
void bootcrumb_clock_stage(uint32_t stage);
/* The trail works this boot (its sectors found and written): only then may the CPU's FLAC point be
 * tried (a boot that dies in its switch must leave a mark for the next one). */
int bootcrumb_ok(void);
/* The CPU point record (services/cpuclk.c), sector 1 words 6 and 7, carried across every boot of
 * this build (a new build starts from 0): w 0 = status, w 1 = the stage armed (0 = none in flight).
 * _set: both words on the eMMC at once; 0, or -1 (not written). */
uint32_t bootcrumb_hp(unsigned w);
int bootcrumb_hp_set(uint32_t status, uint32_t armed);

#endif
