/*
 * test_input.c — host tests for the key event queue and ADC mapping.
 *
 * Covers: FIFO queue semantics, empty/NULL poll, invalid types, queue
 * overflow drop, mock clear, ADC band mapping (including the documented
 * 1023 idle reading) and adc_value pass-through.
 */
#include "input.h"

#include <stdio.h>
#include <string.h>

void watchdog_feed(void) { }        /* (services/watchdog.c is target-only) */

static int s_checks;
static int s_failures;

#define CHECK(cond) do {                                                   \
    s_checks++;                                                            \
    if (!(cond)) {                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        s_failures++;                                                      \
    }                                                                      \
} while (0)

static void test_queue_fifo(void)
{
    input_event_t ev;

    input_mock_clear();
    CHECK(input_pending() == 0);
    CHECK(input_poll(&ev) == 0);             /* empty */
    CHECK(input_poll(NULL) == INPUT_ERR_ARG);

    /* Invalid event types are rejected. */
    CHECK(input_push(KEY_NONE, 0) == INPUT_ERR_ARG);
    CHECK(input_push(KEY_CHORD + 1, 0) == INPUT_ERR_ARG);
    CHECK(input_push(-1, 0) == INPUT_ERR_ARG);

    CHECK(input_push(KEY_UP, 100) == INPUT_OK);
    CHECK(input_push(KEY_SELECT, 406) == INPUT_OK);   /* scripted ADC value */
    CHECK(input_push(KEY_POWER, -1) == INPUT_OK);     /* GPIO power key */
    CHECK(input_pending() == 3);

    CHECK(input_poll(&ev) == 1);
    CHECK(ev.type == KEY_UP && ev.adc_value == 100);
    CHECK(input_poll(&ev) == 1);
    CHECK(ev.type == KEY_SELECT && ev.adc_value == 406);
    CHECK(input_poll(&ev) == 1);
    CHECK(ev.type == KEY_POWER && ev.adc_value == -1);
    CHECK(input_poll(&ev) == 0);
    CHECK(input_pending() == 0);
}

static void test_overflow(void)
{
    input_event_t ev;
    int i;

    input_mock_clear();
    for (i = 0; i < INPUT_QUEUE_LEN; i++) {
        CHECK(input_push(KEY_DOWN, i) == INPUT_OK);
    }
    CHECK(input_pending() == (unsigned)INPUT_QUEUE_LEN);
    CHECK(input_push(KEY_UP, 0) == INPUT_ERR_FULL);   /* dropped */
    CHECK(input_pending() == (unsigned)INPUT_QUEUE_LEN);

    /* Oldest events survive; order preserved. */
    CHECK(input_poll(&ev) == 1);
    CHECK(ev.type == KEY_DOWN && ev.adc_value == 0);
    for (i = 1; i < INPUT_QUEUE_LEN; i++) {
        CHECK(input_poll(&ev) == 1);
        CHECK(ev.type == KEY_DOWN && ev.adc_value == i);
    }
    CHECK(input_poll(&ev) == 0);

    input_mock_clear();
    CHECK(input_pending() == 0);
}

static void test_adc_map(void)
{
    /* Extremes of the clusters measured on hardware (input.c). */
    CHECK(input_map_adc(0) == KEY_DOWN);      /* vol- */
    CHECK(input_map_adc(121) == KEY_UP);      /* vol+ */
    CHECK(input_map_adc(177) == KEY_UP);
    CHECK(input_map_adc(245) == KEY_SELECT);  /* play */
    CHECK(input_map_adc(299) == KEY_SELECT);
    CHECK(input_map_adc(321) == KEY_MENU);
    CHECK(input_map_adc(395) == KEY_MENU);
    CHECK(input_map_adc(483) == KEY_BACK);
    CHECK(input_map_adc(549) == KEY_BACK);

    /* Gaps, idle (>= 1001 measured) and out-of-range map to no key. */
    CHECK(input_map_adc(190) == KEY_NONE);
    CHECK(input_map_adc(430) == KEY_NONE);
    CHECK(input_map_adc(1001) == KEY_NONE);
    CHECK(input_map_adc(1023) == KEY_NONE);
    CHECK(input_map_adc(2000) == KEY_NONE);
    CHECK(input_map_adc(-1) == KEY_NONE);
}

