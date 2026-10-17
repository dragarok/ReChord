/*
 * usb_uac_rknanoc.c -- USB Audio Class 2.0 device on the DWC2 core (usb_uac.h).
 *
 * Interrupt driven (NVIC IRQ 5): SETUPs and their data stages, the isochronous
 * OUT audio endpoint (EP1 OUT, one packet per (micro)frame) and the feedback
 * endpoint (EP1 IN, every 8 microframes at high speed, every frame at full speed)
 * are all served from uac_irq() (usb_irq_hook); usb_uac_run() sleeps in WFI and does what
 * may take time: re-clocking the DAC, I2C volume writes, the screen.
 *
 * Samples go from the RX FIFO straight into the ring audio.c's DMA plays (I2S
 * words). The ring is the clock master: the feedback value is the nominal rate
 * corrected by how far the ring's fill is from its target, so the host sends
 * exactly what the DAC consumes and nothing is resampled, dropped or padded.
 */
#include <stdint.h>
#include <string.h>

#include "clock.h"
#include "services/watchdog.h"
#include "dac_cs43131.h"
#include "rknanoc.h"
#include "usb_msc.h"
#include "usb_uac.h"
#include "audio/audio.h"
#include "services/log.h"
#include "services/bootcrumb.h"

#define OTG(off)    (*(volatile uint32_t *)(0x60080000u + (off)))
#define DIEPCTL(n)  OTG(0x900u + (n) * 0x20u)
#define DIEPINT(n)  OTG(0x908u + (n) * 0x20u)
#define DIEPTSIZ(n) OTG(0x910u + (n) * 0x20u)
#define DTXFSTS(n)  OTG(0x918u + (n) * 0x20u)
#define DOEPCTL(n)  OTG(0xB00u + (n) * 0x20u)
#define DOEPINT(n)  OTG(0xB08u + (n) * 0x20u)
#define DOEPTSIZ(n) OTG(0xB10u + (n) * 0x20u)
#define EPFIFO(n)   (*(volatile uint32_t *)(0x60081000u + (n) * 0x1000u))
#define NVIC_ISER0  (*(volatile uint32_t *)0xE000E100u)
#define NVIC_ICER0  (*(volatile uint32_t *)0xE000E180u)
#define USB_IRQ     5u

enum {
    GAHBCFG = 0x08, GRSTCTL = 0x10, GINTSTS = 0x14, GINTMSK = 0x18, GRXSTSP = 0x20, GRXFSIZ = 0x24,
    GNPTXFSIZ = 0x28, GHWCFG3 = 0x4C, DIEPTXF1 = 0x104, DIEPTXF2 = 0x108,
    DCFG = 0x800, DCTL = 0x804, DSTS = 0x808, DAINTMSK = 0x81C,
};

#define GI_RXFLVL   (1u << 4)
#define GI_USBSUSP  (1u << 11)
#define GI_USBRST   (1u << 12)
#define GI_ENUMDNE  (1u << 13)
#define GI_IEPINT   (1u << 18)
#define GI_OEPINT   (1u << 19)
#define GI_INCOMPISOIN  (1u << 20)
#define GI_INCOMPISOOUT (1u << 21)
#define GI_ERLYSUSP (1u << 10)
#define GI_SESSREQ  (1u << 30)
#define GI_WKUP     (1u << 31)

#define EP_ENA      0x80000000u
#define EP_DIS      0x40000000u
#define EP_SODD     0x20000000u
#define EP_SEVN     0x10000000u
#define EP_SNAK     0x08000000u
#define EP_CNAK     0x04000000u
#define EP_STALL    0x00200000u
#define EP_ISO      0x00040000u   /* EPTYPE 01 */
#define EP_INTR     0x000C0000u   /* EPTYPE 11 */
#define EP_SD0PID   0x10000000u   /* (= SEVN: for interrupt/bulk endpoints the DATA0 toggle) */
#define EP_ACTIVE   0x00008000u

/* ---- the function: entity IDs, interfaces, rates, formats ----------------- */

#define IF_AC   0
#define IF_AS   1
#define ID_CLK  0x10
#define ID_IT   0x01
#define ID_FU   0x02
#define ID_OT   0x03
#define EP_AUDIO 1u                   /* OUT 0x01; its feedback IN 0x81 */
#define IF_HID  2
#define EP_HID  2u                    /* IN 0x82: media keys for the host */

static const uint32_t k_rates[6] = { 44100, 48000, 88200, 96000, 176400, 192000 };
static const uint8_t k_subslot[3] = { 4, 3, 2 };    /* alternate settings 1, 2, 3 */
static const uint8_t k_bits[3] = { 32, 24, 16 };
#define VOL_MIN_256 (-96 * 256)        /* the FU's range: -96 .. 0 dB in 0.5 dB steps */

#define RING_FRAMES (AUDIO_EXT_BLOCKS * AUDIO_EXT_FRAMES)
#define FILL_TARGET (RING_FRAMES / 2u)

/* Fields the interrupt and usb_uac_run() both use are volatile: the loop must
 * see what the interrupt wrote (a SET CUR of the rate went unseen otherwise). */
static struct {
    int high;
    volatile int configured, alt, primed;
    volatile uint32_t rate, rate_req; /* rate_req: SET CUR waiting for usb_uac_run() */
    volatile int vol_req;             /* volume/mute changed: usb_uac_run() writes the DAC */
    int local_db;                     /* the player's own volume (its keys), on top of the host's */
    volatile int32_t vol;             /* 1/256 dB */
    volatile uint8_t mute;
    uint32_t w, wt;                   /* next ring frame to write; frames written in all */
    uint32_t z, zt;                   /* next ring frame to zero (the DMA's last position); passed in all */
    volatile int wake;                /* the interrupt has work for usb_uac_run() */
    volatile int rate_ack;            /* SET CUR's status stage waits for the re-clock */
    uint32_t seen_pkts, seen_ms;      /* the loop's view: packets at seen_ms (the host stopped?) */
    uint32_t drop_fill, drop_pos;     /* where the last drop happened (diagnostics) */
    uint8_t fb_odd;                   /* parity the feedback EP is armed for */
    uint8_t hid_q[8];                 /* HID reports waiting for the endpoint (keys: press, release) */
    volatile uint8_t hid_head, hid_n, hid_busy;
    usb_uac_status_t st;
    /* EP0 */
    uint8_t setup[8];
    const uint8_t *in;
    uint32_t in_left;
    int in_zlp, in_busy;
    uint8_t out[64];
    uint32_t out_len, out_got;
    int out_wait;
    int suspended;
} s;

