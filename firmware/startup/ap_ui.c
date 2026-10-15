/*
 * ap_ui.c -- milestone 2: key-driven UI loop on the AP (fw1), called by
 * Main() after the splash. Keys come from the board (keys_adc/keys_power),
 * are debounced by input_scan() and dispatched to the screen stack.
 */
#include "drivers/board.h"
#include <stdio.h>
#include <string.h>

#include "drivers/clock.h"
#include "drivers/display.h"
#include "drivers/emmc.h"
#include "drivers/panel_io.h"
#include "drivers/usb_msc.h"
#include "drivers/usb_cdc.h"
#include "drivers/usb_uac.h"
#include "drivers/usb_probe.h"
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
#include "ui/ui.h"
#include "ui/screens/screens.h"

static int s_adc = 1023, s_adc2 = 1023, s_power, s_cable;
static int s_power_press_ok;
static uint32_t s_log_flushed;  /* time_ms() of the last periodic log write */
static uint32_t s_shortcut_at;  /* VOL+/VOL- chord: a shortcut key may follow within 2 s */
static uint32_t s_vol_at;       /* the last volume key press, and the volume before it */
static int s_vol_before;  /* the power key's current press began after boot's was released */
static uint32_t s_power_down_ms;     /* time_ms() when the power key went down */
static int s_power_armed;    /* power key seen released since boot */
static uint32_t s_user;              /* user volume start, 0 = unknown */
static char s_emmc[32], s_usb[32] = "plug in", s_sd[32];
static int s_sd_in = -1;
static uint32_t s_cpu_shown_ms;

