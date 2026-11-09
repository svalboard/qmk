"""Measure the pad two-finger scroll rate with no hands (dev builds, SVAL_TOUCH_DEV).

usage: python pad_scroll_rate.py --serial EXACT_SERIAL [--mac] [--mm 100]

Injects pad scroll pixels (47.6 per mm across, 44.8 up and down of swipe on the pad) into the
board over raw HID 0xFC op 0x42 action 3, forces the Mac scaling on (--mac) or
off, logs the wheel reports Windows receives with tb_log.py, and prints the
wheel counts, detents (120 counts) and mm of swipe per detent. Run it on the
test board only: the logger keeps wheel reports from every Svalboard.

Dev builds only: release builds have no raw HID 0xFC command.
"""
import argparse
import glob
import json
import os
import struct
import subprocess
import sys
import time

from sval_raw import Board

PX_PER_MM = {"v": 1792 / 40, "h": 2048 / 43}
TB_LOG = r"C:\Users\morga\Svalboard\handoff\ptp-grab\tools\tb_log.py"
CAPTURES = r"C:\Users\morga\Svalboard\handoff\ptp-grab\captures"


def inject(board, mac, h, v):
    r = board.command(0xFC, 0x42, bytes([3, 1 if mac else 0]) + struct.pack("<hh", h, v))
    if r[6] != 0xC2 or not (r[9] & 16):
        raise RuntimeError("this firmware has no pad scroll dev command (macscroll build)")


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--serial", required=True)
    p.add_argument("--mac", action="store_true")
    p.add_argument("--mm", type=float, default=100.0, help="mm of swipe to inject")
    p.add_argument("--axis", choices=["v", "h"], default="v")
    a = p.parse_args()
    px_total = round(a.mm * PX_PER_MM[a.axis])
    step = 12  # pixels per 10 ms: about 25 mm/s, a gentle swipe
    seconds = px_total / step * 0.01 + 4
    board = Board(a.serial)
    inject(board, a.mac, 0, 0)
    before = set(glob.glob(os.path.join(CAPTURES, "tblog-*.json")))
    logger = subprocess.Popen([sys.executable, TB_LOG, "--seconds", str(round(seconds))])
    try:
        time.sleep(1.5)
        sent = 0
        t0 = time.perf_counter()
        n = 0
        while sent < px_total:
            d = min(step, px_total - sent)
            inject(board, a.mac, d if a.axis == "h" else 0, d if a.axis == "v" else 0)
            sent += d
            n += 1
            time.sleep(max(0, t0 + n * 0.01 - time.perf_counter()))
        time.sleep(1.0)
        inject(board, False, 0, 0)
    finally:
        logger.wait()
        board.close()
    new = sorted(set(glob.glob(os.path.join(CAPTURES, "tblog-*.json"))) - before)
    rows = json.load(open(new[-1]))["rows"]
    flag = 0x0400 if a.axis == "v" else 0x0800
    # Other Svalboards may be in use: the injected one reports about every
    # 10 ms, so keep the device with the most wheel reports.
    wheel = [r for r in rows if r[4] & flag]
    names = [r[1] for r in wheel]
    if names:
        best = max(set(names), key=names.count)
        wheel = [r for r in wheel if r[1] == best]
    counts = sum(r[5] for r in wheel)
    reports = len(wheel)
    # Windows: 120 counts are a detent. A Mac reads each count as a line.
    unit = abs(counts) if a.mac else abs(counts) / 120
    name = "lines (counts)" if a.mac else "detents"
    print(f"mode {'Mac' if a.mac else 'Windows'}: injected {px_total} px ({a.mm:g} mm), "
          f"{reports} wheel reports, {counts} counts = {unit:.2f} {name}, "
          f"{a.mm / unit if unit else float('inf'):.2f} mm per {name.split()[0].rstrip('s')}")


if __name__ == "__main__":
    sys.exit(main())
