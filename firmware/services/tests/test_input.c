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

static void test_scan(void)
{
    input_event_t ev;
    int i;

    input_mock_clear();
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(130, 0) == 0);          /* 1st sample: not stable */
    CHECK(input_scan(130, 0) == 0);          /* stable: VOL+'s band starts to count */
    for (i = 1; i < INPUT_PRESS_SCANS; i++)
        CHECK(input_scan(127, 0) == 0);
    CHECK(input_scan(127, 0) == 1);          /* held INPUT_PRESS_SCANS: the press acts */
    CHECK(input_scan(127, 0) == 0);          /* hold: no repeat */
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);         /* release: nothing more */
    CHECK(input_scan(500, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);         /* 1-sample glitch ignored */
    CHECK(input_scan(1023, 1) == 0);
    CHECK(input_scan(1023, 1) == 0);         /* power GPIO wins; acts on release */
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 1);         /* short press: released */
    CHECK(input_pending() == 2);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP && ev.adc_value == 127);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_POWER && ev.adc_value == -1);
    input_mock_clear();
}

/* The ladder as a soft press on the device drives it (2026-10-16): the voltage settles through
 * the bands of other keys. Only the key it settles on acts. */
static void test_sweep(void)
{
    input_event_t ev;
    int i;

    input_mock_clear();
    /* DOWN pressed softly: VOL+'s band for a while (not INPUT_PRESS_SCANS), then VOL- */
    CHECK(input_scan(130, 0) == 0);
    for (i = 0; i < INPUT_PRESS_SCANS - 2; i++)
        CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(30, 0) == 0);
    for (i = 0; i < INPUT_PRESS_SCANS; i++)
        CHECK(input_scan(30, 0) == 0);
    CHECK(input_scan(30, 0) == 1);           /* DOWN acts once its own band held */
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_pending() == 1);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_DOWN);
    /* MENU pressed softly: BACK's band first, then MENU's, then released: only MENU */
    CHECK(input_scan(515, 0) == 0);
    CHECK(input_scan(515, 0) == 0);
    CHECK(input_scan(515, 0) == 0);
    CHECK(input_scan(360, 0) == 0);
    CHECK(input_scan(360, 0) == 0);
    CHECK(input_scan(360, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 1);         /* released: the key it settled on */
    CHECK(input_pending() == 1);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_MENU);
    /* a quick tap of UP, released before INPUT_PRESS_SCANS: it acts on the release */
    CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 1);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP);
    input_mock_clear();
}

/* Keys with a hold meaning: short on release, or INPUT_ADC_LONG once held
 * INPUT_LONG_SCANS scans (and nothing on that release). */
static void test_hold(void)
{
    input_event_t ev;
    int i, n = 0;

    input_mock_clear();
    CHECK(input_scan(1023, 1) == 0);
    for (i = 0; i < INPUT_LONG_SCANS + 5; i++)
        n += input_scan(1023, 1);
    CHECK(n == 1);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_POWER && ev.adc_value == INPUT_ADC_LONG);
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);         /* released after a hold: no short press */
    CHECK(input_pending() == 0);
    input_mock_clear();
}

/* VOL+ held, then the ladder reads VOL-: one KEY_CHORD, nothing more until all keys are up. */
static void test_chord(void)
{
    input_event_t ev;
    int i;

    input_mock_clear();
    CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(130, 0) == 0);
    for (i = 1; i < INPUT_PRESS_SCANS; i++)
        CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(130, 0) == 1);          /* VOL+ press */
    for (i = INPUT_PRESS_SCANS; i < INPUT_CHORD_SCANS; i++)
        CHECK(input_scan(130, 0) == 0);      /* held long enough to be a deliberate chord */
    CHECK(input_scan(30, 0) == 0);
    CHECK(input_scan(30, 0) == 1);           /* the other volume key: the chord */
    CHECK(input_scan(130, 0) == 0);
    CHECK(input_scan(130, 0) == 0);          /* VOL- up, VOL+ still down: nothing */
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_scan(1023, 0) == 0);
    CHECK(input_pending() == 2);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_UP);
    CHECK(input_poll(&ev) == 1 && ev.type == KEY_CHORD);
    input_mock_clear();
}

int main(void)
{
    test_chord();
    test_sweep();
    test_queue_fifo();
    test_hold();
    test_overflow();
    test_adc_map();
    test_scan();

    printf("test_input: %d checks, %d failures\n", s_checks, s_failures);
    return (s_failures == 0) ? 0 : 1;
}
