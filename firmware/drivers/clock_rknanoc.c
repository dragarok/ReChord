/*
 * clock_rknanoc.c -- PLL, core dividers, SysTick time base, WFI idle (see clock.h).
 */
#include "clock.h"
#include "services/watchdog.h"

#define CRU(off) (*(volatile uint32_t *)(0x40180000u + (off)))
#define APLL_CON0 0x00u
#define APLL_CON1 0x04u
#define APLL_CON2 0x08u
#define MODE_CON  0x10u
#define CLKSEL0   0x14u
#define CLKSEL1   0x18u
#define CLKSEL7   0x30u            /* eMMC [15] / SD [7] source: 1 = 24 MHz */
#define APLL_LOCK (1u << 10)       /* CON1 */

/* GRF_INTER_CON0, as stock's SetPllFreq writes it (MINIV390 0x0305233a..0x03052392): bits
 * 0x3040 set while core A (sys hclk) runs at 180 MHz or more, bits 0xC000 while core B (cal
 * hclk) runs at 300 MHz or more; RECHO does the first too (docs/RECHO_POWER_CFG.md C.1). Memory
 * timing for the fast clocks [inferred]. Set before a core speeds up, cleared only after it has
 * slowed down. */
#define GRF_INTER_CON0 (*(volatile uint32_t *)0x500100C4u)
#define MEM_FAST_A_HZ  180000000u
#define MEM_FAST_B_HZ  300000000u
#define CORE_A_MAX_HZ  200000000u

static void mem_timing(uint32_t a_hz, uint32_t b_hz)
{
    GRF_INTER_CON0 = 0xF0400000u | (a_hz >= MEM_FAST_A_HZ ? 0x3040u : 0u) |
                     (b_hz >= MEM_FAST_B_HZ ? 0xC000u : 0u);
    __asm__ volatile ("dsb" ::: "memory");
}

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SCB_ICSR  (*(volatile uint32_t *)0xE000ED04u)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20u)
#define ICSR_PENDSTSET (1u << 26)

#define OSC_HZ 24000000u
#define TICK_US (1000000u / CLOCK_TICK_HZ)

static uint32_t s_pll_hz = OSC_HZ, s_a_hz = OSC_HZ, s_b_hz = OSC_HZ, s_pclk_hz = OSC_HZ;
static unsigned s_pclk_shift;

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

    watchdog_feed();                 /* a loop that waits for the tick is alive (UI waits on keys, cable) */

    while (s_ticks == t)
        cpu_idle();
}

void cpu_wait_tick_or(volatile const int *flag)
{
    uint32_t t = s_ticks;

    watchdog_feed();

    while (s_ticks == t && !*flag)
        cpu_idle();
}

unsigned cpu_load_permille(void)
{
    return s_load_pm;
}

/* ---- clocks ------------------------------------------------------------- */


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

/* Operating points (services/bootcrumb.c moves one down for every boot of this build that
 * stopped in clock_init; past the last, the ROM's clocks are kept):
 *   0  PLL 96 (VCO 480: FBDIV 120, REFDIV 6, POSTDIV 1/5: CON0 0x1078, CON1 0x1146), A and B /1
 *      = 96 MHz, pclk 48 MHz: stock's UI and main-loop clock on the 512 MB unit (FREQ_INIT;
 *      SetPllFreq 0x0305222c traced, work/stock_boot/01_entry_init.md B7, 06_clock_stability.md).
 * Not used: the 200 MHz points from VCO 400 (RECHO's PLL 400 /2, stock's MediaInit PLL 200 /1).
 * On the 512 MB unit 200 MHz from VCO 400 corrupted data and RAM (2026-10-16, docs/
 * BRINGUP_512M.md); stock only passes through VCO 400 for milliseconds. A faster point comes back
 * only after a device self-test proves it. */
static const struct { uint16_t con0, con1; uint32_t pll_hz; uint8_t a_div, b_div; } k_points[] = {
    { 0x1078u, 0x1146u,  96000000u, 1u, 1u },
};

