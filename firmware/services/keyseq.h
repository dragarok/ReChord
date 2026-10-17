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
 * A rule flagged KEYSEQ_ALSO is matched on its own, over the whole recent
 * history: its events keep their own (one-event) actions, run at once and
 * never held back for it, and when it completes its action comes in addition,
 * from the next keyseq_poll(). That is how a setting rides on keys that keep
 * working as before (Bluetooth: services/bt_keys.c).
 * Pure code, host-tested (services/tests/test_keyseq.c).
 */
#ifndef RECHORD_KEYSEQ_H
#define RECHORD_KEYSEQ_H

#include <stdint.h>

#define KEYSEQ_MAX     6        /* longest rule, and the events kept */
#define KEYSEQ_NONE    0
#define KEYSEQ_ALSO    1        /* keyseq_rule_t.flags: in addition to its events' own actions */

typedef struct {
    uint8_t n;                  /* 1..KEYSEQ_MAX codes */
    uint8_t codes[KEYSEQ_MAX];
    uint16_t max_gap_ms;        /* largest time between neighbouring events (ignored for n = 1) */
    int action;                 /* returned when it matches (not KEYSEQ_NONE) */
    uint8_t flags;              /* KEYSEQ_ALSO, or 0 */
} keyseq_rule_t;

typedef struct {
    const keyseq_rule_t *rules;
    int nrules;
    uint8_t code[KEYSEQ_MAX];   /* recent events, oldest first */
    uint32_t t[KEYSEQ_MAX];
    int n;
    int pending;                /* an action held back for a longer rule, or KEYSEQ_NONE */
    uint32_t pending_until;     /* keyseq_poll fires it from this time */
    uint8_t hcode[KEYSEQ_MAX];  /* the history KEYSEQ_ALSO rules match on (not used up by others) */
    uint32_t ht[KEYSEQ_MAX];
    int hn;
    int also;                   /* a KEYSEQ_ALSO action for the next keyseq_poll, or KEYSEQ_NONE */
} keyseq_t;

void keyseq_init(keyseq_t *k, const keyseq_rule_t *rules, int nrules);
/* An event: returns the action to run now, or KEYSEQ_NONE. */
int keyseq_feed(keyseq_t *k, uint8_t code, uint32_t t_ms);
/* Time passing: returns a completed KEYSEQ_ALSO action, else a held-back action whose wait
 * is over, else KEYSEQ_NONE (one per call: call it every tick). */
int keyseq_poll(keyseq_t *k, uint32_t t_ms);

#endif
