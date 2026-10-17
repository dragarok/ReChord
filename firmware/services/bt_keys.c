/*
 * bt_keys.c -- earphone key events -> sequence matcher -> actions (bt_keys.h).
 */
#include "bt_keys.h"
#include "keyseq.h"
#include "log.h"

/* One-event rules: stock's own mapping (bt_keys.h). Sequences go here later. */
static const keyseq_rule_t k_rules[] = {
    { 1, { 'P' }, 0, BT_KEY_PAUSE },
    { 1, { 'C' }, 0, BT_KEY_PLAY },
    { 1, { 'A' }, 0, BT_KEY_PREV },
    { 1, { 'B' }, 0, BT_KEY_NEXT },
};

static keyseq_t s_seq;
static uint32_t s_events;

__attribute__((weak)) void bt_keys_action(int action, uint32_t t_ms)
{
    (void)action;
    (void)t_ms;
}

const char *bt_keys_name(int action)
{
    static const char *const k[] = { "none", "pause", "play", "prev", "next" };

    return action >= 0 && action <= BT_KEY_NEXT ? k[action] : "?";
}

void bt_keys_init(void)
{
    keyseq_init(&s_seq, k_rules, (int)(sizeof k_rules / sizeof k_rules[0]));
    s_events = 0;
}

static void run(int a, uint32_t t_ms)
{
    if (a == KEYSEQ_NONE)
        return;
    log_printf("bt: key action %s", bt_keys_name(a));
    bt_keys_action(a, t_ms);
}

void bt_keys_event(uint8_t code, uint32_t t_ms)
{
    int a;

    if (!s_seq.rules)
        bt_keys_init();
    s_events++;
    a = keyseq_feed(&s_seq, code, t_ms);
    log_printf("bt: key C%c at %lu ms", code >= 0x20 && code < 0x7f ? code : '?', (unsigned long)t_ms);
    run(a, t_ms);
}

void bt_keys_poll(uint32_t t_ms)
{
    if (s_seq.rules)
        run(keyseq_poll(&s_seq, t_ms), t_ms);
}

uint32_t bt_keys_events(void) { return s_events; }
