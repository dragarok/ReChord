/*
 * input.c — key event queue + ADC threshold mapping (see input.h).
 *
 * The queue and mapping are ours; the hardware hookup is the only part that
 * touches the SDK and is marked TODO below. No third-party code is used.
 */
#include "watchdog.h"
#include "input.h"

#include <stddef.h>

static input_event_t s_queue[INPUT_QUEUE_LEN];
static unsigned s_head;      /* index of the oldest queued event */
static unsigned s_count;     /* events currently queued */

/*
 * ADC ladder bands (10-bit, SAR-ADC channel 1, idle 1023). Measured on
 * hardware: vol- 0, vol+ 121..177, play 245..299, menu 321..395,
 * back 483..549, idle >= 1001. Bands sit between those clusters; readings
 * in the gaps map to no key. Power is a separate GPIO (input_scan).
 */
typedef struct {
    int type;
    int adc_min;
    int adc_max;
} adc_band_t;

static const adc_band_t s_adc_bands[] = {
    { KEY_DOWN,     0,   69 },   /* vol- */
    { KEY_UP,      70,  183 },   /* vol+ */
    { KEY_SELECT, 200,  319 },   /* play */
    { KEY_MENU,   320,  409 },   /* menu */
    { KEY_BACK,   450,  549 },   /* back */
};

static int s_scan_last = KEY_NONE;   /* previous raw sample   */
static int s_scan_held = KEY_NONE;   /* debounced key, if any */

#define ADC_IDLE 1023   /* documented idle reading: no key pressed */

int input_map_adc(int adc_value)
{
    unsigned i;

    if (adc_value < 0 || adc_value >= ADC_IDLE) {
        return KEY_NONE;
    }
    for (i = 0; i < sizeof(s_adc_bands) / sizeof(s_adc_bands[0]); i++) {
        if (adc_value >= s_adc_bands[i].adc_min &&
            adc_value <= s_adc_bands[i].adc_max) {
            return s_adc_bands[i].type;
        }
    }
    return KEY_NONE;
}

int input_push(int type, int adc_value)
{
    input_event_t *slot;

    if (type < KEY_UP || type > KEY_CHORD) {
        return INPUT_ERR_ARG;
    }
    if (s_count >= (unsigned)INPUT_QUEUE_LEN) {
        return INPUT_ERR_FULL;  /* dropped; the UI is not draining fast enough */
    }
    slot = &s_queue[(s_head + s_count) % (unsigned)INPUT_QUEUE_LEN];
    slot->type = type;
    slot->adc_value = adc_value;
    s_count++;
    return INPUT_OK;
}

int input_poll(input_event_t *ev)
{
    if (ev == NULL) {
        return INPUT_ERR_ARG;
    }
    if (s_count == 0) {
        return 0;
    }
    *ev = s_queue[s_head];
    s_head = (s_head + 1) % (unsigned)INPUT_QUEUE_LEN;
    s_count--;
    return 1;
}

static unsigned s_held_scans;        /* scans the debounced key has been held */
static int s_pressed;                /* the held press-acting key has acted */

/* UP/DOWN act on the press (and repeat while held); the other keys have a press
 * and a hold meaning, so they act on release (short) or at INPUT_LONG_SCANS. */
static int acts_on_press(int key)
{
    return key == KEY_UP || key == KEY_DOWN;
}

int input_scan(int adc_value, int power)
{
    int key = power ? KEY_POWER : input_map_adc(adc_value), prev, r = 0;

    watchdog_feed();             /* a loop that scans the keys is alive */
    if (key != s_scan_last) {          /* not stable yet */
        s_scan_last = key;
        return 0;
    }
    if (s_scan_held == KEY_CHORD) {    /* nothing more until every key is up */
        if (key == KEY_NONE) {
            s_scan_held = KEY_NONE;
            s_held_scans = 0;
        }
        return 0;
    }
    if (key == s_scan_held) {
        if (key == KEY_NONE)
            return 0;
        s_held_scans++;
        if (acts_on_press(key)) {
            /* a press acts once its band has held INPUT_PRESS_SCANS: a soft press of another key
             * sweeps through this band for tens of ms on its way (device 2026-10-16) */
            if (s_held_scans == INPUT_PRESS_SCANS && !s_pressed) {
                s_pressed = 1;
                return input_push(key, adc_value) == INPUT_OK;
            }
            if (s_held_scans >= INPUT_REPEAT_START &&
                (s_held_scans - INPUT_REPEAT_START) % INPUT_REPEAT_EVERY == 0)
                return input_push(key, INPUT_ADC_REPEAT) == INPUT_OK;
        } else if (s_held_scans == INPUT_LONG_SCANS) {
            return input_push(key, INPUT_ADC_LONG) == INPUT_OK;
        }
        return 0;
    }
    prev = s_scan_held;
    /* RECHO's chord: one volume key held (now INPUT_CHORD_SCANS, a deliberate hold, not the
     * sweep of a soft press), then the ladder reads the other */
    if (((prev == KEY_UP && key == KEY_DOWN) || (prev == KEY_DOWN && key == KEY_UP)) &&
        s_held_scans >= INPUT_CHORD_SCANS) {
        s_scan_held = KEY_CHORD;
        s_held_scans = 0;
        return input_push(KEY_CHORD, adc_value) == INPUT_OK;
    }
    if (prev != KEY_NONE && key != KEY_NONE && prev != KEY_POWER && key != KEY_POWER) {
        /* one ladder key to another with no release between: the voltage is still settling and
         * the lowest reading wins (drivers: keys_adc), so no event for the one it passed; as stock's
         * KeyScan 0x0306240a, the count goes on (work/stock_boot/05_keys_adc.md 1.3) */
        s_scan_held = key;
        if (acts_on_press(key) && !s_pressed)
            s_held_scans = 0;          /* (a press key counts its own settling time) */
        return 0;
    }
    if (prev != KEY_NONE) {
        if (!acts_on_press(prev) && s_held_scans < INPUT_LONG_SCANS)
            r = input_push(prev, prev == KEY_POWER ? -1 : 0) == INPUT_OK;   /* short press, on release */
        else if (acts_on_press(prev) && !s_pressed)
            r = input_push(prev, 0) == INPUT_OK;      /* a tap released before it acted: now */
    }
    s_scan_held = key;
    s_held_scans = 0;
    s_pressed = 0;
    return r;
}

void input_mock_clear(void)
{
    s_head = 0;
    s_count = 0;
    s_scan_last = KEY_NONE;
    s_scan_held = KEY_NONE;
    s_pressed = 0;
}

unsigned input_pending(void)
{
    return s_count;
}
