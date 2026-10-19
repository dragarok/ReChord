/*
 * ap_ui.c -- milestone 2: key-driven UI loop on the AP (fw1), called by
 * Main() after the splash. Keys come from the board (keys_adc/keys_power),
 * are debounced by input_scan() and dispatched to the screen stack.
 */
#include "drivers/board.h"
#include "drivers/bt_jieli.h"
#include <stdio.h>
#include <string.h>

#include "drivers/clock.h"
#include "drivers/display.h"
#include "drivers/emmc.h"
#include "drivers/panel_io.h"
#include "drivers/usb_msc.h"
#include "services/sdcard.h"
#include "drivers/usb_cdc.h"
#include "drivers/usb_uac.h"
#include "drivers/usb_probe.h"
#include "drivers/usb_out.h"
#include "drivers/text.h"
#include "services/bootguard.h"
#include "drivers/dac_cs43131.h"
#include "services/fs_target.h"
#include "services/fw_update.h"
#include "services/input.h"
#include "services/config.h"
#include "services/queue.h"
#include "services/log.h"
#include "services/power.h"
#include "services/screenshot.h"
#include "audio/audio.h"
#include "player.h"
#include "services/bootcrumb.h"
#include "services/cpuclk.h"
#include "services/watchdog.h"
#include "services/fs.h"
#include "services/scratch.h"
#include "ui/ui.h"
#include "ui/nav.h"

void ap_bg_poll(void);                /* ap_bg.c: the queued writes, the log, the settings */
void ap_bg_sync(void);
void bt_app_init(void);
void bt_app_tick(void);
void bt_app_external(int streaming);
void ap_usb_dac(void);                /* ap_ovl.c: the USB DAC overlay's gate */
#include "services/lib.h"              /* the library's gates (ap_ovl.c) */
/* the card's block writer: drivers' sd_write when there is one (else the card stays read-only) */
extern int sd_write(uint32_t lba, uint32_t count, const void *buf) __attribute__((weak));

static int s_adc = 1023, s_adc2 = 1023, s_power, s_cable, s_phy_parked;
static int s_power_press_ok;    /* the power key's current press began after boot's was released */
static int s_ui_up;             /* the new UI runs (before it: recovery's screens) */
static int s_pass_held;         /* this pass held a USB disk session: not timed as a slow pass */
static uint32_t s_power_down_ms;     /* time_ms() when the power key went down */
static int s_power_armed;    /* power key seen released since boot */
static uint32_t s_user;              /* user volume start, 0 = unknown */
static char s_emmc[32], s_usb[32] = "plug in";

/* Read-only eMMC probe for the root menu: size and user volume start. */
static int s_storage_failed;          /* our driver could not use the storage the ROM booted from */

static void emmc_probe(void)
{
    uint32_t user;
    int r = emmc_init();

    if (r) {
        s_storage_failed = 1;
        snprintf(s_emmc, sizeof s_emmc, "init %d", r);
        log_printf("emmc: init failed (%d)", r);
        return;
    }
    log_printf("emmc: %s, %d-bit, %u sectors", emmc_kind(), emmc_width(), (unsigned)emmc_sectors());
    if (!board_check(emmc_sectors())) {          /* this build is for the other board */
        snprintf(s_emmc, sizeof s_emmc, "%uM: not a %s unit", (unsigned)(emmc_sectors() >> 11),
                 board_name());
        return;
    }
    r = emmc_layout(&user);
    if (r) {
        s_storage_failed = 1;                    /* (no IDB where the ROM just found one: we read wrong) */
        snprintf(s_emmc, sizeof s_emmc, "%uM lay %d", (unsigned)(emmc_sectors() >> 11), r);
    }
    else
        snprintf(s_emmc, sizeof s_emmc, "%uM u%x", (unsigned)(emmc_sectors() >> 11),
                 (unsigned)(s_user = user));
    if (s_user)
        fs_target_mount_rw(0, "Internal", emmc_read, emmc_write_async, s_user);
    text_fonts_load();                           /* the fonts module from our slot (drivers/text.h) */
}



static const char *usbd_text(void)
{
    static char buf[16];
    snprintf(buf, sizeof buf, "%02x %02x g%x", (unsigned)(usb_msc_diag & 0xFFu),
             (unsigned)(usb_msc_diag >> 8 & 0xFFu), (unsigned)(usb_msc_diag >> 16));
    return buf;
}

static void line(int row, const char *s)
{
    display_draw_text(8, 8 + row * 14, s, 0xFFFF, 0x0000);
}

static void screen(const char *title)
{
    display_fill_rect(0, 0, DISPLAY_W - 1, DISPLAY_H - 1, 0x0000);
    line(0, title);
    display_flush_rows(0, DISPLAY_H - 1);
}

static void update_progress(const char *what, uint32_t done, uint32_t total)
{
    static unsigned last = 101;
    char buf[40];
    unsigned pct = (unsigned)(done / (total / 100u + 1u));

    if (pct == last)
        return;
    last = pct;
    snprintf(buf, sizeof buf, "%-10s %3u%%", what, pct);
    line(2, buf);
    display_flush_rows(8 + 2 * 14, 8 + 2 * 14 + 7);
}

/* The boot's slot check is silent; only a slot that needs rebuilding (an update cut short)
 * gets a screen, at its first progress step. */
static void repair_progress(const char *what, uint32_t done, uint32_t total)
{
    static int shown;

    if (!shown) {
        shown = 1;
        screen("Repairing the firmware slots");
    }
    update_progress(what, done, total);
}

/* A screen that runs its own loop keeps what the main loop serves going: the music, the storage
 * queue and log, the BT module (audit bug 3: the cable question starved the input ring). */
static int s_bt_ready;                    /* bt_app_init() ran (ask() runs at boot too) */

static void pump(void)
{
    (void)player_process();
    ap_bg_poll();
    panel_io_poll();                      /* (the display trial's 5 s count goes on in a prompt) */
    if (s_bt_ready)
        bt_app_tick();
}

/* KEY_SELECT = yes; BACK / power = no; any = 1 with any_key. */
static int ask(int any_key)
{
    input_event_t ev;

    while (keys_power()) {
        pump();
        cpu_wait_tick();
    }
    input_mock_clear();
    for (;;) {
        input_scan(keys_adc(), keys_power());
        while (input_poll(&ev) > 0) {
            static const char *const k_names[] = { "none", "up", "down", "select", "back", "power", "menu", "chord" };

            display_mark_input(time_ms(), 0);   /* key->px: logged when the answer is on the panel */
            log_printf("ui: key %s%s on %s", ev.type >= 0 && ev.type < 8 ? k_names[ev.type] : "?",
                       ev.adc_value == INPUT_ADC_LONG ? " (hold)" : ev.adc_value == INPUT_ADC_REPEAT ? " (repeat)" : "",
                       nav_top() ? nav_top()->name : "-");
            if (any_key || ev.type == KEY_SELECT)
                return 1;
            if (ev.type == KEY_BACK || ev.type == KEY_POWER)
                return 0;
        }
        pump();
        cpu_wait_tick();
    }
}

static void power_off(const char *why);

/* After a USB session: offer an update image (fw_update.c names) if present. */
static void update_prompt(void)
{
    static fw_image_t img;
    char buf[40];
    int r;

    screen("Looking for a firmware update");
    r = fw_update_find(s_user, &img, update_progress);
    if (r == FWU_NONE || (r && !img.name[0]))
        return;
    log_printf("update: %s, %u bytes in %u pieces: %s %d", img.name, (unsigned)img.size, (unsigned)img.n_ext,
               r ? "NOT usable, error" : "valid", r);
    if (r) {
        screen("Firmware update NOT usable");
        snprintf(buf, sizeof buf, "%s: error %d", img.name, r);
        line(2, buf);
        line(4, "press any key");
        display_flush_rows(0, DISPLAY_H - 1);
        ask(1);
        return;
    }
    screen("Firmware update detected");
    snprintf(buf, sizeof buf, "%s  %u KiB", img.name, (unsigned)(img.size >> 10));
    line(2, buf);
    for (int half = 0; half < 2; half++) {
        char *p = buf + snprintf(buf, sizeof buf, half ? "    " : "SHA ");
        for (int i = 0; i < 16; i++)
            p += snprintf(p, 3, "%02x", img.sha256[half * 16 + i]);
        line(3 + half, buf);
    }
    line(6, "Update?  PLAY = yes   BACK = no");
    display_flush_rows(0, DISPLAY_H - 1);
    if (!ask(0))
        return;

    screen("Updating - do NOT power off");
    r = fw_update_install(&img, update_progress);
    log_printf("update: install %d", r);
    if (r) {
        snprintf(buf, sizeof buf, "Update FAILED: error %d", r);
        line(4, buf);
        line(5, "keep power on: unplug/replug USB");
        line(6, "to retry, or debrick via maskrom");
        display_flush_rows(0, DISPLAY_H - 1);
        ask(1);
        return;
    }
    r = fw_update_retire(&img);
    log_printf("update: retire %d", r);
    log_flush("update");
    snprintf(buf, sizeof buf, "Update done (cleanup %d)", r);
    line(4, buf);
    line(5, "press power to turn off, then on");
    display_flush_rows(0, DISPLAY_H - 1);
    ask(1);                              /* no working warm reboot yet */
    power_off(NULL);
}

