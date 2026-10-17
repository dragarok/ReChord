/*
 * power.h -- screen, backlight, core-A clock, battery and the idle timers, as
 * RECHO 0.9.2 does them (docs/RECHO_POWER_CFG.md A, C.2, C.5, C.6):
 *  - brightness 1..8 -> PWM duty {99, 98, 96, 93, 88, 75, 50, 100};
 *  - the screen goes off (backlight only) after g_cfg's screen_off with no key;
 *    core A then drops from 200 to 50 MHz unless cpu=max or a USB session;
 *  - auto power off after auto_off minutes idle: not playing, no USB power;
 *  - battery from SAR-ADC channel 0 every 10 s: mV = raw * 5000 >> 10, RECHO's
 *    percent table and level thresholds, VBUS and the charge-done pin, mV/min
 *    and %/h over a 30-minute ring, time left.
 * Beyond RECHO (stock's cut-off, A.4): three readings in a row below 3450 mV on
 * battery power the player off before the cell is run flat.
 */
#ifndef RECHORD_POWER_H
#define RECHORD_POWER_H

#include <stdint.h>

typedef struct {
    uint16_t mv;                 /* 0 = not measured yet */
    uint16_t pct10;              /* 0..1000 (0.1 %) */
    uint8_t level;               /* 0..4 (the icon) */
    uint8_t vbus, full;          /* USB power in; with it, charged */
    uint8_t rate_ok;             /* mv_min valid (90 s on battery) */
    int16_t mv_min;              /* mV per minute, over up to 5 min */
    int16_t pct_h10[3];          /* %/h x10 over 1, 5, 30 min; 0 = not yet */
    uint16_t left_min;           /* minutes left, 0 = not known */
} power_battery_t;

enum { POWER_NONE = 0, POWER_OFF_IDLE, POWER_OFF_BATTERY };

void power_init(void);           /* after config_load(): the backlight at g_cfg.brightness */
/* Every main-loop pass. Returns POWER_OFF_* when the player should switch off.
 * usb_session: 0, 1 (USB disk / console: core A at 200 MHz) or POWER_SESSION_DAC
 * (the USB DAC: core A at g_cfg.dac_mhz). */
#define POWER_SESSION_DAC 2
int power_poll(int playing, int usb_session);
/* 1: the cable is in but the player draws nothing from it (USB DAC, dac_charge=off): the
 * battery is shown, rated and guarded as with no cable (a low battery ends the session). */
void power_usb_supply_off(int off);
void power_activity(void);       /* a key: restarts the screen-off and idle timers */
uint32_t power_key_idle_ms(void);  /* since the last key */
int power_screen_on(void);
void power_screen(int on);       /* backlight + core-A clock; logged */
void power_brightness_changed(void);
const power_battery_t *power_battery(void);
/* "68% 3.91V", "3.91V chg", "3.91V full", or "..." before the first reading */
const char *power_battery_text(void);

#endif
