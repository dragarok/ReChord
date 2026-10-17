/*
 * bt_jieli.c -- the Bluetooth module on UART1 (bt_jieli.h). Every hardware
 * value cites stock MINIV390 (work/stock_boot/08_bluetooth.md) or says
 * [inferred].
 */
#include <stdio.h>
#include <string.h>

#include "bt_jieli.h"
#include "clock.h"
#include "services/bt_keys.h"
#include "services/log.h"

#define REG32(a) (*(volatile uint32_t *)(a))

/* ---- pins (GPIO2 at 0x50030000: DR +0, DDR +4; GRF iomux 0x50010040 + (pin >> 3) * 4,
 * two bits per pin, hi-word write mask: stock ram:03052d70) ---------------------------- */
#define GPIO2_DR   REG32(0x50030000u)
#define GPIO2_DDR  REG32(0x50030004u)
#define PIN_POWER  7u                   /* GPIO2.7: 1 = module on */
#define PIN_TX     16u                  /* GPIO2.16: UART1 TX (func1) */
#define PIN_RX     17u                  /* GPIO2.17: UART1 RX (func1) */
#define PIN_BT     0u                   /* GPIO2.0: 1 = analog path to the module [inferred meaning] */
#define PIN_JACK   14u                  /* GPIO2.14: 1 = jack output path [inferred meaning] */

static void iomux2(unsigned pin, unsigned func)
{
    REG32(0x50010040u + (pin >> 3) * 4u) = (0x30000u | func) << ((pin & 7u) * 2u);
}

/* stock's sequence at every GPIO write site: iomux(port, pin, 0), direction out, level */
static void pin_out(unsigned pin, int level)
{
    iomux2(pin, 0);
    GPIO2_DDR |= 1u << pin;
    if (level)
        GPIO2_DR |= 1u << pin;
    else
        GPIO2_DR &= ~(1u << pin);
}

/* ---- UART1 (DesignWare APB UART, 0x400B0000) ------------------------------------ */
#define U(off)     REG32(0x400B0000u + (off))
#define U_RBR      0x00u
#define U_THR      0x00u
#define U_DLL      0x00u
#define U_DLH      0x04u
#define U_IER      0x04u
#define U_IIR      0x08u
#define U_LCR      0x0Cu
#define U_MCR      0x10u
#define U_USR      0x7Cu                /* bit 1 TFNF, bit 3 RFNE (stock 03056de0 / 03056d78) */
#define U_SRR      0x88u
#define U_SFE      0x98u
#define U_SRT      0x9Cu
#define U_STET     0xA0u
#define U_DMASA    0xA8u
#define USR_TFNF   (1u << 1)
#define USR_RFNE   (1u << 3)
#define UART1_IRQ  25u                  /* vector 0x29 - 16 (ram:03067fe0(0x29, ...)) */
#define CRU(off)   REG32(0x40180000u + (off))
#define GRF(off)   REG32(0x50010000u + (off))
#define NVIC_ISER0 REG32(0xE000E100u)
#define NVIC_ICER0 REG32(0xE000E180u)

/* RX: the interrupt drains the FIFO into a ring; bt_poll() frames it. */
#define RING 512u
static volatile uint8_t s_ring[RING];
static volatile uint32_t s_head, s_tail, s_overrun;

void UART1_IRQHandler(void)
{
    (void)U(U_IIR);                     /* (stock 0305180c: IIR & 0xF, 4 data / 0xC timeout) */
    while (U(U_USR) & USR_RFNE) {
        uint8_t b = (uint8_t)U(U_RBR);
        uint32_t h = s_head;

        if (h - s_tail < RING) {
            s_ring[h % RING] = b;
            s_head = h + 1u;
        } else {
            s_overrun++;
        }
    }
}

