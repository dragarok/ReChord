/*
 * usb_out.h -- USB audio out: the player as USB host to a USB Audio Class 1 or 2 device
 * (a USB-C earphone or dongle DAC) on the USB-C port.
 *
 * The DWC2 core runs as host in slave mode (FIFOs), all from the USB interrupt: port
 * reset, enumeration (control transfers on channel 0), and per (micro)frame an
 * isochronous OUT packet built straight from core B's PCM ring. The ring is consumed the
 * way the I2S DMA does it (audio_shared.h): block by block along the LLI chain, DONE
 * written back into each LLI, the chain's end ends the stream -- so core B, gapless and
 * the ring are the same as for the headphone jack. Asynchronous devices pace the packets
 * through their feedback endpoint, adaptive and synchronous ones get the nominal rate.
 * Bit-perfect: the track's own rate, the deepest PCM format the device has (a 16-bit
 * track is exact in 24 or 32 bits); volume is the device's feature unit, or, if it has
 * none, a gain on the samples.
 *
 * The 5 V on VBUS: what the board can drive is not known yet (docs/USB_HOST_AUDIO.md
 * section 0; the USB-C probe measures it). usb_out_start() switches on what the setting
 * names: the PHY's DRVVBUS and the core's port power always, GPIO2.1 with
 * USB_OUT_SUPPLY_GPIO21.
 */
#ifndef RECHORD_USB_OUT_H
#define RECHORD_USB_OUT_H

#include <stdint.h>

enum { USB_OUT_OFF = 0, USB_OUT_WAIT, USB_OUT_READY, USB_OUT_UNSUPPORTED, USB_OUT_ERROR };
enum { USB_OUT_SUPPLY_PORT = 0, USB_OUT_SUPPLY_GPIO21 = 1 };

typedef struct {
    uint32_t min, max, res;
} usb_out_range_t;

typedef struct {
    int state;                    /* USB_OUT_* */
    int high_speed, uac;          /* uac: 1 or 2 */
    char name[32];                /* the device's product string */
    usb_out_range_t rates[8];     /* what it accepts (UAC1: of the deepest alternate) */
    uint32_t nrates;
    uint32_t bits_max;            /* deepest PCM stereo format */
    int hw_volume;                /* a feature unit with volume */
    int async;                    /* a feedback endpoint paces the stream */
    /* the stream */
    uint32_t rate, bits, subslot; /* bits 0 = not streaming */
    uint32_t packets, late, errors, fb_q16;   /* fb_q16: frames per packet from the feedback */
} usb_out_status_t;

/* Host mode on: PHY, host role, VBUS (supply: USB_OUT_SUPPLY_*), port power. */
int usb_out_start(int supply);
void usb_out_stop(void);
int usb_out_active(void);          /* between start and stop (VBUS is the player's own) */
/* Every main-loop tick: a device plugged in is reset and enumerated (blocking, ~0.3 s),
 * one unplugged dropped. Returns the state. */
int usb_out_poll(void);
const usb_out_status_t *usb_out_status(void);

/* The audio sink (audio.c). */
int usb_out_set_format(uint32_t rate, uint32_t bits);   /* 0, or < 0: the device cannot */
void usb_out_stream_start(void);   /* consume the LLI chain from AS_LLI, block 0 */
void usb_out_stream_stop(void);    /* silence */
void usb_out_release(void);        /* playing stopped: alternate 0, the device idles (no packets) */
int usb_out_streaming(void);       /* 1 until the chain has ended (or stopped) */
void usb_out_pause(int on);        /* silence, the ring kept */
/* dB, -60..0: the device's feature unit (1), or a gain on the samples (0). */
int usb_out_set_volume(int db);

#endif
