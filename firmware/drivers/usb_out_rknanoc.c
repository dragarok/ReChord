/*
 * usb_out_rknanoc.c -- USB audio out (usb_out.h): the DWC2 core as host, slave mode.
 *
 * Channels: 0 control (EP0), 1 and 2 the isochronous OUT packets (alternating, so each has a
 * whole packet interval to finish before it is used again), 3 the feedback endpoint (IN).
 * Everything that touches the channels runs in the USB interrupt: SOF builds the next
 * (micro)frame's packet; channel halts advance control transfers; the RX FIFO is drained
 * there alone. The main loop only waits for control transfers to finish.
 *
 * Synopsys DWC2 host programming model (slave): HCCHARn/HCTSIZn/HCINTn at 0x500 + 0x20 n,
 * OUT data written to FIFO n (0x1000 (n + 1)) after the channel is enabled, IN data popped
 * from GRXSTSP (PktSts 2 data, 3 transfer complete), periodic packets go out in the
 * (micro)frame whose parity HCCHAR.OddFrm names. Host role from the SDK's HostConnect()
 * (GRF UOC_CON1 IDDIG override, UOC_CON2 D+/D- pull-downs).
 */
#include <stdint.h>
#include <string.h>

#include "board.h"
#include "clock.h"
#include "rknanoc.h"
#include "usb_msc.h"
#include "usb_out.h"
#include "audio/audio_shared.h"
#include "services/log.h"

#define OTG(off)     (*(volatile uint32_t *)(0x60080000u + (off)))
#define HCCHAR(n)    OTG(0x500u + (n) * 0x20u)
#define HCINT(n)     OTG(0x508u + (n) * 0x20u)
#define HCINTMSK(n)  OTG(0x50Cu + (n) * 0x20u)
#define HCTSIZ(n)    OTG(0x510u + (n) * 0x20u)
#define FIFO(n)      (*(volatile uint32_t *)(0x60081000u + (n) * 0x1000u))
#define NVIC_ISER0   (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICER0   (*(volatile uint32_t *)0xE000E180u)
#define USB_IRQ      5u

enum {
    GAHBCFG = 0x08, GUSBCFG = 0x0C, GRSTCTL = 0x10, GINTSTS = 0x14, GINTMSK = 0x18, GRXSTSP = 0x20,
    GRXFSIZ = 0x24, GNPTXFSIZ = 0x28, GHWCFG3 = 0x4C, HPTXFSIZ = 0x100, HCFG = 0x400, HFNUM = 0x408,
    HAINT = 0x414, HAINTMSK = 0x418, HPRT = 0x440, PCGCCTL = 0xE00,
};

#define GI_SOF      (1u << 3)
#define GI_RXFLVL   (1u << 4)
#define GI_PRTINT   (1u << 24)
#define GI_HCHINT   (1u << 25)
#define GI_DISCONN  (1u << 29)

#define HP_CONN     (1u << 0)
#define HP_CONNDET  (1u << 1)
#define HP_ENA      (1u << 2)
#define HP_ENCHNG   (1u << 3)
#define HP_OCCHNG   (1u << 5)
#define HP_RST      (1u << 8)
#define HP_PWR      (1u << 12)
#define HP_W1C      (HP_CONNDET | HP_ENA | HP_ENCHNG | HP_OCCHNG)   /* (ENA: writing 1 disables) */

#define HC_XFERCOMPL (1u << 0)
#define HC_CHHLTD    (1u << 1)
#define HC_STALL     (1u << 3)
#define HC_NAK       (1u << 4)
#define HC_XACTERR   (1u << 7)
#define HC_BBLERR    (1u << 8)
#define HC_FRMOVRUN  (1u << 9)
#define HC_DTERR     (1u << 10)

#define CC_ENA  (1u << 31)
#define CC_DIS  (1u << 30)
#define CC_IN   (1u << 15)
#define CC_ISO  (1u << 18)
#define CC_MC1  (1u << 20)

#define PID_DATA0 0u
#define PID_DATA1 2u
#define PID_SETUP 3u

#define CH_CTRL 0u
#define CH_FB   3u
#define LLI_DONE (1u << 12)

/* ---- what the device is (from its descriptors) ---------------------------------- */

typedef struct {
    uint8_t ifn, alt, subslot, bits, ch, ep, sync, fb_ep, interval, fb_interval, freq_ctl, terminal;
    uint16_t mps, fb_mps;
    usb_out_range_t r[8];             /* UAC1: its rates */
    uint8_t nr;
} alt_t;

#define MAX_ALTS 8
#define MAX_ENT  8

static struct {
    usb_out_status_t st;
    int supply, started;
    uint8_t addr, mps0, cfg_value, ac_if;
    alt_t alts[MAX_ALTS];
    uint8_t nalts;
    uint8_t it_id[MAX_ENT], it_clk[MAX_ENT], nit;          /* input terminals (USB streaming) */
    uint8_t fu_id[MAX_ENT], fu_src[MAX_ENT], fu_vol[MAX_ENT], nfu;   /* fu_vol: 1 master, 2 per channel */
    uint8_t cx_id[MAX_ENT], cx_src[MAX_ENT], ncx;          /* UAC2 clock selectors/multipliers: a source */
    uint8_t fu, fu_chan, clock;       /* the feature unit (and its channel: 0 master, 1 L+R) and clock */
    int16_t vol_min, vol_max, vol_res;
    uint32_t conn_ms;
    volatile int disconnected;
    /* control transfer on channel 0 */
    struct {
        uint8_t setup[8];
        uint8_t *buf;
        uint16_t len, got;
        uint8_t stage, retries;
        volatile int done;            /* 0 running, 1 done, < 0 failed */
    } c;
    /* the stream */
    const alt_t *a;                   /* the alternate on, or NULL */
    volatile int iso_on;              /* SOF builds packets */
    volatile int consuming, paused, ended;
    uint32_t iv, fb_iv;               /* packet / feedback intervals in (micro)frames */
    uint32_t per_pkt, nominal;        /* frames per packet, Q16.16: what is sent, nominal */
    uint32_t acc, maxf, bpf;
    volatile int fb_busy;
    uint32_t li, fo, bf;              /* the ring: LLI index, frame in its block, block frames */
    const int32_t *src;
    int32_t gain;                     /* Q15, software volume (no feature unit) */
} s;