static int s_self_powered;            /* (usb_uac_self_powered(): kept across usb_uac_run's reset) */
static uint8_t s_buf[256];            /* EP0 replies built at run time */
static uint8_t s_cfg[320];
static uint32_t s_cfg_len, s_cfg_hid;   /* (the HID descriptor's offset in it) */
static uint32_t s_pkt[1024 / 4];      /* one isochronous packet, as it left the FIFO */

static const uint8_t k_dev[18] = {
    18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64,   /* USB 2.0, IAD class triple */
    0x1B, 0x07, 0x07, 0x32, 0x00, 0x01,        /* 071b:3207, bcdDevice 1.00 */
    1, 2, 3, 1,
};
static const uint8_t k_qual[10] = { 10, 6, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 1, 0 };
static const char *const k_str[4] = { 0, "ReChord", "ReChord USB DAC", "000000000002" };

/* HID consumer control, one byte: bit 0 play/pause, 1 next track, 2 previous track
 * (what stock's DAC sends too), 3 stop; the other four bits padding. */
static const uint8_t k_hid_report[] = {
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,             /* Consumer page, Consumer Control, application */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x04,  /* 4 one-bit fields */
    0x09, 0xCD, 0x09, 0xB5, 0x09, 0xB6, 0x09, 0xB7,  /* play/pause, next, previous, stop */
    0x81, 0x02, 0x95, 0x04, 0x81, 0x01, 0xC0,        /* input (var), 4 bits padding, end */
};

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static uint32_t max_rate(int high) { return high ? 192000u : 96000u; }
static uint32_t nrates(int high) { return high ? 6u : 4u; }

/* The configuration for this speed (or the other one: GET_DESCRIPTOR(7)). */
static uint32_t build_cfg(int high, uint8_t type)
{
    uint8_t *p = s_cfg, *ac;
    uint32_t n;

    p[0] = 9; p[1] = type; p[4] = 3; p[5] = 1; p[6] = 0;
    p[7] = s_self_powered ? 0xC0 : 0x80;                      /* self powered: */
    p[8] = s_self_powered ? 1 : 50;                           /* 2 mA; else 100 mA */
    p += 9;
    memcpy(p, (const uint8_t[]){ 8, 0x0B, IF_AC, 2, 1, 0, 0x20, 0 }, 8);      /* IAD */
    p += 8;
    memcpy(p, (const uint8_t[]){ 9, 4, IF_AC, 0, 0, 1, 1, 0x20, 0 }, 9);      /* AudioControl */
    p += 9;
    ac = p;
    memcpy(p, (const uint8_t[]){ 9, 0x24, 1, 0x00, 0x02, 0x01, 0, 0, 0 }, 9);  /* header, UAC 2.0 */
    p += 9;
    memcpy(p, (const uint8_t[]){ 8, 0x24, 0x0A, ID_CLK, 0x03, 0x07, 0, 0 }, 8);  /* clock source */
    p += 8;
    memcpy(p, (const uint8_t[]){ 17, 0x24, 0x02, ID_IT, 0x01, 0x01, 0, ID_CLK, 2, 3, 0, 0, 0, 0, 0, 0, 0 }, 17);
    p += 17;
    memcpy(p, (const uint8_t[]){ 18, 0x24, 0x06, ID_FU, ID_IT, 0x0F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 18);
    p += 18;
    memcpy(p, (const uint8_t[]){ 12, 0x24, 0x03, ID_OT, 0x02, 0x03, 0, ID_FU, ID_CLK, 0, 0, 0 }, 12);
    p += 12;
    put16(ac + 6, (uint32_t)(p - ac));
    memcpy(p, (const uint8_t[]){ 9, 4, IF_AS, 0, 0, 1, 2, 0x20, 0 }, 9);      /* alt 0: no bandwidth */
    p += 9;
    for (unsigned a = 0; a < 3u; a++) {
        uint32_t mps = (max_rate(high) / (high ? 8000u : 1000u) + 1u) * 2u * k_subslot[a];

        memcpy(p, (const uint8_t[]){ 9, 4, IF_AS, (uint8_t)(a + 1u), 2, 1, 2, 0x20, 0 }, 9);
        p += 9;
        memcpy(p, (const uint8_t[]){ 16, 0x24, 0x01, ID_IT, 0, 1, 1, 0, 0, 0, 2, 3, 0, 0, 0, 0 }, 16);
        p += 16;
        memcpy(p, (const uint8_t[]){ 6, 0x24, 0x02, 1, k_subslot[a], k_bits[a] }, 6);
        p += 6;
        memcpy(p, (const uint8_t[]){ 7, 5, EP_AUDIO, 0x05, 0, 0, 1 }, 7);       /* iso, async, data */
        put16(p + 4, mps);
        p += 7;
        memcpy(p, (const uint8_t[]){ 8, 0x25, 0x01, 0, 0, 0, 0, 0 }, 8);
        p += 8;
        memcpy(p, (const uint8_t[]){ 7, 5, 0x80 | EP_AUDIO, 0x11, 0, 0, 0 }, 7);  /* iso, feedback */
        put16(p + 4, high ? 4u : 3u);
        p[6] = high ? 4 : 1;                                                    /* 1 ms either way */
        p += 7;
    }
    memcpy(p, (const uint8_t[]){ 9, 4, IF_HID, 0, 1, 3, 0, 0, 0 }, 9);        /* HID, no boot protocol */
    p += 9;
    s_cfg_hid = (uint32_t)(p - s_cfg);
    memcpy(p, (const uint8_t[]){ 9, 0x21, 0x11, 0x01, 0, 1, 0x22, sizeof k_hid_report, 0 }, 9);
    p += 9;
    memcpy(p, (const uint8_t[]){ 7, 5, 0x80 | EP_HID, 0x03, 1, 0, 0 }, 7);  /* interrupt, 1 byte */
    p[6] = high ? 7 : 8;                                                        /* every 8 ms */
    p += 7;
    n = (uint32_t)(p - s_cfg);
    put16(s_cfg + 2, n);
    return n;
}

/* ---- EP0 --------------------------------------------------------------------- */

static void fifo_write(unsigned ep, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i += 4u) {
        uint32_t v = 0;

        for (uint32_t b = 0; b < 4u && i + b < n; b++)
            v |= (uint32_t)src[i + b] << (8u * b);
        EPFIFO(ep) = v;
    }
}

