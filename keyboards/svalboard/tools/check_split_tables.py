#!/usr/bin/env python3
"""Check that every supported left/right pairing of builds agrees on the split message table (P1).

    check_split_tables.py --left L1.elf [L2.elf ...] --right R1.elf [R2.elf ...]
    check_split_tables.py --dir BUILD_DIR [--dir ...]
    check_split_tables.py --dump BUILD.elf [--json]

Both halves of a Svalboard run the same release. A release has one build per
side (the auto builds, D34), but a half may run another build of the same
release (a single-module build, D18). Over the split link each
side frames every transaction with its own table: the transaction ID, the
buffer sizes and the shared-memory offsets come from
quantum/split_common/transactions.c's split_transaction_table, and both sides
fold NUM_TOTAL_TRANSACTIONS into every handshake
(platforms/chibios/drivers/serial_protocol.c). Two builds whose tables differ
cannot talk, or worse, misread each other's buffers. See
docs/updater-plan.md, P1 and R14.

From each linked ELF this reads split_transaction_table (its size gives
NUM_TOTAL_TRANSACTIONS) and, for each transaction ID (the table index), the
initiator-to-target and target-to-initiator buffer sizes and shared-memory
offsets and whether a slave callback is set. The ELFs carry no DWARF, so the
entry layout is QMK's split_transaction_desc_t on the Arm EABI:

    uint8_t i2t_size; (pad) uint16_t i2t_offset; uint8_t t2i_size; (pad)
    uint16_t t2i_offset; slave_callback_t callback      -> 12 bytes

The layout is cross-checked: padding bytes must be 0, and each callback must
be 0 or an odd (Thumb) address in flash or RAM; anything else fails as an
unreadable table. Entries for keyboard RPC IDs (SPLIT_TRANSACTION_IDS_KB) are
all zero in the ELF: transaction_register_rpc() fills them at boot with the
RPC buffers' offsets, which the PUT_RPC_REQ_DATA / GET_RPC_RESP_DATA entries
already carry.

So that the order of those IDs can be checked too, updater builds carry a
table sval_split_kb_ids (updater/update_split.c): b"SVKB", the number of
entries, then for each keyboard RPC its ID and a two-letter tag (SA, SB, UP
for KEYBOARD_SYNC_A, KEYBOARD_SYNC_B, KEYBOARD_UPDATE). It must be byte for
byte the same on both sides; one side having it and the other not is a
mismatch. Default builds have none (they are not paired with updater builds:
their transaction counts differ anyway).

With --left/--right every left ELF is checked against every right ELF; with
--dir, *left*.elf and *right*.elf in each directory are paired the same way,
and an *either*.elf (one image for both halves) counts as both.
Exit 1 on any mismatch or unreadable table. The size of the shared-memory
block is printed and compared too, but a difference there is only a warning:
it does not travel on the wire.

Golden table (M3): --golden FILE also compares every table read with a
committed golden copy (keyboards/svalboard/tools/split_table_golden.json),
so a release cannot change the split message table, its buffer sizes or its
shared-memory offsets without the golden file changing in the same commit.
--write-golden FILE ELF writes one from a build. Release CI runs

    check_split_tables.py --dir ELF_DIR --golden keyboards/svalboard/tools/split_table_golden.json
"""
import argparse
import glob
import json
import os
import struct
import sys

ENTRY = 12
TABLE = "split_transaction_table"
SHMEM = "shared_memory"  # quantum/split_common/transport.c: static split_shared_memory_t shared_memory
KB_IDS = "sval_split_kb_ids"  # updater/update_split.c, updater builds only
MAX_TRANSACTIONS = 32  # transaction_id_define.h: 5 bits


class ElfError(Exception):
    pass


