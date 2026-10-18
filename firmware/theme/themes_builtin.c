/*
 * themes_builtin.c — built-in ReChord themes as .c data tables.
 *
 * WHY data in .c: the build-from-source rule (docs/rewrite/architecture.md §1)
 * wants every byte rebuildable from source; .c tables need no asset pipeline,
 * compile on host and target alike, and can be `const` (flash-resident on the
 * Cortex-M3). This file is the ONLY place in the firmware where color values
 * exist — everything else asks theme_get().
 *
 * The values are DESIGN.md 5.1's tokens (8-bit sRGB, in the comments), the same
 * as tools/ui_mockups.py THEMES in the project repo, quantised to RGB565 as
 * ((r >> 3) << 11 | (g >> 2) << 5 | b >> 3). tests/ui_tokens_check.py there
 * checks every one against the mockups.
 *
 *   Classic  — dark slate, the default.
 *   Paper    — warm light/sepia for daylight (no pure white/black pair).
 *   Night    — very dim, for bedside use.
 *   Contrast — black, white and yellow, WCAG ratios 13..21.
 */
#include "theme.h"
#include "theme_priv.h"

static const theme_t builtin_themes[] =
{
    {
        .name = "Classic",
        .background = 0x1083,   /* 121218 */
        .surface    = 0x1905,   /* 1E2029 */
        .text       = 0xEF7E,   /* ECEEF2 */
        .text_dim   = 0x94B4,   /* 9296A0 */
        .accent     = 0x44FF,   /* 409CFF */
        .danger     = 0xFA8A,   /* FF5252 */
        .success    = 0x56AF,   /* 52D678 */
        .header_bg  = 0x1083,   /* 121218 */
        .divider    = 0x2987,   /* 2E313C */
        .text_off   = 0x5B0D,   /* 5C606A */
        .on_accent  = 0xFFFF,   /* FFFFFF */
        .sel_bg     = 0x2B37,   /* 2A66BE */
        .sel_text   = 0xFFFF,   /* FFFFFF */
        .value      = 0x6DBF,   /* 6FB5FF */
        .keybar_bg  = 0x18E4,   /* 1A1C24 */
        .key_text   = 0xD6BB,   /* D4D7DE */
        .key_hold   = 0x7C11,   /* 7C808A */
        .key_press  = 0x2B77,   /* 2D6FB8 */
        .bar_track  = 0x2987,   /* 2E313C */
        .bar_fill   = 0x44FF,   /* 409CFF */
        .badge_bg   = 0x2987,   /* 2E313C */
        .badge_text = 0xE73D,   /* E2E4EA */
        .hires      = 0xED89,   /* E8B04B */
        .warn       = 0xFD84,   /* FFB020 */
        .scroll     = 0x5B0D,   /* 5C606A */
        .spacing = 8, .radius = 6, .font_id = 0,
    },
    {
        .name = "Paper",
        .background = 0xF79C,   /* F6F0E4 */
        .surface    = 0xFFFE,   /* FFFCF4 */
        .text       = 0x3144,   /* 302A20 */
        .text_dim   = 0x7B8C,   /* 7A7060 */
        .accent     = 0x9AC2,   /* 9E5A14 */
        .danger     = 0xB145,   /* B02A2A */
        .success    = 0x2BE8,   /* 2E7C46 */
        .header_bg  = 0xEF3A,   /* EDE5D4 */
        .divider    = 0xD656,   /* D3C8B4 */
        .text_off   = 0xB552,   /* B3A994 */
        .on_accent  = 0xFFFF,   /* FFFFFF */
        .sel_bg     = 0x9AC2,   /* 9E5A14 */
        .sel_text   = 0xFFFF,   /* FFFFFF */
        .value      = 0x9AC2,   /* 9E5A14 */
        .keybar_bg  = 0xEF3A,   /* EDE5D4 */
        .key_text   = 0x3144,   /* 302A20 */
        .key_hold   = 0x8C0D,   /* 8A806E */
        .key_press  = 0xDDD1,   /* D9B98F */
        .bar_track  = 0xDE97,   /* DCD2BF */
        .bar_fill   = 0x9AC2,   /* 9E5A14 */
        .badge_bg   = 0xE6D8,   /* E3D9C5 */
        .badge_text = 0x3144,   /* 302A20 */
        .hires      = 0x9B40,   /* 9A6A00 */
        .warn       = 0xAB00,   /* A86200 */
        .scroll     = 0xB552,   /* B3A994 */
        .spacing = 10, .radius = 2, .font_id = 0,
    },
    {
        .name = "Night",
        .background = 0x0841,   /* 08080C */
        .surface    = 0x10A3,   /* 16161E */
        .text       = 0xAD56,   /* AAAAB2 */
        .text_dim   = 0x630D,   /* 62626E */
        .accent     = 0x23B6,   /* 2474B4 */
        .danger     = 0xB2AA,   /* B45656 */
        .success    = 0x44AD,   /* 46966A */
        .header_bg  = 0x0841,   /* 08080C */
        .divider    = 0x18E4,   /* 1C1C26 */
        .text_off   = 0x39E8,   /* 3C3C46 */
        .on_accent  = 0xDEDC,   /* D8D8E0 */
        .sel_bg     = 0x1A6F,   /* 1D4E78 */
        .sel_text   = 0xDEDC,   /* D8D8E0 */
        .value      = 0x4C78,   /* 4F8CC0 */
        .keybar_bg  = 0x0862,   /* 0E0E14 */
        .key_text   = 0x8C52,   /* 8A8A94 */
        .key_hold   = 0x630D,   /* 62626E */
        .key_press  = 0x1A6F,   /* 1D4E78 */
        .bar_track  = 0x18E4,   /* 1C1C26 */
        .bar_fill   = 0x23B6,   /* 2474B4 */
        .badge_bg   = 0x18E4,   /* 1C1C26 */
        .badge_text = 0x9CD4,   /* 9A9AA4 */
        .hires      = 0xA3C6,   /* A07A30 */
        .warn       = 0xABC5,   /* A87A2C */
        .scroll     = 0x39E8,   /* 3C3C46 */
        .spacing = 8, .radius = 6, .font_id = 0,
    },
    {
        .name = "Contrast",
        .background = 0x0000,   /* 000000 */
        .surface    = 0x0000,   /* 000000 */
        .text       = 0xFFFF,   /* FFFFFF */
        .text_dim   = 0xE73C,   /* E6E6E6 */
        .accent     = 0xFEA0,   /* FFD500 */
        .danger     = 0xFA8A,   /* FF5050 */
        .success    = 0x57F0,   /* 50FF80 */
        .header_bg  = 0x0000,   /* 000000 */
        .divider    = 0xFFFF,   /* FFFFFF */
        .text_off   = 0x8C71,   /* 8C8C8C */
        .on_accent  = 0x0000,   /* 000000 */
        .sel_bg     = 0xFEA0,   /* FFD500 */
        .sel_text   = 0x0000,   /* 000000 */
        .value      = 0xFEA0,   /* FFD500 */
        .keybar_bg  = 0x0000,   /* 000000 */
        .key_text   = 0xFFFF,   /* FFFFFF */
        .key_hold   = 0xCE59,   /* C8C8C8 */
        .key_press  = 0xFEA0,   /* FFD500 */
        .bar_track  = 0x5ACB,   /* 5A5A5A */
        .bar_fill   = 0xFEA0,   /* FFD500 */
        .badge_bg   = 0xFFFF,   /* FFFFFF */
        .badge_text = 0x0000,   /* 000000 */
        .hires      = 0xFEA0,   /* FFD500 */
        .warn       = 0xFD80,   /* FFB000 */
        .scroll     = 0xFFFF,   /* FFFFFF */
        .spacing = 8, .radius = 0, .font_id = 0,
    },
};

#define BUILTIN_COUNT ((int)(sizeof(builtin_themes) / sizeof(builtin_themes[0])))

int theme_builtin_count(void)
{
    return BUILTIN_COUNT;
}

const theme_t *theme_builtin_get(int i)
{
    if (i < 0 || i >= BUILTIN_COUNT)
        return 0;
    return &builtin_themes[i];
}
