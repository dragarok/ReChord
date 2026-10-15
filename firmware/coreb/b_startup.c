/* Core B reset and vector table (linked at 0, see coreb.ld). */
#include <stdint.h>

extern uint32_t _b_bss[], _b_bss_end[], _b_stack_top[], _b_data[], _b_data_end[], _b_data_load[];
extern uint32_t _b_stack_limit[];
int b_main(void);
void SysTick_Handler(void);

void b_reset(void) __attribute__((noreturn));
void b_reset(void)
{
    const uint32_t *s = _b_data_load;

    for (uint32_t *p = _b_data; p < _b_data_end; p++)
        *p = *s++;
    for (uint32_t *p = _b_bss; p < _b_bss_end; p++)
        *p = 0;
    /* paint the stack reserve below what this function uses (b_main.c reads it back) */
    {
        uint32_t *p = _b_stack_limit, *sp;

        __asm__ volatile ("mov %0, sp" : "=r"(sp));
        while (p < sp - 16)
            *p++ = 0x5A5A5A5Au;
    }
    *(volatile uint32_t *)0xE000ED08u = 0;      /* VTOR: our table at 0 */
    b_main();
    for (;;) { }
}

static void b_fault(void)
{
    for (;;) { }
}

__attribute__((section(".vectors"), used))
static void (*const b_vectors[16])(void) = {
    (void (*)(void))_b_stack_top, b_reset, b_fault, b_fault,
    b_fault, b_fault, b_fault, 0, 0, 0, 0,
    b_fault, b_fault, 0, b_fault, SysTick_Handler,
};