static uint8_t s_cfg[1024];
static uint8_t s_pkt[1024];

const usb_out_status_t *usb_out_status(void)
{
    return &s.st;
}

int usb_out_active(void)
{
    return s.started;
}

/* ---- control transfers (channel 0), driven from the interrupt ---------------------- */

static void fifo_write(unsigned ch, const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 4u) {
        uint32_t v = 0;

        for (uint32_t b = 0; b < 4u && i + b < n; b++)
            v |= (uint32_t)p[i + b] << (8u * b);
        FIFO(ch) = v;
    }
}

enum { ST_SETUP = 1, ST_DATA_IN, ST_DATA_OUT, ST_STATUS_IN, ST_STATUS_OUT };

static void ch0_go(void)
{
    uint32_t chr = s.mps0 | (uint32_t)s.addr << 22, n = s.c.len, mps = s.mps0, pc;

    switch (s.c.stage) {
    case ST_SETUP:
        HCTSIZ(CH_CTRL) = 8u | 1u << 19 | PID_SETUP << 29;
        HCCHAR(CH_CTRL) = chr | CC_ENA;
        fifo_write(CH_CTRL, s.c.setup, 8);
        break;
    case ST_DATA_IN:
        pc = (n + mps - 1u) / mps;
        HCTSIZ(CH_CTRL) = pc * mps | pc << 19 | PID_DATA1 << 29;
        HCCHAR(CH_CTRL) = chr | CC_IN | CC_ENA;
        break;
    case ST_DATA_OUT:
        pc = (n + mps - 1u) / mps;
        HCTSIZ(CH_CTRL) = n | (pc ? pc : 1u) << 19 | PID_DATA1 << 29;
        HCCHAR(CH_CTRL) = chr | CC_ENA;
        fifo_write(CH_CTRL, s.c.buf, n);
        break;
    case ST_STATUS_IN:
        HCTSIZ(CH_CTRL) = 1u << 19 | PID_DATA1 << 29;
        HCCHAR(CH_CTRL) = chr | CC_IN | CC_ENA;
        break;
    case ST_STATUS_OUT:
        HCTSIZ(CH_CTRL) = 1u << 19 | PID_DATA1 << 29;
        HCCHAR(CH_CTRL) = chr | CC_ENA;
        break;
    }
}

static void ch0_halted(uint32_t hc)
{
    if (!s.c.stage || s.c.done)
        return;
    if (hc & HC_STALL) {
        s.c.done = -2;
    } else if (hc & HC_XFERCOMPL) {
        s.c.retries = 0;
        switch (s.c.stage) {
        case ST_SETUP:
            s.c.stage = !s.c.len ? ST_STATUS_IN : (s.c.setup[0] & 0x80u) ? ST_DATA_IN : ST_DATA_OUT;
            break;
        case ST_DATA_IN:  s.c.stage = ST_STATUS_OUT; break;
        case ST_DATA_OUT: s.c.stage = ST_STATUS_IN; break;
        default:          s.c.done = 1; return;
        }
        ch0_go();
    } else if (++s.c.retries <= 3u || (hc & HC_NAK)) {   /* errors: three tries; NAK: until the timeout */
        ch0_go();
    } else {
        s.c.done = -1;
    }
}

/* One request; returns the data stage's bytes, or < 0 (-1 error, -2 stall, -3 timeout). */
static int ctrl(uint8_t bm, uint8_t br, uint16_t val, uint16_t idx, uint16_t len, void *buf)
{
    uint8_t *p = s.c.setup;
    uint32_t t0;

    p[0] = bm; p[1] = br; p[2] = (uint8_t)val; p[3] = (uint8_t)(val >> 8);
    p[4] = (uint8_t)idx; p[5] = (uint8_t)(idx >> 8); p[6] = (uint8_t)len; p[7] = (uint8_t)(len >> 8);
    s.c.buf = buf;
    s.c.len = len;
    s.c.got = 0;
    s.c.retries = 0;
    s.c.done = 0;
    NVIC_ICER0 = 1u << USB_IRQ;
    s.c.stage = ST_SETUP;
    ch0_go();
    NVIC_ISER0 = 1u << USB_IRQ;
    for (t0 = time_ms(); !s.c.done && time_ms() - t0 < 500u; )
        cpu_wait_tick_or(&s.c.done);
    NVIC_ICER0 = 1u << USB_IRQ;
    if (!s.c.done) {
        s.c.done = -3;
        if (HCCHAR(CH_CTRL) & CC_ENA)
            HCCHAR(CH_CTRL) |= CC_DIS;
    }
    s.c.stage = 0;
    s.c.buf = NULL;                                  /* (nothing late lands in the caller's buffer) */
    NVIC_ISER0 = 1u << USB_IRQ;
    if (s.c.done < 0) {
        log_printf("usbout: request %02x %02x %04x %04x %u: %s", bm, br, val, idx, len,
                   s.c.done == -2 ? "stall" : s.c.done == -3 ? "timeout" : "error");
        return s.c.done;
    }
    return (bm & 0x80u) ? s.c.got : len;
}

/* ---- the ring and the packets --------------------------------------------------- */

