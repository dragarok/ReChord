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
#define CFG_VOLUME_MUTE (-128)

typedef struct {
    uint8_t play;                /* services/queue.h Q_* */
    uint8_t brightness;          /* 1..8 */
    uint8_t screen_off;          /* index: 10 s, 30 s, 1 min, 5 min, never */
    uint8_t auto_off;            /* index: 5, 15, 30 min, never */
    uint8_t usb, resume, strip, tags, cpu;   /* cpu: 0 auto, 1 max */
    uint8_t dac_mhz;             /* core A in USB DAC mode: 50, 100 or 200 (MHz) */
    int16_t volume;              /* dB, -90..0, or CFG_VOLUME_MUTE */
    char browse[128];            /* the browser's folder */
    uint16_t cursor;             /* its highlighted row */
    char track[160];             /* the queue: "f:<clus>:<folder>" or "t:<clus>:<tree root>", "" = none */
} config_t;

extern config_t g_cfg;

void config_defaults(void);      /* RECHO's defaults; browse/track/volume kept unless all */
void config_reset(void);         /* everything to defaults */
int config_load(void);           /* 0, or < 0 (defaults stay) */
/* Writes the file when its text changed since the last load/save. 0, or < 0. */
int config_save(void);
/* Ask for a save in 5 s (coalesces volume key bursts); config_poll() does it. */
void config_save_later(void);
void config_poll(void);

/* Seconds / minutes of the screen_off / auto_off index; 0 = never. */
uint32_t config_screen_off_s(void);
uint32_t config_auto_off_min(void);

#endif
