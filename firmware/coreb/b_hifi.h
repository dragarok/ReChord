/*
 * b_hifi.h -- core B's HIFI accelerator (0x01060000) for FLAC's LPC restore (b_hifi.c; every
 * register value and its source in docs/HIFI.md).
 *
 * The block is used as stock's FLAC decoder uses it (orders 3..32, blocks up to 4608, fed by DMA2)
 * and only after it reproduced the decoder's own software on test vectors. Two classes: narrow (the
 * sum of |coefficient| times the largest sample stays below 2^31, so a 32-bit accumulator would be
 * exact) and wide (needs the 64-bit sum stock's 24-bit files rely on); each is tested and dropped on
 * its own, since the silicon's accumulator width is not measured.
 * RECHORD.CFG hifi=auto|off|verify (audio_shared.h AS_HIFI_*) comes from core A.
 */
#ifndef RECHORD_B_HIFI_H
#define RECHORD_B_HIFI_H

#include <stdint.h>

#define B_HIFI_MAX_COUNT 4608u      /* stock's FLAC_MAX_BLOCKSIZE: the longest job it ever runs */

/* At a FLAC track's open: power the block and run the self-test, once a boot. 1 = usable. */
int b_hifi_open(void);
/* 1 = this subframe may go to the block (order 3..32, n <= B_HIFI_MAX_COUNT, the block usable). */
int b_hifi_wants(uint32_t n, uint32_t order);
/* The subframe's class (1 = wide), and whether that class may go to the block. */
int b_hifi_wide(uint32_t bits, uint32_t order, const int32_t *coef);
int b_hifi_class_ok(int wide);
/* One LPC restore: in[] = the `order` warm-up samples, then the residuals; coef[0] multiplies
 * x[n-1] (dr_flac's order). out[] gets the samples. 0 = done; < 0 = timed out (out[] undefined,
 * the block is off until power-off; in[] is untouched). */
int b_hifi_flac(const int32_t *in, int32_t *out, uint32_t n, uint32_t order, uint32_t shift,
                const int32_t *coef);
/* A result that disagreed with the software (sample i of a job): logged once; in auto mode the
 * class is off for the rest of the boot. */
void b_hifi_mismatch(uint32_t i, int32_t hw, int32_t sw, uint32_t order, uint32_t shift, int wide);
uint32_t b_hifi_mode(void);         /* AS_HIFI_* in effect: OFF once the block is unusable */

/* Core-B cycles from SysTick (wraps; for differences within one decoder call), and in us. */
uint32_t b_cycles(void);
uint32_t b_us(uint32_t cycles);
/* A line for core A's log (%u %d %x %s, %0Nu %0Nx), dropped when core A has 4 unread. */
void b_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Shared with the other functions' code (b_hifi_fn.c): the block's power-up (clocks on, 1 ms reset;
 * b_hifi_up adds FLAC's set-up), its power-down (channels stopped, reset, gated; b_hifi_down also sets
 * hifi_state), DMA2's two transfers of one job, and the system address DMA2 takes. */
void b_hifi_power(void);
int b_hifi_is_up(void);                     /* powered by this core B, not gated since */
void b_hifi_up(void);
void b_hifi_gate(void);
void b_hifi_down(uint32_t state);
void b_hifi_dma(const int32_t *in, int32_t *out, uint32_t n);
uint32_t b_hifi_sysaddr(const void *p);

/* Counters for the track's summary line, cleared at each FLAC track's open (b_flac.c). */
typedef struct {
    uint32_t jobs, samples, wait_cyc, wait_max_cyc, mismatch_jobs, sw_subframes;
    uint32_t polls;                 /* turns of the wait loop (QEMU: its instructions are not the work) */
} b_hifi_stats_t;
extern b_hifi_stats_t b_hifi_stats;

#endif