/* Stock's UART1 open: ram:030538da -> uart_init(3, 115200, 8, 0, 2) ram:03056f16 -> 03051894. */
static void uart_open(void)
{
    GRF(0xBCu) = 0x02000000u;           /* ram:03052f4e(0): GRF +0xBC bit 9 = 0 (UART1 pin group [inferred]) */
    iomux2(PIN_TX, 1);                  /* ram:03056f6e/03056f78: GPIO2.16, 2.17 func1 */
    iomux2(PIN_RX, 1);
    CRU(0xC0u) = 0x01000100u;           /* ram:030570a8: SOFTRST0 bit 8 (UART1) asserted, 1 ms, released */
    delay_ms(1);
    CRU(0xC0u) = 0x01000000u;
    CRU(0x84u) = 0x20000000u;           /* ram:030570be: CLKGATE1 bit 13 open (gate 29) */
    CRU(0xA0u) = 0x02000000u;           /* ram:030570c6: CLKGATE8 bit 9 open (gate 0x89) */
    CRU(0x38u) = 0x00C00040u;           /* ram:03052ae0 (0x030529f0(1, 1, 24 MHz)): CLKSEL9 source */
    CRU(0x38u) = 0x003F0000u;           /*   and divider 1: the UART clock is 24 MHz */
    U(U_SRR) = 7u;                      /* ram:03051894..: reset UART and FIFOs */
    U(U_IER) = 0;
    U(U_DMASA) = 1u;
    U(U_MCR) = 0;                       /* no RTS/CTS */
    U(U_SFE) = 1u;                      /* FIFOs on */
    U(U_SRT) = 0;                       /* RX trigger: 1 character */
    U(U_STET) = 0;
    U(U_LCR) = 0x03u;                   /* ram:03056cc8(1, 8, 2, 0): 8 data bits, no parity, 1 stop */
    U(U_LCR) |= 0x80u;
    U(U_DLL) = 1500000u / 115200u;      /* 13 (0x16E360 / 115200): 115385 baud from 24 MHz */
    U(U_DLH) = 0;
    U(U_LCR) &= ~0x80u;
    U(U_SRR) = 6u;
    s_tail = s_head;
    NVIC_ISER0 = 1u << UART1_IRQ;       /* ram:03057756(0x29) */
    U(U_IER) |= 1u;                     /* ram:03056d5a(1, 1): RX data interrupt */
}

static void uart_close(void)
{
    U(U_IER) = 0;                       /* (stock leaves the UART as is and only drops the pins) */
    NVIC_ICER0 = 1u << UART1_IRQ;
}

/* ---- timing ---------------------------------------------------------------------- *
 * Stock times the BT code on its 10 ms tick 0x0301D120 (not milliseconds: ram:03067c02 turns
 * it into seconds with / 100, and in QEMU it counts 1000 per 10 virtual s; the screen's CQ00
 * poll, "/ 300", comes every 3.0 s). The constants below are stock's ticks x 10 ms. */
#define T_POWER_OFF_MS    100u          /* seg84:0307a13e Delay(100): off before power-on (a ms delay loop, ram:03065f50) */
#define T_HELLO_MS        5000u         /* 5000 x Delay(1) for CV1x */
#define T_POLL_UI_MS      3000u         /* seg84 0307b394: tick / 300 */
#define T_POLL_MS         5000u         /* ram:030677ae: tick / 500 */
#define T_HOLD_POLLS      10u           /* ram:030677c4: after CP/CC, 10 of those polls skipped */
#define T_RECONNECT_MS    3000u         /* ram:030676ca: > 300 ticks between CL06 tries, 2 tries */
#define T_CH06_MS         4000u         /* seg84 0307b288: 400 ticks into state 2 */
#define T_STATE_MS        30000u        /* seg84 0307b31e/0307b338: 3000 ticks in state 1 (CP00) / 2 (CT00);
                                         * QEMU: stock's CP00 came 30.000 s after its CS00 */
#define T_PAIRED_MS       30000u        /* [inferred]: CLOK and no CQO, give up waiting (stock: state 0 at CLOK) */
#define T_IDLE_OFF_MS     300000u       /* ram:03067cda: > 30000 ticks unconnected outside the screen */
/* [ours], from the device log of 2026-10-17 (log/ECHOLOG_20261017_bt.txt in the project): */
#define T_RETRY_MS        10000u        /* after stock's two tries: 10 s, 20 s, then every 30 s. A CLER
                                         * there came 5-8 s after a CL06 (earphones in their case) */
