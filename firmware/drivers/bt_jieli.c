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
#include "services/watchdog.h"

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
/* [ours], from the device log of 2026-10-18 (log/ECHOLOG_20261018_round3.txt): earphones in their case
 * refuse every try (CLER), and while music plays on the jack the run never ends, a try and its lines
 * every 30 s. After 12 tries (about 4 min) every 2 min; the tries' lines from then on every 5th. */
#define T_RETRY_SLOW_MS   120000u
#define RC_SLOW_AFTER     12u
#define T_LOST_MS         2000u         /* a lost link: the first try 2 s later */
/* [ours] Only CQO says linked. The device (2026-10-20, log/device_2026-10-20_round7_session in the
 * project): a CH06 echoed 20 s before the link came, then CHOK + CLOK as it came up, and the
 * earphones' keys arriving while we called it closed; on 2026-10-17 CT00 was answered CKKK + CLOK.
 * Unlinked, the module answers no CQ00 (no CQI/CQS/CQL in any device log); linked, it left at
 * most 1 in a row unanswered (6 times). So a frame that says the link changed without saying how
 * starts a check: CQ00 every second, nothing holding it back, until a CQO or 3 unanswered. */
#define T_VERIFY_FIRST_MS 300u          /* the check's first CQ00 */
#define T_VERIFY_POLL_MS  1000u         /* and the next ones */
#define VERIFY_MISSES     3u            /* unanswered in a row: no link */
#define T_STOP_MS         300u          /* a stop reaches the module once it lasted 300 ms: a USB host
                                         * flipped its alternate 4 times in 40 ms at a rate change */
#define T_PLAY_AFTER_CP_MS 1000u        /* the earphones' CP to our CY0 30 + C\x86, at least (870 ms worked) */
#define T_ACK_MS          200u          /* a frame's CKKK awaited at most (137 ms the slowest seen) */
#define T_PROBE_ECHO_MS   500u          /* the module's C?OK to a probe frame (110 ms on the device) */
#define T_DISC_MS         3000u         /* our CT00 unanswered for 3 s: unrouted anyway */
#define T_SERVED_MAX_MS   1000u         /* the idle time counts at most 1 s per tick: a USB disk
                                         * session that held the loop for 42 min idled it off at once */

/* ---- state ---------------------------------------------------------------------- */
static bt_state_t s_state;
static uint32_t s_t;                    /* time the current state (or step) began */
static uint32_t s_poll_t, s_hold, s_sub_t;
static int s_ver = -1, s_ui, s_playing = -1, s_routed, s_ch_sent, s_paired_wait;
static char s_probe_cmd;                /* the resume probe's last frame, its answer awaited */
static uint32_t s_probe_t;
static uint32_t s_play_due;             /* the deferred CY0 30 + C\x86 go out then (0: none) */
static uint32_t s_mod_cp;               /* [ours] the earphones' CP that stopped the module's stream
                                         * (0: none since their CC): the device, 2026-10-20, stayed
                                         * silent through the player's plays until the earphones'
                                         * own play (log/device_2026-10-20_ppsync_session 312.8 s) */
static int s_rc;                        /* reconnecting to s_peer until it links */
static unsigned s_rc_n;                 /* CL06 tries sent in this run */
static uint32_t s_rc_t;                 /* the last try */
static int s_user_off;                  /* the user's Disconnect: no reconnect until a connect or a boot */
static uint32_t s_disc_t;               /* our CT00 waiting for its answer (0: none) */
static int s_disc_check;                /* after the user's Disconnect: a CQO means the link stayed */
static int s_disc_cycled;               /* the module was switched off and on for that Disconnect */
static uint32_t s_vf_t, s_vf_poll;      /* the link in question since, the check's last CQ00 (0: none) */
static unsigned s_vf_n;                 /* the check's CQ00s unanswered */
static uint32_t s_idle, s_last;         /* unconnected time served (ms); the last bt_poll (0: none yet) */
static int s_want;                      /* the source plays (bt_set_playing), linked or not */
static uint32_t s_stop_t;               /* the source stopped then, not yet sent (0: none) */
static uint32_t s_q_t;                  /* our CQ00 while connected, unanswered (0: none) */
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

