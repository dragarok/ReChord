/*
 * usb_uac.h -- the player as a USB DAC: USB Audio Class 2.0, asynchronous, the
 * CS43131's clock is the master and the host follows the feedback endpoint.
 * Bit-perfect: the samples go untouched into the I2S ring (32-bit as is, 24-bit
 * and 16-bit left-justified); host volume and mute are the DAC's own registers,
 * with the player's own volume (audio_get_volume_db(), its keys) added on top.
 * 44.1 to 192 kHz at high speed (96 kHz at full speed), 32/24/16-bit stereo.
 * Core B stays in reset; core A runs from the USB interrupt and sleeps otherwise.
 */
#ifndef RECHORD_USB_UAC_H
#define RECHORD_USB_UAC_H

#include <stdint.h>

typedef struct {
    uint32_t rate, bits;          /* what the host chose; bits 0 = not streaming */
    int high_speed, configured;
    int32_t volume_256db;         /* host volume, 1/256 dB */
    int local_db;                 /* the player's volume (its keys), added to the host's */
    int mute;
    uint32_t fill;                /* frames in the ring */
    uint32_t packets, dropped, underruns, missed;   /* since the session began */
} usb_uac_status_t;

/* Runs the USB DAC session until the cable goes or tick() returns nonzero (called
 * every 10 ms tick: keys, screen, power); status() about once a second for the
 * screen. Returns USB_EXIT_* (usb_msc.h). */
int usb_uac_run(int (*tick)(void), void (*status)(const usb_uac_status_t *));

/* Before usb_uac_run(): 1 = the player runs on its own battery (draws nothing from
 * VBUS): the configuration says self-powered, 2 mA; 0 = bus powered, 100 mA. */
void usb_uac_self_powered(int on);

/* A media key for the host (HID consumer control: press, then release), from
 * tick(). Ignored until the host has configured the device. */
enum { USB_UAC_PLAY_PAUSE, USB_UAC_NEXT, USB_UAC_PREV };
void usb_uac_key(int key);

#endif
