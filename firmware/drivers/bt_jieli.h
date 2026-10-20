/*
 * bt_jieli.h -- the Bluetooth module (a JieLi part on UART1), driven as stock
 * MINIV390 (512 MB) drives it. Sources: work/stock_boot/08_bluetooth.md
 * (addresses MINIV390: "ram:" seg0 at 0x03050000, "segNN:" overlays); the
 * frames are services/bt_proto.h.
 *
 *   link      UART1 0x400B0000, 115200 8N1, no flow control, IRQ 25 (vector
 *             0x29, ram:03067fe0), TX GPIO2.16 / RX GPIO2.17 func1 (GRF
 *             0x50010048, ram:03056f50..03056f7a)
 *   power     GPIO2.7 = 1 on, 0 off; while off GPIO2.16/2.17 are GPIO outputs
 *             at 0 (every off site, e.g. seg84:0307a0d6). No reset/status pin.
 *   audio     analog: connected -> GPIO2.0 = 1, GPIO2.14 = 0 (ram:03067aa6,
 *             ram:03067ac4); disconnected -> 2.0 = 0, 2.14 = 1 (ram:03067b0e,
 *             ram:03067b2c). The jack wins (headphone task ram:03066494) --
 *             not wired here: ReChord has no headphone sense yet (bt_jack_in()).
 *
 * Non-blocking: bt_poll() from the main loop (every 10 ms tick) runs the
 * power-on sequence, the scan, the connect, the status poll, the reconnect and
 * the idle power-off (5 min: stock's "30000" counts 10 ms ticks, see bt_jieli.c;
 * only time this code was served counts). Nothing is ever printed on UART1
 * (stock's fault handler prints there; ours does not touch UART1 at all).
 */
#ifndef RECHORD_BT_JIELI_H
#define RECHORD_BT_JIELI_H

#include <stdint.h>

#include "services/bt_proto.h"

#define BT_SCAN_MAX 6                 /* seg84 0307b41a: at most 6 entries */

typedef enum {
    BT_OFF = 0,
    BT_STARTING,                      /* pins low 100 ms, then powered, waiting for CV1x (5 s) */
    BT_NO_MODULE,                     /* no CV1x within 5 s: off again (stock: screen state 7) */
    BT_ON,                            /* powered, not connected */
    BT_SCANNING,                      /* CS00 sent, collecting CS frames */
    BT_CONNECTING,                    /* CL06 sent (CH06 at +400 ms), or paired and waiting for CQO */
    BT_CONNECTED,                     /* CQO / CH seen: audio routed to the module */
} bt_state_t;

typedef struct {
    uint8_t mac[BT_MAC_LEN];
    char name[BT_NAME_MAX + 1];
} bt_dev_t;

/* Boot levels, stock Main step 1 (ram:03062a74..03062ab0): GPIO2.7 = 0,
 * GPIO2.17 = 0, GPIO2.16 = 0 (GPIO outputs): the module off. (Stock's
 * GPIO2.14 = 1 / GPIO2.0 = 0 there are left to the DAC driver.)
 * From board_early_init(). */
void bt_early_pins(void);

/* Every power-off of the player (board_power_off_pins): stock's shutdown,
 * ram:0306745c: GPIO2.0 = 1, GPIO2.7 = 0, GPIO2.16 = GPIO2.17 = 0, GPIO2.14 = 1.
 * Only if the module was powered this boot (else nothing: the pins are as at boot). */
void bt_board_off(void);

void bt_poll(void);                    /* every main-loop tick */
int  bt_power(int on);                 /* 0 */
int  bt_scan(void);                    /* powers on first if needed; 0, or < 0 */
int  bt_connect(const uint8_t mac[6], const char *name);   /* 0, or < 0 */
int  bt_disconnect(void);              /* 0, or < 0 (not connected) */

bt_state_t bt_state(void);
const char *bt_state_name(bt_state_t s);
int  bt_version(void);                 /* the x of CV1x (5 = what stock gates 0x86/0x87 on), -1 = none yet */
int  bt_scan_count(void);
const bt_dev_t *bt_scan_dev(int i);
const bt_dev_t *bt_peer(void);         /* the connected (or connecting) device */
uint32_t bt_seq(void);                 /* changes whenever anything shown changed */
int  bt_routed(void);                  /* GPIO2.0 = 1 / GPIO2.14 = 0 now */

