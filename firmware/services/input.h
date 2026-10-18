/*
 * input.h -- ReChord key input: sampling every 10 ms, a gesture recogniser, an event queue.
 *
 * The keys are sampled on core A's SysTick (10 ms, drivers: keys_tick -> input_sample), not once
 * per main-loop pass, so a pass held up by the storage or a redraw neither loses nor stretches a
 * key (DESIGN 3.3, 4.2; OURS_AUDIT 1.5). Each sample goes through the recogniser below, which
 * queues timestamped gestures; the UI takes them with input_next() at its own pace.
 *
 * Gestures (one set of numbers for every key, DESIGN 3.3):
 *   IG_DOWN     the key is down. VOL- and POWER at once (2 agreeing samples); the keys a lower
 *               key's press sweeps past on the ladder (VOL+, PLAY, MENU, BACK) once their band
 *               has held INPUT_SWEEP_MS (device 2026-10-16: a soft VOL- press read as VOL+ for
 *               tens of ms), or at their release if that came first.
 *   IG_TAP      released before the hold time. taps = its place in a multi-press: 1, 2 (x2), 3 (x3)
 *               when the same key was pressed within INPUT_MULTI_GAP_MS of the last release. It
 *               comes at once on every release: a first tap never waits for a second one.
 *   IG_HOLD     held for the hold time (500 ms, input_set_hold_ms), still down; taps 2/3 =
 *               2x+hold / 3x+hold. input_hold_progress() fills 0..10 on the way.
 *   IG_REPEAT   VOL+/VOL- held: from 400 ms on, `steps` rows (or dB) to move now, accelerating as
 *               input_set_repeat() says (DESIGN 4.4). Repeats the UI has not taken yet are merged
 *               into one event (steps add up): nothing is lost, the cursor never lags behind.
 *   IG_RELEASE  released after a HOLD or a REPEAT (seek: "release = go").
 *   IG_END      the multi-press window closed: no new press INPUT_MULTI_GAP_MS after the last tap
 *               (taps = the count reached).
 *   IG_CHORD    VOL+ and VOL- together: one held INPUT_CHORD_MS, then the other (the ladder reads
 *               the lower key). Nothing more until every key is up.
 *
 * The old events (input_poll(): UP/DOWN on the press and repeating, the other keys on release or
 * INPUT_ADC_LONG at the hold time, KEY_CHORD) are made from the gestures, so the current screens
 * keep working until the UI rewrite takes input_next(). Use one of the two, not both.
 */
#ifndef RECHORD_SERVICES_INPUT_H
#define RECHORD_SERVICES_INPUT_H

#include <stdint.h>

/* Keys. The enum is named so both services and UI code can use the `input_key_t` type. */
typedef enum input_key {
    KEY_NONE = 0,   /* not a key event (ADC idle) */
    KEY_UP = 1,     /* VOL+ */
    KEY_DOWN = 2,   /* VOL- */
    KEY_SELECT = 3, /* PLAY */
    KEY_BACK = 4,
    KEY_POWER = 5,
    KEY_MENU = 6,
    KEY_CHORD = 7   /* VOL+ and VOL- together (RECHO's key 7: shortcuts) */
} input_key_t;

typedef enum input_gesture {
    IG_NONE = 0, IG_DOWN, IG_TAP, IG_HOLD, IG_REPEAT, IG_RELEASE, IG_END, IG_CHORD
} input_gesture_t;

/* One key event.
 *   type, adc_value -- the old event (input_poll): type = KEY_*, adc_value = the ADC reading, -1
 *                      for the power key, INPUT_ADC_LONG (a hold) or INPUT_ADC_REPEAT. input_next()
 *                      sets type = key and adc_value = 0.
 *   the rest        -- the gesture (input_next(); input_poll() fills them too). */
typedef struct {
    int type;
    int adc_value;
    uint8_t key;        /* KEY_* (KEY_CHORD for IG_CHORD) */
    uint8_t gesture;    /* IG_* */
    uint8_t taps;       /* TAP, HOLD, RELEASE, END: 1..3 */
    uint8_t steps;      /* REPEAT: rows or dB to move now (>= 1) */
    uint16_t repeat;    /* REPEAT: steps since the press, these included */
    uint16_t held_ms;   /* how long the key had been down (65535 at most) */
    uint16_t in_ms;     /* the finger's edge to this event: the press to a DOWN (stability, sweep), the
                         * release (t_ms) to a TAP or a RELEASE (the ladder back at idle, then stable);
                         * 0 when nothing waited (key->px's "in", drivers/display.h) */
    uint32_t t_ms;      /* when it happened: the 10 ms sample that showed it; a TAP or a RELEASE: the
                         * release edge */
} input_event_t;

/* ---- the gesture API (the new UI) ---- */

