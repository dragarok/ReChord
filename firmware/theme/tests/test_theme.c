/*
 * test_theme.c — host test for the ReChord theme module.
 *
 * Verifies:
 *   1. every built-in theme exists, is named, and has a valid palette
 *      (readable text/accent vs background, distinct semantic colors);
 *   2. theme_load_named() installs a theme case-insensitively, unknown
 *      names fail WITHOUT clobbering the current theme;
 *   3. the file parser round-trips a sample theme file: every field written
 *      to the file comes back unchanged, unknown/garbage lines are skipped,
 *      and a partial file overrides only the keys it contains;
 *   4. theme_set()/theme_get() identity;
 *   5. the DESIGN.md 5.1 tokens: four built-ins in order, sample values,
 *      readable pairs, selection by index and name, `#RRGGBB` and the
 *      `bg`/`ok` key aliases in files.
 *
 * Compile (host):
 *   cc -Wall -Werror -O2 -o test_theme \
 *      firmware/theme/tests/test_theme.c firmware/theme/theme.c \
 *      firmware/theme/themes_builtin.c firmware/theme/theme_file.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../theme.h"

static int failures = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("  PASS: %s\n", name); } \
    else { printf("  FAIL: %s\n", name); failures++; } \
} while (0)

/* Sample theme exercising every key the parser knows. Values are chosen
 * distinct so a mix-up between fields is caught. */
static const char sample_theme[] =
    "# ReChord sample theme\n"
    "; second comment style\n"
    "\n"
    "name = Round Trip\n"
    "background = 0x1234\n"
    "surface=0x2345\n"
    "text = 0x3456\n"
    "text_dim=0x4567\n"
    "accent = 0x5678\n"
    "danger = 0x6789\n"
    "success=0x789A\n"
    "spacing = 7\n"
    "radius=3\n"
    "font_id = 5\n"
    "unknown_key = 42\n"
    "this line has no equals sign\n"
    "background = not_a_number\n"
    "spacing = 9\n";

