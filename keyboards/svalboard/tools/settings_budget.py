#!/usr/bin/env python3
"""The settings area's budget (keyboards/svalboard/settings_ids.h).

Prints how much of SVAL_SETTINGS_AREA_SIZE every record takes at full capacity.
Fails on overflow, a reused id, a reserved id (0, 0xFFFE, 0xFFFF, or the
range kept for downstream builds), or code that still uses the retired VIA
custom config; warns at half full, so a decision about large data comes early.
Run by tools/pre_pr.sh and tests/sval_storage.
"""
import pathlib
import re
import sys

KB = pathlib.Path(__file__).resolve().parents[1]
HEADER, ENTRY_HEAD, SLOT_HEAD, END = 8, 4, 4, 2
# Record ids kept for downstream builds: settings_ids.h never assigns them.
DOWNSTREAM_IDS = range(0xF000, 0xF100)


def records(text):
    """(name, id, capacity) of every X(...) line of settings_ids.h; ids may be decimal or hex."""
    return [(name, int(num, 0), int(cap, 0)) for name, num, cap in re.findall(r'^\s*X\((\w+),\s*(0[xX][0-9A-Fa-f]+|\d+),\s*(0[xX][0-9A-Fa-f]+|\d+)\)', text, re.M)]


def id_problems(ids):
    problems = []
    seen = {}
    for name, num, _ in ids:
        if num in seen:
            problems.append(f'id {num} used by both {seen[num]} and {name}')
        seen[num] = name
        if num in (0, 0xFFFE, 0xFFFF):
            problems.append(f'{name}: id {num} is reserved')
        elif num in DOWNSTREAM_IDS:
            problems.append(f'{name}: id 0x{num:04X} is in 0xF000-0xF0FF, reserved for downstream builds')
    return problems


def main():
    ids = records((KB / 'settings_ids.h').read_text())
    size = int(re.search(r'#define SVAL_SETTINGS_AREA_SIZE (\d+)', (KB / 'config.h').read_text()).group(1))
    problems = id_problems(ids)
    used = HEADER + END + sum(ENTRY_HEAD + 2 * (SLOT_HEAD + cap) for _, _, cap in ids)
    print(f'settings area: {used} of {size} bytes ({100 * used // size}%), {len(ids)} records')
    if used > size:
        problems.append(f'over the area by {used - size} bytes')
    elif used * 2 >= size:
        print(f'WARNING: the settings area is {100 * used // size}% full; plan where large data goes before it fills')
    for path in KB.rglob('*.[ch]'):
        if re.search(r'\bvia_(read|update)_custom_config\s*\(', path.read_text(errors='replace')):
            problems.append(f'{path.relative_to(KB)}: uses the retired VIA custom config; use the settings area')
    for p in problems:
        print(f'FAILED: {p}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