#define T_RETRY_MAX_MS    30000u
#define T_LOST_MS         2000u         /* a lost link: the first try 2 s later */
#define T_CL_ECHO_MS      5000u         /* a CLOK this soon after our CL06 answers it, even linked */
#define T_STOP_MS         300u          /* a stop reaches the module once it lasted 300 ms: a USB host
                                         * flipped its alternate 4 times in 40 ms at a rate change */
#define T_DISC_MS         3000u         /* our CT00 unanswered for 3 s: unrouted anyway */
#define T_SERVED_MAX_MS   1000u         /* the idle time counts at most 1 s per tick: a USB disk
                                         * session that held the loop for 42 min idled it off at once */

/* ---- state ---------------------------------------------------------------------- */
static bt_state_t s_state;
static uint32_t s_t;                    /* time the current state (or step) began */
static uint32_t s_poll_t, s_hold, s_sub_t;
static int s_ver = -1, s_ui, s_playing = -1, s_routed, s_ch_sent, s_paired_wait;
static int s_avrcp;                     /* stock 0x0301D8AC: 0 none, 1 paused, 2 playing */
static int s_rc;                        /* reconnecting to s_peer until it links */
static unsigned s_rc_n;                 /* CL06 tries sent in this run */
static uint32_t s_rc_t;                 /* the last try */
static uint32_t s_cl_t;                 /* our last CL06 (0: none) */
static int s_user_off;                  /* the user's Disconnect: no reconnect until a connect or a boot */
static uint32_t s_disc_t;               /* our CT00 waiting for its answer (0: none) */
static uint32_t s_idle, s_last;         /* unconnected time served (ms); the last bt_poll (0: none yet) */
static int s_want;                      /* the source plays (bt_set_playing), linked or not */
static uint32_t s_stop_t;               /* the source stopped then, not yet sent (0: none) */
static uint32_t s_q_t;                  /* our CQ00 while connected, unanswered (0: none) */
static unsigned s_q_miss;               /* such polls in a row (measured: the device's answer set is open) */
static uint32_t s_seq = 1, s_tx_fail;
static bt_dev_t s_list[BT_SCAN_MAX], s_peer;
static int s_n;
static int s_used;                      /* the module was powered this boot */
static bt_rx_t s_rx;

__attribute__((weak)) void bt_event(int ev, const bt_dev_t *dev) { (void)ev; (void)dev; }
__attribute__((weak)) int bt_jack_in(void) { return 0; }

static void changed(void) { s_seq++; }

static void go(bt_state_t st)
{
    if (st != s_state) {
        s_state = st;
        s_t = time_ms();
        changed();
    }
}

static int powered(void)
{
    return s_state != BT_OFF && s_state != BT_NO_MODULE &&
           (s_state != BT_STARTING || (GPIO2_DR & (1u << PIN_POWER)));
}

/* Polled TX, one byte per USR.TFNF (stock ram:03056de0), with a timeout stock lacks. */
static int tx(const uint8_t *p, int n)
{
    char t[3 * BT_TX_MAX + 8];

    if (!powered())
        return -1;
    for (int i = 0; i < n; i++) {
        uint32_t t0 = time_ms();

        while (!(U(U_USR) & USR_TFNF)) {
            if (time_ms() - t0 > 5u) {
                s_tx_fail++;
                log_printf("bt: tx timeout (USR %02x)", (unsigned)U(U_USR));
                return -2;
            }
        }
        U(U_THR) = p[i];
    }
    if (!(n == 5 && p[1] == 'Q')) {     /* (the 3 s / 5 s status poll is not logged) */
        bt_frame_text(p, n, t, sizeof t);
        log_printf("bt: tx %s", t);
    }
    return 0;
}

static int tx_cmd(char c)
{
    uint8_t b[BT_TX_MAX];

    return tx(b, bt_enc_cmd(b, c));
}

static int tx_mac(char c, const uint8_t *mac)
{
    uint8_t b[BT_TX_MAX];

    return tx(b, bt_enc_mac(b, c, mac));
}

