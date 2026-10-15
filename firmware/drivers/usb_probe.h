/*
 * usb_probe.h -- what can put voltage on the USB-C port's VBUS, measured by the SoC itself.
 *
 * The Inno USB PHY has four VBUS comparators (GRF_SOC_USB_STATUS 0x500100E8: bit 0 VBUS
 * valid, 1 B-session valid, 2 A-session valid, 3 session end), each with a 3-bit
 * reference (GRF_USBPHY_CON6/CON7). Sweeping the eight codes of each and recording which
 * trip brackets the voltage on the pin: an on-chip voltmeter. The probe does that with no
 * cable in, once as the board is and once per candidate (each a pin state stock firmware
 * itself uses): GPIO2.1 (stock's VBUS -> system path), GPIO2.0, GPIO0.14, the PHY's
 * DRVVBUS, the controller as host with port power, all of them at once, and last the PHY's
 * own weak SRP source (a small known rise: a reference for the comparator codes). As a
 * host it also looks for a device's D+ pull-up: a USB-C earphone plugged in that powers
 * up connects. Everything is put back as it was.
 */
#ifndef RECHORD_USB_PROBE_H
#define RECHORD_USB_PROBE_H

#include <stdint.h>

typedef struct {
    const char *name;
    uint8_t vbus, bsess, asess, sessend;   /* bit n: that comparator tripped at reference code n */
    int16_t adc[4];                        /* SAR-ADC channels 0..3 (-1 = none) */
    int8_t host;                           /* -1 not a host step; else HPRT: 1 = a device connected */
    uint8_t linestate;                     /* host steps: HPRT.PrtLnSts (bit 0 D+, bit 1 D-) */
} usb_probe_step_t;

#define USB_PROBE_STEPS 8

/* The USB PHY's configuration registers as the probe found them after bring-up (GRF
 * USBPHY_CON0..11 at 0x50010080, UOC_CON0..2 at 0x500100B0): what the silicon holds in the
 * fields no firmware writes (ODT compensation, driver strength, HS eye height). */
extern uint32_t usb_probe_phy[15];

/* Runs every step (about 1.5 s). Returns the number of steps, or -1 if VBUS is already
 * present (a cable: the probe needs the port free; an earphone is fine). */
int usb_probe_run(usb_probe_step_t *out, void (*progress)(int step, const char *name));

#endif
