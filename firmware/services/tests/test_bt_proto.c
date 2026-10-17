/*
 * test_bt_proto.c -- host tests for the Bluetooth module's frames (bt_proto.h).
 *
 * Covers: the three TX forms byte for byte (stock ram:03067658, 0306763c, the
 * CL06/CH06 builders); every RX form stock parses (08_bluetooth.md §2.3);
 * a frame split at every byte across two bursts; several frames merged in one
 * burst; garbage before, between and inside frames; overlong and malformed
 * lengths (CS LL > 37, CQO LL < 6, non-digit lengths); a frame that stalls
 * mid-way (dropped) vs a 2-byte key frame that ends with its burst (closed);
 * names with 'C' in them; the bounded buffer never overruns (fuzz).
 */
#include "bt_proto.h"

#include <stdio.h>
#include <stdlib.h>
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

static const uint8_t MAC[6] = { 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5 };

/* Feed bytes at time t; collect up to 16 frames. Returns how many came. */
static bt_frame_t s_out[16];
static int feed(bt_rx_t *r, const void *p, int n, uint32_t t)
{
    int got = 0;

    for (int i = 0; i < n; i++)
        if (bt_rx_byte(r, ((const uint8_t *)p)[i], t, &s_out[got < 15 ? got : 15]))
            got++;
    return got;
}

static int idle(bt_rx_t *r, uint32_t t, int slot)
{
    return bt_rx_idle(r, t, &s_out[slot]);
}

/* "CS" LL MAC name */
static int scan_frame(uint8_t *o, const uint8_t *mac, const char *name, int ll_override)
{
    int nl = (int)strlen(name), ll = ll_override >= 0 ? ll_override : 6 + nl;

    o[0] = 'C';
    o[1] = 'S';
    o[2] = (uint8_t)('0' + ll / 10);
    o[3] = (uint8_t)('0' + ll % 10);
    memcpy(o + 4, mac, 6);
    memcpy(o + 10, name, (size_t)nl);
    return 10 + nl;
}

static int status_o(uint8_t *o, const uint8_t *mac, const char *name)
{
    int nl = (int)strlen(name), ll = 6 + nl;

    memcpy(o, "CQO", 3);
    o[3] = (uint8_t)('0' + ll / 10);
    o[4] = (uint8_t)('0' + ll % 10);
    memcpy(o + 5, mac, 6);
    memcpy(o + 11, name, (size_t)nl);
    return 11 + nl;
}

static void test_encoder(void)
{
    uint8_t b[16];
    static const uint8_t cs[5] = { 0x43, 0x53, 0x30, 0x30, 0x00 };
    static const uint8_t cy30[5] = { 0x43, 0x59, 0x30, 0x1E, 0x00 };
    static const uint8_t cy0[5] = { 0x43, 0x59, 0x30, 0x00, 0x00 };
    static const uint8_t c86[5] = { 0x43, 0x86, 0x30, 0x30, 0x00 };
    static const uint8_t cl[10] = { 0x43, 0x4C, 0x30, 0x36, 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5 };
    static const uint8_t ch[10] = { 0x43, 0x48, 0x30, 0x36, 0xA0, 0xB1, 0xC2, 0xD3, 0xE4, 0xF5 };

    CHECK(bt_enc_cmd(b, 'S') == 5 && !memcmp(b, cs, 5));
    CHECK(bt_enc_cmd(b, (char)0x86) == 5 && !memcmp(b, c86, 5));
    CHECK(bt_enc_level(b, 0x1E) == 5 && !memcmp(b, cy30, 5));
    CHECK(bt_enc_level(b, 0) == 5 && !memcmp(b, cy0, 5));
    CHECK(bt_enc_mac(b, 'L', MAC) == 10 && !memcmp(b, cl, 10));
    CHECK(bt_enc_mac(b, 'H', MAC) == 10 && !memcmp(b, ch, 10));
    CHECK(BT_TX_MAX >= 10);
}

