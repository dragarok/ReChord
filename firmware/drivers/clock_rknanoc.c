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
#define CLKSEL2   0x1Cu            /* TIMER0 [7] source (0 = 24 MHz), [4:0] divider - 1 */
#define CLKSEL7   0x30u            /* eMMC [15] / SD [7] source: 1 = 24 MHz */
#define CLKGATE(n) (0x80u + 4u * (n))
#define APLL_LOCK (1u << 10)       /* CON1 */
#define OSC_TICKS_PER_US 24u

/* GRF_INTER_CON0, as stock's SetPllFreq writes it (MINIV390 0x0305233a..0x03052392): bits
 * 0x3040 set while core A (sys hclk) runs at 180 MHz or more, bits 0xC000 while core B (cal
 * hclk) runs at 300 MHz or more; RECHO does the first too (docs/RECHO_POWER_CFG.md C.1). Memory
 * timing for the fast clocks [inferred]. Set before a core speeds up, cleared only after it has
 * slowed down. */
#define GRF_INTER_CON0 (*(volatile uint32_t *)0x500100C4u)
#define MEM_FAST_A_HZ  180000000u
/* Core B's half (bits 15:14, RECHO 0.10's "HIGHRAM response cycle", work/recho010/STATIC.md C.4):
 * on above 150 MHz [code], set before B speeds up past it and cleared only once B has acked a
 * slower clock (clock_core_b_settled). Stock set it at 300 MHz and up; RECHO 0.9.2 raised the clock
 * first and never cleared it. ReChord's B never runs above the 96 MHz PLL point today, so this
 * keeps the order for when it does. */
#define MEM_FAST_B_HZ  150000000u
/* Core A's ceiling: stock's own pair at its decode points, A 224 at 1344 and 232 at 1392 [trace,
 * model: FREQSetARMFreq 0x0306872E rounds A's divider down, so A ends above the table's 200]. Was
 * 200 MHz (the SDK table), which blocked stock's 224. Only the FLAC point runs A above 96. */
#define CORE_A_MAX_HZ  CLOCK_A_MAX_HZ

static void mem_timing(uint32_t a_hz, uint32_t b_hz)
{
    GRF_INTER_CON0 = 0xF0400000u | (a_hz >= MEM_FAST_A_HZ ? 0x3040u : 0u) |
                     (b_hz > MEM_FAST_B_HZ ? 0xC000u : 0u);
    __asm__ volatile ("dsb" ::: "memory");
}

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define SCB_ICSR  (*(volatile uint32_t *)0xE000ED04u)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20u)
#define ICSR_PENDSTSET (1u << 26)

#define OSC_HZ 24000000u
#define SCB_ICSR_PENDSTCLR (1u << 25)
#define TICK_US (1000000u / CLOCK_TICK_HZ)

static uint32_t s_pll_hz = OSC_HZ, s_a_hz = OSC_HZ, s_b_hz = OSC_HZ, s_pclk_hz = OSC_HZ;
static uint32_t s_b_mem_hz = OSC_HZ;       /* the core-B clock the memory timing is set for */
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

/* The 10 ms work that must not wait for the main loop (startup/ap_tick.c: the keys, core B's
 * clock requests); weak, so the tests that link the clock alone need none. */
extern void tick_hook(void) __attribute__((weak));