static void route(int bt)
{
    bt = bt && !bt_jack_in();
    if (bt == s_routed)
        return;
    s_routed = bt;
    pin_out(PIN_BT, bt);                /* ram:03067aa6 / 03067b0e */
    pin_out(PIN_JACK, !bt);             /* ram:03067ac4 / 03067b2c */
    log_printf("bt: audio to the %s", bt ? "module" : "jack");
    changed();
}

/* Off: GPIO2.7 = 0 and the UART pins GPIO outputs at 0 (seg84:0307a0d6 and every off site). */
static void module_off(void)
{
    uart_close();
    pin_out(PIN_POWER, 0);
    pin_out(PIN_RX, 0);
    pin_out(PIN_TX, 0);
}

/* Stock Main step 1 also sets GPIO2.14 = 1 and GPIO2.0 = 0 here (ram:03062ace, ram:03062aec).
 * Those two are the audio path, and the DAC driver drives them at its power-on and unmute
 * (drivers/dac_cs43131.c, as the build proven on the device does): left to it, so a boot with
 * Bluetooth off drives exactly what that build drives, plus the module's three pins low. */
void bt_early_pins(void)
{
    pin_out(PIN_POWER, 0);              /* ram:03062a74 */
    pin_out(PIN_RX, 0);                 /* ram:03062a92 */
    pin_out(PIN_TX, 0);                 /* ram:03062ab0 */
}

/* Only after the module was powered this boot: otherwise its pins are still bt_early_pins'
 * and the audio pins the DAC driver's, as without Bluetooth. */
void bt_board_off(void)
{
    if (!s_used)
        return;
    uart_close();
    pin_out(PIN_BT, 1);                 /* ram:0306745c..030674fc, in stock's order */
    pin_out(PIN_POWER, 0);
    pin_out(PIN_TX, 0);
    pin_out(PIN_RX, 0);
    pin_out(PIN_JACK, 1);
    s_state = BT_OFF;
}

static int peer_set(void)
{
    for (int i = 0; i < BT_MAC_LEN; i++)
        if (s_peer.mac[i])
            return 1;
    return 0;
}

/* The wait before the next CL06 of a reconnect run, by the tries already sent. */
static uint32_t rc_gap(void)
{
    if (s_rc_n < 2)
        return T_RECONNECT_MS;          /* stock's two tries, 3 s apart (ram:03067692) */
    return s_rc_n == 2 ? T_RETRY_MS : s_rc_n == 3 ? 2u * T_RETRY_MS : T_RETRY_MAX_MS;
}

/* Reconnect to s_peer, the first try (first_ms) after the module is up; why = NULL: not logged. */
static void rc_start(const char *why, uint32_t first_ms)
{
    if (s_rc || s_user_off || !peer_set())
        return;
    s_rc = 1;
    s_rc_n = 0;
    s_rc_t = time_ms() - T_RECONNECT_MS + first_ms;
    if (why)
        log_printf("bt: reconnecting to '%s' (%s)", s_peer.name, why);
}

/* lost = 1: the link went without our asking (reconnected unless the user disconnected). */
static void link_down(int lost)
{
    int was = s_state == BT_CONNECTED;

    route(0);
    s_paired_wait = 0;
    s_disc_t = 0;
    s_q_t = 0;
    if (was) {
        log_printf("bt: disconnected");
        bt_event(BT_EV_DISCONNECTED, &s_peer);
        if (lost)
            rc_start("link lost", T_LOST_MS);
    }
}

int bt_power(int on)
{
    if (on) {
        if (s_state != BT_OFF && s_state != BT_NO_MODULE)
            return 0;
        module_off();                   /* seg84:0307a13e: off, pins low, 100 ms */
        s_used = 1;
        bt_rx_init(&s_rx);
        s_ver = -1;
        s_n = 0;
        s_idle = 0;
        s_last = 0;
        go(BT_STARTING);
        log_printf("bt: power on");
        return 0;
    }
    if (s_state == BT_OFF)
        return 0;
    link_down(0);
    module_off();
    s_rc = 0;
    go(BT_OFF);
    log_printf("bt: power off");
    return 0;
}

