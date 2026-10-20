/*
 * ap_bt.c -- Bluetooth in the UI: the "Bluetooth" screen on the main menu, the
 * glue between drivers/bt_jieli.c and the player (earphone keys, the separate
 * BT volume, CY0 on play/pause) and the settings (on at boot, the remembered
 * device). Stock's behaviour it follows: work/stock_boot/08_bluetooth.md §1.3, §3.
 *
 * Screen rows (the list template, ui/nav.c):
 *   Bluetooth   Off / Starting / On / Scanning / Connecting / Connected / No module
 *               (PLAY toggles the module; on: the remembered device is connected
 *               (stock seg84:0307a13e: CL06) or, with none, a scan starts (CS00))
 *   <remembered device>   Connected / Connecting / Paired (PLAY: connect, or disconnect)
 *   <found devices>       up to 6 (PLAY: connect: CL06 + MAC)
 *   Scan for devices      found N (PLAY: CS00, 3 s)
 *   Disconnect  (connected: CT00)
 *   Forget      (a remembered device: our settings only -- stock sends the module
 *               nothing to forget, seg84:0307ae40)
 */
#include <stdio.h>
#include <string.h>

#include "drivers/bt_jieli.h"
#include "drivers/clock.h"
#include "drivers/display.h"
#include "drivers/usb_uac.h"
#include "services/bt_keys.h"
#include "services/config.h"
#include "services/log.h"
#include "services/power.h"
#include "services/queue.h"
#include "audio/audio.h"
#include "player.h"
#include "ui/ui.h"
#include "ui/nav.h"

enum { ROW_POWER, ROW_PAIRED, ROW_SCAN, ROW_DEV, ROW_DISCONNECT, ROW_FORGET };

typedef struct {
    uint8_t kind, idx;
} bt_row_t;

#define ROWS (4 + BT_SCAN_MAX)
static bt_row_t s_rows[ROWS];
static int s_nrows;
static uint32_t s_shown_seq;
static int s_after_on;                  /* the screen turned it on: connect or scan once it is up */
static int s_routed;                    /* the volume is the BT volume now */
static int s_jack_db, s_bt_db;

extern const page_t pg_bt;

static int same_mac(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, BT_MAC_LEN); }

static void dev_label(char *out, int cap, const uint8_t *mac, const char *name)
{
    char m[18];

    bt_mac_text(mac, m);
    snprintf(out, (unsigned)cap, "%s", name && name[0] ? name : m);
}

static int here(const uint8_t *mac)
{
    bt_state_t st = bt_state();

    return (st == BT_CONNECTED || st == BT_CONNECTING) && same_mac(bt_peer()->mac, mac);
}

/* The rows (DESIGN 1.2): Bluetooth, the remembered device, the devices found, Scan for devices,
 * Disconnect, Forget. Each has an id (kind, index), so the cursor stays on its row as they come. */
static int build(void)
{
    bt_state_t st = bt_state();

    s_nrows = 0;
    s_rows[s_nrows++].kind = ROW_POWER;
    if (g_cfg.bt_paired)
        s_rows[s_nrows++].kind = ROW_PAIRED;
    for (int i = 0; i < bt_scan_count() && s_nrows < ROWS - 3; i++) {
        if (g_cfg.bt_paired && same_mac(bt_scan_dev(i)->mac, g_cfg.bt_mac))
            continue;                   /* (the remembered row has it) */
        s_rows[s_nrows].kind = ROW_DEV;
        s_rows[s_nrows++].idx = (uint8_t)i;
    }
    s_rows[s_nrows++].kind = ROW_SCAN;
    if (st == BT_CONNECTED || st == BT_CONNECTING)
        s_rows[s_nrows++].kind = ROW_DISCONNECT;
    if (g_cfg.bt_paired)
        s_rows[s_nrows++].kind = ROW_FORGET;
    return s_nrows;
}

const char *bt_state_text(bt_state_t st)
{
    static const char *const k[] = { "Off", "Starting\xE2\x80\xA6", "No module", "On", "Scanning\xE2\x80\xA6",
                                     "Connecting\xE2\x80\xA6", "Connected" };

    return st <= BT_CONNECTED ? k[st] : "?";
}