static void block_load(void)
{
    volatile uint32_t *l = (volatile uint32_t *)AS_LLI + s.li * 7u;

    s.src = (const int32_t *)l[0];
    s.bf = (l[4] & 0xFFFu) / 2u;
    s.fo = 0;
}

/* The block played: DONE into its LLI (core B recycles it), then the next one, if any. */
static void block_next(void)
{
    volatile uint32_t *l = (volatile uint32_t *)AS_LLI + s.li * 7u;
    uint32_t nxt = l[2];

    l[4] |= LLI_DONE;
    if (!nxt) {
        s.ended = 1;
        return;
    }
    s.li = (nxt - AS_LLI) / 28u;
    block_load();
}

static void put_frame(uint8_t *d, int32_t l, int32_t r)
{
    if (s.gain != 32768) {
        l = (int32_t)(((int64_t)l * s.gain) >> 15);
        r = (int32_t)(((int64_t)r * s.gain) >> 15);
    }
    switch (s.a->subslot) {
    case 4:
        memcpy(d, &l, 4);
        memcpy(d + 4, &r, 4);
        break;
    case 3:
        d[0] = (uint8_t)(l >> 8); d[1] = (uint8_t)(l >> 16); d[2] = (uint8_t)(l >> 24);
        d[3] = (uint8_t)(r >> 8); d[4] = (uint8_t)(r >> 16); d[5] = (uint8_t)(r >> 24);
        break;
    default:
        d[0] = (uint8_t)(l >> 16); d[1] = (uint8_t)(l >> 24);
        d[2] = (uint8_t)(r >> 16); d[3] = (uint8_t)(r >> 24);
        break;
    }
}

/* n frames into s_pkt: the ring's next frames, or silence (paused, stopped, ended). */
static void take(uint32_t n)
{
    uint8_t *d = s_pkt;

    for (uint32_t i = 0; i < n; i++, d += s.bpf) {
        if (s.consuming && !s.paused && !s.ended && s.fo >= s.bf)
            block_next();
        if (!s.consuming || s.paused || s.ended) {
            memset(d, 0, s.bpf);
            continue;
        }
        put_frame(d, s.src[2u * s.fo], s.src[2u * s.fo + 1u]);
        s.fo++;
    }
}

static void send_packet(uint32_t next)
{
    unsigned ch = 1u + ((next / s.iv) & 1u);
    uint32_t n, bytes;

    if (HCCHAR(ch) & CC_ENA) {                       /* its last packet is still out: too late */
        s.st.late++;
        return;
    }
    s.acc += s.per_pkt;
    n = s.acc >> 16;
    s.acc &= 0xFFFFu;
    if (n > s.maxf)
        n = s.maxf;
    take(n);
    bytes = n * s.bpf;
    HCTSIZ(ch) = bytes | 1u << 19 | PID_DATA0 << 29;
    HCCHAR(ch) = s.a->mps | (uint32_t)s.a->ep << 11 | CC_ISO | CC_MC1 | (uint32_t)s.addr << 22 |
                 (next & 1u) << 29 | CC_ENA;
    fifo_write(ch, s_pkt, bytes);
}

static void arm_feedback(uint32_t next)
{
    HCTSIZ(CH_FB) = s.a->fb_mps | 1u << 19 | PID_DATA0 << 29;
    HCCHAR(CH_FB) = s.a->fb_mps | (uint32_t)(s.a->fb_ep & 15u) << 11 | CC_IN | CC_ISO | CC_MC1 |
                    (uint32_t)s.addr << 22 | (next & 1u) << 29 | CC_ENA;
    s.fb_busy = 1;
}

/* A feedback value (n bytes) -> frames per OUT packet, Q16.16; kept if within 1/8 of nominal. */
static void feedback(uint32_t v, uint32_t n)
{
    uint32_t per;

    if (s.st.high_speed) {
        per = v * s.iv;                              /* Q16.16 per microframe */
    } else {
        uint32_t q1414 = (v & 0xFFFFFFu) << 2, q1616 = v;
        uint32_t nom = s.nominal / s.iv;             /* per frame */

        if (n == 4u && q1616 > nom - nom / 8u && q1616 < nom + nom / 8u)
            per = q1616 * s.iv;                      /* 4 bytes at full speed: Q16.16 (some devices) */
        else
            per = q1414 * s.iv;                      /* Q10.14 per frame */
    }
    if (per > s.nominal - s.nominal / 8u && per < s.nominal + s.nominal / 8u) {
        s.per_pkt = per;
        s.st.fb_q16 = per;
    }
}

static void sof(void)
{
    uint32_t next = (OTG(HFNUM) + 1u) & 0x3FFFu;

    if (!s.iso_on)
        return;
    if (next % s.iv == 0u)
        send_packet(next);
    if (s.a->fb_ep && !s.fb_busy && next % s.fb_iv == 0u)
        arm_feedback(next);
}

static void rx_one(void)
{
    uint32_t st = OTG(GRXSTSP), ch = st & 15u, cnt = (st >> 4) & 0x7FFu, sts = (st >> 17) & 15u;
    uint32_t words = (cnt + 3u) / 4u;

    if (sts != 2u)
        return;
    for (uint32_t i = 0; i < words; i++) {
        uint32_t v = FIFO(0);

        if (ch == CH_CTRL) {
            for (uint32_t b = 0; b < 4u && 4u * i + b < cnt; b++)
                if (s.c.buf && s.c.got < s.c.len)
                    s.c.buf[s.c.got++] = (uint8_t)(v >> (8u * b));
        } else if (ch == CH_FB && i == 0u && s.a) {
            feedback(v, cnt);
        }
    }
}

