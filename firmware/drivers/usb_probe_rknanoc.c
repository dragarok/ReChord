/*
 * usb_probe_rknanoc.c -- usb_probe.h: the PHY's VBUS comparators swept per candidate supply.
 *
 * GRF (0x50010000, write-masked: the upper half says which low bits a write changes):
 *   USBPHY_CON6 +0x98: [6:3] force A-sess/B-sess/sess-end/VBUS-valid, [9:7] A-session
 *                      reference, [12:10] B-session reference, [15:13] session-end reference
 *   USBPHY_CON7 +0x9C: [2:0] VBUS-valid reference, [15] VBUS detection powered down
 *   UOC_CON1    +0xB4: [9] IDDIG from software, [10] IDDIG (0 = A-device: host)
 *   UOC_CON2    +0xB8: [1]/[2] D+/D- 15k pull-downs, [4] UTMI_DRVVBUS, [5] UTMI_CHRGBUS
 *   SOC_USB_STATUS +0xE8: [0] VBUS valid, [1] B-session valid, [2] A-session valid, [3] session end
 * (SDK driver/GRF/hw_grf.h.) No pin state here is one stock firmware does not use itself.
 */
#include <stdint.h>

#include "clock.h"
#include "panel_io.h"
#include "rknanoc.h"
#include "usb_msc.h"
#include "usb_probe.h"

#define OTG(off)    (*(volatile uint32_t *)(0x60080000u + (off)))
#define GPIO_DR(p)  (*(volatile uint32_t *)(k_gpio[p]))
#define GPIO_DDR(p) (*(volatile uint32_t *)(k_gpio[p] + 4u))

enum { GUSBCFG = 0x0C, GRSTCTL = 0x10, DCTL = 0x804, HCFG = 0x400, HPRT = 0x440 };
enum { CON6 = 0x98 / 4, CON7 = 0x9C / 4, UOC1 = 0xB4 / 4, UOC2 = 0xB8 / 4, STATUS = 0xE8 / 4 };

static const uint32_t k_gpio[3] = { 0x40160000u, 0x40170000u, 0x50030000u };

/* One comparator's trip pattern over its eight reference codes. */
static uint8_t sweep(unsigned reg, unsigned shift, unsigned bit)
{
    uint8_t m = 0;

    for (unsigned code = 0; code < 8u; code++) {
        RK_GRF[reg] = (7u << (shift + 16u)) | code << shift;
        rk_delay_us(200);
        if (RK_GRF[STATUS] & (1u << bit))
            m |= (uint8_t)(1u << code);
    }
    return m;
}

static void measure(usb_probe_step_t *s, const char *name, int host)
{
    uint32_t con6 = RK_GRF[CON6] & 0xFFFFu, con7 = RK_GRF[CON7] & 0xFFFFu;

    s->name = name;
    delay_ms(50);                                   /* let VBUS settle (and a device boot) */
    s->vbus = sweep(CON7, 0, 0);
    s->bsess = sweep(CON6, 10, 1);
    s->asess = sweep(CON6, 7, 2);
    s->sessend = sweep(CON6, 13, 3);
    RK_GRF[CON6] = 0xFFFF0000u | con6;               /* the references back */
    RK_GRF[CON7] = 0xFFFF0000u | con7;
    for (unsigned ch = 0; ch < 4u; ch++)
        s->adc[ch] = (int16_t)adc_value(ch);
    s->host = -1;
    s->linestate = 0;
    if (host) {
        uint32_t p = 0;

        for (int i = 0; i < 20 && !(p & 1u); i++) {  /* up to 200 ms for a device to pull D+ up */
            p = OTG(HPRT);
            delay_ms(10);
        }
        s->host = (int8_t)(p & 1u);
        s->linestate = (uint8_t)((p >> 10) & 3u);
    }
}

static void gpio_set(unsigned port, unsigned pin, int level)
{
    if (port == 0) {
        panel_io_gpio0_pin(pin, level);              /* (GPIO0 goes through the panel's shadow) */
    } else if (level) {
        GPIO_DR(port) |= 1u << pin;
    } else {
        GPIO_DR(port) &= ~(1u << pin);
    }
    GPIO_DDR(port) |= 1u << pin;
}

static int gpio_get(unsigned port, unsigned pin)
{
    return (int)((GPIO_DR(port) >> pin) & 1u);
}

/* The controller as host (the SDK's HostConnect(): IDDIG from software = 0, the 15k
 * pull-downs on D+/D-), then port power (the core's own drvvbus). */
static void host_on(void)
{
    RK_GRF[UOC2] = 0x00060006u;
    RK_GRF[UOC1] = 0x06000200u;
    OTG(GUSBCFG) |= 1u << 29;                        /* ForceHstMode */
    delay_ms(25);
    OTG(HCFG) &= ~3u;
    OTG(HPRT) = (OTG(HPRT) & ~0x2Eu) | 1u << 12;     /* PrtPwr (W1C/W1-disable bits left alone) */
}

static void host_off(void)
{
    OTG(HPRT) &= ~(0x2Eu | 1u << 12);
    OTG(GUSBCFG) &= ~(1u << 29);
    RK_GRF[UOC1] = 0x06000000u;                      /* IDDIG from the PHY again */
    RK_GRF[UOC2] = 0x00060000u;
}

int usb_probe_run(usb_probe_step_t *out, void (*progress)(int step, const char *name))
{
    uint32_t dr1 = GPIO_DR(1), dr2 = GPIO_DR(2), ddr1 = GPIO_DDR(1), ddr2 = GPIO_DDR(2);
    int g21, g20, g014 = gpio_get(0, 14), n = 0;

    if (usb_cable())
        return -1;
    usb_dwc2_phy_on();                               /* (the comparators need the PHY up) */
    OTG(DCTL) |= 2u;                                 /* no D+ pull-up of our own */
    g21 = gpio_get(2, 1);
    g20 = gpio_get(2, 0);
#define STEP(name, host) do { if (progress) progress(n, name); measure(&out[n++], name, host); } while (0)
    STEP("baseline", 0);
    gpio_set(2, 1, !g21);
    STEP(g21 ? "GPIO2.1 0" : "GPIO2.1 1", 0);
    gpio_set(2, 1, g21);
    gpio_set(2, 0, !g20);
    STEP(g20 ? "GPIO2.0 0" : "GPIO2.0 1", 0);
    gpio_set(2, 0, g20);
    gpio_set(0, 14, !g014);
    STEP(g014 ? "GPIO0.14 0" : "GPIO0.14 1", 0);
    gpio_set(0, 14, g014);
    RK_GRF[UOC2] = 0x00100010u;
    STEP("DRVVBUS", 0);
    RK_GRF[UOC2] = 0x00100000u;
    host_on();
    STEP("host+port power", 1);
    gpio_set(2, 1, 1);
    gpio_set(2, 0, 1);
    RK_GRF[UOC2] = 0x00100010u;
    STEP("all", 1);
    RK_GRF[UOC2] = 0x00100000u;
    host_off();
    gpio_set(2, 1, g21);
    gpio_set(2, 0, g20);
    RK_GRF[UOC2] = 0x00200020u;
    STEP("CHRGBUS (ref)", 0);
    RK_GRF[UOC2] = 0x00200000u;
#undef STEP
    usb_dwc2_phy_off();
    GPIO_DR(1) = dr1;                                /* (the PHY bring-up drives GPIO1.4, 2.0, 2.2) */
    GPIO_DDR(1) = ddr1;
    GPIO_DR(2) = dr2;
    GPIO_DDR(2) = ddr2;
    return n;
}
