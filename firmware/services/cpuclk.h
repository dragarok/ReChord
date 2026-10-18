/*
 * cpuclk.h -- core B's clock while playing: which CPU point a track gets, the switch at its open,
 * the self-test that admits the FLAC point, and the record that keeps it off after a failure
 * (cpuclk.c; drivers/clock.h has the points; docs/CPU_CLOCK.md the evidence and the device plan).
 *
 * RECHORD.CFG cpu_b=:
 *   96    (default) never leaves the UI point: core B at most 96 MHz, as every build before.
 *   auto  the FLAC point (A 224, B 448 MHz) for a track whose format is not comfortably real-time at
 *         96 MHz on the device: its QEMU instruction count (the worst measured file of the format)
 *         times the device's measured ~2.6 cycles an instruction, above 60 % of 96 MHz.
 *   stock the FLAC point for every track (stock never plays at 96 MHz: FINDINGS.md).
 * The FLAC point is used only after this boot's self-test passed at its first use, only with the
 * boot trail working (bootcrumb_ok) and the watchdog armed, never for USB-C earphones, and never
 * again in this build once a self-test failed or a boot died inside one.
 */
#ifndef RECHORD_CPUCLK_H
#define RECHORD_CPUCLK_H

#include <stdint.h>

#include "audio/audio_shared.h"

/* The device's core-B cycles per QEMU instruction, x100 (measured 2.5-2.7 on 2026-10-18: APE extra
 * high at 284 M cycles/s for 105 M QEMU instructions/s; the task brief, FINDINGS.md). */
#define CPUCLK_DEVICE_CPI_X100 260u

/* The point for a track about to be queued (CLOCK_POINT_*): cpu_b, the estimate, the record.
 * extra: APE compression level (1000..5000), DSD rate multiple (1, 2, 4); 0 otherwise. */
unsigned cpuclk_point_for(const as_track_t *t, uint32_t extra, int usb_out);
/* Main loop, before the first FLAC track of the boot is queued: the record marked "in flight" on
 * the eMMC (a boot that dies in the switch or the self-test leaves it). 0 = the point may be used. */
int cpuclk_arm(void);
/* Core B holds at a track open asking for want: 1 = switch now, 0 = no (stay), -1 = not yet. */
int cpuclk_may(unsigned want);
/* The switch to want, core B quiet (held, b_held = 1, or in reset). The first FLAC switch of a boot
 * runs the self-test (both cores; B must be held) before it returns. ST_OK, or the stage it failed
 * at (selftest_kernel.h): the point is then back at UI and stays there for the build. */
int cpuclk_switch(unsigned want, int b_held);
/* A switch that went wrong outside the self-test (core B did not come back from a hold). */
void cpuclk_fail(unsigned stage);
/* Main loop: the lines the switches left (logging is not for interrupt context), the record's
 * writes, pclk's users re-timed (the backlight's PWM, the watchdog). */
void cpuclk_poll(void);
/* After config_load(): the record of the earlier boots of this build, logged and judged. */
void cpuclk_boot(void);
/* For the B image loader (audio.c b_load): the image core A just wrote at 0x01000000, n bytes. */
void cpuclk_b_loaded(uint32_t n);
/* For the self-test's code check: 1 = the FLAC point has been admitted this boot. */
int cpuclk_admitted(void);

#endif
