/*
 * clock.h -- CPU clocks and the time base (RKNanoD, both cores).
 *
 * Clock tree (SDK Hw_cru.h + PowerManager.c; the sequence is stock MINIV390's SetPllFreq
 * 0x0305222C, work/stock_boot/06_clock_stability.md 1.1):
 *   PLL      = 24 MHz / REFDIV * (FBDIV + FRAC / 2^24) / POSTDIV1 / POSTDIV2
 *   core A (sys_core) = PLL / (SEL0[2:0] + 1); the AHB (LOW RAMs, DMA, VOP, storage) runs with it
 *   core B (cal_core) = PLL / (SEL0[14:12] + 1); its AHB (the HIGH RAMs, HIFI) runs with it
 *   pclk_logic = core A >> SEL1[9:8] (I2C, PWM, WDT, the APB side of every peripheral)
 *   eMMC/SD card clocks, UART1 (BT), SAR-ADC and the 24 MHz timer run from the crystal; I2S1's MCLK
 *   from the ACODEC PLL: none of them follows the PLL.
 * Two operating points, both stock's own on the 512 MB unit (work/coreb_clock/FINDINGS.md):
 *   UI   PLL 96 (VCO 480): A 96 (or 48), B 96 (or the governor's /2../8), pclk 48 -- boot, UI, the
 *        light formats. clock_init() sets it; it is the only point a boot ever uses.
 *   FLAC PLL 1344 (VCO 1344, integer): A /6 = 224, B /3 = 448, pclk /4 = 56, GRF 0x3040 + 0xC000 --
 *        stock's FLAC/hi-res decode point [trace], fixed for a whole track (no divider changes).
 * clock_set_point() moves between them at a track open (services/cpuclk.c decides; audio.c parks
 * core B). Never VCO 400 or 200 MHz (the corruption on this unit, docs/BRINGUP_512M.md).
 *
 * Time base: SysTick on the core-A clock at 100 Hz (stock's SysTickPeriodSet(10)); time_us()
 * interpolates inside the tick. Delays are time-based, so they hold at any clock. cpu_idle() sleeps
 * in WFI and accounts idle time for cpu_load_permille(). Across a PLL switch (the cores on the
 * crystal for a while) the time base is carried by the 24 MHz timer (TIMER0).
 */
#ifndef RECHORD_CLOCK_H
#define RECHORD_CLOCK_H

#include <stdint.h>

#define CLOCK_TICK_HZ 100u

/* clock_init_at(0, 0). Returns 0, or -1 if the PLL did not lock (cores stay on 24 MHz). */
int clock_init(void);
/* Operating point 0 (stock's 96 MHz UI clock: PLL 96, A/B /1, pclk 48); 1 and up keep the ROM's
 * clocks. trace(n), if given, is called before each stage of the sequence (1..8; 0x14 = no lock),
 * so a boot that stops in here leaves where. */
int clock_init_at(unsigned point, void (*trace)(uint32_t));
int clock_keep_rom(void);            /* the ROM's clocks, read back; no CRU write (returns 1) */
/* The core A divider for hz at the UI point's PLL, or the nearest below it. */
unsigned clock_div_for(uint32_t hz);

/* Core A divider of the UI point's PLL (1..8); reprograms the APB divider and SysTick. Call from
 * thread context only. At the FLAC point the divider is fixed (/6): the request is kept and applied
 * when the UI point comes back. */
void clock_set_core_a_div(unsigned div);
/* Core B divider of the PLL (1..8), never faster than CLOCK_B_MAX_HZ; at the FLAC point fixed (/3). */
void clock_set_core_b_div(unsigned div);
/* Core B acked its new clock (audio_shared b_div_ack): a slower clock's memory timing applies now. */
void clock_core_b_settled(void);