static void arm_ep0_out(void)
{
    DOEPTSIZ(0) = 0x60080040u;       /* 3 SETUP packets, 1 packet, 64 bytes */
    DOEPCTL(0) |= EP_ENA | EP_CNAK;
}

/* Next packet of the IN data stage (one per transfer: EP0 transfers are <= 127 B). */
static void ep0_in_next(void)
{
    uint32_t n = s.in_left < 64u ? s.in_left : 64u;

    if (!s.in_busy)
        return;
    if (!n && !s.in_zlp) {
        s.in_busy = 0;               /* all sent: the host's status OUT follows */
        return;
    }
    if (n < 64u)
        s.in_zlp = 0;                /* a short packet ends the stage */
    DIEPTSIZ(0) = n | 1u << 19;
    DIEPCTL(0) |= EP_ENA | EP_CNAK;
    fifo_write(0, s.in, n);
    s.in += n;
    s.in_left -= n;
}

static void ep0_send(const void *data, uint32_t n, uint32_t wlen)
{
    if (n > wlen)
        n = wlen;
    s.in = data;
    s.in_left = n;
    s.in_zlp = n < wlen && (n & 63u) == 0u;       /* a full last packet needs a ZLP after it */
    s.in_busy = 1;
    ep0_in_next();
}

static void ep0_status_in(void)
{
    DIEPTSIZ(0) = 1u << 19;
    DIEPCTL(0) |= EP_ENA | EP_CNAK;
}

static void ep0_stall(void)
{
    DIEPCTL(0) |= EP_STALL;
    DOEPCTL(0) |= EP_STALL;
}

/* ---- streaming ------------------------------------------------------------------ */

static uint32_t next_odd(void)
{
    return ((OTG(DSTS) >> 8) + 1u) & 1u;
}

static void iso_out_arm(void)
{
    uint32_t mps = DOEPCTL(EP_AUDIO) & 0x7FFu;

    DOEPTSIZ(EP_AUDIO) = mps | 1u << 19;
    DOEPCTL(EP_AUDIO) |= EP_ENA | EP_CNAK | (next_odd() ? EP_SODD : EP_SEVN);
}

/* Everything the DMA has passed is zeroed at once: if the host falls behind (or
 * stops), the ring plays silence, never what it played a lap ago. Called from the
 * interrupt (every packet) and the loop (every tick): the DMA moves less than a
 * lap in between (2048 frames: 10.7 ms at 192 kHz). */
static void zero_played(void)
{
    uint32_t pos = audio_ext_pos(), z = s.z, n = (pos - z) % RING_FRAMES;

    if (!n)
        return;
    {
        uint64_t *ring = (uint64_t *)audio_ext_ring();   /* a frame: two I2S words */

        s.zt += n;
        while (n--) {
            ring[z] = 0;
            z = (z + 1u) % RING_FRAMES;
        }
        s.z = z;
    }
}

/* Frames written and not yet passed by the DMA (block granular); < 0: it caught up. */
static int32_t ring_fill(void)
{
    return (int32_t)(s.wt - s.zt);
}

/* Samples per (micro)frame the DAC consumes, as the host wants it: Q16.16 per
 * microframe at high speed, Q10.14 per frame at full speed. The nominal rate,
 * corrected by the fill error (a P loop with a ~2 s time constant). */
static uint32_t feedback_value(void)
{
    int32_t err = s.primed ? (int32_t)FILL_TARGET - ring_fill() : 0;

    if (s.high) {
        int32_t nom = (int32_t)((s.rate << 13) / 1000u), fb = nom + err * 4;   /* rate/8000 in Q16.16 */

        if (fb > nom + nom / 100)
            fb = nom + nom / 100;
        if (fb < nom - nom / 100)
            fb = nom - nom / 100;
        return (uint32_t)fb;
    } else {
        int32_t nom = (int32_t)((s.rate << 14) / 1000u), fb = nom + err * 8;   /* rate/1000 in Q10.14 */

        if (fb > nom + nom / 100)
            fb = nom + nom / 100;
        if (fb < nom - nom / 100)
            fb = nom - nom / 100;
        return (uint32_t)fb;
    }
}

static void fb_arm(uint32_t odd)
{
    uint32_t v = feedback_value(), n = s.high ? 4u : 3u;

    DIEPTSIZ(EP_AUDIO) = n | 1u << 19 | 1u << 29;  /* 1 packet, MC 1 */
    DIEPCTL(EP_AUDIO) |= EP_ENA | EP_CNAK | (odd ? EP_SODD : EP_SEVN);
    EPFIFO(EP_AUDIO) = v;
    s.fb_odd = (uint8_t)odd;
}