static void host_irq(void)
{
    uint32_t gi = OTG(GINTSTS) & OTG(GINTMSK);

    while (OTG(GINTSTS) & GI_RXFLVL)
        rx_one();
    if (gi & GI_HCHINT) {
        uint32_t haint = OTG(HAINT);

        for (unsigned ch = 0; ch < 4u; ch++) {
            uint32_t hc;

            if (!(haint & (1u << ch)))
                continue;
            hc = HCINT(ch);
            HCINT(ch) = hc;
            if (ch == CH_CTRL) {
                ch0_halted(hc);
            } else if (ch == CH_FB) {
                s.fb_busy = 0;
            } else {
                if (hc & HC_XFERCOMPL)
                    s.st.packets++;
                if (hc & (HC_FRMOVRUN | HC_XACTERR))
                    s.st.late++;
            }
        }
    }
    if (gi & GI_PRTINT) {
        uint32_t p = OTG(HPRT);

        OTG(HPRT) = (p & ~HP_W1C) | (p & (HP_CONNDET | HP_ENCHNG | HP_OCCHNG));
        if ((p & HP_ENCHNG) && !(p & HP_ENA) && s.st.state == USB_OUT_READY)
            s.disconnected = 1;
    }
    if (gi & GI_DISCONN) {
        OTG(GINTSTS) = GI_DISCONN;
        s.disconnected = 1;
        s.iso_on = 0;
    }
    if (gi & GI_SOF) {
        OTG(GINTSTS) = GI_SOF;
        sof();
    }
}

/* ---- descriptors ------------------------------------------------------------------ */

static uint32_t rd24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }

/* The configuration: the AudioControl entities, and every AudioStreaming alternate that
 * plays (an isochronous OUT endpoint fed from a USB-streaming input terminal) stereo PCM. */
static void parse(uint32_t len)
{
    const uint8_t *d = s_cfg;
    int cls = 0, sub = 0, ifn = -1, alt = 0, uac2 = 0;
    alt_t *a = NULL;

    s.nalts = s.nit = s.nfu = s.ncx = 0;
    s.clock = s.fu = 0;
    s.cfg_value = d[5];
    for (uint32_t i = 0; i + 2u <= len && d[i] >= 2u; i += d[i]) {
        const uint8_t *x = d + i;

        if (x[1] == 4u && x[0] >= 9u) {               /* interface */
            ifn = x[2];
            alt = x[3];
            cls = x[5];
            sub = x[6];
            a = NULL;
            if (cls == 1 && sub == 1) {
                s.ac_if = (uint8_t)ifn;
                uac2 = x[7] == 0x20u;
                s.st.uac = uac2 ? 2 : 1;
            } else if (cls == 1 && sub == 2 && alt > 0 && s.nalts < MAX_ALTS) {
                a = &s.alts[s.nalts];
                memset(a, 0, sizeof *a);
                a->ifn = (uint8_t)ifn;
                a->alt = (uint8_t)alt;
                a->terminal = 0xFF;
            }
        } else if (x[1] == 0x24u && cls == 1 && sub == 1) {   /* AudioControl entities */
            if (x[2] == 2u && rd16(x + 4) == 0x0101u && s.nit < MAX_ENT) {    /* input terminal, USB streaming */
                s.it_id[s.nit] = x[3];
                s.it_clk[s.nit++] = uac2 ? x[7] : 0;
            } else if (x[2] == 6u && s.nfu < MAX_ENT) {                       /* feature unit */
                uint32_t n = uac2 ? 4u : x[5], c0 = uac2 ? 5u : 6u;
                uint32_t m0 = 0, m1 = 0;

                for (uint32_t b = 0; b < n && c0 + b < x[0]; b++) {
                    m0 |= (uint32_t)x[c0 + b] << (8u * b);
                    if (c0 + n + b < x[0])
                        m1 |= (uint32_t)x[c0 + n + b] << (8u * b);
                }
                s.fu_id[s.nfu] = x[3];
                s.fu_src[s.nfu] = x[4];
                s.fu_vol[s.nfu++] = uac2 ? ((m0 >> 2 & 3u) == 3u ? 1 : (m1 >> 2 & 3u) == 3u ? 2 : 0)
                                         : ((m0 & 2u) ? 1 : (m1 & 2u) ? 2 : 0);
            } else if (uac2 && (x[2] == 0x0Bu || x[2] == 0x0Cu) && s.ncx < MAX_ENT) {   /* clock selector, multiplier */
                s.cx_id[s.ncx] = x[3];
                s.cx_src[s.ncx++] = x[2] == 0x0Bu ? x[5] : x[4];
            } else if (uac2 && x[2] == 0x0Au && !s.clock) {
                s.clock = x[3];                       /* (the first; replaced below by the terminal's) */
            }
        } else if (x[1] == 0x24u && a) {              /* AudioStreaming, this alternate */
            if (x[2] == 1u) {                         /* AS_GENERAL */
                a->terminal = x[3];
                if (uac2) {
                    a->ch = x[10];
                    if (!(x[6] & 1u))
                        a->terminal = 0xFF;           /* not PCM */
                } else if (rd16(x + 5) != 1u) {
                    a->terminal = 0xFF;
                }
            } else if (x[2] == 2u) {                  /* FORMAT_TYPE */
                if (uac2) {
                    a->subslot = x[4];
                    a->bits = x[5];
                } else {
                    a->ch = x[4];
                    a->subslot = x[5];
                    a->bits = x[6];
                    if (x[7] == 0u) {                 /* continuous */
                        a->r[0].min = rd24(x + 8);
                        a->r[0].max = rd24(x + 11);
                        a->nr = 1;
                    } else {
                        for (uint32_t k = 0; k < x[7] && k < 8u; k++) {
                            a->r[k].min = a->r[k].max = rd24(x + 8 + 3 * k);
                            a->nr = (uint8_t)(k + 1u);
                        }
                    }
                }
            }
        } else if (x[1] == 0x25u && a && x[2] == 1u && !uac2) {
            a->freq_ctl = x[3] & 1u;                  /* UAC1: sampling frequency control */
        } else if (x[1] == 5u && a && (x[3] & 3u) == 1u) {   /* isochronous endpoint */
            if (!(x[2] & 0x80u)) {
                a->ep = x[2];
                a->sync = (x[3] >> 2) & 3u;
                a->mps = (uint16_t)(rd16(x + 4) & 0x7FFu);
                a->interval = x[6];
                if (!uac2 && x[0] >= 9u && x[8])
                    a->fb_ep = x[8];
            } else if (((x[3] >> 4) & 3u) == 1u || (!uac2 && a->fb_ep == x[2])) {   /* feedback */
                a->fb_ep = x[2];
                a->fb_mps = (uint16_t)(rd16(x + 4) & 0x7FFu);
                a->fb_interval = !uac2 && x[0] >= 9u ? (uint8_t)(x[7] + 1u) : x[6];   /* (UAC1: bRefresh) */
            } else {
                a->terminal = 0xFF;                   /* data IN: a microphone */
            }
        }
        if (a && i + d[i] + 2u <= len && d[i + d[i] + 1] == 4u) {   /* this alternate ends here */
            if (a->ep && a->terminal != 0xFF && a->ch == 2u && a->subslot >= 2u && a->subslot <= 4u)
                s.nalts++;
            a = NULL;
        }
    }
    if (a && a->ep && a->terminal != 0xFF && a->ch == 2u && a->subslot >= 2u && a->subslot <= 4u)
        s.nalts++;
}

