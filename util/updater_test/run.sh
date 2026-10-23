#!/usr/bin/env bash
# Host tests for the Svalboard in-firmware updater (keyboards/svalboard/updater).
# Standalone: needs gcc (or cc) and python3, no QMK build.
#
#   util/updater_test/run.sh [REAL.uf2 ...]
#
# REAL.uf2: optional QMK builds (with a .bin beside each) to run make_update.py on.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
kb="$root/keyboards/svalboard"
tests="$root/tests/sval_updater"
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT

python3 -I "$tests/test_make_update.py" "$out/cross" "$@"
python3 -I "$tests/test_release_version.py" "$out/release_version"
python3 -I "$tests/test_release_artifacts.py" "$out/release_artifacts"
python3 -I "$tests/test_either_hand.py"

# Settings reset on the first boot of a different firmware build (kb/build_reset.c).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -DBUILD_RESET_HOST_TEST -I"$kb" "$tests/test_build_reset.c" "$kb/build_reset.c" -o "$out/test_build_reset"

# The side record of the either-hand builds (kb/side.c): priority, migration
# from per-hand boards, the record across build resets, the pairing report.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all     -DSVAL_SIDE_HOST_TEST -DBUILD_RESET_HOST_TEST -I"$kb" "$tests/test_side.c" "$kb/side.c" "$kb/build_reset.c" -o "$out/test_side"
"$out/test_side"
"$out/test_build_reset"

# Raw HID framing (tmk_core/protocol/chibios/usb_packet.h) against ChibiOS's
# real input buffers queue: a short packet never misframes the next report.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -O1 -g     -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer     -I"$root/tests/sval_rawhid" -I"$root/lib/chibios/os/hal/include" -I"$root/tmk_core/protocol/chibios"     "$root/tests/sval_rawhid/test_packet.c" "$root/lib/chibios/os/hal/src/hal_buffers.c"     -o "$out/test_packet"
"$out/test_packet"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" "$kb/updater/update_pair.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_updater"
"$out/test_updater" "$out/cross"

# kb/tools/sval_update.py against the same state machine, through ctypes
# (no sanitizers: they need to be preloaded into python).
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -shared -fPIC \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATER_HOST_LIB -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" "$kb/updater/update_pair.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/libupdater_host.so"
python3 -I "$tests/test_sval_update_tool.py" "$out/libupdater_host.so" "$out/tool"

# Dev builds with no chord (SVAL_UPDATE_NO_CHORD, D43): the protocol 4 cases
# for them (test_pair.c), then the tool with no chord and no question for this
# half, the other half and pair.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_NO_CHORD -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" "$kb/updater/update_pair.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_nochord"
"$out/test_nochord"
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -shared -fPIC \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATER_HOST_LIB -DSVAL_UPDATE_NO_CHORD -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" "$kb/updater/update_pair.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/libupdater_nochord.so"
python3 -I "$tests/test_no_chord_tool.py" "$out/libupdater_nochord.so" "$out/nochord"

# The commit routine (update_commit.c, included by test_commit.c) against the
# same mock die, with mock registers, the test hooks and power cuts.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_TEST_REAL_COMMIT -DSVAL_UPDATE_TEST_HOOKS -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_updater.c" \
    "$kb/updater/updater.c" "$kb/updater/update_gesture.c" \
    "$kb/updater/update_image.c" "$kb/updater/update_build_info.c" \
    "$kb/updater/update_split.c" "$kb/updater/update_split_wire.c" "$kb/updater/update_pair.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_commit"
"$out/test_commit"

# M2: the split pause (kb/split_pause.c), the KEYBOARD_UPDATE wire format and
# page assembly. The other half's session and the relay (update_split.c) are
# tested in the builds above, by test_relay.c, against the mock die.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" \
    "$tests/test_split.c" "$kb/split_pause.c" "$kb/updater/update_split_wire.c" \
    -o "$out/test_split"
"$out/test_split"

# Release builds: test hooks, keytest, the host bootloader, the bench settings
# wipe and the dev builds' no-chord updates and kept settings (D43) cannot be
# compiled into one (update_build_info.c refuses; update_gesture.c too, for
# no-chord).
for flags in "-DSVAL_UPDATE_TEST_HOOKS" "-DSVAL_KEYTEST" "-DSVAL_HOST_BOOTLOADER=1" "-DSVAL_TEST_WIPE" \
    "-DSVAL_UPDATE_NO_CHORD" "-DSVAL_DEV_KEEP_SETTINGS"; do
    if ${CC:-cc} -std=c11 -O1 -DINIT_EE_HANDS_LEFT -DSVAL_UPDATE_RELEASE $flags -include "$kb/config.h" \
        -I"$kb/updater" -I"$kb/updater/vendor" \
        -c "$kb/updater/update_build_info.c" -o "$out/bi.o" 2>/dev/null; then
        echo "FAIL: a release build compiled with $flags"; exit 1
    fi
done
if ${CC:-cc} -std=c11 -O1 -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_RELEASE -DSVAL_UPDATE_NO_CHORD -include "$kb/config.h" \
    -I"$kb/updater" -c "$kb/updater/update_gesture.c" -o "$out/gesture.o" 2>/dev/null; then
    echo "FAIL: a release build compiled update_gesture.c with SVAL_UPDATE_NO_CHORD"; exit 1
fi
echo "release builds: refuse test hooks, keytest, the host bootloader, SVAL_TEST_WIPE, SVAL_UPDATE_NO_CHORD and SVAL_DEV_KEEP_SETTINGS"

# M3: a release build, against the real update_build_info.c and
# update_image.c. SVAL_RELEASE_SVUPS, if set, adds .svup files to check as a
# release build would before erasing: "a.svup:ok b.svup:refused ...".
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Wno-unused-function -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -DSVAL_UPDATER_HOST_TEST -DSVAL_UPDATE_RELEASE -include "$kb/config.h" \
    -I"$tests" -I"$kb" -I"$kb/updater" -I"$kb/updater/vendor" \
    "$tests/test_release.c" "$kb/updater/update_build_info.c" "$kb/updater/update_image.c" \
    "$kb/updater/vendor/sha512.c" \
    -o "$out/test_release"
# shellcheck disable=SC2086 # SVAL_RELEASE_SVUPS is a list of FILE:expect words
"$out/test_release" ${SVAL_RELEASE_SVUPS:-}

# Pair UF2 (docs/pair-uf2.md): the relay with no chord, update_relay_start_self,
# is called from the boot relay (update_pair.c) and nowhere else, and only
# update_flash.c's fixed-address routine writes the pair record sector.
callers="$(grep -rl 'update_relay_start_self' "$kb" --include='*.c' | sort | tr '\n' ' ')"
if [ "$callers" != "$kb/updater/update_pair.c $kb/updater/update_split.c " ]; then
    echo "FAIL: update_relay_start_self is used outside update_pair.c: $callers"; exit 1
fi
if grep -rn 'UPDATE_PAIR_RECORD_OFFSET' "$kb" --include='*.c' | grep -v "$kb/updater/update_flash.c" | grep -v "$kb/updater/update_pair.c" | grep -q .; then
    echo "FAIL: UPDATE_PAIR_RECORD_OFFSET is used outside update_flash.c and update_pair.c"; exit 1
fi
echo "pair UF2: the no-chord relay and the record sector have one caller each"