int clock_init_at(unsigned point, void (*trace)(uint32_t))
{
    unsigned n;

#define TRACE(x) do { if (trace) trace(x); } while (0)
    if (point >= sizeof k_points / sizeof k_points[0])
        return clock_keep_rom();
    /* storage card clocks to the 24 MHz oscillator before the PLL comes up
     * (dividers kept; the eMMC/SD drivers set their own) */
    TRACE(1);
    CRU(CLKSEL7) = 0x80808080u;

    /* stock's and RECHO's sequence: slow mode, dividers /1, PLL reset, program, release */
    TRACE(2);
    CRU(MODE_CON) = 0x00010000u;
    CRU(CLKSEL0) = 0xFFFF0000u;
    CRU(CLKSEL1) = 0x03000000u;
    TRACE(3);
    CRU(APLL_CON1) = 0x80008000u;
    CRU(APLL_CON1) = 0x40004000u;
    CRU(APLL_CON2) = 0x08000000u;
    CRU(APLL_CON0) = 0xFFFF0000u | k_points[point].con0;
    CRU(APLL_CON1) = 0x11FF0000u | k_points[point].con1;
    CRU(APLL_CON2) = 0x08000000u;
    CRU(APLL_CON1) = 0x40000000u;
    TRACE(4);
    for (n = 0; n < 200000u && !(CRU(APLL_CON1) & APLL_LOCK); n++) { }
    if (!(CRU(APLL_CON1) & APLL_LOCK)) {
        TRACE(0x14);
        systick_start();           /* stay on the oscillator */
        return -1;
    }
    TRACE(5);
    s_pll_hz = k_points[point].pll_hz;
    mem_timing(s_pll_hz / k_points[point].a_div, s_pll_hz / k_points[point].b_div);
    TRACE(6);
    apply_dividers(k_points[point].a_div, k_points[point].b_div);
    TRACE(7);
    CRU(MODE_CON) = 0x00010001u;   /* cores onto the PLL */
    TRACE(8);                      /* (persisted at the new clock: the switch survived) */
    retime();
    return 0;
#undef TRACE
}

int clock_init(void)
{
    return clock_init_at(0, 0);
}

/* The divider that runs core A at hz or the nearest below (never above 200 MHz). */
unsigned clock_div_for(uint32_t hz)
{
    unsigned div = (unsigned)((s_pll_hz + hz - 1u) / hz);

    while (div < 8u && s_pll_hz / div > CORE_A_MAX_HZ)
        div++;
    return div < 1u ? 1u : div > 8u ? 8u : div;
}

/* Fallback when clock_init stopped a boot of this build (services/bootcrumb.c): no CRU write
 * at all. The clocks the ROM left (48 MHz on the 512 MB unit) are read back, and only the time
 * base starts. */
int clock_keep_rom(void)
{
    uint32_t c0 = CRU(APLL_CON0), c1 = CRU(APLL_CON1);
    uint32_t fb = c0 & 0xFFFu, p1 = (c0 >> 12) & 7u, p2 = (c1 >> 6) & 7u, ref = c1 & 0x3Fu;

    s_pll_hz = (CRU(MODE_CON) & 1u) && fb && p1 && p2 && ref ? OSC_HZ / ref * fb / p1 / p2 : OSC_HZ;
    s_a_hz = s_pll_hz / ((CRU(CLKSEL0) & 7u) + 1u);
    s_b_hz = s_pll_hz / (((CRU(CLKSEL0) >> 12) & 7u) + 1u);
    s_pclk_shift = (CRU(CLKSEL1) >> 8) & 3u;
    s_pclk_hz = s_a_hz >> s_pclk_shift;
    systick_start();
    return 1;
}

void clock_set_core_a_div(unsigned div)
{
    uint32_t a;

    if (div < 1u || div > 8u || s_pll_hz == OSC_HZ || s_pll_hz / div > CORE_A_MAX_HZ)
        return;
    a = s_pll_hz / div;
    __asm__ volatile ("cpsid i" ::: "memory");
    s_base_us = time_us();
    s_base_ms = time_ms();
    mem_timing(a > s_a_hz ? a : s_a_hz, s_b_hz);   /* the timing for the faster of old and new */
    apply_dividers(div, s_pll_hz / s_b_hz);
    mem_timing(s_a_hz, s_b_hz);                     /* then for the new */
    systick_start();
    __asm__ volatile ("cpsie i" ::: "memory");
}

void clock_set_core_b_div(unsigned div)
{
    uint32_t b;

    if (div < 1u || div > 8u || s_pll_hz == OSC_HZ)
        return;
    b = s_pll_hz / div;
    mem_timing(s_a_hz, b > s_b_hz ? b : s_b_hz);
    s_b_hz = b;
    CRU(CLKSEL0) = 0x70000000u | (div - 1u) << 12;
    mem_timing(s_a_hz, s_b_hz);
}

uint32_t clock_pll_hz(void)    { return s_pll_hz; }
uint32_t clock_core_hz(void)   { return s_a_hz; }
uint32_t clock_core_b_hz(void) { return s_b_hz; }
uint32_t clock_pclk_hz(void)   { return s_pclk_hz; }
