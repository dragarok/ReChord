/*
 * bt_proto.h -- the Bluetooth module's UART protocol: frame encoder and a
 * stream framer/parser. Pure code (no hardware): host-tested in
 * services/tests/test_bt_proto.c, used by drivers/bt_jieli.c.
 *
 * Everything here is the stock firmware's own protocol, MINIV390 (512 MB),
 * reverse-engineered in work/stock_boot/08_bluetooth.md (§2). Addresses are
 * MINIV390: "ram:" = seg0 at 0x03050000, "segNN:" = that overlay.
 *
 * Frames are ASCII-led: 'C' <cmd> then, by form:
 *   TX (what we send; the only forms stock builds, §2.2):
 *     'C' cmd '0' '0' 0x00             5 bytes  ram:03067658 (CS CP CT CQ CH 0x86 0x87)
 *     'C' 'Y' '0' v 0x00               5 bytes  ram:0306763c (v = 0x1E or 0)
 *     'C' 'L'|'H' '0' '6' MAC[6]       10 bytes inline builders (seg84:0307b288,
 *                                               resident ram:03067692)
 *   RX (what stock parses, §2.3; seg84:0307b0be body 0x0307b3d6..0x0307b91e):
 *     'C' 'S' d d MAC[6] name[LL-6]    scan result, LL = 2 ASCII digits, LL <= 37
 *                                      (stock drops LL > 0x25)
 *     'C' 'Q' s d d [MAC[6] name]      status, s = 'O' (connected: LL = 6 + name
 *                                      length, name <= 32) / 'S' / 'L' / 'I'
 *                                      (stock reads the length digits at +3/+4
 *                                      for 'O' only; the "00" after S/L/I is
 *                                      [inferred] from the same form)
 *     'C' 'S' 'E' 'N'|'R'              scan finished / scan error
 *     'C' 'L'|'H'|'T' 'O' 'K'          connect / second-stage / disconnect ok
 *     'C' 'L'|'H' 'E' 'R'              connect failed
 *     'C' 'V' '1' x                    hello at power-on, x = '5': version 5
 *     'C' 'K' 'K' 'K'                  skipped by stock
 *     'C' 'P'|'C'|'A'|'B'|'D'|'U'|'M' ..  earphone keys / AVRCP state: stock reads
 *                                      only the letter. The two bytes after it
 *                                      are [inferred]: this framer takes up to 4
 *                                      bytes and closes the frame early at a gap
 *                                      in the byte stream (bt_rx_idle), so a
 *                                      2-byte "CP" works as well as "CP00".
 *     'C' 'H' ..                       any other CH: connected (stock: event 0x1C8)
 *
 * Unlike stock (one 100-byte buffer refilled from offset 0 at every burst,
 * only the first frame of a burst parsed: ram:0305180c), the framer here takes
 * a byte stream: frames split across bursts, several frames in one burst and
 * garbage between frames all parse. Bounded: no frame is longer than
 * BT_FRAME_MAX; a frame that stalls mid-way for BT_RX_GAP_MS is closed (short
 * fixed forms) or dropped.
 */
#ifndef RECHORD_BT_PROTO_H
#define RECHORD_BT_PROTO_H

#include <stdint.h>

#define BT_MAC_LEN      6
#define BT_NAME_MAX     32              /* stock: name32 at settings +0x328, slots MAC6+name32 */
#define BT_SCAN_LL_MAX  37              /* seg84 0307b41a: LL > 0x25 dropped */
#define BT_QO_LL_MAX    (6 + BT_NAME_MAX)
#define BT_FRAME_MAX    (5 + BT_QO_LL_MAX)
#define BT_TX_MAX       10              /* the longest TX form (CL06 + MAC) */
#define BT_RX_GAP_MS    20u             /* [inferred] about 230 byte times at 115200: a burst has ended */

typedef enum {
    BT_RX_NONE = 0,
    BT_RX_HELLO,        /* CV1x: .ver = x - '0' (5 = the version stock gates 0x86/0x87 on) */
    BT_RX_SCAN,         /* CS LL MAC name */
    BT_RX_SCAN_END,     /* CSEN */
    BT_RX_SCAN_ERR,     /* CSER */
    BT_RX_OK,           /* C<cmd>OK: .cmd = 'L' 'H' 'T' */
    BT_RX_ERR,          /* C<cmd>ER: .cmd = 'L' 'H' */
    BT_RX_STATUS,       /* CQ<s>: .state = 'O' (MAC/name valid) 'S' 'L' 'I' */
    BT_RX_KEY,          /* C<letter>: .cmd = 'P' 'C' 'A' 'B' 'D' 'U' 'M' */
    BT_RX_LINKED,       /* any other CH...: connected (stock 0x1C8) */
    BT_RX_SKIP,         /* CKKK */
    BT_RX_UNKNOWN,      /* a well-formed 4-byte frame of an unknown letter */
} bt_rx_kind_t;

typedef struct {
    bt_rx_kind_t kind;
    char cmd;                   /* the letter after 'C' */
    char state;                 /* BT_RX_STATUS */
    uint8_t ver;                /* BT_RX_HELLO */
    uint8_t mac[BT_MAC_LEN];    /* BT_RX_SCAN, BT_RX_STATUS 'O' */
    char name[BT_NAME_MAX + 1]; /* UTF-8, NUL-terminated (stock converts for display: seg84:0307a2b0) */
    uint8_t raw[BT_FRAME_MAX];  /* the frame as received (for the log) */
    uint8_t raw_len;
} bt_frame_t;

/* ---- encoder: returns the frame length (bytes to send) ---- */
int bt_enc_cmd(uint8_t *out, char cmd);                         /* C cmd 0 0 \0, 5 */
int bt_enc_level(uint8_t *out, uint8_t v);                      /* C Y 0 v \0, 5 */
int bt_enc_mac(uint8_t *out, char cmd, const uint8_t mac[6]);   /* C L|H 0 6 MAC, 10 */

/* ---- framer ---- */
typedef struct {
    uint8_t buf[BT_FRAME_MAX];
    uint8_t len;                /* bytes of the frame being assembled */
    uint8_t want;               /* its total length once known, 0 = not yet */
    uint32_t last_ms;           /* time of its last byte */
    uint32_t dropped;           /* bytes thrown away (garbage, bad lengths, stalls) */
    uint32_t frames;            /* frames delivered */
} bt_rx_t;

void bt_rx_init(bt_rx_t *r);

/* Feed one byte received at now_ms. Returns 1 and fills *f when a frame
 * completes, else 0. A byte that cannot continue the current frame restarts
 * the search at it (a new 'C' starts a new frame). */
int bt_rx_byte(bt_rx_t *r, uint8_t b, uint32_t now_ms, bt_frame_t *f);

/* Call when no byte came for a while: a frame stalled for BT_RX_GAP_MS is
 * closed if it is a complete short form (a 2- or 3-byte key frame), else
 * dropped. Returns 1 with *f filled when it closes one. */
int bt_rx_idle(bt_rx_t *r, uint32_t now_ms, bt_frame_t *f);

/* "AA:BB:CC:DD:EE:FF" (18 bytes with the NUL) */
void bt_mac_text(const uint8_t mac[6], char out[18]);
/* Printable form of a raw frame for the log: letters as is, others as \xNN. */
void bt_frame_text(const uint8_t *p, int n, char *out, int cap);

#endif
