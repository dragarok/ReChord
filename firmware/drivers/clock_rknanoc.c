/*
 * clock_rknanoc.c -- PLL, core dividers, SysTick time base, WFI idle (see clock.h).
 */
#include "clock.h"

#define CRU(off) (*(volatile uint32_t *)(0x40180000u + (off)))
#define APLL_CON0 0x00u
#define APLL_CON1 0x04u
#define APLL_CON2 0x08u
#define MODE_CON  0x10u
#define CLKSEL0   0x14u
#define CLKSEL1   0x18u
#define CLKSEL7   0x30u            /* eMMC [15] / SD [7] source: 1 = 24 MHz */
#define APLL_LOCK (1u << 10)       /* CON1 */

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SCB_ICSR  (*(volatile uint32_t *)0xE000ED04u)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20u)
#define ICSR_PENDSTSET (1u << 26)

#define OSC_HZ 24000000u
#define PLL_HZ 400000000u
#define TICK_US (1000000u / CLOCK_TICK_HZ)

static uint32_t s_pll_hz = OSC_HZ, s_a_hz = OSC_HZ, s_b_hz = OSC_HZ, s_pclk_hz = OSC_HZ;

/* time base: time = base + ticks * TICK_US + partial tick from the SysTick counter */
static volatile uint32_t s_ticks;
static uint64_t s_base_us;
static uint32_t s_reload;          /* SysTick reload (cycles per tick - 1), 0 = not running */
static uint32_t s_us_frac;         /* TICK_US * 2^32 / cycles per tick */
static uint32_t s_cyc_per_us_q8;   /* core cycles per us, 24.8 fixed point */

/* idle accounting, per one-second window */
static volatile uint32_t s_idle_cyc, s_win_ticks, s_load_pm;   /* idle SysTick cycles this window */

static void systick_start(void)
{
    uint32_t cyc = s_a_hz / CLOCK_TICK_HZ;

    SYST_CSR = 0;
    s_reload = cyc - 1u;
    s_us_frac = (uint32_t)(((uint64_t)TICK_US << 32) / cyc);
    s_cyc_per_us_q8 = (s_a_hz / 1000u) * 256u / 1000u;
    s_ticks = 0;
    SYST_RVR = s_reload;
    SYST_CVR = 0;
    SCB_SHPR3 = (SCB_SHPR3 & 0x00FFFFFFu) | 0xC0000000u;   /* SysTick: low priority */
    SYST_CSR = 7u;                 /* core clock, interrupt, enable */
}

void SysTick_Handler(void)
{
    s_ticks++;
    if (++s_win_ticks == CLOCK_TICK_HZ) {    /* a second: idle cycles per mille of it */
        uint32_t idle = s_idle_cyc, pm = (s_reload + 1u) * CLOCK_TICK_HZ / 1000u;

        s_idle_cyc = 0;
        s_win_ticks = 0;
        s_load_pm = !pm || idle / pm >= 1000u ? 0u : 1000u - idle / pm;
    }
}

/* ticks and elapsed cycles in the current tick, consistent with each other */
static uint32_t now_parts(uint32_t *el)
{
    uint32_t t, v, pend;

    do {
        t = s_ticks;
        v = SYST_CVR;
        pend = SCB_ICSR & ICSR_PENDSTSET;
    } while (t != s_ticks);
    *el = s_reload - v;
    if (pend && *el < s_reload / 2u)   /* wrapped, interrupt not taken yet (IRQs off) */
        t++;
    return t;
}

/* The counter can reload a moment before PENDSTSET shows; a read in that gap
 * lands a whole tick in the past. Time never goes backwards, so a step back of
 * more than half a tick is that race: count the tick. */
static uint64_t s_last_us;

uint64_t time_us(void)
{
    uint32_t el, t;
    uint64_t r;

    if (!s_reload)
        return 0;
    t = now_parts(&el);
    r = s_base_us + (uint64_t)t * TICK_US + (uint32_t)(((uint64_t)el * s_us_frac) >> 32);
    if (r + TICK_US / 2u < s_last_us)
        r += TICK_US;
    if (r > s_last_us)
        s_last_us = r;
    return r;
}

/* Its own arithmetic (32-bit: the M3 divides those in hardware; time_us()'s
 * 64-bit / 1000 is a library call, and the UI loop asks the time many times per
 * pass), with the same never-backwards rule: the reload race reads a tick in the
 * past, and timers comparing "now - then" then saw ~49 days elapsed. */
static uint32_t s_base_ms, s_last_ms;

uint32_t time_ms(void)
{
    uint32_t el, t, r;

    if (!s_reload)
        return 0;
    t = now_parts(&el);
    r = s_base_ms + t * (TICK_US / 1000u) + (uint32_t)(((uint64_t)el * s_us_frac) >> 32) / 1000u;
    if ((int32_t)(r - s_last_ms) < -(int32_t)(TICK_US / 2000u))
        r += TICK_US / 1000u;                /* the race: count the tick */
    if ((int32_t)(r - s_last_ms) < 0)
        r = s_last_ms;                        /* never backwards */
    s_last_ms = r;
    return r;
}

/* Counts SysTick cycles directly, so it also works with interrupts off
 * (as long as nothing else blocks for a whole tick). */
