/*
 * watchdog.c -- the SoC's DW APB watchdog at 0x40120000, armed as stock's WatchDogInitial does
 * (MINIV390 0x03061e48; work/stock_boot/03_reset_power.md section 4): PCLK_WDT on (gate 0x8F),
 * WDT_SRST pulsed (SOFTRST1 bit 15), CR = 0x16 (RMOD 1: an interrupt at the first timeout, a
 * reset at the second; RPL 5 = 64 pclk), the timeout range, CRR = 0x76 to restart, CR |= 1.
 * Stock uses TORR 13 (2^29 pclk, 11 s at 48 MHz); 12 here (2^28: 5.6 s, the reset 5.6 s later).
 *
 * Stock feeds it on every pass of its OS loop and inside every long operation (110 sites). The
 * interrupt is NVIC IRQ 8 (startup/ap_startup.c's table: "IMDCT WDT"; stock's IntRegister ID 24
 * = 16 + 8). It records where the firmware hung (the stacked pc and lr) in the boot trail, so a
 * hang that needed RST on the device (the USB DAC session, 2026-10-16) now names its address;
 * the second timeout then resets the chip and the ROM boots us again.
 */
#include <stdint.h>

#include "watchdog.h"
#include "bootcrumb.h"
#include "drivers/clock.h"
#include "drivers/rknanoc.h"

#define WDT(o)    (*(volatile uint32_t *)(0x40120000u + (o)))
#define WDT_CR    0x00u
#define WDT_TORR  0x04u
#define WDT_CRR   0x0Cu
#define WDT_KICK  0x76u
#define NVIC_ISER0 (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICER0 (*(volatile uint32_t *)0xE000E180u)
#define NVIC_ICPR0 (*(volatile uint32_t *)0xE000E280u)
#define WDT_IRQ   8u

static int s_on;

/* The timeout range for pclk: the smallest TORR whose 2^(16 + TORR) pclk cycles last at least 4.5 s
 * (12 at the UI point's 48 MHz: 5.6 s, and at the FLAC point's 56 MHz: 4.8 s; stock 13 at 48). */
static uint32_t torr_for(uint32_t pclk)
{
    uint32_t t = 0;

    while (t < 15u && (uint64_t)(1u << (16u + t)) * 2u < (uint64_t)pclk * 9u)
        t++;
    return t;
}

/* pclk changed (services/cpuclk.c, after a CPU point switch): the range for the new pclk (DW WDT:
 * it applies from the next restart), then a restart. */
void watchdog_retime(void)
{
    if (!s_on)
        return;
    WDT(WDT_TORR) = torr_for(clock_pclk_hz());
    WDT(WDT_CRR) = WDT_KICK;
}

void watchdog_start(void)
{
    cru_gate(0x8Fu, 1);                      /* PCLK_WDT */
    cru_reset(0x1Fu, 1);                     /* WDT_SRST (SOFTRST1 bit 15) */
    delay_ms(1);
    cru_reset(0x1Fu, 0);
    WDT(WDT_CR) = 0x16u;
    WDT(WDT_TORR) = 12u;
    WDT(WDT_CRR) = WDT_KICK;
    NVIC_ICPR0 = 1u << WDT_IRQ;
    NVIC_ISER0 = 1u << WDT_IRQ;
    WDT(WDT_CR) = 0x17u;
    s_on = 1;
}

int watchdog_running(void)
{
    return s_on;
}

void watchdog_feed(void)
{
    if (s_on)
        WDT(WDT_CRR) = WDT_KICK;
}

/* Before a power-off or reboot (stock: WDT DeInit 0x03061dde in PowerOff and SysReboot): a unit
 * powered off with the cable in stays powered, and must not be woken by its own watchdog. */
void watchdog_stop(void)
{
    NVIC_ICER0 = 1u << WDT_IRQ;
    WDT(WDT_CR) = 0;
    cru_reset(0x1Fu, 1);
    s_on = 0;
}

void watchdog_isr(const uint32_t *frame);
void watchdog_isr(const uint32_t *frame)
{
    int ok = (uint32_t)frame >= 0x03000000u && (uint32_t)frame <= 0x0309FFE0u;

    NVIC_ICER0 = 1u << WDT_IRQ;              /* once: the second timeout resets */
    bootcrumb_fault(ok ? frame[6] : 0, ok ? frame[5] : 0, 16u + WDT_IRQ);
}

void WDT_IRQHandler(void) __attribute__((naked));
void WDT_IRQHandler(void)
{
    __asm__ volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "push {r4, lr}\n"
        "bl watchdog_isr\n"
        "pop {r4, pc}\n");
}
