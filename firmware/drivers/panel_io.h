/*
 * panel_io.h -- ReChord display transport abstraction (clean-room).
 *
 * The display driver speaks to the panel only through these calls,
 * so the same driver code runs:
 *   - on target:   panel_io_rknanoc.c (RKNanoC VOP MCU register interface),
 *   - on the host: panel_io_sim.c (simulated DCS panel, writes PPM files),
 * making every display/font behavior unit-testable with `cc`.
 *
 * Wire contract (MIPI-DCS style, byte oriented):
 *   - commands are one byte (0x2A CASET, 0x2B RASET, 0x2C RAMWR, ...)
 *   - data bytes belong to the most recent command
 *   - an RGB565 pixel is two bytes on the wire, HIGH byte first
 */

#ifndef RECHORD_PANEL_IO_H
#define RECHORD_PANEL_IO_H

#include <stdint.h>

/* Bring the transport up (clocks/pins on target, state reset in sim). */
void panel_io_init(void);

/* Send one DCS command byte; the following data bytes belong to it. */
void panel_io_send_cmd(uint8_t cmd);

/* Send one parameter/data byte to the current command. */
void panel_io_send_data(uint8_t data);

/* Send one RGB565 pixel (two data bytes, high byte first) -- the fast
 * path used after RAMWR (0x2C). */
void panel_io_send_pixel(uint16_t rgb565);

/* A block of pixels into a panel window (the flush path): CASET x0..x1, RASET y0..y1 (PHYSICAL
 * panel rows), RAMWR, then (x1-x0+1) x (y1-y0+1) pixels from src, rows 'pitch' pixels apart. The
 * transport may still be reading src when this returns (DMA): src must stay unchanged until the
 * next panel_io call (every call waits for the block before it) or panel_io_sync(). */
void panel_io_blit(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch);
/* Wait until every block is on the panel. */
void panel_io_sync(void);
/* The path pixels take now ("pio" = GPIO bit-bang, "vop" = VOP MCU, CPU-fed, "dma" = VOP + DMA). */
const char *panel_io_path(void);

/*
 * The pixel path (config key display=auto|pio|gpio|vop|dma; auto = dma; gpio = pio). The boot
 * always runs on the GPIO bus (proven on the device); panel_io_select() moves to the VOP afterwards,
 * only with the watchdog running, as a TRIAL guarded the way RECHO 0.10 guards it: a marker in SRAM
 * that a warm reset keeps (and the boot trail's step on the eMMC), cleared once the new path has
 * pushed frames for 5 s (panel_io_poll(), every main-loop pass). A boot that finds the marker of an
 * unfinished trial (a hang, then the watchdog's reset) refuses the VOP until power-off. A band that
 * does not finish (DMA: 50 ms) falls back to the CPU-fed VOP; a VOP that stays busy falls back to
 * GPIO, latched. Returns the path now in use.
 */
enum { PANEL_PATH_PIO = 0, PANEL_PATH_VOP, PANEL_PATH_DMA, PANEL_PATH_PIO2, PANEL_PATH_AUTO };   /* (PIO2 = PIO) */
int panel_io_select(int path);
void panel_io_poll(void);
/* Counters: [0] DMA bands, [1] DMA band timeouts, [2] VOP busy timeouts, [3] us of the biggest band
 * waited for to its end (the bus's own time: logged with its bytes when the trial passes). */
const uint32_t *panel_io_counters(void);
/* Time for the frame accounting: microseconds (for differences; wraps) and time_ms(). */
uint32_t panel_io_now_us(void);
uint32_t panel_io_now_ms(void);

/* Millisecond delay hook for init sequences (power-on delays).
 * The sim backend counts but never sleeps. */
void panel_io_delay_ms(uint16_t ms);

void backlight_on(void);
/* 0 = off, 100 = full on, 1..99 = RECHO's PWM duty (on for 100 - duty %). */
void backlight_set(int duty);

/* Keys: last ADC ladder reading 0..1023 (call once per main-loop pass: it also
 * moves the ADC scan on) and the power key level (1 = pressed). Feed both to
 * input_scan(). */
int keys_adc(void);
/* From core A's SysTick (10 ms): the converter and the key samples (services/input.c). Off until
 * keys_isr_start(); then keys_adc() only returns the latest stable reading. */
void keys_tick(void);
void keys_isr_start(void);
/* The last conversion of a SAR-ADC channel (0 battery, 1 keys, 2, 3), -1 = none yet. */
int adc_value(unsigned ch);
void adc_init(void);            /* stock's AdcInit, write for write */
/* What the converter did since boot (keys_adc): completions, timeouts, CTRL bit 6 seen, and the
 * first completion's STATUS (bit 31 set once taken) / CTRL / DATA / microseconds after its start. */
enum { ADC_DIAG_DONE, ADC_DIAG_TIMEOUTS, ADC_DIAG_BIT6, ADC_DIAG_FIRST_STAS, ADC_DIAG_FIRST_CTRL,
       ADC_DIAG_FIRST_DATA, ADC_DIAG_FIRST_US, ADC_DIAG_LAST_CTRL, ADC_DIAG_LAST_STAS, ADC_DIAG_N };
const uint32_t *adc_diag(void);
int keys_power(void);
/* The last release: ms from its edge to the first idle reading (bits 0-15) and, since boot, presses
 * that read idle and went back down within 30 ms (bits 16-31). */
uint32_t keys_release_stats(void);
/* Developer > Panel TE (test): which GPIO inputs change with the panel's TE output on and off (log). */
void panel_te_probe(void);
void board_power_off(void);   /* does not return */
/* A GPIO0 pin that is not the panel's, set through the panel bus's shadow of the
 * port (every panel byte rewrites the whole GPIO0 data register from it). */
void panel_io_gpio0_pin(unsigned pin, int level);

#endif /* RECHORD_PANEL_IO_H */
