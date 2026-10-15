/* Core B reset and vector table (linked at 0, see coreb.ld). */
#include <stdint.h>

extern uint32_t _b_bss[], _b_bss_end[], _b_stack_top[], _b_data[], _b_data_end[], _b_data_load[];
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