/* Pop the oldest gesture into *ev: 1, or 0 when there is none (INPUT_ERR_ARG for NULL). */
int input_next(input_event_t *ev);
/* 0..10: how far the key now down is towards its hold (10 = the hold fired); 0 when it is up. */
int input_hold_progress(int key);
/* ms the key has been down (0 when up): POWER's 1 s menu / 3 s off, the letter jump at 3 s. */
uint32_t input_held_ms(int key);
/* The hold time: 400, 500 (default) or 700 ms (Settings "Hold time"). */
void input_set_hold_ms(uint32_t ms);
/* How VOL+/VOL- repeat: lists (8, 16, 32 rows/s after 0.4, 1, 2 s) or volume (1 dB a 100 ms from
 * 0.4 s, 2 dB a 100 ms after 1 s). */
enum { INPUT_REPEAT_LIST = 0, INPUT_REPEAT_VOLUME = 1 };
void input_set_repeat(int profile);
/* "VOLP", "VOLM", "PLAY", "MENU", "BACK", "POWER", "CHORD" and "tap", "x2", "x3", "hold", "x2hold",
 * "x3hold", "down", "repeat", "release", "end", "chord" (logs: "ui: key PLAY.x2 ..."). */
const char *input_key_name(int key);
const char *input_gesture_name(const input_event_t *ev);
/* Gestures lost to a full queue since boot (the UI took none for INPUT_QUEUE_LEN of them). */
unsigned input_dropped(void);

/* ---- sampling ---- */

/* One sample: the ladder reading (drivers: keys_adc(), the stable band's value) and the power key
 * (1 = pressed) at now_ms. Called every INPUT_TICK_MS from core A's SysTick once
 * input_isr_sampling(1); before that (and on the host), input_scan() does it. */
void input_sample(int adc_value, int power, uint32_t now_ms);
/* The same with the ladder's edges (drivers: keys_tick; 0 = not known): press_ms when the
 * reading left idle, release_ms when it rose off the held key's level. A press is then timed from
 * the finger, not from when its band was stable (which moves 20..100 ms with how fast the voltage
 * settles and where the samples fall): holds at 500 ms, x2/x3 gaps and held_ms as pressed. */
void input_sample_edges(int adc_value, int power, uint32_t now_ms, uint32_t press_ms, uint32_t release_ms);
/* 1: the samples come from the timer (input_scan() then only says the loop is alive). */
void input_isr_sampling(int on);
int input_isr_active(void);
/* The clock input_scan() stamps its samples with (NULL: INPUT_TICK_MS per call, the host tests). */
void input_set_clock(uint32_t (*now_ms)(void));

/* ---- the old API (the current screens; an adapter over the gestures) ---- */

/* Pop the oldest old-style event into *ev (UP/DOWN: on the press, then INPUT_ADC_REPEAT; SELECT,
 * BACK, MENU, POWER: on the release, or INPUT_ADC_LONG at the hold; KEY_CHORD). 1, 0 when empty,
 * INPUT_ERR_ARG for NULL. The gesture fields say which gesture it came from. */
int input_poll(input_event_t *ev);

/* Called with every gesture input_poll() takes from the queue, old event or not (the log). */
void input_set_observer(void (*fn)(const input_event_t *ev));

/* Push an old-style event (scripts and host tests): INPUT_OK, INPUT_ERR_ARG for an unknown type,
 * INPUT_ERR_FULL when the queue is full (dropped). */
int input_push(int type, int adc_value);

/* Map a raw ADC reading to a key type (KEY_NONE when idle/unmapped). Bands measured on hardware
 * (see input.c). */
int input_map_adc(int adc_value);

/* A key sample from the main loop: feeds the watchdog (a loop that scans the keys is alive) and,
 * while the timer does not sample, samples. Returns 1 if an event became pending. */
int input_scan(int adc_value, int power);

/* Drop every queued event; a key still down is ignored until it is released (a screen that
 * starts while its key is still held does not see it). */
void input_mock_clear(void);

/* Number of queued gestures. */
unsigned input_pending(void);

#define INPUT_TICK_MS        10u    /* the sampling period */
#define INPUT_SWEEP_MS       60u    /* VOL+, PLAY, MENU, BACK: DOWN once the band held this long */
#define INPUT_HOLD_MS        500u   /* default hold (stock 400, RECHO 600) */
#define INPUT_MULTI_GAP_MS   300u   /* release to the next press, for x2 / x3 */
#define INPUT_CHORD_MS       150u   /* the first volume key held this long before the second */
#define INPUT_REPEAT_MS      400u   /* VOL+/VOL- held: the first repeat */

#define INPUT_ADC_LONG     (-2)
#define INPUT_ADC_REPEAT   (-3)

/* Queue capacity (gestures). */
#ifndef INPUT_QUEUE_LEN
#define INPUT_QUEUE_LEN 32
#endif

#define INPUT_OK        0
#define INPUT_ERR_ARG (-1)
#define INPUT_ERR_FULL (-2)

#endif /* RECHORD_SERVICES_INPUT_H */
