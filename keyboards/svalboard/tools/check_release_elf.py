#!/usr/bin/env python3
"""Lint release updater ELFs before they are published (plan M3).

    check_release_elf.py BUILD.elf [BUILD.elf ...]

Release CI builds every release keymap as a release updater build
(SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes). For each ELF this checks, and
exits 1 on any failure:

  - it is an updater build: one sval_update_build_info record (updater/
    update_build_info.h) with magic SVBI and info_ver 3, and updater_task
    linked;
  - the record says RELEASE, and none of TEST_HOOKS, KEYTEST,
    HOST_BOOTLOADER, NO_CHORD or DEV_KEEP (D43); its reserved bits and bytes
    are 0; its updater protocol
    is the tools'; it is a release build (D34): one image for both halves (hand
    2, svalboard/either), the auto
    pointing build (every driver) and a release keymap (sval or blank, D40);
  - no test hooks (update_commit_test_halt, op_test_halt, halt_point,
    halt_fed), no SVAL_KEYTEST (keytest_task, keytest_command), no
    SVAL_HOST_BOOTLOADER (scanlab.c's reboot_pending, reboot_due_ms), and none
    of the dev builds' SVAL_UPDATE_NO_CHORD (update_no_chord) or
    SVAL_DEV_KEEP_SETTINGS (sval_dev_keep_settings).

The symbol checks look at the ELF's own symbol table, independently of the
build-info record, so a record that lies about the build is caught. The
split message table is checked separately (check_split_tables.py --golden),
and the commit's RAM code by check_ram_funcs.py.
"""
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check_split_tables as cst  # noqa: E402  (same directory: the ELF reader)
import make_update as mu  # noqa: E402  (build-info layout)

STT_OBJECT, STT_FUNC = 1, 2
FORBIDDEN = {
    "update_commit_test_halt": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "op_test_halt": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "halt_point": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "halt_fed": "test hooks (SVAL_UPDATE_TEST_HOOKS)",
    "keytest_task": "SVAL_KEYTEST",
    "keytest_command": "SVAL_KEYTEST",
    "reboot_pending": "SVAL_HOST_BOOTLOADER",
    "reboot_due_ms": "SVAL_HOST_BOOTLOADER",
    "update_no_chord": "SVAL_UPDATE_NO_CHORD (dev builds, D43)",
    "sval_dev_keep_settings": "SVAL_DEV_KEEP_SETTINGS (dev builds, D43)",
    "sval_perf_stats_build": "SVAL_PERF_STATS (dev builds)",
    "sval_perf_command": "SVAL_PERF_STATS (dev builds)",
    "sval_dev_command": "the raw HID 0xFC dev command (SVAL_TOUCH_DEV, SVAL_PERF_STATS)",
    "sval_mt_command": "SVAL_TOUCH_DEV (dev builds)",
    "velocity_scroll_dev_hold": "SVAL_TOUCH_DEV (dev builds)",
}


def check(path):
    errors = []
    symbols, read = cst.read_elf(path, kinds=(STT_OBJECT, STT_FUNC))
    for name, what in FORBIDDEN.items():
        if name in symbols:
            errors.append(f"symbol {name}: {what} is in this build")
    if "updater_task" not in symbols:
        errors.append("no updater_task: not an updater build (SVAL_UPDATER=yes)")

    info = symbols.get("sval_update_build_info", [])
    if len(info) != 1 or info[0][1] != mu.BUILD_INFO_BYTES:
        errors.append(f"expected one {mu.BUILD_INFO_BYTES}-byte sval_update_build_info, found {info}")
    else:
        (magic, ver, flags, reserved0, proto, fw, version, hand, pointing_id, keymap_id, reserved) = struct.unpack(mu.BUILD_INFO_FMT, read(info[0][0], mu.BUILD_INFO_BYTES))
        if magic + bytes([ver]) != mu.BUILD_INFO_MAGIC:
            errors.append(f"sval_update_build_info has magic {magic!r} version {ver}")
        if hand not in mu.HAND.values() or pointing_id not in mu.POINTING.values() or reserved or reserved0:
            errors.append(f"the build-info record has hand {hand}, pointing_id {pointing_id}, reserved {reserved0}/{reserved}")
        elif pointing_id != mu.POINTING["auto"]:
            errors.append(f"the build-info record has pointing_id {pointing_id}: a release is the auto build "
                          f"({mu.POINTING['auto']}, svalboard/either)")
        elif hand != mu.HAND["either"]:
            errors.append(f"the build-info record has hand {hand}: a release is one image for both halves "
                          f"(hand {mu.HAND['either']}, svalboard/either; docs/pair-uf2.md)")
        if keymap_id not in mu.KEYMAP.values():
            errors.append(f"the build-info record has keymap_id {keymap_id}: a release is the sval or blank keymap")
        if not flags & mu.BI_RELEASE:
            errors.append("not a release updater build (SVAL_UPDATE_RELEASE)")
        for bit, what in mu.BI_NOT_IN_RELEASE.items():
            if flags & bit:
                errors.append(f"the build-info record says {what}")
        if flags & mu.BI_RESERVED:
            errors.append(f"the build-info record has reserved flags set ({flags:#x})")
        if proto != mu.UPDATER_PROTO:
            errors.append(f"updater protocol {proto}, the tools speak {mu.UPDATER_PROTO}")
    return errors


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0 if argv else 2
    failed = 0
    for path in argv:
        try:
            errors = check(path)
        except (cst.ElfError, OSError, struct.error) as e:
            errors = [str(e)]
        name = Path(path).name
        if errors:
            failed += 1
            print(f"FAIL {name}")
            for e in errors:
                print(f"    {e}")
        else:
            print(f"ok   {name}: release updater build, no test hooks, keytest or host bootloader")
    print(f"{len(argv) - failed} of {len(argv)} release ELFs pass")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
