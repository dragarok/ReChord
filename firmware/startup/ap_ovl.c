/*
 * ap_ovl.c -- core A's overlays (services/ovl.h): each one's header and table of entry points,
 * which firmware_ap.ld places in the overlay, and the resident gates in front of them.
 *
 *   usbdac  the USB DAC (UAC2): drivers/usb_uac_rknanoc.c and the session (startup/ap_ui.c)
 *   usbout  USB-C earphones, the player as USB host: drivers/usb_out_rknanoc.c; and the Custom EQ
 *           editor (ui/eq_page.c), which must never evict them
 *   dev     the Developer tests: DAC test, USB console (drivers/usb_cdc_rknanoc.c), USB-C probe,
 *           USB detect, BT listen (drivers/bt_jieli.c bt_listen), Panel TE (drivers/panel_io_rknanoc.c
 *           panel_te_probe) -- startup/ap_ui.c
 *
 * Which code goes in which overlay is firmware_ap.ld's list (by object, and by function section
 * for a file that is partly resident), not this file's. The callers keep the drivers' own names:
 * the link wraps them (ap.mk AP_OVL_WRAP: with -Wl,--wrap=usb_out_start every call from another
 * object reaches __wrap_usb_out_start here), and each table holds the real ones (__real_...), so
 * the drivers and their callers are the same source as when they were resident. A gate that
 * loads (ovl_get) runs from the main loop; the others (ovl_if) only reach an overlay already in
 * the window and give its mode's "off" answer otherwise.
 */
#include <stddef.h>
#include <stdint.h>

#include "services/ovl.h"
#include "services/log.h"
#include "drivers/usb_msc.h"
#include "drivers/usb_out.h"
#include "audio/audio.h"
#include "ui/frame.h"
#include "ui/nav.h"

extern const char __ovl_abi[];       /* absolute: this link's ABI (Makefile) */

/* The header every overlay starts with; tools/ovl_pack.py sets len and crc in the stored copy. */
#define OVL_HEADER(name, id, api) \
    const ovl_hdr_t ovl_hdr_##name __attribute__((section(".ovl_hdr." #name), used)) = \
        { OVL_MAGIC, (uint32_t)__ovl_abi, (id), 0, 0, (api) }
#define OVL_API(name) __attribute__((section(".ovl." #name ".api")))

/* A call into the window, held while it runs (ovl_hold: nothing replaces the overlay under it).
 * get: ovl_get (load it) or ovl_if (only if it is in); dflt when it is not there. */
#define GATE(get, id, T, dflt, call) ({ const T *a_ = get(id); __typeof__(dflt) r_ = (dflt); \
        if (a_) { ovl_hold(1); r_ = a_->call; ovl_hold(-1); } r_; })
#define GATE_V(get, id, T, call) do { const T *a_ = get(id); \
        if (a_) { ovl_hold(1); a_->call; ovl_hold(-1); } } while (0)

static void usbout_stop(void);

const ovl_desc_t ovl_desc[OVL_COUNT] = {
    [OVL_USBDAC] = { "ov_dac", "USB DAC", NULL },
    [OVL_USBOUT] = { "ov_uout", "USB-C out", usbout_stop },
    [OVL_DEV]    = { "ov_dev", "Developer test", NULL },
};

/* The USB interrupt goes wherever usb_irq_hook points (drivers/usb_msc_rknanoc.c): the USB DAC's
 * and the host's handlers are in their overlays and clear it when their mode ends. */
void *volatile *const ovl_hooks[] = { (void *volatile *)&usb_irq_hook, NULL };

/* A refused overlay, on the screen: what the user asked for is not there (the log has why). */
static void refused(unsigned id)
{
    const char *why = ovl_why(id);

    nav_toast(ICON_WARN, TOAST_WARN, "%s: %s", ovl_desc[id].what, why ? why : "unavailable");
}

/* ---- usbdac: the USB DAC ---------------------------------------------------------------------- */

