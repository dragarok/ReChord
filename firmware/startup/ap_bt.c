/*
 * ap_bt.c -- Bluetooth in the UI: the "Bluetooth" screen on the main menu, the
 * glue between drivers/bt_jieli.c and the player (earphone keys, the separate
 * BT volume, CY0 on play/pause) and the settings (on at boot, the remembered
 * device). Stock's behaviour it follows: work/stock_boot/08_bluetooth.md §1.3, §3.
 *
 * Screen rows:
 *   Bluetooth   off / starting / on / scanning / connecting / connected / no module
 *               (PLAY toggles the module; on: the remembered device is connected
 *               (stock seg84:0307a13e: CL06) or, with none, a scan starts (CS00))
 *   <remembered device>   connected / connecting / remembered (PLAY: connect)
 *   Scan        found N (PLAY: CS00, 3 s)
 *   <found devices>       up to 6 (PLAY: connect: CL06 + MAC)
 *   Disconnect  (connected: CT00)
 *   Forget      (a remembered device: our settings only -- stock sends the module
 *               nothing to forget, seg84:0307ae40)
 */
#include <stdio.h>
#include <string.h>

#include "drivers/bt_jieli.h"
#include "drivers/clock.h"
#include "drivers/display.h"
#include "services/bt_keys.h"
#include "services/config.h"
#include "services/log.h"
#include "services/power.h"
#include "audio/audio.h"
#include "player.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "ui/screens/screens.h"

enum { ROW_POWER, ROW_PAIRED, ROW_SCAN, ROW_DEV, ROW_DISCONNECT, ROW_FORGET };

typedef struct {
    uint8_t kind, idx;
    char label[40];
    char value[16];
} bt_row_t;

#define ROWS (4 + BT_SCAN_MAX)
static bt_row_t s_rows[ROWS];
static int s_nrows;
static menu_view_t s_view;
static uint32_t s_shown_seq;
static int s_after_on;                  /* the screen turned it on: connect or scan once it is up */
static int s_routed;                    /* the volume is the BT volume now */
static int s_jack_db, s_bt_db;

extern const ui_screen_t bt_screen;

static int same_mac(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, BT_MAC_LEN); }

static void dev_label(char *out, int cap, const uint8_t *mac, const char *name)
{
    char m[18];

    bt_mac_text(mac, m);
    snprintf(out, (unsigned)cap, "%s", name && name[0] ? name : m);
}

static void build(void)
{
    bt_state_t st = bt_state();
    const bt_dev_t *peer = bt_peer();
    bt_row_t *r;

    s_nrows = 0;
    r = &s_rows[s_nrows++];
    r->kind = ROW_POWER;
    snprintf(r->label, sizeof r->label, "Bluetooth");
    snprintf(r->value, sizeof r->value, "%s", bt_state_name(st));
    if (g_cfg.bt_paired) {
        int here = (st == BT_CONNECTED || st == BT_CONNECTING) && same_mac(peer->mac, g_cfg.bt_mac);

        r = &s_rows[s_nrows++];
        r->kind = ROW_PAIRED;
        dev_label(r->label, sizeof r->label, g_cfg.bt_mac, g_cfg.bt_name);
        snprintf(r->value, sizeof r->value, "%s", here ? bt_state_name(st) : "remembered");
    }
    r = &s_rows[s_nrows++];
    r->kind = ROW_SCAN;
    snprintf(r->label, sizeof r->label, "Scan");
    if (st == BT_SCANNING)
        snprintf(r->value, sizeof r->value, "scanning");
    else if (bt_scan_count())
        snprintf(r->value, sizeof r->value, "found %d", bt_scan_count());
    else
        r->value[0] = 0;
    for (int i = 0; i < bt_scan_count() && s_nrows < ROWS - 2; i++) {
        const bt_dev_t *d = bt_scan_dev(i);
        int here = (st == BT_CONNECTED || st == BT_CONNECTING) && same_mac(peer->mac, d->mac);

        if (g_cfg.bt_paired && same_mac(d->mac, g_cfg.bt_mac))
            continue;                   /* (the remembered row has it) */
        r = &s_rows[s_nrows++];
        r->kind = ROW_DEV;
        r->idx = (uint8_t)i;
        dev_label(r->label, sizeof r->label, d->mac, d->name);
        snprintf(r->value, sizeof r->value, "%s", here ? bt_state_name(st) : "");
    }
    if (st == BT_CONNECTED || st == BT_CONNECTING) {
        r = &s_rows[s_nrows++];
        r->kind = ROW_DISCONNECT;
        snprintf(r->label, sizeof r->label, "Disconnect");
        r->value[0] = 0;
    }
    if (g_cfg.bt_paired) {
        r = &s_rows[s_nrows++];
        r->kind = ROW_FORGET;
        snprintf(r->label, sizeof r->label, "Forget");
        r->value[0] = 0;
    }
    if (s_view.cursor >= s_nrows)
        s_view.cursor = s_nrows - 1;
}

