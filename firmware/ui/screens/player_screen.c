/*
 * player_screen.c — now-playing skeleton: track title, progress bar, volume.
 *
 * Pure presentation + local state: the audio side (B core / player module)
 * feeds it through the setters; keys adjust the local view (volume up/down,
 * play/pause toggle) which the player module will mirror to the audio
 * service later.
 */
#include <stdio.h>
#include <string.h>

#include "screens.h"
#include "services/queue.h"
#include "services/log.h"
#include "../theme.h"
#include "../internal/hal.h"

#define PLAYER_BAR_X0   THEME_PAD
#define PLAYER_BAR_X1   (DISPLAY_W - 1 - THEME_PAD)
#define PLAYER_BAR_W    (PLAYER_BAR_X1 - PLAYER_BAR_X0 + 1)
#define PLAYER_BAR_Y    78
#define PLAYER_BAR_H    10

#define PLAYER_VOL_Y    122
#define PLAYER_VOL_H    6

static char     s_title[PLAYER_TITLE_MAX];
static char     s_artist[PLAYER_TITLE_MAX];   /* from the tags; "" = none */
static uint32_t s_duration_s;
static uint32_t s_position_s;
static int      s_volume_db;   /* -60..0 */
static bool     s_playing;
static char     s_status[40];  /* non-empty: shown instead of Playing/Paused */

/* ---- pure geometry helper (host-tested) -------------------------------- */

int player_bar_width(int width, uint32_t pos, uint32_t dur)
{
    if (width <= 0 || dur == 0 || pos == 0)
        return 0;
    if (pos >= dur)
        return width;
    return (int)(((uint64_t)width * pos) / dur);
}

/* ---- local helpers ------------------------------------------------------ */

static void format_time(char *buf, int cap, uint32_t sec)
{
    snprintf(buf, (size_t)cap, "%u:%02u",
             (unsigned)(sec / 60u), (unsigned)(sec % 60u));
}

static void draw_bar(int y, int h, int fill_w)
{
    display_fill_rect(PLAYER_BAR_X0, y, PLAYER_BAR_X1, y + h - 1,
                      THEME_COLOR_BAR_BG);
    if (fill_w > 0) {
        if (fill_w > PLAYER_BAR_W)
            fill_w = PLAYER_BAR_W;
        display_fill_rect(PLAYER_BAR_X0, y,
                          PLAYER_BAR_X0 + fill_w - 1, y + h - 1,
                          THEME_COLOR_BAR_FG);
    }
}

/* ---- screen vtable ------------------------------------------------------ */

static void player_screen_draw(void)
{
    char line[PLAYER_TITLE_MAX + 16];
    char t_now[16];
    char t_end[16];
    int  vol_w;

    display_clear(THEME_COLOR_BG);

    /* title bar */
    display_fill_rect(0, 0, DISPLAY_W - 1, THEME_TITLE_H - 1,
                      THEME_COLOR_TITLE_BG);
    display_draw_text(THEME_PAD, THEME_TITLE_TEXT_Y + THEME_TEXT_YOFF,
                      "Now playing", THEME_COLOR_ACCENT, THEME_COLOR_TITLE_BG);

    /* track title (truncated to fit one line) */
    snprintf(line, sizeof(line), "%s", s_title);
    display_draw_text(THEME_PAD, 32 + THEME_TEXT_YOFF, line,
                      THEME_COLOR_FG, THEME_COLOR_BG);
    if (s_artist[0])
        display_draw_text(THEME_PAD, 46 + THEME_TEXT_YOFF, s_artist,
                          THEME_COLOR_DIM, THEME_COLOR_BG);

    /* play state (or a status line: unsupported file, no renderer, ...) */
    {
        const char *state_line = s_status[0] ? s_status
                                             : (s_playing ? "Playing" : "Paused");
        display_draw_text(THEME_PAD, (s_artist[0] ? 61 : 48) + THEME_TEXT_YOFF, state_line,
                          s_status[0] ? THEME_COLOR_WARM
                                      : (s_playing ? THEME_COLOR_WARM
                                                   : THEME_COLOR_DIM),
                          THEME_COLOR_BG);
    }

    /* progress bar + times */
    draw_bar(PLAYER_BAR_Y, PLAYER_BAR_H,
             player_bar_width(PLAYER_BAR_W, s_position_s, s_duration_s));
    format_time(t_now, (int)sizeof(t_now), s_position_s);
    format_time(t_end, (int)sizeof(t_end), s_duration_s);
    snprintf(line, sizeof(line), "%s / %s", t_now, t_end);
    display_draw_text(THEME_PAD, PLAYER_BAR_Y + PLAYER_BAR_H + 4,
                      line, THEME_COLOR_DIM, THEME_COLOR_BG);

    /* volume bar (-60..0 dB maps to 0..full width) */
    display_draw_text(THEME_PAD, PLAYER_VOL_Y - THEME_LINE_H + 2,
                      "Volume", THEME_COLOR_DIM, THEME_COLOR_BG);
    vol_w = (s_volume_db + 60) * PLAYER_BAR_W / 60;
    if (vol_w < 0)
        vol_w = 0;
    draw_bar(PLAYER_VOL_Y, PLAYER_VOL_H, vol_w);
    snprintf(line, sizeof(line), "%d dB", s_volume_db);
    display_draw_text(DISPLAY_W - THEME_PAD - THEME_TEXT_W((int)strlen(line)),
                      PLAYER_VOL_Y - THEME_LINE_H + 2,
                      line, THEME_COLOR_FG, THEME_COLOR_BG);
}

