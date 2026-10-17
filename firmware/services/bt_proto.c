/*
 * bt_proto.c -- the Bluetooth module's frames (see bt_proto.h for the forms
 * and the stock addresses they come from).
 */
#include "bt_proto.h"

#include <string.h>

/* ---- encoder ---------------------------------------------------------- */

int bt_enc_cmd(uint8_t *out, char cmd)          /* ram:03067658: 43 cmd 30 30 00 */
{
    out[0] = 'C';
    out[1] = (uint8_t)cmd;
    out[2] = '0';
    out[3] = '0';
    out[4] = 0;
    return 5;
}

int bt_enc_level(uint8_t *out, uint8_t v)       /* ram:0306763c: 43 59 30 v 00 */
{
    out[0] = 'C';
    out[1] = 'Y';
    out[2] = '0';
    out[3] = v;
    out[4] = 0;
    return 5;
}

int bt_enc_mac(uint8_t *out, char cmd, const uint8_t mac[6])   /* 43 4C|48 30 36 MAC[6] */
{
    out[0] = 'C';
    out[1] = (uint8_t)cmd;
    out[2] = '0';
    out[3] = '6';
    memcpy(out + 4, mac, BT_MAC_LEN);
    return 10;
}

/* ---- framer ----------------------------------------------------------- */

void bt_rx_init(bt_rx_t *r)
{
    memset(r, 0, sizeof *r);
}

static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static int is_key(uint8_t c)
{
    return c == 'P' || c == 'C' || c == 'A' || c == 'B' || c == 'D' || c == 'U' || c == 'M';
}

/* Letters whose frame is "up to 4 bytes" (fixed 4, or closed early at a gap). */
static int is_short(uint8_t c)
{
    return is_key(c) || c == 'L' || c == 'H' || c == 'T' || c == 'V' || c == 'K';
}

/* What the frame so far says: 1 = can go on, 0 = cannot (resync). Sets
 * r->want once the total length is known. */
static int header_ok(bt_rx_t *r)
{
    const uint8_t *b = r->buf;
    unsigned ll;

    switch (r->len) {
    case 1:
        return b[0] == 'C';
    case 2:
        return b[1] == 'S' || b[1] == 'Q' || is_short(b[1]);
    case 3:
        if (b[1] == 'S')
            return b[2] == 'E' || is_digit(b[2]);
        if (b[1] == 'Q')
            return b[2] == 'O' || b[2] == 'S' || b[2] == 'L' || b[2] == 'I';
        if (b[1] == 'V')
            return b[2] == '1';
        r->want = 4;
        return 1;
    case 4:
        if (b[1] == 'S' && b[2] == 'E') {
            r->want = 4;
            return b[3] == 'N' || b[3] == 'R';
        }
        if (b[1] == 'S') {
            if (!is_digit(b[3]))
                return 0;
            ll = (unsigned)(b[2] - '0') * 10u + (unsigned)(b[3] - '0');
            if (ll < BT_MAC_LEN || ll > BT_SCAN_LL_MAX)
                return 0;               /* (stock: LL > 0x25 dropped; < 6 has no MAC) */
            r->want = (uint8_t)(4u + ll);
            return 1;
        }
        if (b[1] == 'Q')
            return is_digit(b[3]);
        r->want = 4;
        return 1;
    case 5:
        if (b[1] == 'Q') {
            if (!is_digit(b[4]))
                return 0;
            ll = (unsigned)(b[3] - '0') * 10u + (unsigned)(b[4] - '0');
            if (ll > BT_QO_LL_MAX || (b[2] == 'O' && ll < BT_MAC_LEN))
                return 0;
            r->want = (uint8_t)(5u + ll);
        }
        return 1;
    default:
        return 1;
    }
}