/* ---- operating points (services/cpuclk.c) ---- */
enum { CLOCK_POINT_UI = 0, CLOCK_POINT_FLAC = 1, CLOCK_POINTS };
#define CLOCK_A_MAX_HZ 232000000u   /* stock's highest core A, its 1392 MHz point [trace/model] */
#define CLOCK_B_MAX_HZ 450000000u   /* stock 512 MB's highest core B, its 1350 MHz point [trace] */
/* What a switch did (logged by services/cpuclk.c). Times from the 24 MHz timer. */
typedef struct {
    uint8_t from, to, locked, back;  /* back: the point did not lock, the old one was restored */
    uint32_t total_us, lock_us, dma_off_us;   /* dma_off_us: the longest the I2S DMA was suspended */
    uint32_t dma_mask;               /* the channels that were running (suspended, resumed) */
} clock_switch_t;
unsigned clock_point(void);
const char *clock_point_name(unsigned point);
/* The switch, stock's order (FreqSetPrevProcess 0x03068678 + SetPllFreq 0x0305222C): IRQs off, DMA
 * suspended, cores to the crystal, PLL programmed, GRF memory timing and VOP timing for the new
 * clock, lock polled, dividers, cores back on the PLL, DMA resumed, the time base carried across.
 * keep_dma: channels that run on while the PLL locks with the bus on the crystal (the I2S's, so a
 * gapless boundary has no gap; beyond stock, which suspends it throughout). Core B must be quiet
 * (held in its loop, or in reset): the caller's job. Returns 0, or ST_LOCK (back at the old point). */
int clock_set_point(unsigned point, uint32_t keep_dma, clock_switch_t *rec);
enum { CLOCK_SW_BAD = -1, CLOCK_SW_OK = 0, CLOCK_SW_NOLOCK = 1 /* back at the old point */,
       CLOCK_SW_CRYSTAL = 2 /* neither locked: the cores on the 24 MHz crystal */ };
/* Every PLL-fed peripheral clock (I2S0/1, EBC, SDMMC, eMMC, SPI0/1, UART0-5, SFC) whose clock gate is
 * open, at a PLL of pll_hz: the SDK gate id of the first one above 50 MHz, or -1 if none is. */
int clock_periph_check(uint32_t pll_hz);
/* Boot: stock's CpuClockGateInit (B2h, MINIV390 0x0307a214) where it applies, and the PLL-fed
 * peripheral clocks ReChord keeps moved to the crystal (clock_rknanoc.c lists each). */
void clock_periph_init(void);
/* The 24 MHz timer (TIMER0, SDK hw_timer.h, 0x400E0000): opened and started, read, closed. The tick
 * count between two readings does not depend on which way it counts. */
void clock_timer_open(void);
uint32_t clock_timer_now(void);
uint32_t clock_timer_ticks(uint32_t a, uint32_t b);
void clock_timer_close(void);
/* A long stretch in a handler or with interrupts off (a self-test): mark the time, and afterwards
 * carry the time base across it from the 24 MHz timer (SysTick restarted, no tick lost). */
void clock_hold_begin(void);
void clock_hold_end(void);
/* Registers for the log after a switch: CON0, CON1, CON2, MODE, CLKSEL0..12, CLKGATE0..9, GRF
 * INTER_CON0 (30 words). */
void clock_regs(uint32_t out[30]);
/* The panel's hook: the VOP's bus timing for a core-A clock of hz, written while the cores run on
 * the crystal (stock step 10, Freq_VopTiming_Update 0x0305449C); returns what it wrote (0: none).
 * Weak; drivers/panel_io_rknanoc.c. */
uint32_t clock_vop_hook(uint32_t hz);

uint32_t clock_pll_hz(void);
uint32_t clock_core_hz(void);       /* core A */
uint32_t clock_core_b_hz(void);
uint32_t clock_pclk_hz(void);       /* APB (pclk_logic) */

/* Monotonic time since clock_init(). */
uint64_t time_us(void);
uint32_t time_ms(void);
/* Core-A clock cycles since clock_init() (SysTick ticks x reload + the cycles into this tick),
 * wrapping at 2^32 (21 s at 200 MHz): sub-microsecond stamps for spans well under that, at a
 * clock that does not change meanwhile. Works with interrupts off, as delay_us() does. */
uint32_t time_cycles(void);

/* A time_ms() value as a nonzero stamp (0 meaning "not started"). Never `t | 1`: that is a
 * millisecond in the future half the time, so `time_ms() - stamp` wraps to ~2^32 in the same
 * millisecond and every "N ms passed" check fires at once (the USB arm was held after 10 ms
 * on half the boots). 0 maps to the millisecond before it, one early at worst. */
static inline uint32_t time_stamp(uint32_t t) { return t ? t : 0xFFFFFFFFu; }

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

/* Sleep until the next interrupt (WFI), counting the time as idle. */
void cpu_idle(void);
/* Sleep until the next SysTick (other IRQs are serviced meanwhile). */
void cpu_wait_tick(void);
/* The same, or until *flag is set (by an interrupt handler). */
void cpu_wait_tick_or(volatile const int *flag);
/* Core A busy share over the last full second, 0..1000. */
unsigned cpu_load_permille(void);

#endif