/* Samples every 10 ms (input_scan's own clock on the host): n samples of one reading. Returns the
 * gestures queued meanwhile. */
static unsigned feed(int adc, int power, int n)
{
    unsigned before = input_pending();

    while (n-- > 0)
        input_scan(adc, power);
    return input_pending() - before;
}

static int next_is(int key, int gesture, int taps)
{
    input_event_t ev;

    return input_next(&ev) == 1 && ev.key == key && ev.gesture == gesture && (!taps || ev.taps == taps);
}

static void reset(void)
{
    input_mock_clear();
    feed(1023, 0, 3);
    input_mock_clear();
}

/* The old events: UP/DOWN on the press (once the band held INPUT_SWEEP_MS), the power key on
 * release, a 1-sample glitch ignored. */
static void test_scan(void)
{
    input_event_t ev;

    reset();
    CHECK(feed(130, 0, 2) == 0);             /* 2 agreeing samples: VOL+'s band starts */
    CHECK(feed(127, 0, INPUT_SWEEP_MS / 10 - 1) == 0);
    CHECK(feed(127, 0, 1) == 1);             /* held INPUT_SWEEP_MS: DOWN */
    CHECK(feed(127, 0, 5) == 0);             /* held: no repeat yet */
    CHECK(feed(1023, 0, 2) == 1);            /* release: a TAP (no old event for VOL+) */
    CHECK(feed(500, 0, 1) == 0);
    CHECK(feed(1023, 0, 1) == 0);            /* 1-sample glitch ignored */
    CHECK(feed(1023, 1, 2) == 2);            /* power: VOL+'s multi-press closed, DOWN at once... */
    CHECK(input_pending() == 4);
    CHECK(feed(1023, 0, 2) >= 1);            /* ...and a TAP on release */
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP && ev.adc_value == 0 && ev.gesture == IG_DOWN);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_POWER && ev.adc_value == -1 && ev.gesture == IG_TAP);
    CHECK(input_poll(&ev) == 0);
    reset();
}

/* The ladder as a soft press on the device drives it (2026-10-16): the voltage settles through
 * the bands of other keys. Only the key it settles on acts. */
static void test_sweep(void)
{
    input_event_t ev;

    reset();
    /* VOL- pressed softly: VOL+'s band for 40 ms (under INPUT_SWEEP_MS), then VOL- */
    feed(130, 0, 5);
    feed(30, 0, 2);                          /* VOL-: DOWN at once (nothing sweeps past it) */
    feed(1023, 0, 2);
    CHECK(next_is(KEY_DOWN, IG_DOWN, 1));
    CHECK(next_is(KEY_DOWN, IG_TAP, 1));
    CHECK(input_next(&ev) == 0);
    feed(1023, 0, 40);                       /* (the multi-press window closes) */
    CHECK(next_is(KEY_DOWN, IG_END, 1));
    /* MENU pressed softly: BACK's band first, then MENU's, then released: only MENU */
    feed(515, 0, 3);
    feed(360, 0, 3);
    feed(1023, 0, 2);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_MENU && ev.adc_value == 0);
    CHECK(input_poll(&ev) == 0);
    /* VOL+ pressed slowly (QEMU's key-ramp-ms=50): PLAY's band held 70 ms on the way; only VOL+
     * counts (a DOWN for PLAY may come first, but never a TAP) */
    feed(1023, 0, 40);
    while (input_next(&ev) == 1)
        ;
    feed(260, 0, 2 + 7);
    feed(150, 0, 2 + 20);
    feed(1023, 0, 2);
    CHECK(input_next(&ev) == 1 && ev.key == KEY_SELECT && ev.gesture == IG_DOWN);
    CHECK(next_is(KEY_UP, IG_DOWN, 1));
    CHECK(next_is(KEY_UP, IG_TAP, 1));
    CHECK(input_next(&ev) == 0);
    /* a quick tap of UP, released before INPUT_SWEEP_MS: it acts on the release */
    feed(1023, 0, 40);
    feed(130, 0, 3);
    feed(1023, 0, 2);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP && ev.gesture == IG_DOWN);
    reset();
}

