/*
 * ReChord AP (fw1) startup -- Cortex-M3 vector table + reset handler for the
 * A_CORE build.
 *
 * VERIFIED BOOT MODEL (docs/HANDOVER.md "PHASE A AUDIT RESULT"): the loader
 * scatter-loads the fw1 body into RAM contiguously (RAM = 0x0304F298 + IMG
 * offset) so the vector table lands at RAM 0x03050000 = IMG 0xD68 (the
 * RKnanoFW header base) and the image executes in place.  The ROM reads
 * word 0 (initial MSP) and word 1 (reset) from that table and enters at the
 * reset handler.  This file mirrors the reference image's recipe (its
 * startup at RAM 0x030500E4): bounded .data copy, bounded .bss zero, VTOR =
 * vector table, dsb/isb, Main.
 *
 * The stack is the 16 KiB BELOW 0x03004000 (the zone 0x03000000..0x03004000)
 * and nothing else may live there.  BSS is also zero-fillable by scatter
 * region rows, so the explicit clear here is defensive (idempotent).
 */
#include <stdint.h>

/* NOTE (milestone 1a compile fix): this file previously typedef'd uint32
 * locally and used ap_vectors before declaring it.  The local typedef
 * collided with the SDK typedef.h uint32 (unsigned long) that arrives via
 * the force-included armcc_compat.h, and the use-before-declaration of
 * ap_vectors is a hard error in every C mode.  Fixed by using stdint's
 * uint32_t throughout and declaring ap_vectors up front. */

extern const uint32_t ap_vectors[];   /* defined at the bottom of this file */

extern uint32_t Main(void);
extern uint32_t _bss_start, _bss_end;
extern uint32_t _data_start, _data_end;

/* Initial MSP: byte-verified word 0 of the reference vector table.  The
 * stack zone is 0x03000000..0x03004000 ONLY (firmware_ap.ld __stack_top). */
#define AP_INITIAL_MSP 0x03004000u

/* Every fault and every unexpected interrupt: never a silent hang (services/bootguard.h).
 * The stacked frame (MSP or PSP, EXC_RETURN bit 2) and the exception number go to
 * bootguard_fault(), which records them and resets. SP is moved to a known-good spot first:
 * a fault from a stack overflow must not fault again here. */
void Default_Handler(void) __attribute__((naked));
void Default_Handler(void)
{
    __asm__ volatile(
        "tst lr, #4\n"
        "ite eq\n"
        "mrseq r0, msp\n"
        "mrsne r0, psp\n"
        "mrs r1, ipsr\n"
        "ldr r2, =0x03003F00\n"                    /* inside the stack zone, below its top */
        "mov sp, r2\n"
        "b bootguard_fault\n");
}

/* Reset handler per the verified boot recipe (extracted from the reference
 * image's startup at 0x030500E4): zero ONLY the real .bss span, set VTOR to
 * our 256-aligned vector table, barriers, then main. The ROM does not set
 * VTOR - the reference writes it explicitly and so do we. */
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
void PowerOn_Reset(void) __attribute__((noreturn, used));
void PowerOn_Reset(void)
{
    /* Reference startup 0x030500E4: quiesce whatever the ROM left running
     * before VTOR moves to our table (any live IRQ would land in
     * Default_Handler). SysTick off, all NVIC IRQs disabled + unpended,
     * SHCSR cleared, AIRCR VECTCLRACTIVE. */
    __asm__ volatile("cpsid i");
    *(volatile uint32_t *)0xE000E010u = 0;
    *(volatile uint32_t *)0xE000E180u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0xE000E184u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0xE000E280u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0xE000E284u = 0xFFFFFFFFu;
    *(volatile uint32_t *)0xE000ED24u = 0;
    *(volatile uint32_t *)0xE000ED0Cu = 0x05FA0002u;
    /* No .data copy here: the ROM already copied it (scatter row 1). Our
     * row 0 ends where the .data LMA begins, so that LMA is never loaded
     * (verdict #23: copying from it clobbered every initialized global). */
    uint32_t *p = &_bss_start;
    while (p < &_bss_end)
        *p++ = 0;
    SCB_VTOR = (uint32_t)&ap_vectors;   /* our table, 256-aligned */
    __asm__ volatile("dsb; isb" ::: "memory");
    Main();
#ifdef AP_CHAIN_REF
    /* ENTRY test (make entry-img): hand the ROM's r0-r3 to the reference's
     * reset (veneer -> its startup 0x030500E4, which redoes SP/IRQ/data/bss/
     * VTOR) so the device always ends in a firmware with USB + updater. */
    __asm__ volatile("ldr r4, =0x03003FF0\n ldm r4, {r0-r3}\n"
                     "ldr r4, =0x0306296F\n bx r4");
#endif
    for (;;)
        __asm volatile("");
}

#ifdef AP_CHAIN_REF
/* ROM entry: park r0-r3 (boot params; the ref startup saves them) at the
 * top of the stack zone, run below them. */
void ChainEntry(void) __attribute__((naked));
void ChainEntry(void)
{
    __asm__ volatile("ldr r4, =0x03003FF0\n stm r4, {r0-r3}\n mov sp, r4\n"
                     "b PowerOn_Reset");
}
#endif

