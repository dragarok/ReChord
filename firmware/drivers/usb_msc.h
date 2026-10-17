/*
 * usb_msc.h -- polled USB mass storage (DWC2 OTG device at 0x60080000)
 * exposing up to USB_MAX_LUNS block windows as LUNs.
 */
#ifndef RECHORD_USB_MSC_H
#define RECHORD_USB_MSC_H

#include <stdint.h>

enum {
    USB_EXIT_EJECT = 1,      /* host START STOP UNIT with LoEj */
    USB_EXIT_UNPLUG = 2,     /* VBUS gone */
    USB_EXIT_SUSPEND = 3,    /* host suspended after configuration */
    USB_EXIT_NO_HOST = 4,    /* no bus reset within ~3 s (charger) */
    USB_EXIT_IO = 5,         /* USB transfer failed */
    USB_EXIT_USER = 6,       /* user_exit() returned nonzero */
};

int usb_cable(void);         /* 1 = VBUS present */

/* What usb_cable() depends on, for the log (the unit missed a cable plugged in after an RST,
 * 2026-10-16): w[0] = GRF_SOC_USB_STATUS[3:0] | CRU SOFTRST0[2:0] << 4 (USB resets) |
 * CLKGATE0 bit 7 << 7 | CLKGATE6 bit 10 << 8 (USB clocks gated) | pins << 9 (DR, DDR of
 * GPIO0.14, GPIO1.4, GPIO2.0, GPIO2.1, GPIO2.2: two bits each, DR low); w[1] = UOC_CON1 |
 * UOC_CON2 << 16; w[2] = USBPHY_CON6 | CON7 << 16 (the comparator references). */
void usb_cable_state(uint32_t w[3]);

/* Shared DWC2 bring-up (PHY + core + FIFOs + EP0) used by MSC and CDC. */
void usb_dwc2_init(void);
void usb_dwc2_init_hs(void);         /* the same, allowing high speed (the USB DAC) */

/* Shared teardown between USB modes: soft-disconnect (host-visible, held
 * past the hub debounce) + FIFO flush + PHY/clock power-down. */
void usb_dwc2_stop(void);

/* Just the PHY and the USB clocks (and the pins the reference drives with them), for
 * the core in host mode and the VBUS probe; usb_dwc2_phy_off() undoes it. */
void usb_dwc2_phy_on(void);
/* The USB interrupt (NVIC IRQ 5) goes to whichever mode set this (the USB DAC, the host). */
extern void (*volatile usb_irq_hook)(void);
void usb_dwc2_phy_off(void);
/* Idle for a later plug: the PHY run configured (as a session's hw_on) for USB_ARM_MS, then held
 * as a session's end leaves it (usb_dwc2_idle_tick does the hold). The device (ECHOLOG
 * 2026-10-16/17): its comparators track VBUS in the held state only after the PHY has run
 * configured for a while -- after a disk session an unplug (e8 e->c->8) and a plug (e8 e) were
 * seen with the block held; the boot park and every detect-test state, each held ~200 us after
 * the configuration, never saw a plug. Nothing if it is held already. While it runs, usb_cable()
 * keeps the value it had (the comparators read invalid for ~200 ms after the resets are released). */
#define USB_ARM_MS 1000u
void usb_dwc2_park(void);
void usb_dwc2_idle_tick(void);
int usb_dwc2_arming(void);
/* GOTGCTL, GUSBCFG, GINTSTS, PCGCCTL, GHWCFG2..4, GSNPSID, UOC_CON0, DSTS (zeros while held). */
void usb_regs_read(uint32_t r[10]);

/* The USB detect test (startup/ap_ui.c): put the block in candidate idle state n, then the user
 * plugs and unplugs; which states see both settles the device question. */
#define USB_DETECT_STATES 4
extern const char *const usb_detect_names[USB_DETECT_STATES];
void usb_detect_state(int n);

/* 1 once VBUS has been absent for 1 s (stock's 100-tick rule): what a session takes as an unplug.
 * usb_cable_settle() restarts the count (the PHY just came up). */
int usb_cable_gone(void);
void usb_cable_settle(void);

/* Last session: [7:0] last SCSI opcode, [15:8] last START STOP byte 4,
 * [19:16] GINTSTS bits 10..13 ever seen (early suspend, suspend, reset, enum). */
extern uint32_t usb_msc_diag;

#define USB_MAX_LUNS 2

/* [base, base+count) of a block device; count 0 = no medium (fixed for the
 * session), write NULL = write protected. */
typedef struct {
    int (*read)(uint32_t lba, uint32_t count, void *buf);
    int (*write)(uint32_t lba, uint32_t count, const void *buf);
    uint32_t base, count;
} usb_lun_t;

/* Runs the session until one of USB_EXIT_*; user_exit (may be NULL) is
 * polled continuously. nluns: 1..USB_MAX_LUNS. */
int usb_msc_run(const usb_lun_t *luns, unsigned nluns, int (*user_exit)(void),
                uint32_t *rd_sectors, uint32_t *wr_sectors);

#endif
