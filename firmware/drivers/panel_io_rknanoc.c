/*
 * panel_io_rknanoc.c -- TARGET panel_io backend for the RKNanoC (Cortex-M3).
 *
 * Clean-room implementation of the panel byte transport as FACTS
 * extracted from the reference firmware image (full trace in
 * docs/rewrite/drivers.md, "Transport"): the Echo Mini panel sits on a
 * GPIO-attached 8080-style 8-bit parallel bus that the reference
 * bit-bangs through the GPIO0 port registers -- it does NOT use the
 * SDK's VOP MCU interface at 0x60070000 (an earlier version of this
 * file assumed VOP from the SDK docs alone; the proven display path in
 * the reference writes GPIO0, so this file follows the reference's
 * hardware facts).
 *
 * Bus wiring (extracted from the reference's byte-write routine at
 * 0x03051c6c and its bring-up code):
 *   GPIO0.16..23  D0..D7   (byte value lives in bits 16..23 of GPIO0 DR)
 *   GPIO0.24      WR       (write strobe; the panel latches on the
 *                           rising edge of this bit)
 *   GPIO0.25      D/C      (0 = command byte, 1 = data/parameter byte)
 *   GPIO0.12      CS/RD-style control, held high outside ID probing
 *   GPIO2.15      panel RESET (active low pulse)
 *
 * One bus byte = three GPIO0 DR writes (exactly the extracted pattern:
 * two writes with WR low, one with WR high):
 *   DR = ctl | byte<<16 | D/C  with WR=0, again with WR=0, then WR=1.
 *
 * Register map (vendor SDK driver/hw_memap.h + reference usage):
 *   GPIO0_BASE 0x40160000, GPIO2_BASE 0x50030000,
 *   SWPORTA_DR +0x00, SWPORTA_DDR +0x04, EXT_PORTA +0x50.
 *
 * Link this file ONLY in target builds, in place of panel_io_sim.c.
 *
 * TODO (bring-up, see docs/rewrite/drivers.md "open items"):
 *   - panel_io_delay_ms(): the loop below is an uncalibrated placeholder
 *     (the reference uses a timer-based delay; that code is not decoded
 *     yet). Only affects init timing margins, not correctness.
 *   - CRU clock gating is assumed to be done by earlier boot stages
 *     (the reference's 0x030518d8 does a broad CRU/GRF setup not ported
 *     yet). Pinmux IS done here (panel_io_init).
 *   - The panel ID probe (GPIO0 bus read via EXT_PORTA) is not
 *     implemented; panel variant selection is compile-time (see
 *     panel_init_rknanoc.c).
 */

#include "services/watchdog.h"
#include <stdint.h>

#include "panel_io.h"
#include "clock.h"
#include "rknanoc.h"
#include "services/input.h"
#include "services/log.h"
#include "services/bootcrumb.h"
#include "display.h"

/* GPIO port register block (see file header). */
typedef struct {
    volatile uint32_t dr;          /* +0x00 SWPORTA_DR  (output data)   */
    volatile uint32_t ddr;         /* +0x04 SWPORTA_DDR (direction)     */
    volatile uint32_t unused[18];  /* +0x08 .. +0x4C                   */
    volatile uint32_t ext_porta;   /* +0x50 EXT_PORTA   (input data)    */
} gpio_regs_t;

#define GPIO0 ((gpio_regs_t *)0x40160000u)
#define GPIO2 ((gpio_regs_t *)0x50030000u)

/* Bus bit positions in GPIO0 DR (extracted facts). */
#define PIN_DATA_SHIFT 16
#define PIN_DATA_MASK  (0xFFu << PIN_DATA_SHIFT)  /* GPIO0.16..23 = D0..D7 */
#define PIN_WR         (1u << 24)                 /* GPIO0.24 = WR strobe */
#define PIN_DC         (1u << 25)                 /* GPIO0.25 = D/C       */
#define PIN_CS         (1u << 12)                 /* GPIO0.12 = CS/RD ctl */

/* GPIO0 pins the reference drives for the panel: 12, 15, 16..25
 * (0x03FF9000 = bits 12 + 15 + 16..25; exact mask extracted from the
 * reference's direction-register setup). */
#define PANEL_DDR_MASK 0x03FF9000u

/* GPIO2.15 = panel RESET (active low). */
#define PIN_RESET      (1u << 15)

/*
 * Static shadow of the GPIO0 DR control bits (WR high, CS high, data
 * pins low). The reference keeps the same shadow in RAM (at 0x03006030)
 * and ORs each outgoing byte into it, so unrelated GPIO0 pins keep
 * their state across panel writes.
 */
static uint32_t bus_ctl;

void panel_io_gpio0_pin(unsigned pin, int level)
{
    if (level)
        bus_ctl |= 1u << pin;
    else
        bus_ctl &= ~(1u << pin);
    if (level)
        GPIO0->dr |= 1u << pin;
    else
        GPIO0->dr &= ~(1u << pin);
}

static void bus_write(uint8_t byte, uint32_t dc_bit)
{
    uint32_t word = bus_ctl | ((uint32_t)byte << PIN_DATA_SHIFT) | dc_bit;

    GPIO0->dr = word & ~PIN_WR;   /* WR low  */
    GPIO0->dr = word & ~PIN_WR;   /* WR low  (pattern as extracted) */
    GPIO0->dr = word;             /* WR high: panel latches the byte */
}

/* GRF IOMUX (reference 0x03051458): 2-bit function per pin, 8 pins per
 * word, 0x20 per port, write-masked in the upper half. Function 0 = GPIO. */
static void iomux_gpio(unsigned port, unsigned pin)
{
    *(volatile uint32_t *)(0x50010000u + port * 0x20u + (pin >> 3) * 4u) =
        0x30000u << ((pin & 7u) * 2u);
}