/* [ours] One frame at a time: the module answers each frame CKKK (52-137 ms later on the device)
 * and loses one that comes before that answer. The device, 2026-10-20: CY0 30 and C\x86 sent in
 * the same ms got one CKKK, the C\x86 was lost and the earphones stayed silent after their own
 * pause; the probe's C\x86 alone got its CKKK and brought the music back
 * (log/device_2026-10-20_resprobe_session 31.4 s, 136.3 s). A frame waits in s_txq until the
 * previous one's CKKK, or T_ACK_MS without it (a module that does not answer). */
#define TXQ 4
static struct { uint8_t b[BT_TX_MAX]; uint8_t n; } s_txq[TXQ];
static unsigned s_txq_head, s_txq_n;
static uint32_t s_ack_t;                /* our last frame, its CKKK not yet in (0: none awaited) */

static void txq_clear(void)
{
    s_txq_n = 0;
    s_ack_t = 0;
}

/* Polled TX, one byte per USR.TFNF (stock ram:03056de0), with a timeout stock lacks. */
static int tx_now(const uint8_t *p, int n)
{
    char t[3 * BT_TX_MAX + 8];

    s_ack_t = time_stamp(time_ms());
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

static int txq_open(uint32_t now)
{
    return !s_ack_t || now - s_ack_t >= T_ACK_MS;
}

/* The frames held back, one per answer (bt_poll, after the frames received). */
static void txq_drain(uint32_t now)
{
    if (s_txq_n && txq_open(now) && powered()) {
        unsigned i = s_txq_head;

        s_txq_head = (s_txq_head + 1u) % TXQ;
        s_txq_n--;
        (void)tx_now(s_txq[i].b, s_txq[i].n);
    }
}

static int tx(const uint8_t *p, int n)
{
    unsigned i;

    if (!powered())
        return -1;
    if (!s_txq_n && txq_open(time_ms()))
        return tx_now(p, n);
    if (s_txq_n == TXQ) {
        log_printf("bt: tx queue full: the oldest frame dropped");
        s_txq_head = (s_txq_head + 1u) % TXQ;
        s_txq_n--;
    }
    i = (s_txq_head + s_txq_n++) % TXQ;
    memcpy(s_txq[i].b, p, (size_t)n);
    s_txq[i].n = (uint8_t)n;
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
    txq_clear();
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
    if (s_rc_n >= RC_SLOW_AFTER)
        return T_RETRY_SLOW_MS;
    return s_rc_n == 2 ? T_RETRY_MS : s_rc_n == 3 ? 2u * T_RETRY_MS : T_RETRY_MAX_MS;
}

/* A reconnect try worth a line: the first ones, then every 5th of the slow ones */
static int rc_logged(void)
{
    return s_rc_n <= RC_SLOW_AFTER || s_rc_n % 5u == 0;
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
    s_vf_t = 0;
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
        s_vf_t = 0;
        s_disc_check = 0;
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
    s_disc_check = 0;
    s_vf_t = 0;
    go(BT_CONNECTING);
    return tx_mac('L', mac);            /* CL06+MAC: seg84 0307aace */
}

int bt_disconnect(void)
{
    if (s_state != BT_CONNECTED && s_state != BT_CONNECTING)
        return -1;
    s_user_off = 1;
    s_rc = 0;
    s_disc_cycled = 0;
    s_disc_t = time_stamp(time_ms());
    return tx_cmd('T');                 /* CT00: seg84 0307ba00 (event 0xDF) */
}

static void connected(const bt_frame_t *f);

/* ---- [ours] the link in question (T_VERIFY_*): asked, not guessed ---------------- *
 * why: the frame that raised it ('L' CLOK, 'H' a CH frame or CHOK, 'T' our CT00 answered or not,
 * 'K' an earphone key while unlinked, 'Q' a status poll unanswered while linked). */
static void verify(uint32_t now, char why)
{
    if (!s_vf_t)
        log_printf("bt: link? C%c", why);
    s_vf_t = time_stamp(now);
    s_vf_poll = time_stamp(now - T_VERIFY_POLL_MS + T_VERIFY_FIRST_MS);
    s_vf_n = 0;
}

/* The check's end. linked: a CQO came; else no link (3 unanswered). After the
 * user's Disconnect a CQO means the CT00 left the link up: the module off and on (as stock's
 * screen entry does, seg84:0307a13e) ends it, once; no reconnect follows (s_user_off). */
static void verify_end(int linked, const bt_frame_t *f)
{
    int disc = s_disc_check;

    s_vf_t = 0;
    s_disc_check = 0;
    log_printf("bt: link? %s", linked ? "CQO" : "none");
    if (disc && linked && !s_disc_cycled) {
        s_disc_cycled = 1;
        bt_power(0);
        bt_power(1);
    } else if (linked && !s_user_off) {
        connected(f);
    } else if (!linked && s_state == BT_CONNECTED) {
        link_down(1);
        go(BT_ON);
    }
}

/* Our CT00 answered or not in 3 s: unrouted at once. sure: a CTOK; otherwise (CLOK, as on
 * 2026-10-17, or nothing) the module is asked, and a CQO then means the link stayed. */
static void disc_done(uint32_t now, int sure)
{
    s_disc_t = 0;
    link_down(0);
    go(BT_ON);
    if (!sure) {
        verify(now, 'T');
        s_disc_check = 1;
    }
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
    s_vf_t = 0;
    if (s_state == BT_CONNECTED)
        return;
    s_rc = 0;
    s_disc_t = 0;
    s_paired_wait = 0;
    go(BT_CONNECTED);
    log_printf("bt: connected to '%s'", s_peer.name);
    route(1);
    s_playing = -1;                     /* bt_set_playing() sends CY0 for the current state */
    s_mod_cp = 0;
    s_play_due = 0;
    bt_event(BT_EV_CONNECTED, &s_peer);
}

/* CH... / CHOK (stock: connected = 1) and CLOK (stock: paired, connected = 0): the link changed,
 * but these do not say how (T_VERIFY_*). A connect waits for the CQO (stock's state 0). */
static void link_news(uint32_t now, char why)
{
    if (s_state == BT_CONNECTING || (s_state == BT_ON && s_rc)) {
        s_paired_wait = 1;
        s_t = now;
    }
    verify(now, why);
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
    if (f->kind == BT_RX_SKIP)
        s_ack_t = 0;                    /* the answer to our last frame: the next may go */
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
        if (f->cmd == 'L' && (s_disc_t || s_disc_check)) {
            disc_done(now, 0);          /* the device answers our CT00 so (2026-10-17, six times) */
        } else if (f->cmd == 'L') {     /* stock: paired, connected = 0 (08_bluetooth.md 2.3) */
            if (s_state == BT_CONNECTING || (s_state == BT_ON && s_rc)) {
                log_printf("bt: paired '%s'", s_peer.name);
                bt_event(BT_EV_PAIRED, &s_peer);
            }
            link_news(now, 'L');
        } else if (f->cmd == 'H') {     /* CHOK (stock: connected = 1) */
            if (s_state != BT_CONNECTED)
                link_news(now, 'H');
        } else if (f->cmd == 'T') {     /* CTOK (or the resident's CT): disconnected; the device
                                         * sends it unasked when the earphones go */
            if (s_disc_t || s_disc_check) {
                disc_done(now, 1);
            } else {
                link_down(1);
                go(BT_ON);
            }
        }
        break;
    case BT_RX_ERR:                     /* CLER / CHER */
        if (s_state == BT_ON && s_rc) {
            s_paired_wait = 0;
            s_vf_t = 0;
            if (rc_logged())
                log_printf("bt: reconnect refused (C%cER), try %u", f->cmd, s_rc_n);
        } else if (s_state == BT_CONNECTING) {
            log_printf("bt: connect failed (C%cER)", f->cmd);
            s_paired_wait = 0;
            s_vf_t = 0;
            go(BT_ON);
            bt_event(BT_EV_CONNECT_FAILED, &s_peer);
        }
        break;
    case BT_RX_LINKED:                  /* CH06 echoed, CH36+MAC+name (stock: connected = 1) */
        if (s_state != BT_CONNECTED)
            link_news(now, 'H');
        break;
    case BT_RX_STATUS:
        if (s_vf_t) {
            if (f->state == 'O')        /* (CQS/CQL/CQI count as unanswered: a link may be coming) */
                verify_end(1, f);
        } else if (f->state == 'O' && !s_user_off)
            connected(f);               /* (after the user's Disconnect a link is not taken) */
        else if (f->state != 'O' && s_state == BT_CONNECTED) {  /* the resident: connected = 0 */
            link_down(1);
            go(BT_ON);
        }
        break;
    case BT_RX_KEY:
        if (s_state != BT_CONNECTED && !s_user_off)
            verify(now, 'K');
        if (s_probe_cmd && f->cmd == s_probe_cmd && now - s_probe_t < T_PROBE_ECHO_MS) {
            s_probe_cmd = 0;            /* [ours] the module's answer to the resume probe's frame
                                         * (CD00 -> CDOK, CP00 -> CPER: the device 2026-10-20), not a key */
            bt_frame_text(f->raw, f->raw_len, t, sizeof t);
            log_printf("bt: %s: the answer to the probe's C%c00", t, f->cmd);
            break;
        }
        if (f->cmd == 'P' || f->cmd == 'C') {
            s_hold = T_HOLD_POLLS;      /* (outside the screen) the next 10 polls skipped */
            s_mod_cp = f->cmd == 'P' ? time_stamp(now) : 0;   /* the stream stopped / running */
            s_play_due = 0;
        }
        /* (stock seg84 0307b736 sends CY0+30 again on a CC while playing: here that CC pauses,
         * startup/ap_bt.c, so the pause's CY0 0 is the only frame) */
        bt_keys_event((uint8_t)f->cmd, now);
        break;
    default:
        break;
    }
}

