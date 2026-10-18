#!/bin/sh
# Build and run the services host tests with a strict host compiler.
# Usage: ./run_tests.sh   (from anywhere; runs inside tests/)
set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--Wall -Werror -O1}"

mkdir -p .work

$CC $CFLAGS -I.. -o .work/test_settings test_settings.c ../settings.c ../fs_host.c
$CC $CFLAGS -I.. -DLOG_PATH='"/ECHOLOG.TXT"' -o .work/test_log test_log.c ../log.c ../fs_host.c
$CC $CFLAGS -I.. -I../.. -DLOG_PATH='"/ECHOLOG.TXT"' -o .work/test_config test_config.c ../config.c ../log.c ../fs_host.c
$CC $CFLAGS -I.. -o .work/test_input   test_input.c   ../input.c
$CC $CFLAGS -I.. -o .work/test_fs      test_fs.c      ../fs_host.c
$CC $CFLAGS -I.. -o .work/test_bt_proto test_bt_proto.c ../bt_proto.c
$CC $CFLAGS -I.. -o .work/test_keyseq  test_keyseq.c  ../keyseq.c ../bt_keys.c

.work/test_settings
.work/test_log
.work/test_config
.work/test_input
.work/test_fs
.work/test_bt_proto
.work/test_keyseq
./run_tags.sh
echo "ALL SERVICES TESTS PASSED"