/* Backlight (RECHO 0x03051dd8, docs/RECHO_POWER_CFG.md C.5): PWM0 at
 * 0x400F0000 (CNT, PERIOD, DUTY, CTRL) on GPIO2.4. duty <= 0: off (GPIO low);
 * >= 100: full on (GPIO high, no PWM); else PWM at 10 kHz, or (100 - duty) kHz
 * when that is lower, negative polarity (the light is on for 100 - duty %). */
#define PWM0 ((volatile uint32_t *)0x400F0000u)
void backlight_set(int duty)
{
    PWM0[3] = 0;
    iomux_gpio(2, 4);
    if (duty <= 0 || duty >= 100) {
        if (duty <= 0)
            GPIO2->dr &= ~(1u << 4);
        else
            GPIO2->dr |= 1u << 4;
        GPIO2->ddr |= 1u << 4;
        return;
    }
    {
        uint32_t khz = 100u - (uint32_t)duty < 10u ? 100u - (uint32_t)duty : 10u;
        uint32_t period = clock_pclk_hz() / (khz * 1000u);

        *(volatile uint32_t *)0x50010040u = 0x03000100u;   /* GPIO2.4: PWM0 */
        cru_gate(0x90, 1);
        cru_reset(0x17, 1);
        delay_ms(1);
        cru_reset(0x17, 0);
        PWM0[2] = period * (uint32_t)duty / 100u;
        PWM0[0] = 0;
        PWM0[1] = period;
        PWM0[3] = 0x00010003u;            /* enable, continuous, negative polarity */
    }
}

void backlight_on(void)
{
    backlight_set(100);
}

/* SAR-ADC at 0x400D0000: [0] data, [1] status (bit 0: converting), [2] ctrl = ch | 0x08 power |
 * 0x10 start | 0x20 interrupt enable | 0x40 done (interrupt status). One conversion per call
 * (once per main-loop pass): channel 1 (the key ladder) three times out of four, the fourth goes
 * round channels 0 (battery), 2, 3.
 *
 * Completion is detected the way stock's own polled conversions do it (BatteryInit, MINIV390 seg01
 * 0x0307a780..0x0307a7c0: start with the interrupt enable clear, wait 1 ms, then until STATUS bit 0
 * reads 0, at most 20 ms; it reads DATA either way). Those conversions pass on every stock boot of
 * the 512 MB unit on battery, so this is proven on that silicon. Waiting for CTRL bit 6 with bit 5
 * clear, as this driver did, is a combination no working firmware uses; on the device it never
 * completed and the keys read the initial 1023 forever (work/stock_boot/05_keys_adc.md). A
 * conversion can no longer wedge the scan: after 20 ms DATA is taken and the next one starts.
 * The counters say which signal the chip gave (adc_diag, logged once after boot). */
#define ADC ((volatile uint32_t *)0x400D0000u)
#define ADC_SETTLE_US  1000u                       /* stock: delay_ms(1) before STATUS */
#define ADC_TIMEOUT_US 20000u                      /* stock: at most 20 x 1 ms */
static int s_adc[4] = { -1, 1023, 1023, 1023 };
static unsigned s_adc_ch = 8u, s_adc_n;           /* channel converting; 8 = none yet */
static uint64_t s_adc_t0;
static uint32_t s_adc_diag[ADC_DIAG_N];

static uint64_t adc_now(void)
{
    static uint64_t fake;
    uint64_t t = time_us();

    return t ? t : (fake += 1000u);                /* before the time base: 1 ms per call */
}

/* The key ladder (channel 1) as stock decodes it (GetKey 0x03062362 and KeyScan 0x0306240a,
 * work/stock_boot/05_keys_adc.md 1.1-1.3): while a key is down the lowest reading since the last
 * release is decoded (a release's rising voltage cannot pass through a higher band), a reading in
 * a gap between bands (184..199, 410..449, 550..1000) keeps the previous key, and a band counts only
 * once two fresh samples at least 20 ms apart agree (stock: 2 scans 20 ms apart). The UI used to
 * see the last raw sample on every main-loop pass, so one sample taken while the voltage swept
 * through another key's band read as a press of that key (device 2026-10-16: a "select" and a
 * "chord" fired during DOWN presses, PLAY needed three presses). keys_adc() returns a value of the
 * stable band, which input_scan() maps again. */
#define KEY_STABLE_US 20000u
static int s_key_min = 0xFFFF, s_key_out = 1023, s_key_cand = -2, s_key_cand_v;
static uint64_t s_key_cand_t;

/* The edges, for timing (services/input.c input_sample_edges): when the reading left idle, and
 * when it rose KEY_RISE above the lowest of the press (the finger let go: the band itself is kept
 * until idle, the release's rising voltage cannot pass through a higher band). Which key it is
 * stays the stable band's business above. 0 = none. */
#define KEY_RISE 64
static uint32_t s_press_ms, s_release_ms;

static void key_edges(int v)
{
    uint32_t now = time_ms();

    now = now ? now : 1u;
    if (v > 1000) {
        if (s_press_ms && !s_release_ms)
            s_release_ms = now;
        s_press_ms = 0;
    } else if (!s_press_ms) {
        s_press_ms = now;
        s_release_ms = 0;
    } else if (s_key_min <= 1000 && v > s_key_min + KEY_RISE) {
        if (!s_release_ms)
            s_release_ms = now;
    } else if (s_key_min <= 1000 && v <= s_key_min + KEY_RISE / 2) {
        s_release_ms = 0;                       /* (back down: not a release after all) */
    }
}