static void bt_row(int i, row_t *r)
{
    const bt_row_t *b = &s_rows[i < s_nrows ? i : 0];
    bt_state_t st = bt_state();

    r->id = (uint16_t)(b->kind << 8 | b->idx);
    switch (b->kind) {
    case ROW_POWER:
        r->icon = ICON_BT;
        snprintf(r->text, sizeof r->text, "Bluetooth");
        snprintf(r->value, sizeof r->value, "%s", bt_state_text(st));
        break;
    case ROW_PAIRED:
        r->icon = ICON_HEADPHONES;
        dev_label(r->text, sizeof r->text, g_cfg.bt_mac, g_cfg.bt_name);
        snprintf(r->value, sizeof r->value, "%s", here(g_cfg.bt_mac) ? bt_state_text(st) : "Paired");
        break;
    case ROW_DEV: {
        const bt_dev_t *d = bt_scan_dev(b->idx);

        r->icon = ICON_HEADPHONES;
        if (d) {
            dev_label(r->text, sizeof r->text, d->mac, d->name);
            if (here(d->mac))
                snprintf(r->value, sizeof r->value, "%s", bt_state_text(st));
        }
        break;
    }
    case ROW_SCAN:
        r->icon = ICON_UPDATE;
        snprintf(r->text, sizeof r->text, "Scan for devices");
        if (st == BT_SCANNING)
            snprintf(r->value, sizeof r->value, "Scanning\xE2\x80\xA6");
        else if (bt_scan_count())
            snprintf(r->value, sizeof r->value, "found %d", bt_scan_count());
        break;
    case ROW_DISCONNECT:
        r->icon = ICON_POWER;
        snprintf(r->text, sizeof r->text, "Disconnect");
        break;
    case ROW_FORGET:
        r->icon = ICON_WARN;
        snprintf(r->text, sizeof r->text, "Forget");
        break;
    }
}

static void power_on(void)
{
    g_cfg.bt = 1;                       /* stock's toggle: settings +0x34b */
    s_after_on = 1;
    bt_power(1);
    config_save_later();
}

static void bt_act(int a, int i)
{
    const bt_row_t *r;
    bt_state_t st = bt_state();

    if (a != A_OPEN || i >= build())
        return;
    r = &s_rows[i];
    {
        static row_t lr;                /* (the row's own words in the log: "bt: ui: Disconnect") */

        bt_row(i, &lr);
        log_printf("bt: ui: %s", r->kind == ROW_SCAN ? "Scan" : lr.text);
    }
    switch (r->kind) {
    case ROW_POWER:
        if (st == BT_OFF || st == BT_NO_MODULE) {
            power_on();
        } else {
            g_cfg.bt = 0;
            s_after_on = 0;
            bt_power(0);
            config_save_later();
        }
        break;
    case ROW_PAIRED:
        if (st == BT_OFF || st == BT_NO_MODULE)
            power_on();
        else if (here(g_cfg.bt_mac))
            bt_disconnect();
        else if (st == BT_ON || st == BT_SCANNING)
            bt_connect(g_cfg.bt_mac, g_cfg.bt_name);
        break;
    case ROW_SCAN:
        bt_scan();
        break;
    case ROW_DEV: {
        const bt_dev_t *d = bt_scan_dev(r->idx);

        if (d && here(d->mac))
            bt_disconnect();
        else if (d)
            bt_connect(d->mac, d->name);
        break;
    }
    case ROW_DISCONNECT:
        bt_disconnect();
        break;
    case ROW_FORGET:
        if (st == BT_CONNECTED || st == BT_CONNECTING)
            bt_disconnect();
        g_cfg.bt_paired = 0;
        memset(g_cfg.bt_mac, 0, sizeof g_cfg.bt_mac);
        g_cfg.bt_name[0] = 0;
        config_save_later();
        break;
    }
    nav_live_now();
}

static const char *bt_verb(int i, int hold)
{
    if (hold || i >= s_nrows)
        return "";
    switch (s_rows[i].kind) {
    case ROW_POWER:      return "CHANGE";
    case ROW_SCAN:       return "SCAN";
    case ROW_DISCONNECT: return "DO";
    case ROW_FORGET:     return "FORGET";
    case ROW_PAIRED:     return here(g_cfg.bt_mac) ? "DISCONN." : "CONNECT";
    default: {
        const bt_dev_t *d = bt_scan_dev(s_rows[i].idx);

        return d && here(d->mac) ? "DISCONN." : "CONNECT";
    }
    }
}

static const keydef_t k_bt[] = {
    { KEY_UP, IG_DOWN, A_UP, "UP" }, { KEY_UP, IG_REPEAT, A_UP, NULL },
    { KEY_DOWN, IG_DOWN, A_DOWN, "DOWN" }, { KEY_DOWN, IG_REPEAT, A_DOWN, NULL },
    { KEY_SELECT, IG_TAP, A_OPEN, VERB }, { KEY_SELECT, IG_HOLD, A_GO_NP, "PLAYING" },
    { KEY_MENU, IG_TAP, A_NP, "PLAYING" }, { KEY_MENU, IG_HOLD, A_OPTIONS, "OPTIONS" },
    { KEY_BACK, IG_TAP, A_BACK, "BACK" }, { KEY_BACK, IG_HOLD, A_HOME, "HOME" },
    { 0 }
};