/* Before anything that ends the session (power off, USB disk): the settings and
 * where the browser and player were, then the log. */
static void save_state(const char *why)
{
    config_save();                       /* (g_cfg.browse / cursor: the browser keeps them, ui/screens.c) */
    log_flush(why);
    ap_bg_sync();                        /* (everything queued on the card before it ends) */
}

/* RECHO's order (docs/RECHO_POWER_CFG.md C.6): stop, save, say so, 500 ms,
 * backlight off, cut the supply. why: shown for a moment first (low battery). */
static void power_off(const char *why)
{
    player_stop();
    log_printf("ui: power off%s%s", why ? ": " : "", why ? why : "");
    save_state("power off");
    if (why) {
        power_screen(1);
        screen(why);
        display_flush_rows(0, DISPLAY_H - 1);
        delay_ms(1500);
    }
    screen("Powering off");
    display_flush_rows(0, DISPLAY_H - 1);
    while (keys_power())
        cpu_wait_tick();
    delay_ms(500);
    backlight_set(0);
    display_clear(0x0000);
    display_flush_rows(0, DISPLAY_H - 1);
    board_power_off();
}

/* MSC session, run on demand from the "USB storage" menu entry (the cable
 * edge opens the CDC console instead): expose the user volume (LUN 0) and
 * the SD card (LUN 1, writable: services/sdcard.c unmounts it first and mounts
 * it again after) until eject/unplug/power key. */
/* Before a USB device mode (disk, console, DAC) or the probe: the core back from host. */
/* USB detect test (menu, last row): which idle state of the USB block lets the PHY's comparators see
 * a cable plugged in after boot. For each candidate (usb_detect_names): unplug, the state applied,
 * plug in within 15 s, unplug within 15 s. Each result to the log and the trail (bit 2n: plug
 * seen, 2n+1: unplug seen). BACK leaves. The ROM's own state is the boot's (never sees one). */
static void usb_out_off(void);

static unsigned s_det_lines;

static int detect_wait(int want, uint32_t ms, uint32_t *took)
{
    uint32_t t0 = time_ms();
    input_event_t ev;

    uint32_t st[3], last = 0x10u;

    while (time_ms() - t0 < ms) {
        usb_cable_state(st);
        if ((st[0] & 0xFu) != last && s_det_lines < 80u) {   /* each comparator change, for the log */
            s_det_lines++;
            last = st[0] & 0xFu;
            log_printf("usbdet: +%u ms e8 %x rst %x gate %x", (unsigned)(time_ms() - t0), (unsigned)last,
                       (unsigned)(st[0] >> 4 & 7u), (unsigned)(st[0] >> 7 & 3u));
        }
        if (usb_cable() == want) {
            *took = time_ms() - t0;
            return 1;
        }
        input_scan(keys_adc(), keys_power());
        if (input_poll(&ev) > 0 && ev.type == KEY_BACK)
            return -1;
        pump();
        cpu_wait_tick();
    }
    *took = ms;
    return 0;
}

void usb_detect_test(void)   /* in the Developer tests overlay (startup/ap_ovl.c) */
{
    uint32_t result = 0, took;
    char buf[48];

    usb_out_off();
    player_stop();
    input_mock_clear();
    for (int n = 0; n < USB_DETECT_STATES; n++) {
        uint32_t st[3];
        int in, out;

        screen("USB detect test");
        snprintf(buf, sizeof buf, "%d/%d: %s", n + 1, USB_DETECT_STATES, usb_detect_names[n]);
        line(1, buf);
        line(3, "unplug the cable");
        display_flush_rows(0, DISPLAY_H - 1);
        if (detect_wait(0, 15000u, &took) < 0)
            break;
        bootcrumb(BC_USBDET + (uint32_t)n);
        usb_detect_state(n);
        line(3, "PLUG IN the cable now");
        line(4, "(15 s; BACK: stop)");
        display_flush_rows(0, DISPLAY_H - 1);
        in = detect_wait(1, 15000u, &took);
        if (in < 0)
            break;
        usb_cable_state(st);
        log_printf("usbdet: %s: plug %s in %u ms, usbst e8 %x rst %x gate %x uoc %08x con6/7 %08x",
                   usb_detect_names[n], in ? "seen" : "NOT seen", (unsigned)took, (unsigned)(st[0] & 0xFu),
                   (unsigned)(st[0] >> 4 & 7u), (unsigned)(st[0] >> 7 & 3u), (unsigned)st[1], (unsigned)st[2]);
        line(3, in ? "seen. UNPLUG now" : "not seen. UNPLUG now");
        display_flush_rows(0, DISPLAY_H - 1);
        out = in ? detect_wait(0, 15000u, &took) : 0;
        if (out < 0)
            break;
        if (in)
            log_printf("usbdet: %s: unplug %s in %u ms", usb_detect_names[n], out ? "seen" : "NOT seen", (unsigned)took);
        result |= (uint32_t)(in > 0) << (2 * n) | (uint32_t)(out > 0) << (2 * n + 1);
        bootcrumb_set(BCK_USBDET_RESULT, result | 0x80000000u);
    }
    usb_detect_state(3);                     /* back to the armed idle (as usb_dwc2_park leaves it) */
    bootcrumb(BC_LOOP);
    log_printf("usbdet: result %02x", (unsigned)result);
    log_flush("usbdet");
    screen("USB detect test");
    snprintf(buf, sizeof buf, "result %02x (see ECHOLOG)", (unsigned)result);
    line(2, buf);
    display_flush_rows(0, DISPLAY_H - 1);
    panel_io_delay_ms(1500);
    ui_invalidate_all();
}

/* Developer > BT listen (test): the module's own output, sending it nothing (bt_listen() in
 * drivers/bt_jieli.h), kept in /BTLISTEN.TXT for tools/bt_listen_decode.py. */
#define LISTEN_MS   4000u
#define LISTEN_PATH "/Internal/BTLISTEN.TXT"

static int listen_save(const bt_listen_t *r)
{
    fs_file_t *f = fs_open_write(LISTEN_PATH);
    char b[100];
    int n, ok;

    if (!f)
        return -1;
    n = snprintf(b, sizeof b, "ReChord BT listen 1\ncore_hz %u\nuart 115200 ms %u bytes %u lsr_errors %u full %u\n",
                 (unsigned)r->core_hz, LISTEN_MS, r->nbytes, r->lsr_errs, r->full & 1u);
    ok = fs_write(f, b, (unsigned long)n) == n;
    for (unsigned i = 0; ok && i < r->nbytes; i++) {
        n = snprintf(b, sizeof b, "u %u %02x %02x\n", (unsigned)r->bytes[2 * i],
                     (unsigned)(r->bytes[2 * i + 1] & 0xFFu), (unsigned)(r->bytes[2 * i + 1] >> 8));
        ok = fs_write(f, b, (unsigned long)n) == n;
    }
    n = snprintf(b, sizeof b, "gpio ms %u level0 %u edges %u full %u\n", LISTEN_MS, r->level0, r->nedges,
                 r->full >> 1 & 1u);
    ok = ok && fs_write(f, b, (unsigned long)n) == n;
    for (unsigned i = 0; ok && i < r->nedges; i += 8) {
        n = snprintf(b, sizeof b, "e");
        for (unsigned k = i; k < i + 8 && k < r->nedges; k++)
            n += snprintf(b + n, sizeof b - (unsigned)n, " %x", (unsigned)r->edges[k]);
        b[n++] = '\n';
        ok = fs_write(f, b, (unsigned long)n) == n;
    }
    ok = ok && fs_write(f, "end\n", 4) == 4;
    return fs_close(f) || !ok ? -1 : 0;
}

void bt_listen_test(void)   /* in the Developer tests overlay (startup/ap_ovl.c) */
{
    bt_listen_t r;
    char txt[48], buf[48];
    unsigned k = 0;
    int saved;

    usb_out_off();
    player_stop();
    screen("BT listen (test)");
    line(1, "listening, nothing sent (9 s)");
    display_flush_rows(0, DISPLAY_H - 1);
    bt_listen(&r, g_scratch, SCRATCH_WORDS, LISTEN_MS);
    saved = listen_save(&r);
    while (k < r.nbytes && k < sizeof txt - 1u) {
        uint8_t c = (uint8_t)r.bytes[2 * k + 1];

        txt[k++] = c >= 0x20 && c < 0x7F ? (char)c : '.';   /* (one log line) */
    }
    txt[k] = 0;
    log_printf("btlisten: uart %u bytes (%u line errors), the first at %u ms: '%.40s'%s", r.nbytes, r.lsr_errs,
               r.nbytes ? (unsigned)(r.bytes[0] / 1000u) : 0u, txt, r.full & 1u ? " (full)" : "");
    log_printf("btlisten: gpio %u edges, the first at %u us, level %u at power-on%s; " LISTEN_PATH " %s", r.nedges,
               r.nedges ? (unsigned)(r.edges[0] / (r.core_hz / 1000000u)) : 0u, r.level0,
               r.full & 2u ? " (full)" : "", saved ? "NOT saved" : "saved");
    log_flush("btlisten");
    snprintf(buf, sizeof buf, "UART: %u bytes, %u errors", r.nbytes, r.lsr_errs);
    line(2, buf);
    snprintf(buf, sizeof buf, "%.40s", txt);
    line(3, buf);
    snprintf(buf, sizeof buf, "GPIO: %u edges", r.nedges);
    line(4, buf);
    line(5, saved ? "BTLISTEN.TXT NOT saved" : "saved BTLISTEN.TXT");
    line(7, "the module is off; any key");
    display_flush_rows(0, DISPLAY_H - 1);
    ask(1);
    ui_invalidate_all();
}