/* The feature unit and clock for the playback terminal. */
static void entities(void)
{
    uint8_t term = s.nalts ? s.alts[0].terminal : 0;

    s.fu = 0;
    s.fu_chan = 0;
    for (uint32_t i = 0; i < s.nfu; i++) {
        if (s.fu_src[i] == term && s.fu_vol[i]) {
            s.fu = s.fu_id[i];
            s.fu_chan = s.fu_vol[i] == 2u;
        }
    }
    for (uint32_t i = 0; !s.fu && i < s.nfu; i++) {
        if (s.fu_vol[i]) {
            s.fu = s.fu_id[i];
            s.fu_chan = s.fu_vol[i] == 2u;
        }
    }
    for (uint32_t i = 0; i < s.nit; i++) {
        if (s.it_id[i] == term && s.it_clk[i]) {
            uint8_t c = s.it_clk[i];

            for (uint32_t hop = 0; hop < 4u; hop++) {   /* through selectors / multipliers */
                uint32_t k;

                for (k = 0; k < s.ncx && s.cx_id[k] != c; k++)
                    ;
                if (k == s.ncx)
                    break;
                c = s.cx_src[k];
            }
            s.clock = c;
        }
    }
}

static int rate_in(const usb_out_range_t *r, uint32_t n, uint32_t rate)
{
    for (uint32_t i = 0; i < n; i++) {
        if (rate < r[i].min || rate > r[i].max)
            continue;
        if (rate == r[i].min || rate == r[i].max || !r[i].res || (rate - r[i].min) % r[i].res == 0u)
            return 1;
    }
    return 0;
}

static uint32_t interval_of(const alt_t *a, uint8_t binterval)
{
    if (!binterval)
        binterval = 1;
    return s.st.high_speed ? 1u << (binterval - 1u > 3u ? 3u : binterval - 1u)
                           : 1u << (binterval - 1u > 4u ? 4u : binterval - 1u);
}

/* ---- enumeration ---------------------------------------------------------------- */

static int get_desc(uint8_t type, uint8_t index, uint16_t lang, void *buf, uint16_t len)
{
    return ctrl(0x80, 6, (uint16_t)(type << 8 | index), lang, len, buf);
}