const page_t pg_bt = {
    .name = "bluetooth", .kind = PK_LIST, .icon = ICON_BT, .live = 1, .keys = k_bt, .title = "Bluetooth",
    .count = build, .row = bt_row, .act = bt_act, .verb = bt_verb,
};

/* ---- glue ---------------------------------------------------------------------- */

void bt_event(int ev, const bt_dev_t *dev)
{
    switch (ev) {
    case BT_EV_PAIRED:                  /* CLOK: stock copies it to a settings slot (+0x349) */
        memcpy(g_cfg.bt_mac, dev->mac, BT_MAC_LEN);
        snprintf(g_cfg.bt_name, sizeof g_cfg.bt_name, "%s", dev->name);
        g_cfg.bt_paired = 1;
        config_save_later();
        break;
    case BT_EV_CONNECTED:
        if (!g_cfg.bt_paired || !same_mac(g_cfg.bt_mac, dev->mac) || strcmp(g_cfg.bt_name, dev->name)) {
            memcpy(g_cfg.bt_mac, dev->mac, BT_MAC_LEN);   /* (stock: CQO updates +0x23e/+0x328) */
            if (dev->name[0])
                snprintf(g_cfg.bt_name, sizeof g_cfg.bt_name, "%s", dev->name);
            g_cfg.bt_paired = 1;
            config_save_later();
        }
        break;
    case BT_EV_NO_MODULE:               /* seg84: timeout -> +0x34b = 0 */
        g_cfg.bt = 0;
        s_after_on = 0;
        config_save_later();
        break;
    case BT_EV_IDLE_OFF:                /* ram:03067a0c: +0x34a = +0x34b = 0. [ours] the setting stays
                                         * on and the next play wakes it (bt_wake): stock's off left a
                                         * device's later boots without Bluetooth (2026-10-17) */
    default:
        break;
    }
}

/* The USB DAC session (ap_ui.c): 1 = the host streams, 0 = it does not, -1 = no session (the
 * player's state). The module's audio comes from the DAC's analog output (GPIO2.0), the DAC's
 * own unmute keeps that path when connected (dac_cs43131.c), so the host's audio reaches the
 * earphones; the module needs only to be served, told the source is playing (CY0+30, else it
 * stays muted: the device, 2026-10-17, played the USB DAC on the jack only), and its keys sent
 * to the host. */
static int s_ext = -1;

void bt_app_external(int streaming)
{
    s_ext = streaming;
}

/* Earphone keys (services/bt_keys.h): play and pause each toggle, previous/next with a track
 * open. In a USB DAC session they are the host's media keys. [ours] stock gates pause to
 * playing and play to paused (seg84 0307b67c); the device's AirPods (2026-10-20) send CC and CP
 * by turns from their own memory, whatever C\x86/C\x87 told them (CC while playing at 20.1 s
 * and 77.1 s, CP 1 s after the player's pause at 59.8 s and 78.6 s): each was ignored and the
 * press after it acted, so a pause on the player made the next earphone press do nothing. */
void bt_keys_action(int action, uint32_t t_ms)
{
    int st = player_get_state();

    (void)t_ms;
    power_activity();
    if (s_ext >= 0 && action >= BT_KEY_PAUSE && action <= BT_KEY_NEXT) {
        if (action == BT_KEY_PAUSE || action == BT_KEY_PLAY)
            usb_uac_key(USB_UAC_PLAY_PAUSE);
        else
            usb_uac_key(action == BT_KEY_PREV ? USB_UAC_PREV : USB_UAC_NEXT);
        return;
    }
    switch (action) {
    case BT_KEY_PAUSE:
    case BT_KEY_PLAY:
        if (st == PLAYER_PLAYING)
            player_pause();
        else if (st == PLAYER_PAUSED)
            player_play();
        break;
    case BT_KEY_PREV:
        if (st == PLAYER_PLAYING || st == PLAYER_PAUSED)
            player_prev();              /* (the start of this track after 3 s, as seg33) */
        break;
    case BT_KEY_NEXT:
        if (st == PLAYER_PLAYING || st == PLAYER_PAUSED)
            player_next();
        break;
    case BT_KEY_PLAY_MODE:              /* (settings by sequence: services/bt_keys.h) */
        log_printf("bt: sequence: play mode %s", queue_mode_name(player_cycle_mode()));
        break;
    case BT_KEY_VOL_UP:
    case BT_KEY_VOL_DOWN:
        if (bt_routed()) {              /* the active volume is the BT volume (volume_follow keeps it) */
            int db = audio_get_volume_db() + (action == BT_KEY_VOL_UP ? BT_SEQ_DB : -BT_SEQ_DB);

            player_set_volume(db < -90 ? -90 : db > 0 ? 0 : db);
            log_printf("bt: sequence: BT volume %d dB", audio_get_volume_db());
        } else {
            log_printf("bt: sequence: BT volume: not routed to the module");
        }
        break;
    default:
        break;
    }
}