/* drivers/dac_cs43131.c: each dac_init step in the trail (a reset there names it next boot) */
void dac_trace(int step)
{
    bootcrumb(BC_DAC_STEP + (uint32_t)step);
}

static void usb_out_off(void)
{
    if (usb_out_active()) {
        usb_out_stop();
        audio_set_output(0);
    }
}

static void usb_session(void)
{
    const usb_lun_t luns[2] = {
        { emmc_read, emmc_write, s_user, emmc_sectors() - s_user },
        { sd_read, sd_write, 0, sd_sectors() },
    };
    uint32_t rd, wr;
    int r;

    usb_out_off();

    if (!s_user) {      /* layout unknown or wrong board: never export LBA 0.. */
        snprintf(s_usb, sizeof s_usb, "no user volume");
        return;
    }
    player_stop();                       /* the PC may rewrite the files we stream */
    ap_lib_volume(0, LIB_EV_SUSPEND);    /* the library's files closed: the computer's now */
    save_state("usb");
    if (s_ui_up) {                       /* the disk session's page (the session holds the loop) */
        extern const page_t pg_usbdisk;

        nav_push(&pg_usbdisk, 0);
        nav_tick();
        ui_render();
    } else {                             /* (recovery: as it always was) */
        screen("USB mass storage");
        line(1, "eject on the PC or press power");
        display_flush_rows(0, DISPLAY_H - 1);
    }
    sdcard_usb_begin();
    r = usb_msc_run(luns, 2, keys_power, &rd, &wr);
    if (s_ui_up)
        nav_pop();
    fs_target_mount_rw(0, "Internal", emmc_read, emmc_write_async, s_user);   /* host may have written */
    sdcard_usb_end();
    ap_lib_volume(0, wr ? LIB_EV_WROTE : LIB_EV_RESUME);
    snprintf(s_usb, sizeof s_usb, "x%d r%uM w%uM", r, (unsigned)(rd >> 11), (unsigned)(wr >> 11));
    log_printf("usb: disk session ended: exit %d, read %u MB, written %u MB, cable %d", r,
               (unsigned)(rd >> 11), (unsigned)(wr >> 11), usb_cable());
    if (config_reload_if_changed()) {    /* the PC's edit wins over our copy (services/config.c) */
        queue_set_mode(g_cfg.play);
        audio_set_volume_db(g_cfg.volume == CFG_VOLUME_MUTE ? -90 : g_cfg.volume);
        theme_load_index(g_cfg.theme);
        log_printf("cfg: changed on the PC, read again: play %s, volume %d (bt=, display=: at the next start)",
                   queue_mode_name(g_cfg.play), g_cfg.volume);
    }
    s_pass_held = 1;                     /* (the session's time is not a slow pass of the loop) */
    while (keys_power())                 /* don't leak the exit press */
        cpu_wait_tick();
    /* Scan for an update on ANY session exit the screen advertises:
     * eject (LoEj), unplug (VBUS gone), AND the power key — the screen
     * says "eject on the PC or press power", and the reference updater
     * scans after "usb: exit key pressed" too (ECHOLOG). With no image
     * present fw_update_find returns FWU_NONE and we return seamlessly. */
    if (r == USB_EXIT_EJECT || r == USB_EXIT_UNPLUG || r == USB_EXIT_USER)
        update_prompt();
    ui_invalidate_all();
}

/* "200M 1.5%": core A clock and its busy share over the last second */
static const char *cpu_text(void)
{
    static char buf[24];
    unsigned pm = cpu_load_permille();

    snprintf(buf, sizeof buf, "%uM %u.%u%%", (unsigned)(clock_core_hz() / 1000000u), pm / 10u, pm % 10u);
    return buf;
}

static const char *keys_text(void)
{
    static char buf[24];
    if (s_adc2 >= 0)
        snprintf(buf, sizeof buf, "adc %d/%d pwr %d", s_adc, s_adc2, s_power);
    else
        snprintf(buf, sizeof buf, "adc %d pwr %d", s_adc, s_power);
    return buf;
}

/* Feed `seconds` of the 64-sample sine at 44.1 kHz through the polled I2S
 * FIFO. Returns dac_i2s_feed's rc (DAC_ERR_TIMEOUT = FIFO stopped
 * draining = no I2S clock); *frames_out = frames actually queued. */
static int tone_burst(const int16_t *sine64, int seconds, int16_t *chunk,
                      int *frames_out)
{
    int i, n = 0, rc = DAC_OK;

    dac_i2s_start();
    for (;;) {
        if (keys_power())
            break;
        for (i = 0; i < 256; i++) {
            int idx = (n + i) & 63;
            chunk[2 * i] = sine64[idx];
            chunk[2 * i + 1] = sine64[idx];
        }
        rc = dac_i2s_feed(chunk, 256);
        if (rc != DAC_OK)
            break;
        n += 256;
        if (n > 44100 * seconds)
            break;
    }
    dac_i2s_stop();
    *frames_out = n;
    return rc;
}

/* DAC bring-up test (Exp 1/2 of docs/rewrite/dac.md §7): run the CS43131
 * init (recovered from the reference fw1), show the breadcrumb + I2C scan
 * + HP status, then feed a 1 kHz tone over I2S1 until POWER (or 3 s). */
void dac_test_run(void)   /* in the Developer tests overlay (startup/ap_ovl.c) */
{
    /* sin(2*pi*i/64) * 9000, i = 0..63 */
    static const int16_t sine64[64] = {
           0,  785, 1567, 2340, 3096, 3827, 4530, 5200,
        5831, 6417, 6954, 7439, 7865, 8226, 8520, 8742,
        8889, 8960, 8952, 8867, 8703, 8462, 8148, 7762,
        7307, 6788, 6209, 5575, 4891, 4161, 3390, 2583,
        1746,  885,     0,  -885,-1746,-2583,-3390,-4161,
       -4891,-5575,-6209,-6788,-7307,-7762,-8148,-8462,
       -8703,-8867,-8952,-8960,-8889,-8742,-8520,-8226,
       -7865,-7439,-6954,-6417,-5831,-5200,-4530,-3827,
    };
    int16_t chunk[512];
    uint8_t hp[2];
    char buf[40];
    int rc, i, n = 0;

    screen("DAC test");
    line(1, "bring-up 44100 Hz...");
    display_flush_rows(0, DISPLAY_H - 1);
    rc = dac_init(44100);

    dac_get_hp_status(hp);
    screen("DAC test result");
    snprintf(buf, sizeof buf, "rc %d step %d", rc, dac_last_step());
    line(1, buf);
    snprintf(buf, sizeof buf, "scan A%d%d B%d%d C%d%d",
             dac_scan_result(0, 0), dac_scan_result(0, 1),
             dac_scan_result(1, 0), dac_scan_result(1, 1),
             dac_scan_result(2, 0), dac_scan_result(2, 1));
    line(2, buf);
    snprintf(buf, sizeof buf, "hp %02x %02x", hp[0], hp[1]);
    line(3, buf);

    /* Register readback (chip 0) to diagnose "no sound": confirms which
     * CS43131 regs actually took vs our write sequence (POWER, unmute,
     * volume, output routing, FUN_03055ecc channel/gain). */
    {
        uint8_t v0, v1;
        struct { uint32_t map; const char *name; } regs[] = {
            { 0x000B0000, "PWR" }, { 0x00010009, "MUT" }, { 0x0001000B, "VOL" },
            { 0x0005000A, "CH0" }, { 0x0005000B, "CH1" }, { 0x00040010, "OUT" },
        };
        int ln = 4;
        for (i = 0; i < 6; i += 2) {
            v0 = v1 = 0xFF;
            dac_read_reg(0, regs[i].map, &v0);
            dac_read_reg(0, regs[i + 1].map, &v1);
            snprintf(buf, sizeof buf, "%s%02x %s%02x",
                     regs[i].name, v0, regs[i + 1].name, v1);
            line(ln++, buf);
        }
    }
    display_flush_rows(0, DISPLAY_H - 1);
    ask(1);                          /* read the register dump first */

    if (rc != DAC_OK) {
        screen("DAC test");
        line(1, "bring-up FAILED - any key");
        display_flush_rows(0, DISPLAY_H - 1);
        ask(1);
        return;
    }

    screen("DAC test tone");
    line(1, "tone: 1 kHz, POWER = stop");
    display_flush_rows(0, DISPLAY_H - 1);
    rc = tone_burst(sine64, 3, chunk, &n);

    /* The three observables that split the no-sound hypotheses:
     * fifo 00-00 = the TXDR writes vanish; fifo 00-1x = SCLK runs and the
     * fault is DAC-side (MCLK/PLL); clk/pdn = what the chip latched. */
    {
        unsigned lo, hi;
        uint8_t clk = 0xFF, pdn = 0xFF;

        dac_i2s_fifo_range(&lo, &hi);
        dac_read_reg(0, 0x00010006, &clk);
        dac_read_reg(0, 0x00020000, &pdn);
        screen("DAC test tone result");
        snprintf(buf, sizeof buf, "tone rc %d f%d", rc, n);
        line(1, buf);
        snprintf(buf, sizeof buf, "fifo %02x-%02x", lo, hi);
        line(2, buf);
        snprintf(buf, sizeof buf, "clk %02x pdn %02x", clk, pdn);
        line(3, buf);
        line(4, "any key = end");
        display_flush_rows(0, DISPLAY_H - 1);
        ask(1);
    }

    /* Leave the hardware in the stock STOPPED state (FUN_03055F80 stop +
     * FUN_030564d8 close: hw mute + amp off + I2S1 reset held) — a live
     * amp path with I2S stopped is continuous noise (DFIU 60609d7f).
     * (The RCO-clock diagnostic tone is gone: MCLK_SRC_SEL=10 routes the
     * CS43131's uncalibrated RC standby oscillator into the audio path,
     * so it is BY DESIGN noisy/off-pitch — a probe, not a test mode.) */
    dac_output_off();
}

