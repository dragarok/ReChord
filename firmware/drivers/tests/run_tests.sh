#!/bin/sh
# Build and run the ReChord display/font host tests with plain `cc`.
#
# Usage: ./run_tests.sh
# Env:   CC=...                compiler (default: cc)
#        RECHORD_TEST_OUT=dir  where to put binaries + PPM artifact
#                              (default: fresh mktemp -d)
set -e

cd "$(dirname "$0")"
CC="${CC:-cc}"
CFLAGS="-std=c99 -Wall -Werror"
OUT="${RECHORD_TEST_OUT:-$(mktemp -d)}"
mkdir -p "$OUT"

$CC $CFLAGS -I.. -o "$OUT/test_display" \
    test_display.c ../display.c ../text_stub.c ../panel_init_rknanoc.c ../font.c ../font8x8.c \
    ../panel_io_sim.c
$CC $CFLAGS -I.. -o "$OUT/test_display_list" \
    test_display_list.c ../display.c ../panel_init_rknanoc.c ../font.c ../font8x8.c \
    ../panel_io_sim.c
$CC $CFLAGS -I.. -o "$OUT/test_panel_init" \
    test_panel_init.c ../display.c ../text_stub.c ../panel_init_rknanoc.c ../font.c \
    ../font8x8.c ../panel_io_sim.c
$CC $CFLAGS -I.. -o "$OUT/test_font_ppm" \
    test_font_ppm.c ../display.c ../text_stub.c ../panel_init_rknanoc.c ../font.c ../font8x8.c \
    ../panel_io_sim.c

$CC $CFLAGS -O2 -I.. -o "$OUT/test_text" \
    test_text.c ../text.c ../icons.c ../fonts/icons_data.c ../fonts/fonts_blob.c ../fonts/fonts_s8.c

"$OUT/test_display"
"$OUT/test_display_list"
"$OUT/test_panel_init"
"$OUT/test_font_ppm" "$OUT/font_render.ppm"
"$OUT/test_text" golden ../fonts/deva_hb.txt "$OUT"

echo "all host tests passed (PPM artifacts: $OUT/font_render.ppm, $OUT/<case>.ppm)"
