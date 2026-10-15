/*
 * bootguard.c -- the crash record and the reset after a fault (bootguard.h).
 *
 * The record lives in the 16 spare bytes above the PCM ring's DMA descriptors (AS_LLI + 8 x 28 B
 * ends at 0x0308FFE0): not in the loaded image, not .bss, not touched by core B. A magic and a
 * check word make random or stale RAM read as "no record".
 */
#include <stdint.h>

#include "bootguard.h"
#include "drivers/clock.h"
#include "drivers/panel_io.h"

#define REC        ((volatile bootguard_rec_t *)0x0308FFE8u)
#define MAGIC      0x44524752u                      /* "RGRD" */
#define SCB_AIRCR  (*(volatile uint32_t *)0xE000ED0Cu)

static uint32_t check(uint32_t count, uint32_t pc, uint32_t lr)
{
    return MAGIC ^ count * 0x9E3779B1u ^ pc ^ (lr << 1);
}

static int valid(void)
{
    return REC->magic == MAGIC && REC->check == check(REC->count, REC->pc, REC->lr) && REC->count &&
           REC->count < 1000u;
}

void bootguard_fault(const uint32_t *frame, uint32_t exc)
{
    uint32_t n, pc, lr;

    if ((uint32_t)frame < 0x03000000u || (uint32_t)frame > 0x0309FFE0u)
        frame = 0;                                   /* (a stack pointer gone wild: no frame) */
    n = valid() ? REC->count + 1u : 1u;
    pc = frame ? frame[6] : 0xFFFFFF00u | exc;
    lr = frame ? frame[5] : 0;

    __asm__ volatile("cpsid i");
    REC->magic = MAGIC;
    REC->count = n;
    REC->pc = pc;
    REC->lr = lr;
    REC->check = check(n, pc, lr);
    __asm__ volatile("dsb");
    if (n >= BOOTGUARD_MAX_CRASHES)
        board_power_off();                           /* no reset loop: off, the record kept */
    SCB_AIRCR = 0x05FA0004u;                         /* SYSRESETREQ: the ROM boots us again */
    __asm__ volatile("dsb");
    for (;;)
        ;
}

int bootguard_crashed(bootguard_rec_t *rec)
{
    if (!valid())
        return 0;
    rec->magic = REC->magic;
    rec->count = REC->count;
    rec->pc = REC->pc;
    rec->lr = REC->lr;
    rec->check = REC->check;
    return 1;
}

void bootguard_clear(void)
{
    REC->magic = 0;
    REC->count = 0;
}

void bootguard_poll(void)
{
    static int done;

    if (!done && time_ms() >= BOOTGUARD_STABLE_MS) {
        done = 1;
        if (valid())
            bootguard_clear();
    }
}