void SysTick_Handler(void)
{
    s_ticks++;
    if (tick_hook)
        tick_hook();
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
    if (r < s_last_us)
        r = s_last_us;                        /* never backwards, as time_ms(): a frame's duration
                                               * read 4294960 ms on the device (2026-10-18) */
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

static uint32_t s_last_cyc;

uint32_t time_cycles(void)
{
    uint32_t el, t = now_parts(&el), r = t * (s_reload + 1u) + el;

    if ((int32_t)(r - s_last_cyc) < -(int32_t)(s_reload / 2u))
        r += s_reload + 1u;                   /* the reload race (time_us): count the tick */
    s_last_cyc = r;
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

/* The run-time points (clock_set_point). Every value is stock MINIV390's on the 512 MB unit:
 *   UI   = the boot point above: CON0 0x1078, CON1 0x1146 (VCO 480, POSTDIV 1/5), CLKSEL0 0x0000,
 *          CLKSEL1 0x0100 (pclk 48) [trace: work/coreb_clock/runs/stock_points.txt, "UI" rows];
 *          core A at power.c's divider (s_ui_a_div), as before this point existed.
 *   FLAC = CON0 0x1150 (FBDIV 336, POSTDIV1 1), CON1 0x1046 (DSMPD 1: integer, POSTDIV2 1, REFDIV 6),
 *          CON2 FRAC 0, CLKSEL0 0x2005 (A /6, B /3), CLKSEL1 0x0200 (pclk = A / 4 = 56 MHz)
 *          [trace: stock_CD16 and stock_HR24_192, 28.034 s / 28.041 s, held to the end of the track].
 *          GRF INTER_CON0 0x3040 (A >= 180 MHz) and 0xC000 (B >= 300 MHz) [code: 0x0305235A..92].
 * Not here, though stock uses them: 1350 MHz (fractional, APE/ALAC/DSD), 1392 MHz (lossy), 144 MHz
 * (audio init), and the VCO 400 points (192 MHz boot, 200 MHz MediaInit): one decode point to prove. */
typedef struct {
    uint16_t con0, con1;
    uint32_t frac;                 /* CON2 [23:0] */
    uint32_t pll_hz;
    uint8_t a_div, b_div, pclk_shift;
    const char *name;
} run_point_t;

static const run_point_t k_run[CLOCK_POINTS] = {
    [CLOCK_POINT_UI]   = { 0x1078u, 0x1146u, 0u,   96000000u, 1u, 1u, 1u, "UI" },
    [CLOCK_POINT_FLAC] = { 0x1150u, 0x1046u, 0u, 1344000000u, 6u, 3u, 2u, "FLAC" },
};

static unsigned s_point = CLOCK_POINT_UI;  /* (clock_init's point 0 is the UI point) */
static unsigned s_ui_a_div = 1u;           /* power.c's core A divider at the UI point */
static int s_ui_ok;                        /* clock_init() set the UI point (else no point switch) */

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
    s_b_mem_hz = s_pll_hz / k_points[point].b_div;
    mem_timing(s_pll_hz / k_points[point].a_div, s_b_mem_hz);
    TRACE(6);
    apply_dividers(k_points[point].a_div, k_points[point].b_div);
    TRACE(7);
    CRU(MODE_CON) = 0x00010001u;   /* cores onto the PLL */
    TRACE(8);                      /* (persisted at the new clock: the switch survived) */
    retime();
    s_ui_ok = point == 0u;
    return 0;
#undef TRACE
}

int clock_init(void)
{
    return clock_init_at(0, 0);
}

/* The divider that runs core A at hz or the nearest below, on the UI point's PLL (the only one
 * whose core A divider moves: power.c's states). */
unsigned clock_div_for(uint32_t hz)
{
    uint32_t pll = s_point == CLOCK_POINT_UI ? s_pll_hz : k_run[CLOCK_POINT_UI].pll_hz;
    unsigned div = (unsigned)((pll + hz - 1u) / hz);

    while (div < 8u && pll / div > CORE_A_MAX_HZ)
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
    s_b_hz = s_b_mem_hz = s_pll_hz / (((CRU(CLKSEL0) >> 12) & 7u) + 1u);
    s_pclk_shift = (CRU(CLKSEL1) >> 8) & 3u;
    s_pclk_hz = s_a_hz >> s_pclk_shift;
    systick_start();
    return 1;
}

void clock_set_core_a_div(unsigned div)
{
    uint32_t a;

    if (div < 1u || div > 8u)
        return;
    if (s_point != CLOCK_POINT_UI) {       /* fixed at the FLAC point: for when the UI point is back */
        s_ui_a_div = div;
        return;
    }
    if (s_pll_hz == OSC_HZ || s_pll_hz / div > CORE_A_MAX_HZ)
        return;
    s_ui_a_div = div;
    a = s_pll_hz / div;
    __asm__ volatile ("cpsid i" ::: "memory");
    s_base_us = time_us();
    s_base_ms = time_ms();
    mem_timing(a > s_a_hz ? a : s_a_hz, s_b_mem_hz);   /* the timing for the faster of old and new */
    apply_dividers(div, s_pll_hz / s_b_hz);
    mem_timing(s_a_hz, s_b_mem_hz);                     /* then for the new */
    systick_start();
    __asm__ volatile ("cpsie i" ::: "memory");
}

void clock_set_core_b_div(unsigned div)
{
    uint32_t b;

    if (div < 1u || div > 8u || s_pll_hz == OSC_HZ || s_point != CLOCK_POINT_UI)
        return;                            /* (the FLAC point holds B at /3 for the whole track) */
    while (div < 8u && s_pll_hz / div > CLOCK_B_MAX_HZ)
        div++;                             /* never above stock's 450 MHz, whatever was asked */
    b = s_pll_hz / div;
    if (b > s_b_mem_hz) {                  /* faster: the memory timing for it first */
        s_b_mem_hz = b;
        mem_timing(s_a_hz, s_b_mem_hz);
    }
    s_b_hz = b;
    CRU(CLKSEL0) = 0x70000000u | (div - 1u) << 12;
    /* slower: the timing stays for the old clock until B has acked (clock_core_b_settled) */
}

void clock_core_b_settled(void)
{
    if (s_b_mem_hz != s_b_hz) {
        s_b_mem_hz = s_b_hz;
        mem_timing(s_a_hz, s_b_mem_hz);
    }
}

uint32_t clock_pll_hz(void)    { return s_pll_hz; }
uint32_t clock_core_hz(void)   { return s_a_hz; }
uint32_t clock_core_b_hz(void) { return s_b_hz; }
uint32_t clock_pclk_hz(void)   { return s_pclk_hz; }

/* ---- the 24 MHz timer ------------------------------------------------------
 * TIMER0 (SDK hw_timer.h: LoadCount0/1 +0/+4, CurrentValue0/1 +8/+0xC, ControlReg +0x10: EN bit 0,
 * user mode bit 1, interrupt unmask bit 2), clocked from the crystal (CLKSEL2 bit 7 = 0, divider 1:
 * SDK timer.c SetTimerFreq(XIN24M, 24 MHz), the ROM's CLKSEL2 = 0), free-running from all ones.
 * Gates: CLK_TIMER0 (SDK id 11), PCLK_TIMER (142); stock gates both at boot (B2h), so this opens
 * them around a switch and its self-test only. Which way it counts is [inferred] (down, like the
 * Rockchip timers Linux drives, timer-rockchip.c); clock_timer_ticks() works either way. */
#define TMR(o) (*(volatile uint32_t *)(0x400E0000u + (o)))

static int s_tmr_open;

void clock_timer_open(void)
{
    if (s_tmr_open)
        return;                                /* (running: a reading in progress stays valid) */
    s_tmr_open = 1;
    CRU(CLKGATE(0)) = 1u << (11 + 16);         /* CLK_TIMER0 on */
    CRU(CLKGATE(8)) = 1u << (14 + 16);         /* PCLK_TIMER on */
    CRU(CLKSEL2) = 0x009F0000u;                /* TIMER0: the crystal, /1 */
    TMR(0x10) = 0;
    TMR(0x00) = 0xFFFFFFFFu;
    TMR(0x04) = 0xFFFFFFFFu;
    TMR(0x10) = 1u;                            /* enabled, free-running, interrupt masked */
}

uint32_t clock_timer_now(void)
{
    return TMR(0x08);
}

uint32_t clock_timer_ticks(uint32_t a, uint32_t b)
{
    uint32_t d1 = a - b, d2 = b - a;

    return d1 < d2 ? d1 : d2;
}

void clock_timer_close(void)
{
    s_tmr_open = 0;
    TMR(0x10) = 0;
    CRU(CLKGATE(0)) = 0x08000800u;             /* stock's boot state: both gated (B2h) */
    CRU(CLKGATE(8)) = 0x40004000u;
}

/* ---- DMA suspend (stock DmaSuspend 0x03060B06 / DmaResume 0x03060B44) -------
 * Each channel 0..5 enabled in ChEnReg (0x600003A0): CFG_LO bit 8 (CH_SUSP) set, then FIFO_EMPTY
 * (bit 9) polled up to 800 times; resume clears CH_SUSP. */
#define DMAR(o) (*(volatile uint32_t *)(0x60000000u + (o)))
#define DMA_CFG(c) DMAR(0x40u + (c) * 0x58u)

static uint32_t dma_suspend(uint32_t chans)
{
    uint32_t on = DMAR(0x3A0u) & chans & 0x3Fu;

    for (unsigned c = 0; c < 6u; c++) {
        if (!(on & (1u << c)))
            continue;
        DMA_CFG(c) |= 1u << 8;
        for (unsigned n = 0; n < 800u && !(DMA_CFG(c) & (1u << 9)); n++)
            ;
    }
    return on;
}

static void dma_resume(uint32_t chans)
{
    for (unsigned c = 0; c < 6u; c++)
        if (chans & (1u << c))
            DMA_CFG(c) &= ~(1u << 8);
}

/* ---- PLL-fed peripheral clocks -------------------------------------------
 * Each block's function clock, its gate (SDK eCLOCK_GATE), and where its source and divider sit
 * (SDK Hw_cru.h CLKSELn bit fields and cru.c Set*Freq: the source bit 1 = the 24 MHz crystal, 0 =
 * the PLL). The ROM leaves them all on the PLL (/8; EBC /16) and every gate open (device:
 * decoded.txt); at PLL 1344 that would be 168 (84) MHz. Limit 50 MHz [inferred]: the most any of
 * them has run at on this unit (ReChord's P0, PLL 400 / 8; the TRM is not on disk). */
typedef struct {
    uint8_t gate, sel, src_shift, src_mask, src_pll, div_shift, div_mask;
} periph_clk_t;

static const periph_clk_t k_periph[] = {
    /* gate  CLKSEL  src at, mask, PLL value   divider at, mask */
    { 16, 3, 15, 1, 0, 0, 0x7F },     /* I2S1: [15] 0 SOC PLL / 1 ACODEC PLL ([9:8] = 2: 12 MHz, below) */
    { 20, 4, 15, 1, 0, 0, 0x7F },     /* I2S0 */
    { 23, 5,  8, 1, 0, 0, 0xFF },     /* EBC dclk: [8] 0 PLL / 1 USB 480 MHz */
    { 24, 7,  7, 1, 0, 0, 0x1F },     /* SDMMC */
    { 25, 7, 15, 1, 0, 8, 0x1F },     /* eMMC */
    { 26, 8,  7, 1, 0, 0, 0x3F },     /* SPI0 */
    { 27, 8, 15, 1, 0, 8, 0x3F },     /* SPI1 */
    { 28, 9, 15, 1, 0, 8, 0x3F },     /* UART0 */
    { 29, 9,  6, 3, 0, 0, 0x3F },     /* UART1: [7:6] 0 PLL / 1 24 MHz / 2 480 MHz */
    { 30, 10, 7, 1, 0, 0, 0x3F },     /* UART2 */
    {  8, 10, 15, 1, 0, 8, 0x1F },    /* SFC */
    { 32, 11, 15, 1, 0, 8, 0x3F },    /* UART3 */
    { 33, 12, 7, 1, 0, 0, 0x3F },     /* UART4 */
    { 34, 12, 15, 1, 0, 8, 0x3F },    /* UART5 */
};
#define PERIPH_MAX_HZ 50000000u

int clock_periph_check(uint32_t pll_hz)
{
    for (unsigned i = 0; i < sizeof k_periph / sizeof k_periph[0]; i++) {
        const periph_clk_t *p = &k_periph[i];
        uint32_t sel = CRU(0x14u + 4u * p->sel);

        if (CRU(CLKGATE(p->gate >> 4)) & (1u << (p->gate & 15u)))
            continue;                          /* gated */
        if (((sel >> p->src_shift) & p->src_mask) != p->src_pll)
            continue;                          /* not on the PLL */
        if ((p->sel == 3u || p->sel == 4u) && ((sel >> 8) & 3u) == 2u)
            continue;                          /* I2S on the 12 MHz crystal path */
        if (pll_hz / (((sel >> p->div_shift) & p->div_mask) + 1u) > PERIPH_MAX_HZ)
            return p->gate;
    }
    return -1;
}

/* Boot (after clock_init): stock's CpuClockGateInit (B2h, 0x0307a214..0x0307a23e: CLKGATE0..9 =
 * 0x7FFF1FA4, 0xFFFFCF9F, 0x00070007, 0x03FF03FF, 0x1FFF1FFF, 0x07EF0606, 0xFFFFEC00, 0xFFFF4000,
 * 0xFFFFFCFE, 0x01FF0004; work/stock_boot/01_entry_init.md) for every block ReChord never uses, and
 * the PLL-fed function clocks of the ones it keeps moved to the crystal first.
 * Gated here, as stock: SFC (8, hclk 111), TIMER0/1 (11, 12; pclk 142: opened around a switch), I2S0
 * (src 20, pclk 129), EBC (23, hclk 109), SPI0/1 (26, 27; pclk 134, 135), UART2 (30, 138; stock
 * gates it too, then opens it for its debug UART, which ReChord has not), UART3-5 (32-34; 139-141),
 * TEST (31), PVTM (90), SYNTH (110), IMDCT (126), I2C1/2 (pclk 132, 133).
 * Kept on, against stock's word, and why:
 *   HCLK_CAL_CORE (2), the HIGH RAMs, HIFI, DMA2 (CLKGATE3/4): core B, released at the first play
 *     (the release with HCLK_CAL_CORE gated is a VIOLATION in the model: stock opens it then);
 *   CLK_USBPHY (7), HCLK_USBC (106): the USB block's own boot park and sessions (usb_msc_rknanoc.c),
 *     and the ROM's maskrom path, which is never touched (memory: maskrom-path-sacred);
 *   SARADC (10): the keys and the battery, from the first line of Main (adc_init);
 *   SDMMC, eMMC (24, 25; hclk 107, 108): the boot trail writes the eMMC from the first line of Main
 *     (bootcrumb.c) and the storage driver opens them itself; both run from the crystal (CLKSEL7);
 *   I2S1 (16-19; pclk 130), I2C0 (pclk 131), ACODEC: the DAC driver opens them at its bring-up;
 *   PCLK_WDT (143): board_reboot_rockusb() writes the watchdog before it opens every gate, on the
 *     way to the loader (the maskrom path again);
 *   UART0 (28, pclk 136): open in stock's word too (B2h); unused, but moved to the crystal below;
 *   PCLK_MAILBOX (146): core B's wake-up while it holds for a switch (audio_shared.h), as stock's
 *     BB_HOLD (stock gates it at boot and opens it with core B).
 * The function clocks moved to the crystal (CLKSELn source bits; values per SDK cru.c): UART0
 * (CLKSEL9 [15]), UART1 (CLKSEL9 [7:6] = 1: the ROM's value, before the BT driver's own), UART2 (CLKSEL10 [7], what stock's DebugInit writes: 0x00800080), SFC (CLKSEL10
 * [15], the ROM's), UART3 (CLKSEL11 [15]), UART4/5 (CLKSEL12 [7], [15]), SPI0/1 (CLKSEL8 [7], [15]),
 * I2S0 (CLKSEL4 [9:8] = 2: the 12 MHz crystal path). EBC has no crystal source: gated only. */
void clock_periph_init(void)
{
    CRU(0x24u) = 0x03000200u;              /* CLKSEL4: I2S0 from the 12 MHz crystal path */
    CRU(0x34u) = 0x80808080u;              /* CLKSEL8: SPI0, SPI1 from the crystal */
    CRU(0x38u) = 0x80C08040u;              /* CLKSEL9: UART0 and UART1 from the crystal (UART1: as the
                                            * ROM leaves it, 0x740, and bt_jieli.c sets it again) */
    CRU(0x3Cu) = 0x80808080u;              /* CLKSEL10: UART2, SFC from the crystal */
    CRU(0x40u) = 0x80008000u;              /* CLKSEL11: UART3 */
    CRU(0x44u) = 0x80808080u;              /* CLKSEL12: UART4, UART5 */
    CRU(CLKGATE(0)) = 0x19001900u;         /* SFC, TIMER0, TIMER1 */
    CRU(CLKGATE(1)) = 0xCC90CC90u;         /* I2S0 src, EBC dclk, SPI0, SPI1, UART2, TEST */
    CRU(CLKGATE(2)) = 0x00070007u;         /* UART3, UART4, UART5 */
    CRU(CLKGATE(5)) = 0x04000400u;         /* PVTM */
    CRU(CLKGATE(6)) = 0xE000E000u;         /* HCLK_EBC, HCLK_SYNTH, HCLK_SFC */
    CRU(CLKGATE(7)) = 0x40004000u;         /* HCLK_IMDCT */
    CRU(CLKGATE(8)) = 0x7CF27CF2u;         /* PCLK I2S0, I2C1, I2C2, SPI0, SPI1, UART2-5, TIMER */
}

/* ---- the operating point switch ----------------------------------------- */

static uint64_t s_hold_us;
static uint32_t s_hold_ms, s_hold_tmr, s_vop_timing;

/* The time base carried across a stretch that began at base (the 24 MHz timer read t0 then). */
static void clock_hold_end_at(uint64_t base_us, uint32_t base_ms, uint32_t t0)
{
    uint32_t us = clock_timer_ticks(t0, clock_timer_now()) / OSC_TICKS_PER_US;

    s_base_us = base_us + us;
    s_base_ms = base_ms + us / 1000u;
    s_last_us = s_base_us;
    s_last_ms = s_base_ms;
    systick_start();
    SCB_ICSR = SCB_ICSR_PENDSTCLR;           /* a tick that came due meanwhile: counted in the base */
}

void clock_hold_begin(void)
{
    clock_timer_open();
    s_hold_us = time_us();
    s_hold_ms = time_ms();
    s_hold_tmr = clock_timer_now();
}

void clock_hold_end(void)
{
    clock_hold_end_at(s_hold_us, s_hold_ms, s_hold_tmr);
}

static uint32_t irqs_off(void)
{
    uint32_t m;

    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r" (m) :: "memory");
    return m;
}

static void irqs_restore(uint32_t m)
{
    __asm__ volatile ("msr primask, %0" :: "r" (m) : "memory");
}

/* stock's "short delay" after the divider writes (0x030523E4 / 0x030523F4): on the crystal */
static void settle(void)
{
    for (volatile unsigned n = 0; n < 24u; n++)
        ;
}

/* Cores on the crystal: program the PLL for p (stock SetPllFreq steps 2-6), then the memory and VOP
 * timing for its clocks (steps 8-10). */
static void pll_program(const run_point_t *p)
{
    CRU(APLL_CON1) = 0x80008000u;            /* reset select, */
    CRU(APLL_CON1) = 0x40004000u;            /* power down, */
    CRU(APLL_CON2) = 0x08000000u;            /* FOUTVCO/DAC up (FOUT4PHASE stays down) */
    CRU(APLL_CON0) = 0xFFFF0000u | p->con0;
    CRU(APLL_CON1) = 0x11FF0000u | p->con1;
    CRU(APLL_CON2) = 0x08000000u | p->frac;  /* (CON2 & 0x0F000000) | FRAC, as stock */
    CRU(APLL_CON1) = 0x40000000u;            /* power up */
    mem_timing(p->pll_hz / (p == &k_run[CLOCK_POINT_UI] ? s_ui_a_div : p->a_div), p->pll_hz / p->b_div);
    s_vop_timing = clock_vop_hook(p->pll_hz / (p == &k_run[CLOCK_POINT_UI] ? s_ui_a_div : p->a_div));
}

/* The lock, polled on the 24 MHz timer for up to 10 ms [inferred: stock allows 240000 polls with a
 * delay each]; the channels in run go on meanwhile. 1 = locked; *us = the time it took. */
static int pll_lock(uint32_t run, uint32_t *us, uint32_t *off_us, uint32_t *t_susp)
{
    uint32_t t0, t = 0;
    int ok = 0;

    dma_resume(run);
    t0 = clock_timer_now();
    if (run)
        *off_us = clock_timer_ticks(*t_susp, t0) / OSC_TICKS_PER_US;
    while ((t = clock_timer_ticks(t0, clock_timer_now())) < 10000u * OSC_TICKS_PER_US)
        if (CRU(APLL_CON1) & APLL_LOCK) {
            ok = 1;
            break;
        }
    *us = t / OSC_TICKS_PER_US;
    *t_susp = clock_timer_now();
    dma_suspend(run);
    return ok;
}

int clock_set_point(unsigned point, uint32_t keep_dma, clock_switch_t *rec)
{
    const run_point_t *to = &k_run[point], *from = &k_run[s_point];
    uint32_t m, t0, t_susp, susp, run, a_div, sel0, off1 = 0, off2 = 0, lock_us = 0;
    uint64_t base_us;
    uint32_t base_ms;
    int locked;

    if (rec) {
        rec->from = (uint8_t)s_point;
        rec->to = (uint8_t)point;
        rec->locked = 1;
        rec->back = 0;
        rec->total_us = rec->lock_us = rec->dma_off_us = rec->dma_mask = 0;
    }
    if (point >= CLOCK_POINTS || point == s_point || !s_ui_ok)
        return 0;
    if (to->pll_hz / to->a_div > CLOCK_A_MAX_HZ || to->pll_hz / to->b_div > CLOCK_B_MAX_HZ)
        return CLOCK_SW_BAD;                 /* (a table error: never programmed) */
    clock_timer_open();
    m = irqs_off();
    base_us = time_us();
    base_ms = time_ms();
    t0 = clock_timer_now();
    /* stock FreqSetPrevProcess 0x03068678: DmaSuspend(0..5) (the IRQs off and core B held: here and
     * the caller) */
    susp = dma_suspend(0x3Fu);
    run = susp & keep_dma;
    t_susp = clock_timer_now();
    /* SetPllFreq 0x0305222C step 1: the cores onto the crystal, dividers /1, pclk /1 */
    CRU(MODE_CON) = 0x00010000u;
    CRU(CLKSEL0) = 0xFFFF0000u;
    CRU(CLKSEL1) = 0x03000000u;
    s_pll_hz = s_a_hz = s_b_hz = s_pclk_hz = OSC_HZ;
    s_pclk_shift = 0;
    pll_program(to);
    locked = pll_lock(run, &lock_us, &off1, &t_susp);
    if (!locked) {                           /* back to where we came from, the same way */
        pll_program(from);
        to = from;
        point = s_point;
        if (rec)
            rec->back = 1;
        if (!pll_lock(run, &lock_us, &off2, &t_susp)) {
            /* neither locks: the cores stay on the crystal (as clock_init_at's no-lock path) */
            s_ui_ok = 0;
            dma_resume(susp);
            clock_hold_end_at(base_us, base_ms, t0);
            irqs_restore(m);
            if (rec)
                rec->locked = 0;
            return CLOCK_SW_CRYSTAL;
        }
    }
    a_div = to == &k_run[CLOCK_POINT_UI] ? s_ui_a_div : to->a_div;
    sel0 = (uint32_t)(to->b_div - 1u) << 12 | (a_div - 1u);
    /* steps 14-16: dividers, pclk, the cores back on the PLL */
    CRU(CLKSEL0) = 0xFFFF0000u | sel0;
    settle();
    if (to == &k_run[CLOCK_POINT_UI]) {
        unsigned shift = 0;

        while (((to->pll_hz / a_div) >> shift) > 50000000u && shift < 3u)
            shift++;
        s_pclk_shift = shift;
    } else {
        s_pclk_shift = to->pclk_shift;
    }
    CRU(CLKSEL1) = 0x03000000u | s_pclk_shift << 8;
    settle();
    CRU(MODE_CON) = 0x00010001u;
    s_pll_hz = to->pll_hz;
    s_a_hz = to->pll_hz / a_div;
    s_b_hz = s_b_mem_hz = to->pll_hz / to->b_div;
    s_pclk_hz = s_a_hz >> s_pclk_shift;
    s_point = point;
    dma_resume(susp);
    if (rec) {
        rec->locked = (uint8_t)locked;
        rec->lock_us = lock_us;
        rec->dma_mask = susp;
        rec->dma_off_us = off1 > off2 ? off1 : off2;
        if (run && clock_timer_ticks(t_susp, clock_timer_now()) / OSC_TICKS_PER_US > rec->dma_off_us)
            rec->dma_off_us = clock_timer_ticks(t_susp, clock_timer_now()) / OSC_TICKS_PER_US;
        rec->total_us = clock_timer_ticks(t0, clock_timer_now()) / OSC_TICKS_PER_US;
    }
    clock_hold_end_at(base_us, base_ms, t0);
    irqs_restore(m);
    return locked ? CLOCK_SW_OK : CLOCK_SW_NOLOCK;
}

unsigned clock_point(void)
{
    return s_point;
}

const char *clock_point_name(unsigned point)
{
    return point < CLOCK_POINTS ? k_run[point].name : "?";
}

void clock_regs(uint32_t out[30])
{
    out[0] = CRU(APLL_CON0);
    out[1] = CRU(APLL_CON1);
    out[2] = CRU(APLL_CON2);
    out[3] = CRU(MODE_CON);
    for (unsigned i = 0; i < 13u; i++)
        out[4u + i] = CRU(0x14u + 4u * i);
    for (unsigned i = 0; i < 10u; i++)
        out[17u + i] = CRU(CLKGATE(i));
    out[27] = GRF_INTER_CON0;
    out[28] = s_vop_timing;                  /* VOP TIMING the hook wrote (0: the VOP unused) */
    out[29] = s_point;
}

__attribute__((weak)) uint32_t clock_vop_hook(uint32_t hz)
{
    (void)hz;
    return 0;
}
