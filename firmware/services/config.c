/*
 * config.c -- RECHORD.CFG (config.h). The file is rewritten only when its text
 * changes (an FNV-1a hash of what was last read or written).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "fs.h"
#include "log.h"
#include "queue.h"
#include "drivers/clock.h"

config_t g_cfg;

static const char *const k_play[] = { "normal", "shuffle", "repeat_track", "repeat_all" };
static const uint16_t k_screen_off[] = { 10, 30, 60, 300, 0 };
static const uint8_t k_auto_off[] = { 5, 15, 30, 0 };
static const char *const k_usb[] = { "ask", "disk", "dac", "charge" };
static const char *const k_resume[] = { "off", "folder", "track" };
static const char *const k_off_on[] = { "off", "on" };
static const char *const k_dac_charge[] = { "off", "on", "power" };   /* CFG_DAC_* */
static const char *const k_usb_vbus[] = { "port", "gpio2.1" };       /* USB_OUT_SUPPLY_* */
static const char *const k_cpu[] = { "auto", "max" };

static uint32_t s_hash, s_save_at;

void config_defaults(void)
{
    g_cfg.play = Q_NORMAL;
    g_cfg.brightness = 8;
    g_cfg.screen_off = 1;                    /* 30 s */
    g_cfg.auto_off = 1;                      /* 15 min */
    g_cfg.usb = CFG_USB_ASK;
    g_cfg.resume = CFG_RESUME_FOLDER;
    g_cfg.strip = 0;
    g_cfg.tags = 1;
    g_cfg.cpu = 0;
    g_cfg.dac_mhz = 50;                      /* the DAC needs ~7-12 % of it (QEMU profile) */
    g_cfg.dac_charge = CFG_DAC_BATTERY;
    g_cfg.usb_out = 0;                       /* until the board's VBUS supply is known (USB-C probe) */
    g_cfg.usb_vbus = 0;      /* stock's default: the phone's battery is not the player's charger */
}

void config_reset(void)
{
    memset(&g_cfg, 0, sizeof g_cfg);
    config_defaults();
    g_cfg.volume = -40;
}

uint32_t config_screen_off_s(void) { return k_screen_off[g_cfg.screen_off % 5u]; }
uint32_t config_auto_off_min(void) { return k_auto_off[g_cfg.auto_off % 4u]; }

static int pick(const char *v, const char *const *opts, int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(v, opts[i]))
            return i;
    return -1;
}

static void set(const char *k, const char *v)
{
    int i;

    if (!strcmp(k, "play") && (i = pick(v, k_play, 4)) >= 0)
        g_cfg.play = (uint8_t)i;
    else if (!strcmp(k, "brightness") && (i = atoi(v)) >= 1 && i <= 8)
        g_cfg.brightness = (uint8_t)i;
    else if (!strcmp(k, "screen_off")) {
        int s = !strcmp(v, "never") ? 0 : atoi(v);

        for (i = 0; i < 5; i++)
            if (k_screen_off[i] == s && (s || !strcmp(v, "never") || !strcmp(v, "0")))
                g_cfg.screen_off = (uint8_t)i;
    } else if (!strcmp(k, "auto_off")) {
        int m = !strcmp(v, "never") ? 0 : atoi(v);

        for (i = 0; i < 4; i++)
            if (k_auto_off[i] == m && (m || !strcmp(v, "never") || !strcmp(v, "0")))
                g_cfg.auto_off = (uint8_t)i;
    } else if (!strcmp(k, "usb") && (i = pick(v, k_usb, 4)) >= 0)
        g_cfg.usb = (uint8_t)i;
    else if (!strcmp(k, "resume") && (i = pick(v, k_resume, 3)) >= 0)
        g_cfg.resume = (uint8_t)i;
    else if (!strcmp(k, "strip") && (i = pick(v, k_off_on, 2)) >= 0)
        g_cfg.strip = (uint8_t)i;
    else if (!strcmp(k, "tags") && (i = pick(v, k_off_on, 2)) >= 0)
        g_cfg.tags = (uint8_t)i;
    else if (!strcmp(k, "cpu") && (i = pick(v, k_cpu, 2)) >= 0)
        g_cfg.cpu = (uint8_t)i;
    else if (!strcmp(k, "usb_out") && (i = pick(v, k_off_on, 2)) >= 0)
        g_cfg.usb_out = (uint8_t)i;
    else if (!strcmp(k, "usb_vbus") && (i = pick(v, k_usb_vbus, 2)) >= 0)
        g_cfg.usb_vbus = (uint8_t)i;
    else if (!strcmp(k, "dac_charge") && (i = pick(v, k_dac_charge, 3)) >= 0)
        g_cfg.dac_charge = (uint8_t)i;
    else if (!strcmp(k, "dac_mhz") && ((i = atoi(v)) == 50 || i == 100 || i == 200))
        g_cfg.dac_mhz = (uint8_t)i;
    else if (!strcmp(k, "volume")) {
        if (!strcmp(v, "mute"))
            g_cfg.volume = CFG_VOLUME_MUTE;
        else if ((v[0] == '-' || (v[0] >= '0' && v[0] <= '9')) && (i = atoi(v)) >= -90 && i <= 0)
            g_cfg.volume = (int16_t)i;
    } else if (!strcmp(k, "browse"))
        snprintf(g_cfg.browse, sizeof g_cfg.browse, "%s", v);
    else if (!strcmp(k, "cursor"))
        g_cfg.cursor = (uint16_t)atoi(v);
    else if (!strcmp(k, "track"))
        snprintf(g_cfg.track, sizeof g_cfg.track, "%s", v);
}

