/*
 * browser_screen.c -- the file browser: folders first, then files, natural order
 * (services/dirlist.c); "/" lists the volumes. A folder of more than DL_MAX
 * entries is shown a page at a time (DOWN past the end / UP past the top).
 *
 *   UP/DOWN        move
 *   SELECT         open a folder; on a file, play its folder from it
 *   SELECT (hold)  on a folder: play it and everything under it
 *   BACK           parent folder (leaves the browser at its root)
 */
#include <stdio.h>
#include <string.h>

#include "screens.h"
#include "../theme.h"
#include "../internal/hal.h"
#include "services/dirlist.h"
#include "services/fs_target.h"
#include "services/log.h"

static browser_state_t s_state;
static char s_root[BROWSER_PATH_MAX];
static char s_vol[FS_VOLUMES][16];           /* "/": the mounted volumes */

static int at_volumes(void)
{
    return !strcmp(s_state.path, "/");
}

/* (Re)load the listing of the current page. */
static void load(void)
{
    s_state.mounted = true;
    s_state.more = 0;
    if (at_volumes()) {
        fs_dirent_t e;
        uint32_t got, total, n = 0;

        for (uint32_t i = 0; i < FS_VOLUMES; i++)
            if (fs_list_dir("/", i, &e, 1, &got, &total) == 0 && got)
                snprintf(s_vol[n++], sizeof s_vol[0], "%s", e.name);
        s_state.total = n;
    } else {
        int n = dirlist_load(s_state.path, NULL, s_state.page, &s_state.more);

        if (n < 0)
            log_printf("browser: list '%s' failed (%d)", s_state.path, n);
        s_state.total = n < 0 ? 0u : (uint32_t)n;
    }
    if (s_state.cursor >= (int)s_state.total)
        s_state.cursor = s_state.total ? (int)s_state.total - 1 : 0;
    if (s_state.top > s_state.cursor)
        s_state.top = s_state.cursor;
}

static const char *entry_name(int i)
{
    return at_volumes() ? s_vol[i] : dirlist_name(i);
}

static int entry_is_dir(int i)
{
    return at_volumes() || dirlist_is_dir(i);
}

static void go(const char *path)
{
    snprintf(s_state.path, sizeof s_state.path, "%s", path);
    s_state.cursor = s_state.top = s_state.page = 0;
    load();
}

/* "<path>/<name>" into out; 0, or -1 when it does not fit. */
static int join(char *out, size_t cap, const char *name)
{
    size_t len = strlen(s_state.path), nlen = strlen(name);

    if (len + nlen + 2 > cap)
        return -1;
    memcpy(out, s_state.path, len);
    if (len == 0 || out[len - 1] != '/')
        out[len++] = '/';
    memcpy(out + len, name, nlen + 1);
    return 0;
}

static bool to_parent(void)
{
    char next[BROWSER_PATH_MAX], *slash;

    if (!strcmp(s_state.path, s_root))
        return false;
    snprintf(next, sizeof next, "%s", s_state.path);
    slash = strrchr(next, '/');
    if (!slash)
        return false;
    if (slash == next)
        next[1] = '\0';
    else
        *slash = '\0';
    if (strlen(next) < strlen(s_root))
        snprintf(next, sizeof next, "%s", s_root);
    go(next);
    return true;
}

/* ---- screen ------------------------------------------------------------------ */

static void browser_screen_on_enter(void)
{
    if (s_state.path[0] == '\0')
        snprintf(s_state.path, sizeof s_state.path, "/");
    load();                                  /* also after a queue reused dirlist */
}

static void browser_screen_draw(void)
{
    char footer[32];

    display_clear(THEME_COLOR_BG);
    display_fill_rect(0, 0, DISPLAY_W - 1, THEME_TITLE_H - 1, THEME_COLOR_TITLE_BG);
    display_draw_text(THEME_PAD, THEME_TITLE_TEXT_Y + THEME_TEXT_YOFF, s_state.path,
                      THEME_COLOR_ACCENT, THEME_COLOR_TITLE_BG);
    for (int row = 0; row < BROWSER_WINDOW; row++) {
        int i = s_state.top + row;
        char label[FS_NAME_MAX + 2];

        if (i >= (int)s_state.total)
            break;
        snprintf(label, sizeof label, entry_is_dir(i) ? "%s/" : "%s", entry_name(i));
        menu_draw_item(THEME_MENU_TOP + row * THEME_ITEM_H, label, NULL, i == s_state.cursor);
    }
    if (s_state.page || s_state.more)
        snprintf(footer, sizeof footer, "%u / %u  page %d%s", (unsigned)(s_state.total ? s_state.cursor + 1 : 0),
                 (unsigned)s_state.total, s_state.page + 1, s_state.more ? "+" : "");
    else
        snprintf(footer, sizeof footer, "%u / %u", (unsigned)(s_state.total ? s_state.cursor + 1 : 0),
                 (unsigned)s_state.total);
    menu_draw_item(DISPLAY_H - THEME_ITEM_H, footer, NULL, false);
}

