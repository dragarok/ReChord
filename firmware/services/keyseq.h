/*
 * keyseq.h -- a small key-sequence matcher over timestamped events.
 *
 * Generic on purpose: today the Bluetooth earphone keys (drivers/bt_jieli.c,
 * frames CP/CC/CA/CB/CD/CU) go through it with one-event rules (one frame =
 * one player action); later, settings driven by *sequences* of earphone keys,
 * and on-device gestures (double/triple/long press), can be added as rules
 * without touching the drivers. A rule is a list of codes with a largest gap
 * between neighbours. Matching:
 *   - the longest rule whose codes end the recent events (each gap within its
 *     limit) wins;
 *   - if the events so far could still grow into a longer rule, the match is
 *     held back and keyseq_poll() fires it once that longer rule can no longer
 *     complete (its gap ran out): a single press is told from a double press;
 *   - an event that matches nothing is passed back as KEYSEQ_NONE.
 * Pure code, host-tested (services/tests/test_keyseq.c).
 */
#ifndef RECHORD_KEYSEQ_H
#define RECHORD_KEYSEQ_H

#include <stdint.h>

#define KEYSEQ_MAX     6        /* longest rule, and the events kept */
#define KEYSEQ_NONE    0

typedef struct {
    uint8_t n;                  /* 1..KEYSEQ_MAX codes */
    uint8_t codes[KEYSEQ_MAX];
    uint16_t max_gap_ms;        /* largest time between neighbouring events (ignored for n = 1) */
    int action;                 /* returned when it matches (not KEYSEQ_NONE) */
} keyseq_rule_t;

typedef struct {
    const keyseq_rule_t *rules;
    int nrules;
    uint8_t code[KEYSEQ_MAX];   /* recent events, oldest first */
    uint32_t t[KEYSEQ_MAX];
    int n;
    int pending;                /* an action held back for a longer rule, or KEYSEQ_NONE */
    uint32_t pending_until;     /* keyseq_poll fires it from this time */
} keyseq_t;

void keyseq_init(keyseq_t *k, const keyseq_rule_t *rules, int nrules);
/* An event: returns the action to run now, or KEYSEQ_NONE. */
int keyseq_feed(keyseq_t *k, uint8_t code, uint32_t t_ms);
/* Time passing: returns a held-back action whose wait is over, or KEYSEQ_NONE. */
int keyseq_poll(keyseq_t *k, uint32_t t_ms);

#endif
