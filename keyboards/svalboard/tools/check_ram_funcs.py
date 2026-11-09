#!/usr/bin/env python3
"""Check that the updater's commit code can never reach flash while XIP is off.

    check_ram_funcs.py BUILD.elf [--obj update_commit.o] [--su update_commit.su]
                       [--objdump arm-none-eabi-objdump] [--max-stack 512]

The commit (keyboards/svalboard/updater/update_commit.c, plan R2) runs from RAM
while the firmware area is erased. Its RAM set is every .time_critical.*
section of update_commit.o. For that set this checks, and exits 1 on any
failure:

  object file (before linking)
  - every relocation in the RAM set is a call/branch to another RAM-set
    function, or a data word naming the commit's own .bss/.data or a RAM-set
    function; nothing in .rodata or .text, no external symbol (memcpy,
    __aeabi_*, flash code), no other relocation type;
  - every commit_ram_* function is in the RAM set;

  linked ELF (arm-none-eabi-objdump -d)
  - each function lies in SRAM;
  - every direct branch or call lands inside the same function, at the start
    of a RAM-set function, or in the boot ROM (< 0x4000); a branch to a linker
    veneer is followed to its final target, which must pass the same test;
  - every blx/bx rN (other than bx lr) is proven, by reaching definitions over
    the function's control-flow graph, to take its address from a literal that
    is the commit's boot2 copy + 1, or from a word of commit_rom (the boot ROM
    pointers step 0 looked up and checked); anything unproven fails;
  - no other write to pc (ldr pc, mov pc, add pc: jump tables), no svc or
    bkpt; udf only in commit_ram_halt (the test-hook HardFault);
  - no literal word in flash (0x10000000-0x12FFFFFF: code or .rodata read
    with XIP off), and no literal equal to the double-tap magic 0xCAFEB0BA
    (the reset sweeps RAM for it);

  stack (-fstack-usage, update_commit.su)
  - each RAM-set function's frame is static and at most --max-stack bytes,
    and so is the deepest chain of RAM-set calls from commit_ram_main and
    from commit_ram_reset (the fault handler).

The allow-list is fixed here (RAM_DATA_OK, the ROM bound, the boot2 entry);
widen it only together with a review of update_commit.c.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROM_END = 0x4000
SRAM = (0x20000000, 0x20042000)
FLASH_LITERAL = (0x10000000, 0x13000000)  # XIP cached alias: code and .rodata
DOUBLE_TAP_MAGIC = 0xCAFEB0BA
ROM_TABLE = "commit_rom"
BOOT2 = "commit_boot2"
REQUIRED = {"commit_ram_main", "commit_ram_reset", "commit_ram_flash"}
CALL_RELOCS = {"R_ARM_THM_CALL", "R_ARM_THM_JUMP24", "R_ARM_THM_JUMP11", "R_ARM_THM_JUMP8"}
RAM_DATA_OK = re.compile(r"^\.(bss|data)\.")  # the commit's own statics (-fdata-sections)

errors = []


def fail(msg):
    errors.append(msg)


def run(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


# ---- object file ------------------------------------------------------------------------


def ram_set_from_obj(objdump, obj):
    names = []
    for line in run(objdump, "-h", str(obj)).splitlines():
        m = re.match(r"\s*\d+\s+\.time_critical\.(\S+)\s", line)
        if m:
            names.append(m.group(1))
    syms = set()
    for line in run(objdump, "-t", str(obj)).splitlines():
        m = re.search(r"\s(\S+)\s+[0-9a-f]+\s+(commit_ram_\w+)$", line)
        if m and " F " in line:
            syms.add((m.group(1), m.group(2)))
    for sec, name in syms:
        if sec != f".time_critical.{name}":
            fail(f"{name} is in {sec}, not in its own .time_critical section (it would run from flash)")
    return set(names)


def check_relocs(objdump, obj, ram_set):
    current = None
    for line in run(objdump, "-r", str(obj)).splitlines():
        m = re.match(r"RELOCATION RECORDS FOR \[(.+)\]:", line)
        if m:
            sec = m.group(1)
            current = sec[len(".time_critical."):] if sec.startswith(".time_critical.") else None
            continue
        if current is None:
            continue
        m = re.match(r"([0-9a-f]+)\s+(\S+)\s+(\S+)", line)
        if not m or m.group(1) == "OFFSET":
            continue
        off, rtype, target = m.group(1), m.group(2), m.group(3)
        base = re.sub(r"[+-]0x[0-9a-f]+$", "", target)
        if rtype in CALL_RELOCS:
            if base not in ram_set:
                fail(f"{current}+0x{off}: {rtype} to {target}, which is not in the RAM set")
        elif rtype == "R_ARM_ABS32":
            if not (RAM_DATA_OK.match(base) or base in ram_set or base.startswith(".time_critical.")):
                fail(f"{current}+0x{off}: data word refers to {target} (not the commit's own RAM)")
        else:
            fail(f"{current}+0x{off}: unexpected relocation {rtype} to {target}")


# ---- ELF ----------------------------------------------------------------------------------


def elf_symbols(objdump, elf):
    """name -> list of (addr, size, kind) for every symbol."""
    syms = {}
    for line in run(objdump, "-t", str(elf)).splitlines():
        m = re.match(r"([0-9a-f]{8}) (.{7}) (\S+)\s+([0-9a-f]{8}) (\S+)$", line)
        if not m:
            continue
        addr, flags, sec, size, name = int(m.group(1), 16), m.group(2), m.group(3), int(m.group(4), 16), m.group(5)
        kind = "F" if "F" in flags else ("O" if "O" in flags else "?")
        syms.setdefault(name, []).append((addr, size, kind))
    return syms


def unique(syms, name, kind=None):
    hits = [s for s in syms.get(name, []) if kind is None or s[2] == kind]
    if len(hits) != 1:
        fail(f"{name}: expected one symbol in the ELF, found {len(hits)}")
        return None
    return hits[0]


INSN = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")


def disassemble(objdump, elf, start, end):
    """[(addr, mnemonic, operands, comment)], literal words as ('.word', value)."""
    out = []
    text = run(objdump, "-d", "--no-show-raw-insn", f"--start-address=0x{start:x}", f"--stop-address=0x{end:x}", str(elf))
    for line in text.splitlines():
        m = INSN.match(line)
        if not m:
            continue
        addr, rest = int(m.group(1), 16), m.group(2)
        comment = ""
        if "@" in rest:
            rest, comment = rest.split("@", 1)
        rest = rest.strip()
        if ";" in rest:
            rest = rest.split(";", 1)[0].strip()
        parts = rest.split(None, 1)
        if not parts:
            continue
        mnem = parts[0]
        ops = parts[1].strip() if len(parts) > 1 else ""
        out.append((addr, mnem, ops, comment.strip()))
    return out


def literal_at(insns_by_addr, addr):
    i = insns_by_addr.get(addr)
    if i and i[1] == ".word":
        return int(i[2].split()[0], 16)
    return None


BRANCH = re.compile(r"^(b|bl)(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.n|\.w)?$")
NO_WRITE = re.compile(r"^(str\w*|cmp|cmn|tst|push|stm\w*|nop|cps\w+|dsb|isb|dmb|svc|bkpt|udf|wfi|wfe|sev|yield|b\w*|\.word|\.short|\.byte)$")
REG = re.compile(r"\b(r\d+|ip|lr|sp|pc|fp|sl|sb)\b")
ALIAS = {"ip": "r12", "fp": "r11", "sl": "r10", "sb": "r9"}


def norm(r):
    return ALIAS.get(r, r)


def branch_target(ops):
    m = re.match(r"([0-9a-f]+)\b", ops)
    return int(m.group(1), 16) if m else None


def regs_written(mnem, ops):
    if NO_WRITE.match(mnem):
        return set()
    if mnem.startswith("pop") or mnem.startswith("ldm"):
        written = set()
        lst = re.search(r"\{([^}]*)\}", ops)
        if lst:
            for part in lst.group(1).split(","):
                part = part.strip()
                rng = re.match(r"r(\d+)-r(\d+)", part)
                if rng:
                    written |= {f"r{k}" for k in range(int(rng.group(1)), int(rng.group(2)) + 1)}
                elif part:
                    written.add(norm(part))
        if mnem.startswith("ldm") and "!" in ops:
            written.add(norm(REG.search(ops).group(1)))
        return written
    m = REG.search(ops)
    return {norm(m.group(1))} if m else set()


class Function:
    def __init__(self, name, start, size, insns):
        self.name, self.start, self.end = name, start, start + size
        self.insns = [i for i in insns if self.start <= i[0] < self.end]
        self.by_addr = {i[0]: i for i in self.insns}
        self.code = [i for i in self.insns if i[1] not in (".word", ".short", ".byte")]
        self.index = {i[0]: k for k, i in enumerate(self.code)}
        self.preds = {i[0]: set() for i in self.code}
        for k, (addr, mnem, ops, _) in enumerate(self.code):
            nxt = self.code[k + 1][0] if k + 1 < len(self.code) else None
            m = BRANCH.match(mnem)
            if m and m.group(1) == "b":
                tgt = branch_target(ops)
                if tgt in self.preds:
                    self.preds[tgt].add(addr)
                if m.group(2) not in (None, "al") and nxt is not None:
                    self.preds[nxt].add(addr)  # conditional: falls through too
                continue
            ends = (mnem.startswith("pop") and "pc" in ops) or (mnem == "bx") or mnem == "udf"
            if not ends and nxt is not None:
                self.preds[nxt].add(addr)

    def reaching_defs(self, use_addr, reg):
        """Instructions that may define reg for the instruction at use_addr.
        None when some path reaches the entry, or crosses a call that
        clobbers reg, without a definition."""
        defs, seen = set(), set()
        stack = list(self.preds[use_addr])
        if use_addr == self.start and not stack:
            return None
        while stack:
            a = stack.pop()
            if a in seen:
                continue
            seen.add(a)
            _, mnem, ops, _ = self.by_addr[a]
            if reg in regs_written(mnem, ops):
                defs.add(a)
                continue
            if mnem in ("bl", "blx") and reg in ("r0", "r1", "r2", "r3", "r12", "lr"):
                return None  # clobbered by the call
            if a == self.start:
                return None  # the value comes from the caller
            stack.extend(self.preds[a])
        return defs


def resolve_address(fn, use_addr, reg, depth=0):
    """The set of possible values of reg at use_addr, as ('lit', value) or
    ('rom_word', offset); None when unproven."""
    if depth > 8:
        return None
    defs = fn.reaching_defs(use_addr, reg)
    if not defs:
        return None
    values = set()
    for d in defs:
        _, mnem, ops, comment = fn.by_addr[d]
        m = re.match(r"ldr\s*$", mnem) and re.match(r"(r\d+), \[pc, #\d+\]", ops)
        if m:
            lit = re.search(r"\(([0-9a-f]+) <", comment) or re.search(r"\(([0-9a-f]+)\)", comment)
            if not lit:
                return None
            v = literal_at(fn.by_addr, int(lit.group(1), 16))
            if v is None:
                return None
            values.add(("lit", v))
            continue
        m = re.match(r"(r\d+), \[(r\d+)(?:, #(\d+))?\]$", ops) if mnem == "ldr" else None
        if m:
            base = resolve_address(fn, d, m.group(2), depth + 1)
            if base is None:
                return None
            for kind, v in base:
                if kind != "lit":
                    return None
                values.add(("load", v + int(m.group(3) or 0)))
            continue
        m = re.match(r"(r\d+), (r\d+|ip)$", ops) if mnem in ("mov", "movs") else None
        if m:
            src = resolve_address(fn, d, norm(m.group(2)), depth + 1)
            if src is None:
                return None
            values |= src
            continue
        m = re.match(r"(r\d+), (?:(r\d+), )?#(\d+)$", ops) if mnem in ("adds", "add") else None
        if m and (m.group(2) in (None, m.group(1))):
            src = resolve_address(fn, d, m.group(1), depth + 1)
            if src is None:
                return None
            for kind, v in src:
                if kind != "lit":
                    return None
                values.add(("lit", v + int(m.group(3))))
            continue
        return None
    return values


def check_function(fn, ctx):
    for addr, mnem, ops, _ in fn.insns:
        where = f"{fn.name}+0x{addr - fn.start:x} ({addr:08x})"
        if mnem == ".word":
            v = int(ops.split()[0], 16)
            if v == DOUBLE_TAP_MAGIC:
                fail(f"{where}: literal 0x{v:08x} is the double-tap magic")
            if FLASH_LITERAL[0] <= v < FLASH_LITERAL[1]:
                fail(f"{where}: literal 0x{v:08x} points into flash")
            continue
        if mnem in ("svc", "bkpt"):
            fail(f"{where}: {mnem}")
        if mnem == "udf" and fn.name != "commit_ram_halt":
            fail(f"{where}: udf outside commit_ram_halt")
        m = BRANCH.match(mnem)
        if m:
            tgt = branch_target(ops)
            if tgt is None:
                fail(f"{where}: cannot read the target of {mnem} {ops}")
            elif fn.start <= tgt < fn.end and m.group(1) == "b":
                pass
            else:
                check_call_target(where, tgt, ctx)
            continue
        if mnem in ("blx", "bx"):
            reg = norm(ops.strip())
            if mnem == "bx" and reg == "lr":
                continue
            vals = resolve_address(fn, addr, reg)
            if not vals:
                fail(f"{where}: {mnem} {reg} with an address that cannot be proven safe")
                continue
            for kind, v in vals:
                if kind == "load" and ctx["rom"] and ctx["rom"][0] <= v < ctx["rom"][0] + ctx["rom"][1] and v % 4 == 0:
                    continue  # a boot ROM pointer from commit_rom
                if kind == "lit" and ctx["boot2"] and v == ctx["boot2"][0] + 1:
                    continue  # the commit's boot2 copy
                if kind == "lit" and v & 1 and v < ROM_END:
                    continue
                if kind == "lit" and (v & ~1) in ctx["starts"]:
                    continue
                fail(f"{where}: {mnem} {reg} may go to {kind} 0x{v:08x}, which is not allowed")
            continue
        if "pc" in [norm(r) for r in REG.findall(ops)][:1] and not mnem.startswith("pop") and not mnem.startswith("push"):
            fail(f"{where}: {mnem} {ops} writes pc")
        if mnem.startswith("pop") and "pc" in ops:
            continue


def check_call_target(where, tgt, ctx, hops=0):
    if tgt < ROM_END:
        return
    if tgt in ctx["starts"]:
        return
    name = ctx["addr_name"].get(tgt, "?")
    if name.endswith("_veneer") and hops < 3:
        final = veneer_target(ctx, tgt)
        if final is None:
            fail(f"{where}: calls veneer {name} whose target cannot be read")
        else:
            check_call_target(f"{where} via {name}", final & ~1, ctx, hops + 1)
        return
    fail(f"{where}: branches to 0x{tgt:08x} ({name}), outside the RAM set and the ROM")


def veneer_target(ctx, addr):
    insns = disassemble(ctx["objdump"], ctx["elf"], addr, addr + 0x20)
    by_addr = {i[0]: i for i in insns}
    for a, mnem, ops, comment in insns:
        if mnem == "ldr":
            lit = re.search(r"\(([0-9a-f]+) <", comment) or re.search(r"\(([0-9a-f]+)\)", comment)
            if lit:
                return literal_at(by_addr, int(lit.group(1), 16))
        m = BRANCH.match(mnem)
        if m:
            return branch_target(ops)
    words = [i for i in insns if i[1] == ".word"]
    return int(words[0][2].split()[0], 16) if words else None


# ---- stack -------------------------------------------------------------------------------


def check_stack(su, ram_set, calls, limit):
    frames = {}
    for line in Path(su).read_text().splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        name = parts[0].rsplit(":", 1)[-1]
        frames[name] = (int(parts[1]), parts[2].strip())
    for name in sorted(ram_set):
        if name not in frames:
            fail(f"{name}: no -fstack-usage entry")
            continue
        size, kind = frames[name]
        if kind != "static":
            fail(f"{name}: stack usage is {kind}, not static")
        if size > limit:
            fail(f"{name}: {size} B of stack, over {limit}")

    def depth(name, path=()):
        if name in path:
            return 0  # recursion would also be a problem, but the RAM code has none
        own = frames.get(name, (10**6, ""))[0]
        return own + max([depth(c, path + (name,)) for c in calls.get(name, ()) if c != name] or [0])

    report = {}
    for root in ("commit_ram_main", "commit_ram_reset"):
        if root in ram_set:
            d = depth(root)
            report[root] = d
            if d > limit:
                fail(f"deepest RAM-set call chain from {root} uses {d} B of stack, over {limit}")
    return frames, report


# ---- main --------------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("elf", type=Path)
    ap.add_argument("--obj", type=Path, help="update_commit.o (default: found under the ELF's obj_ directory)")
    ap.add_argument("--su", type=Path, help="update_commit.su (default: next to the .o)")
    ap.add_argument("--objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--max-stack", type=int, default=512)
    args = ap.parse_args()

    obj = args.obj or args.elf.parent / f"obj_{args.elf.stem}" / "updater" / "update_commit.o"
    su = args.su or obj.with_suffix(".su")
    ram_set = ram_set_from_obj(args.objdump, obj)
    if not ram_set:
        fail(f"{obj}: no .time_critical sections")
    for name in REQUIRED - ram_set:
        fail(f"{name} is not in the RAM set")
    check_relocs(args.objdump, obj, ram_set)

    syms = elf_symbols(args.objdump, args.elf)
    ctx = {"objdump": args.objdump, "elf": args.elf, "starts": set(), "addr_name": {}}
    for name, entries in syms.items():
        for addr, _, _ in entries:
            ctx["addr_name"].setdefault(addr & ~1, name)
    funcs = []
    for name in sorted(ram_set):
        s = unique(syms, name, "F")
        if not s:
            continue
        addr, size, _ = s
        if not (SRAM[0] <= addr and addr + size <= SRAM[1]):
            fail(f"{name} is at 0x{addr:08x}, not in SRAM")
        ctx["starts"].add(addr & ~1)
        funcs.append((name, addr & ~1, size))
    rom = unique(syms, ROM_TABLE, "O")
    boot2 = unique(syms, BOOT2, "O")
    ctx["rom"] = rom[:2] if rom else None
    ctx["boot2"] = boot2[:2] if boot2 else None
    if rom and rom[1] != 20:
        fail(f"{ROM_TABLE} is {rom[1]} B, expected 5 pointers")
    if boot2 and not (SRAM[0] <= boot2[0] < SRAM[1] and boot2[1] == 256):
        fail(f"{BOOT2} is not a 256 B RAM buffer")

    calls = {}
    total = 0
    for name, start, size in funcs:
        fn = Function(name, start, size, disassemble(args.objdump, args.elf, start, start + size))
        if not fn.code:
            fail(f"{name}: no instructions disassembled")
        total += len(fn.code)
        check_function(fn, ctx)
        by_start = {a: n for n, a, _ in funcs}
        calls[name] = sorted({by_start[branch_target(ops)]
                              for _, mnem, ops, _ in fn.code
                              if mnem in ("bl",) and branch_target(ops) in by_start})

    frames, depths = check_stack(su, ram_set, calls, args.max_stack)
    for name, start, size in funcs:
        print(f"  {name:<20} 0x{start:08x} {size:5d} B  stack {frames.get(name, ('?',))[0]} B  calls {', '.join(calls.get(name, [])) or '-'}")
    for root, d in depths.items():
        print(f"  deepest chain from {root}: {d} B")
    if errors:
        for e in errors:
            print("FAIL:", e)
        print(f"check_ram_funcs: {len(errors)} problem(s) in {args.elf}")
        return 1
    print(f"check_ram_funcs: OK, {len(funcs)} RAM functions, {total} instructions ({args.elf.name})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
