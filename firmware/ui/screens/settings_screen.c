/*
 * settings_screen.c -- Settings, as RECHO has them (docs/RECHO_POWER_CFG.md D):
 * PLAY steps the highlighted value forward (wrapping), MENU/BACK save
 * RECHORD.CFG and leave. Values live in g_cfg (services/config.h).
 */
#include <stdio.h>
#include <string.h>

#include "screens.h"
#include "../theme.h"
#include "../internal/hal.h"
#include "services/config.h"
#include "services/queue.h"

void settings_apply(void);                   /* ap_ui.c: brightness, strip, CPU */

static char s_val[24];
static menu_view_t s_view;
static int s_restored;

static const char *v_play(void) { return queue_mode_name(queue_mode()); }
static const char *v_tags(void) { return g_cfg.tags ? "on" : "off"; }
static const char *v_bright(void) { snprintf(s_val, sizeof s_val, "%u / 8", g_cfg.brightness); return s_val; }
static const char *v_screen(void)
{
    static const char *const k[] = { "10 s", "30 s", "1 min", "5 min", "never" };
    return k[g_cfg.screen_off % 5u];
}
static const char *v_auto(void)
{
    if (!config_auto_off_min())
        return "never";
    snprintf(s_val, sizeof s_val, "%u min", (unsigned)config_auto_off_min());
    return s_val;
}
static const char *v_usb(void)
{
    static const char *const k[] = { "ask", "disk", "DAC", "charge only" };
    return k[g_cfg.usb % 4u];
}
static const char *v_dacpow(void)
{
    static const char *const k[] = { "battery", "charge", "power only" };
    return k[g_cfg.dac_charge % 3u];
}
static const char *v_usbout(void) { return g_cfg.usb_out ? "on" : "off"; }
static const char *v_usbvbus(void) { return g_cfg.usb_vbus ? "GPIO2.1" : "port"; }
static const char *v_resume(void)
{
    static const char *const k[] = { "off", "folder", "track" };
    return k[g_cfg.resume % 3u];
}
static const char *v_strip(void) { return g_cfg.strip ? "on" : "off"; }
static const char *v_cpu(void) { return g_cfg.cpu ? "max" : "auto"; }
static const char *v_restore(void) { return s_restored ? "done" : NULL; }
static const char *v_about(void) { return "ReChord echo512"; }

static void s_play(void)
{
    queue_set_mode((queue_mode() + 1) % Q_MODES);
    g_cfg.play = (uint8_t)queue_mode();
}
static void s_tags(void) { g_cfg.tags ^= 1u; }
static void s_bright(void) { g_cfg.brightness = (uint8_t)((g_cfg.brightness & 7u) + 1u); }
static void s_screen(void) { g_cfg.screen_off = (uint8_t)((g_cfg.screen_off + 1u) % 5u); }
static void s_auto(void) { g_cfg.auto_off = (uint8_t)((g_cfg.auto_off + 1u) & 3u); }
static void s_usb(void) { g_cfg.usb = (uint8_t)((g_cfg.usb + 1u) & 3u); }
/* battery -> power only -> charge -> battery */
static void s_dacpow(void)
{
    static const uint8_t next[3] = { CFG_DAC_POWER_ONLY, CFG_DAC_BATTERY, CFG_DAC_CHARGE };
    g_cfg.dac_charge = next[g_cfg.dac_charge % 3u];
}
static void s_usbout(void) { g_cfg.usb_out ^= 1u; }
static void s_usbvbus(void) { g_cfg.usb_vbus ^= 1u; }
static void s_resume(void) { g_cfg.resume = (uint8_t)((g_cfg.resume + 1u) % 3u); }
static void s_strip(void) { g_cfg.strip ^= 1u; }
static void s_cpu(void) { g_cfg.cpu ^= 1u; }
static void s_restore(void)
{
    config_defaults();                       /* not volume, browse or track (as RECHO) */
    queue_set_mode(g_cfg.play);
    s_restored = 1;
}

static const menu_item_t k_items[] = {
    { "Play mode", v_play, s_play },
    { "Show tags", v_tags, s_tags },
    { "Brightness", v_bright, s_bright },
    { "Screen off after", v_screen, s_screen },
    { "Auto power off", v_auto, s_auto },
    { "On USB connect", v_usb, s_usb },
    { "DAC power", v_dacpow, s_dacpow },
    { "USB-C earphones", v_usbout, s_usbout },
    { "USB-C power", v_usbvbus, s_usbvbus },
    { "Resume at power-on", v_resume, s_resume },
    { "Debug strip", v_strip, s_strip },
    { "CPU clock", v_cpu, s_cpu },
    { "Restore defaults", v_restore, s_restore },
    { "About", v_about, NULL },
};
static const menu_model_t k_model = { "Settings", k_items, sizeof k_items / sizeof k_items[0] };

void settings_screen_reload(void)
{
    s_restored = 0;
}

void settings_screen_store(void)
{
    config_save();
}

static void settings_screen_on_enter(void)
{
    menu_view_init(&s_view);
    s_restored = 0;
}

static void settings_screen_on_exit(void)
{
    config_save();
}

static void settings_screen_draw(void)
{
    menu_draw(&k_model, &s_view);
}

static void settings_screen_handle_event(const input_event_t *ev)
{
    switch (ev->type) {
    case KEY_UP:
        menu_view_step(&s_view, -1, k_model.item_count);
        break;
    case KEY_DOWN:
        menu_view_step(&s_view, +1, k_model.item_count);
        break;
    case KEY_SELECT:
        if (ev->adc_value == INPUT_ADC_LONG || !k_items[s_view.cursor].on_select)
            break;
        k_items[s_view.cursor].on_select();
        settings_apply();
        menu_screen_invalidate_item(s_view.cursor);
        ui_invalidate_all();
        break;
    case KEY_BACK:
        ui_pop_screen();                     /* on_exit saves */
        break;
    default:
        break;
    }
}

const ui_screen_t settings_screen = {
    "settings",
    settings_screen_on_enter,
    settings_screen_on_exit,
    settings_screen_draw,
    settings_screen_handle_event,
};

const menu_model_t *settings_screen_model(void) { return &k_model; }
const menu_view_t *settings_screen_view(void) { return &s_view; }
