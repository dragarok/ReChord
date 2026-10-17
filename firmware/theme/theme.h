/*
 * theme.h — ReChord theme API (the ONLY theme interface other modules use).
 *
 * A theme is pure data: an RGB565 palette, spacing/radius metrics and a font
 * id. UI code never hardcodes a color — it asks theme_get(). Built-in themes
 * are .c data tables (themes_builtin.c); a user file of `key=value` lines can
 * override any subset of fields at runtime (theme_file.c).
 *
 * WHY RGB565: the display contract (docs/re/route-b-minimum.md) fills and
 * blits RGB565 pixels directly, so the palette stores final pixel values —
 * no per-frame channel swizzling on the Cortex-M3.
 */
#ifndef THEME_H
#define THEME_H

#include <stdint.h>

/* Max theme name length including the NUL terminator (embedded in the struct
 * so a theme_t copy is fully self-contained — no lifetime traps). */
#define THEME_NAME_MAX 24

typedef struct theme
{
    char     name[THEME_NAME_MAX];

    /* Palette — RGB565 (5-6-5) pixel values, exactly as sent to the panel.
     * The first seven are the original set; DESIGN.md 5.1's token for each is
     * noted (bg, surface, text, text_dim, accent, danger, ok). */
    uint16_t background;  /* bg: screen/clear color                      */
    uint16_t surface;     /* cards, bars, popups sitting on background   */
    uint16_t text;        /* primary text                                */
    uint16_t text_dim;    /* secondary text, hints                       */
    uint16_t accent;      /* selection marks, live values, bars          */
    uint16_t danger;      /* destructive actions, errors, low battery    */
    uint16_t success;     /* ok: confirmations, OK states, charging      */

    /* DESIGN.md 5.1 tokens (the new UI reads these). */
    uint16_t header_bg;   /* header band                                 */
    uint16_t divider;     /* 1-px rules between header/body/key bar      */
    uint16_t text_off;    /* disabled rows                               */
    uint16_t on_accent;   /* text/icons on an accent fill                */
    uint16_t sel_bg;      /* selected row fill                           */
    uint16_t sel_text;    /* text on the selected row                    */
    uint16_t value;       /* right-hand values in rows                   */
    uint16_t keybar_bg;   /* key bar band                                */
    uint16_t key_text;    /* key bar labels                              */
    uint16_t key_hold;    /* hold labels (the second line)               */
    uint16_t key_press;   /* a pressed key's slot / hold fill            */
    uint16_t bar_track;   /* progress/volume bar track                   */
    uint16_t bar_fill;    /* progress/volume bar fill                    */
    uint16_t badge_bg;    /* badges (format, bit depth)                  */
    uint16_t badge_text;
    uint16_t hires;       /* the hi-res badge only                       */
    uint16_t warn;        /* warnings (amber)                            */
    uint16_t scroll;      /* scrollbar thumb                             */

    /* Layout metrics (px). Kept for the file format; the new UI's geometry
     * is fixed by DESIGN.md and does not read them. */
    int      spacing;     /* gap between elements                        */
    int      radius;      /* corner radius of rounded rects              */
    int      font_id;     /* unused (0)                                  */
} theme_t;

/*
 * Current theme access.
 *   theme_get()  returns a pointer valid until the next theme_set()/load.
 *   theme_set()  copies *t into the current slot (NULL is ignored).
 * On first use the current theme defaults to built-in theme 0 ("Classic").
 */
const theme_t *theme_get(void);
void           theme_set(const theme_t *t);

/*
 * Built-in theme registry (indices 0 .. theme_count()-1 are stable order).
 *   theme_load_named()  installs a built-in theme by name (ASCII case-
 *                       insensitive); returns 0 on success, <0 if unknown.
 *   theme_name(i)       returns the name of built-in i, NULL if out of range.
 */
int           theme_load_named(const char *name);
int           theme_count(void);
const char   *theme_name(int i);

/*
 * Selection by index (the order of theme_name(): Classic, Paper, Night,
 * Contrast). theme_load_index() installs built-in i: 0 on success, < 0 out of
 * range. theme_index() is the index of a built-in by name (ASCII case-
 * insensitive), < 0 if none. theme_builtin(i) is built-in i itself (NULL out
 * of range): a base for a user file, or a preview without switching.
 */
int           theme_load_index(int i);
int           theme_index(const char *name);
const theme_t *theme_builtin(int i);

/* Sanity-check a theme: 0 = usable, <0 = which field failed (see .c). */
int           theme_validate(const theme_t *t);

/*
 * File-based override loader (theme_file.c). Both functions are OVERRIDE
 * style: they write only the keys present in the text and leave every other
 * field of *inout untouched, so a user file may override 1..N fields of a
 * base theme ("a file-based theme can override at runtime").
 *
 *   theme_file_parse()  parses one `key=value` per line text blob.
 *   theme_file_load()   reads the file at `path` and parses it.
 *
 * Tolerant by design (user-edited files must not brick the UI): blank lines,
 * `#`/`;` comments, unknown keys and malformed lines are skipped. Keys are the
 * field names above (DESIGN.md's `bg` and `ok` name background and success).
 * Colour values are RGB565 as decimal or 0x-prefixed hex, or `#RRGGBB` (8-bit
 * sRGB, quantised to RGB565). Returns the number of keys applied (>= 0), or
 * < 0 on hard errors (NULL arguments, unreadable file).
 */
int           theme_file_parse(const char *text, theme_t *inout);
int           theme_file_load(const char *path, theme_t *inout);

#endif /* THEME_H */