/* [ours] Developer > BT resume probe (test), in the dev overlay: one frame of stock's short form
 * 'C' cmd '0' v 0x00 (ram:0306763c builds CY0 so), some of them frames stock never sends: CC00
 * (the earphones' play frame, which stock only receives). (The BT volume probe before it sent
 * CY0 1..29 and CD00/CU00: CY0 sets a level, CD/CU are answered CDOK/CUOK and change nothing,
 * the device 2026-10-20.) Only while linked. */
int bt_probe_send(char cmd, uint8_t v)
{
    uint8_t b[5] = { 'C', (uint8_t)cmd, '0', v, 0 };

    if (s_state != BT_CONNECTED)
        return -1;
    s_probe_cmd = cmd;
    s_probe_t = time_ms();
    return tx(b, 5);
}

/* [ours] What the framer threw away, with the bytes around it: a frame of a form nobody has seen
 * (the earphones' volume, 2026-10-20: none of the known forms came for a swipe) is in the log so,
 * not lost. At most 64 lines a boot (a line with garbage replies would flood it). */
static uint8_t s_raw[16];               /* the last bytes received, a ring */
static unsigned s_raw_n;
static uint32_t s_drop_seen;
static unsigned s_drop_lines;

static void raw_dropped(void)
{
    uint8_t b[sizeof s_raw];
    char t[3 * sizeof s_raw + 8];
    unsigned n = s_raw_n < sizeof s_raw ? s_raw_n : sizeof s_raw;

    if (s_rx.dropped == s_drop_seen)
        return;
    s_drop_seen = s_rx.dropped;
    if (s_drop_lines >= 64u)
        return;
    s_drop_lines++;
    for (unsigned i = 0; i < n; i++)
        b[i] = s_raw[(s_raw_n - n + i) % sizeof s_raw];
    bt_frame_text(b, (int)n, t, sizeof t);
    log_printf("bt: rx dropped (%u in all), the last bytes: %s", (unsigned)s_rx.dropped, t);
}

