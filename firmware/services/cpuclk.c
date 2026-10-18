/*
 * cpuclk.c -- core B's clock while playing (cpuclk.h; docs/CPU_CLOCK.md).
 *
 * Contexts: cpuclk_point_for(), cpuclk_arm(), cpuclk_poll() and cpuclk_boot() run in the main loop;
 * cpuclk_may() and cpuclk_switch() run from core A's SysTick (audio.c audio_tick: core B asked at
 * a track open, so a main loop held up by the storage never holds up the switch) or from the main
 * loop (audio.c: back to the UI point at a stop). Nothing here logs or touches the eMMC from the
 * SysTick: the switch leaves a record, the main loop writes it out.
 */
#include <stdio.h>
#include <string.h>

#include "cpuclk.h"
#include "bootcrumb.h"
#include "config.h"
#include "log.h"
#include "watchdog.h"
#include "audio/selftest_kernel.h"
#include "drivers/clock.h"
#include "drivers/panel_io.h"

#define REG(a)       (*(volatile uint32_t *)(a))
#define SYST_CSR     REG(0xE000E010u)
#define SYST_RVR     REG(0xE000E014u)
#define SYST_CVR     REG(0xE000E018u)
#define CRU_CON1     REG(0x40180004u)
#define PLL_LOCK     (1u << 10)
#define I2S_DMA_CH   5u                    /* audio.c DMA_CH: runs on while the PLL locks */

/* The record (bootcrumb sector 1 words 6, 7): status = 0xC1 << 24 | failed stage << 16 | boots that
 * passed << 8 | boots that tried; armed = 0xA1000000 | ST_ARMED while a switch and its self-test are
 * in flight (a boot that ends then is found by the next: ST_DIED). */
#define REC_TAG      0xC1000000u
#define REC_ARMED    (0xA1000000u | ST_ARMED)

static uint32_t s_rec;                     /* the status word as now */
static int s_armed;                        /* this boot: the record says "in flight", the point may be used */
static int s_admitted;                     /* this boot: the self-test passed */
static unsigned s_failed;                  /* the stage the point failed at (this boot or the record), 0 = none */
static int s_arm_logged;
static uint32_t s_wait_ms;                 /* when a switch first had to wait (cpuclk_may) */
static uint32_t s_a_code_ref, s_a_code_words, s_b_code_ref;

/* what the SysTick side left for the main loop */
static volatile uint32_t s_sw_seq, s_sw_logged;
static clock_switch_t s_sw;
static uint32_t s_sw_regs[30];
static volatile int s_persist;             /* 1 passed, 2 failed: the record to write */
static volatile uint32_t s_retime;         /* switches whose pclk users are not re-timed yet */
static volatile uint32_t s_deny_seq, s_deny_logged;
static volatile unsigned s_deny_why;
static volatile int s_deny_gate;           /* the clock (SDK gate id) that was too fast */

/* the self-test's numbers, for its log line */
static struct {
    uint32_t us, a_cyc, a_ticks, a_sig, a_march, a_code, unlock;
    uint32_t b_cyc, b_ticks, b_sig, b_high, b_high_kib, b_low, b_code, b_unlock;
    unsigned stage;
    volatile int done;
} s_st;

static const char *const k_stage[ST_N] = {
    "ok", "armed", "core B did not hold", "the PLL did not lock", "core A's clock", "core B's clock",
    "the PLL lost lock", "core A's datapath", "core B's datapath", "core A's LOW RAM",
    "core B's HIGH RAM", "core B's LOW RAM", "core A's code", "core B's code",
    "core B did not finish", "core B did not come back", "a boot died in the test",
};

static const char *const k_why[] = {
    "", "cpu_b=96", "an earlier failure", "no boot trail", "the watchdog is not running",
    "a PLL-fed peripheral clock would be too fast", "USB output",
};

/* ---- which point ------------------------------------------------------------ */