def read_elf(path, kinds=(1,)):
    """Symbols {name: [(value, size)]} of the given STT_ kinds (1 object, 2 function) and a
    reader for initialized bytes at a virtual address."""
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise ElfError(f"{path}: not a 32-bit little-endian ELF")
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from("<IIIIIIIIII", data, e_shoff + i * e_shentsize)
        sections.append(dict(name=sh[0], type=sh[1], addr=sh[3], offset=sh[4], size=sh[5], link=sh[6], entsize=sh[9]))
    shstr = sections[e_shstrndx]

    def cstr(sec, off):
        start = sec["offset"] + off
        return data[start:data.index(b"\0", start)].decode("latin-1")

    for s in sections:
        s["name"] = cstr(shstr, s["name"])
    symbols = {}
    for s in sections:
        if s["type"] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[s["link"]]
        for i in range(s["size"] // 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, s["offset"] + i * 16)
            if name and (info & 0xF) in kinds:  # STT_OBJECT by default
                symbols.setdefault(cstr(strtab, name), []).append((value, size))

    def read(addr, n):
        for s in sections:
            if s["type"] == 1 and s["addr"] and s["addr"] <= addr and addr + n <= s["addr"] + s["size"]:  # SHT_PROGBITS
                o = s["offset"] + addr - s["addr"]
                return data[o:o + n]
        raise ElfError(f"{path}: no initialized section holds {n} bytes at {addr:#x}")

    return symbols, read


def one_symbol(symbols, name, path):
    found = symbols.get(name, [])
    if len(found) != 1:
        raise ElfError(f"{path}: expected one object symbol {name}, found {len(found)}")
    return found[0]


def callback_ok(cb):
    if cb == 0:
        return True
    in_code = 0x10000000 <= cb < 0x11000000 or 0x20000000 <= cb < 0x20042000
    return in_code and cb & 1 == 1


def read_table(path):
    symbols, read = read_elf(path)
    addr, size = one_symbol(symbols, TABLE, path)
    if size == 0 or size % ENTRY:
        raise ElfError(f"{path}: {TABLE} is {size} bytes, not a whole number of {ENTRY}-byte entries")
    count = size // ENTRY
    if count > MAX_TRANSACTIONS:
        raise ElfError(f"{path}: {count} transactions, more than the {MAX_TRANSACTIONS} the protocol allows")
    raw = read(addr, size)
    entries = []
    for i in range(count):
        i2t_size, pad0, i2t_off, t2i_size, pad1, t2i_off, cb = struct.unpack_from("<BBHBBHI", raw, i * ENTRY)
        if pad0 or pad1 or not callback_ok(cb):
            raise ElfError(f"{path}: entry {i} does not look like split_transaction_desc_t "
                           f"(padding {pad0:#x}/{pad1:#x}, callback {cb:#x}): has the struct changed?")
        entries.append(dict(id=i, i2t_size=i2t_size, i2t_offset=i2t_off, t2i_size=t2i_size, t2i_offset=t2i_off, callback=cb != 0))
    shmem = symbols.get(SHMEM, [])
    kb_ids = None
    if KB_IDS in symbols:
        kaddr, ksize = one_symbol(symbols, KB_IDS, path)
        raw_ids = read(kaddr, ksize)
        if ksize < 5 or raw_ids[:4] != b"SVKB" or ksize != 5 + 3 * raw_ids[4]:
            raise ElfError(f"{path}: {KB_IDS} is not b'SVKB', n, then n (id, tag) triples ({raw_ids.hex()})")
        kb_ids = [dict(id=raw_ids[5 + 3 * i], tag=raw_ids[6 + 3 * i:8 + 3 * i].decode("latin-1")) for i in range(raw_ids[4])]
        for e in kb_ids:
            if e["id"] >= count or any(entries[e["id"]][f] for f in ("i2t_size", "t2i_size", "callback")):
                raise ElfError(f"{path}: keyboard RPC {e['tag']} has ID {e['id']}, which is not an empty "
                               "(boot-filled) entry of the table")
    return dict(elf=path, count=count, entries=entries, shmem_size=shmem[0][1] if len(shmem) == 1 else None, kb_ids=kb_ids)


FIELDS = ("i2t_size", "i2t_offset", "t2i_size", "t2i_offset", "callback")


def golden_of(t):
    """What the golden file holds: the wire-relevant part of a table (no path, no shared-memory size)."""
    return dict(count=t["count"], entries=t["entries"], kb_ids=t.get("kb_ids"))


def compare(a, b):
    """Differences between two tables, as strings (empty: they match)."""
    diffs = []
    if a["count"] != b["count"]:
        diffs.append(f"NUM_TOTAL_TRANSACTIONS {a['count']} vs {b['count']}")
    for ea, eb in zip(a["entries"], b["entries"]):
        for f in FIELDS:
            if ea[f] != eb[f]:
                diffs.append(f"ID {ea['id']}: {f} {ea[f]} vs {eb[f]}")
    if a.get("kb_ids") != b.get("kb_ids"):

        def ids(t):
            return "none" if t.get("kb_ids") is None else ", ".join(f"{e['tag']}={e['id']}" for e in t["kb_ids"])

        diffs.append(f"keyboard RPC IDs ({KB_IDS}) {ids(a)} vs {ids(b)}")
    return diffs


def fmt(t):
    lines = [f"{t['elf']}: {t['count']} transactions, shared memory {t['shmem_size']} B"]
    if t.get("kb_ids") is not None:
        lines.append("  keyboard RPC IDs: " + ", ".join(f"{e['tag']}={e['id']}" for e in t["kb_ids"]))
    for e in t["entries"]:
        lines.append(f"  ID {e['id']:2}: m2s {e['i2t_size']:3} B @ {e['i2t_offset']:#06x}  "
                     f"s2m {e['t2i_size']:3} B @ {e['t2i_offset']:#06x}  {'callback' if e['callback'] else ''}")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--left", nargs="+", default=[], metavar="ELF")
    ap.add_argument("--right", nargs="+", default=[], metavar="ELF")
    ap.add_argument("--dir", action="append", default=[], help="pair every *left*.elf with every *right*.elf here")
    ap.add_argument("--dump", metavar="ELF", help="print one ELF's table")
    ap.add_argument("--json", action="store_true", help="with --dump: as JSON")
    ap.add_argument("--golden", metavar="FILE", help="also compare every table with this golden file (M3)")
    ap.add_argument("--write-golden", nargs=2, metavar=("FILE", "ELF"), help="write a golden file from one build")
    args = ap.parse_args()

    try:
        if args.dump:
            t = read_table(args.dump)
            print(json.dumps(t, indent=1) if args.json else fmt(t))
            return 0
        if args.write_golden:
            out, elf = args.write_golden
            with open(out, "w") as f:
                json.dump(golden_of(read_table(elf)), f, indent=1)
                f.write(chr(10))
            print(f"wrote {out} from {elf}")
            return 0
        left, right = list(args.left), list(args.right)
        for d in args.dir:
            for p in sorted(glob.glob(os.path.join(d, "*.elf"))):
                base = os.path.basename(p)
                if "left" in base:
                    left.append(p)
                elif "right" in base:
                    right.append(p)
                elif "either" in base:
                    # One image for both halves: it pairs with itself and
                    # with every other one (and, through the golden table,
                    # with the per-side releases in the field).
                    left.append(p)
                    right.append(p)
        if not left or not right:
            ap.error("need at least one left and one right ELF")
        tables = {p: read_table(p) for p in left + right}
    except (ElfError, OSError) as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1

    failed = 0
    for l in left:
        for r in right:
            diffs = compare(tables[l], tables[r])
            name = f"{os.path.basename(l)} + {os.path.basename(r)}"
            if diffs:
                failed += 1
                print(f"FAIL {name}")
                for d in diffs:
                    print(f"    {d}")
            else:
                print(f"ok   {name}: {tables[l]['count']} transactions")
            if tables[l]["shmem_size"] != tables[r]["shmem_size"]:
                print(f"     warning: shared memory {tables[l]['shmem_size']} B vs {tables[r]['shmem_size']} B")
    print(f"{len(left) * len(right) - failed} of {len(left) * len(right)} pairings match")
    if args.golden:
        try:
            with open(args.golden) as f:
                golden = json.load(f)
        except (OSError, ValueError) as e:
            print(f"FAIL: golden file {args.golden}: {e}")
            return 1
        golden["elf"] = os.path.basename(args.golden)
        bad = 0
        builds = list(dict.fromkeys(left + right))
        for p in builds:
            diffs = compare(tables[p], golden)
            if diffs:
                bad += 1
                print(f"FAIL {os.path.basename(p)} differs from the golden table")
                for d in diffs:
                    print(f"    {d}")
        print(f"{len(builds) - bad} of {len(builds)} builds match the golden table")
        failed += bad
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