static int enumerate(void)
{
    uint8_t dd[18], b[100];
    int n;
    uint32_t t0;

    OTG(HPRT) = (OTG(HPRT) & ~HP_W1C) | HP_RST;      /* port reset, 50 ms */
    delay_ms(50);
    OTG(HPRT) = OTG(HPRT) & ~(HP_W1C | HP_RST);
    for (t0 = time_ms(); !(OTG(HPRT) & HP_ENA) && time_ms() - t0 < 100u; )
        cpu_wait_tick();
    if (!(OTG(HPRT) & HP_ENA)) {
        log_printf("usbout: port did not enable after reset (HPRT %08x)", (unsigned)OTG(HPRT));
        return -1;
    }
    s.st.high_speed = ((OTG(HPRT) >> 17) & 3u) == 0u;
    if (((OTG(HPRT) >> 17) & 3u) == 2u) {
        log_printf("usbout: a low-speed device: not audio");
        return -1;
    }
    delay_ms(20);
    s.addr = 0;
    s.mps0 = 64;
    if (get_desc(1, 0, 0, dd, 8) < 8)
        return -1;
    s.mps0 = dd[7] ? dd[7] : 8;
    if (ctrl(0x00, 5, 1, 0, 0, NULL) < 0)
        return -1;
    s.addr = 1;
    delay_ms(5);
    if (get_desc(1, 0, 0, dd, 18) < 18)
        return -1;
    if (get_desc(2, 0, 0, s_cfg, 9) < 9)
        return -1;
    n = rd16(s_cfg + 2);
    if (n > (int)sizeof s_cfg) {
        log_printf("usbout: configuration %d B, reading the first %u", n, (unsigned)sizeof s_cfg);
        n = sizeof s_cfg;
    }
    if ((n = get_desc(2, 0, 0, s_cfg, (uint16_t)n)) < 9)
        return -1;
    memset(s.st.name, 0, sizeof s.st.name);
    if (dd[15] && get_desc(3, dd[15], 0x0409, b, sizeof b) > 2) {
        for (uint32_t i = 0; 2u + 2u * i + 1u < b[0] && i < sizeof s.st.name - 1u; i++)
            s.st.name[i] = b[2 + 2 * i] >= 32u && b[2 + 2 * i] < 127u ? (char)b[2 + 2 * i] : '?';
    }
    log_printf("usbout: %04x:%04x '%s', %s speed, configuration %d B", (unsigned)rd16(dd + 8),
               (unsigned)rd16(dd + 10), s.st.name, s.st.high_speed ? "high" : "full", n);
    parse((uint32_t)n);
    if (!s.nalts) {
        log_printf("usbout: no stereo PCM playback interface: not a USB audio output");
        return -2;
    }
    entities();
    if (ctrl(0x00, 9, s.cfg_value, 0, 0, NULL) < 0)
        return -1;
    s.st.bits_max = 0;
    s.st.async = 0;
    for (uint32_t i = 0; i < s.nalts; i++) {
        if (s.alts[i].bits > s.st.bits_max)
            s.st.bits_max = s.alts[i].bits;
        s.st.async |= s.alts[i].fb_ep != 0;
        log_printf("usbout: alt %u.%u: %u-bit in %u B, EP %02x max %u every %u%s, feedback %02x", s.alts[i].ifn,
                   s.alts[i].alt, s.alts[i].bits, s.alts[i].subslot, s.alts[i].ep, s.alts[i].mps, s.alts[i].interval,
                   s.alts[i].sync == 1 ? " async" : s.alts[i].sync == 2 ? " adaptive" : s.alts[i].sync == 3 ? " sync" : "",
                   s.alts[i].fb_ep);
    }
    s.st.nrates = 0;
    if (s.st.uac == 2) {                             /* the clock's sampling frequency RANGE */
        uint16_t idx = (uint16_t)(s.clock << 8 | s.ac_if);

        if (ctrl(0xA1, 2, 0x0100, idx, 2, b) == 2) {
            uint32_t k = rd16(b);

            k = k > 8u ? 8u : k;
            if (ctrl(0xA1, 2, 0x0100, idx, (uint16_t)(2u + 12u * k), b) >= (int)(2u + 12u * k)) {
                for (uint32_t i = 0; i < k; i++) {
                    s.st.rates[i].min = b[2 + 12 * i] | (uint32_t)b[3 + 12 * i] << 8 | (uint32_t)b[4 + 12 * i] << 16 |
                                        (uint32_t)b[5 + 12 * i] << 24;
                    s.st.rates[i].max = b[6 + 12 * i] | (uint32_t)b[7 + 12 * i] << 8 | (uint32_t)b[8 + 12 * i] << 16 |
                                        (uint32_t)b[9 + 12 * i] << 24;
                    s.st.rates[i].res = b[10 + 12 * i] | (uint32_t)b[11 + 12 * i] << 8 |
                                        (uint32_t)b[12 + 12 * i] << 16 | (uint32_t)b[13 + 12 * i] << 24;
                }
                s.st.nrates = k;
            }
        }
    } else {
        const alt_t *deep = &s.alts[0];

        for (uint32_t i = 1; i < s.nalts; i++)
            if (s.alts[i].bits > deep->bits)
                deep = &s.alts[i];
        memcpy(s.st.rates, deep->r, sizeof deep->r);
        s.st.nrates = deep->nr;
    }
    s.st.hw_volume = 0;
    if (s.fu) {                                      /* its volume range */
        uint16_t idx = (uint16_t)(s.fu << 8 | s.ac_if), val = (uint16_t)(0x0200u | s.fu_chan);

        if (s.st.uac == 2 && ctrl(0xA1, 2, val, idx, 8, b) >= 8) {
            s.vol_min = (int16_t)rd16(b + 2);
            s.vol_max = (int16_t)rd16(b + 4);
            s.vol_res = (int16_t)rd16(b + 6);
            s.st.hw_volume = 1;
        } else if (s.st.uac == 1 && ctrl(0xA1, 0x82, val, idx, 2, b) == 2) {
            s.vol_min = (int16_t)rd16(b);
            if (ctrl(0xA1, 0x83, val, idx, 2, b) == 2)
                s.vol_max = (int16_t)rd16(b);
            if (ctrl(0xA1, 0x84, val, idx, 2, b) == 2)
                s.vol_res = (int16_t)rd16(b);
            s.st.hw_volume = 1;
        }
    }
    log_printf("usbout: UAC%d, %u rate ranges (%u..%u Hz), up to %u-bit, %s, volume %s (%d..%d /256 dB)", s.st.uac,
               (unsigned)s.st.nrates, (unsigned)(s.st.nrates ? s.st.rates[0].min : 0),
               (unsigned)(s.st.nrates ? s.st.rates[s.st.nrates - 1u].max : 0), (unsigned)s.st.bits_max,
               s.st.async ? "async" : "adaptive/sync", s.st.hw_volume ? "on the device" : "in software",
               s.vol_min, s.vol_max);
    return 0;
}

/* ---- the sink -------------------------------------------------------------------- */

static void iso_off(void)
{
    s.iso_on = 0;
    OTG(GINTMSK) &= ~GI_SOF;
    for (unsigned ch = 1; ch < 4u; ch++)
        if (HCCHAR(ch) & CC_ENA)
            HCCHAR(ch) |= CC_DIS;
    s.fb_busy = 0;
}