void bt_autostart(const uint8_t mac[6], const char *name)
{
    bt_power(1);
    s_user_off = 0;
    if (mac) {
        memcpy(s_peer.mac, mac, BT_MAC_LEN);
        snprintf(s_peer.name, sizeof s_peer.name, "%s", name ? name : "");
        s_rc = 0;
        rc_start(0, 0);                 /* ram:03067692: counter 1 -> 2 tries, 3 s apart, then ours */
    }
}

void bt_wake(const uint8_t mac[6], const char *name)
{
    if (s_user_off || !mac || s_state == BT_CONNECTED || s_state == BT_CONNECTING ||
        s_state == BT_SCANNING || s_state == BT_NO_MODULE || (s_state == BT_ON && s_rc))
        return;
    if (s_state == BT_OFF) {
        log_printf("bt: the source plays: the module on again");
        bt_power(1);
    }
    if (!s_rc) {
        memcpy(s_peer.mac, mac, BT_MAC_LEN);
        snprintf(s_peer.name, sizeof s_peer.name, "%s", name ? name : "");
    }
    rc_start(s_state == BT_STARTING ? 0 : "the source plays", 0);
}

int bt_scan(void)
{
    if (s_state == BT_OFF || s_state == BT_NO_MODULE)
        bt_power(1);
    if (s_state == BT_STARTING) {
        s_sub_t = 1;                    /* scan once the module is up */
        return 0;
    }
    if (s_state == BT_CONNECTED || s_state == BT_CONNECTING)
        return -1;
    s_n = 0;
    s_rc = 0;
    go(BT_SCANNING);
    return tx_cmd('S');                 /* CS00: seg84 0307a2a6 / 0307aeee */
}

int bt_connect(const uint8_t mac[6], const char *name)
{
    if (s_state != BT_ON && s_state != BT_SCANNING)
        return -1;
    /* (stock sends no CP00 first: OK on a list entry goes straight to state 2, seg84 0307aace) */
    memcpy(s_peer.mac, mac, BT_MAC_LEN);
    snprintf(s_peer.name, sizeof s_peer.name, "%s", name ? name : "");
    s_ch_sent = 0;
    s_paired_wait = 0;
    s_rc = 0;
    s_user_off = 0;
    go(BT_CONNECTING);
    s_cl_t = time_stamp(time_ms());
    return tx_mac('L', mac);            /* CL06+MAC: seg84 0307aace */
}

int bt_disconnect(void)
{
    if (s_state != BT_CONNECTED && s_state != BT_CONNECTING)
        return -1;
    s_user_off = 1;
    s_rc = 0;
    s_disc_t = time_stamp(time_ms());
    return tx_cmd('T');                 /* CT00: seg84 0307ba00 (event 0xDF) */
}

/* ---- frames from the module ------------------------------------------------------ */
static void add_scan(const bt_frame_t *f)
{
    char m[18];

    for (int i = 0; i < s_n; i++)
        if (!memcmp(s_list[i].mac, f->mac, BT_MAC_LEN))
            return;                     /* (stock dedupes on the MAC's byte sum; we on the MAC) */
    if (s_n >= BT_SCAN_MAX)
        return;
    memcpy(s_list[s_n].mac, f->mac, BT_MAC_LEN);
    memcpy(s_list[s_n].name, f->name, sizeof s_list[s_n].name);
    s_n++;
    bt_mac_text(f->mac, m);
    log_printf("bt: found %s '%s'", m, f->name);
    changed();
}

static void connected(const bt_frame_t *f)
{
    if (f && f->kind == BT_RX_STATUS && f->raw_len >= 11u) {
        memcpy(s_peer.mac, f->mac, BT_MAC_LEN);      /* (stock: copies MAC/name on a change) */
        if (f->name[0])
            memcpy(s_peer.name, f->name, sizeof s_peer.name);
    }
    s_q_t = 0;
    if (s_q_miss) {
        log_printf("bt: CQ00 answered again after %u unanswered", s_q_miss);
        s_q_miss = 0;
    }
    if (s_state == BT_CONNECTED)
        return;
    s_rc = 0;
    s_user_off = 0;
    s_disc_t = 0;
    s_paired_wait = 0;
    go(BT_CONNECTED);
    log_printf("bt: connected to '%s'", s_peer.name);
    route(1);
    s_playing = -1;                     /* bt_set_playing() sends CY0 for the current state */
    s_avrcp = 0;
    bt_event(BT_EV_CONNECTED, &s_peer);
}