static void key_sample(int v)
{
    int k;

    key_edges(v);
    if (v > 1000)
        s_key_min = 0xFFFF;                     /* released: the next press starts afresh */
    else {
        if (v < s_key_min)
            s_key_min = v;
        v = s_key_min;
    }
    k = input_map_adc(v);
    if (k == KEY_NONE && v <= 1000)
        return;                                 /* a gap: the previous key stays */
    if (k != s_key_cand) {
        s_key_cand = k;
        s_key_cand_v = v;
        s_key_cand_t = adc_now();
        return;
    }
    if (adc_now() - s_key_cand_t >= KEY_STABLE_US)
        s_key_out = s_key_cand_v;
}

/* One step of the converter: the conversion started last time collected (if done), the next one
 * started. From the main loop (keys_adc) until keys_tick() takes over, then every 10 ms from core
 * A's SysTick: channel 1 (the ladder) seven times in eight, the eighth goes round 0, 2, 3. */
static volatile int s_keys_isr;

static int adc_step(void)
{
    static const uint8_t k_other[3] = { 0, 2, 3 };
    unsigned ch;

    if (s_adc_ch < 8u) {
        uint64_t el = adc_now() - s_adc_t0;
        uint32_t stas = ADC[1], ctrl = ADC[2];

        if (el < ADC_SETTLE_US || ((stas & 1u) && el < ADC_TIMEOUT_US))
            return s_key_out;                     /* still converting: next call */
        s_adc[s_adc_ch & 3u] = (int)(ADC[0] & 0x3FFu);
        if (s_adc_ch == 1u)
            key_sample(s_adc[1]);
        s_adc_diag[(stas & 1u) ? ADC_DIAG_TIMEOUTS : ADC_DIAG_DONE]++;
        if (ctrl & 0x40u)
            s_adc_diag[ADC_DIAG_BIT6]++;
        if (!s_adc_diag[ADC_DIAG_FIRST_STAS]) {   /* the first completion, as the chip shows it */
            s_adc_diag[ADC_DIAG_FIRST_STAS] = 0x80000000u | stas;
            s_adc_diag[ADC_DIAG_FIRST_CTRL] = ctrl;
            s_adc_diag[ADC_DIAG_FIRST_DATA] = ADC[0];
            s_adc_diag[ADC_DIAG_FIRST_US] = (uint32_t)el;
        }
    }
    s_adc_n++;
    if (s_keys_isr)
        ch = s_adc_n % 8u ? 1u : k_other[(s_adc_n / 8u) % 3u];
    else
        ch = s_adc_n % 4u ? 1u : k_other[(s_adc_n / 4u) % 3u];
    ADC[2] &= ~0x10u;
    ADC[2] = (ADC[2] & ~0x67u) | ch | 0x18u;      /* (interrupt enable clear, as stock's polled path) */
    s_adc_ch = ch;
    s_adc_t0 = adc_now();
    return s_key_out;
}

int keys_adc(void)
{
    watchdog_feed();                 /* every screen that waits for a key polls here */
    if (s_keys_isr)
        return s_key_out;            /* (the timer converts: keys_tick) */
    return adc_step();
}

/* Core A's SysTick, every 10 ms once keys_isr_start() ran: the converter stepped and the keys
 * sampled into services/input.c's recogniser, whatever the main loop is doing (DESIGN 3.3). */
void keys_tick(void)
{
    if (!s_keys_isr)
        return;
    adc_step();
    input_sample_edges(s_key_out, keys_power(), time_ms(), s_press_ms, s_release_ms);
}

void keys_isr_start(void)
{
    (void)keys_power();              /* (its pin set up before the first tick reads it) */
    s_keys_isr = 1;
    input_isr_sampling(1);
}

const uint32_t *adc_diag(void)
{
    s_adc_diag[ADC_DIAG_LAST_CTRL] = ADC[2];
    s_adc_diag[ADC_DIAG_LAST_STAS] = ADC[1];
    return s_adc_diag;
}

/* The SAR-ADC as stock sets it up (MINIV390 AdcInit, seg01 0x0307a008), write for write: clock
 * 24 MHz / 24 = 1 MHz (0x03052802(1000000)), reference trim 3 and 5, reference on (0x03053078(3),
 * 0x0305306a(5), 0x03053086(1): GRF 0x500100CC, write-masked), CLK_SARADC and PCLK_SARADC
 * ungated, the converter powered. (The ROM's handoff already had the clock and gates so; whether
 * it leaves the reference on is not measured.) */
void adc_init(void)
{
    RK_CRU[0x2C / 4] = 0x00FF0017u;
    RK_GRF[0xCC / 4] = 0x000E0006u;
    RK_GRF[0xCC / 4] = 0x00700050u;
    RK_GRF[0xCC / 4] = 0x00010001u;
    cru_gate(10u, 1);
    cru_gate(0x93u, 1);
    ADC[2] |= 8u;
}

int adc_value(unsigned ch)
{
    return s_adc[ch & 3u];
}

/* Power key: GPIO2.5 input, 1 = pressed (reference 0x03051b2c). */
int keys_power(void)
{
    iomux_gpio(2, 5);
    GPIO2->ddr &= ~(1u << 5);
    return (int)((GPIO2->ext_porta >> 5) & 1u);
}

/* Power hold: GPIO2.6 low cuts the supply (reference 0x03051c1c). With
 * USB plugged in the player stays powered by VBUS. */
void board_power_off(void)
{
    watchdog_stop();              /* stock's PowerOff: WDT DeInit first (a unit on the cable stays powered) */
    extern void board_power_off_pins(void);   /* board_rknanoc.c */

    __asm__ volatile ("cpsid i");
    board_power_off_pins();                   /* 512 MB: GPIO0.14, GPIO2.1 low */
    panel_io_delay_ms(5);
    iomux_gpio(2, 6);
    GPIO2->dr &= ~(1u << 6);
    GPIO2->ddr |= 1u << 6;
    for (;;) { }
}