static void stream_on(int alt)
{
    uint32_t mps = (max_rate(s.high) / (s.high ? 8000u : 1000u) + 1u) * 2u * k_subslot[alt - 1];

    s.alt = alt;
    s.st.bits = k_bits[alt - 1];
    s.primed = 0;
    DOEPCTL(EP_AUDIO) = mps | EP_ISO | EP_ACTIVE | EP_SNAK;
    DIEPCTL(EP_AUDIO) = (s.high ? 4u : 3u) | EP_ISO | EP_ACTIVE | EP_AUDIO << 22 | EP_SNAK;
    OTG(DAINTMSK) |= 1u << (16 + EP_AUDIO) | 1u << EP_AUDIO;
    iso_out_arm();
    fb_arm(next_odd());
}


#define GI_GOUTNAKEFF (1u << 7)
/* Disabling an endpoint as the DWC2 databook has it (Linux dwc2_hsotg_ep_stop_xfr): OUT = the
 * global OUT NAK first (DCTL.SGOUTNak, GINTSTS.GOUTNakEff), then EPDis|SNAK, EPDisbld awaited,
 * CGOUTNak; IN = SNAK, DIEPINT.INEPNakEff awaited, EPDis|SNAK, EPDisbld awaited, then its TX
 * FIFO flushed (a disable does not empty it). */
static void ep_out_stop(unsigned ep)
{
    uint32_t n;

    if (!(DOEPCTL(ep) & EP_ENA))
        return;
    OTG(DCTL) |= 1u << 9;
    for (n = 0; n < 100000u && !(OTG(GINTSTS) & GI_GOUTNAKEFF); n++)
        ;
    DOEPCTL(ep) |= EP_DIS | EP_SNAK;
    for (n = 0; n < 100000u && !(DOEPINT(ep) & 2u); n++)
        ;
    DOEPINT(ep) = 2u;
    OTG(DCTL) |= 1u << 10;
}

static void ep_in_stop(unsigned ep)
{
    uint32_t n;

    if (!(DIEPCTL(ep) & EP_ENA))
        return;
    DIEPCTL(ep) |= EP_SNAK;
    for (n = 0; n < 100000u && !(DIEPINT(ep) & (1u << 6)); n++)
        ;
    DIEPINT(ep) = 1u << 6;
    DIEPCTL(ep) |= EP_DIS | EP_SNAK;
    for (n = 0; n < 100000u && !(DIEPINT(ep) & 2u); n++)
        ;
    DIEPINT(ep) = 2u;
    OTG(GRSTCTL) = (uint32_t)ep << 6 | 0x20u;
    for (n = 0; n < 100000u && (OTG(GRSTCTL) & 0x20u); n++)
        ;
}

static void stream_off(void)
{
    ep_out_stop(EP_AUDIO);
    ep_in_stop(EP_AUDIO);
    DOEPCTL(EP_AUDIO) = 0;
    DIEPCTL(EP_AUDIO) = 0;
    s.alt = 0;
    s.st.bits = 0;
    s.primed = 0;               /* what is buffered plays out (a pause loses nothing), then zeros */
}

/* One packet of n bytes in s_pkt -> the ring (I2S words, left-justified). */
static void pcm_in(uint32_t n)
{
    uint32_t sub = k_subslot[s.alt - 1], frames = n / (2u * sub);
    int32_t *ring = audio_ext_ring(), fill;
    const uint8_t *b = (const uint8_t *)s_pkt;

    s.st.packets++;                             /* (no clock read here: 8000 packets a second) */
    zero_played();
    fill = ring_fill();
    if (fill < 32) {                            /* start, or the DMA caught up: begin FILL_TARGET */
        if (s.primed)                           /* ahead of it (that span is silence: zero_played) */
            s.st.underruns++;
        s.w = (s.z + FILL_TARGET) % RING_FRAMES;
        s.wt = s.zt + FILL_TARGET;
        fill = FILL_TARGET;
    }                                           /* else a resume while the last of it still plays: */
    s.primed = 1;                               /* straight on after it */
    if ((uint32_t)fill + frames > RING_FRAMES - AUDIO_EXT_FRAMES) {   /* the host is ahead: drop, never overwrite */
        s.st.dropped += frames;
        s.drop_fill = fill;
        s.drop_pos = audio_ext_pos();
        return;
    }
    {
        uint32_t w = s.w;                           /* (locals: the stores to the ring would make */
                                                    /* the compiler reload s.w every frame) */
        if (sub == 4u) {
            for (uint32_t f = 0; f < frames; f++, w = (w + 1u) % RING_FRAMES) {
                ring[2u * w] = (int32_t)s_pkt[2u * f];
                ring[2u * w + 1u] = (int32_t)s_pkt[2u * f + 1u];
            }
        } else if (sub == 3u) {
            for (uint32_t f = 0; f < frames; f++, w = (w + 1u) % RING_FRAMES) {
                const uint8_t *q = b + 6u * f;

                ring[2u * w] = (int32_t)((uint32_t)q[0] << 8 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 24);
                ring[2u * w + 1u] = (int32_t)((uint32_t)q[3] << 8 | (uint32_t)q[4] << 16 | (uint32_t)q[5] << 24);
            }
        } else {
            for (uint32_t f = 0; f < frames; f++, w = (w + 1u) % RING_FRAMES) {
                uint32_t v = s_pkt[f];

                ring[2u * w] = (int32_t)(v << 16);
                ring[2u * w + 1u] = (int32_t)(v & 0xFFFF0000u);
            }
        }
        s.w = w;
    }
    s.wt += frames;
}

/* All TX FIFOs (a few PHY clocks; bounded: a stuck core must not hang the player). */
static void fifo_flush(void)
{
    OTG(GRSTCTL) = 0x10u << 6 | 0x20u;
    for (uint32_t n = 0; n < 100000u && (OTG(GRSTCTL) & 0x20u); n++)
        ;
}

/* ---- media keys (HID) ----------------------------------------------------------- */

/* Off (bus reset, a new configuration): an armed report is disabled, not just
 * forgotten (writing 0 does not clear EPENA), and its FIFO emptied. */
