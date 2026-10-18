/*
 * input.c -- key sampling, the gesture recogniser and the event queue (see input.h).
 *
 * The recogniser keeps one key at a time (the ladder reads one band; the power key wins over it,
 * as before) and works in milliseconds from the samples' own timestamps, so it does not matter
 * when the main loop gets round to the events. The queue is filled from core A's SysTick and
 * emptied by the main loop: one producer, one consumer, the consumer's side with interrupts off
 * for the few instructions it takes.
 */
#include "watchdog.h"
#include "input.h"

#include <stddef.h>

#if defined(__arm__)
static uint32_t lock(void)
{
    uint32_t m;

    __asm__ volatile ("mrs %0, primask\n cpsid i" : "=r" (m) :: "memory");
    return m;
}
static void unlock(uint32_t m) { __asm__ volatile ("msr primask, %0" :: "r" (m) : "memory"); }
#else
static uint32_t lock(void) { return 0; }
static void unlock(uint32_t m) { (void)m; }
#endif

/*
 * ADC ladder bands (10-bit, SAR-ADC channel 1, idle 1023). Measured on
 * hardware: vol- 0, vol+ 121..177, play 245..299, menu 321..395,
 * back 483..549, idle >= 1001. Bands sit between those clusters; readings
 * in the gaps map to no key. Power is a separate GPIO (input_sample).
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

/* ---- the queue ---- */

static input_event_t s_queue[INPUT_QUEUE_LEN];
static volatile unsigned s_head, s_count;   /* oldest, queued */
static unsigned s_dropped;

static input_event_t *q_tail(void)
{
    return s_count ? &s_queue[(s_head + s_count - 1u) % (unsigned)INPUT_QUEUE_LEN] : NULL;
}

static int q_put(const input_event_t *ev)
{
    if (s_count >= (unsigned)INPUT_QUEUE_LEN) {
        s_dropped++;
        return INPUT_ERR_FULL;  /* dropped; the UI is not draining fast enough */
    }
    s_queue[(s_head + s_count) % (unsigned)INPUT_QUEUE_LEN] = *ev;
    s_count++;
    return INPUT_OK;
}

static int q_get(input_event_t *ev)
{
    uint32_t m = lock();
    int r = 0;

    if (s_count) {
        *ev = s_queue[s_head];
        s_head = (s_head + 1u) % (unsigned)INPUT_QUEUE_LEN;
        s_count--;
        r = 1;
    }
    unlock(m);
    return r;
}

int input_push(int type, int adc_value)
{
    input_event_t ev = { 0 };
    uint32_t m;
    int r;

    if (type < KEY_UP || type > KEY_CHORD) {
        return INPUT_ERR_ARG;
    }
    ev.type = type;
    ev.adc_value = adc_value;
    ev.key = (uint8_t)type;          /* gesture IG_NONE: an old-style event, passed through */
    m = lock();
    r = q_put(&ev);
    unlock(m);
    return r;
}

unsigned input_pending(void)
{
    return s_count;
}

unsigned input_dropped(void)
{
    return s_dropped;
}

/* ---- the recogniser ---- */

static uint32_t s_hold_ms = INPUT_HOLD_MS;
static int s_repeat_profile = INPUT_REPEAT_LIST;

static struct {
    int last;                /* the previous sample's key: two agreeing samples count */
    int key;                 /* the key down (KEY_NONE) */
    uint32_t t0;             /* when it went down (the press; a band it swept past included) */
    uint32_t t_band;         /* when its own band started */
    uint8_t down, held, taps, ignore;   /* DOWN sent, HOLD sent, place in a multi-press, wait for all up */
    uint8_t settled;         /* this press's place in the multi-press decided (its key is sure) */
    uint32_t reps;           /* repeat steps sent */
    int seq_key;             /* a multi-press open: its key, its taps, the last release */
    uint8_t seq_taps;
    uint32_t seq_up;
    uint32_t now;            /* the latest sample's time */
    uint32_t edge_press, edge_release;  /* the ladder's own edges (drivers: keys_tick), 0 = none */
} k = { KEY_NONE, KEY_NONE };

void input_set_hold_ms(uint32_t ms)
{
    s_hold_ms = ms >= 200u && ms <= 2000u ? ms : INPUT_HOLD_MS;
}

void input_set_repeat(int profile)
{
    s_repeat_profile = profile;
}

static int is_vol(int key)
{
    return key == KEY_UP || key == KEY_DOWN;
}

