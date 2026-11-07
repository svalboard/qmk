#!/usr/bin/env python3
"""Read the trackball sensor quality stream (Sval 0x39, docs/protocol.md "Pointing sensor").

Needs hidapi (pip install hidapi) and a board whose firmware has POINTING_SENSOR.
Close other configuration clients (Keybard) first: raw HID replies go to every open handle.

    read_squal.py [--serial S] [--seconds N] [--window BINS] [--csv FILE] [--quiet]

Polls both hands, prints each hand's rolling window (SQUAL min / mean / max, raw data sum,
raw max and min, shutter, the sensor read rate the firmware achieved) once a second, and at
the end the bins each hand delivered per second. With --csv every bin is written out.
"""
import argparse
import csv
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sval_update as su  # noqa: E402  (same directory: the HID device and the client wrapper)

CMD = 0x39
OP_SAMPLES, OP_STATS = 0, 1
REPLY_LEN = 25
HEAD_LEN, SAMPLE_LEN = 7, 9
F_CAPABLE, F_3389, F_LOST, F_MORE, F_RIGHT, F_LIFTED = 0x01, 0x02, 0x04, 0x08, 0x10, 0x80
FEATURE_BYTE, FEATURE_BIT = 21, 0x08  # GET_INFO byte 21, sval_flag4_pointing_sensor
HANDS = ("left", "right")


def request(dev, op, hand, arg=0):
    return bytes(dev.sval(CMD, bytes([op, hand, arg & 0xFF, arg >> 8]))[:REPLY_LEN])


def decode_bin(b):
    squal, smin, smax, rawsum, rawmax, rawmin, shutter, moving = struct.unpack("<BBBBBBHB", b)
    return {"squal": squal, "squal_min": smin, "squal_max": smax, "rawsum": rawsum, "rawmax": rawmax,
            "rawmin": rawmin, "shutter": shutter, "moving": moving & 0x7F, "lifted": bool(moving & 0x80)}


def decode_samples(r):
    if r[0] != 1:
        return None
    flags, first, n, head = r[1], struct.unpack_from("<H", r, 2)[0], r[4], struct.unpack_from("<H", r, 5)[0]
    bins = [decode_bin(r[HEAD_LEN + i * SAMPLE_LEN:HEAD_LEN + (i + 1) * SAMPLE_LEN]) for i in range(n)]
    for i, b in enumerate(bins):
        b["seq"] = (first + i) & 0xFFFF
    return {"flags": flags, "first": first, "head": head, "bins": bins}


def decode_stats(r):
    if r[0] != 1:
        return None
    u16 = lambda o: struct.unpack_from("<H", r, o)[0]
    return {"flags": r[1], "head": u16(2), "bins": u16(4), "squal_min": r[6], "squal_max": r[7],
            "squal_mean": u16(8) / 256.0, "rawsum_min": r[10], "rawsum_max": r[11], "rawsum_mean": r[12],
            "rawmax": r[13], "rawmin": r[14], "shutter_min": u16(15), "shutter_max": u16(17),
            "shutter_mean": u16(19), "moving_pct": r[21], "reads_per_s": u16(22)}