static void hid_off(void)
{
    if (DIEPCTL(EP_HID) & EP_ENA) {
        DIEPCTL(EP_HID) |= EP_SNAK;
        DIEPCTL(EP_HID) |= EP_DIS;
        for (uint32_t n = 0; n < 10000u && (DIEPCTL(EP_HID) & EP_ENA); n++)
            ;
        DIEPINT(EP_HID) = 0xFFu;
        OTG(GRSTCTL) = EP_HID << 6 | 0x20u;
        for (uint32_t n = 0; n < 100000u && (OTG(GRSTCTL) & 0x20u); n++)
            ;
    }
    DIEPCTL(EP_HID) = 0;
    s.hid_n = s.hid_busy = 0;
}

/* The next queued report onto the interrupt endpoint (the host polls it every 8 ms).
 * From the interrupt, or with it masked. */
static void hid_next(void)
{
    if (s.hid_busy || !s.hid_n || !s.configured)
        return;
    DIEPTSIZ(EP_HID) = 1u | 1u << 19;
    DIEPCTL(EP_HID) |= EP_ENA | EP_CNAK;
    EPFIFO(EP_HID) = s.hid_q[s.hid_head];
    s.hid_head = (uint8_t)((s.hid_head + 1u) % sizeof s.hid_q);
    s.hid_n--;
    s.hid_busy = 1;
}

static void hid_queue(uint8_t report)
{
    if (s.hid_n < sizeof s.hid_q) {
        s.hid_q[(s.hid_head + s.hid_n) % sizeof s.hid_q] = report;
        s.hid_n++;
    }
}

void usb_uac_self_powered(int on)
{
    s_self_powered = on;
}

void usb_uac_key(int key)
{
    static const uint8_t bit[3] = { 1u << 0, 1u << 1, 1u << 2 };

    if (key < 0 || key > 2 || !s.configured)
        return;
    NVIC_ICER0 = 1u << USB_IRQ;
    hid_queue(bit[key]);                             /* press, */
    hid_queue(0);                                    /* release */
    hid_next();
    NVIC_ISER0 = 1u << USB_IRQ;
    log_printf("uac: key %s to the host", key == USB_UAC_PLAY_PAUSE ? "play/pause" : key == USB_UAC_NEXT ? "next" : "previous");
}

/* ---- requests -------------------------------------------------------------------- */

static void bus_reset(void)
{
    OTG(DCFG) &= ~0x7F0u;
    for (unsigned n = 0; n < 4u; n++) {
        DIEPINT(n) = 0xFFu;
        DOEPINT(n) = 0xFFu;
    }
    if (s.alt)
        stream_off();
    OTG(DAINTMSK) = 0x10001u;
    s.configured = 0;
    s.in_busy = s.out_wait = 0;
    hid_off();
    arm_ep0_out();
}

/* Class request to the AudioControl interface's entities. */
static void class_request(const uint8_t *r, int out_done)
{
    uint8_t type = r[0], req = r[1], cs = r[3], id = r[5];
    uint32_t wlen = r[6] | r[7] << 8;

    if (type & 0x80u) {                              /* GET */
        if (id == ID_CLK && cs == 1u && req == 1u) {             /* sampling frequency CUR */
            put32(s_buf, s.rate_req ? s.rate_req : s.rate);  /* (a SET not applied yet: it) */
            ep0_send(s_buf, 4, wlen);
        } else if (id == ID_CLK && cs == 1u && req == 2u) {      /* its RANGE: the discrete rates */
            uint32_t k = nrates(s.high);

            put16(s_buf, k);
            for (uint32_t i = 0; i < k; i++) {
                put32(s_buf + 2 + 12 * i, k_rates[i]);
                put32(s_buf + 6 + 12 * i, k_rates[i]);
                put32(s_buf + 10 + 12 * i, 0);
            }
            ep0_send(s_buf, 2 + 12 * k, wlen);
        } else if (id == ID_CLK && cs == 2u && req == 1u) {      /* clock valid */
            s_buf[0] = 1;
            ep0_send(s_buf, 1, wlen);
        } else if (id == ID_FU && cs == 1u && req == 1u) {       /* mute */
            s_buf[0] = s.mute;
            ep0_send(s_buf, 1, wlen);
        } else if (id == ID_FU && cs == 2u && req == 1u) {       /* volume CUR */
            put16(s_buf, (uint32_t)s.vol);
            ep0_send(s_buf, 2, wlen);
        } else if (id == ID_FU && cs == 2u && req == 2u) {       /* volume RANGE */
            put16(s_buf, 1);
            put16(s_buf + 2, (uint32_t)VOL_MIN_256);
            put16(s_buf + 4, 0);
            put16(s_buf + 6, 0x80);
            ep0_send(s_buf, 8, wlen);
        } else {
            ep0_stall();
        }
        return;
    }
    if (!out_done) {                                 /* SET: the data stage first */
        if (!wlen || wlen > sizeof s.out) {
            ep0_stall();
            return;
        }
        s.out_len = wlen;
        s.out_got = 0;
        s.out_wait = 1;
        return;
    }
    if (id == ID_CLK && cs == 1u && req == 1u && s.out_got >= 4u) {
        uint32_t f = s.out[0] | s.out[1] << 8 | s.out[2] << 16 | (uint32_t)s.out[3] << 24;
        uint32_t ok = 0;

        for (uint32_t i = 0; i < nrates(s.high); i++)
            ok |= k_rates[i] == f;
        if (!ok) {
            ep0_stall();
            return;
        }
        s.rate_req = f;                              /* the loop re-clocks, then acks (status) */
        s.rate_ack = 1;
        s.wake = 1;
        return;
    } else if (id == ID_FU && cs == 1u && req == 1u && s.out_got >= 1u) {
        s.mute = s.out[0] ? 1 : 0;
        s.vol_req = 1;
        s.wake = 1;
    } else if (id == ID_FU && cs == 2u && req == 1u && s.out_got >= 2u) {
        int32_t v = (int16_t)(s.out[0] | s.out[1] << 8);

        s.vol = v < VOL_MIN_256 ? VOL_MIN_256 : v > 0 ? 0 : v;
        s.vol_req = 1;
        s.wake = 1;
    } else {
        ep0_stall();
        return;
    }
    ep0_status_in();
}