static void deliver(bt_rx_t *r, bt_frame_t *f)
{
    const uint8_t *b = r->buf;
    unsigned n = r->len, nl;

    memset(f, 0, sizeof *f);
    memcpy(f->raw, b, n);
    f->raw_len = (uint8_t)n;
    f->cmd = (char)b[1];
    f->kind = BT_RX_UNKNOWN;
    switch (b[1]) {
    case 'V':
        if (n == 4 && b[2] == '1') {
            f->kind = BT_RX_HELLO;
            f->ver = (uint8_t)(b[3] - '0');
        }
        break;
    case 'S':
        if (b[2] == 'E')
            f->kind = b[3] == 'N' ? BT_RX_SCAN_END : BT_RX_SCAN_ERR;
        else {
            f->kind = BT_RX_SCAN;
            memcpy(f->mac, b + 4, BT_MAC_LEN);
            nl = n - 10u;
            memcpy(f->name, b + 10, nl > BT_NAME_MAX ? BT_NAME_MAX : nl);
        }
        break;
    case 'Q':
        f->kind = BT_RX_STATUS;
        f->state = (char)b[2];
        if (b[2] == 'O' && n >= 11u) {
            memcpy(f->mac, b + 5, BT_MAC_LEN);
            nl = n - 11u;
            memcpy(f->name, b + 11, nl > BT_NAME_MAX ? BT_NAME_MAX : nl);
        }
        break;
    case 'L':
    case 'H':
    case 'T':
        if (n == 4 && b[2] == 'O' && b[3] == 'K')
            f->kind = BT_RX_OK;
        else if (n == 4 && b[2] == 'E' && b[3] == 'R')
            f->kind = BT_RX_ERR;
        else if (b[1] == 'H')
            f->kind = BT_RX_LINKED;       /* seg84: any other CH... = connected */
        else if (b[1] == 'T')
            f->kind = BT_RX_OK;           /* resident ram:03067692: CT = disconnected */
        break;
    case 'K':
        f->kind = BT_RX_SKIP;
        break;
    default:
        if (is_key(b[1]))
            f->kind = BT_RX_KEY;
        break;
    }
    r->frames++;
    r->len = 0;
    r->want = 0;
}

int bt_rx_idle(bt_rx_t *r, uint32_t now_ms, bt_frame_t *f)
{
    if (!r->len || now_ms - r->last_ms < BT_RX_GAP_MS)
        return 0;
    if (r->len >= 2 && is_short(r->buf[1]) && r->buf[1] != 'V') {
        deliver(r, f);                  /* a short form that ended with its burst */
        return 1;
    }
    r->dropped += r->len;               /* stalled mid-frame: gone */
    r->len = 0;
    r->want = 0;
    return 0;
}

int bt_rx_byte(bt_rx_t *r, uint8_t b, uint32_t now_ms, bt_frame_t *f)
{
    uint8_t keep[BT_FRAME_MAX];
    unsigned n;

    if (r->len && now_ms - r->last_ms >= BT_RX_GAP_MS && bt_rx_idle(r, now_ms, f)) {
        /* the stalled frame closed: this byte starts afresh, and a frame it would
         * complete cannot exist (a frame needs at least 2 bytes) */
        if (b == 'C') {
            r->buf[0] = b;
            r->len = 1;
            r->last_ms = now_ms;
        } else {
            r->dropped++;
        }
        return 1;
    }
    r->last_ms = now_ms;
    if (r->len == 0 && b != 'C') {
        r->dropped++;
        return 0;
    }
    r->buf[r->len++] = b;
    if (header_ok(r)) {
        if (r->want && r->len >= r->want) {
            deliver(r, f);
            return 1;
        }
        return 0;
    }
    /* not a frame: drop its 'C' and look again from the next 'C'. Only header bytes
     * get here (at most 5, each refeed shorter, so this ends), and the refeed cannot
     * complete a frame: a 'C' in it has at most 3 bytes after it, and the only
     * frames that short (the 4-byte forms) are accepted whole at len 4 */
    n = r->len - 1u;
    memcpy(keep, r->buf + 1, n);
    r->len = 0;
    r->want = 0;
    r->dropped++;
    for (unsigned i = 0; i < n; i++)
        (void)bt_rx_byte(r, keep[i], now_ms, f);
    return 0;
}

void bt_mac_text(const uint8_t mac[6], char out[18])
{
    static const char hx[] = "0123456789ABCDEF";

    for (int i = 0; i < 6; i++) {
        out[i * 3] = hx[mac[i] >> 4];
        out[i * 3 + 1] = hx[mac[i] & 15];
        out[i * 3 + 2] = i < 5 ? ':' : 0;
    }
}

void bt_frame_text(const uint8_t *p, int n, char *out, int cap)
{
    static const char hx[] = "0123456789abcdef";
    int o = 0;

    for (int i = 0; i < n && o + 5 < cap; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '\\') {
            out[o++] = (char)p[i];
        } else {
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hx[p[i] >> 4];
            out[o++] = hx[p[i] & 15];
        }
    }
    out[o] = 0;
}