static void frame(const bt_frame_t *f, uint32_t now)
{
    char t[3 * BT_FRAME_MAX + 8];

    static char s_last_q;               /* (the status poll's answer is logged when it changes) */

    if (f->kind != BT_RX_STATUS || f->state != s_last_q) {
        bt_frame_text(f->raw, f->raw_len, t, sizeof t);
        log_printf("bt: rx %s", t);
    }
    if (f->kind == BT_RX_STATUS)
        s_last_q = f->state;
    switch (f->kind) {
    case BT_RX_HELLO:
        if (s_state == BT_STARTING) {
            s_ver = f->ver;             /* seg84 0307a098: '5' -> version 5, else 0 */
            if (s_ver != 5)
                s_ver = 0;
            log_printf("bt: module up (CV1%c), version %d", '0' + f->ver, s_ver);
            go(BT_ON);
            s_poll_t = now;
            if (s_sub_t) {
                s_sub_t = 0;
                bt_scan();
            }
        }
        break;
    case BT_RX_SCAN:
        add_scan(f);
        break;
    case BT_RX_SCAN_END:
    case BT_RX_SCAN_ERR:
        if (s_state == BT_SCANNING && f->kind == BT_RX_SCAN_END)
            go(BT_ON);                  /* CSEN: state 0. (CSER: ignored by stock) */
        break;
    case BT_RX_OK:
        if (f->cmd == 'L' && s_state == BT_CONNECTED && !(s_cl_t && now - s_cl_t < T_CL_ECHO_MS)) {
            /* stock's screen parser: CLOK -> connected = 0 (08_bluetooth.md 2.3); the device
             * answers our CT00 with CLOK, never CTOK (2026-10-17, six times) */
            log_printf("bt: CLOK: the link closed");
            link_down(1);
            go(BT_ON);
        } else if (f->cmd == 'L' && (s_state == BT_CONNECTING || s_rc)) {
            log_printf("bt: paired '%s'", s_peer.name);    /* connected comes with CQO */
            bt_event(BT_EV_PAIRED, &s_peer);
            s_paired_wait = 1;
            s_t = now;
        } else if (f->cmd == 'H') {     /* CHOK: connected */
            connected(0);
        } else if (f->cmd == 'T') {     /* CTOK (or the resident's CT): disconnected; the device
                                         * sends it unasked when the earphones go */
            link_down(1);
            go(BT_ON);
        }
        break;
    case BT_RX_ERR:                     /* CLER / CHER */
        if (s_state == BT_ON && s_rc) {
            s_paired_wait = 0;
            log_printf("bt: reconnect refused (C%cER), try %u", f->cmd, s_rc_n);
        } else if (s_state == BT_CONNECTING) {
            log_printf("bt: connect failed (C%cER)", f->cmd);
            go(BT_ON);
            bt_event(BT_EV_CONNECT_FAILED, &s_peer);
        }
        break;
    case BT_RX_LINKED:
        connected(0);
        break;
    case BT_RX_STATUS:
        if (f->state == 'O') {
            connected(f);
        } else {
            if (s_state == BT_CONNECTED) {      /* CQS/CQL/CQI: the resident sets connected = 0 */
                link_down(1);
                go(BT_ON);
            }
        }
        break;
    case BT_RX_KEY:
        if (f->cmd == 'P' || f->cmd == 'C') {
            s_hold = T_HOLD_POLLS;      /* (outside the screen) the next 10 polls skipped */
            s_avrcp = f->cmd == 'P' ? 1 : 2;   /* seg84 0307b67c: CP -> 1, CC -> 2 */
        }
        if (f->cmd == 'C' && s_state == BT_CONNECTED && s_playing == 1) {
            uint8_t b[BT_TX_MAX];       /* seg84 0307b736: CC while not paused -> 0x1D5: CY0+30 again */

            tx(b, bt_enc_level(b, 0x1Eu));
        }
        bt_keys_event((uint8_t)f->cmd, now);
        break;
    default:
        break;
    }
}