static void handle_setup(void)
{
    const uint8_t *r = s.setup;
    uint8_t type = r[0], req = r[1];
    uint32_t val = r[2] | r[3] << 8, idx = r[4] | r[5] << 8, wlen = r[6] | r[7] << 8;

    s.in_busy = s.out_wait = 0;
    if ((type & 0x60u) == 0x20u) {                   /* class */
        if ((type & 0x1Fu) == 1u && (idx & 0xFFu) == IF_AC) {
            class_request(r, 0);
            return;
        }
        if ((type & 0x1Fu) == 1u && (idx & 0xFFu) == IF_HID) {
            if (req == 0x0A || req == 0x0B) {        /* SET_IDLE, SET_PROTOCOL */
                ep0_status_in();
                return;
            }
            if (req == 0x01 || req == 0x02 || req == 0x03) {   /* GET_REPORT / IDLE / PROTOCOL */
                s_buf[0] = req == 0x03 ? 1 : 0;     /* (report protocol) */
                ep0_send(s_buf, 1, wlen);
                return;
            }
        }
        ep0_stall();
        return;
    }
    if ((type & 0x60u) == 0u && (type & 0x1Fu) == 1u && req == 6 && (idx & 0xFFu) == IF_HID) {
        if (val >> 8 == 0x22) {                      /* GET_DESCRIPTOR: the HID report descriptor */
            ep0_send(k_hid_report, sizeof k_hid_report, wlen);
            return;
        }
        if (val >> 8 == 0x21) {                      /* the HID descriptor (inside the configuration) */
            build_cfg(s.high, 2);
            ep0_send(s_cfg + s_cfg_hid, 9, wlen);
            return;
        }
        ep0_stall();
        return;
    }
    if (type & 0x60u) {
        ep0_stall();
        return;
    }
    switch (req) {
    case 0:                                          /* GET_STATUS */
        s_buf[0] = (type & 0x1Fu) == 0u && s_self_powered ? 1 : 0;   /* (device: self powered) */
        s_buf[1] = 0;
        ep0_send(s_buf, 2, wlen);
        return;
    case 1:                                          /* CLEAR_FEATURE */
    case 3:                                          /* SET_FEATURE */
        ep0_status_in();
        return;
    case 5:                                          /* SET_ADDRESS */
        OTG(DCFG) = (OTG(DCFG) & ~0x7F0u) | (val & 0x7Fu) << 4;
        ep0_status_in();
        return;
    case 6:                                          /* GET_DESCRIPTOR */
        switch (val >> 8) {
        case 1:
            ep0_send(k_dev, sizeof k_dev, wlen);
            return;
        case 2:
            s_cfg_len = build_cfg(s.high, 2);
            ep0_send(s_cfg, s_cfg_len, wlen);
            return;
        case 7:                                      /* the other speed's configuration */
            s_cfg_len = build_cfg(!s.high, 7);
            ep0_send(s_cfg, s_cfg_len, wlen);
            return;
        case 6:
            ep0_send(k_qual, sizeof k_qual, wlen);
            return;
        case 3:
            if ((val & 0xFFu) == 0u) {
                s_buf[0] = 4; s_buf[1] = 3; s_buf[2] = 0x09; s_buf[3] = 0x04;
                ep0_send(s_buf, 4, wlen);
                return;
            }
            if ((val & 0xFFu) < 4u) {
                const char *str = k_str[val & 0xFFu];
                uint32_t n = 0;

                while (str[n] && n < 60u) {
                    s_buf[2 + 2 * n] = (uint8_t)str[n];
                    s_buf[3 + 2 * n] = 0;
                    n++;
                }
                s_buf[0] = (uint8_t)(2 * n + 2);
                s_buf[1] = 3;
                ep0_send(s_buf, 2 * n + 2, wlen);
                return;
            }
            break;
        }
        ep0_stall();
        return;
    case 8:                                          /* GET_CONFIGURATION */
        s_buf[0] = (uint8_t)s.configured;
        ep0_send(s_buf, 1, wlen);
        return;
    case 9:                                          /* SET_CONFIGURATION */
        if (s.alt)
            stream_off();
        s.configured = val == 1u;
        s.st.configured = s.configured;
        hid_off();
        if (s.configured)
            DIEPCTL(EP_HID) = 1u | EP_INTR | EP_ACTIVE | EP_HID << 22 | EP_SNAK | EP_SD0PID;
        if (s.configured)
            OTG(DAINTMSK) |= 1u << EP_HID;
        ep0_status_in();
        return;
    case 10:                                         /* GET_INTERFACE */
        s_buf[0] = (idx & 0xFFu) == IF_AS ? (uint8_t)s.alt : 0;
        ep0_send(s_buf, 1, wlen);
        return;
    case 11:                                         /* SET_INTERFACE */
        if ((idx & 0xFFu) == IF_AS && val <= 3u) {
            if (s.alt)
                stream_off();
            if (val)
                stream_on((int)val);
            ep0_status_in();
            return;
        }
        if ((idx & 0xFFu) == IF_AC && val == 0u) {
            ep0_status_in();
            return;
        }
        break;
    }
    ep0_stall();
}

/* ---- the interrupt ------------------------------------------------------------ */