/* VOL- (the lowest band) and the power GPIO are never passed on the way to another key. */
static uint32_t sweep_ms(int key)
{
    return key == KEY_DOWN || key == KEY_POWER ? 0u : INPUT_SWEEP_MS;
}

/* Repeat steps due after `held` ms (DESIGN 4.4): lists 1 step at 400 ms, then 8/s, 16/s after
 * 1 s, 32/s after 2 s; volume 1 a 100 ms from 400 ms, 2 a 100 ms after 1 s. */
static uint32_t repeat_due(uint32_t held)
{
    if (held < INPUT_REPEAT_MS)
        return 0;
    if (s_repeat_profile == INPUT_REPEAT_VOLUME) {
        if (held < 1000u)
            return 1u + (held - INPUT_REPEAT_MS) / 100u;
        return 7u + 2u * ((held - 1000u) / 100u);
    }
    if (held < 1000u)
        return 1u + (held - INPUT_REPEAT_MS) * 8u / 1000u;
    if (held < 2000u)
        return 5u + (held - 1000u) * 16u / 1000u;
    return 21u + (held - 2000u) * 32u / 1000u;
}

static void make(input_event_t *ev, int gesture, int key, uint32_t now)
{
    uint32_t held = gesture != IG_END && k.key != KEY_NONE ? now - k.t0 : 0u;

    ev->type = key;
    ev->adc_value = 0;
    ev->key = (uint8_t)key;
    ev->gesture = (uint8_t)gesture;
    ev->taps = gesture == IG_END ? k.seq_taps : k.taps;
    ev->steps = 0;
    ev->repeat = 0;
    ev->held_ms = (uint16_t)(held > 65535u ? 65535u : held);
    ev->in_ms = (uint16_t)(k.now - (gesture == IG_DOWN ? k.t0 : now));
    ev->t_ms = now;
}

static void emit(int gesture, int key, uint32_t now)
{
    input_event_t ev;

    make(&ev, gesture, key, now);
    q_put(&ev);
}

static void emit_repeat(uint32_t now)
{
    uint32_t due = repeat_due(now - k.t0), n = due - k.reps;
    input_event_t *t = q_tail();

    if (!n)
        return;
    k.reps = due;
    if (t && t->gesture == IG_REPEAT && t->key == k.key && t->steps + n <= 255u) {
        t->steps = (uint8_t)(t->steps + n);  /* the UI has not taken the last one: one bigger step */
        t->repeat = (uint16_t)due;
        t->t_ms = now;
        t->held_ms = (uint16_t)(now - k.t0 > 65535u ? 65535u : now - k.t0);
        return;
    }
    {
        input_event_t ev;

        make(&ev, IG_REPEAT, k.key, now);
        ev.steps = (uint8_t)(n > 255u ? 255u : n);
        ev.repeat = (uint16_t)due;
        q_put(&ev);
    }
}

static void end_sequence(uint32_t now)
{
    if (k.seq_key != KEY_NONE) {
        emit(IG_END, k.seq_key, now);
        k.seq_key = KEY_NONE;
    }
}

/* This press's place in the open multi-press, if its key is that one's. */
static uint8_t taps_for(int key)
{
    return k.seq_key == key && k.t0 - k.seq_up <= INPUT_MULTI_GAP_MS && k.seq_taps < 3u ?
           (uint8_t)(k.seq_taps + 1u) : 1u;
}

/* The key down is sure (its DOWN, its release, a chord): a press of another key closes the open
 * multi-press now, not when a band the voltage only passed through became stable for a moment
 * (QEMU key-ramp-ms=25: MENU's band held 20 ms on the way to PLAY's and broke PLAY's x2). */
static void settle(uint32_t now)
{
    if (k.settled)
        return;
    k.settled = 1;
    if (k.taps == 1u)
        end_sequence(now);
    k.seq_key = KEY_NONE;                    /* (open again at this press's release, if a tap) */
}

/* The ladder's edge, when it is this press's: the time the voltage left idle (press) or rose off
 * the held key's level (release), not the time a band was stable (20..100 ms later, depending on
 * how fast the voltage settles and where the 10 ms samples fall). */
static uint32_t edge(uint32_t e, uint32_t now, uint32_t not_before)
{
    return e && now - e <= 250u && (int32_t)(e - not_before) >= 0 ? e : now;
}

static void press(int key, uint32_t now)
{
    k.key = key;
    k.t0 = key == KEY_POWER ? now : edge(k.edge_press, now, now - 250u);
    k.t_band = now;
    k.taps = taps_for(key);
    k.down = k.held = k.settled = 0;
    k.reps = 0;
}

