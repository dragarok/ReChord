/*
 * bt_keys.h -- the Bluetooth earphone keys as a timestamped event stream.
 *
 * drivers/bt_jieli.c turns each key frame from the module (stock's RX forms
 * CP CC CA CB CD CU CM, 08_bluetooth.md §2.3) into bt_keys_event(code, t_ms):
 * the frame letter and the time it arrived. Every event is logged with that
 * time ("bt: key CB at 21034 ms"), so sequences can be designed from device
 * logs later. The events go through a key-sequence matcher (keyseq.h) with a
 * rule table; today it holds only one-event rules, stock's mapping
 * (seg84:0307b67c, resident ram:03067692):
 *   CP -> BT_KEY_PAUSE   (stock: event 0x1B7 toggle, only while playing)
 *   CC -> BT_KEY_PLAY    (stock: 0x1B7 only while paused)
 *   CA -> BT_KEY_PREV    (stock: 0x1B6; seg33 restarts the track after 3 s)
 *   CB -> BT_KEY_NEXT    (stock: 0x1B5)
 *   CD, CU, CM           recognised and ignored by stock: logged, no action
 * Settings driven by key sequences are NOT implemented: they will be rules in
 * the same table. The matched action goes to bt_keys_action() (weak; the UI
 * glue in startup/ap_bt.c drives the player).
 */
#ifndef RECHORD_BT_KEYS_H
#define RECHORD_BT_KEYS_H

#include <stdint.h>

enum {
    BT_KEY_NONE = 0,
    BT_KEY_PAUSE,
    BT_KEY_PLAY,
    BT_KEY_PREV,
    BT_KEY_NEXT,
};

void bt_keys_init(void);
/* One key frame: code = its letter ('P' 'C' 'A' 'B' 'D' 'U' 'M'), t_ms = when it came. */
void bt_keys_event(uint8_t code, uint32_t t_ms);
/* Time passing (every main-loop tick): a held-back sequence match fires here. */
void bt_keys_poll(uint32_t t_ms);
/* The matched action (weak default: nothing). */
void bt_keys_action(int action, uint32_t t_ms);
/* Events seen and actions run (for the screen and the log). */
uint32_t bt_keys_events(void);
const char *bt_keys_name(int action);

#endif