/* Core-B QEMU instructions per output frame, the worst measured file of each format, software only
 * (a HIFI function can be off after its own self-test, docs/HIFI.md): ReChord docs/HIFI_PLAN.md
 * section 1 ("After" where measured) and docs/HIFI.md 6.2 (APE, ALAC: hifi=off). */
static uint32_t insn_per_frame(const as_track_t *t, uint32_t extra)
{
    static const uint16_t k_ape[5] = { 394, 616, 920, 2390, 10080 };   /* fast .. insane, 16/44.1 */

    switch (t->codec) {
    case AS_CODEC_FLAC:      return t->bits > 16u ? 163u : 137u;   /* 24/96, 16/44.1 software alone */
    case AS_CODEC_WAV: case AS_CODEC_AIFF: case AS_CODEC_AIFF_LE: case AS_CODEC_WAVF:
                             return 80u;                           /* before the PCM fix: the most */
    case AS_CODEC_MP3:       return 665u;                          /* 320 CBR, dense */
    case AS_CODEC_AAC:       return 1260u;                         /* HE-AAC v2 (the profile is not known here) */
    case AS_CODEC_ALAC:      return t->bits > 16u ? 424u : 332u;
    case AS_CODEC_VORBIS:    return 445u;                          /* q10, dense */
    case AS_CODEC_OPUS:      return 709u;                          /* 256k, dense */
    case AS_CODEC_WAVPACK:   return 595u;                          /* -hh, hybrid, float */
    case AS_CODEC_DSD:       return extra >= 4u ? 2014u : extra >= 2u ? 872u : 486u;   /* a 176.4 k (DSD256: 88.2 k) frame */
    case AS_CODEC_APE:
        if (extra >= 1000u && extra <= 5000u)
            return (uint32_t)k_ape[extra / 1000u - 1u] * (t->bits > 16u ? 104u : 100u) / 100u;
        return 10080u;
    case AS_CODEC_WMA:       return 260u;
    case AS_CODEC_WMAPRO:    return t->rate > 48000u ? 207u : 243u;
    case AS_CODEC_WMALOSSLESS: return t->bits > 16u ? 552u : 2520u;
    default:                 return 2520u;                          /* not measured: as the heaviest */
    }
}

unsigned cpuclk_point_for(const as_track_t *t, uint32_t extra, int usb_out)
{
    uint64_t cyc;

    if (g_cfg.cpu_b == CFG_CPU_B_96 || s_failed || usb_out)
        return CLOCK_POINT_UI;
    if (g_cfg.cpu_b == CFG_CPU_B_STOCK)
        return CLOCK_POINT_FLAC;
    /* device cycles a second at the UI point, against 60 % of 96 MHz (b_main.c choose_clock's line) */
    cyc = (uint64_t)insn_per_frame(t, extra) * t->rate * CPUCLK_DEVICE_CPI_X100 / 100u;
    return cyc > 96000000ull * 60u / 100u ? CLOCK_POINT_FLAC : CLOCK_POINT_UI;
}

/* ---- the record --------------------------------------------------------------- */

static unsigned rec_failed(uint32_t r) { return (r & 0xFF000000u) == REC_TAG ? (r >> 16) & 0xFFu : 0u; }
static unsigned rec_passed(uint32_t r) { return (r & 0xFF000000u) == REC_TAG ? (r >> 8) & 0xFFu : 0u; }
static unsigned rec_tried(uint32_t r)  { return (r & 0xFF000000u) == REC_TAG ? r & 0xFFu : 0u; }

static uint32_t rec_make(unsigned failed, unsigned passed, unsigned tried)
{
    return REC_TAG | (failed & 0xFFu) << 16 | (passed > 255u ? 255u : passed) << 8 | (tried > 255u ? 255u : tried);
}