/* The BT screen is open: stock does not idle-off the module meanwhile, and
 * polls every 3 s instead of 5 (seg84 0307b394 vs ram:030677ae). */
void bt_ui_active(int on);
/* The player's state, every tick: on a change while connected stock sends
 * CY0+30 (playing) / CY0+0 (paused) (0x1D5/0x1D4, ram:03067b5a/03067bcc), and
 * to a version-5 module C\x86 / C\x87 (seg27 0307dec8 / 0307de7e, gate ram:0306765c).
 * debounce: a stop goes out only once it lasted 300 ms (a USB host's flips); 0 for the
 * player's own pause, which stops the music at once (the module's stream, its noise floor
 * alone, would be heard for those 300 ms: the user, 2026-10-20). */
void bt_set_playing(int playing, int debounce);

/* The power-on hook for the boot auto-on (stock seg21 0307a4d2 when the
 * setting is on): power on and reconnect to (mac): stock's two tries 3 s apart,
 * then [ours] every 10, 20, then 30 s until it links. A link lost without the
 * user's Disconnect is reconnected the same way. */
void bt_autostart(const uint8_t mac[6], const char *name);

/* [ours] The source started playing (the player, or a USB DAC host streaming)
 * with Bluetooth wanted: bring the link to (mac) back if it is down. The module
 * off after its idle time: power on and reconnect; on and unlinked: reconnect
 * now. Nothing after the user's Disconnect (until a connect or the next boot). */
void bt_wake(const uint8_t mac[6], const char *name);

/* 1 = a jack is in (the jack then wins the audio path). Weak, 0: ReChord has
 * no headphone sense yet (stock reads GPIO2.8 / GPIO0.13: polarity unverified). */
int bt_jack_in(void);

/* What happened, for the UI glue (weak default: nothing). dev is the device concerned. */
enum {
    BT_EV_PAIRED = 1,                  /* CLOK: remember it (stock: settings slot +0x349) */
    BT_EV_CONNECTED,                   /* CQO / CH: routed */
    BT_EV_DISCONNECTED,
    BT_EV_CONNECT_FAILED,              /* CLER / CHER (also right after a CH link), or 3 s without an answer (then CT00) */
    BT_EV_NO_MODULE,                   /* no CV1x in 5 s (stock also turns the setting off) */
    BT_EV_IDLE_OFF,                    /* 5 min unconnected outside the screen, the source not
                                        * playing (stock: setting off; ReChord keeps it: bt_wake) */
};
void bt_event(int ev, const bt_dev_t *dev);

/* Counters for the log/screen: frames in, bytes dropped by the framer, TX timeouts. */
void bt_stats(uint32_t *rx_frames, uint32_t *rx_dropped, uint32_t *tx_fail);

/* [ours] Developer > BT listen (test): what the module says on its own, sending it nothing.
 * Powers it up twice the stock way (off, pins low, 100 ms, UART open, GPIO2.7 = 1), listening
 * ms each time, then leaves it off (state BT_OFF; a powered module is switched off first):
 *   1  UART1 at 115200 8N1 (as stock), polled: each byte with its time and line status
 *   2  GPIO2.17 (the module's TX) as a GPIO input: the time of every edge, any baud rate
 * Our TX (GPIO2.16) stays on the UART, idle-high, throughout; THR is never written. buf/words:
 * the store (a quarter for the bytes, two words each; the rest for the edges). Blocks for
 * 2 x ms + 0.7 s, feeding the watchdog. Decode on a computer: tools/bt_listen_decode.py. */
typedef struct {
    uint32_t core_hz;                  /* edge times are in cycles of this clock */
    uint32_t *bytes;                   /* nbytes x { us since power-on, lsr << 8 | byte } */
    uint32_t *edges;                   /* nedges x cycles since power-on; the first leaves level0 */
    uint16_t nbytes, nedges, lsr_errs;
    uint8_t level0, full;              /* full: bit 0 the bytes, bit 1 the edges ran out of room */
} bt_listen_t;
int bt_listen(bt_listen_t *r, uint32_t *buf, unsigned words, uint32_t ms);

#endif