def stats_line(hand, st):
    sensor = "3389" if st["flags"] & F_3389 else "3360"
    lift = " LIFTED" if st["flags"] & F_LIFTED else ""
    return (f"{hand:5} PMW{sensor} bins {st['bins']:3}  SQUAL {st['squal_min']:3}/{st['squal_mean']:6.1f}/{st['squal_max']:3}"
            f"  rawsum {st['rawsum_min']:3}/{st['rawsum_mean']:3}/{st['rawsum_max']:3}  raw max {st['rawmax']:3} min {st['rawmin']:3}"
            f"  shutter {st['shutter_min']}/{st['shutter_mean']}/{st['shutter_max']}"
            f"  moving {st['moving_pct']:3}%  reads/s {st['reads_per_s']}{lift}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="exact device serial (sval_update.py list)")
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--window", type=int, default=0, help="bins in the rolling window (0 = all held, up to 256 = 12.8 s)")
    ap.add_argument("--csv", type=Path, help="write every bin to this file")
    ap.add_argument("--quiet", action="store_true", help="only the summary at the end")
    args = ap.parse_args()

    dev = su.Device(args.serial)
    try:
        info = dev.sval(0x00)
        if not (info[FEATURE_BYTE - 2] & FEATURE_BIT):
            print("This firmware has no POINTING_SENSOR (0x39).", file=sys.stderr)
            return 2
        print(f"board {dev.serial}")
        cursor = {0: 0, 1: 0}
        delivered = {0: 0, 1: 0}
        lost = {0: 0, 1: 0}
        capable = {0: None, 1: None}
        last_stats = {0: None, 1: None}
        rows = []
        t0 = time.monotonic()
        next_print = t0 + 1.0
        requests = 0
        while time.monotonic() - t0 < args.seconds:
            for hand in (0, 1):
                if capable[hand] is False:
                    continue
                for _ in range(8):  # catch up, at most 8 requests per hand per pass
                    r = decode_samples(request(dev, OP_SAMPLES, hand, cursor[hand]))
                    requests += 1
                    if r is None:
                        break
                    capable[hand] = bool(r["flags"] & F_CAPABLE)
                    if r["flags"] & F_LOST:
                        lost[hand] += 1
                    for b in r["bins"]:
                        rows.append((time.monotonic() - t0, hand, b))
                    delivered[hand] += len(r["bins"])
                    cursor[hand] = (r["first"] + len(r["bins"])) & 0xFFFF
                    if not (r["flags"] & F_MORE):
                        break
                else:
                    pass
                if capable[hand] is None:  # format 0: nothing on that hand, or the link is down
                    capable[hand] = None
                st = decode_stats(request(dev, OP_STATS, hand, args.window))
                requests += 1
                if st is not None:
                    last_stats[hand] = st
            now = time.monotonic()
            if now >= next_print:
                next_print += 1.0
                if not args.quiet:
                    print(f"t={now - t0:5.1f}s")
                    for hand in (0, 1):
                        if last_stats[hand]:
                            print("  " + stats_line(HANDS[hand], last_stats[hand]))
                        else:
                            print(f"  {HANDS[hand]:5} no sensor data (no PMW3360/3389 on that hand, or the link is down)")
            time.sleep(0.02)
        elapsed = time.monotonic() - t0
        print(f"\n{elapsed:.1f} s, {requests} requests ({requests / elapsed:.0f}/s)")
        for hand in (0, 1):
            st = last_stats[hand]
            if st is None:
                print(f"{HANDS[hand]:5}: no data")
                continue
            hand_rows = [b for _, h, b in rows if h == hand]
            sq = [b["squal"] for b in hand_rows]
            changing = len(set(sq)) > 1
            print(f"{HANDS[hand]:5}: {delivered[hand]} bins ({delivered[hand] / elapsed:.1f}/s), lost-flag {lost[hand]}x,"
                  f" firmware read rate {st['reads_per_s']}/s, SQUAL changing: {changing}")
            print("       " + stats_line(HANDS[hand], st)[6:])
        if args.csv:
            with args.csv.open("w", newline="") as f:
                w = csv.writer(f)
                w.writerow(["t_s", "hand", "seq", "squal", "squal_min", "squal_max", "rawsum", "rawmax", "rawmin", "shutter", "moving", "lifted"])
                for t, hand, b in rows:
                    w.writerow([f"{t:.3f}", HANDS[hand], b["seq"], b["squal"], b["squal_min"], b["squal_max"], b["rawsum"],
                                b["rawmax"], b["rawmin"], b["shutter"], b["moving"], int(b["lifted"])])
            print(f"wrote {args.csv}")
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
