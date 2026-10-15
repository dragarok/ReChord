/*
 * board_rknanoc.c -- board variant pins (see board.h).
 *
 * 512 MB board, from stock MINIV390 (docs/BOARD_DIFF_512M.md in the
 * snowsky_echo_mini repo; addresses are MINIV390 RAM):
 *   Reset_Handler 0x03062af0: GPIO0.14 output high, GPIO2.1 output high.
 *   Reset_Handler 0x03062eee: GPIO0.14 output high; GPIO2.1 output high if
 *     the panel ID is 0x66666666, else low. The ID is forced to 0x66666666
 *     when the GPIO2.9 strap reads 0, so GPIO2.1 = !GPIO2.9.
 *   power-off 0x03061f62: GPIO0.14 low, GPIO2.1 low, 5 ms, then GPIO2.6 low.
 * On the 8 GB board GPIO0.14 is only ever muxed to a peripheral and GPIO2.1
 * belongs to the DAC driver; board_early_init() leaves both alone there.
 */
#include <stdint.h>

#include "board.h"
#include "panel_io.h"

typedef struct {
    volatile uint32_t dr;
    volatile uint32_t ddr;
    volatile uint32_t unused[18];
    volatile uint32_t ext_porta;
} board_gpio_t;

#define GPIO0 ((board_gpio_t *)0x40160000u)
#define GPIO2 ((board_gpio_t *)0x50030000u)

static void iomux_gpio(unsigned port, unsigned pin)
{
    *(volatile uint32_t *)(0x50010000u + port * 0x20u + (pin >> 3) * 4u) =
        0x30000u << ((pin & 7u) * 2u);
}

static void out(board_gpio_t *g, unsigned port, unsigned pin, int level)
{
    iomux_gpio(port, pin);
    if (level)
        g->dr |= 1u << pin;
    else
        g->dr &= ~(1u << pin);
    g->ddr |= 1u << pin;
}

static int s_strap = -1;              /* GPIO2.9 at boot: 0 = the 0x66666666 board */

void board_early_init(void)
{
#if BOARD == BOARD_512M
    out(GPIO0, 0, 14, 1);
    out(GPIO2, 2, 1, 1);
    iomux_gpio(2, 9);
    GPIO2->ddr &= ~(1u << 9);
    s_strap = (int)((GPIO2->ext_porta >> 9) & 1u);
    out(GPIO2, 2, 1, !s_strap);
#endif
}

int board_usb_supply(int mode)
{
#if BOARD == BOARD_512M
    if (s_strap != 1)                    /* the 0x66666666 board: what the pins do there is unknown */
        return -1;
    panel_io_gpio0_pin(14, mode == BOARD_SUPPLY_NORMAL);   /* (GPIO0 goes through the panel's shadow) */
    out(GPIO2, 2, 1, mode == BOARD_SUPPLY_VBUS);           /* (NORMAL: !strap = 0 on this board) */
    return 0;
#else
    (void)mode;
    return -1;
#endif
}

/* Called by board_power_off() before the supply is cut. */
void board_power_off_pins(void)
{
#if BOARD == BOARD_512M
    out(GPIO0, 0, 14, 0);
    out(GPIO2, 2, 1, 0);
#endif
}

int board_check(uint32_t emmc_sectors)
{
    int small = emmc_sectors != 0 && emmc_sectors < (4u << 21);   /* < 4 GiB */

    return BOARD == BOARD_512M ? small : !small;
}

const char *board_name(void)
{
    return BOARD == BOARD_512M ? "512M" : "8G";
}

int board_adc_ch2(void)
{
#if BOARD == BOARD_512M
    extern int adc_value(unsigned ch);

    return adc_value(2);
#else
    return -1;
#endif
}

/* GPIO2.3 (RECHO and stock): with the cable in, 1 = charged, 0 = charging. */
int board_charge_done(void)
{
    iomux_gpio(2, 3);
    GPIO2->ddr &= ~(1u << 3);
    return (int)((GPIO2->ext_porta >> 3) & 1u);
}