/* Keys with a hold meaning: short on release, or INPUT_ADC_LONG at the hold time (500 ms after the
 * press) and nothing on that release but IG_RELEASE. */
static void test_hold(void)
{
    input_event_t ev;
    uint32_t t_down = 0;
    int got = 0;

    reset();
    feed(1023, 1, 2);
    CHECK(input_next(&ev) == 1 && ev.key == KEY_POWER && ev.gesture == IG_DOWN);
    t_down = ev.t_ms;
    CHECK(input_hold_progress(KEY_POWER) == 0);
    feed(1023, 1, 25);                       /* 250 ms */
    CHECK(input_hold_progress(KEY_POWER) == 5);
    while (!got && feed(1023, 1, 1) == 0)
        ;
    got = input_next(&ev);
    CHECK(got && ev.gesture == IG_HOLD && ev.taps == 1);
    CHECK(ev.t_ms - t_down == INPUT_HOLD_MS);  /* exactly, on the 10 ms grid */
    CHECK(ev.held_ms == INPUT_HOLD_MS);
    CHECK(input_hold_progress(KEY_POWER) == 10);
    feed(1023, 1, 10);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_POWER, IG_RELEASE, 1));
    CHECK(input_pending() == 0);

    /* the same through the old API */
    reset();
    feed(1023, 1, 2 + 60);
    feed(1023, 0, 3);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_POWER && ev.adc_value == INPUT_ADC_LONG);
    CHECK(input_poll(&ev) == 0);             /* released after a hold: no short press */

    /* the hold time is a setting */
    reset();
    input_set_hold_ms(700);
    feed(300, 0, 2 + 69);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(input_pending() == 0);
    feed(300, 0, 1);
    CHECK(next_is(KEY_SELECT, IG_HOLD, 1));
    input_set_hold_ms(INPUT_HOLD_MS);
    reset();
}

/* The press that switched the player on: nothing until the power key is seen up; then it counts.
 * (Must run first: the latch is per boot.) */
static void test_boot_press(void)
{
    input_event_t ev;

    input_mock_clear();                      /* (not reset(): its idle samples would arm the key) */
    feed(1023, 1, 150);                      /* held 1.5 s from the first sample: no DOWN, no HOLD */
    CHECK(input_next(&ev) == 0);
    CHECK(input_hold_progress(KEY_POWER) == 0);
    feed(1023, 0, 3);
    CHECK(input_next(&ev) == 0);             /* nor a release */
    feed(1023, 1, 2);
    CHECK(input_next(&ev) == 1 && ev.key == KEY_POWER && ev.gesture == IG_DOWN);
    feed(1023, 0, 3);
    reset();
}

