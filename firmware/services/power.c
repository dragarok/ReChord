/*
 * power.c -- screen, backlight, core-A clock, battery, idle timers (power.h).
 */
#include <stdio.h>

#include "power.h"
#include "config.h"
#include "log.h"
#include "drivers/board.h"
#include "drivers/clock.h"
#include "drivers/panel_io.h"
#include "drivers/usb_msc.h"
#include "drivers/usb_out.h"

#define SAMPLE_MS   10000u           /* a battery reading every 10 s */
#define RING        181u             /* 30 min of readings on battery */
#define LOW_MV      3450u            /* stock's runtime cut-off (0 %) */
#define LOW_COUNT   3u               /* readings in a row below it */
#define SANE_MV     2800u            /* below: a bad reading, not the cell */

static const uint8_t k_duty[8] = { 99, 98, 96, 93, 88, 75, 50, 100 };
static const uint16_t k_level_mv[4] = { 3480, 3550, 3650, 3830 };
static const uint16_t k_pct_mv[11] = { 3450, 3580, 3660, 3710, 3750, 3800, 3850, 3920, 4000, 4100, 4200 };
static const char *const k_state[] = { "UI", "DARK", "IDLE", "USB", "MAX", "DAC", "USBOUT" };
enum { ST_UI, ST_DARK, ST_IDLE, ST_USB, ST_MAX, ST_DAC, ST_USBOUT };

static power_battery_t s_bat;
static uint16_t s_ring[RING];
static unsigned s_ring_n, s_ring_at, s_low, s_samples;
static uint32_t s_key_ms, s_idle_ms, s_sample_ms;
static int s_screen = 1, s_state = -1;

static unsigned pct10(unsigned mv)
{
    unsigned i = 1;

    if (mv < 3451u)
        return 0;
    if (mv >= 4200u)
        return 1000;
    while (mv > k_pct_mv[i])
        i++;
    return (i - 1u) * 100u + (mv - k_pct_mv[i - 1u]) * 100u / (k_pct_mv[i] - k_pct_mv[i - 1u]);
}

static unsigned level(unsigned mv)
{
    unsigned l = 0;

    while (l < 4u && mv >= k_level_mv[l])
        l++;
    return l;
}

/* mean of the three readings ending k readings back */
static unsigned avg3(unsigned k)
{
    unsigned s = 0;

    for (unsigned j = 0; j < 3u; j++)
        s += s_ring[(s_ring_at + RING - 1u - k - j) % RING];
    return s / 3u;
}

static void rates(void)
{
    static const uint8_t k_win[3] = { 6, 30, 177 };
    unsigned n = s_ring_n, now = avg3(0);
    int best = 0;

    s_bat.rate_ok = n >= 9u;
    s_bat.left_min = 0;
    s_bat.mv_min = 0;
    if (s_bat.rate_ok) {
        unsigned k = n - 3u < 30u ? n - 3u : 30u;

        s_bat.mv_min = (int16_t)(((int)now - (int)avg3(k)) * 6 / (int)k);
    }
    for (unsigned w = 0; w < 3u; w++) {
        unsigned k = k_win[w];

        s_bat.pct_h10[w] = 0;
        if (n >= k + 3u) {
            s_bat.pct_h10[w] = (int16_t)(((int)pct10(now) - (int)pct10(avg3(k))) * 360 / (int)k);
            best = s_bat.pct_h10[w];
        }
    }
    if (best < 0)
        s_bat.left_min = (uint16_t)((int)s_bat.pct10 * -60 / best);
}

/* One reading. Returns 1 when the cell is low enough to switch off. */
static int sample(void)
{
    int raw = adc_value(0);
    unsigned mv;

    if (raw < 0)
        return 0;                            /* no conversion yet */
    mv = (unsigned)raw * 5000u >> 10;
    s_samples++;
    if (mv < SANE_MV) {
        log_printf("bat: implausible reading %u mV (raw %d), ignored", mv, raw);
        return 0;
    }
    s_bat.mv = (uint16_t)mv;
    s_bat.pct10 = (uint16_t)pct10(mv);
    s_bat.level = (uint8_t)level(mv);
    s_bat.full = s_bat.vbus && board_charge_done();
    if (s_bat.vbus) {
        s_ring_n = 0;                        /* rates are for battery time only */
        s_bat.rate_ok = 0;
        s_bat.left_min = 0;
        s_low = 0;
        return 0;
    }
    s_low = mv < LOW_MV ? s_low + 1u : 0u;
    if (s_samples <= LOW_COUNT)
        return s_low >= LOW_COUNT;           /* the quick boot readings stay out of the rates */
    s_ring[s_ring_at] = (uint16_t)mv;
    s_ring_at = (s_ring_at + 1u) % RING;
    if (s_ring_n < RING)
        s_ring_n++;
    rates();
    if (s_ring_n % 6u == 0)
        log_printf("bat: %u mV on battery, %u.%u %%, %d mV/min, %d.%u %%/h (5 min), left %u min", mv,
                   s_bat.pct10 / 10u, s_bat.pct10 % 10u, s_bat.mv_min, s_bat.pct_h10[1] / 10,
                   (unsigned)(s_bat.pct_h10[1] < 0 ? -s_bat.pct_h10[1] : s_bat.pct_h10[1]) % 10u,
                   s_bat.left_min);
    return s_low >= LOW_COUNT;
}

