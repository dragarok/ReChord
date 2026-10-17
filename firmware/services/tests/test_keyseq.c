/*
 * test_keyseq.c -- host tests for the key-sequence matcher (keyseq.h) and the
 * Bluetooth earphone key mapping on it (bt_keys.h).
 *
 * Covers: stock's one-event mapping CP/CC/CA/CB -> pause/play/prev/next and
 * CD/CU/CM -> nothing; every event logged with its time; the generic matcher
 * with longer rules (a double press told from a single one by the gap; the
 * single fires from keyseq_poll once the double can no longer complete; a
 * triple beats a double; a too-slow sequence is not one).
 */
#include "bt_keys.h"
#include "keyseq.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

static int s_checks;
static int s_failures;

#define CHECK(cond) do {                                                   \
    s_checks++;                                                            \
    if (!(cond)) {                                                         \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        s_failures++;                                                      \
    }                                                                      \
} while (0)

/* log.c stand-in: keep the last line */
static char s_last[160];
void log_printf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s_last, sizeof s_last, fmt, ap);
    va_end(ap);
}

static int s_action[16], s_nact;
void bt_keys_action(int action, uint32_t t_ms)
{
    (void)t_ms;
    if (s_nact < 16)
        s_action[s_nact++] = action;
}

static void test_bt_mapping(void)
{
    bt_keys_init();
    s_nact = 0;
    bt_keys_event('P', 1000);
    CHECK(!strcmp(s_last, "bt: key action pause"));
    bt_keys_event('C', 2000);
    bt_keys_event('A', 3000);
    bt_keys_event('B', 21034);
    CHECK(s_nact == 4);
    CHECK(s_action[0] == BT_KEY_PAUSE && s_action[1] == BT_KEY_PLAY);
    CHECK(s_action[2] == BT_KEY_PREV && s_action[3] == BT_KEY_NEXT);
    bt_keys_event('D', 22000);              /* volume -/+ [inferred]: logged, no action (as stock) */
    CHECK(!strcmp(s_last, "bt: key CD at 22000 ms"));
    bt_keys_event('U', 22100);
    bt_keys_event('M', 22200);
    CHECK(s_nact == 4);
    bt_keys_poll(60000);
    CHECK(s_nact == 4);
    CHECK(bt_keys_events() == 7);
}

enum { A_SINGLE = 1, A_DOUBLE, A_TRIPLE, A_OTHER };

static const keyseq_rule_t k_rules[] = {
    { 1, { 'P' }, 0, A_SINGLE },
    { 2, { 'P', 'P' }, 400, A_DOUBLE },
    { 3, { 'P', 'P', 'P' }, 400, A_TRIPLE },
    { 1, { 'B' }, 0, A_OTHER },
};

static void test_sequences(void)
{
    keyseq_t k;

    keyseq_init(&k, k_rules, 4);
    /* a single press: held back while a double is possible, fired after the gap */
    CHECK(keyseq_feed(&k, 'P', 1000) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 1300) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 1401) == A_SINGLE);
    CHECK(keyseq_poll(&k, 1500) == KEYSEQ_NONE);

    /* a double press: the triple is still possible, then the gap runs out */
    CHECK(keyseq_feed(&k, 'P', 2000) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'P', 2200) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 2601) == A_DOUBLE);

    /* a triple: nothing longer, fires at once */
    CHECK(keyseq_feed(&k, 'P', 3000) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'P', 3100) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'P', 3200) == A_TRIPLE);
    CHECK(keyseq_poll(&k, 9000) == KEYSEQ_NONE);

    /* too slow for a double: two singles */
    CHECK(keyseq_feed(&k, 'P', 10000) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 10401) == A_SINGLE);
    CHECK(keyseq_feed(&k, 'P', 10900) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 11301) == A_SINGLE);

    /* another key in between ends the sequence: the held-back single fires then, and
     * B (no longer rule) at the next poll */
    CHECK(keyseq_feed(&k, 'P', 20000) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'B', 20100) == A_SINGLE);
    CHECK(keyseq_poll(&k, 20100) == A_OTHER);
    CHECK(keyseq_feed(&k, 'B', 20150) == A_OTHER);      /* nothing held: at once */
    CHECK(keyseq_feed(&k, 'P', 20200) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 20601) == A_SINGLE);

    /* unknown codes match nothing; the history is bounded */
    for (int i = 0; i < 50; i++)
        CHECK(keyseq_feed(&k, 'Z', 30000u + (uint32_t)i) == KEYSEQ_NONE);
    CHECK(k.n <= KEYSEQ_MAX);
}

int main(void)
{
    test_bt_mapping();
    test_sequences();
    printf("test_keyseq: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