void cpuclk_boot(void)
{
    uint32_t st = bootcrumb_hp(0), armed = bootcrumb_hp(1);

    s_rec = (st & 0xFF000000u) == REC_TAG ? st : rec_make(0, 0, 0);
    if (armed == REC_ARMED) {                /* the last boot ended inside a switch or its self-test */
        s_rec = rec_make(ST_DIED, rec_passed(s_rec), rec_tried(s_rec));
        if (bootcrumb_hp_set(s_rec, 0) != 0)
            log_printf("cpuclk: the record could not be written");
    }
    s_failed = rec_failed(s_rec);
    log_printf("cpuclk: cpu_b=%s, FLAC point (PLL 1344, A 224, B 448 MHz): %u boots tried, %u passed%s%s%s",
               g_cfg.cpu_b == CFG_CPU_B_AUTO ? "auto" : g_cfg.cpu_b == CFG_CPU_B_STOCK ? "stock" : "96",
               rec_tried(s_rec), rec_passed(s_rec), s_failed ? "; failed at " : "",
               s_failed ? k_stage[s_failed < ST_N ? s_failed : 0] : "",
               s_failed ? ": off for this build (the UI point, 96 MHz)" : "");
    if (s_failed)
        log_printf("cpuclk: failed at %u", s_failed);   /* (the number, for the tools) */
}

int cpuclk_arm(void)
{
    extern const uint32_t _data_load_start[];
    unsigned why = 0;

    if (s_armed)
        return 0;
    if (g_cfg.cpu_b == CFG_CPU_B_96)
        why = 1;
    else if (s_failed)
        why = 2;
    else if (!bootcrumb_ok())
        why = 3;
    if (!why) {
        s_a_code_words = (uint32_t)(_data_load_start - (const uint32_t *)0x03050000u);
        s_a_code_ref = st_sum((const volatile uint32_t *)0x03050000u, s_a_code_words);
        s_rec = rec_make(0, rec_passed(s_rec), rec_tried(s_rec) + 1u);
        if (bootcrumb_hp_set(s_rec, REC_ARMED) != 0)
            why = 3;
    }
    if (why) {
        if (!s_arm_logged)
            log_printf("cpuclk: FLAC point not armed: %s", k_why[why]);
        s_arm_logged = 1;
        return -1;
    }
    s_armed = 1;
    log_printf("cpuclk: FLAC point armed (in flight on the eMMC until its self-test is over)");
    return 0;
}

int cpuclk_admitted(void)
{
    return s_admitted;
}

void cpuclk_b_loaded(uint32_t n)
{
    AS->st_code_words = n / 4u;
    s_b_code_ref = st_sum((const volatile uint32_t *)AS_B_RAM, n / 4u);
}

/* ---- may we --------------------------------------------------------------------- */

static void deny(unsigned why)
{
    s_deny_why = why;
    s_deny_seq++;
}

int cpuclk_may(unsigned want)
{
    if (want == CLOCK_POINT_UI || want >= CLOCK_POINTS)
        return want == CLOCK_POINT_UI;
    if (g_cfg.cpu_b == CFG_CPU_B_96 || s_failed || !s_armed || !bootcrumb_ok()) {
        deny(g_cfg.cpu_b == CFG_CPU_B_96 ? 1u : s_failed ? 2u : 3u);
        return 0;
    }
    if (!watchdog_running()) {               /* (a hang in the switch must end in a reset) */
        uint32_t now = time_ms();

        if (!s_wait_ms)
            s_wait_ms = time_stamp(now);
        if (now - s_wait_ms < 3000u)
            return -1;
        deny(4);
        return 0;
    }
    s_wait_ms = 0;
    if ((s_deny_gate = clock_periph_check(1344000000u)) >= 0) {
        deny(5);
        return 0;
    }
    return 1;
}

/* ---- the self-test ------------------------------------------------------------- */

static int lock_ok(void)
{
    return (CRU_CON1 & PLL_LOCK) != 0;
}

