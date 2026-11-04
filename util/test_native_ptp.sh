#!/bin/bash
# Host tests for the native Precision Touchpad (SVAL_NATIVE_PTP). No board
# needed. Run from the repository root: util/test_native_ptp.sh
# The Azoteq driver's contact read has its own gtest:
#   make test:pointing/azoteq_multitouch
set -euo pipefail
cd "$(dirname "$0")/.."
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
t=tests/sval_native_ptp
cc=(${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
    -Wno-missing-field-initializers -Ikeyboards/svalboard/azoteq -I$t)
export ASAN_OPTIONS=detect_leaks=0

"${cc[@]}" $t/dump_descriptor.c -o "$work/dump"
"$work/dump" > "$work/descriptor.hex"

# The unifier, feature reports and frame builder (ptp.c), on the pads' area
# alone, so positions are the pads' own; then on the firmware's surface (seven
# times the pads' area, ptp_descriptor.h), where its own tests run.
"${cc[@]}" -DSVAL_PTP_SURFACE_TIMES=1 $t/test_ptp.c -o "$work/test_ptp"
"$work/test_ptp" "$work/trace.txt"
"${cc[@]}" $t/test_ptp.c -o "$work/test_ptp_os"
"$work/test_ptp_os" "$work/trace-os.txt"

# The contact path and the split link (multitouch.c with ptp.c).
"${cc[@]}" -DSVAL_PTP_SURFACE_TIMES=1 -Wno-unused-function $t/test_multitouch.c -o "$work/test_multitouch"
"$work/test_multitouch"
# Again on the firmware's surface.
"${cc[@]}" -Wno-unused-function $t/test_multitouch.c -o "$work/test_multitouch_os"
"$work/test_multitouch_os" > "$work/multitouch-os.txt" || { cat "$work/multitouch-os.txt"; exit 1; }
tail -n 1 "$work/multitouch-os.txt" | sed 's/^/(OS surface) /'

# The pad read schedule (pad_schedule.h) against a simulated chip and loop.
"${cc[@]}" $t/test_schedule.c -o "$work/test_schedule"
"$work/test_schedule"

py=${PTP_TEST_PYTHON:-python3}
# The descriptor, and every report the C tests sent, decoded with it.
"$py" $t/test_descriptor.py "$work/descriptor.hex" "$work/trace.txt" "$work/trace-os.txt"
# The dev tools' parsers (no board).
"$py" -m unittest discover -s keyboards/svalboard/tools/multitouch -p 'test_*.py'