/* ---- the tick ------------------------------------------------------------------- */
/* The player plays into a stream the earphones' CP stopped: the level, then a C\x86 (bt_set_playing). */
static void play_restart(void)
{
    uint8_t b[BT_TX_MAX];

    s_mod_cp = 0;
    tx(b, bt_enc_level(b, 0x1Eu));
    if (s_ver == 5)
        tx_cmd((char)0x86);
}

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
        s_raw[s_raw_n++ % sizeof s_raw] = b;
        if (bt_rx_byte(&s_rx, b, now, &f))
            frame(&f, now);
    }
    if (bt_rx_idle(&s_rx, now, &f))
        frame(&f, now);
    raw_dropped();
    if (s_play_due && (int32_t)(now - s_play_due) >= 0) {
        s_play_due = 0;
        if (s_state == BT_CONNECTED && s_playing == 1 && s_mod_cp)
            play_restart();
    }
    txq_drain(now);

    if (s_disc_t && now - s_disc_t >= T_DISC_MS) {
        s_disc_t = 0;
        if (s_state == BT_CONNECTED || s_state == BT_CONNECTING) {
            log_printf("bt: CT00 unanswered");
            disc_done(now, 0);
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
        }
        break;
    default:
        break;
    }

    if ((s_state == BT_ON || s_state == BT_CONNECTING) && s_paired_wait && now - s_t >= T_PAIRED_MS) {
        log_printf("bt: paired but no link in 30 s");
        s_paired_wait = 0;
        go(BT_ON);
    }

    /* reconnect: stock's auto-reconnect (ram:03067692: CL06 + the remembered MAC twice, 3 s
     * apart), then [ours] 10, 20 and every 30 s, never while a CLOK waits for its link */
    if (s_state == BT_ON && s_rc && !s_vf_t && now - s_rc_t >= rc_gap() && !(s_rc_n >= 2 && s_paired_wait)) {
        s_rc_n++;
        s_rc_t = now;
        if (s_rc_n > 2 && rc_logged())
            log_printf("bt: reconnect try %u%s", s_rc_n, s_rc_n == RC_SLOW_AFTER ? " (every 2 min from now)" : "");
        tx_mac('L', s_peer.mac);
    }

    /* [ours] the link in question: CQ00 every second, whatever holds the polls below back */
    if (s_vf_t && (s_state == BT_ON || s_state == BT_CONNECTING || s_state == BT_CONNECTED)) {
        if (now - s_vf_poll >= T_VERIFY_POLL_MS) {
            if (s_vf_n >= VERIFY_MISSES) {
                verify_end(0, 0);
            } else {
                s_vf_n++;
                s_vf_poll = time_stamp(now);
                s_poll_t = now;         /* (the regular poll waits its own period from here) */
                tx_cmd('Q');
            }
        }
        return;
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
            if (s_q_t) {                /* the last one unanswered: the link in question */
                verify(now, 'Q');
                return;
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

/* [ours] Developer > BT lab (test): while on, the lab's frames are the only play/pause frames the
 * module gets (bt_set_playing sends none, the earphones' keys move no player: startup/ap_bt.c).
 * Off: s_playing = -1, so the next bt_set_playing sends the player's state again. */
static int s_lab;

void bt_lab(int on)
{
    s_lab = on;
    s_play_due = 0;
    if (!on)
        s_playing = -1;
}

int bt_lab_on(void) { return s_lab; }

/* One frame 'C' cmd '0' v 0x00 at once, past the one-at-a-time queue (a lab burst), or queued. */
int bt_lab_send(char cmd, uint8_t v, int burst)
{
    uint8_t b[5] = { 'C', (uint8_t)cmd, '0', v, 0 };

    if (s_state != BT_CONNECTED)
        return -1;
    return burst ? tx_now(b, 5) : tx(b, 5);
}

void bt_set_playing(int playing, int debounce)
{
    uint8_t b[BT_TX_MAX];

    playing = !!playing;
    s_want = playing;
    if (s_state != BT_CONNECTED) {
        s_playing = -1;
        s_stop_t = 0;
        return;
    }
    if (s_lab) {
        s_playing = playing;
        s_stop_t = 0;
        return;
    }
    if (playing == s_playing) {
        s_stop_t = 0;
        return;
    }
    if (!playing && s_playing == 1 && debounce) {   /* [ours] a stop once it lasted T_STOP_MS
                                         * (stock sends one frame per action; a USB host flips
                                         * within ms); the player's pause at once (bt_jieli.h) */
        uint32_t now = time_ms();

        if (!s_stop_t) {
            s_stop_t = time_stamp(now);
            return;
        }
        if (now - s_stop_t < T_STOP_MS)
            return;
    }
    s_stop_t = 0;
    /* [ours] The module's stream is the one latch the earphones see: their CP stops it, their CC
     * restarts it, and so does a C\x86 or C\x87 that reaches it (stock's status frames, seg27
     * 0307de7e..0307e102, sent on every play and pause). Ours were lost while they went out in
     * the same ms as the CY0 (one CKKK for two frames); once delivered (4288fa4) they stopped the
     * running stream at a track's start and flipped it at each pause (log/device_2026-10-20_
     * txgate_session 90.96 s, 92.7 s). So the player never touches the stream but for one case:
     * it plays while the earphones' CP has the stream stopped. Then the level and a C\x86 go out
     * together, CY0 30 first, and not before T_PLAY_AFTER_CP_MS after their CP: the module, still
     * stopping, ignores what comes sooner. The device: CY0 30 + C\x86 870, 960, 1290 and 2060 ms
     * after the CP brought the music back (onelatch session); a CY0 30 10 ms and a C\x86 120 ms
     * after it, silent 4 times; a CY0 30 10 ms and a C\x86 1000 ms after it, silent until the
     * level was sent again (c86delay session 202.1 s, the probe's step 3). Version 5 only for
     * the C\x86 (stock's gate ram:0306765c). bt_poll sends a deferred one. */
    if (playing && s_mod_cp) {
        uint32_t now = time_ms();

        if (now - s_mod_cp < T_PLAY_AFTER_CP_MS) {
            s_play_due = time_stamp(s_mod_cp + T_PLAY_AFTER_CP_MS);
            log_printf("bt: playing after the earphones' pause (CP at %lu ms): CY0 30 and C\\x86 at %lu ms",
                       (unsigned long)s_mod_cp, (unsigned long)s_play_due);
            s_playing = playing;
            return;
        }
        log_printf("bt: playing after the earphones' pause (CP at %lu ms): CY0 30 and C\\x86",
                   (unsigned long)s_mod_cp);
        play_restart();
        s_playing = playing;
        return;
    }
    s_play_due = 0;                     /* (paused again before it: the stream stays stopped) */
    tx(b, bt_enc_level(b, playing ? 0x1Eu : 0u));   /* CY0+30 / CY0+0 */
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

/* ---- [ours] listen only: Developer > BT listen (test) (bt_jieli.h) ---------------- *
 * Nothing goes to the module: THR is never written, no break, GPIO2.16 (our TX, the module's
 * RX) stays idle-high on the UART while the module is powered, and GPIO2.7 is switched only as
 * stock switches it (off, pins low, 100 ms, UART open, on: seg84:0307a13e; off: every off site).
 * QEMU's module (-machine jieli=listen=1) fails a run that breaks any of that. */
#define GPIO2_EXT  REG32(0x50030050u)   /* EXT_PORTA: the pin levels (board_rknanoc.c reads GPIO2.9 so) */
#define U_LSR      0x14u                /* bit 1 OE, 2 PE, 3 FE, 4 BI */
#define LISTEN_OFF_MS 500u              /* off between the two power-ups (stock's rescan: 500 ms) */

/* Off, pins low, 100 ms, the UART open (TX idle-high), then on; uart = 0: RX a GPIO input. */
static void listen_power_on(int uart)
{
    module_off();
    delay_ms(T_POWER_OFF_MS);
    uart_open();
    uart_close();                       /* polled: no interrupt (stamps here, not in the ISR) */
    if (!uart) {
        iomux2(PIN_RX, 0);
        GPIO2_DDR &= ~(1u << PIN_RX);
    }
    pin_out(PIN_POWER, 1);              /* listen: powered, nothing sent */
}

static int listen_over(uint32_t t0, uint32_t ms, uint32_t *fed)
{
    uint32_t now = time_ms();

    if (now - *fed >= 100u) {
        watchdog_feed();
        *fed = now;
    }
    return now - t0 >= ms;
}

int bt_listen(bt_listen_t *r, uint32_t *buf, unsigned words, uint32_t ms)
{
    unsigned nb = words / 4u, ne = words - 2u * nb, n = 0, spin = 0;
    uint32_t t0, fed, c0, last;

    memset(r, 0, sizeof *r);
    if (s_state != BT_OFF && s_state != BT_NO_MODULE)
        bt_power(0);                    /* (sends nothing either) */
    s_used = 1;
    r->core_hz = clock_core_hz();
    r->bytes = buf;
    r->edges = buf + 2u * nb;
    log_printf("btlisten: start, %u ms per power-up, nothing sent", (unsigned)ms);

    /* 1: the UART at 115200 8N1, as stock opens it; every byte with its time and line status */
    listen_power_on(1);
    t0 = fed = time_ms();
    c0 = time_cycles();
    while (!listen_over(t0, ms, &fed)) {
        while (U(U_USR) & USR_RFNE) {
            uint32_t lsr = U(U_LSR) & 0x1Eu, b = U(U_RBR) & 0xFFu;

            if (lsr)
                r->lsr_errs++;
            if (n < nb) {
                buf[2u * n] = (time_cycles() - c0) / (r->core_hz / 1000000u);
                buf[2u * n + 1u] = lsr << 8 | b;
                n++;
            } else {
                r->full |= 1u;
            }
        }
    }
    r->nbytes = n;
    module_off();
    delay_ms(LISTEN_OFF_MS);

    /* 2: GPIO2.17 as an input, every edge in core cycles: any baud rate */
    listen_power_on(0);
    t0 = fed = time_ms();
    c0 = time_cycles();
    last = GPIO2_EXT & (1u << PIN_RX);
    r->level0 = last != 0;
    n = 0;
    for (;;) {
        uint32_t v = GPIO2_EXT & (1u << PIN_RX);

        if (v != last) {
            last = v;
            if (n < ne)
                r->edges[n++] = time_cycles() - c0;
            else
                r->full |= 2u;
            continue;
        }
        if (!(++spin & 0xFFu) && listen_over(t0, ms, &fed))
            break;
    }
    r->nedges = n;
    module_off();
    s_state = BT_OFF;
    log_printf("btlisten: done: %u bytes (%u line errors), %u edges, the module off",
               (unsigned)r->nbytes, (unsigned)r->lsr_errs, (unsigned)r->nedges);
    return 0;
}