/* RECHO's keys: VOL+/VOL- volume (held: repeats); PLAY pause/resume, held: next
 * track; BACK previous track (or the start of this one after 3 s), held: next
 * play mode. MENU (ap_ui.c): Files; held: the main menu. */
static void player_screen_handle_event(const input_event_t *ev)
{
    int hold = ev->adc_value == INPUT_ADC_LONG;

    switch (ev->type) {
    case KEY_UP:
    case KEY_DOWN:
        player_set_volume(player_get_volume() + (ev->type == KEY_UP ? 1 : -1));
        player_screen_set_volume(player_get_volume());
        break;
    case KEY_SELECT:
        if (hold) {
            player_next();
        } else if (s_status[0]) {
            /* an error view: PLAY tries again (it used to do nothing, so one failure looked like
             * "cannot play" on every later press, with nothing in the log; device 2026-10-16) */
            track_info_t ti;
            int rc;

            if (player_get_track(&ti) == PLAYER_OK) {
                player_screen_set_status(NULL);
                rc = player_play();
                log_printf("player: retry from the error view: %d", rc);
                if (rc != PLAYER_OK) {
                    char eb[32];

                    snprintf(eb, sizeof eb, "cannot play (%d)", rc);
                    player_screen_set_status(eb);
                    log_flush("error");
                }
            }
        } else {
            if (s_playing)
                player_pause();
            else
                player_play();
        }
        player_screen_set_playing(player_get_state() == PLAYER_PLAYING);
        break;
    case KEY_BACK:
        if (hold) {
            char m[32];

            snprintf(m, sizeof m, "play mode: %s", queue_mode_name(player_cycle_mode()));
            player_screen_set_status(m);
        } else {
            player_prev();
        }
        break;
    default:
        break;
    }
}

/* Sync the screen from the player module when the screen is (re)entered. */
static void player_screen_on_enter(void)
{
    track_info_t ti;

    if (s_status[0])
        return;                       /* error view: keep what's shown */
    if (player_get_track(&ti) != PLAYER_OK) {
        player_screen_set_status("nothing playing");
        return;
    }
    player_screen_set_track(ti.title,
                            ti.duration_ms > 0
                                ? (uint32_t)(ti.duration_ms / 1000) : 0);
    player_screen_set_artist(ti.artist);
    player_screen_set_volume(player_get_volume());
    player_screen_set_playing(player_get_state() == PLAYER_PLAYING);
}

const ui_screen_t player_screen = {
    "player",
    player_screen_on_enter,
    NULL,
    player_screen_draw,
    player_screen_handle_event,
};

void player_screen_set_track(const char *title, uint32_t duration_s)
{
    snprintf(s_title, sizeof(s_title), "%s", title ? title : "");
    s_duration_s = duration_s;
    s_position_s = 0;
    ui_invalidate_all();
}

void player_screen_set_artist(const char *artist)
{
    snprintf(s_artist, sizeof(s_artist), "%s", artist ? artist : "");
    ui_invalidate_all();
}

void player_screen_set_position(uint32_t position_s)
{
    s_position_s = position_s;
    ui_invalidate(0, PLAYER_BAR_Y, DISPLAY_W - 1,
                  PLAYER_BAR_Y + PLAYER_BAR_H + THEME_LINE_H);
}

void player_screen_set_volume(int volume_db)
{
    if (volume_db < -60)
        volume_db = -60;
    if (volume_db > 0)
        volume_db = 0;
    s_volume_db = volume_db;
    ui_invalidate(0, PLAYER_VOL_Y - THEME_LINE_H,
                  DISPLAY_W - 1, PLAYER_VOL_Y + PLAYER_VOL_H);
}

void player_screen_set_playing(bool playing)
{
    s_playing = playing;
    ui_invalidate(0, 48, DISPLAY_W - 1, 48 + THEME_LINE_H);
}

void player_screen_set_status(const char *msg)
{
    snprintf(s_status, sizeof(s_status), "%s", msg ? msg : "");
    ui_invalidate(0, 48, DISPLAY_W - 1, 48 + THEME_LINE_H);
}

/* Pulled from the app loop every pass: updates position + play state from
 * the player module (no-ops without a track; redraws only on change). */
void player_screen_refresh(void)
{
    track_info_t ti;
    uint32_t pos;
    int st;

    if (player_get_track(&ti) != PLAYER_OK) {
        if (!s_status[0])
            player_screen_set_status("nothing playing");
        return;
    }
    st = player_get_state();
    if (st == PLAYER_ERROR) {
        if (!s_status[0])
            player_screen_set_status("no audio renderer");
        if (s_playing)
            player_screen_set_playing(false);
        return;
    }
    /* gapless: the next track took over without an open from the browser */
    if (ti.title[0] && (strncmp(ti.title, s_title, sizeof s_title - 1) ||
                        (ti.duration_ms > 0 && (uint32_t)(ti.duration_ms / 1000) != s_duration_s)))
        player_screen_set_track(ti.title, ti.duration_ms > 0 ? (uint32_t)(ti.duration_ms / 1000) : 0);
    if (strcmp(ti.artist, s_artist))
        player_screen_set_artist(ti.artist);
    pos = (uint32_t)(player_get_position_ms() / 1000);
    if (pos != s_position_s)
        player_screen_set_position(pos);
    if ((st == PLAYER_PLAYING) != s_playing)
        player_screen_set_playing(st == PLAYER_PLAYING);
}