static void invalidate_row(int i)
{
    int y = THEME_MENU_TOP + (i - s_state.top) * THEME_ITEM_H;

    ui_invalidate(0, y, DISPLAY_W - 1, y + THEME_ITEM_H - 1);
}

/* Cursor step: without a scroll only two rows and the footer change. */
static void step(int delta)
{
    int old = s_state.cursor, old_top = s_state.top;

    s_state.cursor += delta;
    if (s_state.cursor < s_state.top)
        s_state.top = s_state.cursor;
    if (s_state.cursor > s_state.top + BROWSER_WINDOW - 1)
        s_state.top = s_state.cursor - BROWSER_WINDOW + 1;
    if (s_state.top != old_top) {
        ui_invalidate_all();
        return;
    }
    invalidate_row(old);
    invalidate_row(s_state.cursor);
    ui_invalidate(0, DISPLAY_H - THEME_ITEM_H, DISPLAY_W - 1, DISPLAY_H - 1);
}

static void flip_page(int delta)
{
    s_state.page += delta;
    s_state.cursor = s_state.top = 0;
    load();
    if (delta < 0 && s_state.total) {
        s_state.cursor = (int)s_state.total - 1;
        s_state.top = s_state.cursor > BROWSER_WINDOW - 1 ? s_state.cursor - BROWSER_WINDOW + 1 : 0;
    }
    ui_invalidate_all();
}

static void show_result(int rc, const char *name)
{
    char shown[64];

    snprintf(shown, sizeof shown, "%s", name);   /* dirlist may be reloaded below */
    if (rc == PLAYER_OK) {
        track_info_t ti;

        player_screen_set_status(NULL);
        rc = player_play();
        if (player_get_track(&ti) == PLAYER_OK)
            player_screen_set_track(ti.title, ti.duration_ms > 0 ? (uint32_t)(ti.duration_ms / 1000) : 0);
    }
    if (rc != PLAYER_OK) {
        char eb[32];

        snprintf(eb, sizeof eb, rc == PLAYER_ERR_EMPTY ? "nothing to play here" : "cannot play (%d)", rc);
        player_screen_set_track(shown, 0);
        player_screen_set_status(eb);
        log_printf("browser: play '%s' failed (%d)", shown, rc);
        log_flush("error");              /* on the disk now, whatever happens next */
    }
    ui_push_screen(&player_screen);
}

static void browser_screen_handle_event(const input_event_t *ev)
{
    int i = s_state.cursor;
    int hold = ev->adc_value == INPUT_ADC_LONG;

    switch (ev->type) {
    case KEY_UP:
        if (i > 0)
            step(-1);
        else if (s_state.page > 0)
            flip_page(-1);
        break;
    case KEY_DOWN:
        if (i < (int)s_state.total - 1)
            step(+1);
        else if (s_state.more)
            flip_page(+1);
        break;
    case KEY_SELECT: {
        char full[BROWSER_PATH_MAX + 2];

        if (i >= (int)s_state.total || join(full, sizeof full, entry_name(i)) != 0)
            break;
        if (entry_is_dir(i) && !hold) {
            go(full);
            ui_invalidate_all();
        } else if (entry_is_dir(i)) {
            show_result(player_open_tree(full), entry_name(i));
        } else if (!hold) {
            show_result(player_open(full), entry_name(i));
        }
        break;
    }
    case KEY_BACK:
        if (hold)
            break;
        if (to_parent())
            ui_invalidate_all();
        else
            ui_pop_screen();
        break;
    default:
        break;
    }
}

const ui_screen_t browser_screen = {
    "browser",
    browser_screen_on_enter,
    NULL,
    browser_screen_draw,
    browser_screen_handle_event,
};

void browser_screen_set_root(const char *path)
{
    if (path == NULL || path[0] == '\0')
        path = "/";
    snprintf(s_root, sizeof s_root, "%s", path);
    snprintf(s_state.path, sizeof s_state.path, "%s", path);
    s_state.cursor = s_state.top = s_state.page = 0;
    s_state.total = 0;
    s_state.mounted = false;
}

const browser_state_t *browser_screen_state(void)
{
    return &s_state;
}

/* Open at a saved folder and row (resume at power-on); falls back to "/". */
void browser_screen_resume(const char *path, int cursor)
{
    go(path);
    if (!s_state.total && strcmp(path, "/")) {
        go("/");
        return;
    }
    s_state.cursor = cursor < (int)s_state.total ? cursor : 0;
    s_state.top = s_state.cursor > BROWSER_WINDOW - 1 ? s_state.cursor - BROWSER_WINDOW + 1 : 0;
}