void panel_io_init(void)
{
    /* Mux every panel pin to GPIO first, as the reference does (0x03051ce8:
     * GPIO0 16..25, 15, 12; 0x03051cb0: GPIO2.15). Nothing guarantees the
     * ROM left them in GPIO mode. */
    static const uint8_t gpio0_pins[] = {16, 17, 18, 19, 20, 21, 22, 23,
                                         24, 25, 15, 12};
    for (unsigned i = 0; i < sizeof gpio0_pins; i++)
        iomux_gpio(0, gpio0_pins[i]);
    iomux_gpio(2, 15);

    /* GPIO2.15 = reset: output, released (high). */
    GPIO2->ddr |= PIN_RESET;
    GPIO2->dr |= PIN_RESET;

    /* GPIO0 panel pins: outputs (data bus + WR + D/C + CS/RD ctl). */
    GPIO0->ddr |= PANEL_DDR_MASK;

    /* Control idle state: WR high, CS high, data pins low; unrelated
     * GPIO0 pins preserved. */
    bus_ctl = (GPIO0->dr & ~PANEL_DDR_MASK) | PIN_WR | PIN_CS;
    GPIO0->dr = bus_ctl;

    /*
     * Reset pulse, timing extracted from the reference bring-up
     * (active low: high 50 ms, low 150 ms, high). The reference pulses
     * reset twice around its panel ID probe: first with a 30 ms settle,
     * later with a 120 ms settle right before the DCS stream. We have
     * no ID probe, so both pulses run back-to-back with their extracted
     * settle times (harmless, and keeps the pre-stream timing exact).
     */
    GPIO2->dr |= PIN_RESET;
    panel_io_delay_ms(50);
    GPIO2->dr &= ~PIN_RESET;
    panel_io_delay_ms(150);
    GPIO2->dr |= PIN_RESET;
    panel_io_delay_ms(30);

    GPIO2->dr |= PIN_RESET;
    panel_io_delay_ms(50);
    GPIO2->dr &= ~PIN_RESET;
    panel_io_delay_ms(150);
    GPIO2->dr |= PIN_RESET;
    panel_io_delay_ms(120);
}

static int s_path;                /* PANEL_PATH_PIO, _VOP, _DMA: where bytes go now (below) */
static void vop_send_cmd(uint8_t cmd);
static void vop_send_data(uint8_t data);

void panel_io_send_cmd(uint8_t cmd)
{
    if (s_path != PANEL_PATH_PIO) {
        panel_io_sync();          /* (a band still on its way first) */
        vop_send_cmd(cmd);
    } else {
        bus_write(cmd, 0);        /* D/C low = command byte */
    }
}

void panel_io_send_data(uint8_t data)
{
    if (s_path != PANEL_PATH_PIO) {
        panel_io_sync();
        vop_send_data(data);
    } else {
        bus_write(data, PIN_DC);  /* D/C high = parameter/data byte */
    }
}

void panel_io_send_pixel(uint16_t rgb565)
{
    /* Two data bytes on the wire, HIGH byte first (panel_io contract;
     * the reference's flush loop sends them exactly this way). */
    panel_io_send_data((uint8_t)(rgb565 >> 8));
    panel_io_send_data((uint8_t)(rgb565 & 0xFFu));
}

void panel_io_delay_ms(uint16_t ms)
{
    delay_ms(ms);
}

/* ---- the flush path ------------------------------------------------------------------------------
 * GPIO (bit-bang) pixel writer: every byte is the extracted pattern -- the whole GPIO0 DR from the
 * shadow with the byte in 16..23, D/C high, WR low, written twice, then once more with WR high
 * (the panel latches on the rising edge) -- but with the shadow word, the DR address and the
 * WR bit in registers, 4 pixels per loop, no call per byte. s_wr_stores = 2 drops the repeated
 * WR-low store (2 stores a byte: ~42 ns low, ~42 ns high at pclk 48 MHz, inside the ST7789's 15 /
 * 15 / 66 ns on paper) -- a measurement option (display=pio2), never the default: the repeated
 * store is what the reference does on this board. */

#if defined(__GNUC__) && !defined(__clang__)
#define HOT __attribute__((optimize("O2")))
#else
#define HOT
#endif

static int s_wr_stores = 3;

/* The path display=auto takes: the GPIO bus, as stock drives it, until the VOP + DMA path has run
 * on the 512 MB unit (it is proven in QEMU only; a wrong VOP timing would show a garbled screen,
 * which no trial marker catches). display=dma opts in, behind the trial guard. */
#ifndef PANEL_PATH_DEFAULT
#define PANEL_PATH_DEFAULT PANEL_PATH_PIO
#endif
#define PANEL_ROW_OFFSET_PHYS PANEL_ROW_OFFSET

#define BYTE3(dr, lo, b) do { uint32_t w_ = (lo) | (b); *(dr) = w_; *(dr) = w_; *(dr) = w_ | PIN_WR; } while (0)
#define BYTE2(dr, lo, b) do { uint32_t w_ = (lo) | (b); *(dr) = w_; *(dr) = w_ | PIN_WR; } while (0)
/* a pixel's bytes in place for the DR: high byte (bits 15..8 -> 23..16), then low (7..0 -> 23..16) */
#define HI(p) (((uint32_t)(p) & 0xFF00u) << 8)
#define LO(p) (((uint32_t)(p) & 0x00FFu) << 16)