static void release(uint32_t now)
{
    uint32_t up = k.key == KEY_POWER ? now : edge(k.edge_release, now, k.t0);

    settle(now);
    if (!k.down) {                           /* a tap shorter than the sweep time: its DOWN now */
        emit(IG_DOWN, k.key, now);
        k.down = 1;
    }
    if (k.held || k.reps) {
        emit(IG_RELEASE, k.key, up);
    } else {
        emit(IG_TAP, k.key, up);
        k.seq_key = k.key;
        k.seq_taps = k.taps;
        k.seq_up = up;
    }
    k.key = KEY_NONE;
}

/* The key stays down: DOWN once past the sweep time, HOLD at the hold time, VOL's repeats. */
static void held(uint32_t now)
{
    uint32_t t = now;

    if (!k.down && now - k.t_band >= sweep_ms(k.key)) {
        settle(now);
        emit(IG_DOWN, k.key, now);
        k.down = 1;
    }
    /* the finger is already off (the release edge; the band stays until the voltage is back at
     * idle, up to ~200 ms later): the hold and the repeats stop where it let go */
    if (k.key != KEY_POWER && k.edge_release && (int32_t)(k.edge_release - k.t0) > 0 &&
        now - k.edge_release <= 250u)
        t = k.edge_release;
    if (k.down && !k.held && t - k.t0 >= s_hold_ms) {
        emit(IG_HOLD, k.key, t);
        k.held = 1;
    }
    if (k.down && is_vol(k.key))
        emit_repeat(t);
}

static void sample(int key, uint32_t now)
{
    k.now = now;
    if (key != k.last) {                     /* not stable yet */
        k.last = key;
        return;
    }
    if (k.ignore) {                          /* a chord, or a key held across a clear */
        if (key == KEY_NONE)
            k.ignore = 0;
        return;
    }
    if (key == k.key) {
        if (key == KEY_NONE) {
            if (k.seq_key != KEY_NONE && now - k.seq_up > INPUT_MULTI_GAP_MS)
                end_sequence(now);
            return;
        }
        held(now);
        return;
    }
    if (k.key != KEY_NONE && key != KEY_NONE && k.key != KEY_POWER && key != KEY_POWER) {
        /* one ladder key to another with no release between */
        if (is_vol(k.key) && is_vol(key) && k.down && now - k.t_band >= INPUT_CHORD_MS) {
            /* RECHO's chord: one volume key held (a deliberate hold, not the sweep of a soft
             * press), then the ladder reads the other */
            settle(now);
            emit(IG_CHORD, KEY_CHORD, now);
            k.key = KEY_NONE;
            k.seq_key = KEY_NONE;
            k.ignore = 1;
            return;
        }
        /* the voltage is still settling and the lowest reading wins (drivers: keys_adc): the press
         * was this key all along, no TAP or RELEASE for the band it passed (a DOWN it may have had
         * stays: a key acting on its press is the reason for INPUT_SWEEP_MS); the press counts from
         * its start, as stock's KeyScan 0x0306240a (work/stock_boot/05_keys_adc.md 1.3) */
        k.key = key;
        k.t_band = now;
        k.down = k.held = 0;
        k.reps = 0;
        if (!k.settled)
            k.taps = taps_for(key);
        held(now);
        return;
    }
    if (k.key != KEY_NONE)
        release(now);
    if (key != KEY_NONE) {
        press(key, now);
        held(now);                           /* (VOL- and POWER: DOWN now) */
    }
}

static volatile int s_isr;
static input_event_t s_rep;          /* input_poll: a REPEAT whose steps are still being handed out */
static unsigned s_rep_left;

/* The press that switched the player on is not a key: the power key counts only once it has been
 * seen up since boot (a boot by cable arms it at the first sample). Without this the power-on
 * press arrived as a POWER hold on the first screen (m32, 2026-10-18), where a hold opens the
 * power menu (DESIGN 1.2). */
static int s_power_seen_up;

void input_sample(int adc_value, int power, uint32_t now_ms)
{
    input_sample_edges(adc_value, power, now_ms, 0, 0);
}

void input_sample_edges(int adc_value, int power, uint32_t now_ms, uint32_t press_ms, uint32_t release_ms)
{
    k.edge_press = press_ms;
    k.edge_release = release_ms;
    if (!power)
        s_power_seen_up = 1;
    else if (!s_power_seen_up)
        power = 0;
    sample(power ? KEY_POWER : input_map_adc(adc_value), now_ms);
}

