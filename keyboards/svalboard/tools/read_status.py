#!/usr/bin/env python3
"""Read a board's status report (Sval STATUS_REPORT 0x3F) and print it.

    python3 read_status.py --serial sval:E465B0621725122B [--raw]

Prints GET_INFO byte 22 (the fifth feature byte), the build text (page 1)
and every field of the state (page 0), as docs/protocol.md "Status report"
describes them. --raw also prints page 0's bytes. Needs hidapi.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "multitouch"))
from sval_raw import Board  # noqa: E402

SVAL = 0xDF
OS = {0: "not known", 1: "Linux", 2: "Windows", 3: "macOS", 4: "iOS"}


def u16(b, o):
    return b[o] | b[o + 1] << 8


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial", required=True)
    ap.add_argument("--raw", action="store_true")
    a = ap.parse_args()
    board = Board(a.serial)
    try:
        info = board.command(SVAL, 0x00)[5:]  # info[i] is data[i]: [0xDF] [0x00] ...
        flags5 = info[22] if len(info) > 22 else 0
        print(f"GET_INFO byte 22: 0x{flags5:02x} (status report {'yes' if flags5 & 0x02 else 'no'})")
        if not flags5 & 0x02:
            return 1
        build, off = b"", 0
        while True:
            r = board.command(SVAL, 0x3F, bytes((1, off)))[7:]
            if r[1] != 1:
                print("page 1: format", r[1])
                return 1
            total, n = r[2], r[4]
            build += r[5:5 + n]
            off += n
            if n == 0 or off >= total:
                break
        print("build:", build.decode(errors="replace"))
        p = board.command(SVAL, 0x3F, bytes((0, 0)))[7:]
        if p[1] != 1:
            print("page 0: format", p[1])
            return 1
        if a.raw:
            print("page 0:", p[:40].hex(" "))
        fa, fb = p[2], p[3]
        bit = lambda v, n: "on" if v & (1 << n) else "off"  # noqa: E731
        print(f"left scroll {bit(fa, 0)}, {u16(p, 5)} CPI; right scroll {bit(fa, 1)}, {u16(p, 7)} CPI")
        print(f"computer {OS.get(p[4], p[4])}, Mac mode {bit(fa, 7)}, axis lock {bit(fa, 2)}, natural scroll {bit(fa, 3)}")
        am = "no timeout" if fb & 0x10 else f"{u16(p, 9)} ms"
        print(f"Auto Mouse {bit(fa, 4)}, left {bit(fa, 5)}, right {bit(fa, 6)}, {am}")
        print(f"Boost held {p[11]} {p[12]} {p[13]}, toggled 0x{p[14]:x}, multiplier {p[15]}")
        print(f"hardware rev {'B' if fb & 1 else 'A'}, waits {u16(p, 16)}/{u16(p, 18)} us, optical debounce {bit(fb, 1)}")
        print(f"storage reset {bit(fb, 2)}, write failed {bit(fb, 3)}")
        print(f"scan period {u16(p, 20)} us; light idle {u16(p, 22)} ms after {u16(p, 24)} ms; deep idle {u16(p, 26)} ms after {u16(p, 28)} s")
        frame = u16(p, 30) | u16(p, 32) << 16
        print(f"measured frame {frame} us, LED on {u16(p, 34)} us, stage {p[36]}")
        print(f"idle flags 0x{p[37]:02x}, deep clock {p[38]} MHz, clock now {p[39]} MHz")
    finally:
        board.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
