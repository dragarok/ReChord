/*
 * test_keyseq.c -- host tests for the key-sequence matcher (keyseq.h) and the
 * Bluetooth earphone key mapping on it (bt_keys.h).
 *
 * Covers: stock's one-event mapping CP/CC/CA/CB -> pause/play/prev/next and
 * CD/CU/CM -> nothing; every event logged with its time; the generic matcher
 * with longer rules (a double press told from a single one by the gap; the
 * single fires from keyseq_poll once the double can no longer complete; a
 * triple beats a double; a too-slow sequence is not one); KEYSEQ_ALSO rules
 * (the settings sequences: every key keeps its own action at once, the
 * sequence's comes from the next poll, once per sequence, not across a gap
 * too long or another key).
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

/* a press: the key's own action at once, then a tick's poll (what the main loop does) */
static void press(uint8_t code, uint32_t t)
{
    bt_keys_event(code, t);
    bt_keys_poll(t + 10u);
}

static void test_bt_sequences(void)
{
    static const struct { uint8_t seq[4]; int want; } k[] = {
        { { 'B', 'A', 'B', 'A' }, BT_KEY_PLAY_MODE },
        { { 'B', 'B', 'A', 'A' }, BT_KEY_VOL_UP },
        { { 'B', 'A', 'A', 'B' }, BT_KEY_VOL_DOWN },
    };
    uint32_t t = 100000;

    for (unsigned i = 0; i < 3; i++) {
        bt_keys_init();
        s_nact = 0;
        for (int j = 0; j < 4; j++, t += BT_SEQ_GAP_MS)  /* (the gap exactly at the limit) */
            press(k[i].seq[j], t);
        CHECK(s_nact == 5);                              /* 4 skips at once, then the setting */
        for (int j = 0; j < 4; j++)
            CHECK(s_action[j] == (k[i].seq[j] == 'B' ? BT_KEY_NEXT : BT_KEY_PREV));
        CHECK(s_action[4] == k[i].want);
        t += 10000;
    }
    CHECK(!strcmp(bt_keys_name(BT_KEY_PLAY_MODE), "play mode"));
    CHECK(!strcmp(bt_keys_name(BT_KEY_VOL_DOWN), "BT volume down"));

    /* too slow: one gap over the limit, no setting (the skips still happen) */
    bt_keys_init();
    s_nact = 0;
    press('B', t);
    press('A', t + 1000);
    press('B', t + 1000 + BT_SEQ_GAP_MS + 1);
    press('A', t + 2000 + BT_SEQ_GAP_MS + 1);
    CHECK(s_nact == 4);
    t += 20000;

    /* another key inside it: no setting */
    s_nact = 0;
    press('B', t);
    press('A', t + 300);
    press('P', t + 600);
    press('B', t + 900);
    press('A', t + 1200);
    CHECK(s_nact == 5);
    for (int j = 0; j < s_nact; j++)
        CHECK(s_action[j] < BT_KEY_PLAY_MODE);
    t += 20000;

    /* each sequence counts once: B A B A B A is one play mode (the last two start afresh),
     * B A B A B A B A two */
    s_nact = 0;
    for (int j = 0; j < 6; j++)
        press(j & 1 ? 'A' : 'B', t + 300u * (uint32_t)j);
    CHECK(s_nact == 7 && s_action[4] == BT_KEY_PLAY_MODE);
    press('B', t + 1800);
    press('A', t + 2100);
    CHECK(s_nact == 10 && s_action[9] == BT_KEY_PLAY_MODE);
    t += 20000;

    /* the volume keys and the module's other letters never make one */
    s_nact = 0;
    for (int j = 0; j < 8; j++)
        press("DUMD"[j & 3], t + 200u * (uint32_t)j);
    CHECK(s_nact == 0);
}

enum { A_SINGLE = 1, A_DOUBLE, A_TRIPLE, A_OTHER, A_ALSO };

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

/* KEYSEQ_ALSO next to hold-back rules: it never delays them, nor they it */
static const keyseq_rule_t k_also_rules[] = {
    { 1, { 'P' }, 0, A_SINGLE, 0 },
    { 2, { 'P', 'P' }, 400, A_DOUBLE, 0 },
    { 1, { 'B' }, 0, A_OTHER, 0 },
    { 3, { 'B', 'B', 'B' }, 500, A_ALSO, KEYSEQ_ALSO },
};

static void test_also(void)
{
    keyseq_t k;

    keyseq_init(&k, k_also_rules, 4);
    CHECK(keyseq_feed(&k, 'B', 1000) == A_OTHER);       /* not held back for the triple */
    CHECK(keyseq_poll(&k, 1010) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'B', 1400) == A_OTHER);
    CHECK(keyseq_feed(&k, 'B', 1800) == A_OTHER);
    CHECK(keyseq_poll(&k, 1800) == A_ALSO);             /* in addition, from the next poll */
    CHECK(keyseq_poll(&k, 1810) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'B', 2000) == A_OTHER);       /* used up: a fourth B starts afresh */
    CHECK(keyseq_poll(&k, 2010) == KEYSEQ_NONE);

    /* a held-back double and the ALSO history side by side */
    CHECK(keyseq_feed(&k, 'P', 5000) == KEYSEQ_NONE);
    CHECK(keyseq_feed(&k, 'P', 5200) == A_DOUBLE);
    CHECK(keyseq_feed(&k, 'B', 5300) == A_OTHER);
    CHECK(keyseq_feed(&k, 'B', 5600) == A_OTHER);
    CHECK(keyseq_feed(&k, 'P', 5700) == KEYSEQ_NONE);   /* single P held back (a double possible) */
    CHECK(keyseq_poll(&k, 5710) == KEYSEQ_NONE);
    CHECK(keyseq_poll(&k, 6101) == A_SINGLE);
    CHECK(keyseq_feed(&k, 'B', 6200) == A_OTHER);       /* B B P B: no triple */
    CHECK(keyseq_poll(&k, 6210) == KEYSEQ_NONE);
}

int main(void)
{
    test_bt_mapping();
    test_sequences();
    test_also();
    test_bt_sequences();
    printf("test_keyseq: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