static void rx_one(void)
{
    uint32_t st = OTG(GRXSTSP), ep = st & 15u, cnt = (st >> 4) & 0x7FFu, sts = (st >> 17) & 15u;
    uint32_t words = (cnt + 3u) >> 2, i;

    if (sts == 6u) {                                 /* SETUP data */
        uint32_t w0 = EPFIFO(0), w1 = EPFIFO(0);

        for (i = 2; i < words; i++)
            (void)EPFIFO(0);
        memcpy(s.setup, &w0, 4);
        memcpy(s.setup + 4, &w1, 4);
    } else if (sts == 4u) {                          /* SETUP done */
        handle_setup();
    } else if (sts == 2u && ep == 0u) {              /* EP0 OUT data (or the status ZLP) */
        for (i = 0; i < words; i++) {
            uint32_t v = EPFIFO(0);

            for (uint32_t b = 0; b < 4u && 4u * i + b < cnt; b++)
                if (s.out_wait && s.out_got < s.out_len)
                    s.out[s.out_got++] = (uint8_t)(v >> (8u * b));
        }
        if (s.out_wait && s.out_got >= s.out_len) {
            s.out_wait = 0;
            class_request(s.setup, 1);
        }
    } else if (sts == 2u && ep == EP_AUDIO) {        /* audio */
        if (words > sizeof s_pkt / 4u)
            words = sizeof s_pkt / 4u;
        for (i = 0; i < words; i++)
            s_pkt[i] = EPFIFO(0);
        if (s.alt && cnt)
            pcm_in(cnt);
    } else {
        for (i = 0; i < words; i++)
            (void)EPFIFO(0);
    }
}

static void uac_irq(void)
{
    uint32_t gi = OTG(GINTSTS) & OTG(GINTMSK);

    /* core_init()'s GINTMSK has these in (stock's mask) and they latch until written 1: early
     * suspend fires 3 ms after every connect (the bus idles until the host resets it, USB 2.0
     * 7.1.7.6), then at every host suspend; wake-up at every resume; session request at a
     * B-session edge. Not cleared, the line stays up and the session loop never runs (USB DAC
     * audit 2026-10-16, Linux dwc2 clears ErlySusp the same way). */
    if (gi & (GI_ERLYSUSP | GI_SESSREQ | GI_WKUP))
        OTG(GINTSTS) = gi & (GI_ERLYSUSP | GI_SESSREQ | GI_WKUP);
    if (gi & GI_WKUP)
        s.suspended = 0;

    if (gi & GI_USBRST) {
        OTG(GINTSTS) = GI_USBRST;
        bus_reset();
    }
    if (gi & GI_ENUMDNE) {
        OTG(GINTSTS) = GI_ENUMDNE;
        s.high = (OTG(DSTS) & 6u) == 0u;
        s.st.high_speed = s.high;
        DIEPCTL(0) &= ~3u;                           /* EP0 MPS 64 */
        arm_ep0_out();
    }
    while (OTG(GINTSTS) & GI_RXFLVL)
        rx_one();
    /* every endpoint interrupt bit is acknowledged, acted on or not: one left set
     * (EPDISBLD after a re-arm, ...) keeps the line up and starves everything else */
    if (gi & GI_OEPINT) {
        uint32_t o0 = DOEPINT(0), o1 = DOEPINT(EP_AUDIO);

        DOEPINT(0) = o0;
        DOEPINT(EP_AUDIO) = o1;
        if (o0 & 0x9u)                               /* EP0 OUT done / SETUP: ready for the next */
            arm_ep0_out();
        if ((o1 & 1u) && s.alt)                      /* one packet in: ready for the next frame */
            iso_out_arm();
    }
    if (gi & GI_IEPINT) {
        uint32_t i0 = DIEPINT(0), i1 = DIEPINT(EP_AUDIO);

        DIEPINT(0) = i0;
        DIEPINT(EP_AUDIO) = i1;
        if (i0 & 1u)
            ep0_in_next();
        if (DIEPINT(EP_HID)) {                       /* (a media-key report went to the host) */
            uint32_t i2 = DIEPINT(EP_HID);

            DIEPINT(EP_HID) = i2;
            if (i2 & 1u) {
                s.hid_busy = 0;
                hid_next();
            }
        }
        if ((i1 & 1u) && s.alt)                      /* the host read the feedback: its next poll */
            fb_arm(s.high ? s.fb_odd : !s.fb_odd);   /* is 8 microframes on (same parity) or 1 frame */
    }
    if (gi & GI_INCOMPISOOUT) {                      /* the audio EP missed its frame */
        OTG(GINTSTS) = GI_INCOMPISOOUT;
        s.st.missed++;
        if (s.alt) {
            ep_out_stop(EP_AUDIO);
            iso_out_arm();
        }
    }
    if (gi & GI_INCOMPISOIN) {                       /* feedback armed for the other parity */
        OTG(GINTSTS) = GI_INCOMPISOIN;
        if (s.alt) {                                 /* (end of the polled frame: at high speed */
            ep_in_stop(EP_AUDIO);                    /* every poll has that frame's parity; at full */
            fb_arm(s.high ? !s.fb_odd : next_odd()); /* speed the host polls the next frame too) */
        }
    }
    if (gi & GI_USBSUSP) {
        OTG(GINTSTS) = GI_USBSUSP;
        s.suspended = 1;
    }
}

/* ---- the session ---------------------------------------------------------------- */

/* The DAC's attenuation in 0.5 dB steps: the host's volume plus the player's own
 * (its keys work here too: a host that leaves its USB volume at full scale and
 * attenuates in software would otherwise play at full scale). */
static void dac_apply_volume(void)
{
    uint32_t att = (uint32_t)(-s.vol / 128) + (uint32_t)(-s.local_db * 2);
    uint8_t v = s.mute ? 0xFFu : att > 0xFEu ? 0xFEu : (uint8_t)att;

    for (int chan = 0; chan < 2; chan++) {
        dac_write_reg(chan, 0x090001u, v);
        dac_write_reg(chan, 0x090002u, v);
    }
}

static int dac_on;                           /* the DAC powered for this session (usb_uac_run) */