/* Core A's clock against the 24 MHz timer: its SysTick's cycles over 4 ms, within one reload. */
static void measure_a(uint32_t *cyc, uint32_t *ticks)
{
    uint32_t m, c0, c1, t0, need = clock_core_hz() / 250u + clock_core_hz() / 2000u;

    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r" (m) :: "memory");
    c0 = SYST_CVR;
    if (c0 < need) {                          /* too close to the reload: past it first */
        while (SYST_CVR <= c0)
            ;
        c0 = SYST_CVR;
    }
    t0 = clock_timer_now();
    while (clock_timer_ticks(t0, clock_timer_now()) < 96000u)
        ;
    c1 = SYST_CVR;
    *ticks = clock_timer_ticks(t0, clock_timer_now());
    *cyc = c0 > c1 ? c0 - c1 : 0u;
    __asm__ volatile ("msr primask, %0" :: "r" (m) : "memory");
}

/* within 0.5 %: |cyc * 24 MHz / ticks - hz| <= hz / 200 */
static int freq_ok(uint32_t cyc, uint32_t ticks, uint32_t hz)
{
    uint64_t meas = ticks ? (uint64_t)cyc * 24000000u / ticks : 0u;
    uint64_t d = meas > hz ? meas - hz : hz - meas;

    return ticks && d * 200u <= hz;
}

static int32_t ppm(uint32_t cyc, uint32_t ticks, uint32_t hz)
{
    int64_t meas = ticks ? (int64_t)((uint64_t)cyc * 24000000u / ticks) : 0;

    return hz ? (int32_t)((meas - (int64_t)hz) * 1000000 / (int64_t)hz) : 0;
}

/* Both halves at once (B started first, then A's half; B's verdicts read after both). */
static unsigned selftest(void)
{
    static volatile uint32_t ten = 10u;
    uint32_t buf[64], t0, t_start, unlock = 0;
    unsigned stage = ST_OK;

    t_start = clock_timer_now();
    AS->st_b_state = 0;
    AS->st_b_unlock = 0;
    __asm__ volatile ("dmb" ::: "memory");
    AS->cmd = AS_CMD_SELFTEST;
    as_ping_b();
    unlock += !lock_ok();
    measure_a(&s_st.a_cyc, &s_st.a_ticks);
    unlock += !lock_ok();
    s_st.a_sig = st_kernel(ST_KERNEL_SEED, ST_KERNEL_ITERS, buf, &ten);
    unlock += !lock_ok();
    s_st.a_march = st_march2((volatile uint32_t *)ST_LOW_A, ST_LOW_BYTES / 4u);
    unlock += !lock_ok();
    s_st.a_code = st_sum((const volatile uint32_t *)0x03050000u, s_a_code_words);
    unlock += !lock_ok();
    /* B's half: up to 300 ms on the 24 MHz timer (A's own SysTick is not counting meanwhile) */
    for (t0 = clock_timer_now(); AS->st_b_state != 2u || AS->cmd != AS_CMD_NONE; )
        if (clock_timer_ticks(t0, clock_timer_now()) > 300000u * 24u)
            break;
    s_st.unlock = unlock;
    s_st.b_cyc = AS->st_b_cyc;
    s_st.b_ticks = AS->st_b_ticks;
    s_st.b_sig = AS->st_b_sig;
    s_st.b_high = AS->st_b_high;
    s_st.b_high_kib = AS->st_b_high_words / 256u;
    s_st.b_low = AS->st_b_low;
    s_st.b_code = AS->st_b_code;
    s_st.b_unlock = AS->st_b_unlock;
    if (AS->st_b_state != 2u)
        stage = ST_B_TIMEOUT;
    else if (!freq_ok(s_st.a_cyc, s_st.a_ticks, clock_core_hz()))
        stage = ST_A_FREQ;
    else if (!freq_ok(s_st.b_cyc, s_st.b_ticks, clock_core_b_hz()))
        stage = ST_B_FREQ;
    else if (unlock || s_st.b_unlock || !lock_ok())
        stage = ST_UNLOCK;
    else if (s_st.a_sig != ST_KERNEL_SIG)
        stage = ST_A_KERNEL;
    else if (s_st.b_sig != ST_KERNEL_SIG)
        stage = ST_B_KERNEL;
    else if (s_st.a_march)
        stage = ST_A_MARCH;
    else if (s_st.b_high)
        stage = ST_B_MARCH_HIGH;
    else if (s_st.b_low)
        stage = ST_B_MARCH_LOW;
    else if (s_st.a_code != s_a_code_ref)
        stage = ST_A_CODE;
    else if (s_st.b_code != s_b_code_ref || !AS->st_code_words)
        stage = ST_B_CODE;
    s_st.us = clock_timer_ticks(t_start, clock_timer_now()) / 24u;
    s_st.stage = stage;
    s_st.done = 1;
    return stage;
}