int usb_out_set_format(uint32_t rate, uint32_t bits)
{
    const alt_t *best = NULL;
    uint32_t per_s;
    uint8_t b[4];

    if (s.st.state != USB_OUT_READY)
        return -1;
    for (uint32_t i = 0; i < s.nalts; i++) {         /* the deepest that plays this rate */
        const alt_t *a = &s.alts[i];
        uint32_t iv = interval_of(a, a->interval), ps = (s.st.high_speed ? 8000u : 1000u) / iv;
        uint32_t maxf = rate / ps + 2u;

        if (maxf * 2u * a->subslot > a->mps)
            continue;
        if (s.st.uac == 1 ? !rate_in(a->r, a->nr, rate) : !rate_in(s.st.rates, s.st.nrates, rate))
            continue;
        if (!best || a->bits > best->bits || (a->bits == best->bits && a->subslot > best->subslot))
            best = a;
    }
    if (!best) {
        log_printf("usbout: %u Hz: the device cannot play it", (unsigned)rate);
        return -2;
    }
    if (best->bits < bits)
        log_printf("usbout: a %u-bit track as %u-bit: the device has no deeper format at %u Hz", (unsigned)bits,
                   best->bits, (unsigned)rate);
    if (s.a && (s.a != best || s.st.rate != rate)) { /* the stream off before anything changes */
        iso_off();
        ctrl(0x01, 11, 0, s.a->ifn, 0, NULL);
        s.a = NULL;
        s.st.bits = 0;
    }
    if (s.a)
        return 0;
    if (s.st.uac == 2 && s.clock) {
        b[0] = (uint8_t)rate; b[1] = (uint8_t)(rate >> 8); b[2] = (uint8_t)(rate >> 16); b[3] = (uint8_t)(rate >> 24);
        if (ctrl(0x21, 1, 0x0100, (uint16_t)(s.clock << 8 | s.ac_if), 4, b) < 0)
            return -3;
    }
    if (ctrl(0x01, 11, best->alt, best->ifn, 0, NULL) < 0)
        return -3;
    if (s.st.uac == 1 && best->freq_ctl) {
        b[0] = (uint8_t)rate; b[1] = (uint8_t)(rate >> 8); b[2] = (uint8_t)(rate >> 16);
        if (ctrl(0x22, 1, 0x0100, best->ep, 3, b) < 0)
            return -3;
    }
    NVIC_ICER0 = 1u << USB_IRQ;
    s.a = best;
    s.iv = interval_of(best, best->interval);
    s.fb_iv = best->fb_ep ? interval_of(best, best->fb_interval) : 1u;
    if (s.fb_iv < s.iv)
        s.fb_iv = s.iv;
    per_s = (s.st.high_speed ? 8000u : 1000u) / s.iv;
    s.nominal = s.per_pkt = (uint32_t)(((uint64_t)rate << 16) / per_s);
    s.acc = 0;
    s.bpf = 2u * best->subslot;
    s.maxf = best->mps / s.bpf;
    s.consuming = s.ended = 0;
    s.st.rate = rate;
    s.st.bits = best->bits;
    s.st.subslot = best->subslot;
    s.st.fb_q16 = 0;
    s.iso_on = 1;
    OTG(GINTSTS) = GI_SOF;
    OTG(GINTMSK) |= GI_SOF;
    NVIC_ISER0 = 1u << USB_IRQ;
    log_printf("usbout: %u Hz, %u-bit in %u B (alt %u), %u.%04u frames every %u %sframe(s)%s", (unsigned)rate,
               best->bits, best->subslot, best->alt, (unsigned)(s.nominal >> 16),
               (unsigned)((s.nominal & 0xFFFFu) * 10000u >> 16), (unsigned)s.iv, s.st.high_speed ? "micro" : "",
               best->fb_ep ? ", feedback" : "");
    return 0;
}

void usb_out_release(void)
{
    const alt_t *a = s.a;

    if (!a)
        return;
    iso_off();
    s.a = NULL;
    s.consuming = 0;
    s.st.bits = 0;
    if (s.st.state == USB_OUT_READY)
        ctrl(0x01, 11, 0, a->ifn, 0, NULL);
}

void usb_out_stream_start(void)
{
    NVIC_ICER0 = 1u << USB_IRQ;
    s.li = 0;
    block_load();
    s.ended = 0;
    s.paused = 0;
    s.consuming = 1;
    NVIC_ISER0 = 1u << USB_IRQ;
}

void usb_out_stream_stop(void)
{
    s.consuming = 0;
}

int usb_out_streaming(void)
{
    return s.consuming && !s.ended && s.iso_on;
}

void usb_out_pause(int on)
{
    s.paused = on;
}

int usb_out_set_volume(int db)
{
    if (s.st.state == USB_OUT_READY && s.st.hw_volume) {
        int32_t v = db * 256;
        uint8_t b[2];

        s.gain = 32768;
        if (v < s.vol_min)
            v = s.vol_min;
        if (v > s.vol_max)
            v = s.vol_max;
        b[0] = (uint8_t)v;
        b[1] = (uint8_t)((uint32_t)v >> 8);
        if (s.fu_chan) {                             /* per channel: left and right */
            ctrl(0x21, 1, 0x0201, (uint16_t)(s.fu << 8 | s.ac_if), 2, b);
            ctrl(0x21, 1, 0x0202, (uint16_t)(s.fu << 8 | s.ac_if), 2, b);
        } else {
            ctrl(0x21, 1, 0x0200, (uint16_t)(s.fu << 8 | s.ac_if), 2, b);
        }
        return 1;
    }
    {                                                /* 10^(db/20) in Q15, 1 dB steps from a table */
        static const uint16_t k_db[20] = { 32767, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627,
                                           10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677 };
        int d = -db;
        int32_t g = 32768;

        while (d >= 20) {
            g /= 10;                                 /* -20 dB */
            d -= 20;
        }
        s.gain = d ? (int32_t)((g * (int32_t)k_db[d]) >> 15) : g;
        return 0;
    }
}

/* ---- the session ---------------------------------------------------------------- */