typedef struct {
    void (*session)(void);           /* the whole session: returns when it ends */
    void (*key)(int key);            /* a media key to the host (drivers/usb_uac.h) */
} usbdac_api_t;

void usb_dac_session(void);          /* startup/ap_ui.c */
void __real_usb_uac_key(int key);

static const usbdac_api_t k_usbdac OVL_API(usbdac) = { usb_dac_session, __real_usb_uac_key };
OVL_HEADER(usbdac, OVL_USBDAC, &k_usbdac);

void ap_usb_dac(void)                /* the menu row, the cable's mode (startup/ap_ui.c cable_mode) */
{
    if (!ovl_get(OVL_USBDAC)) {
        refused(OVL_USBDAC);
        return;
    }
    GATE_V(ovl_if, OVL_USBDAC, usbdac_api_t, session());
}

void __wrap_usb_uac_key(int key)     /* startup/ap_bt.c: the earphones' keys, during a session */
{
    GATE_V(ovl_if, OVL_USBDAC, usbdac_api_t, key(key));
}

/* ---- usbout: USB-C earphones (the player as USB host) ------------------------------------------ */

typedef struct {
    int (*active)(void);
    const usb_out_status_t *(*status)(void);
    int (*start)(int supply, uint32_t budget_ma);
    void (*stop)(void);
    int (*poll)(void);
    int (*set_format)(uint32_t rate, uint32_t bits);
    void (*stream_start)(void);
    void (*stream_stop)(void);
    void (*release)(void);
    int (*streaming)(void);
    void (*pause)(int on);
    int (*set_volume)(int db);
    void (*eq)(int a, int i);        /* Settings > Playback > EQ > Custom (ui/eq_page.c), here as well */
} usbout_api_t;

int __real_usb_out_active(void);
const usb_out_status_t *__real_usb_out_status(void);
int __real_usb_out_start(int supply, uint32_t budget_ma);
void __real_usb_out_stop(void);
int __real_usb_out_poll(void);
int __real_usb_out_set_format(uint32_t rate, uint32_t bits);
void __real_usb_out_stream_start(void);
void __real_usb_out_stream_stop(void);
void __real_usb_out_release(void);
int __real_usb_out_streaming(void);
void __real_usb_out_pause(int on);
int __real_usb_out_set_volume(int db);
void eq_page(int a, int i);          /* ui/eq_page.c */

static const usbout_api_t k_usbout OVL_API(usbout) = {
    __real_usb_out_active, __real_usb_out_status, __real_usb_out_start, __real_usb_out_stop,
    __real_usb_out_poll, __real_usb_out_set_format, __real_usb_out_stream_start,
    __real_usb_out_stream_stop, __real_usb_out_release, __real_usb_out_streaming, __real_usb_out_pause,
    __real_usb_out_set_volume, eq_page,
};
OVL_HEADER(usbout, OVL_USBOUT, &k_usbout);

static usb_out_status_t s_out_off;   /* what usb_out_status() says with the host's code not in */

int __wrap_usb_out_active(void);
void __wrap_usb_out_stop(void);

/* Before another overlay takes the window: host mode off and the music back to the jack, as
 * startup/ap_ui.c usb_out_off() does before a USB device mode. (usb_out_tick starts it again.)
 * The gates by their own names: ld's cross-reference check does not follow --wrap for a call
 * from the object that defines the __wrap_ symbol (the call itself does go to the gate). */
static void usbout_stop(void)
{
    if (__wrap_usb_out_active()) {
        __wrap_usb_out_stop();
        audio_set_output(0);
    }
}

int __wrap_usb_out_start(int supply, uint32_t budget_ma)   /* startup/ap_ui.c usb_out_tick */
{
    static int s_told;
    int r = GATE(ovl_get, OVL_USBOUT, usbout_api_t, -1, start(supply, budget_ma));

    if (!ovl_if(OVL_USBOUT) && !s_told) {   /* (it asks every pass while the setting is on) */
        s_told = 1;
        refused(OVL_USBOUT);
    }
    return r;
}