/* ---- the switch ------------------------------------------------------------------ */

static void sw_record(void)
{
    clock_regs(s_sw_regs);
    s_sw_seq++;
    s_retime++;
}

int cpuclk_switch(unsigned want, int b_held)
{
    int r, test = want == CLOCK_POINT_FLAC && !s_admitted;
    unsigned stage = ST_OK;

    if (want >= CLOCK_POINTS || want == clock_point())
        return ST_OK;
    if (test && !b_held)
        return ST_PARK;                        /* the first one needs core B for its half */
    if (test)
        clock_hold_begin();                    /* (the self-test keeps core A in here for a while) */
    r = clock_set_point(want, 1u << I2S_DMA_CH, &s_sw);
    sw_record();
    if (r != CLOCK_SW_OK) {
        stage = ST_LOCK;
    } else if (test) {
        stage = selftest();
        if (stage != ST_OK) {
            clock_set_point(CLOCK_POINT_UI, 1u << I2S_DMA_CH, &s_sw);
            sw_record();
        }
    }
    if (test)
        clock_hold_end();
    clock_timer_close();                       /* (stock's boot state: TIMER0 gated) */
    if (stage == ST_OK && test) {
        s_admitted = 1;
        s_persist = 1;
    } else if (stage != ST_OK) {
        cpuclk_fail(stage);
    }
    return stage;
}

void cpuclk_fail(unsigned stage)
{
    if (!s_failed)
        s_failed = stage;
    s_persist = 2;
}

/* ---- the main loop's side ------------------------------------------------------------- */

static void log_switch(void)
{
    const uint32_t *g = s_sw_regs;

    log_printf("cpuclk: switch %s -> %s%s in %u us (lock %u us%s), DMA %02x suspended, I2S DMA off at most "
               "%u us; A %u B %u pclk %u MHz", clock_point_name(s_sw.from), clock_point_name(s_sw.to),
               s_sw.back ? " (did not lock: back)" : "", (unsigned)s_sw.total_us, (unsigned)s_sw.lock_us,
               s_sw.locked ? "" : ", NO LOCK", (unsigned)s_sw.dma_mask, (unsigned)s_sw.dma_off_us,
               (unsigned)(clock_core_hz() / 1000000u), (unsigned)(clock_core_b_hz() / 1000000u),
               (unsigned)(clock_pclk_hz() / 1000000u));
    log_printf("cpuclk: pll %08x %08x %08x mode %08x grf %08x vop %08x point %u", (unsigned)g[0],
               (unsigned)g[1], (unsigned)g[2], (unsigned)g[3], (unsigned)g[27], (unsigned)g[28], (unsigned)g[29]);
    log_printf("cpuclk: sel %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)g[4],
               (unsigned)g[5], (unsigned)g[6], (unsigned)g[7], (unsigned)g[8], (unsigned)g[9], (unsigned)g[10],
               (unsigned)g[11], (unsigned)g[12], (unsigned)g[13], (unsigned)g[14], (unsigned)g[15], (unsigned)g[16]);
    log_printf("cpuclk: gate %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x", (unsigned)g[17], (unsigned)g[18],
               (unsigned)g[19], (unsigned)g[20], (unsigned)g[21], (unsigned)g[22], (unsigned)g[23], (unsigned)g[24],
               (unsigned)g[25], (unsigned)g[26]);
}