static HOT void gpio_pixels3(const uint16_t *src, int n)
{
    volatile uint32_t *dr = &GPIO0->dr;
    uint32_t lo = (bus_ctl | PIN_DC) & ~PIN_WR;

    for (; n >= 4; n -= 4, src += 4) {
        uint32_t a = ((const uint32_t *)(const void *)src)[0], b = ((const uint32_t *)(const void *)src)[1];

        BYTE3(dr, lo, HI(a)); BYTE3(dr, lo, LO(a));
        BYTE3(dr, lo, HI(a >> 16)); BYTE3(dr, lo, LO(a >> 16));
        BYTE3(dr, lo, HI(b)); BYTE3(dr, lo, LO(b));
        BYTE3(dr, lo, HI(b >> 16)); BYTE3(dr, lo, LO(b >> 16));
    }
    for (; n > 0; n--, src++) {
        BYTE3(dr, lo, HI(*src));
        BYTE3(dr, lo, LO(*src));
    }
}

static HOT void gpio_pixels2(const uint16_t *src, int n)
{
    volatile uint32_t *dr = &GPIO0->dr;
    uint32_t lo = (bus_ctl | PIN_DC) & ~PIN_WR;

    for (; n >= 4; n -= 4, src += 4) {
        uint32_t a = ((const uint32_t *)(const void *)src)[0], b = ((const uint32_t *)(const void *)src)[1];

        BYTE2(dr, lo, HI(a)); BYTE2(dr, lo, LO(a));
        BYTE2(dr, lo, HI(a >> 16)); BYTE2(dr, lo, LO(a >> 16));
        BYTE2(dr, lo, HI(b)); BYTE2(dr, lo, LO(b));
        BYTE2(dr, lo, HI(b >> 16)); BYTE2(dr, lo, LO(b >> 16));
    }
    for (; n > 0; n--, src++) {
        BYTE2(dr, lo, HI(*src));
        BYTE2(dr, lo, LO(*src));
    }
}

/* CASET x0..x1, RASET y0..y1, RAMWR over the GPIO bus. */
static void gpio_window(int x0, int y0, int x1, int y1)
{
    bus_write(0x2A, 0);
    bus_write((uint8_t)(x0 >> 8), PIN_DC);
    bus_write((uint8_t)x0, PIN_DC);
    bus_write((uint8_t)(x1 >> 8), PIN_DC);
    bus_write((uint8_t)x1, PIN_DC);
    bus_write(0x2B, 0);
    bus_write((uint8_t)(y0 >> 8), PIN_DC);
    bus_write((uint8_t)y0, PIN_DC);
    bus_write((uint8_t)(y1 >> 8), PIN_DC);
    bus_write((uint8_t)y1, PIN_DC);
    bus_write(0x2C, 0);
}

static void gpio_blit(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch)
{
    int w = x1 - x0 + 1;

    gpio_window(x0, y0, x1, y1);
    for (int y = y0; y <= y1; y++, src += pitch) {
        if ((uintptr_t)src & 2u) {               /* (the word loads want a word-aligned row) */
            if (s_wr_stores == 2) {
                BYTE2(&GPIO0->dr, (bus_ctl | PIN_DC) & ~PIN_WR, HI(*src));
                BYTE2(&GPIO0->dr, (bus_ctl | PIN_DC) & ~PIN_WR, LO(*src));
            } else {
                BYTE3(&GPIO0->dr, (bus_ctl | PIN_DC) & ~PIN_WR, HI(*src));
                BYTE3(&GPIO0->dr, (bus_ctl | PIN_DC) & ~PIN_WR, LO(*src));
            }
            if (s_wr_stores == 2)
                gpio_pixels2(src + 1, w - 1);
            else
                gpio_pixels3(src + 1, w - 1);
        } else if (s_wr_stores == 2) {
            gpio_pixels2(src, w);
        } else {
            gpio_pixels3(src, w);
        }
    }
}

/* ---- the VOP MCU interface (0x60070000) ----------------------------------------------------------
 * What drives the panel on the 512 MB board under stock (MINIV390, work/stock_boot/02_display.md
 * §2-3: pins 16..25 and 15 on function 1, MCU_CON 0x800 for commands and parameters, 0x002 for
 * pixels, CMD +0x28, DATA +0x2C) and what RECHO 0.10 uses with DMA on the 8 GB board
 * (work/recho010/sections/30_display.md D.2-D.4: MCU_CON 0x16 = two pixels a word, DMAC channel
 * 0, destination handshake 14, 50 ms band timeout, a STATUS poll every 4 pixels when the CPU
 * feeds it, the bus timing from the core clock). Register names: SDK driver/vop/hw_vop.h. */
#define VOP_R(o)     (*(volatile uint32_t *)(0x60070000u + (o)))
#define VOP_CON      0x00u
#define VOP_TIMING   0x08u
#define VOP_SIZE     0x0Cu
#define VOP_WM       0x10u
#define VOP_INT_CLR  0x1Cu
#define VOP_INT_ST   0x20u
#define VOP_STATUS   0x24u                 /* bit 0: busy (RECHO's use) */
#define VOP_CMD      0x28u
#define VOP_DATA     0x2Cu
#define CON_CMD      0x800u                /* BYPASS_FIFO, SPLIT_ONE: a write = a bus byte */
#define CON_PIX      0x002u                /* SPLIT_TWO: a write = an RGB565 pixel, high byte first */
#define CON_DMA      0x016u                /* SPLIT_FOUR | HWORD_SWAP: a word = 2 pixels, low half first */
#define VOP_POLLS    100001u               /* RECHO's bound on a STATUS poll */

