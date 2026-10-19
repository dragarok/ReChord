/*
 * eq_page.c -- Settings > Playback > EQ > Custom (ui/screens.c pg_eq): ten bands, a bar each; VOL+/VOL-
 * the band's gain (-12..+12 dB, and the EQ to Custom), PLAY the next band, PLAY held the band to 0 dB.
 * Each change goes to core B at once (audio_eq: crossfaded) and is saved 5 s later.
 *
 * In the usbout overlay (firmware_ap.ld, startup/ap_ovl.c ap_eq): core A's resident
 * code is full, and USB-C earphones keep that overlay in the window while they are on, so the editor
 * never evicts them (any other overlay would, every main-loop pass).
 */
#include <stdio.h>

#include "nav.h"
#include "ui.h"
#include "audio/audio.h"
#include "services/config.h"

static uint8_t s_band;

static void draw(void)
{
    static const char *const hz[10] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
    const theme_t *t = T;
    char v[24];

    ui_fill(0, BODY_Y, DISPLAY_W, DISPLAY_H - BODY_Y, t->background);
    snprintf(v, sizeof v, "%s Hz   %s%d dB", hz[s_band], g_cfg.eq_gain[s_band] > 0 ? "+" : "", g_cfg.eq_gain[s_band]);
    ui_text(&font_b13, 16, 44, 288, 1, v, t->text, 30, 48);
    for (int k = 0; k < 10; k++) {                 /* 0 dB at y 96, 10 px every 3 dB */
        int x = 20 + 29 * k, g = g_cfg.eq_gain[k], h;

        g = g < -12 ? -12 : g > 12 ? 12 : g;           /* (a hand-edited RECHORD.CFG) */
        h = (g < 0 ? -g : g) * 10 / 3;

        ui_fill(x + 9, 56, 3, 81, k == s_band ? t->text_dim : t->bar_track);   /* (the band being edited: */
        ui_fill(x + 5, g > 0 ? 96 - h : 96, 11, h ? h : 1, k == s_band ? t->text : t->bar_fill);   /* bright) */
        ui_text(&font_s8, x, 152, 21, 1, hz[k], k == s_band ? t->text : t->text_dim, 142, 154);
    }
}

/* the page's act (a = A_*), and its draw (a = -1): one entry (startup/ap_ovl.c ap_eq) */
void eq_page(int a, int i)
{
    int8_t *g = &g_cfg.eq_gain[s_band];

    if (a < 0) {
        draw();
        return;
    }
    if (a == A_DONE) {
        s_band = (uint8_t)((s_band + 1u) % 10u);
    } else if (a == A_VOL_UP || a == A_VOL_DN || a == A_DEFAULT) {
        int v = *g < -12 ? -12 : *g > 12 ? 12 : *g;

        *g = (int8_t)(a == A_DEFAULT ? 0 : a == A_VOL_UP ? (v < 12 ? v + 1 : 12) : (v > -12 ? v - 1 : -12));
        g_cfg.eq = 7;                              /* (Custom: what is edited is what is heard) */
        audio_eq();
        config_save_later();
    } else {
        if (a == A_CANCEL)
            nav_pop();
        return;
    }
    ui_invalidate(0, BODY_Y, DISPLAY_W - 1, DISPLAY_H - 1);
}