void input_isr_sampling(int on)
{
    s_isr = on;
}

int input_isr_active(void)
{
    return s_isr;
}

static uint32_t (*s_clock)(void);
static uint32_t s_fake_now;

void input_set_clock(uint32_t (*now_ms)(void))
{
    s_clock = now_ms;
}

int input_scan(int adc_value, int power)
{
    unsigned before = s_count;

    watchdog_feed();             /* a loop that scans the keys is alive */
    if (s_isr)
        return 0;
    input_sample(adc_value, power, s_clock ? s_clock() : (s_fake_now += INPUT_TICK_MS));
    return s_count > before;
}

int input_hold_progress(int key)
{
    uint32_t held = input_held_ms(key);

    if (!held)
        return 0;
    return held >= s_hold_ms ? 10 : (int)(held * 10u / s_hold_ms);
}

uint32_t input_held_ms(int key)
{
    uint32_t m = lock(), r = 0;

    if (k.key == key && key != KEY_NONE && k.down)
        r = k.now - k.t0;
    unlock(m);
    return r;
}

void input_mock_clear(void)
{
    uint32_t m = lock();

    s_head = 0;
    s_count = 0;
    s_rep_left = 0;
    if (k.key != KEY_NONE || k.last != KEY_NONE)
        k.ignore = 1;                        /* still down: not a new press for whoever asked */
    k.key = KEY_NONE;
    k.seq_key = KEY_NONE;
    unlock(m);
}

/* ---- names ---- */

const char *input_key_name(int key)
{
    static const char *const n[] = { "NONE", "VOLP", "VOLM", "PLAY", "BACK", "POWER", "MENU", "CHORD" };

    return key >= 0 && key <= KEY_CHORD ? n[key] : "?";
}

const char *input_gesture_name(const input_event_t *ev)
{
    switch (ev->gesture) {
    case IG_DOWN:    return "down";
    case IG_TAP:     return ev->taps == 3 ? "x3" : ev->taps == 2 ? "x2" : "tap";
    case IG_HOLD:    return ev->taps == 3 ? "x3hold" : ev->taps == 2 ? "x2hold" : "hold";
    case IG_REPEAT:  return "repeat";
    case IG_RELEASE: return "release";
    case IG_END:     return "end";
    case IG_CHORD:   return "chord";
    default:         return "event";
    }
}

/* ---- the two ways out ---- */

int input_next(input_event_t *ev)
{
    if (ev == NULL)
        return INPUT_ERR_ARG;
    if (!q_get(ev))
        return 0;
    ev->type = ev->key;
    if (ev->gesture != IG_NONE)
        ev->adc_value = 0;
    return 1;
}

/* The old events, from the gestures: UP/DOWN act on DOWN and repeat (a REPEAT of n steps gives n
 * events), the other keys on TAP (short) or HOLD (INPUT_ADC_LONG), the chord as KEY_CHORD. */
static void (*s_observer)(const input_event_t *);

void input_set_observer(void (*fn)(const input_event_t *ev))
{
    s_observer = fn;
}

int input_poll(input_event_t *ev)
{
    if (ev == NULL)
        return INPUT_ERR_ARG;
    for (;;) {
        input_event_t g;

        if (s_rep_left) {
            *ev = s_rep;
            ev->type = s_rep.key;
            ev->adc_value = INPUT_ADC_REPEAT;
            s_rep_left--;
            return 1;
        }
        if (!q_get(&g))
            return 0;
        if (s_observer && g.gesture != IG_NONE)
            s_observer(&g);
        *ev = g;
        switch (g.gesture) {
        case IG_NONE:                        /* input_push() */
            return 1;
        case IG_DOWN:
            if (!is_vol(g.key))
                continue;
            ev->adc_value = 0;
            return 1;
        case IG_REPEAT:
            s_rep = g;
            s_rep_left = g.steps ? g.steps : 1u;
            continue;
        case IG_TAP:
            if (is_vol(g.key))
                continue;
            ev->adc_value = g.key == KEY_POWER ? -1 : 0;
            return 1;
        case IG_HOLD:
            if (is_vol(g.key))
                continue;
            ev->adc_value = INPUT_ADC_LONG;
            return 1;
        case IG_CHORD:
            ev->type = KEY_CHORD;
            ev->adc_value = 0;
            return 1;
        default:                             /* RELEASE, END: no old event */
            continue;
        }
    }
}