/* ---- USB CDC-ACM debug console ----------------------------------------- */
static int cdc_hex(const char *p, uint32_t *out)
{
    uint32_t v = 0; int n = 0;
    for (; *p == ' '; p++)
        ;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    for (; *p; p++) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
        else break;
        v = v * 16 + (uint32_t)d;
        n++;
    }
    *out = v;
    return n;
}

static void cdc_reply(const char *fmt, uint32_t a, uint32_t b, uint32_t c)
{
    char out[64];
    snprintf(out, sizeof out, fmt, a, b, c);
    usb_cdc_puts(out);
}

static void cdc_exec(char *ln)
{
    while (*ln == ' ')
        ln++;
    if (ln[0] == 'r') {                     /* r <map>  -> read chip 0     */
        uint32_t map; uint8_t v = 0xFF;
        cdc_hex(ln + 1, &map);
        if (dac_read_reg(0, map, &v) == DAC_OK)
            cdc_reply("0x%06x = 0x%02x\r\n", map, v, 0);
        else
            cdc_reply("0x%06x READ FAIL\r\n", map, 0, 0);
    } else if (ln[0] == 'w') {              /* w <map> <val> -> write both */
        uint32_t map, val;
        int n = cdc_hex(ln + 1, &map);
        cdc_hex(ln + 1 + n, &val);
        dac_write_reg(0, map, (uint8_t)val);
        cdc_reply("w 0x%06x <- 0x%02x\r\n", map, val, 0);
    } else if (ln[0] == 's') {              /* s -> I2C scan (3 candidates)*/
        uint32_t a, b, c;
        a = (dac_scan_result(0, 0) << 1) | dac_scan_result(0, 1);
        b = (dac_scan_result(1, 0) << 1) | dac_scan_result(1, 1);
        c = (dac_scan_result(2, 0) << 1) | dac_scan_result(2, 1);
        cdc_reply("scan A%x B%x C%x\r\n", (a << 4) | b, c, 0);
    } else if (ln[0] == 'q') {
        usb_cdc_request_exit();
    } else {
        usb_cdc_puts("cmds: r<map> w<map><val> s(scan) q(quit)\r\n");
    }
}

/* Cable plugged in: default USB mode = the CDC-ACM console (the "USB
 * storage" menu entry runs MSC on demand). Returns on POWER or unplug,
 * then tears the core down so the next mode starts from a host-visible
 * disconnect instead of stale endpoint state. */
void usb_console_run(void)   /* in the Developer tests overlay (startup/ap_ovl.c) */
{
    char ln[64];
    unsigned len = 0, frames = 0;

    usb_out_off();
    screen("USB diag");
    line(1, "enumerating... POWER=exit");
    display_flush_rows(0, DISPLAY_H - 1);
    dac_init(44100);                       /* I2C0 up so r/w cmds work      */
    usb_cdc_init();
    for (;;) {
        int c;
        usb_cdc_poll();
        if (keys_power() || usb_cable_gone())
            break;
        /* Live enumeration state, refreshed every ~256 polls. */
        if ((frames++ & 0xFFu) == 0u) {
            const struct usb_cdc_diag_t *d = usb_cdc_diag();
            snprintf(ln, sizeof ln, "rst %u set %u cfg %u", d->resets, d->setups, d->configured ? 1u : 0u);
            line(2, ln);
            snprintf(ln, sizeof ln, "dev%u cfg%u str%u adr%u", d->desc_dev, d->desc_cfg, d->desc_str, d->set_addr);
            line(3, ln);
            snprintf(ln, sizeof ln, "cls%u oth%u xfer%u", d->class_req, d->other_req, d->xfer_err);
            line(4, ln);
            snprintf(ln, sizeof ln, "s %02x %02x %02x %02x %02x %02x %02x %02x", d->last_setup[0], d->last_setup[1], d->last_setup[2], d->last_setup[3], d->last_setup[4], d->last_setup[5], d->last_setup[6], d->last_setup[7]);
            line(5, ln);
            snprintf(ln, sizeof ln, "GINT %08x DSTS %03x", d->gintsts, d->dsts & 0xFFFu);
            line(6, ln);
            display_flush_rows(0, DISPLAY_H - 1);
        }
        c = usb_cdc_getc();
        if (c < 0)
            continue;
        if (c == '\r' || c == '\n') {
            ln[len] = 0;
            if (len) {
                cdc_exec(ln);
                len = 0;
            }
        } else if (len < sizeof ln - 1u) {
            ln[len++] = (char)c;
        }
    }
    usb_cdc_request_exit();
    usb_dwc2_stop();               /* host-visible disconnect, no stale EPs */
}

/* MSC on demand (was the automatic VBUS action; the console owns that now). */
static void usb_storage_run(void)
{
    if (!s_user) {
        screen("USB storage");
        line(1, "no eMMC user area");
        display_flush_rows(0, DISPLAY_H - 1);
        return;
    }
    usb_session();
}

/* USB-C probe: which of the player's own pins puts voltage on VBUS (usb_probe.h), and
 * whether a USB-C earphone plugged in then connects. A row per step: the comparators'
 * trip masks over their eight reference codes (v VBUS valid, b B-session, a A-session,
 * e session end); "dev" = a device pulled D+ up while the player was host. */
static void usb_probe_screen(int step, const char *name)
{
    char buf[40];

    snprintf(buf, sizeof buf, "%d/%d %s", step + 1, USB_PROBE_STEPS, name);
    line(2, buf);
    display_flush_rows(0, DISPLAY_H - 1);
}