int usb_uac_run(int (*tick)(void), void (*status)(const usb_uac_status_t *))
{
    uint32_t t_status = 0, t_log = 0, drain_ms = 0;
    int ret = 0;

    memset(&s, 0, sizeof s);
    s.rate = 48000;
    s.local_db = audio_get_volume_db();      /* the host's volume starts at 0 dB (until it sets one) */
    /* (Stock powers the DAC only once the host streams, 07_dac_512m.md 4.1. Here it comes up first:
     * starting it later means the USB interrupt off for its ~200 ms of init, and packets lost. What
     * the order must not do is what hw_on() did: drive the DAC's supply pins low after it came up,
     * so usb_dwc2_init_hs() leaves the audio pins alone.) */
    bootcrumb(BC_DAC_AUDIO);
    dac_on = audio_ext_start(s.rate) == 0;
    log_printf("uac: DAC up at %u Hz: %d (step %d)", (unsigned)s.rate, dac_on ? 0 : -1, dac_last_step());
    if (!dac_on)
        return USB_EXIT_IO;
    bootcrumb(BC_DAC_VOLUME);
    dac_apply_volume();
    bootcrumb(BC_DAC_USB);
    usb_dwc2_init_hs();
    if ((OTG(GHWCFG3) >> 16) >= 0x160u) {    /* the FIFO RAM, in words, either FIFO mode: */
        OTG(GRXFSIZ) = 0x120u;               /* RX 1152 B (a 192 kHz 32-bit packet is 200 B), */
        OTG(GNPTXFSIZ) = 0x00200120u;        /* EP0 IN 128 B, the feedback (4 B) and the media */
        OTG(DIEPTXF1) = 0x00100140u;         /* keys (1 B) 64 B each (DIEPTXFn: in shared-FIFO */
        OTG(DIEPTXF2) = 0x00100150u;         /* mode the same registers are DPTXFSIZn) */
        fifo_flush();
    }
    OTG(GINTMSK) |= GI_INCOMPISOIN | GI_INCOMPISOOUT;
    usb_irq_hook = uac_irq;
    OTG(GAHBCFG) |= 1u;                      /* global interrupt */
    NVIC_ISER0 = 1u << USB_IRQ;
    bootcrumb(BC_DAC_WAIT);
    log_printf("uac: waiting for the host");
    while (!ret) {
        uint32_t now = time_ms();

        watchdog_feed();             /* (device 2026-10-16: the DAC session never fed it and reset at ~11 s) */


        if (usb_cable_gone())
            ret = USB_EXIT_UNPLUG;
        else if (tick && tick())
            ret = USB_EXIT_USER;
        s.wake = 0;
        if (s.rate_req && s.rate_req != s.rate && !s.alt && ring_fill() > 0 && (!drain_ms || now - drain_ms < 100u)) {
            if (!drain_ms)                           /* the next track's rate: the last of this */
                drain_ms = now;                      /* one plays out first (the re-clock restarts */
        } else if (s.rate_req) {                     /* the ring silent), up to 100 ms */
            uint32_t r;                              /* SET CUR: re-clock outside the interrupt; */
                                                     /* the host waits for its status stage */
            if (drain_ms)
                delay_us(1000);                      /* (and the I2S FIFO: < 0.4 ms of it) */
            drain_ms = 0;
            NVIC_ICER0 = 1u << USB_IRQ;
            r = s.rate_req;
            s.rate_req = 0;
            if (r != s.rate) {
                s.primed = 0;
                audio_ext_rate(r);
                s.rate = r;
                s.z = s.zt = s.w = s.wt = 0;         /* the ring restarted at frame 0 */
                log_printf("uac: %u Hz", (unsigned)r);
            }
            if (s.rate_ack) {
                s.rate_ack = 0;
                ep0_status_in();
            }
            NVIC_ISER0 = 1u << USB_IRQ;
        }
        if (dac_on && (s.vol_req || audio_get_volume_db() != s.local_db)) {   /* the host's, or the keys' */
            s.vol_req = 0;
            s.local_db = audio_get_volume_db();
            dac_apply_volume();
        }
        NVIC_ICER0 = 1u << USB_IRQ;
        zero_played();                               /* (also while no packets come) */
        NVIC_ISER0 = 1u << USB_IRQ;
        if (*(volatile uint32_t *)&s.st.packets != s.seen_pkts) {   /* packets come: the host streams */
            s.seen_pkts = s.st.packets;
            s.seen_ms = now;
        } else if (s.primed && now - s.seen_ms > 50u) {  /* the host stopped: what was buffered plays */
            s.primed = 0;                            /* out, then the zeroed ring: silence */
            log_printf("uac: idle: packets %u, dropped %u (fill %u at %u), underruns %u, missed %u",
                       (unsigned)s.st.packets, (unsigned)s.st.dropped, (unsigned)s.drop_fill,
                       (unsigned)s.drop_pos, (unsigned)s.st.underruns, (unsigned)s.st.missed);
        }
        if (now - t_status >= 1000u) {
            t_status = now;
            s.st.rate = s.rate;
            s.st.volume_256db = s.vol;
            s.st.local_db = s.local_db;
            s.st.mute = s.mute;
            s.st.fill = s.primed && ring_fill() > 0 ? (uint32_t)ring_fill() : 0;
            if (status)
                status(&s.st);
        }
        if (now - t_log >= 10000u && s.alt) {
            t_log = now;
            log_printf("uac: %u Hz %u-bit, fill %u, packets %u, dropped %u, underruns %u, missed %u",
                       (unsigned)s.rate, (unsigned)s.st.bits, (unsigned)s.st.fill, (unsigned)s.st.packets,
                       (unsigned)s.st.dropped, (unsigned)s.st.underruns, (unsigned)s.st.missed);
        }
        cpu_wait_tick_or(&s.wake);
    }
    NVIC_ICER0 = 1u << USB_IRQ;
    OTG(GAHBCFG) &= ~1u;
    usb_irq_hook = 0;
    if (s.alt)                                       /* left mid-stream (MENU, an unplug): the endpoints */
        stream_off();                                /* stopped as the databook says before */
    hid_off();                                       /* usb_dwc2_stop() flushes every TX FIFO */
    log_printf("uac: end (%d): packets %u, dropped %u, underruns %u, missed %u", ret, (unsigned)s.st.packets,
               (unsigned)s.st.dropped, (unsigned)s.st.underruns, (unsigned)s.st.missed);
    usb_dwc2_stop();
    audio_ext_stop();
    return ret;
}