/* Read-only eMMC probe for the root menu: size and user volume start. */
static void emmc_probe(void)
{
    uint32_t user;
    int r = emmc_init();

    if (r) {
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
    if (r)
        snprintf(s_emmc, sizeof s_emmc, "%uM lay %d", (unsigned)(emmc_sectors() >> 11), r);
    else
        snprintf(s_emmc, sizeof s_emmc, "%uM u%x", (unsigned)(emmc_sectors() >> 11),
                 (unsigned)(s_user = user));
    if (s_user)
        fs_target_mount_rw(0, "Internal", emmc_read, emmc_write, s_user);
}

static const char *emmc_text(void) { return s_emmc; }

/* SD row: size, type, bus width and the first volume's file system. */
static void sd_probe(void)
{
    int r = sd_init();

    fs_target_unmount(1);
    if (r == -1) {
        snprintf(s_sd, sizeof s_sd, "no card");
        return;
    }
    if (r) {
        snprintf(s_sd, sizeof s_sd, "init %d", r);
        return;
    }
    r = fs_target_mount(1, "SD", sd_read, 0);
    snprintf(s_sd, sizeof s_sd, "%uM %s %db %s", (unsigned)(sd_sectors() >> 11),
             sd_is_hc() ? "HC" : "SC", sd_bus_width(),
             r ? (r == -3 ? "rd err" : "no fs") : fs_target_type(1));
}

static const char *sd_text(void) { return s_sd; }
static const char *usb_text(void)  { return s_usb; }

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

/* KEY_SELECT = yes; BACK / power = no; any = 1 with any_key. */
static int ask(int any_key)
{
    input_event_t ev;

    while (keys_power())
        cpu_wait_tick();
    input_mock_clear();
    for (;;) {
        input_scan(keys_adc(), keys_power());
        while (input_poll(&ev) > 0) {
            static const char *const k_names[] = { "none", "up", "down", "select", "back", "power", "menu", "chord" };

            log_printf("ui: key %s%s on %s", ev.type >= 0 && ev.type < 8 ? k_names[ev.type] : "?",
                       ev.adc_value == INPUT_ADC_LONG ? " (hold)" : ev.adc_value == INPUT_ADC_REPEAT ? " (repeat)" : "",
                       ui_top_screen() ? ui_top_screen()->name : "-");
            if (any_key || ev.type == KEY_SELECT)
                return 1;
            if (ev.type == KEY_BACK || ev.type == KEY_POWER)
                return 0;
        }
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
    const browser_state_t *b = browser_screen_state();

    if (b->path[0] && strcmp(b->path, "/")) {
        snprintf(g_cfg.browse, sizeof g_cfg.browse, "%s", b->path);
        g_cfg.cursor = (uint16_t)b->cursor;
    }
    config_save();
    log_flush(why);
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
 * the SD card (LUN 1, read-only) until eject/unplug/power key. */
static void usb_session(void)
{
    const usb_lun_t luns[2] = {
        { emmc_read, emmc_write, s_user, emmc_sectors() - s_user },
        { sd_read, NULL, 0, sd_sectors() },
    };
    uint32_t rd, wr;
    int r;

    if (!s_user) {      /* layout unknown or wrong board: never export LBA 0.. */
        snprintf(s_usb, sizeof s_usb, "no user volume");
        return;
    }
    player_stop();                       /* the PC may rewrite the files we stream */
    save_state("usb");
    screen("USB mass storage");
    line(1, "eject on the PC or press power");
    display_flush_rows(0, DISPLAY_H - 1);
    r = usb_msc_run(luns, 2, keys_power, &rd, &wr);
    fs_target_mount_rw(0, "Internal", emmc_read, emmc_write, s_user);   /* host may have written */
    snprintf(s_usb, sizeof s_usb, "x%d r%uM w%uM", r, (unsigned)(rd >> 11), (unsigned)(wr >> 11));
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

/* After a settings change: what applies at once (brightness, debug strip and CPU
 * policy arrive with the power work; the play mode applies in settings_screen). */
void settings_apply(void)
{
    power_brightness_changed();
    config_save_later();
}

static void open_files(void)    { browser_screen_set_root("/"); ui_push_screen(&browser_screen); }
static void open_player(void)   { ui_push_screen(&player_screen); }
static void open_settings(void) { settings_screen_reload(); ui_push_screen(&settings_screen); }

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
static void dac_test_run(void)
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
static void usb_console_run(void)
{
    char ln[64];
    unsigned len = 0, frames = 0;

    screen("USB diag");
    line(1, "enumerating... POWER=exit");
    display_flush_rows(0, DISPLAY_H - 1);
    dac_init(44100);                       /* I2C0 up so r/w cmds work      */
    usb_cdc_init();
    for (;;) {
        int c;
        usb_cdc_poll();
        if (keys_power() || !usb_cable())
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

/* RECHO's Debug > Power page: the battery state and rates, the clocks. Refreshed
 * once a second; any key returns. */
static void battery_info_run(void)
{
    input_event_t ev;
    uint32_t shown = 0;

    while (keys_power())
        cpu_wait_tick();
    input_mock_clear();
    for (;;) {
        if (!shown || time_ms() - shown >= 1000u) {
            const power_battery_t *b = power_battery();
            char buf[40];

            shown = time_ms() | 1u;
            screen("Power");
            snprintf(buf, sizeof buf, "%u mV  %u.%u %%  level %u", b->mv, b->pct10 / 10u, b->pct10 % 10u, b->level);
            line(1, b->mv ? buf : "battery: measuring");
            line(2, !b->vbus ? "on battery" : b->full ? "cable, full" : "cable, charging");
            if (b->rate_ok) {
                snprintf(buf, sizeof buf, "%d mV/min", b->mv_min);
                line(3, buf);
                snprintf(buf, sizeof buf, "%%/h 1m %d.%u 5m %d.%u 30m %d.%u", b->pct_h10[0] / 10,
                         (unsigned)(b->pct_h10[0] < 0 ? -b->pct_h10[0] : b->pct_h10[0]) % 10u, b->pct_h10[1] / 10,
                         (unsigned)(b->pct_h10[1] < 0 ? -b->pct_h10[1] : b->pct_h10[1]) % 10u, b->pct_h10[2] / 10,
                         (unsigned)(b->pct_h10[2] < 0 ? -b->pct_h10[2] : b->pct_h10[2]) % 10u);
                line(4, buf);
            } else {
                line(3, b->vbus ? "rates: on battery only" : "rates: after 90 s on battery");
            }
            if (b->left_min)
                snprintf(buf, sizeof buf, "time left %u h %02u min", b->left_min / 60u, b->left_min % 60u);
            else
                snprintf(buf, sizeof buf, "time left: not yet");
            line(5, buf);
            snprintf(buf, sizeof buf, "A %u MHz  B %u MHz", (unsigned)(clock_core_hz() / 1000000u),
                     (unsigned)(clock_core_b_hz() / 1000000u));
            line(6, buf);
            display_flush_rows(0, DISPLAY_H - 1);
        }
        (void)player_process();          /* the music goes on meanwhile */
        input_scan(keys_adc(), keys_power());
        if (input_poll(&ev) > 0) {
            power_activity();
            break;
        }
        cpu_wait_tick();
    }
    ui_invalidate_all();
}

static void usb_dac_session(void);

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

static void usb_probe_menu(void)
{
    static usb_probe_step_t st[USB_PROBE_STEPS];
    char buf[64], up[40] = "", dev[40] = "";
    input_event_t ev;
    int n;

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
        cpu_wait_tick();
    }
    ui_invalidate_all();
}

static const menu_item_t k_root_items[] = {
    { "DAC test",    NULL,      dac_test_run },
    { "USB console", NULL,      usb_console_run },
    { "USB storage", NULL,      usb_storage_run },
    { "Files",       NULL,      open_files },
    { "Now Playing", NULL,      open_player },
    { "Settings",    NULL,      open_settings },
    { "CPU",         cpu_text,  NULL },
    { "Keys",        keys_text, NULL },
    { "eMMC",        emmc_text, NULL },
    { "SD",          sd_text,   NULL },
    { "USB",         usb_text,  NULL },
    { "USB diag",    usbd_text, NULL },
    { "Battery",     power_battery_text, battery_info_run },
    { "USB DAC",     NULL,      usb_dac_session },       /* (last but one: tests count rows to Files) */
    { "USB-C probe", NULL,      usb_probe_menu },
};
#define ROOT_CPU_ROW 6               /* index of "CPU" in k_root_items */
#define ROOT_BAT_ROW 12
#define RC_BUILD "m7a"
static const menu_model_t k_root = { "ReChord " RC_BUILD, k_root_items,
                                      sizeof k_root_items / sizeof k_root_items[0] };

/* A key restarts the screen and idle timers. The power key's short press turns
 * the screen off and on. With the screen off (RECHO, C.6): VOL+/VOL- and PLAY
 * work as on Now Playing, MENU and BACK only turn it on. Returns 1 if the event
 * still goes to the screen on top. */
static int dark_or_power_key(const input_event_t *ev)
{
    int on = power_screen_on();

    power_activity();
    if (ev->type == KEY_POWER) {
        if (ev->adc_value != INPUT_ADC_LONG) {
            power_screen(!on);
            if (on)
                config_save();           /* RECHO saves at screen off */
            else
                ui_invalidate_all();
        }
        return 0;
    }
    if (on)
        return 1;
    if (ev->type == KEY_UP || ev->type == KEY_DOWN || ev->type == KEY_SELECT)
        player_screen.handle_event(ev);
    else if (ev->type == KEY_MENU || ev->type == KEY_BACK) {
        if (ev->adc_value != INPUT_ADC_LONG) {
            power_screen(1);
            ui_invalidate_all();
        }
    }
    return 0;
}

/* RECHO's shortcuts (0x0305b3cc): VOL+ and VOL- together arm them, and the next
 * key within 2 s picks one: PLAY saves the screen as /Internal/SHOTn.BMP. The
 * chord undoes a volume step its first key made in the last 500 ms. Returns 1 if
 * the event was taken. */
static int shortcut_key(const input_event_t *ev)
{
    uint32_t armed = s_shortcut_at;

    if (ev->type == KEY_CHORD) {
        power_activity();
        if (time_ms() - s_vol_at < 500u && player_get_volume() != s_vol_before)
            player_set_volume(s_vol_before);
        s_shortcut_at = time_ms() | 1u;
        log_printf("ui: shortcut: play = screenshot");
        return 1;
    }
    if (!armed)
        return 0;
    s_shortcut_at = 0;
    if (time_ms() - armed > 2000u || ev->type != KEY_SELECT || ev->adc_value == INPUT_ADC_LONG)
        return 0;
    power_activity();
    log_printf("ui: screenshot: %d", screenshot_save());
    return 1;
}

/* The USB DAC's screen. usb_uac_run() reports about once a second; it is drawn
 * only when what it shows changed (and not while the screen is off). */
static usb_uac_status_t s_uac;
static char s_uac_shown[4][40];

static void uac_draw(int force)
{
    char t[4][40];
    const usb_uac_status_t *st = &s_uac;

    if (!power_screen_on())
        return;
    memset(t, 0, sizeof t);              /* (compared whole below: no stack bytes after the text) */
    if (!st->configured)
        snprintf(t[0], sizeof t[0], "waiting for the host");
    else if (!st->bits)
        snprintf(t[0], sizeof t[0], "%s speed, idle", st->high_speed ? "high" : "full");
    else
        snprintf(t[0], sizeof t[0], "%u Hz  %u-bit", (unsigned)st->rate, (unsigned)st->bits);
    if (st->mute)
        snprintf(t[1], sizeof t[1], "host: muted");
    else
        snprintf(t[1], sizeof t[1], "host volume %d.%d dB", (int)(st->volume_256db / 256),
                 (int)((st->volume_256db < 0 ? -st->volume_256db : st->volume_256db) % 256 * 10 / 256));
    snprintf(t[2], sizeof t[2], "player volume %d dB", audio_get_volume_db());
    snprintf(t[3], sizeof t[3], "late %u  lost %u", (unsigned)st->underruns, (unsigned)(st->dropped + st->missed));
    if (!force && !memcmp(t, s_uac_shown, sizeof t))
        return;
    memcpy(s_uac_shown, t, sizeof t);
    screen("USB DAC");
    for (int i = 0; i < 4; i++)
        line(1 + i, t[i]);
    line(6, "PLAY/BACK: host  MENU: leave");
    display_flush_rows(0, DISPLAY_H - 1);
}

static void uac_status(const usb_uac_status_t *st)
{
    s_uac = *st;
    uac_draw(0);
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

/* Every tick of the DAC session, keys as on Now Playing: VOL+/VOL- the player's
 * volume (on top of the host's); PLAY play/pause on the host, held: next track;
 * BACK previous track; MENU leaves; power toggles the screen. Volume and PLAY
 * work with the screen off, BACK and MENU then only light it. Also the screen-off
 * timer and the battery. Nonzero = leave. */
static int uac_tick(void)
{
    input_event_t ev;
    int leave = 0;

    input_scan(keys_adc(), keys_power());
    while (input_poll(&ev) > 0) {
        int was_on = power_screen_on(), hold = ev.adc_value == INPUT_ADC_LONG;

        power_activity();
        if (ev.type == KEY_POWER && !hold) {
            power_screen(!was_on);
            if (!was_on)
                uac_draw(1);
        } else if (ev.type == KEY_UP || ev.type == KEY_DOWN) {
            player_set_volume(player_get_volume() + (ev.type == KEY_UP ? 1 : -1));
            uac_draw(0);
        } else if (ev.type == KEY_SELECT && ev.adc_value != INPUT_ADC_REPEAT) {
            usb_uac_key(hold ? USB_UAC_NEXT : USB_UAC_PLAY_PAUSE);
        } else if (!was_on) {
            power_screen(1);             /* BACK, MENU: just light it */
            uac_draw(1);
        } else if (ev.type == KEY_BACK && !hold) {
            usb_uac_key(USB_UAC_PREV);
        } else if (ev.type == KEY_MENU && !hold) {
            leave = 1;
        }
    }
    if (power_poll(1, POWER_SESSION_DAC) == POWER_OFF_BATTERY)   /* (no auto power off: the cable is in) */
        leave = 1;
    dac_power_only();
    return leave;
}

static void usb_dac_session(void)
{
    int r;

    player_stop();
    save_state("usb dac");
    power_screen(1);
    while (keys_power())                 /* the press that got us here (or switched us on) */
        cpu_wait_tick();
    input_mock_clear();
    memset(&s_uac, 0, sizeof s_uac);
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
    uac_draw(1);
    r = usb_uac_run(uac_tick, uac_status);
    log_printf("ui: USB DAC ended (%d)", r);
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
static void cable_connected(void)
{
    int disk = g_cfg.usb == CFG_USB_DISK, dac = g_cfg.usb == CFG_USB_DAC;

    log_printf("usb: cable in (%s)", g_cfg.usb == CFG_USB_ASK ? "ask" : g_cfg.usb == CFG_USB_DISK ? "disk" :
               g_cfg.usb == CFG_USB_DAC ? "dac" : "charge");
    if (g_cfg.usb == CFG_USB_ASK) {
        input_event_t ev;

        power_screen(1);
        screen("Cable connected");
        line(2, "PLAY: USB disk");
        line(3, "MENU: USB DAC");
        line(4, "BACK: charge only");
        display_flush_rows(0, DISPLAY_H - 1);
        while (keys_power())
            cpu_wait_tick();
        input_mock_clear();
        for (;;) {                       /* PLAY, MENU or BACK/power */
            input_scan(keys_adc(), keys_power());
            if (input_poll(&ev) > 0 && ev.adc_value != INPUT_ADC_REPEAT) {
                disk = ev.type == KEY_SELECT;
                dac = ev.type == KEY_MENU;
                if (disk || dac || ev.type == KEY_BACK || ev.type == KEY_POWER)
                    break;
            }
            if (!usb_cable())
                break;
            cpu_wait_tick();
        }
        ui_invalidate_all();
    }
    if (dac && usb_cable()) {
        usb_dac_session();
    } else if (disk) {
        power_screen(1);
        usb_session();
    }
    power_activity();
}

void ap_ui_loop(void)
{
    input_event_t ev;

    emmc_probe();
    if (s_user)
        update_prompt();  /* at boot too, as stock and RECHO check */
    player_init();       /* register wav/mp3/flac codecs (registry) */
    config_load();
    queue_set_mode(g_cfg.play);
    audio_set_volume_db(g_cfg.volume == CFG_VOLUME_MUTE ? -90 : g_cfg.volume);
    log_printf("cfg: play %s, volume %d, resume %u", queue_mode_name(g_cfg.play), g_cfg.volume, g_cfg.resume);
    log_flush("boot");
    power_init();
    ui_init();
    menu_screen_bind(&k_root);
    ui_push_screen(&menu_screen);
    if (s_user && g_cfg.resume != CFG_RESUME_OFF && g_cfg.browse[0]) {   /* resume: the folder */
        browser_screen_set_root("/");
        browser_screen_resume(g_cfg.browse, g_cfg.cursor);
        ui_push_screen(&browser_screen);
    }
    if (s_user && g_cfg.resume == CFG_RESUME_TRACK && g_cfg.track[0] &&
        player_resume(g_cfg.track) == 0 && player_play() == PLAYER_OK) {
        ui_push_screen(&player_screen);
        log_printf("ui: resumed %s", g_cfg.track);
    }
    for (;;) {
        int adc = keys_adc(), power = keys_power(), cable = usb_cable();
        int adc2 = board_adc_ch2();

        if (sd_present() != s_sd_in) {   /* card inserted / removed */
            s_sd_in = sd_present();
            if (s_sd_in)
                panel_io_delay_ms(300);  /* let the contacts settle */
            sd_probe();
            ui_invalidate_all();
        }

        if (cable && !s_cable)           /* the cable in (also at boot) */
            cable_connected();
        s_cable = cable;

        /* hold power 2 s to switch off; the press that switched us on may
         * still be held, so arm only after the key has been released once
         * (and that press's release is not a short press either) */
        if (power && !s_power)
            s_power_press_ok = s_power_armed;
        s_power_armed |= !power;
        if (!power || !s_power_armed)
            s_power_down_ms = 0;
        else if (!s_power_down_ms)
            s_power_down_ms = time_ms() | 1u;
        else if (time_ms() - s_power_down_ms >= 2000u)
            power_off(NULL);

        /* live "Keys" row on the root menu: hardware check without a log */
        if ((adc - s_adc > 8 || s_adc - adc > 8 || power != s_power ||
             adc2 - s_adc2 > 8 || s_adc2 - adc2 > 8) &&
            ui_stack_depth() == 1) {
            ui_invalidate_all();
        }
        if (time_ms() - s_cpu_shown_ms >= 1000u) {
            s_cpu_shown_ms = time_ms();  /* CPU row: once a second */
            menu_screen_invalidate_item(ROOT_CPU_ROW);
            menu_screen_invalidate_item(ROOT_BAT_ROW);
        }
        s_adc = adc;
        s_adc2 = adc2;
        s_power = power;
        input_scan(adc, power);

        while (input_poll(&ev) > 0) {
            static const char *const k_names[] = { "none", "up", "down", "select", "back", "power", "menu", "chord" };

            log_printf("ui: key %s%s on %s", ev.type >= 0 && ev.type < 8 ? k_names[ev.type] : "?",
                       ev.adc_value == INPUT_ADC_LONG ? " (hold)" : ev.adc_value == INPUT_ADC_REPEAT ? " (repeat)" : "",
                       ui_top_screen() ? ui_top_screen()->name : "-");
            if (ev.type == KEY_POWER && !s_power_press_ok)
                continue;                /* the release of the press that switched us on */
            if (shortcut_key(&ev))
                continue;
            if ((ev.type == KEY_UP || ev.type == KEY_DOWN) && ev.adc_value != INPUT_ADC_REPEAT) {
                s_vol_before = player_get_volume();
                s_vol_at = time_ms();
            }
            if (!dark_or_power_key(&ev))
                continue;
            if (ev.adc_value == INPUT_ADC_LONG && ev.type != KEY_MENU &&
                ui_top_screen() != &player_screen && ui_top_screen() != &browser_screen)
                continue;                /* holds mean something on Now Playing and Files (and MENU) */
            if (ev.type == KEY_MENU && ev.adc_value == INPUT_ADC_LONG) {
                while (ui_stack_depth() > 1)       /* hold: the main menu */
                    ui_pop_screen();
            } else if (ev.type == KEY_MENU) {      /* Files <-> Now playing */
                if (ui_top_screen() == &browser_screen) {
                    ui_push_screen(&player_screen);
                } else {
                    while (ui_stack_depth() > 1 && ui_top_screen() != &browser_screen)
                        ui_pop_screen();
                    if (ui_top_screen() != &browser_screen) {
                        if (browser_screen_state()->path[0])
                            ui_push_screen(&browser_screen);   /* where it was */
                        else
                            open_files();
                    }
                }
                ui_invalidate_all();
            } else {
                ui_handle_event(&ev);
            }
        }
        if (ui_stack_depth() == 0)       /* BACK on the root menu */
            ui_push_screen(&menu_screen);

        /* audio pump + Now Playing sync (no-ops without a playing track) */
        (void)player_process();
        config_poll();
        /* the RAM ring keeps the oldest lines when full: write it out once half full
         * (at most once a minute), so a long session's log reaches ECHOLOG.TXT whole */
        if ((log_bytes() > 2048u && time_ms() - s_log_flushed >= 60000u) ||
            (log_bytes() > 3072u && time_ms() - s_log_flushed >= 5000u)) {   /* (a burst) */
            s_log_flushed = time_ms();
            log_flush(NULL);
        }
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
        if (!power_screen_on())
            ;                            /* backlight off: nothing to draw */
        else if (ui_top_screen() == &player_screen)
            player_screen_refresh();     /* only while it is on screen */
        if (power_screen_on())
            ui_render();
        cpu_wait_tick();                 /* sleep (WFI) until the next 10 ms tick */
    }
}
