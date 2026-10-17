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

static void key_sample(int v)
{
    int k;

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

int keys_adc(void)
{
    static const uint8_t k_other[3] = { 0, 2, 3 };
    unsigned ch;

    watchdog_feed();                 /* every screen that waits for a key polls here */

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
    ch = s_adc_n % 4u ? 1u : k_other[(s_adc_n / 4u) % 3u];
    ADC[2] &= ~0x10u;
    ADC[2] = (ADC[2] & ~0x67u) | ch | 0x18u;      /* (interrupt enable clear, as stock's polled path) */
    s_adc_ch = ch;
    s_adc_t0 = adc_now();
    return s_key_out;
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

void panel_io_send_cmd(uint8_t cmd)
{
    bus_write(cmd, 0);            /* D/C low = command byte */
}

void panel_io_send_data(uint8_t data)
{
    bus_write(data, PIN_DC);      /* D/C high = parameter/data byte */
}

void panel_io_send_pixel(uint16_t rgb565)
{
    /* Two data bytes on the wire, HIGH byte first (panel_io contract;
     * the reference's flush loop sends them exactly this way). */
    bus_write((uint8_t)(rgb565 >> 8), PIN_DC);
    bus_write((uint8_t)(rgb565 & 0xFFu), PIN_DC);
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

void panel_io_blit(int x0, int y0, int x1, int y1, const uint16_t *src, int pitch)
{
    gpio_blit(x0, y0, x1, y1, src, pitch);
}

void panel_io_sync(void)
{
}

const char *panel_io_path(void)
{
    return s_wr_stores == 2 ? "pio2" : "pio";
}

uint32_t panel_io_now_us(void)
{
    return (uint32_t)time_us();
}

uint32_t panel_io_now_ms(void)
{
    return time_ms();
}