static void test_forms(void)
{
    bt_rx_t r;
    uint8_t b[64];
    int n;

    bt_rx_init(&r);
    CHECK(feed(&r, "CV15", 4, 0) == 1 && s_out[0].kind == BT_RX_HELLO && s_out[0].ver == 5);
    CHECK(feed(&r, "CV10", 4, 0) == 1 && s_out[0].kind == BT_RX_HELLO && s_out[0].ver == 0);

    n = scan_frame(b, MAC, "Buds One", -1);
    CHECK(feed(&r, b, n, 0) == 1);
    CHECK(s_out[0].kind == BT_RX_SCAN && !memcmp(s_out[0].mac, MAC, 6) && !strcmp(s_out[0].name, "Buds One"));
    CHECK(s_out[0].raw_len == n);
    n = scan_frame(b, MAC, "", -1);                       /* a nameless device: LL 06 */
    CHECK(feed(&r, b, n, 0) == 1 && s_out[0].kind == BT_RX_SCAN && s_out[0].name[0] == 0);

    CHECK(feed(&r, "CSEN", 4, 0) == 1 && s_out[0].kind == BT_RX_SCAN_END);
    CHECK(feed(&r, "CSER", 4, 0) == 1 && s_out[0].kind == BT_RX_SCAN_ERR);
    CHECK(feed(&r, "CLOK", 4, 0) == 1 && s_out[0].kind == BT_RX_OK && s_out[0].cmd == 'L');
    CHECK(feed(&r, "CHOK", 4, 0) == 1 && s_out[0].kind == BT_RX_OK && s_out[0].cmd == 'H');
    CHECK(feed(&r, "CTOK", 4, 0) == 1 && s_out[0].kind == BT_RX_OK && s_out[0].cmd == 'T');
    CHECK(feed(&r, "CLER", 4, 0) == 1 && s_out[0].kind == BT_RX_ERR && s_out[0].cmd == 'L');
    CHECK(feed(&r, "CHER", 4, 0) == 1 && s_out[0].kind == BT_RX_ERR && s_out[0].cmd == 'H');
    CHECK(feed(&r, "CH12", 4, 0) == 1 && s_out[0].kind == BT_RX_LINKED);
    CHECK(feed(&r, "CKKK", 4, 0) == 1 && s_out[0].kind == BT_RX_SKIP);

    CHECK(feed(&r, "CQI00", 5, 0) == 1 && s_out[0].kind == BT_RX_STATUS && s_out[0].state == 'I');
    CHECK(feed(&r, "CQS00", 5, 0) == 1 && s_out[0].kind == BT_RX_STATUS && s_out[0].state == 'S');
    CHECK(feed(&r, "CQL00", 5, 0) == 1 && s_out[0].kind == BT_RX_STATUS && s_out[0].state == 'L');
    n = status_o(b, MAC, "Buds One");
    CHECK(feed(&r, b, n, 0) == 1);
    CHECK(s_out[0].kind == BT_RX_STATUS && s_out[0].state == 'O' && !memcmp(s_out[0].mac, MAC, 6) &&
          !strcmp(s_out[0].name, "Buds One"));

    for (const char *k = "PCABDUM"; *k; k++) {             /* the key letters, 4-byte form */
        char f[5] = { 'C', *k, '0', '0', 0 };

        CHECK(feed(&r, f, 4, 0) == 1 && s_out[0].kind == BT_RX_KEY && s_out[0].cmd == *k);
    }
    CHECK(r.dropped == 0);
}

/* A frame split at every position across two bursts 10 ms apart: one frame, same content. */
static void test_split(void)
{
    uint8_t b[64];
    int n = scan_frame(b, MAC, "Split C Name", -1);

    for (int cut = 1; cut < n; cut++) {
        bt_rx_t r;
        int a, c;

        bt_rx_init(&r);
        a = feed(&r, b, cut, 100);
        c = feed(&r, b + cut, n - cut, 110);
        CHECK(a == 0 && c == 1);
        CHECK(s_out[0].kind == BT_RX_SCAN && !strcmp(s_out[0].name, "Split C Name"));
        CHECK(r.dropped == 0);
    }
    /* a 5-byte status split byte by byte, 5 ms apart */
    {
        bt_rx_t r;
        int got = 0;

        bt_rx_init(&r);
        for (int i = 0; i < 5; i++)
            got += feed(&r, &"CQI00"[i], 1, (uint32_t)(i * 5));
        CHECK(got == 1 && s_out[0].kind == BT_RX_STATUS && s_out[0].state == 'I');
    }
}

