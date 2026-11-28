#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/check_ram_funcs.py: which registers
an instruction writes, and the reaching-definitions proof of a blx target, on
hand-written Thumb listings (no toolchain needed).
"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "keyboards/svalboard/tools/check_ram_funcs.py"
spec = importlib.util.spec_from_file_location("check_ram_funcs", TOOL)
cr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cr)

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def fn(body):
    """A Function from (mnemonic, operands, comment) rows at 0x20000000, 2 bytes
    each, then a literal word 0x20001001 at the end (what ldr r3, [pc, #n] loads)."""
    start, insns = 0x20000000, []
    for k, (mnem, ops, comment) in enumerate(body):
        insns.append((start + 2 * k, mnem, ops, comment))
    lit = start + 2 * len(body)
    lit += lit % 4
    insns = [(a, m, o, c.replace("LIT", f"{lit:x}")) for a, m, o, c in insns]
    insns.append((lit, ".word", "0x20001001", ""))
    return cr.Function("f", start, lit + 4 - start, insns)


def blx_values(body):
    f = fn(body)
    use = next(a for a, m, _, _ in f.code if m == "blx")
    return cr.resolve_address(f, use, "r3")


# ---- regs_written ---------------------------------------------------------------------
check(cr.regs_written("bics", "r3, r2") == {"r3"}, f"bics writes r3: {cr.regs_written('bics', 'r3, r2')}")
check(cr.regs_written("bic", "r3, r2") == {"r3"}, "bic writes r3")
check(cr.regs_written("stmia", "r3!, {r1, r2}") == {"r3"}, f"stmia r3! writes r3: {cr.regs_written('stmia', 'r3!, {r1, r2}')}")
check(cr.regs_written("stmia", "r3, {r1, r2}") == set(), "stm without write-back writes nothing")
check(cr.regs_written("ldmia", "r3!, {r1, r2}") == {"r1", "r2", "r3"}, "ldmia r3! writes the list and r3")
for mnem, ops in (("b.n", "20000010 <f+0x10>"), ("bne.n", "20000010 <f+0x10>"), ("bl", "20000100 <g>"), ("bx", "lr"), ("blx", "r3"), ("bkpt", "0x0000"), ("str", "r3, [r2, #0]"), ("push", "{r4, lr}")):
    check(cr.regs_written(mnem, ops) == set(), f"{mnem} {ops} writes no register the proof tracks")
check(cr.regs_written("movs", "r3, #1") == {"r3"}, "movs writes r3")

# ---- the blx proof ----------------------------------------------------------------------
LDR = ("ldr", "r3, [pc, #4]", "(LIT <f+0x8>)")
check(blx_values([LDR, ("blx", "r3", "")]) == {("lit", 0x20001001)}, "a literal reaches blx: proven")
v = blx_values([LDR, ("bics", "r3, r2", ""), ("blx", "r3", "")])
check(v is None, f"bics between the literal and blx: unproven, not the stale literal ({v})")
v = blx_values([LDR, ("stmia", "r3!, {r1, r2}", ""), ("blx", "r3", "")])
check(v is None, f"stmia r3! between the literal and blx: unproven, not the stale literal ({v})")
v = blx_values([LDR, ("stmia", "r2!, {r1}", ""), ("blx", "r3", "")])
check(v == {("lit", 0x20001001)}, f"stmia on another base keeps the proof ({v})")

print(f"check_ram_funcs.py: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