/* PLAY: x2, x3 and their holds; every tap reported at its release (the first never waits). */
static void test_multi(void)
{
    input_event_t ev;

    reset();
    feed(260, 0, 2 + 8);                     /* tap 1: 80 ms */
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(next_is(KEY_SELECT, IG_TAP, 1));   /* at once */
    feed(1023, 0, 18);                       /* 200 ms gap */
    feed(260, 0, 2 + 8);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 2));
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_TAP && ev.taps == 2);
    CHECK(strcmp(input_gesture_name(&ev), "x2") == 0);
    feed(1023, 0, 18);
    feed(260, 0, 2 + 8);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 3));
    CHECK(next_is(KEY_SELECT, IG_TAP, 3));
    feed(1023, 0, 31);                       /* 300 ms after the last release: the window closes */
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_END && ev.taps == 3 && ev.key == KEY_SELECT);

    /* a gap over 300 ms starts again at 1 */
    feed(260, 0, 2 + 8);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(next_is(KEY_SELECT, IG_TAP, 1));
    feed(1023, 0, 35);
    CHECK(next_is(KEY_SELECT, IG_END, 1));
    feed(260, 0, 2 + 8);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(next_is(KEY_SELECT, IG_TAP, 1));

    /* 2x+hold: a tap, then a press held; through the old API a short press and a hold */
    feed(1023, 0, 10);
    feed(260, 0, 2 + 55);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 2));
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_HOLD && ev.taps == 2);
    CHECK(strcmp(input_gesture_name(&ev), "x2hold") == 0);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_RELEASE, 2));
    feed(1023, 0, 40);
    CHECK(input_pending() == 0);             /* (a hold closes the multi-press: no END) */

    /* another key closes PLAY's window at once */
    feed(260, 0, 2 + 8);
    feed(1023, 0, 2);
    feed(515, 0, 2 + 8);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(next_is(KEY_SELECT, IG_TAP, 1));
    CHECK(next_is(KEY_SELECT, IG_END, 1));
    CHECK(next_is(KEY_BACK, IG_DOWN, 1));
    CHECK(next_is(KEY_BACK, IG_TAP, 1));
    reset();
}

/* VOL+ held, then the ladder reads VOL-: one chord, nothing more until all keys are up. */
static void test_chord(void)
{
    input_event_t ev;

    reset();
    feed(130, 0, 2 + INPUT_SWEEP_MS / 10);   /* VOL+ press */
    feed(130, 0, (INPUT_CHORD_MS - INPUT_SWEEP_MS) / 10);   /* held long enough to be a chord */
    feed(30, 0, 2);                          /* the other volume key: the chord */
    feed(130, 0, 3);
    feed(30, 0, 3);                          /* nothing while any key stays down */
    feed(1023, 0, 3);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_CHORD && ev.gesture == IG_CHORD);
    CHECK(input_poll(&ev) == 0);
    /* VOL+ for less than INPUT_CHORD_MS, then VOL-: VOL+ was pressed, then VOL- (no chord) */
    feed(1023, 0, 40);
    feed(130, 0, 2 + 10);
    feed(30, 0, 2);
    feed(1023, 0, 2);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_DOWN);
    CHECK(input_poll(&ev) == 0);
    reset();
}

/* VOL held: repeats from 400 ms, accelerating; not taken for a while they become one bigger step;
 * the steps add up to the schedule whatever the UI does. */
static void test_repeat(void)
{
    input_event_t ev;
    unsigned steps = 0, events = 0;

    reset();
    input_set_repeat(INPUT_REPEAT_LIST);
    feed(30, 0, 2);                          /* VOL-: DOWN at once */
    CHECK(next_is(KEY_DOWN, IG_DOWN, 1));
    feed(30, 0, 39);                         /* 390 ms */
    CHECK(input_pending() == 0);
    feed(30, 0, 1);                          /* 400 ms */
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_REPEAT && ev.steps == 1 && ev.repeat == 1);
    for (int i = 0; i < 160; i++) {          /* to 2 s, taken every tick */
        feed(30, 0, 1);
        while (input_next(&ev) == 1) {
            CHECK(ev.gesture == IG_REPEAT || (ev.gesture == IG_HOLD && ev.held_ms == INPUT_HOLD_MS));
            if (ev.gesture == IG_REPEAT) {
                steps += ev.steps;
                events++;
            }
        }
    }
    CHECK(1 + steps == 21);                  /* 1 at 0.4 s, 8/s to 1 s, 16/s to 2 s */
    CHECK(events == steps);                  /* one step per event while taken at once */
    feed(30, 0, 100);                        /* 1 s more, not taken (a stalled main loop) */
    CHECK(input_pending() == 1);
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_REPEAT && ev.steps == 32 && ev.repeat == 53);
    feed(1023, 0, 2);
    CHECK(next_is(KEY_DOWN, IG_RELEASE, 1));
    /* the old API hands a merged step out as that many events */
    feed(130, 0, 2 + 40 + 60);
    input_mock_clear();                      /* (still down: ignored until released) */
    feed(130, 0, 20);
    CHECK(input_pending() == 0);
    feed(1023, 0, 2);
    CHECK(input_pending() == 0);
    input_set_repeat(INPUT_REPEAT_VOLUME);
    feed(130, 0, 2 + 120);                   /* 1.2 s: 7 + 2x2 = 11 dB, from 400 ms */
    {
        unsigned n = 0, rep = 0;

        while (input_poll(&ev) == 1) {
            n++;
            rep += ev.adc_value == INPUT_ADC_REPEAT;
        }
        CHECK(n == 1 + 11 && rep == 11);
    }
    input_set_repeat(INPUT_REPEAT_LIST);
    reset();
}