static void draw(void)
{
    int page = menu_page_size();

    build();
    display_clear(THEME_COLOR_BG);
    display_fill_rect(0, 0, DISPLAY_W - 1, THEME_TITLE_H - 1, THEME_COLOR_TITLE_BG);
    display_draw_text(THEME_PAD, THEME_TITLE_TEXT_Y + THEME_TEXT_YOFF, "Bluetooth",
                      THEME_COLOR_ACCENT, THEME_COLOR_TITLE_BG);
    for (int row = 0; row < page && s_view.top + row < s_nrows; row++) {
        const bt_row_t *r = &s_rows[s_view.top + row];

        menu_draw_item(THEME_MENU_TOP + row * THEME_ITEM_H, r->label, r->value,
                       s_view.top + row == s_view.cursor);
    }
}

static void select_row(const bt_row_t *r)
{
    bt_state_t st = bt_state();

    log_printf("bt: ui: %s", r->label);
    switch (r->kind) {
    case ROW_POWER:
        if (st == BT_OFF || st == BT_NO_MODULE) {
            g_cfg.bt = 1;               /* stock's toggle: settings +0x34b */
            s_after_on = 1;
            bt_power(1);
        } else {
            g_cfg.bt = 0;
            s_after_on = 0;
            bt_power(0);
        }
        config_save_later();
        break;
    case ROW_PAIRED:
        if (st == BT_OFF || st == BT_NO_MODULE) {
            g_cfg.bt = 1;
            s_after_on = 1;
            bt_power(1);
            config_save_later();
        } else if (st == BT_ON || st == BT_SCANNING) {
            bt_connect(g_cfg.bt_mac, g_cfg.bt_name);
        }
        break;
    case ROW_SCAN:
        bt_scan();
        break;
    case ROW_DEV: {
        const bt_dev_t *d = bt_scan_dev(r->idx);

        if (d)
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
    ui_invalidate_all();
}

static void handle(const input_event_t *ev)
{
    build();
    switch (ev->type) {
    case KEY_UP:
    case KEY_DOWN:
        menu_view_move(&s_view, ev->type == KEY_UP ? -1 : 1, s_nrows, menu_page_size());
        ui_invalidate_all();
        break;
    case KEY_SELECT:
        if (ev->adc_value != INPUT_ADC_REPEAT && s_view.cursor >= 0 && s_view.cursor < s_nrows)
            select_row(&s_rows[s_view.cursor]);
        break;
    case KEY_BACK:
        ui_pop_screen();
        break;
    default:
        break;
    }
}

static void enter(void)
{
    menu_view_init(&s_view);
}

const ui_screen_t bt_screen = { "bluetooth", enter, NULL, draw, handle };

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
    case BT_EV_IDLE_OFF:                /* ram:03067a0c: +0x34a = +0x34b = 0 */
        g_cfg.bt = 0;
        s_after_on = 0;
        config_save_later();
        break;
    default:
        break;
    }
}

/* Earphone keys (services/bt_keys.h): stock's gates -- pause only while playing,
 * play only while paused (seg84 0307b67c), previous/next with a track open. */
void bt_keys_action(int action, uint32_t t_ms)
{
    int st = player_get_state();

    (void)t_ms;
    power_activity();
    switch (action) {
    case BT_KEY_PAUSE:
        if (st == PLAYER_PLAYING)
            player_pause();
        break;
    case BT_KEY_PLAY:
        if (st == PLAYER_PAUSED)
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
    int in_screen = ui_top_screen() == &bt_screen;
    bt_state_t st = bt_state();

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
    bt_set_playing(player_get_state() == PLAYER_PLAYING);
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
    if (bt_seq() != s_shown_seq) {
        s_shown_seq = bt_seq();
        if (ui_top_screen() == &bt_screen)
            ui_invalidate_all();
    }
}