#define DMAC_R(o)    (*(volatile uint32_t *)(0x60000000u + (o)))
#define DMA_CH       0u                    /* RECHO's; audio is channel 5 (audio/audio.c) */
#define CH_R(o)      DMAC_R(DMA_CH * 0x58u + (o))
#define DMA_RAW_TFR  0x2C0u
#define DMA_MASK_TFR 0x310u
#define DMA_MASK_BLK 0x318u
#define DMA_MASK_SRC 0x320u
#define DMA_CLR_TFR  0x338u
#define DMA_CLR_BLK  0x340u
#define DMA_CLR_ERR  0x358u
#define DMA_CFG      0x398u
#define DMA_CHEN     0x3A0u
/* CTL_L: 32 -> 32 bit, source increments, destination fixed, bursts of 16, memory to peripheral,
 * source on master 2 (RECHO's 0x0210D924), with INT_EN: the raw transfer-done bit is then set
 * under any reading of the databook; the channel stays masked, nothing interrupts. */
#define DMA_CTL_L    0x0210D925u
#define DMA_CFG_L    0x00000800u           /* source: software handshake; priority 0 (audio has 2) */
#define DMA_CFG_H    0x00007007u           /* FCMODE, FIFO_MODE, PROTCTL 1, destination handshake 14 */
#define DMA_TIMEOUT_US 50000u              /* RECHO: 50 ms */
#define DMA_MAX_WORDS  4095u               /* the block size field */

/* The trial marker: two words that a warm reset keeps, in .noinit (firmware_ap.ld; not next to the
 * boot ROM's own record at 0x0308FFE8, not RECHO's 0x0309FFA0 in our PCM ring). The second word is
 * the first one inverted, so SRAM noise at power-on reads as "no marker". */
volatile uint32_t g_vop_mark[2] __attribute__((section(".noinit")));   /* (tests: by symbol) */
#define MARK         (g_vop_mark[0])
#define MARK_INV     (g_vop_mark[1])
#define MARK_TRIAL   0x564F5031u           /* 'VOP1': a trial started and has not proven itself */
#define MARK_FAIL    0x564F5046u           /* 'VOPF': refused until power-off */
#define TRIAL_MS     5000u

static unsigned s_vop_con = 0xFFFFFFFFu;  /* MCU_CON as last written */
static uint32_t s_vop_hz;                 /* the core clock TIMING was set for */
static int s_dma_busy;                    /* a band is on its way */
static uint32_t s_dma_t0, s_dma_done_us;
static int s_dma_y0, s_dma_y1, s_dma_x0, s_dma_x1;
static const uint16_t *s_dma_src;
static uint32_t s_trial_ms, s_trial_frames;
static int s_vop_stuck;                   /* a wait for STATUS idle timed out (cleared by to_gpio) */
static uint32_t s_cnt[4];

static uint32_t mark_get(void)
{
    uint32_t a = MARK;

    return (a ^ MARK_INV) == 0xFFFFFFFFu ? a : 0u;
}

static void mark_set(uint32_t v)
{
    MARK = v;
    MARK_INV = ~v;
}

static void iomux_func(unsigned port, unsigned pin, unsigned func)
{
    *(volatile uint32_t *)(0x50010000u + port * 0x20u + (pin >> 3) * 4u) =
        (0x30000u << ((pin & 7u) * 2u)) | (func << ((pin & 7u) * 2u));
}

/* The panel pins: D0-7 = GPIO0.16-23, WRN 0.24, RS 0.25, CSN 0.15; RD (0.12) stays a GPIO. */
static void pins_to(unsigned func)
{
    for (unsigned pin = 15; pin <= 25; pin++)
        iomux_func(0, pin, func);
}

/* Up to VOP_POLLS reads of STATUS until it is idle; 0 = idle, -1 = still busy. Right after a DMA,
 * first 12 us (RECHO's settle after a band). */
static int vop_idle(void)
{
    if (s_dma_done_us) {
        while ((uint32_t)time_us() - s_dma_done_us < 12u)
            ;
        s_dma_done_us = 0;
    }
    for (uint32_t n = 0; n < VOP_POLLS; n++)
        if (!(VOP_R(VOP_STATUS) & 1u))
            return 0;
    s_vop_stuck = 1;
    return -1;
}

/* The bus timing for the core clock (RECHO 0.10, D.2): a write pulse >= 20 ns, a cycle >= 70 ns
 * (ST7789: 15 ns, 66 ns): 0x1044 at 96 MHz, 0x1022 at 48 MHz. */
static void vop_timing(void)
{
    uint32_t hz = clock_core_hz(), mhz = hz / 1000000u, rwpw, t70, rwcs;

    if (hz == s_vop_hz)
        return;
    rwpw = mhz ? (mhz * 20u + 999u) / 1000u : 1u;
    t70 = (mhz * 70u + 999u) / 1000u;
    rwcs = t70 > rwpw + 2u ? t70 - 1u - rwpw : 1u;
    if (vop_idle())
        return;                                      /* (never while bytes go out) */
    delay_us(1);
    VOP_R(VOP_TIMING) = 0x1000u | rwpw << 5 | rwcs;
    s_vop_hz = hz;
}

static void vop_con(unsigned v)
{
    if (s_vop_con != v) {
        if (vop_idle())
            return;                                  /* (never while bytes go out) */
        delay_us(1);
        VOP_R(VOP_CON) = v;
        s_vop_con = v;
    }
}

/* LCDC soft reset (RECHO's vop_reset): after a band that did not finish, or a VOP stuck busy. */
static void vop_reset(void)
{
    cru_reset(13u, 1);
    delay_us(10);
    cru_reset(13u, 0);
    delay_us(100);
    s_vop_con = 0xFFFFFFFFu;
    s_vop_hz = 0;
}

static void vop_send_cmd(uint8_t cmd)
{
    vop_con(CON_CMD);
    VOP_R(VOP_CMD) = cmd;
}

static void vop_send_data(uint8_t data)
{
    vop_con(CON_CMD);
    VOP_R(VOP_DATA) = data;
}