/* Several frames in one burst, garbage around them. */
static void test_merged_and_garbage(void)
{
    bt_rx_t r;
    uint8_t b[256];
    int n = 0, got;

    bt_rx_init(&r);
    memcpy(b + n, "\x00\xffxyz", 5); n += 5;              /* garbage */
    n += scan_frame(b + n, MAC, "One", -1);
    memcpy(b + n, "CSEN", 4); n += 4;
    memcpy(b + n, "\r\nPC:0300", 9); n += 9;              /* fault-handler text (stock prints on UART1) */
    memcpy(b + n, "CQI00", 5); n += 5;
    memcpy(b + n, "CB00", 4); n += 4;
    got = feed(&r, b, n, 0);
    CHECK(got == 4);
    CHECK(s_out[0].kind == BT_RX_SCAN && !strcmp(s_out[0].name, "One"));
    CHECK(s_out[1].kind == BT_RX_SCAN_END);
    CHECK(s_out[2].kind == BT_RX_STATUS && s_out[2].state == 'I');
    CHECK(s_out[3].kind == BT_RX_KEY && s_out[3].cmd == 'B');
    CHECK(r.dropped > 0);

    /* "CX" is not a frame: the framer resyncs on the next 'C' */
    bt_rx_init(&r);
    CHECK(feed(&r, "CXCV15", 6, 0) == 1 && s_out[0].kind == BT_RX_HELLO);
    /* "CSE?" bad, "CQZ" bad, "CV2" bad: each resyncs */
    CHECK(feed(&r, "CSEXCQZ0CV2xCTOK", 16, 0) == 1 && s_out[0].kind == BT_RX_OK && s_out[0].cmd == 'T');
}

static void test_lengths(void)
{
    bt_rx_t r;
    uint8_t b[128];
    int n;

    bt_rx_init(&r);
    /* CS LL = 38 (> 37, stock drops): rejected, and what follows still parses */
    n = scan_frame(b, MAC, "", 38);
    memcpy(b + n, "CQI00", 5);
    CHECK(feed(&r, b, n + 5, 0) == 1 && s_out[0].kind == BT_RX_STATUS);
    /* CS LL = 37 is the largest accepted: 31-byte name */
    n = scan_frame(b, MAC, "abcdefghijklmnopqrstuvwxyz01234", -1);
    CHECK(n == 41 && feed(&r, b, n, 0) == 1 && s_out[0].kind == BT_RX_SCAN && strlen(s_out[0].name) == 31);
    /* CS LL = 99 (overlong): rejected */
    CHECK(feed(&r, "CS99", 4, 0) == 0 && r.len == 0);
    /* CS LL < 6 (no room for a MAC) */
    CHECK(feed(&r, "CS05", 4, 0) == 0 && r.len == 0);
    /* non-digit length */
    CHECK(feed(&r, "CS0x", 4, 0) == 0 && r.len == 0);
    CHECK(feed(&r, "CQOx0", 5, 0) == 0 && r.len == 0);
    /* CQO with LL < 6, and LL > 38 */
    CHECK(feed(&r, "CQO05", 5, 0) == 0 && r.len == 0);
    CHECK(feed(&r, "CQO39", 5, 0) == 0 && r.len == 0);
    /* CQO LL = 38: a 32-byte name, the most stock keeps */
    n = status_o(b, MAC, "abcdefghijklmnopqrstuvwxyz012345");
    CHECK(feed(&r, b, n, 0) == 1 && s_out[0].state == 'O' && strlen(s_out[0].name) == 32);
    CHECK(BT_FRAME_MAX == 43);
}

