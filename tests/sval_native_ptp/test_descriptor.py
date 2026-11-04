"""Check the native touchpad's HID report descriptor and the reports ptp.c packs.

usage: test_descriptor.py <descriptor-hex-file> [<report-trace-file> ...]

1. Parses the descriptor (our parser in tools/multitouch/ptp_hid.py) and checks
   the usages, report sizes, ranges and the byte layout ptp.c packs.
2. If hid-tools is importable, parses it again with hid-tools as an
   independent cross-check of the report sizes.
3. Decodes every input report the C tests sent with the descriptor's own
   layout and checks the release rules across frames.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "keyboards", "svalboard", "tools", "multitouch"))
import ptp_hid  # noqa: E402


def check_descriptor(desc):
    failures, summary = ptp_hid.check(desc, gap_mm=10)
    for f in failures:
        print("FAIL descriptor:", f)
    assert not failures
    # The page bug this descriptor must not have: 06 FF 00 is page 0x00FF.
    assert bytes.fromhex("0600ff8503") in desc and bytes.fromhex("06ff00") not in desc
    print(f"  descriptor: {summary['length']} bytes, Touch Pad + Configuration, 5 fingers, "
          f"input 35 bytes, features 2/257/2/2/2, X 0..{summary['xmax']}, Y 0..{summary['ymax']}, "
          f"blob on page {summary['cert_page']}")
    # Negative checks: the checker notices a broken descriptor.
    bad = bytearray(desc)
    i = bad.index(bytes.fromhex("0600ff"))
    bad[i:i + 3] = bytes.fromhex("06ff00")
    assert any("0xFF00" in f or "0x00FF" in f for f in ptp_hid.check(bytes(bad))[0])
    bad = bytearray(desc)
    bad[bad.index(bytes.fromhex("2505")) + 1] = 0x0A  # contact count logical max
    assert ptp_hid.check(bytes(bad))[0]
    assert ptp_hid.check(desc, gap_mm=0)[0]  # the ranges really are checked


def cross_check_hidtools(desc):
    try:
        from hidtools.hid import ReportDescriptor
    except ImportError:
        print("  hid-tools: not installed, cross-check skipped")
        return
    rd = ReportDescriptor.from_bytes(list(desc))
    sizes = {("input", r.report_ID): r.size for r in rd.input_reports.values()}
    sizes.update({("feature", r.report_ID): r.size for r in rd.feature_reports.values()})
    want = {("input", 1): 35, ("feature", 2): 2, ("feature", 3): 257, ("feature", 4): 2, ("feature", 5): 2, ("feature", 6): 2}
    assert sizes == want, sizes
    usages = {f.usage for r in rd.input_reports.values() for f in r.fields if not f.is_const for _ in [0]}
    for u in (0x0D0047, 0x0D0042, 0x0D0051, 0x010030, 0x010031, 0x0D0056, 0x0D0054, 0x090001):
        assert u in usages, hex(u)
    cert = [f for f in rd.feature_reports[3].fields if not f.is_const]
    assert cert and all((f.usage >> 16) == 0xFF00 for f in cert), [hex(f.usage) for f in cert]
    print("  hid-tools: agrees on report sizes 35/2/257/2/2/2, usages, blob on 0xFF00")


def check_trace(desc, path):
    """Release rules across the frames each scenario sent."""
    by_scenario = {}
    with open(path) as fh:
        for line in fh:
            name, hexdata = line.split()
            by_scenario.setdefault(name, []).append(bytes.fromhex(hexdata))
    total = 0
    for name, reports in by_scenario.items():
        down = {}  # contact id -> (x, y) while Tip = 1
        last_scan = None
        for r in reports:
            total += 1
            d = ptp_hid.decode_input(desc, r)
            assert 0 <= d["count"] <= 5, name
            ids = [c["id"] for c in d["contacts"][:d["count"]]]
            assert len(set(ids)) == len(ids), (name, ids)
            for c in d["contacts"][d["count"]:]:
                assert not any(c.values()), (name, "unused slot not zero")
            if last_scan is not None:
                assert d["scan"] != last_scan, (name, "scan time repeated")
            last_scan = d["scan"]
            present = set()
            for c in d["contacts"][:d["count"]]:
                present.add(c["id"])
                if c["tip"]:
                    down[c["id"]] = (c["x"], c["y"])
                else:
                    # A lift is at the last reported position of a down contact.
                    assert c["id"] in down, (name, "lift of a contact never down", c)
                    assert down.pop(c["id"]) == (c["x"], c["y"]), (name, "lift moved", c)
            # A down contact never silently disappears: it is lifted first.
            vanished = set(down) - present
            assert not vanished, (name, "contact vanished without a lift", vanished)
    print(f"  trace: {total} reports from {len(by_scenario)} scenarios decode with the "
          "descriptor; every contact lifts once at its last position")


def main():
    desc = bytes.fromhex(open(sys.argv[1]).read().strip())
    print("native PTP descriptor tests:")
    check_descriptor(desc)
    cross_check_hidtools(desc)
    for path in sys.argv[2:]:
        check_trace(desc, path)
    print("native PTP descriptor: all passed")


if __name__ == "__main__":
    main()