void delay_us(uint32_t us)
{
    uint64_t need;
    uint32_t last;

    if (!s_reload) {               /* before clock_init: ~1 us per pass at <= 24 MHz */
        for (volatile uint32_t n = us * 8u; n; n--) { }
        return;
    }
    need = ((uint64_t)us * s_cyc_per_us_q8) >> 8;
    last = SYST_CVR;
    while (need) {
        uint32_t now = SYST_CVR;
        uint32_t d = last >= now ? last - now : last + s_reload + 1u - now;

        last = now;
        if (d >= need)
            break;
        need -= d;
    }
}

void delay_ms(uint32_t ms)
{
    while (ms--)
        delay_us(1000u);
}

/* Sleeps until an interrupt is pending, counting the time asleep for
 * cpu_load_permille() in SysTick cycles: two counter reads, no clock math (the
 * USB DAC wakes it some 16000 times a second). Interrupts are masked while it
 * sleeps, so the counter wraps at most once: a SysTick wakes it. */
void cpu_idle(void)
{
    uint32_t v0, v1;

    __asm__ volatile ("cpsid i" ::: "memory");
    v0 = SYST_CVR;
    __asm__ volatile ("dsb\n wfi" ::: "memory");   /* wakes on a pending IRQ even when masked */
    v1 = SYST_CVR;
    s_idle_cyc += v0 >= v1 ? v0 - v1 : v0 + s_reload + 1u - v1;
    __asm__ volatile ("cpsie i" ::: "memory");     /* now the IRQ runs */
}

void cpu_wait_tick(void)
{
    uint32_t t = s_ticks;

    while (s_ticks == t)
        cpu_idle();
}

void cpu_wait_tick_or(volatile const int *flag)
{
    uint32_t t = s_ticks;

    while (s_ticks == t && !*flag)
        cpu_idle();
}

unsigned cpu_load_permille(void)
{
    return s_load_pm;
}

/* ---- clocks ------------------------------------------------------------- */

static unsigned s_pclk_shift;

static void apply_dividers(unsigned a_div, unsigned b_div)
{
    unsigned shift = 0;
    uint32_t sel1, sel0;

    s_a_hz = s_pll_hz / a_div;
    s_b_hz = s_pll_hz / b_div;
    while ((s_a_hz >> shift) > 50000000u && shift < 3u)
        shift++;
    s_pclk_hz = s_a_hz >> shift;
    sel1 = 0x03000000u | shift << 8;
    sel0 = 0x70070000u | (b_div - 1u) << 12 | (a_div - 1u);
    /* APB never above 50 MHz: raise its divider before the core speeds up,
     * lower it only after the core has slowed down */
    if (shift >= s_pclk_shift) {
        CRU(CLKSEL1) = sel1;
        CRU(CLKSEL0) = sel0;
    } else {
        CRU(CLKSEL0) = sel0;
        CRU(CLKSEL1) = sel1;
    }
    s_pclk_shift = shift;
}

static void retime(void)
{
    s_base_us = time_us();
    s_base_ms = time_ms();
    systick_start();
}

int clock_init(void)
{
    unsigned n;

    /* storage card clocks to the 24 MHz oscillator before the PLL comes up
     * (dividers kept; the eMMC/SD drivers set their own) */
    CRU(CLKSEL7) = 0x80808080u;

    /* RECHO's sequence: slow mode, dividers /1, PLL reset, program, release */
    CRU(MODE_CON) = 0x00010000u;
    CRU(CLKSEL0) = 0xFFFF0000u;
    CRU(CLKSEL1) = 0x03000000u;
    CRU(APLL_CON1) = 0x80008000u;
    CRU(APLL_CON1) = 0x40004000u;
    CRU(APLL_CON2) = 0x08000000u;
    CRU(APLL_CON0) = 0xFFFF1064u;  /* FBDIV 100, POSTDIV1 1 */
    CRU(APLL_CON1) = 0x11FF1046u;  /* REFDIV 6, POSTDIV2 1 */
    CRU(APLL_CON2) = 0x08000000u;
    CRU(APLL_CON1) = 0x40000000u;
    for (n = 0; n < 200000u && !(CRU(APLL_CON1) & APLL_LOCK); n++) { }
    if (!(CRU(APLL_CON1) & APLL_LOCK)) {
        systick_start();           /* stay on the oscillator */
        return -1;
    }
    s_pll_hz = PLL_HZ;
    apply_dividers(2u, 2u);        /* A 200 MHz, B 200 MHz (B held in reset) */
    CRU(MODE_CON) = 0x00010001u;   /* cores onto the PLL */
    retime();
    return 0;
}

void clock_set_core_a_div(unsigned div)
{
    if (div < 2u || div > 8u || s_pll_hz == OSC_HZ)
        return;
    __asm__ volatile ("cpsid i" ::: "memory");
    s_base_us = time_us();
    s_base_ms = time_ms();
    apply_dividers(div, s_pll_hz / s_b_hz);
    systick_start();
    __asm__ volatile ("cpsie i" ::: "memory");
}

void clock_set_core_b_div(unsigned div)
{
    if (div < 1u || div > 8u || s_pll_hz == OSC_HZ)
        return;
    s_b_hz = s_pll_hz / div;
    CRU(CLKSEL0) = 0x70000000u | (div - 1u) << 12;
}

uint32_t clock_pll_hz(void)    { return s_pll_hz; }
uint32_t clock_core_hz(void)   { return s_a_hz; }
uint32_t clock_core_b_hz(void) { return s_b_hz; }
uint32_t clock_pclk_hz(void)   { return s_pclk_hz; }