static void test_timeouts(void)
{
    bt_rx_t r;
    uint8_t b[64];
    int n = scan_frame(b, MAC, "Stalled", -1);

    /* a scan frame that stalls mid-way: dropped at the gap, then a new frame parses */
    bt_rx_init(&r);
    CHECK(feed(&r, b, 8, 0) == 0);
    CHECK(idle(&r, BT_RX_GAP_MS - 1, 0) == 0 && r.len == 8);   /* not yet */
    CHECK(idle(&r, BT_RX_GAP_MS, 0) == 0 && r.len == 0 && r.dropped == 8);
    CHECK(feed(&r, "CV15", 4, 100) == 1 && s_out[0].kind == BT_RX_HELLO);

    /* the stalled frame's tail arriving late does not become a frame */
    bt_rx_init(&r);
    CHECK(feed(&r, b, 8, 0) == 0);
    CHECK(feed(&r, b + 8, n - 8, 1000) == 0);

    /* a 2-byte key frame "CP" that ends with its burst: closed at the gap */
    bt_rx_init(&r);
    CHECK(feed(&r, "CP", 2, 0) == 0);
    CHECK(idle(&r, BT_RX_GAP_MS, 0) == 1 && s_out[0].kind == BT_RX_KEY && s_out[0].cmd == 'P');
    /* ... or by the next frame's first byte after the gap */
    CHECK(feed(&r, "CC", 2, 1000) == 0);
    CHECK(feed(&r, "CB00", 4, 1000 + BT_RX_GAP_MS) == 2);
    CHECK(s_out[0].kind == BT_RX_KEY && s_out[0].cmd == 'C');
    CHECK(s_out[1].kind == BT_RX_KEY && s_out[1].cmd == 'B');
    /* "CV1" stalled is not a short form: dropped */
    bt_rx_init(&r);
    CHECK(feed(&r, "CV1", 3, 0) == 0 && idle(&r, 1000, 0) == 0 && r.dropped == 3);
}

/* Random bytes, and random cuts of valid frames: never more than BT_FRAME_MAX
 * buffered, every frame delivered is one of the valid kinds, no crash. */
static void test_fuzz(void)
{
    bt_rx_t r;
    bt_frame_t f;
    uint8_t b[64];
    unsigned seed = 12345;
    int n = scan_frame(b, MAC, "Fuzz", -1), whole = 0;

    bt_rx_init(&r);
    for (int i = 0; i < 200000; i++) {
        uint8_t c;

        seed = seed * 1103515245u + 12345u;
        c = (uint8_t)(seed >> 16);
        if ((seed >> 8) % 7 == 0)
            c = 'C';
        if (bt_rx_byte(&r, c, (uint32_t)i, &f))
            CHECK(f.kind >= BT_RX_HELLO && f.kind <= BT_RX_UNKNOWN && f.raw_len <= BT_FRAME_MAX);
        if (r.len > BT_FRAME_MAX)
            CHECK(0);
        if (i % 997 == 0) {                 /* a valid frame now and then must still parse */
            for (int j = 0; j < n; j++)
                if (bt_rx_byte(&r, b[j], (uint32_t)i + 100u, &f) && f.kind == BT_RX_SCAN)
                    whole++;
            i += 100;
        }
    }
    CHECK(whole > 150);
}

static void test_text(void)
{
    char t[64];

    bt_mac_text(MAC, t);
    CHECK(!strcmp(t, "A0:B1:C2:D3:E4:F5"));
    bt_frame_text((const uint8_t *)"CY0\x1e\x00", 5, t, sizeof t);
    CHECK(!strcmp(t, "CY0\\x1e\\x00"));
}

int main(void)
{
    test_encoder();
    test_forms();
    test_split();
    test_merged_and_garbage();
    test_lengths();
    test_timeouts();
    test_fuzz();
    test_text();
    printf("test_bt_proto: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
