#!/bin/sh
# Tag reader host test: make the fixtures, build under ASan/UBSan, run.
# Usage: firmware/services/tests/run_tags.sh [fuzz iterations] [seed]
#   GAPLESS=<dir> to point at RECHO's gapless set (default: the Kai disk copy;
#   its cases are skipped when it is not there). Needs python3, ffmpeg, lame,
#   flac, metaflac.
set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
GAPLESS="${GAPLESS:-/Volumes/Kai/snowsky_echo_mini/work/disks/recho_gapless/GAPLESS}"

mkdir -p .work
python3 gen_tag_fixtures.py .work/tags
$CC -Wall -Wextra -Werror -std=c99 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I.. -o .work/test_tags test_tags.c ../tags.c
.work/test_tags .work/tags "$GAPLESS" "${1:-5000}" ${2:+"$2"}