void usb_probe_menu(void)   /* in the Developer tests overlay (startup/ap_ovl.c) */
{
    static usb_probe_step_t st[USB_PROBE_STEPS];
    char buf[64], up[40] = "", dev[40] = "";
    input_event_t ev;
    int n;

    usb_out_off();
    player_stop();
    screen("USB-C probe");
    line(1, "no cable; an earphone may be in");
    display_flush_rows(0, DISPLAY_H - 1);
    n = usb_probe_run(st, usb_probe_screen);
    screen("USB-C probe");
    if (n < 0) {
        line(2, "unplug the cable first");
        log_printf("probe: VBUS present, not run");
    }
    for (int i = 0; i < n; i++) {
        const usb_probe_step_t *s = &st[i];

        snprintf(buf, sizeof buf, "%-12.12s v%02x b%02x a%02x e%02x%s", s->name, s->vbus, s->bsess, s->asess,
                 s->sessend, s->host < 0 ? "" : s->host ? " dev" : " -");
        line(1 + i, buf);
        log_printf("probe: %s: vbus %02x bsess %02x asess %02x sessend %02x, adc %d %d %d %d%s%s", s->name,
                   s->vbus, s->bsess, s->asess, s->sessend, s->adc[0], s->adc[1], s->adc[2], s->adc[3],
                   s->host < 0 ? "" : s->host ? ", device connected, lines " : ", no device, lines ",
                   s->host < 0 ? "" : s->linestate == 1 ? "J" : s->linestate == 2 ? "K" : s->linestate ? "SE1" : "SE0");
        if (i > 0 && i < n - 1 && (s->vbus != st[0].vbus || s->bsess != st[0].bsess || s->asess != st[0].asess ||
                                   s->sessend != st[0].sessend))
            snprintf(up + strlen(up), sizeof up - strlen(up), "%s%s", up[0] ? ", " : "", s->name);
        if (s->host > 0)
            snprintf(dev + strlen(dev), sizeof dev - strlen(dev), "%s%s", dev[0] ? ", " : "", s->name);
    }
    if (n > 0) {
        log_printf("probe: PHY CON0-11 %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x, UOC %04x %04x %04x",
                   (unsigned)usb_probe_phy[0], (unsigned)usb_probe_phy[1], (unsigned)usb_probe_phy[2],
                   (unsigned)usb_probe_phy[3], (unsigned)usb_probe_phy[4], (unsigned)usb_probe_phy[5],
                   (unsigned)usb_probe_phy[6], (unsigned)usb_probe_phy[7], (unsigned)usb_probe_phy[8],
                   (unsigned)usb_probe_phy[9], (unsigned)usb_probe_phy[10], (unsigned)usb_probe_phy[11],
                   (unsigned)usb_probe_phy[12], (unsigned)usb_probe_phy[13], (unsigned)usb_probe_phy[14]);
        snprintf(buf, sizeof buf, "VBUS up: %s", up[0] ? up : "none");
        line(1 + n, buf);
        snprintf(buf, sizeof buf, "device: %s", dev[0] ? dev : "none");
        line(2 + n, buf);
        log_printf("probe: VBUS raised by: %s; a device connected in: %s", up[0] ? up : "none", dev[0] ? dev : "none");
    }
    display_flush_rows(0, DISPLAY_H - 1);
    log_flush("probe");
    for (;;) {                               /* any key */
        input_scan(keys_adc(), keys_power());
        if (input_poll(&ev) > 0 && ev.adc_value != INPUT_ADC_REPEAT)
            break;
        pump();
        cpu_wait_tick();
    }
    ui_invalidate_all();
}

#define RC_BUILD "m8"

/* The USB DAC's screen is Now Playing's template (ui/screens.c pg_dac): usb_uac_run() reports
 * about once a second into g_uac, the page repaints what changed. */
extern usb_uac_status_t g_uac;
extern int g_uac_leave;

static void uac_status(const usb_uac_status_t *st)
{
    g_uac = *st;
}

/* The DAC session's supply (Settings > DAC power): s_dac_sw = the pins were switched
 * (not the 0x66666666 board); s_dac_vbus = power only, now running from VBUS. */
static int s_dac_sw, s_dac_vbus;

/* Power only (stock's "Power only, no charging"): from VBUS while the battery is below
 * 3.95 V, from the battery again above 4.05 V (stock: 4.0 V both ways, no hysteresis). */
static void dac_power_only(void)
{
    unsigned mv = power_battery()->mv;

    if (!s_dac_sw || g_cfg.dac_charge != CFG_DAC_POWER_ONLY || !mv)
        return;
    if (!s_dac_vbus && mv < 3950u) {
        s_dac_vbus = board_usb_supply(BOARD_SUPPLY_VBUS) == 0;
        log_printf("power: DAC power only: %u mV, from USB", mv);
    } else if (s_dac_vbus && mv > 4050u) {
        s_dac_vbus = !(board_usb_supply(BOARD_SUPPLY_BATTERY) == 0);
        log_printf("power: DAC power only: %u mV, from the battery", mv);
    }
}

/* Every tick of the DAC session: the keys through the UI (VOL the player's volume on top of the
 * host's, PLAY taps the host's transport, BACK ends it; the screen off: VOL and PLAY blind), the
 * screen, the power, the BT module. Nonzero = leave. */
static int uac_tick(void)
{
    input_event_t ev;

    input_scan(keys_adc(), keys_power());   /* (the watchdog: a loop that scans the keys is alive) */
    input_set_repeat(nav_volume_keys() ? INPUT_REPEAT_VOLUME : INPUT_REPEAT_LIST);
    while (input_next(&ev) > 0)
        nav_key(&ev);
    if (power_poll(1, POWER_SESSION_DAC) == POWER_OFF_BATTERY)   /* (no auto power off: the cable is in) */
        g_uac_leave = 1;
    dac_power_only();
    bt_app_external(usb_uac_streaming());   /* the BT module served here too: the host's audio to */
    bt_app_tick();                          /* the earphones, their keys to the host */
    if (power_screen_on()) {
        nav_tick();
        ui_render();
    }
    return g_uac_leave;
}

void usb_dac_session(void)   /* in the USB DAC overlay (startup/ap_ovl.c) */
{
    int r;

    if (!usb_cable()) {                  /* from the menu without a cable: nothing to wait for (on the
                                          * device this path hung with no way out, 2026-10-16) */
        log_printf("ui: USB DAC: no cable");
        nav_toast(ICON_USB, TOAST_WARN, "Connect the cable first");
        return;
    }
    bootcrumb(BC_DAC);                   /* the session's steps, on the eMMC as they start */
    usb_out_off();
    player_stop();
    save_state("usb dac");
    power_screen(1);
    while (keys_power())                 /* the press that got us here (or switched us on) */
        cpu_wait_tick();
    input_mock_clear();
    memset(&g_uac, 0, sizeof g_uac);
    g_uac_leave = 0;
    /* Settings > DAC power, before the USB core connects. battery (stock's default): no
     * charging, no supply from VBUS, the phone or computer gives the player nothing but the
     * audio; power only: no charging, VBUS runs the player while its battery is low
     * (dac_power_only()); charge: as with any cable. */
    s_dac_vbus = 0;
    s_dac_sw = g_cfg.dac_charge != CFG_DAC_CHARGE && board_usb_supply(BOARD_SUPPLY_BATTERY) == 0;
    usb_uac_self_powered(s_dac_sw && g_cfg.dac_charge == CFG_DAC_BATTERY);
    power_usb_supply_off(s_dac_sw);
    log_printf("power: DAC supply %s", !s_dac_sw ? (g_cfg.dac_charge == CFG_DAC_CHARGE ? "USB (charging)" :
               "USB (charging: this board cannot switch it off)") : g_cfg.dac_charge == CFG_DAC_BATTERY ?
               "battery (not charging, nothing drawn from USB)" : "power only (not charging, USB below 3.95 V)");
    (void)power_poll(1, POWER_SESSION_DAC);  /* its clock before the USB core runs on it */
    dac_power_only();
    nav_push(&pg_dac, 0);
    ui_render();
    r = usb_uac_run(uac_tick, uac_status);
    if (nav_find(&pg_dac) > 0)
        nav_pop_to(nav_find(&pg_dac));
    bt_app_external(-1);
    log_printf("ui: USB DAC ended (%d)", r);
    bootcrumb(BC_LOOP);
    if (s_dac_sw) {
        s_dac_sw = s_dac_vbus = 0;
        board_usb_supply(BOARD_SUPPLY_NORMAL);   /* charging again */
        power_usb_supply_off(0);
        log_printf("power: USB supply normal");
    }
    while (keys_power())                 /* don't leak the exit press */
        cpu_wait_tick();
    power_activity();
    ui_invalidate_all();
}

/* RECHO's "On USB connect" (cfg usb): ask, disk, DAC, charge only. The USB DAC
 * is not there yet: it charges. The CDC console stays in the main menu. */
/* The cable comparators woken: the USB-C probe's own sequence, silently. Every cable plugged in
 * after boot that the device ever noticed (8 of 8, rechord3..rechord8, each a real host that then
 * enumerated) came after a USB-C probe; with none before it, none ever was (the boot park, the 1 s
 * arm, all twelve detect-test states, after sessions). The probe's PHY on/off alone does not do
 * it (rechord7's "probe cycle" state stayed blind), and it lasts through the block held in reset
 * (rechord8: seen with rst 7 gate 3), so it is one of its other steps (host mode with port power,
 * the comparator reference sweep, the GPIO2.1/2.0/0.14 and UOC2 charge/discharge toggles).
 * About 1.4 s with the main loop stopped (keys pressed then are lost), so after an unplug it
 * waits for 5 s without a key; its PHY bring-up cuts the DAC's supply, so never while the
 * player plays (or is paused). */
static uint32_t s_wake_due;              /* time_ms() from which usb_wake() runs (0: not due) */
static int s_cable_seen;                 /* a cable was in since boot (before: the boot's own wake) */

static void usb_wake(const char *why)
{
    static usb_probe_step_t st[USB_PROBE_STEPS];
    uint32_t t0 = time_ms(), w[3];
    int n;

    s_wake_due = 0;
    if (usb_out_active())
        return;
    n = usb_probe_run(st, 0);
    usb_cable_state(w);
    log_printf("usb: comparators woken (%s): the probe's sequence, %d steps, %u ms, e8 %x", why, n,
               (unsigned)(time_ms() - t0), (unsigned)(w[0] & 0xFu));
}

static int s_cable_q;                    /* the cable question is up: the answer comes from the UI */
static int s_cable_answer;               /* ap_cable_choice(): 1 + the choice */