/* ---- the tick ------------------------------------------------------------------- */
void bt_poll(void)
{
    uint32_t now = time_ms();
    uint32_t served = s_last ? now - s_last : 0;
    bt_frame_t f;

    s_last = now;
    if (served > T_SERVED_MAX_MS) {     /* the loop was held (a USB disk session): not idle time */
        if (s_state != BT_OFF && s_state != BT_NO_MODULE)
            log_printf("bt: not served for %u ms", (unsigned)served);
        served = T_SERVED_MAX_MS;
    }

    /* frames */
    while (s_tail != s_head) {
        uint8_t b = s_ring[s_tail % RING];

        s_tail++;
        if (bt_rx_byte(&s_rx, b, now, &f))
            frame(&f, now);
    }
    if (bt_rx_idle(&s_rx, now, &f))
        frame(&f, now);

    if (s_disc_t && now - s_disc_t >= T_DISC_MS) {
        s_disc_t = 0;
        if (s_state == BT_CONNECTED || s_state == BT_CONNECTING) {
            log_printf("bt: no answer to CT00 in 3 s: disconnected anyway");
            link_down(0);
            go(BT_ON);
        }
    }

    switch (s_state) {
    case BT_STARTING:
        if (!(GPIO2_DR & (1u << PIN_POWER))) {
            if (now - s_t >= T_POWER_OFF_MS) {   /* seg84:0307a13e: off 100 ms, UART open, power on */
                uart_open();
                pin_out(PIN_POWER, 1);
                s_t = now;
            }
        } else if (now - s_t >= T_HELLO_MS) {
            log_printf("bt: no CV1x in 5 s: no module");
            module_off();
            go(BT_NO_MODULE);
            bt_event(BT_EV_NO_MODULE, 0);
        }
        return;
    case BT_OFF:
    case BT_NO_MODULE:
        return;
    case BT_SCANNING:
        if (now - s_t >= T_STATE_MS) {  /* seg84 0307b31e: 3000 ticks -> CP00, state 0 */
            tx_cmd('P');
            go(BT_ON);
        }
        break;
    case BT_CONNECTING:
        if (!s_paired_wait && !s_ch_sent && now - s_t >= T_CH06_MS) {
            s_ch_sent = 1;              /* seg84 0307b288: CH06 + the MAC 400 ticks into state 2 */
            tx_mac('H', s_peer.mac);
        }
        if (!s_paired_wait && now - s_t >= T_STATE_MS) {
            tx_cmd('T');                /* seg84 0307b338: 3000 ticks in state 2 -> CT00 */
            log_printf("bt: connect timed out");
            go(BT_ON);
            bt_event(BT_EV_CONNECT_FAILED, &s_peer);
        } else if (s_paired_wait && now - s_t >= T_PAIRED_MS) {
            log_printf("bt: paired but no link in 30 s");
            s_paired_wait = 0;
            go(BT_ON);
        }
        break;
    default:
        break;
    }

    if (s_state == BT_ON && s_paired_wait && now - s_t >= T_PAIRED_MS) {
        log_printf("bt: paired but no link in 30 s");
        s_paired_wait = 0;
    }

    /* reconnect: stock's auto-reconnect (ram:03067692: CL06 + the remembered MAC twice, 3 s
     * apart), then [ours] 10, 20 and every 30 s, never while a CLOK waits for its link */
    if (s_state == BT_ON && s_rc && now - s_rc_t >= rc_gap() && !(s_rc_n >= 2 && s_paired_wait)) {
        s_rc_n++;
        s_rc_t = now;
        if (s_rc_n > 2)
            log_printf("bt: reconnect try %u", s_rc_n);
        s_cl_t = time_stamp(now);
        tx_mac('L', s_peer.mac);
    }

    /* status poll: CQ00 every 3 s in the screen (seg84 0307b394), else every 5 s (resident
     * ram:030677ae), which skips 10 polls after a CP/CC key and polls not while reconnecting */
    if (now - s_poll_t >= (s_ui ? T_POLL_UI_MS : T_POLL_MS)) {
        int q = 0;

        s_poll_t = now;
        if (s_ui)
            q = 1;
        else if (s_hold)
            s_hold--;
        else if (!(s_rc && s_rc_n < 2))
            q = 1;
        if (q && s_state == BT_CONNECTED) {
            if (s_q_t) {                /* the last one unanswered: logged, not acted on */
                s_q_miss++;
                if (s_q_miss == 1 || s_q_miss == 3 || s_q_miss % 60 == 10)
                    log_printf("bt: CQ00 unanswered while connected (%u in a row)", s_q_miss);
            }
            s_q_t = time_stamp(now);
        }
        if (q)
            tx_cmd('Q');
    }

    /* idle off (ram:03067a0c): outside the screen, not connected, 5 min since the last
     * connected or in-screen time. [ours] Only time served counts, and not while the source
     * plays and a reconnect runs (the earphones may come back) */
    if (s_state == BT_CONNECTED || s_ui || (s_want && s_rc))
        s_idle = 0;
    else if ((s_idle += served) > T_IDLE_OFF_MS) {
        log_printf("bt: 5 min unconnected: power off");
        bt_power(0);
        bt_event(BT_EV_IDLE_OFF, 0);
    }
}