int __wrap_usb_out_active(void)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, 0, active());
}

const usb_out_status_t *__wrap_usb_out_status(void)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, (const usb_out_status_t *)&s_out_off, status());
}

void __wrap_usb_out_stop(void)
{
    GATE_V(ovl_if, OVL_USBOUT, usbout_api_t, stop());
}

int __wrap_usb_out_poll(void)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, (int)USB_OUT_OFF, poll());
}

int __wrap_usb_out_set_format(uint32_t rate, uint32_t bits)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, -1, set_format(rate, bits));
}

void __wrap_usb_out_stream_start(void)
{
    GATE_V(ovl_if, OVL_USBOUT, usbout_api_t, stream_start());
}

void __wrap_usb_out_stream_stop(void)
{
    GATE_V(ovl_if, OVL_USBOUT, usbout_api_t, stream_stop());
}

void __wrap_usb_out_release(void)
{
    GATE_V(ovl_if, OVL_USBOUT, usbout_api_t, release());
}

int __wrap_usb_out_streaming(void)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, 0, streaming());
}

void __wrap_usb_out_pause(int on)
{
    GATE_V(ovl_if, OVL_USBOUT, usbout_api_t, pause(on));
}

int __wrap_usb_out_set_volume(int db)
{
    return GATE(ovl_if, OVL_USBOUT, usbout_api_t, -1, set_volume(db));
}

/* The Custom EQ editor (ui/screens.c pg_eq, ui/eq_page.c) is in the usbout overlay: USB-C earphones
 * keep it in the window while they are on, so the editor never evicts them (any other overlay would,
 * at every main-loop pass). Refused (damaged: ovl.c logs why): the page stays blank; BACK held leaves it. */
void ap_eq(int a, int i)             /* the page's act; its draw with a = -1 */
{
    GATE_V(ovl_get, OVL_USBOUT, usbout_api_t, eq(a, i));
}

/* ---- dev: the Developer tests (Settings > Developer, ui/screens.c) ------------------------------ */

enum { DEV_DAC_TEST, DEV_USB_CONSOLE, DEV_USB_PROBE, DEV_USB_DETECT, DEV_BT_LISTEN, DEV_TE_PROBE, DEV_HIFI_PROBE };

void dac_test_run(void), usb_console_run(void), usb_probe_menu(void), usb_detect_test(void),
     bt_listen_test(void), te_probe_test(void);   /* startup/ap_ui.c */
void hifi_probe_test(void);                       /* audio/hifi_probe.c */

static void (*const k_dev[])(void) OVL_API(dev) = {
    [DEV_DAC_TEST] = dac_test_run, [DEV_USB_CONSOLE] = usb_console_run, [DEV_USB_PROBE] = usb_probe_menu,
    [DEV_USB_DETECT] = usb_detect_test, [DEV_BT_LISTEN] = bt_listen_test, [DEV_TE_PROBE] = te_probe_test,
    [DEV_HIFI_PROBE] = hifi_probe_test,
};
OVL_HEADER(dev, OVL_DEV, k_dev);

static void dev_run(unsigned n)
{
    if (!ovl_get(OVL_DEV)) {
        refused(OVL_DEV);
        return;
    }
    ovl_run(OVL_DEV, n);
}

void ap_dac_test(void) { dev_run(DEV_DAC_TEST); }
void ap_usb_console(void) { dev_run(DEV_USB_CONSOLE); }
void ap_usb_probe(void) { dev_run(DEV_USB_PROBE); }
void ap_usb_detect(void) { dev_run(DEV_USB_DETECT); }
void ap_bt_listen(void) { dev_run(DEV_BT_LISTEN); }
void ap_te_probe(void) { dev_run(DEV_TE_PROBE); }
void ap_hifi_probe(void) { dev_run(DEV_HIFI_PROBE); }