/* The mode for a cable: 0 USB disk, 1 USB DAC, 2 charge only. */
static void cable_mode(int c)
{
    if (c == 1 && usb_cable()) {
        ap_usb_dac();                    /* (an overlay: through its gate, startup/ap_ovl.c) */
    } else if (c == 0) {
        power_screen(1);
        usb_session();
    }
    power_activity();
}

void ap_cable_choice(int c)
{
    s_cable_answer = 1 + c;
}

/* 1: the question is up (a screen: the music and the UI go on; its answer: ap_cable_choice()). */
static int cable_connected(void)
{
    log_printf("usb: cable in (%s)", g_cfg.usb == CFG_USB_ASK ? "ask" : g_cfg.usb == CFG_USB_DISK ? "disk" :
               g_cfg.usb == CFG_USB_DAC ? "dac" : "charge");
    if (g_cfg.usb == CFG_USB_ASK) {
        extern const page_t pg_cable;

        power_screen(1);
        power_activity();
        s_cable_q = 1;
        nav_push(&pg_cable, 0);
        return 1;
    }
    cable_mode(g_cfg.usb == CFG_USB_DISK ? 0 : g_cfg.usb == CFG_USB_DAC ? 1 : 2);
    return 0;
}

/* USB-C earphones (Settings): while there is no cable, the player is a USB host waiting for
 * one; a USB audio device that enumerates takes the music (what was playing starts again
 * there), unplugged it goes back to the jack. While host, VBUS is the player's own: no cable
 * dialog, no charging shown. */
static void usb_out_tick(void)
{
    static int last = USB_OUT_OFF;
    int st, playing;

    if (g_cfg.usb_out && !usb_out_active() && !usb_cable() && !s_cable)
        usb_out_start(g_cfg.usb_vbus, g_cfg.usb_out_ma);
    else if (!g_cfg.usb_out && usb_out_active())
        usb_out_stop();
    st = usb_out_poll();
    power_usb_supply_off(usb_out_active());
    if (st == last)
        return;
    playing = player_get_state() == PLAYER_PLAYING;
    if (st == USB_OUT_READY) {
        const usb_out_status_t *u = usb_out_status();

        log_printf("ui: USB-C out: '%s' (UAC%d)", u->name, u->uac);
        if (audio_set_output(1) && playing && player_play() != PLAYER_OK)
            log_printf("ui: USB-C out: the track did not start");
        audio_set_volume_db(audio_get_volume_db());   /* the device's volume to the player's */
    } else if (last == USB_OUT_READY) {
        log_printf("ui: USB-C out: gone, back to the jack");
        audio_set_output(0);                      /* (playing stopped: nothing plays aloud by surprise) */
    } else if (st == USB_OUT_UNSUPPORTED || st == USB_OUT_ERROR) {
        log_printf("ui: USB-C out: %s", usb_out_status()->reason);
    }
    last = st;
    ui_invalidate_all();
}

/* The way back that needs none of our drivers: the boot ROM's USB loader (board_reboot_rockusb()).
 * It keeps the player powered only from USB, so with no cable in it asks for one first. */
static void rockusb_flow(const char *why)
{
    log_printf("rockusb: %s", why);
    log_flush("rockusb");                        /* (if the storage lets us) */
    ap_bg_sync();
    power_screen(1);
    for (;;) {
        screen("USB recovery (boot ROM)");
        line(1, why);
        line(3, "On a computer: rkdeveloptool");
        line(4, "  ld, then wl 0 MINIV390.IMG, rd");
        line(6, usb_cable() ? "rebooting into USB mode..." : "connect USB to a computer");
        line(7, usb_cable() ? "still here? unplug, RST, then" : "POWER: switch off instead");
        line(8, usb_cable() ? "hold all keys + plug in USB" : "");
        display_flush_rows(0, DISPLAY_H - 1);
        if (usb_cable())
            break;
        if (keys_power()) {
            while (keys_power())
                cpu_wait_tick();
            power_off(NULL);
        }
        cpu_wait_tick();
    }
    delay_ms(2000);                              /* (time to read it) */
    board_reboot_rockusb();
}

/* The key held at power-on, decided once and first (before the storage driver, slot repair or
 * anything slow): KEY_DOWN (VOL-) = the boot ROM's USB loader, KEY_UP (VOL+) = Recovery. */
static int s_boot_key = KEY_NONE;

static void boot_key_sample(void)
{
    uint32_t t0 = time_ms(), up = 0, down = 0;
    int lo = 1023, hi = 0;

    for (uint32_t idle = 0; time_ms() - t0 < 400u;) {   /* (the ADC's first readings, then 300 ms of one key) */
        int a = keys_adc();

        lo = a < lo ? a : lo;
        hi = a > hi ? a : hi;
        /* nothing held: a finger on VOL+/VOL- at power-on reads low from the first sample, so
         * 50 ms of an idle ladder ends the window (it held the first frame 400 ms on every boot) */
        if (a > 600 && ++idle >= 5u && !up && !down)
            break;
        up = a >= 70 && a <= 183 ? up + 1u : 0u;
        down = a >= 0 && a <= 69 ? down + 1u : 0u;
        if (up >= 30u || down >= 30u) {
            s_boot_key = up >= 30u ? KEY_UP : KEY_DOWN;
            break;
        }
        cpu_wait_tick();
    }
    log_printf("keys: at power-on the ladder read %d..%d", lo, hi);
    if (s_boot_key != KEY_DOWN)
        return;
    /* VOL- leaves our firmware altogether, so a ladder that only reads low (a fault, not a
     * finger) must not do it: the key has to stay down 2 s more, the screen counting, and
     * letting go starts normally */
    power_screen(1);
    t0 = time_ms();
    up = 0;
    for (uint32_t shown = 0;;) {
        uint32_t el = time_ms() - t0, left = el >= 2000u ? 0u : (2000u - el + 999u) / 1000u;

        if (keys_adc() > 69) {
            if (++up >= 5u) {                    /* (50 ms of not VOL-: let go) */
                log_printf("keys: VOL- let go after %u ms: starting normally", (unsigned)el);
                s_boot_key = KEY_NONE;
                return;
            }
        } else {
            up = 0;
        }
        if (!left)
            return;
        if (left != shown) {
            shown = left;
            screen("VOL- held: USB recovery");
            line(2, "keep holding VOL- to go on");
            line(3, left == 2u ? "  2" : "  1");
            line(5, "let go to start normally");
            display_flush_rows(0, DISPLAY_H - 1);
        }
        cpu_wait_tick();
    }
}

/* Recovery (services/bootguard.h): right after the storage is up and before anything else
 * runs, when the last boot ended in a fault or VOL+ is held at power-on. Only the pieces
 * every way back needs: the updater and USB disk. */
static void recovery_menu(void)
{
    bootguard_rec_t rec;
    char why[48];
    input_event_t ev;

    if (bootguard_crashed(&rec))
        snprintf(why, sizeof why, "after a fault (x%u) at %08x", (unsigned)rec.count, (unsigned)rec.pc);
    else if (s_boot_key == KEY_UP)
        snprintf(why, sizeof why, "VOL+ held at power-on");
    else
        return;
    log_printf("recovery: %s (lr %08x); storage %s", why, bootguard_crashed(&rec) ? (unsigned)rec.lr : 0u, s_emmc);
    log_flush("recovery");
    for (;;) {
        int k = KEY_NONE;

        power_screen(1);
        screen("Recovery");
        line(1, why);
        line(2, s_user ? "storage ok" : s_emmc[0] ? s_emmc : "storage: none");
        line(4, "PLAY  install an update (MINIV*.IMG)");
        line(5, "MENU  USB disk: copy the image");
        line(6, "BACK  start normally");
        line(7, "VOL-  USB recovery for a computer");
        line(8, "POWER off");
        display_flush_rows(0, DISPLAY_H - 1);
        while (keys_power())
            cpu_wait_tick();
        input_mock_clear();
        while (k == KEY_NONE) {
            input_scan(keys_adc(), keys_power());
            while (input_poll(&ev) > 0)
                if (ev.adc_value != INPUT_ADC_REPEAT)
                    k = ev.type;
            cpu_wait_tick();
        }
        log_printf("recovery: key %d", k);
        if (k == KEY_SELECT && s_user) {
            update_prompt();
        } else if (k == KEY_MENU && s_user) {
            screen("Recovery: USB disk");
            line(2, "connect the cable (BACK: cancel)");
            display_flush_rows(0, DISPLAY_H - 1);
            while (!usb_cable() && input_map_adc(keys_adc()) != KEY_BACK)
                cpu_wait_tick();
            if (usb_cable())
                usb_session();                   /* (it offers the update when the host lets go) */
        } else if (k == KEY_BACK) {
            bootguard_clear();
            log_printf("recovery: start normally");
            return;
        } else if (k == KEY_DOWN) {
            rockusb_flow("from the recovery menu");
        } else if (k == KEY_POWER) {
            power_off(NULL);
        }
    }
}

/* ---- what the screens (ui/screens.c, ui/nav.c) ask of this file ------------------------------ */

static const ui_screen_t k_rc = { "rc", NULL, NULL, nav_draw, NULL };
static void (*s_later)(void);

