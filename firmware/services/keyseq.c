/*
 * keyseq.c -- key-sequence matcher (see keyseq.h).
 */
#include "keyseq.h"

#include <string.h>

void keyseq_init(keyseq_t *k, const keyseq_rule_t *rules, int nrules)
{
    memset(k, 0, sizeof *k);
    k->rules = rules;
    k->nrules = nrules;
}

/* Does rule r end the recent events (its last r->n codes, gaps within limit)? */
static int ends_with(const keyseq_t *k, const keyseq_rule_t *r)
{
    int base = k->n - r->n;

    if (r->n < 1 || r->n > k->n)
        return 0;
    for (int i = 0; i < r->n; i++) {
        if (k->code[base + i] != r->codes[i])
            return 0;
        if (i && k->t[base + i] - k->t[base + i - 1] > r->max_gap_ms)
            return 0;
    }
    return 1;
}

/* Could rule r still complete: the recent events end with a proper prefix of it
 * (gaps within limit)? Returns the length of that prefix, 0 if none. */
static int grows_into(const keyseq_t *k, const keyseq_rule_t *r)
{
    for (int p = r->n - 1; p >= 1; p--) {
        int base = k->n - p, ok = base >= 0;

        for (int i = 0; ok && i < p; i++) {
            ok = k->code[base + i] == r->codes[i] &&
                 (!i || k->t[base + i] - k->t[base + i - 1] <= r->max_gap_ms);
        }
        if (ok)
            return p;
    }
    return 0;
}

/* The best rule ending the events, and how long to wait for a longer one (plen: the
 * longest prefix of a longer rule the events end with). */
static const keyseq_rule_t *eval(const keyseq_t *k, uint32_t *wait, int *plen)
{
    const keyseq_rule_t *best = 0;

    *wait = 0;
    *plen = 0;
    for (int i = 0; i < k->nrules; i++) {
        const keyseq_rule_t *r = &k->rules[i];

        if (ends_with(k, r) && (!best || r->n > best->n))
            best = r;
    }
    for (int i = 0; i < k->nrules; i++) {
        const keyseq_rule_t *r = &k->rules[i];
        int p;

        if ((!best || r->n > best->n) && (p = grows_into(k, r)) > 0) {
            if (r->max_gap_ms > *wait)
                *wait = r->max_gap_ms;
            if (p > *plen)
                *plen = p;
        }
    }
    return best;
}

int keyseq_feed(keyseq_t *k, uint8_t code, uint32_t t_ms)
{
    const keyseq_rule_t *best;
    int old = k->pending, ret = KEYSEQ_NONE, plen;
    uint32_t wait;

    if (k->n == KEYSEQ_MAX) {
        memmove(k->code, k->code + 1, KEYSEQ_MAX - 1);
        memmove(k->t, k->t + 1, (KEYSEQ_MAX - 1) * sizeof k->t[0]);
        k->n--;
    }
    k->code[k->n] = code;
    k->t[k->n] = t_ms;
    k->n++;
    best = eval(k, &wait, &plen);
    if (old != KEYSEQ_NONE && !((best && best->n >= 2) || plen >= 2)) {
        /* this event does not continue the held-back sequence: that one stands on its
         * own (it fires now), and this event starts afresh */
        ret = old;
        k->code[0] = code;
        k->t[0] = t_ms;
        k->n = 1;
        best = eval(k, &wait, &plen);
    }
    k->pending = KEYSEQ_NONE;
    if (!best)
        return ret;
    if (wait) {                         /* a longer rule still possible: hold back */
        k->pending = best->action;
        k->pending_until = t_ms + wait + 1u;
        return ret;
    }
    k->n = 0;                           /* matched: the events are used up */
    if (ret != KEYSEQ_NONE) {           /* (two at once: this one at the next poll) */
        k->pending = best->action;
        k->pending_until = t_ms;
        return ret;
    }
    return best->action;
}

int keyseq_poll(keyseq_t *k, uint32_t t_ms)
{
    int a = k->pending;

    if (a == KEYSEQ_NONE || (int32_t)(t_ms - k->pending_until) < 0)
        return KEYSEQ_NONE;
    k->pending = KEYSEQ_NONE;
    k->n = 0;
    return a;
}