/* The ladder as QEMU's key-ramp-ms=25 drives it, with the driver's edges (input_sample_edges): a
 * second PLAY tap whose voltage holds MENU's band 20 ms on the way down is still x2; times come
 * from the edges, not from when a band was stable. */
static uint32_t s_t;
static void edges(int adc, int n, uint32_t press, uint32_t release)
{
    while (n-- > 0) {
        s_t += 10;
        input_sample_edges(adc, 0, s_t, press, release);
    }
}

static void test_edges(void)
{
    input_event_t ev;
    uint32_t p1, r1, p2, r2;

    reset();
    s_t = 100000;
    edges(1023, 3, 0, 0);
    while (input_next(&ev) == 1)
        ;
    p1 = s_t + 5;                            /* tap 1: PLAY straight */
    edges(260, 2 + 8, p1, 0);
    r1 = s_t + 5;
    edges(260, 4, p1, r1);                   /* (the band kept while the voltage rises) */
    edges(1023, 2, p1, r1);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_TAP && ev.t_ms == r1 && ev.held_ms == r1 - p1);
    edges(1023, 15, 0, r1);
    p2 = s_t + 5;                            /* tap 2: MENU's band stable 30 ms first */
    edges(360, 2 + 2, p2, 0);
    edges(260, 2 + 6, p2, 0);
    r2 = s_t + 5;
    edges(260, 4, p2, r2);
    edges(1023, 2, p2, r2);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 2));
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_TAP && ev.taps == 2 && ev.t_ms == r2);
    CHECK(input_next(&ev) == 0);             /* (no END for the band it passed) */
    edges(1023, 40, 0, r2);
    CHECK(next_is(KEY_SELECT, IG_END, 2));
    /* a hold: 500 ms after the edge, whenever the band became stable */
    p1 = s_t + 5;
    edges(515, 3, p1, 0);                    /* BACK's band on the way (as long as you like) */
    edges(260, 70, p1, 0);
    CHECK(next_is(KEY_SELECT, IG_DOWN, 1));
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_HOLD && ev.t_ms - p1 >= 500 && ev.t_ms - p1 < 510);
    r1 = s_t + 5;
    edges(260, 5, p1, r1);
    edges(1023, 2, p1, r1);
    CHECK(input_next(&ev) == 1 && ev.gesture == IG_RELEASE && ev.t_ms == r1 && ev.held_ms == r1 - p1);
    reset();
}

int main(void)
{
    test_boot_press();
    test_chord();
    test_sweep();
    test_queue_fifo();
    test_hold();
    test_overflow();
    test_adc_map();
    test_scan();
    test_multi();
    test_repeat();
    test_edges();

    printf("test_input: %d checks, %d failures\n", s_checks, s_failures);
    return (s_failures == 0) ? 0 : 1;
}