void ap_later(void (*fn)(void)) { s_later = fn; }
int ap_cable(void) { return usb_cable() && !usb_out_active(); }
int ap_power_ok(void) { return s_power_press_ok; }
void ap_usb_disk(void) { usb_storage_run(); }
/* ap_usb_dac, ap_dac_test, ap_usb_console, ap_usb_probe, ap_usb_detect, ap_bt_listen, ap_te_probe:
 * the gates of the overlays that hold them (startup/ap_ovl.c) */
void te_probe_test(void) { panel_te_probe(); log_flush("te"); }   /* Developer > Panel TE (test): the log */
void ap_power_off(void) { power_off(NULL); }

void ap_update(void)
{
    if (s_user)
        update_prompt();
}

void ap_screen(int on)
{
    power_screen(on);
    if (!on)
        config_save();                   /* RECHO saves at screen off */
    else
        ui_invalidate_all();
}

void ap_brightness(int d)
{
    int b = g_cfg.brightness + d;

    g_cfg.brightness = (uint8_t)(b < 1 ? 1 : b > 8 ? 8 : b);
    power_brightness_changed();
    config_save_later();
}

const char *ap_keys_text(void)
{
    return keys_text();
}

/* Developer > Diagnostics: the system, then the UI's own numbers (DESIGN 4.7). */
int ap_diag_count(void) { return 14; }

void ap_diag_row(int i, row_t *r)
{
    static const char *const l[14] = { "Core A", "Core B", "Battery", "eMMC", "SD card", "USB", "USB diag",
                                       "Frames", "Last frame", "Key to pixels", "Display path", "Display ops",
                                       "Keys", "Input" };
    const display_stats_t *d = display_stats();
    char *v = r->value;
    int n = sizeof r->value;

    snprintf(r->text, sizeof r->text, "%s", l[i]);
    switch (i) {
    case 0: snprintf(v, n, "%s", cpu_text()); break;
    case 1: snprintf(v, n, "%u MHz", (unsigned)(clock_core_b_hz() / 1000000u)); break;
    case 2: snprintf(v, n, "%s", power_battery_text()); break;
    case 3: snprintf(v, n, "%s", s_emmc); break;
    case 4: snprintf(v, n, "%s", sdcard_text()); break;
    case 5: snprintf(v, n, "%s", s_usb); break;
    case 6: snprintf(v, n, "%s", usbd_text()); break;
    case 7: snprintf(v, n, "%u, %u slow", (unsigned)d->frames, (unsigned)d->slow); break;
    case 8: snprintf(v, n, "%u.%u ms, %u rows", (unsigned)(d->last_us / 1000u), (unsigned)(d->last_us / 100u % 10u),
                     (unsigned)d->last_rows); break;
    case 9: snprintf(v, n, "%u ms (worst %u)", (unsigned)d->key_last_ms, (unsigned)d->key_worst_ms); break;
    case 10: snprintf(v, n, "%s", panel_io_path()); break;
    case 11: snprintf(v, n, "%u, %u dropped", display_op_count(), display_ops_dropped); break;
    case 12: snprintf(v, n, "%s", keys_text()); break;
    case 13: snprintf(v, n, "%u lost", input_dropped()); break;
    }
}