/* The entry both vector tables name: SP set to the top of the stack zone here, before any C
 * runs, whatever the ROM (or a chained loader, or a reset through SYSRESETREQ) left in it.
 * The reference's startup sets SP itself too. */
void PowerOn_Entry(void) __attribute__((naked, noreturn, used));
void PowerOn_Entry(void)
{
    __asm__ volatile("ldr r0, =0x03004000\n"   /* AP_INITIAL_MSP */
                     "msr msp, r0\n"
                     "mov r0, #0\n"
                     "msr control, r0\n"          /* privileged, MSP in use */
                     "isb\n"
                     "b PowerOn_Reset\n");
}

/* ROM boot contract (verdict #19, matches reference + stock): the ROM copies
 * .data (scatter row 1, image right after row 0) to 0x03004000 and enters
 * through the vector table at its start - so .data must begin with one. */
const uint32_t ap_ram_vectors[2]
    __attribute__((section(".data.ram_vectors"), used)) = {
    AP_INITIAL_MSP,
    (uint32_t)&PowerOn_Entry,
};

/* Handlers: weak aliases of Default_Handler, overridden by the drivers.
 * IRQ numbers are the SDK's (rockchip/system/os/interrupt.h). */
#define WEAK_HANDLER(name) void name(void) __attribute__((weak, alias("Default_Handler")))
WEAK_HANDLER(HardFault_Handler);
WEAK_HANDLER(SysTick_Handler);
WEAK_HANDLER(USBC_IRQHandler);         /* 5 */
WEAK_HANDLER(DMA_IRQHandler);          /* 6 */
WEAK_HANDLER(MAILBOX0_IRQHandler);     /* 9..12 */
WEAK_HANDLER(MAILBOX1_IRQHandler);
WEAK_HANDLER(MAILBOX2_IRQHandler);
WEAK_HANDLER(MAILBOX3_IRQHandler);
WEAK_HANDLER(WDT_IRQHandler);          /* 8 */
WEAK_HANDLER(TIMER1_IRQHandler);       /* 18 */
WEAK_HANDLER(TIMER0_IRQHandler);       /* 19 */
WEAK_HANDLER(SARADC_IRQHandler);       /* 20 */
WEAK_HANDLER(I2S1_IRQHandler);         /* 32 */
WEAK_HANDLER(GPIO2_IRQHandler);        /* 36 */
WEAK_HANDLER(GPIO0_IRQHandler);        /* 38 */

#define D (uint32_t)&Default_Handler
#define AP_IRQS 41

/* Cortex-M3 vector table (aligned 256 for VTOR). [0]=initial MSP, [1]=reset,
 * then the 41 SoC interrupts. Word 0 = 0x03004000 (the stack TOP; the stack
 * is the 16 KiB below it), word 1 = the reset entry, as in the reference. */
const uint32_t ap_vectors[16 + AP_IRQS]
    __attribute__((section(".vectors"), used, aligned(256))) = {
    AP_INITIAL_MSP,                         /* 0x00 initial MSP = 0x03004000 */
    (uint32_t)&PowerOn_Entry,               /* 0x04 Reset_Handler             */
    D,                                      /* 0x08 NMI                      */
    (uint32_t)&HardFault_Handler,           /* 0x0C HardFault                */
    D, D, D,                                /* MemManage, BusFault, UsageFault */
    0, 0, 0, 0,                             /* reserved                      */
    D, D, 0,                                /* SVCall, DebugMon, reserved    */
    D,                                      /* PendSV                        */
    (uint32_t)&SysTick_Handler,             /* 0x3C SysTick                  */
    /* IRQ 0..40 */
    D, D, D, D, D,                          /* SFC SYNTH EBC EMMC SDMMC      */
    (uint32_t)&USBC_IRQHandler,             /* 5  USB OTG                    */
    (uint32_t)&DMA_IRQHandler,              /* 6  DMA                        */
    D, (uint32_t)&WDT_IRQHandler,           /* 7 IMDCT, 8 WDT (services/watchdog.c) */
    (uint32_t)&MAILBOX0_IRQHandler,         /* 9                             */
    (uint32_t)&MAILBOX1_IRQHandler,
    (uint32_t)&MAILBOX2_IRQHandler,
    (uint32_t)&MAILBOX3_IRQHandler,         /* 12                            */
    D, D, D, D, D,                          /* 13..15 rsvd, PWM1, PWM0       */
    (uint32_t)&TIMER1_IRQHandler,           /* 18                            */
    (uint32_t)&TIMER0_IRQHandler,           /* 19                            */
    (uint32_t)&SARADC_IRQHandler,           /* 20                            */
    D, D, D, D, D, D,                       /* UART5..UART0                  */
    D, D, D, D, D,                          /* SPI1 SPI0 I2C2 I2C1 I2C0      */
    (uint32_t)&I2S1_IRQHandler,             /* 32                            */
    D, D, D,                                /* I2S0 HIFI PMU                 */
    (uint32_t)&GPIO2_IRQHandler,            /* 36                            */
    D,                                      /* GPIO1                         */
    (uint32_t)&GPIO0_IRQHandler,            /* 38                            */
    D, D,                                   /* VOP DMA2                      */
};
