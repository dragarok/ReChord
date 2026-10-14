/*
 * clock.h -- CPU clocks and the time base (RKNanoD, both cores).
 *
 * Clock tree (SDK Hw_cru.h + PowerManager.c, sequence copied from RECHO 0.9.2
 * as traced in QEMU, docs/PARITY_PLAN.md M2):
 *   APLL 400 MHz = 24 MHz * 100 / 6        (CON0 0x1064, CON1 0x9046)
 *   core A (sys_core) = APLL / (SEL0[2:0] + 1)   default /2 = 200 MHz
 *   core B (cal_core) = APLL / (SEL0[14:12] + 1)
 *   pclk_logic = core A / 2^SEL1[9:8], kept <= 50 MHz (I2C, UART, ADC bus)
 *   eMMC/SD card clocks run from the 24 MHz oscillator, never the PLL.
 * The PLL is set once and never changed afterwards: run-time scaling only
 * moves the core dividers, so I2S/MCLK sources stay stable.
 *
 * Time base: SysTick on the core-A clock at 100 Hz (stock's SysTickPeriodSet
 * (10)); time_us() interpolates inside the tick. Delays are time-based, so
 * they hold at any clock. cpu_idle() sleeps in WFI and accounts idle time for
 * cpu_load_permille().
 */
#ifndef RECHORD_CLOCK_H
#define RECHORD_CLOCK_H

#include <stdint.h>

#define CLOCK_TICK_HZ 100u

/* Bring up the PLL (400 MHz) with core A at 200 MHz, then start the time
 * base. Returns 0, or -1 if the PLL did not lock (cores stay on 24 MHz). */
int clock_init(void);

/* Core A divider of the PLL (2 = 200 MHz .. 8 = 50 MHz); reprograms the APB
 * divider and SysTick. Call from thread context only. */
void clock_set_core_a_div(unsigned div);
/* Core B divider of the PLL (1 = 400 MHz .. 8 = 50 MHz). */
void clock_set_core_b_div(unsigned div);

uint32_t clock_pll_hz(void);
uint32_t clock_core_hz(void);       /* core A */
uint32_t clock_core_b_hz(void);
uint32_t clock_pclk_hz(void);       /* APB (pclk_logic) */

/* Monotonic time since clock_init(). */
uint64_t time_us(void);
uint32_t time_ms(void);

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

/* Sleep until the next interrupt (WFI), counting the time as idle. */
void cpu_idle(void);
/* Sleep until the next SysTick (other IRQs are serviced meanwhile). */
void cpu_wait_tick(void);
/* Core A busy share over the last full second, 0..1000. */
unsigned cpu_load_permille(void);

#endif