void ap_ui_loop(void)
{
    input_event_t ev;
    uint32_t t_first = 0;

    log_set_sync(emmc_sync);   /* a named log flush is on the disk when it returns (log.h) */
    input_set_clock(time_ms);
    keys_isr_start();     /* the keys sampled every 10 ms from now on (ap_tick.c), whatever runs */
    boot_key_sample();    /* first: the keys work whatever comes after */
    if (s_boot_key == KEY_DOWN)   /* before our storage driver runs at all (it might be what fails) */
        rockusb_flow("VOL- held at power-on");
    bootcrumb(BC_EMMC);
    emmc_probe();
    bootcrumb_kv(BCK_EMMC_RC, (uint32_t)s_storage_failed);
    bootcrumb_kv(BCK_EMMC_SECTORS, emmc_sectors());
    bootcrumb_kv(BCK_EMMC_WIDTH, (uint32_t)emmc_width());
    if (s_storage_failed)
        rockusb_flow(s_emmc[0] ? s_emmc : "storage failed");
    bootcrumb(BC_SLOTS);
    if (s_user) {         /* a slot left invalid (an update cut short): copied from the good one */
        int r;

        r = fw_update_repair(repair_progress);   /* silent unless a slot needs it (rare) */
        if (r)
            log_printf("fw: slot repair %d (%s)", r, r == 1 ? "slot 1 rebuilt from slot 2" :
                       r == 2 ? "slot 2 rebuilt from slot 1" : "failed");
        if (r < 0)
            line(3, "firmware slots: repair failed");
    }
    bootcrumb(BC_RECOVERY);
    recovery_menu();      /* a fault last time, or VOL+ held: before anything else */
    bootcrumb(BC_UPDATE);
    /* No update check at boot (the user, 2026-10-18: boot straight to where you were): an image
     * on the disk is offered when a USB disk session ends, and in Settings > System > Firmware
     * update. Recovery (VOL+ at power-on) still installs one. */
    bootcrumb(BC_PLAYER);
    player_init();       /* register wav/mp3/flac codecs (registry) */
    bootcrumb(BC_CONFIG);
    config_load();
    queue_set_mode(g_cfg.play);
    audio_set_volume_db(g_cfg.volume == CFG_VOLUME_MUTE ? -90 : g_cfg.volume);
    log_printf("cfg: play %s, volume %d, resume %u", queue_mode_name(g_cfg.play), g_cfg.volume, g_cfg.resume);
    cpuclk_boot();               /* the CPU point record of this build's earlier boots (services/cpuclk.h) */
    g_lib.data = g_cfg.libdata;
    ap_lib_boot();               /* the library: Internal's index read, its refresh asked for if due */
    log_flush_async("boot");     /* (on the disk within a few passes: not ahead of the first frame) */
    bootcrumb(BC_POWER);
    power_init();
    bt_app_init();                   /* the BT module on only if the setting says so (ap_bt.c) */
    s_bt_ready = 1;
    bootcrumb(BC_UI_INIT);
    ui_init();
    theme_load_index(g_cfg.theme);
    ui_push_screen(&k_rc);               /* one ui.c screen: the page stack draws (ui/nav.c) */
    s_ui_up = 1;
    if (g_cfg.resume == CFG_RESUME_OFF)
        g_cfg.browse[0] = 0;             /* (Folders opens where you were only with resume on) */
    nav_reset(&pg_home);
    if (s_user && g_cfg.resume == CFG_RESUME_TRACK && g_cfg.track[0] &&
        player_resume(g_cfg.track) == 0 && player_play() == PLAYER_OK) {
        nav_push(&pg_np, 0);
        log_printf("ui: resumed %s", g_cfg.track);
    } else if (s_user && g_cfg.resume == CFG_RESUME_FOLDER && g_cfg.browse[0] == '/' && g_cfg.browse[1]) {
        nav_push(&pg_vols, 0);           /* straight into the folder you were in (it reopens there) */
        log_printf("ui: resumed in %s", g_cfg.browse);
    }
    {   /* the USB block as the ROM left it, before the park (USB DAC audit D6, SoC report 6.1) */
        uint32_t r[10];

        usb_regs_read(r);
        log_printf("usb: ROM state: gotgctl %08x gusbcfg %08x gintsts %08x pcgcctl %x hwcfg2 %08x hwcfg3 %08x "
                   "hwcfg4 %08x snpsid %08x uoc0 %04x dsts %x", (unsigned)r[0], (unsigned)r[1], (unsigned)r[2],
                   (unsigned)r[3], (unsigned)r[4], (unsigned)r[5], (unsigned)r[6], (unsigned)r[7], (unsigned)r[8],
                   (unsigned)r[9]);
        bootcrumb_kv(BCK_USB_HWCFG3, r[5]);
        bootcrumb_kv(BCK_USB_HWCFG4, r[6]);
        bootcrumb_kv(BCK_USB_SNPSID, r[7]);
    }
    bootcrumb(BC_LOOP);
    watchdog_start();                /* as stock, once the boot is done (services/watchdog.c) */
    (void)panel_io_select(g_cfg.display);   /* the panel's pixel path: the VOP as a trial, now the
                                             * watchdog ends a hang (drivers/panel_io.h) */
    for (;;) {
        uint32_t t_pass = time_ms(), t_keys, t_play, t_draw, t_start;
        int adc = keys_adc(), power = keys_power(), cable = usb_cable();
        int adc2 = board_adc_ch2();
        static int s_adc_logged;

        if (!s_adc_logged && time_ms() > 3000u) {   /* how the converter behaved (keys_adc) */
            const uint32_t *d = adc_diag();

            s_adc_logged = 1;
            log_printf("adc: %u done, %u timed out, bit6 %u; first: stas %x ctrl %x data %x after %u us; "
                       "now ctrl %x stas %x; ch0..3 %d %d %d %d", (unsigned)d[ADC_DIAG_DONE],
                       (unsigned)d[ADC_DIAG_TIMEOUTS], (unsigned)d[ADC_DIAG_BIT6],
                       (unsigned)d[ADC_DIAG_FIRST_STAS], (unsigned)d[ADC_DIAG_FIRST_CTRL],
                       (unsigned)d[ADC_DIAG_FIRST_DATA], (unsigned)d[ADC_DIAG_FIRST_US],
                       (unsigned)d[ADC_DIAG_LAST_CTRL], (unsigned)d[ADC_DIAG_LAST_STAS],
                       adc_value(0), adc_value(1), adc_value(2), adc_value(3));
            for (unsigned i = 0; i < ADC_DIAG_N; i++)
                bootcrumb_kv(BCK_ADC_DIAG + i, d[i]);
            bootcrumb(BC_LOOP);                     /* persisted with them */
        }

        if (sdcard_tick())               /* card in / out, debounced (services/sdcard.c) */
            ui_invalidate_all();
        switch (sd_event(NULL)) {        /* ... and the library told (its index is per card serial) */
        case SD_EV_INSERTED: ap_lib_volume(1, LIB_EV_MOUNTED); break;
        case SD_EV_REMOVED:  ap_lib_volume(1, LIB_EV_GONE); break;
        case SD_EV_CHANGED:  ap_lib_volume(1, LIB_EV_WROTE); break;
        }

        {                                /* what the cable reading depends on, when it changes */
            static uint32_t s_st[3];
            static unsigned s_st_lines;
            uint32_t st[3];

            usb_cable_state(st);
            if ((st[0] != s_st[0] || st[1] != s_st[1] || st[2] != s_st[2]) && s_st_lines < 40u) {
                s_st_lines++;
                log_printf("usbst: e8 %x rst %x gate %x pins %03x uoc %08x con6/7 %08x", (unsigned)(st[0] & 0xFu),
                           (unsigned)(st[0] >> 4 & 7u), (unsigned)(st[0] >> 7 & 3u), (unsigned)(st[0] >> 9),
                           (unsigned)st[1], (unsigned)st[2]);
            }
            s_st[0] = st[0];
            s_st[1] = st[1];
            s_st[2] = st[2];
        }
        if (usb_out_active())
            cable = 0;                   /* (VBUS is the player's own while host) */
        if (cable && !s_cable) {         /* the cable in (also at boot) */
            s_wake_due = 0;
            if (!cable_connected() && !usb_out_active())
                usb_dwc2_park();         /* (charge only: no session parked it) */
        } else if (!cable && !s_phy_parked && !usb_out_active()) {
            /* no cable at boot: the comparators woken so a later plug is seen, but after the first
             * screen is up (the sequence holds the loop ~890 ms; it was the first pass's whole
             * "keys+events 1362", OURS_AUDIT 3.4); the keys are sampled meanwhile (ap_tick.c) */
            s_wake_due = time_stamp(time_ms() + 300u);
        } else if (!cable && s_cable) {
            s_wake_due = time_stamp(time_ms() + 1500u);   /* the cable out: again, once it is gone */
        }
        if (s_wake_due && !cable && time_ms() - s_wake_due < 0x80000000u &&
            player_get_state() == PLAYER_STOPPED && power_key_idle_ms() >= (s_cable_seen ? 5000u : 1000u))
            usb_wake(s_cable_seen ? "unplug" : "boot");   /* (paused: a resume would find the DAC unpowered) */
        s_phy_parked = 1;
        s_cable_seen |= cable;
        s_cable = cable;
        if (s_cable_answer) {            /* the cable question answered */
            int c = s_cable_answer - 1;

            s_cable_answer = 0;
            s_cable_q = 0;
            cable_mode(c);
            if (!usb_out_active())
                usb_dwc2_park();
        }
        if (!s_cable_q) {                /* (while it asks, the USB block stays as the cable found it) */
            usb_dwc2_idle_tick();        /* the armed PHY held after USB_ARM_MS */
            usb_out_tick();
        }

        /* hold power 2 s to switch off; the press that switched us on may
         * still be held, so arm only after the key has been released once
         * (and that press's release is not a short press either) */
        if (power && !s_power)
            s_power_press_ok = s_power_armed;
        s_power_armed |= !power;
        if (!power || !s_power_armed)
            s_power_down_ms = 0;
        else if (!s_power_down_ms)
            s_power_down_ms = time_stamp(time_ms());
        else if (time_ms() - s_power_down_ms >= 2000u)
            power_off(NULL);

        s_adc = adc;                     /* (Developer > Keys) */
        s_adc2 = adc2;
        s_power = power;
        input_scan(adc, power);
        /* VOL held: the volume's pace on Now Playing and with the screen off, the lists' elsewhere */
        input_set_repeat(nav_volume_keys() ? INPUT_REPEAT_VOLUME : INPUT_REPEAT_LIST);
        while (input_next(&ev) > 0)
            nav_key(&ev);
        if (s_later) {                   /* what a row started (a USB session, a bring-up tool) */
            void (*fn)(void) = s_later;

            s_later = 0;
            fn();
            frame_invalidate();
            ui_invalidate_all();
        }
        t_keys = time_ms();

        /* audio pump + Now Playing sync (no-ops without a playing track) */
        (void)player_process();
        t_play = time_ms();
        bootguard_poll();                /* a fault record is cleared after a minute of normal running */
        panel_io_poll();                 /* the VOP trial's marker cleared after 5 s of frames */
        ap_bg_poll();                    /* queued writes, the log, the settings: never waits (ap_bg.c) */
        ap_lib_poll();                   /* the library's job: 3 ms, only with audio headroom (lib_job.c) */
        switch (power_poll(player_get_state() == PLAYER_PLAYING, 0)) {
        case POWER_OFF_IDLE:
            power_off(NULL);
            break;
        case POWER_OFF_BATTERY:
            power_off("Battery low");
            break;
        default:
            break;
        }
        {
            static uint32_t heard;
            audio_status_t st;

            audio_status(&st);
            if (st.seq && st.seq != heard) {    /* the heard track changed: remember it */
                heard = st.seq;
                player_note_track(st.track.ref.clus);
            }
        }
        bt_app_tick();                   /* the BT module: frames, polls, keys (nothing while off) */
        t_draw = time_ms();
        if (power_screen_on()) {         /* (backlight off: nothing to draw) */
            nav_tick();
            ui_render();
        }
        t_start = time_ms();
        nav_after();                     /* (a track to start: after Now Playing is on the panel) */
        {   /* a pass that held the UI (the device froze 23 s in Files, 2026-10-16): where */
            static unsigned s_slow;
            uint32_t now = time_ms();

            static uint32_t s_slowest;

            if (s_pass_held) {           /* (device 2026-10-18: "slow pass 126424 ms" was the disk session) */
                s_pass_held = 0;
                t_pass = t_keys = t_play = t_draw = t_start = now;
            }

            if (now - t_pass > s_slowest && now - t_pass >= 300u) {
                s_slowest = now - t_pass;
                bootcrumb_set(BCK_SLOW_PASS, s_slowest | (uint32_t)nav_depth() << 24);
            }
            static uint32_t s_worst, s_worst_at, s_worst_parts[5];
            static const char *s_worst_on;

            if (!t_first) {
                t_first = now;
                s_worst_at = time_stamp(now);
                log_printf("ui: first frame at %u ms", (unsigned)now);
            }
            if (now - t_pass > s_worst) {    /* the worst pass of every 10 s, 50 ms or more (300 and more
                                              * has its own "slow pass" line) */
                s_worst = now - t_pass;
                s_worst_parts[0] = t_keys - t_pass;
                s_worst_parts[1] = t_play - t_keys;
                s_worst_parts[2] = t_draw - t_play;
                s_worst_parts[3] = t_start - t_draw;
                s_worst_parts[4] = now - t_start;
                s_worst_on = nav_top() ? nav_top()->name : "-";
            }
            if (now - s_worst_at >= 10000u) {
                if (s_worst >= 50u && s_worst < 300u)
                    log_printf("ui: worst pass %u ms on %s: keys+events %u, player %u, rest %u, draw %u, start %u",
                               (unsigned)s_worst, s_worst_on, (unsigned)s_worst_parts[0],
                               (unsigned)s_worst_parts[1], (unsigned)s_worst_parts[2],
                               (unsigned)s_worst_parts[3], (unsigned)s_worst_parts[4]);
                s_worst_at = time_stamp(now);
                s_worst = 0;
            }
            if (now - t_pass >= 300u && s_slow < 30u) {
                s_slow++;
                log_printf("ui: slow pass %u ms on %s: keys+events %u, player %u, rest %u, draw %u, start %u",
                           (unsigned)(now - t_pass), nav_top() ? nav_top()->name : "-",
                           (unsigned)(t_keys - t_pass), (unsigned)(t_play - t_keys),
                           (unsigned)(t_draw - t_play), (unsigned)(t_start - t_draw), (unsigned)(now - t_start));
            }
        }
        cpu_wait_tick();                 /* sleep (WFI) until the next 10 ms tick */
    }
}