/* The window; -1 = the VOP stays busy (a reset did not help): the caller leaves the VOP. */
static int vop_window(int x0, int y0, int x1, int y1)
{
    s_vop_stuck = 0;                                 /* (a new transaction: judged afresh) */
    if (vop_idle()) {
        vop_reset();
        s_vop_stuck = 0;
        if (vop_idle())
            return -1;
    }
    vop_timing();
    vop_send_cmd(0x2A);
    vop_send_data((uint8_t)(x0 >> 8));
    vop_send_data((uint8_t)x0);
    vop_send_data((uint8_t)(x1 >> 8));
    vop_send_data((uint8_t)x1);
    vop_send_cmd(0x2B);
    vop_send_data((uint8_t)(y0 >> 8));
    vop_send_data((uint8_t)y0);
    vop_send_data((uint8_t)(y1 >> 8));
    vop_send_data((uint8_t)y1);
    vop_send_cmd(0x2C);
    return s_vop_stuck ? -1 : 0;
}

/* The CPU feeds the VOP: 4 pixels, then STATUS until idle (RECHO's PIO over the VOP). -1 = the
 * VOP stayed busy. */
static HOT int vop_pixels(const uint16_t *src, int n)
{
    volatile uint32_t *data = &VOP_R(VOP_DATA), *status = &VOP_R(VOP_STATUS);

    for (; n > 0; n -= 4, src += 4) {
        uint32_t polls = VOP_POLLS;

        data[0] = src[0];
        if (n > 1)
            data[0] = src[1];
        if (n > 2)
            data[0] = src[2];
        if (n > 3)
            data[0] = src[3];
        while ((*status & 1u) && --polls)
            ;
        if (!polls)
            return -1;
    }
    return 0;
}

static void to_gpio(const char *why, int fault);
static void dma_abort(void);

static int vop_blit(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch)
{
    if (vop_window(x0, y0, x1, y1))
        return -1;
    vop_con(CON_PIX);
    if (s_vop_stuck)
        return -1;
    for (int y = y0; y <= y1; y++, src += pitch)
        if (vop_pixels(src, x1 - x0 + 1))
            return -1;
    return 0;
}

/* One band by DMA: the window by the CPU, then the pixels from src (contiguous, word aligned, an
 * even count) as words. Returns at once (0); dma_wait() finishes it. -1 = the VOP stays busy. */
static int dma_start(int x0, int y0, int x1, int y1, const uint16_t *src)
{
    uint32_t words = (uint32_t)(x1 - x0 + 1) * (uint32_t)(y1 - y0 + 1) / 2u;

    if (vop_window(x0, y0, x1, y1))
        return -1;
    VOP_R(VOP_WM) = 0x808u;
    VOP_R(VOP_SIZE) = (uint32_t)(y1 - y0) << 12 | (uint32_t)(x1 - x0);
    vop_con(CON_DMA);
    if (s_vop_stuck)
        return -1;
    if (!(DMAC_R(DMA_CFG) & 1u))
        DMAC_R(DMA_CFG) = 1u;
    DMAC_R(DMA_CLR_TFR) = 1u << DMA_CH;
    DMAC_R(DMA_CLR_BLK) = 1u << DMA_CH;
    DMAC_R(DMA_CLR_ERR) = 1u << DMA_CH;
    DMAC_R(DMA_MASK_TFR) = 0x100u << DMA_CH;         /* (write enable, value 0: masked) */
    DMAC_R(DMA_MASK_BLK) = 0x100u << DMA_CH;
    DMAC_R(DMA_MASK_SRC) = 0x100u << DMA_CH;
    CH_R(0x00) = (uint32_t)(uintptr_t)src;
    CH_R(0x08) = 0x60070000u + VOP_DATA;
    CH_R(0x10) = 0;
    CH_R(0x40) = DMA_CFG_L;
    CH_R(0x44) = DMA_CFG_H;
    CH_R(0x18) = DMA_CTL_L;
    CH_R(0x1C) = words;
    s_dma_x0 = x0; s_dma_y0 = y0; s_dma_x1 = x1; s_dma_y1 = y1;
    s_dma_src = src;
    s_dma_t0 = (uint32_t)time_us();
    s_dma_busy = 1;
    DMAC_R(DMA_CHEN) = 0x101u << DMA_CH;
    return 0;
}

static void dma_abort(void)
{
    DMAC_R(DMA_CHEN) = 0x100u << DMA_CH;             /* (write enable, value 0) */
    for (uint32_t n = 0; n < VOP_POLLS && (DMAC_R(DMA_CHEN) & (1u << DMA_CH)); n++)
        ;
    DMAC_R(DMA_CLR_TFR) = 1u << DMA_CH;
    s_dma_busy = 0;
}

/* The band in flight is on the panel (or, after 50 ms, sent again by the CPU: a band that does not
 * finish moves the path to the CPU-fed VOP, as RECHO does). */
static void dma_wait(void)
{
    uint32_t us;

    if (!s_dma_busy)
        return;
    while (!(DMAC_R(DMA_RAW_TFR) & (1u << DMA_CH))) {
        if ((uint32_t)time_us() - s_dma_t0 > DMA_TIMEOUT_US) {
            dma_abort();
            s_cnt[1]++;
            log_printf("display: DMA band %d..%d did not finish in 50 ms: the CPU feeds the VOP from now on",
                       s_dma_y0 - PANEL_ROW_OFFSET_PHYS, s_dma_y1 - PANEL_ROW_OFFSET_PHYS);
            s_path = PANEL_PATH_VOP;
            (void)vop_idle();
            vop_reset();
            if (vop_blit(s_dma_x0, s_dma_y0, s_dma_x1, s_dma_y1, s_dma_src, s_dma_x1 - s_dma_x0 + 1))
                to_gpio("stayed busy after a DMA timeout", 1);
            return;
        }
    }
    us = (uint32_t)time_us();
    DMAC_R(DMA_CLR_TFR) = 1u << DMA_CH;
    s_dma_busy = 0;
    s_dma_done_us = us ? us : 1u;
    s_cnt[0]++;
    if (us - s_dma_t0 > s_cnt[3])
        s_cnt[3] = us - s_dma_t0;
}