int usb_out_start(int supply)
{
    uint32_t depth;

    if (s.started)
        return 0;
    memset(&s, 0, sizeof s);
    s.gain = 32768;
    s.supply = supply;
    usb_dwc2_phy_on();
    RK_GRF[0xB8 / 4] = 0x00060006u;                  /* D+/D- 15k pull-downs (host) */
    RK_GRF[0xB4 / 4] = 0x06000200u;                  /* IDDIG from software, 0: A-device */
    for (uint32_t i = 0; i < 10000u && !(OTG(GRSTCTL) & 0x80000000u); i++)
        rk_delay_us(1);
    OTG(GRSTCTL) |= 1u;                              /* core soft reset */
    for (uint32_t i = 0; i < 10000u && (OTG(GRSTCTL) & 1u); i++)
        rk_delay_us(1);
    OTG(PCGCCTL) = 0;
    OTG(GUSBCFG) = (OTG(GUSBCFG) & ~(1u << 30)) | 1u << 29;   /* ForceHstMode */
    delay_ms(25);
    OTG(HCFG) &= ~7u;                                /* 30/60 MHz PHY clock, high speed allowed */
    depth = OTG(GHWCFG3) >> 16;
    if (depth >= 0x280u) {                           /* RX 1 KiB; non-periodic TX 512 B; periodic TX 1 KiB */
        OTG(GRXFSIZ) = 0x100u;
        OTG(GNPTXFSIZ) = 0x80u << 16 | 0x100u;
        OTG(HPTXFSIZ) = 0x100u << 16 | 0x180u;
    } else {
        OTG(GRXFSIZ) = 0x80u;
        OTG(GNPTXFSIZ) = 0x40u << 16 | 0x80u;
        OTG(HPTXFSIZ) = 0x80u << 16 | 0xC0u;
    }
    OTG(GRSTCTL) = 0x10u << 6 | 0x20u;               /* flush every TX FIFO, then RX */
    for (uint32_t i = 0; i < 100000u && (OTG(GRSTCTL) & 0x20u); i++)
        ;
    OTG(GRSTCTL) = 0x10u;
    for (uint32_t i = 0; i < 100000u && (OTG(GRSTCTL) & 0x10u); i++)
        ;
    for (unsigned ch = 0; ch < 4u; ch++) {
        HCINT(ch) = 0xFFFFFFFFu;
        HCINTMSK(ch) = HC_XFERCOMPL | HC_CHHLTD | HC_STALL | HC_NAK | HC_XACTERR | HC_BBLERR | HC_FRMOVRUN |
                       HC_DTERR;
    }
    OTG(HAINTMSK) = 0xFu;
    OTG(GINTSTS) = 0xFFFFFFFFu;
    OTG(GINTMSK) = GI_RXFLVL | GI_PRTINT | GI_HCHINT | GI_DISCONN;
    /* VBUS: the PHY's DRVVBUS always; GPIO2.1 too if the setting says so (the player then runs
     * that path the other way: charging off, as stock's "power only" state) */
    RK_GRF[0xB8 / 4] = 0x00100010u;
    if (supply == USB_OUT_SUPPLY_GPIO21)
        board_usb_supply(BOARD_SUPPLY_VBUS);
    OTG(HPRT) = (OTG(HPRT) & ~HP_W1C) | HP_PWR;
    usb_irq_hook = host_irq;
    OTG(GAHBCFG) |= 1u;
    NVIC_ISER0 = 1u << USB_IRQ;
    s.started = 1;
    s.st.state = USB_OUT_WAIT;
    log_printf("usbout: host on, VBUS from %s; FIFO %u words", supply == USB_OUT_SUPPLY_GPIO21 ? "GPIO2.1 + DRVVBUS" :
               "DRVVBUS + port power", (unsigned)depth);
    return 0;
}

static void drop(const char *why)
{
    iso_off();
    s.a = NULL;
    s.consuming = 0;
    s.ended = 1;
    s.st.bits = 0;
    s.addr = 0;
    s.conn_ms = 0;
    s.disconnected = 0;
    log_printf("usbout: %s", why);
}

void usb_out_stop(void)
{
    if (!s.started)
        return;
    drop("host off");
    NVIC_ICER0 = 1u << USB_IRQ;
    OTG(GAHBCFG) &= ~1u;
    usb_irq_hook = 0;
    OTG(HPRT) &= ~(HP_W1C | HP_PWR);
    RK_GRF[0xB8 / 4] = 0x00160000u;                  /* DRVVBUS, pull-downs off */
    if (s.supply == USB_OUT_SUPPLY_GPIO21)
        board_usb_supply(BOARD_SUPPLY_NORMAL);
    OTG(GUSBCFG) &= ~(1u << 29);
    RK_GRF[0xB4 / 4] = 0x06000000u;                  /* IDDIG from the PHY */
    usb_dwc2_phy_off();
    s.started = 0;
    s.st.state = USB_OUT_OFF;
}

int usb_out_poll(void)
{
    uint32_t p, now;

    if (!s.started)
        return USB_OUT_OFF;
    p = OTG(HPRT);
    now = time_ms();
    if (s.st.state != USB_OUT_WAIT && (s.disconnected || !(p & HP_CONN))) {
        drop("device unplugged");
        s.st.state = USB_OUT_WAIT;
        memset(s.st.name, 0, sizeof s.st.name);
        return s.st.state;
    }
    if (s.st.state == USB_OUT_WAIT) {
        if (!(p & HP_CONN)) {
            s.conn_ms = 0;
        } else if (!s.conn_ms) {
            s.conn_ms = now | 1u;
        } else if (now - s.conn_ms >= 100u) {        /* connect debounce, then reset and enumerate */
            int r = enumerate();

            s.st.state = r == 0 ? USB_OUT_READY : r == -2 ? USB_OUT_UNSUPPORTED : USB_OUT_ERROR;
            if (r == 0)
                usb_out_set_volume(0);
        }
    }
    return s.st.state;
}