static void log_selftest(void)
{
    uint32_t a_hz = clock_point() == CLOCK_POINT_FLAC ? clock_core_hz() : 224000000u;
    uint32_t b_hz = clock_point() == CLOCK_POINT_FLAC ? clock_core_b_hz() : 448000000u;
    uint32_t a_meas = s_st.a_ticks ? (uint32_t)((uint64_t)s_st.a_cyc * 24000u / s_st.a_ticks) : 0u;
    uint32_t b_meas = s_st.b_ticks ? (uint32_t)((uint64_t)s_st.b_cyc * 24000u / s_st.b_ticks) : 0u;

    log_printf("cpuclk: self-test %s%s%s in %u us: A %u.%03u MHz (%d ppm), B %u.%03u MHz (%d ppm), lock lost %u/%u",
               s_st.stage ? "FAILED at " : "passed", s_st.stage ? k_stage[s_st.stage < ST_N ? s_st.stage : 0] : "",
               s_st.stage ? "" : "", (unsigned)s_st.us, (unsigned)(a_meas / 1000u), (unsigned)(a_meas % 1000u),
               (int)ppm(s_st.a_cyc, s_st.a_ticks, a_hz), (unsigned)(b_meas / 1000u), (unsigned)(b_meas % 1000u),
               (int)ppm(s_st.b_cyc, s_st.b_ticks, b_hz), (unsigned)s_st.unlock, (unsigned)s_st.b_unlock);
    log_printf("cpuclk: self-test kernel %08x/%08x (want %08x), march A %x, B high %x (%u KiB), B low %x, "
               "code A %08x/%08x B %08x/%08x", (unsigned)s_st.a_sig, (unsigned)s_st.b_sig, (unsigned)ST_KERNEL_SIG,
               (unsigned)s_st.a_march, (unsigned)s_st.b_high, (unsigned)s_st.b_high_kib, (unsigned)s_st.b_low,
               (unsigned)s_st.a_code, (unsigned)s_a_code_ref, (unsigned)s_st.b_code, (unsigned)s_b_code_ref);
    if (s_st.stage)
        log_printf("cpuclk: failed at %u: back at the UI point (96 MHz), off for this build", s_st.stage);
}

void cpuclk_poll(void)
{
    uint32_t seq = s_sw_seq;

    if (s_st.done) {
        s_st.done = 0;
        log_selftest();
    }
    if (seq != s_sw_logged) {
        s_sw_logged = seq;
        log_switch();
    }
    if (s_deny_seq != s_deny_logged) {
        s_deny_logged = s_deny_seq;
        log_printf("cpuclk: FLAC point refused: %s%s", k_why[s_deny_why < 7u ? s_deny_why : 0],
                   s_deny_why == 5u ? (s_deny_gate == 16 ? " (I2S1)" : s_deny_gate == 29 ? " (UART1)" : " (see the gate id)") : "");
        if (s_deny_why == 5u)
            log_printf("cpuclk: the clock of SDK gate %d", s_deny_gate);
    }
    if (s_retime) {                            /* pclk's users that are not re-timed in the switch */
        s_retime = 0;
        backlight_retime();
        watchdog_retime();
    }
    if (s_persist) {
        int p = s_persist;

        s_persist = 0;
        if (p == 1)
            s_rec = rec_make(0, rec_passed(s_rec) + 1u, rec_tried(s_rec));
        else
            s_rec = rec_make(s_failed, rec_passed(s_rec), rec_tried(s_rec));
        if (bootcrumb_hp_set(s_rec, 0) != 0)
            log_printf("cpuclk: the record could not be written");
        if (p == 2 && s_st.stage == ST_OK)
            log_printf("cpuclk: failed at %u (%s): back at the UI point, off for this build", s_failed,
                       k_stage[s_failed < ST_N ? s_failed : 0]);
    }
}