/* The separate BT volume (stock settings +0x15, "BT Volume"): connected -> the active volume
 * is the BT volume (seg84:0307b0f6 +0x12 = +0x15), disconnected -> the jack's again (+0x13). */
static void volume_follow(void)
{
    int routed = bt_routed();

    if (routed && !s_routed) {
        s_jack_db = audio_get_volume_db();
        s_bt_db = g_cfg.bt_volume == CFG_BT_VOLUME_UNSET ? s_jack_db : g_cfg.bt_volume;
        s_bt_db = audio_set_volume_db(s_bt_db);
        log_printf("bt: volume %d dB (BT), jack %d dB", s_bt_db, s_jack_db);
    } else if (!routed && s_routed) {
        audio_set_volume_db(s_jack_db);
        g_cfg.volume = (int16_t)s_jack_db;
        log_printf("bt: volume %d dB (jack)", s_jack_db);
    } else if (routed && audio_get_volume_db() != s_bt_db) {
        s_bt_db = audio_get_volume_db();   /* a volume key while connected: the BT volume */
        g_cfg.bt_volume = (int16_t)s_bt_db;
        g_cfg.volume = (int16_t)s_jack_db; /* (player_set_volume wrote the active one there) */
        config_save_later();
    }
    s_routed = routed;
}

void bt_app_init(void)
{
    bt_keys_init();
    if (g_cfg.bt) {                     /* boot auto-on (stock seg21 0307a4d2, from +0x34b) */
        log_printf("bt: on at boot%s", g_cfg.bt_paired ? ", reconnecting" : "");
        bt_autostart(g_cfg.bt_paired ? g_cfg.bt_mac : 0, g_cfg.bt_name);
    }
}

void bt_app_tick(void)
{
    static int s_live;                  /* the last tick did the work below */
    static int s_was_playing;
    int in_screen = nav_top() == &pg_bt;
    bt_state_t st = bt_state();
    int playing;

    /* Wanted and paired: a start of the source (a track, a USB DAC host streaming) brings a lost
     * link back (bt_wake: the module on again after its idle time, or a reconnect now). */
    if (g_cfg.bt && g_cfg.bt_paired) {
        playing = s_ext >= 0 ? s_ext > 0 : player_get_state() == PLAYER_PLAYING;
        if (playing && !s_was_playing)
            bt_wake(g_cfg.bt_mac, g_cfg.bt_name);
        s_was_playing = playing;
        st = bt_state();
    }

    /* Off (the default) and not on its screen: nothing can happen, so nothing is done -- the
     * work below (the player's state, the time, the framer) cost core A 3-4 instructions per
     * audio frame on every tick (m3, QEMU -icount 2) for a module that is not even powered. */
    if ((st == BT_OFF || st == BT_NO_MODULE) && !in_screen && !s_routed && !s_after_on) {
        if (s_live) {
            s_live = 0;
            bt_ui_active(0);
        }
        return;
    }
    s_live = 1;
    bt_ui_active(in_screen);
    bt_set_playing(s_ext >= 0 ? s_ext : player_get_state() == PLAYER_PLAYING, s_ext >= 0);
    bt_poll();
    bt_keys_poll(time_ms());
    volume_follow();
    st = bt_state();
    if (s_after_on && st != BT_STARTING) {
        s_after_on = 0;
        if (st == BT_ON) {
            if (g_cfg.bt_paired)
                bt_connect(g_cfg.bt_mac, g_cfg.bt_name);   /* seg84:0307a13e with +0x349 != 0 */
            else
                bt_scan();                                   /* seg84 0307a2a6: CS00 */
        }
    }
    if (bt_seq() != s_shown_seq) {      /* what the screen shows changed: its rows now */
        s_shown_seq = bt_seq();
        nav_live_now();
    }
}