/* Back to the GPIO bus, for the rest of this power cycle (a fault: the VOP misbehaved). */
static void to_gpio(const char *why, int fault)
{
    s_cnt[2] += fault ? 1u : 0u;
    log_printf("display: the VOP %s: back to the GPIO bus until power-off", why);
    if (s_dma_busy)
        dma_abort();
    vop_reset();
    pins_to(0);
    GPIO0->ddr |= PANEL_DDR_MASK;
    bus_ctl = (GPIO0->dr & ~PANEL_DDR_MASK) | PIN_WR | PIN_CS;
    GPIO0->dr = bus_ctl;
    s_path = PANEL_PATH_PIO;
    s_vop_stuck = 0;
    mark_set(MARK_FAIL);
    if (s_trial_ms) {                                /* off the VOP: nothing left to hang */
        s_trial_ms = 0;
        if (bootcrumb_step() == BC_DISPLAY_TRIAL)
            bootcrumb(BC_LOOP);
    }
}

void panel_io_blit(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch)
{
    int w = x1 - x0 + 1, h = y1 - y0 + 1;

    if (s_path == PANEL_PATH_DMA) {
        dma_wait();                                  /* (the band before: its buffer is free then) */
        if (s_path == PANEL_PATH_DMA && pitch == w && !((uintptr_t)src & 3u) && !((w * h) & 1) &&
            (uint32_t)(w * h / 2) <= DMA_MAX_WORDS) {
            if (!dma_start(x0, y0, x1, y1, src)) {
                s_trial_frames++;
                return;
            }
            to_gpio("stayed busy", 1);
            gpio_blit(x0, y0, x1, y1, src, pitch);
            return;
        }
    }
    if (s_path == PANEL_PATH_DMA || s_path == PANEL_PATH_VOP) {
        if (!vop_blit(x0, y0, x1, y1, src, pitch)) {
            s_trial_frames++;
            return;
        }
        to_gpio("stayed busy", 1);
    }
    gpio_blit(x0, y0, x1, y1, src, pitch);
}

void panel_io_sync(void)
{
    if (s_path == PANEL_PATH_DMA)
        dma_wait();
}

const char *panel_io_path(void)
{
    return s_path == PANEL_PATH_DMA ? "dma" : s_path == PANEL_PATH_VOP ? "vop" : s_wr_stores == 2 ? "pio2" : "pio";
}

const uint32_t *panel_io_counters(void)
{
    return s_cnt;
}

int panel_io_select(int path)
{
    uint32_t m = mark_get();

    if (path == PANEL_PATH_AUTO)
        path = PANEL_PATH_DEFAULT;
    panel_io_sync();
    if (path == PANEL_PATH_PIO || path == PANEL_PATH_PIO2) {
        if (s_path != PANEL_PATH_PIO)
            to_gpio("is not wanted", 0);            /* (latches: a switch back needs a power cycle) */
        s_wr_stores = path == PANEL_PATH_PIO2 ? 2 : 3;
        return s_path;
    }
    if (s_path != PANEL_PATH_PIO) {                  /* VOP <-> DMA: no new trial */
        s_path = path;
        return s_path;
    }
    if (m == MARK_FAIL || m == MARK_TRIAL || bootcrumb_prev_step() == BC_DISPLAY_TRIAL) {
        mark_set(MARK_FAIL);
        log_printf("display: %s refused: the last VOP trial did not finish (%s); GPIO until power-off",
                   path == PANEL_PATH_DMA ? "dma" : "vop",
                   m == MARK_FAIL ? "latched" : m == MARK_TRIAL ? "marker" : "boot trail");
        return s_path;
    }
    if (!watchdog_running()) {
        log_printf("display: %s refused: no watchdog to end a hang", path == PANEL_PATH_DMA ? "dma" : "vop");
        return s_path;
    }
    mark_set(MARK_TRIAL);
    bootcrumb(BC_DISPLAY_TRIAL);
    s_trial_ms = time_stamp(time_ms());
    s_trial_frames = 0;
    log_printf("display: %s trial (marker set; cleared after %u s of frames)", path == PANEL_PATH_DMA ? "dma" : "vop",
               (unsigned)(TRIAL_MS / 1000u));
    cru_gate(127u, 1);                               /* HCLK_LCDC */
    if (path == PANEL_PATH_DMA)
        cru_gate(125u, 1);                           /* HCLK_DMA (audio's too: only ever switched on) */
    vop_reset();                                     /* (LCDC_SRST pulse) */
    vop_con(CON_CMD);
    vop_timing();
    pins_to(1);
    s_path = path;
    return s_path;
}

void panel_io_poll(void)
{
    if (s_trial_ms && s_trial_frames && time_ms() - s_trial_ms >= TRIAL_MS) {
        s_trial_ms = 0;
        if (mark_get() == MARK_TRIAL)
            mark_set(0);
        if (bootcrumb_step() == BC_DISPLAY_TRIAL)
            bootcrumb(BC_LOOP);
        log_printf("display: the %s path ran %u s (%u bands): trial marker cleared", panel_io_path(),
                   (unsigned)(TRIAL_MS / 1000u), (unsigned)s_trial_frames);
    }
}

uint32_t panel_io_now_us(void)
{
    return (uint32_t)time_us();
}

uint32_t panel_io_now_ms(void)
{
    return time_ms();
}
