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

/* Does rule r end these events (its last r->n codes, gaps within limit)? */
static int ends_with_in(const uint8_t *code, const uint32_t *t, int n, const keyseq_rule_t *r)
{
    int base = n - r->n;

    if (r->n < 1 || r->n > n)
        return 0;
    for (int i = 0; i < r->n; i++) {
        if (code[base + i] != r->codes[i])
            return 0;
        if (i && t[base + i] - t[base + i - 1] > r->max_gap_ms)
            return 0;
    }
    return 1;
}

static int ends_with(const keyseq_t *k, const keyseq_rule_t *r)
{
    return ends_with_in(k->code, k->t, k->n, r);
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

        if (!(r->flags & KEYSEQ_ALSO) && ends_with(k, r) && (!best || r->n > best->n))
            best = r;
    }
    for (int i = 0; i < k->nrules; i++) {
        const keyseq_rule_t *r = &k->rules[i];
        int p;

        if (!(r->flags & KEYSEQ_ALSO) && (!best || r->n > best->n) && (p = grows_into(k, r)) > 0) {
            if (r->max_gap_ms > *wait)
                *wait = r->max_gap_ms;
            if (p > *plen)
                *plen = p;
        }
    }
    return best;
}

/* The KEYSEQ_ALSO rules over the history: the longest one this event completes goes to k->also
 * (and the history starts afresh, so its events count once). */
static void feed_also(keyseq_t *k, uint8_t code, uint32_t t_ms)
{
    const keyseq_rule_t *best = 0;

    if (k->hn == KEYSEQ_MAX) {
        memmove(k->hcode, k->hcode + 1, KEYSEQ_MAX - 1);
        memmove(k->ht, k->ht + 1, (KEYSEQ_MAX - 1) * sizeof k->ht[0]);
        k->hn--;
    }
    k->hcode[k->hn] = code;
    k->ht[k->hn] = t_ms;
    k->hn++;
    for (int i = 0; i < k->nrules; i++) {
        const keyseq_rule_t *r = &k->rules[i];

        if ((r->flags & KEYSEQ_ALSO) && ends_with_in(k->hcode, k->ht, k->hn, r) && (!best || r->n > best->n))
            best = r;
    }
    if (best) {
        k->also = best->action;
        k->hn = 0;
    }
}

int keyseq_feed(keyseq_t *k, uint8_t code, uint32_t t_ms)
{
    const keyseq_rule_t *best;
    int old = k->pending, ret = KEYSEQ_NONE, plen;
    uint32_t wait;

    feed_also(k, code, t_ms);
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
    int a = k->also;

    if (a != KEYSEQ_NONE) {
        k->also = KEYSEQ_NONE;
        return a;
    }
    a = k->pending;

    if (a == KEYSEQ_NONE || (int32_t)(t_ms - k->pending_until) < 0)
        return KEYSEQ_NONE;
    k->pending = KEYSEQ_NONE;
    k->n = 0;
    return a;
}