static uint32_t fnv(const char *s, unsigned long n)
{
    uint32_t h = 2166136261u;

    while (n--)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

int config_load(void)
{
    char text[1024], *p, *e;
    fs_file_t *f;
    long n;

    config_reset();
    f = fs_open_read(CONFIG_PATH);
    if (!f)
        return -1;
    n = fs_read(f, text, sizeof text - 1);
    fs_close(f);
    if (n < 0)
        return -2;
    text[n] = 0;
    s_hash = fnv(text, (unsigned long)n);
    for (p = text; *p; p = e) {
        char *eq, *nl = strchr(p, '\n');

        e = nl ? nl + 1 : p + strlen(p);
        if (nl) {
            *nl = 0;
            if (nl > p && nl[-1] == '\r')
                nl[-1] = 0;
        }
        if (*p == '#' || !(eq = strchr(p, '=')))
            continue;
        *eq = 0;
        set(p, eq + 1);
    }
    return 0;
}

static int text_of(char *t, unsigned long cap)
{
    char so[8], ao[8], vol[8];

    if (k_screen_off[g_cfg.screen_off % 5u])
        snprintf(so, sizeof so, "%u", k_screen_off[g_cfg.screen_off % 5u]);
    else
        snprintf(so, sizeof so, "never");
    if (k_auto_off[g_cfg.auto_off % 4u])
        snprintf(ao, sizeof ao, "%u", k_auto_off[g_cfg.auto_off % 4u]);
    else
        snprintf(ao, sizeof ao, "never");
    if (g_cfg.volume == CFG_VOLUME_MUTE)
        snprintf(vol, sizeof vol, "mute");
    else
        snprintf(vol, sizeof vol, "%d", g_cfg.volume);
    return snprintf(t, cap,
                    "# ReChord settings (RECHO's format): key=value, one per line. Unknown keys are ignored.\n"
                    "play=%s\nbrightness=%u\nscreen_off=%s\nauto_off=%s\nusb=%s\nresume=%s\nstrip=%s\n"
                    "tags=%s\ncpu=%s\ndac_mhz=%u\ndac_charge=%s\nusb_out=%s\nusb_vbus=%s\nvolume=%s\n"
                    "# where the browser and the player were\nbrowse=%s\ncursor=%u\n%s%s%s",
                    k_play[g_cfg.play % 4u], g_cfg.brightness, so, ao, k_usb[g_cfg.usb % 4u],
                    k_resume[g_cfg.resume % 3u], k_off_on[g_cfg.strip & 1u], k_off_on[g_cfg.tags & 1u],
                    k_cpu[g_cfg.cpu & 1u], g_cfg.dac_mhz, k_dac_charge[g_cfg.dac_charge % 3u],
                    k_off_on[g_cfg.usb_out & 1u], k_usb_vbus[g_cfg.usb_vbus & 1u], vol, g_cfg.browse, g_cfg.cursor,
                    g_cfg.track[0] ? "track=" : "", g_cfg.track, g_cfg.track[0] ? "\n" : "");
}

int config_save(void)
{
    char t[1024];
    int n = text_of(t, sizeof t);
    uint32_t h;
    fs_file_t *f;
    long w;

    s_save_at = 0;
    if (n < 0 || n >= (int)sizeof t)
        return -1;
    h = fnv(t, (unsigned long)n);
    if (h == s_hash)
        return 0;                            /* unchanged: no eMMC write */
    f = fs_open_write(CONFIG_PATH);
    if (!f)
        return -2;
    w = fs_write(f, t, (unsigned long)n);
    if (fs_close(f) != 0 || w != n) {
        log_printf("cfg: save failed (%ld)", w);
        return -3;
    }
    s_hash = h;
    log_printf("cfg: saved (%d B)", n);
    return 0;
}

void config_save_later(void)
{
    s_save_at = time_ms() + 5000u;
    if (!s_save_at)
        s_save_at = 1;
}

void config_poll(void)
{
    if (s_save_at && (int32_t)(time_ms() - s_save_at) >= 0)
        config_save();
}