static void set_state(int st)
{
    if (st == s_state)
        return;
    s_state = st;
    if (st == ST_DAC) {                      /* RECHORD.CFG dac_mhz: 50 (default), 100, 200 */
        uint32_t mhz = g_cfg.dac_mhz ? g_cfg.dac_mhz : 50u, div = clock_pll_hz() / (mhz * 1000000u);

        clock_set_core_a_div(div < 2u ? 2u : div > 8u ? 8u : div);
    } else if (st == ST_USBOUT) {             /* USB-C earphones: one clock for the whole session, */
        clock_set_core_a_div(4u);            /* 100 MHz (the bus never changes under a stream) */
    } else {
        clock_set_core_a_div(st == ST_DARK || st == ST_IDLE ? 8u : 2u);
    }
    log_printf("power: %s, A %u MHz", k_state[st], (unsigned)(clock_core_hz() / 1000000u));
}

void power_init(void)
{
    s_key_ms = s_idle_ms = s_sample_ms = time_ms();
    s_bat.vbus = (uint8_t)usb_cable();
    backlight_set(k_duty[(g_cfg.brightness - 1u) % 8u]);
}

void power_activity(void)
{
    s_key_ms = s_idle_ms = time_ms();
}

int power_screen_on(void)
{
    return s_screen;
}

void power_screen(int on)
{
    if (on == s_screen)
        return;
    s_screen = on;
    if (on) {
        set_state(g_cfg.cpu ? ST_MAX : ST_UI);   /* the clock first: the PWM period follows pclk */
        backlight_set(k_duty[(g_cfg.brightness - 1u) % 8u]);
        log_printf("ui: screen on");
    } else {
        backlight_set(0);
        log_printf("ui: screen off");
    }
}

void power_brightness_changed(void)
{
    if (s_screen)
        backlight_set(k_duty[(g_cfg.brightness - 1u) % 8u]);
}

static int s_supply_off;             /* the USB DAC on the battery: VBUS is in but feeds nothing */

void power_usb_supply_off(int off)
{
    s_supply_off = off;
}

int power_poll(int playing, int usb_session)
{
    uint32_t now = time_ms(), screen_s = config_screen_off_s(), off_min = config_auto_off_min();
    int vbus = usb_cable() && !s_supply_off, low = 0;   /* (vbus: USB power feeds the player) */

    if (playing)
        s_idle_ms = now;
    if (vbus != s_bat.vbus) {                /* cable in or out: read now */
        s_bat.vbus = (uint8_t)vbus;
        s_sample_ms = now - SAMPLE_MS;
        log_printf("bat: cable %s", vbus ? "in" : usb_cable() ? "in, not drawn from (battery)" : "out");
    }
    /* at boot three quick readings (the ADC refreshes channel 0 every 120 ms), then every 10 s */
    if (now - s_sample_ms >= (s_samples < LOW_COUNT ? 150u : SAMPLE_MS)) {
        s_sample_ms = now;
        low = sample();
    }
    if (s_screen && screen_s && now - s_key_ms >= screen_s * 1000u)
        power_screen(0);
    set_state(usb_session == POWER_SESSION_DAC ? ST_DAC : usb_session ? ST_USB : usb_out_active() ? ST_USBOUT :
              g_cfg.cpu ? ST_MAX :
              s_screen ? ST_UI : playing ? ST_DARK : ST_IDLE);
    if (low) {
        log_printf("bat: %u mV, below %u mV %u times: power off", s_bat.mv, LOW_MV, LOW_COUNT);
        return POWER_OFF_BATTERY;
    }
    if (off_min && !playing && !vbus && now - s_idle_ms >= off_min * 60000u) {
        log_printf("ui: auto power off after %u min idle", (unsigned)off_min);
        return POWER_OFF_IDLE;
    }
    return POWER_NONE;
}

const power_battery_t *power_battery(void)
{
    return &s_bat;
}

const char *power_battery_text(void)
{
    static char buf[16];

    if (!s_bat.mv)
        return "...";
    if (s_bat.vbus)
        snprintf(buf, sizeof buf, "%u.%02uV %s", s_bat.mv / 1000u, s_bat.mv % 1000u / 10u,
                 s_bat.full ? "full" : "chg");
    else
        snprintf(buf, sizeof buf, "%u%% %u.%02uV", s_bat.pct10 / 10u, s_bat.mv / 1000u, s_bat.mv % 1000u / 10u);
    return buf;
}