int main(void)
{
    int i, j, n;

    /* ---- 1. built-in themes ---------------------------------------- */
    n = theme_count();
    CHECK(n >= 3, "at least 3 built-in themes");
    CHECK(theme_name(0) != 0 && theme_name(0)[0] != '\0',
          "theme_name(0) is a non-empty string");
    CHECK(theme_name(-1) == 0 && theme_name(n) == 0,
          "theme_name() returns NULL out of range");

    for (i = 0; i < n; i++) {
        char label[96];
        const char *name = theme_name(i);
        const theme_t *t;
        int unique = 1;

        CHECK(name != 0 && name[0] != '\0', "built-in name is non-empty");

        /* Names are unique (a settings list keyed by name needs that). */
        for (j = 0; j < n; j++)
            if (j != i && name && theme_name(j) &&
                strcmp(name, theme_name(j)) == 0)
                unique = 0;
        snprintf(label, sizeof label, "built-in %d name is unique", i);
        CHECK(unique, label);

        snprintf(label, sizeof label, "built-in %d '%s' loads", i,
                 name ? name : "?");
        CHECK(theme_load_named(name) == 0, label);

        t = theme_get();
        snprintf(label, sizeof label, "built-in %d palette is valid", i);
        CHECK(theme_validate(t) == 0, label);
        snprintf(label, sizeof label,
                 "built-in %d keeps its name after load", i);
        CHECK(strcmp(t->name, name) == 0, label);
        snprintf(label, sizeof label,
                 "built-in %d: text and accent contrast the background", i);
        CHECK(t->text != t->background && t->accent != t->background, label);
        snprintf(label, sizeof label,
                 "built-in %d: danger and success are distinct", i);
        CHECK(t->danger != t->success, label);
    }

    /* Built-in themes must actually differ from each other. */
    theme_load_named("Classic");
    {
        uint16_t classic_bg = theme_get()->background;
        uint16_t classic_accent = theme_get()->accent;
        CHECK(theme_load_named("Paper") == 0 &&
              theme_get()->background != classic_bg,
              "Paper background differs from Classic");
        CHECK(theme_load_named("Night") == 0 &&
              theme_get()->accent != classic_accent,
              "Night accent differs from Classic");
    }

    /* ---- 2. named load --------------------------------------------- */
    CHECK(theme_load_named("paper") == 0 &&
          strcmp(theme_get()->name, "Paper") == 0,
          "theme_load_named() is case-insensitive");
    {
        uint16_t before = theme_get()->background;
        CHECK(theme_load_named("NoSuchTheme") < 0,
              "unknown theme name reports an error");
        CHECK(theme_get()->background == before,
              "failed load leaves the current theme untouched");
    }

    /* ---- 3. file parser round-trip --------------------------------- */
    {
        theme_t t;
        const char *path = "theme_test_sample.theme";
        FILE *f;

        memset(&t, 0, sizeof t);
        n = theme_file_parse(sample_theme, &t);
        /* 12 known keys survive (the malformed background line is skipped;
         * spacing appears twice and is counted twice — spacing=9 wins). */
        CHECK(n == 12, "parser applied exactly the well-formed keys");
        CHECK(strcmp(t.name, "Round Trip") == 0, "name round-trips");
        CHECK(t.background == 0x1234 && t.surface == 0x2345 &&
              t.text == 0x3456 && t.text_dim == 0x4567 &&
              t.accent == 0x5678 && t.danger == 0x6789 &&
              t.success == 0x789A, "all palette fields round-trip");
        CHECK(t.spacing == 9, "duplicate key: last value wins");
        CHECK(t.radius == 3 && t.font_id == 5, "metrics round-trip");
        CHECK(theme_validate(&t) == 0, "parsed theme validates");

        /* Round-trip through the runtime slot. */
        theme_set(&t);
        CHECK(memcmp(theme_get(), &t, sizeof t) == 0,
              "theme_set()/theme_get() is identity");

        /* File loader == parser on the same bytes. */
        f = fopen(path, "w");
        CHECK(f != 0, "sample theme file created");
        if (f) {
            theme_t from_file;
            fputs(sample_theme, f);
            fclose(f);

            memset(&from_file, 0, sizeof from_file);
            CHECK(theme_file_load(path, &from_file) == 12,
                  "theme_file_load() applies the same keys");
            CHECK(memcmp(&from_file, &t, sizeof t) == 0,
                  "file loader round-trips the sample theme");
            remove(path);
        }
    }

    /* ---- 4. partial override semantics ------------------------------ */
    {
        theme_t t;
        uint16_t old_text;

        CHECK(theme_load_named("Classic") == 0, "reload Classic as base");
        t = *theme_get();
        old_text = t.text;
        CHECK(theme_file_parse("background=0x0000\n", &t) == 1,
              "one-key file applies one key");
        CHECK(t.background == 0x0000, "override writes the key present");
        CHECK(t.text == old_text, "override preserves keys not present");
    }

    /* ---- 5. DESIGN.md 5.1 tokens and selection ---------------------- */
    {
        static const char *const order[] = { "Classic", "Paper", "Night", "Contrast" };
        theme_t t;
        int k;

        CHECK(theme_count() == 4, "four built-in themes");
        for (k = 0; k < 4; k++)
            CHECK(theme_name(k) && strcmp(theme_name(k), order[k]) == 0, "built-in order");
        CHECK(theme_index("contrast") == 3 && theme_index("PAPER") == 1, "theme_index is case-insensitive");
        CHECK(theme_index("Sepia") < 0 && theme_index(0) < 0, "theme_index of an unknown name");
        CHECK(theme_load_index(3) == 0 && strcmp(theme_get()->name, "Contrast") == 0, "theme_load_index");
        CHECK(theme_load_index(4) < 0 && theme_load_index(-1) < 0 &&
              strcmp(theme_get()->name, "Contrast") == 0, "out of range leaves the current theme");
        CHECK(theme_builtin(0) && strcmp(theme_builtin(0)->name, "Classic") == 0 && theme_builtin(4) == 0,
              "theme_builtin");
        /* sample values (8-bit tokens quantised to RGB565) */
        CHECK(theme_builtin(0)->sel_bg == 0x2B37 && theme_builtin(0)->value == 0x6DBF &&
              theme_builtin(0)->keybar_bg == 0x18E4 && theme_builtin(0)->hires == 0xED89, "Classic tokens");
        CHECK(theme_builtin(1)->header_bg == 0xEF3A && theme_builtin(1)->sel_bg == 0x9AC2, "Paper tokens");
        CHECK(theme_builtin(2)->text == 0xAD56 && theme_builtin(2)->sel_bg == 0x1A6F, "Night tokens");
        CHECK(theme_builtin(3)->accent == 0xFEA0 && theme_builtin(3)->divider == 0xFFFF &&
              theme_builtin(3)->sel_text == 0x0000, "Contrast tokens");
        for (k = 0; k < 4; k++) {
            const theme_t *b = theme_builtin(k);

            CHECK(b->sel_text != b->sel_bg && b->key_text != b->keybar_bg && b->on_accent != b->accent &&
                  b->badge_text != b->badge_bg && b->text != b->header_bg && b->bar_fill != b->bar_track,
                  "readable token pairs");
        }
        /* files: #RRGGBB, aliases, new keys */
        t = *theme_builtin(0);
        CHECK(theme_file_parse("sel_bg = #2A66BE\nbg=#000000\nok = #00FF00\nscroll=0x1234\n"
                               "warn = #12345\nhires=#GG0000\n", &t) == 4,
              "four good colour lines, two bad ones skipped");
        CHECK(t.sel_bg == 0x2B37 && t.background == 0x0000 && t.success == 0x07E0 && t.scroll == 0x1234,
              "#RRGGBB quantised, bg/ok aliases");
        CHECK(t.warn == theme_builtin(0)->warn && t.hires == theme_builtin(0)->hires, "bad colours left alone");
    }

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
