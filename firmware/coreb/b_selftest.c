/*
 * b_selftest.c -- core B's half of the clock self-test (services/cpuclk.c runs core A's at the same
 * time; the kernels and the result codes: audio/selftest_kernel.h; docs/CPU_CLOCK.md).
 *
 * Run from b_hold() (b_main.c) on A's AS_CMD_SELFTEST, right after A moved the PLL to the FLAC point:
 * interrupts masked, no track open (B asks for a point at a track's open, before the decoder runs),
 * so its whole heap is free. In order:
 *   1. its clock: its SysTick (the core clock) against the 24 MHz timer over 4 ms;
 *   2. the datapath kernel (the same signature as core A's and the host's);
 *   3. March C- over its heap (the HIGH data RAM at B's clock; the GRF's 0xC000 response cycle) and
 *      over its LOW window (core A's SRAM across the bus at A's clock; GRF 0x3040);
 *   4. the checksum of its own image as core A loaded it;
 * and the PLL lock bit between every step. A judges the numbers.
 */
#include <stdint.h>

#include "b_rt.h"
#include "audio/selftest_kernel.h"

#define SYST_CSR  (*(volatile uint32_t *)0xE000E010u)
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014u)
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018u)
#define CRU_CON1  (*(volatile uint32_t *)0x40180004u)
#define TMR_NOW   (*(volatile uint32_t *)0x400E0008u)   /* TIMER0 CurrentValue0 (drivers/clock_rknanoc.c) */

extern uint8_t _b_heap[], _b_stack_limit[];

static uint32_t ticks_between(uint32_t a, uint32_t b)
{
    uint32_t d1 = a - b, d2 = b - a;

    return d1 < d2 ? d1 : d2;
}

static void lock_check(void)
{
    if (!(CRU_CON1 & (1u << 10)))
        AS->st_b_unlock++;
}

void b_selftest(void)
{
    static volatile uint32_t ten = 10u;
    uint32_t buf[64], t0, c0, c1, n, heap, words;

    AS->st_b_unlock = 0;
    AS->st_b_state = 1u;
    lock_check();
    /* 1. the clock: SysTick from all ones at the core clock, no interrupt (CSR = 5) */
    SYST_CSR = 0;
    SYST_RVR = 0x00FFFFFFu;
    SYST_CVR = 0;
    SYST_CSR = 5u;
    for (n = 0; n < 16u; n++)                  /* (the first reload happens a cycle after the start) */
        __asm__ volatile ("nop");
    t0 = TMR_NOW;
    c0 = SYST_CVR;
    while (ticks_between(t0, TMR_NOW) < 96000u)    /* 4 ms of the 24 MHz timer */
        ;
    c1 = SYST_CVR;
    AS->st_b_ticks = ticks_between(t0, TMR_NOW);
    AS->st_b_cyc = c0 - c1;                    /* (counts down; 4 ms at 450 MHz is 1.8 M of 16.7 M) */
    SYST_CSR = 0;
    lock_check();
    /* 2. the datapath */
    AS->st_b_sig = st_kernel(ST_KERNEL_SEED, ST_KERNEL_ITERS, buf, &ten);
    lock_check();
    /* 3. the memories */
    heap = ((uint32_t)_b_heap + 7u) & ~7u;
    words = ((uint32_t)_b_stack_limit - heap) / 4u;
    AS->st_b_high_at = heap;
    AS->st_b_high_words = words;
    AS->st_b_high = st_march2((volatile uint32_t *)heap, words);
    lock_check();
    AS->st_b_low = st_march2((volatile uint32_t *)ST_LOW_B, ST_LOW_BYTES / 4u);
    lock_check();
    /* 4. the code core A loaded */
    AS->st_b_code = st_sum((const volatile uint32_t *)AS_B_RAM, AS->st_code_words);
    lock_check();
    __asm__ volatile ("dmb" ::: "memory");
    AS->st_b_state = 2u;
}