void bt_set_playing(int playing)
{
    uint8_t b[BT_TX_MAX];

    playing = !!playing;
    s_want = playing;
    if (s_state != BT_CONNECTED) {
        s_playing = -1;
        s_stop_t = 0;
        return;
    }
    if (playing == s_playing) {
        s_stop_t = 0;
        return;
    }
    if (!playing && s_playing == 1) {   /* [ours] a stop once it lasted T_STOP_MS (stock sends one
                                         * frame per action; a USB host flips within ms) */
        uint32_t now = time_ms();

        if (!s_stop_t) {
            s_stop_t = time_stamp(now);
            return;
        }
        if (now - s_stop_t < T_STOP_MS)
            return;
    }
    s_stop_t = 0;
    tx(b, bt_enc_level(b, playing ? 0x1Eu : 0u));   /* CY0+30 / CY0+0 */
    /* the AVRCP-like status (seg27 0307de7e..0307e102, state 0x0301D8AC): a play/pause here (not
     * the connect itself) tells the module when its state differs: resume while it is < 2 ->
     * C\x86 and 2; pause or the end while it is 0 or 2 -> C\x87 and 1. CP/CC from the module set
     * it to 1/2 (so its own keys are not echoed). Only to a version-5 module (gate ram:0306765c). */
    if (s_playing >= 0) {
        if (playing && s_avrcp < 2) {
            if (s_ver == 5)
                tx_cmd((char)0x86);
            s_avrcp = 2;
        } else if (!playing && s_avrcp != 1) {
            if (s_ver == 5)
                tx_cmd((char)0x87);
            s_avrcp = 1;
        }
    }
    s_playing = playing;
}

void bt_ui_active(int on)
{
    if (s_ui && !on)
        s_idle = 0;                     /* (the idle time runs from leaving the screen) */
    s_ui = on;
}

bt_state_t bt_state(void) { return s_state; }
int bt_version(void) { return s_ver; }
int bt_scan_count(void) { return s_n; }
const bt_dev_t *bt_scan_dev(int i) { return i >= 0 && i < s_n ? &s_list[i] : 0; }
const bt_dev_t *bt_peer(void) { return &s_peer; }
uint32_t bt_seq(void) { return s_seq; }
int bt_routed(void) { return s_routed; }

const char *bt_state_name(bt_state_t s)
{
    static const char *const k[] = { "off", "starting", "no module", "on", "scanning", "connecting",
                                     "connected" };

    return (unsigned)s < sizeof k / sizeof k[0] ? k[s] : "?";
}

void bt_stats(uint32_t *rx_frames, uint32_t *rx_dropped, uint32_t *tx_fail)
{
    *rx_frames = s_rx.frames;
    *rx_dropped = s_rx.dropped + s_overrun;
    *tx_fail = s_tx_fail;
}
