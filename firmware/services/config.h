/*
 * config.h -- the settings, kept in /Internal/RECHORD.CFG: RECHO's key=value
 * format and values (docs/RECHO_POWER_CFG.md section D), one per line, '#'
 * comments, unknown keys and invalid values ignored. browse/track name folders
 * by path (RECHO: by first clusters).
 */
#ifndef RECHORD_CONFIG_H
#define RECHORD_CONFIG_H

#include <stdint.h>

#define CONFIG_PATH "/Internal/RECHORD.CFG"

enum { CFG_USB_ASK = 0, CFG_USB_DISK, CFG_USB_DAC, CFG_USB_CHARGE };
enum { CFG_RESUME_OFF = 0, CFG_RESUME_FOLDER, CFG_RESUME_TRACK };
/* USB DAC power (dac_charge=off|on|power), stock MINIV390's "UAC charging" modes */
enum { CFG_DAC_BATTERY = 0, CFG_DAC_CHARGE = 1, CFG_DAC_POWER_ONLY = 2 };
#define CFG_VOLUME_MUTE (-128)

typedef struct {
    uint8_t play;                /* services/queue.h Q_* */
    uint8_t brightness;          /* 1..8 */
    uint8_t screen_off;          /* index: 10 s, 30 s, 1 min, 5 min, never */
    uint8_t auto_off;            /* index: 5, 15, 30 min, never */
    uint8_t usb, resume, strip, tags, cpu;   /* cpu: 0 auto, 1 max */
    uint8_t dac_mhz;             /* core A in USB DAC mode: 50, 100 or 200 (MHz) */
    uint8_t usb_out;             /* USB-C earphones: 1 = host mode while no cable (drivers/usb_out.h) */
    uint8_t usb_vbus;            /* what powers them: USB_OUT_SUPPLY_PORT, _GPIO21 */
    uint16_t usb_out_ma;         /* what the port may give a device, 100..500 mA (USB 2.0: 500 configured) */
    uint8_t dac_charge;          /* USB DAC mode: CFG_DAC_BATTERY (draw nothing from the host), _CHARGE, _POWER_ONLY */
    int16_t volume;              /* dB, -90..0, or CFG_VOLUME_MUTE */
    char browse[128];            /* the browser's folder */
    uint16_t cursor;             /* its highlighted row */
    char track[160];             /* the queue: "f:<clus>:<folder>" or "t:<clus>:<tree root>", "" = none */
    /* Bluetooth (drivers/bt_jieli.h): stock keeps the same in its settings block 0x0301D13C:
     * +0x34b enabled, +0x15 BT volume, +0x23e MAC, +0x328 name (08_bluetooth.md §2.4) */
    uint8_t bt;                  /* 1 = module on at boot (and reconnect to bt_mac) */
    uint8_t bt_paired;           /* bt_mac / bt_name hold a remembered device */
    int16_t bt_volume;           /* dB while connected, CFG_BT_VOLUME_UNSET = take the jack's */
    uint8_t bt_mac[6];
    char bt_name[33];
    uint8_t display;             /* the panel's pixel path: CFG_DISPLAY_AUTO or a PANEL_PATH_* */
    uint8_t theme;               /* theme/theme.h built-in index (theme=<name>, Classic by default) */
    uint8_t hifi;                /* hifi=auto|off|verify: core B's FLAC LPC on the HIFI accelerator
                                  * (audio_shared.h AS_HIFI_*, docs/HIFI.md); written when not auto */
    uint8_t cpu_b;               /* cpu_b=96|auto|stock: core B's clock while playing (services/cpuclk.h,
                                  * docs/CPU_CLOCK.md); written when not 96 */
    uint8_t libdata;             /* libdata=sd|internal: where the library's lists and the card's index go
                                  * (services/lib.h LIB_DATA_*); written when not sd */
} config_t;
enum { CFG_CPU_B_96 = 0, CFG_CPU_B_AUTO = 1, CFG_CPU_B_STOCK = 2 };
#define CFG_DISPLAY_AUTO 4       /* = PANEL_PATH_AUTO (drivers/panel_io.h): the build's default path */
#define CFG_BT_VOLUME_UNSET 127

extern config_t g_cfg;

void config_defaults(void);      /* RECHO's defaults; browse/track/volume kept unless all */
void config_reset(void);         /* everything to defaults */
int config_load(void);           /* 0, or < 0 (defaults stay) */
int config_reload_if_changed(void);   /* after a USB disk session: 1 = the PC changed the file, read again */
/* Writes the file when its text changed since the last load/save. 0, or < 0. */
int config_save(void);
/* Ask for a save in 5 s (coalesces bursts); config_poll() does it. config_save_idle(): the same
 * for the volume, which changes in bursts all through a session: that save waits while defer is set
 * (music playing: DESIGN 4.7, no config write for it mid-track; it comes at the pause, the stop,
 * screen off or power off). A setting changed meanwhile is saved at its time, the volume with it. */
void config_save_later(void);
void config_save_idle(void);
void config_poll(int defer);
int config_save_due(void);      /* a save is waiting */

/* Seconds / minutes of the screen_off / auto_off index; 0 = never. */
uint32_t config_screen_off_s(void);
uint32_t config_auto_off_min(void);

#endif
