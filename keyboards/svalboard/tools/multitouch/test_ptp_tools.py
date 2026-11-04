"""Offline tests for the native touchpad dev tools (no board, no HID)."""
import struct
import unittest

import contacts
import perf
import ptp_hid
from ptp_owner import describe


def owner_reply(status=0, owner=3, mode=3, switches=3, bits=0x65, frames=12, busy=1, scan=4321, pads=3):
    r = bytearray(64)
    r[0] = 0xDD
    r[5], r[6], r[7], r[8], r[9] = 0xFC, 0xC2, status, owner, 3
    s = bytes((1, mode, 0, switches, bits, 2)) + struct.pack("<IIH", frames, busy, scan)
    s += bytes((pads, 0)) + struct.pack("<HHH", 0, 4, 0)
    r[10:34] = s
    r[34:53] = struct.pack("<IIIHHH", 1000, 2, 1, 3, 3, 0) + bytes((5,))
    return bytes(r)


class OwnerReply(unittest.TestCase):
    def test_describe(self):
        text = describe(owner_reply())
        self.assertIn("owner: native", text)
        self.assertIn("input mode: 3 (touchpad)", text)
        self.assertIn("reporting", text)
        self.assertIn("frames sent: 12", text)
        self.assertIn("pads: left, right", text)
        self.assertIn("frames held for the other pad: 4", text)
        self.assertIn("1000 reads", text)
        self.assertIn("hints on", text)

    def test_refused(self):
        self.assertIn("refused, status 2", describe(owner_reply(status=2)))

    def test_one_pad(self):
        self.assertIn("pads: right;", describe(owner_reply(pads=2)))

    def test_lost_fingers_status_v3(self):
        r = bytearray(owner_reply())
        r[9] = 7
        r[34 + 19], r[34 + 20] = 4, 0
        self.assertIn("fingers lifted because their pad's samples stopped: left 4, right 0", describe(bytes(r)))
        self.assertNotIn("fingers lifted", describe(owner_reply()))


def contacts_payload(flags, fingers):
    """A 0x2F payload as sval_ptp_contacts packs it (azoteq/ptp.c)."""
    p = bytearray(28)
    p[0], p[1], p[3] = 1, flags, len(fingers)
    for i, (cid, right, tip, x, y, *extra) in enumerate(fingers):
        p[4 + 4 * i] = cid | (0x10 if tip else 0) | 0x20 | (0x40 if right else 0) | (0x80 if extra else 0)
        p[5 + 4 * i:8 + 4 * i] = (x | y << 12).to_bytes(3, "little")
    return bytes(p)


class Contacts(unittest.TestCase):
    def test_six(self):
        fingers = [(i, i >= 3, True, 100 * i, 1791 - i) for i in range(5)] + [(5, True, True, 500, 1786, 1)]
        c = contacts.parse(contacts_payload(7, fingers))
        self.assertTrue(c["reporting"] and c["pads"]["left"] and c["pads"]["right"])
        self.assertEqual([f["id"] for f in c["left"]], [0, 1, 2])
        self.assertEqual([(f["x"], f["y"]) for f in c["right"]], [(300, 1788), (400, 1787), (500, 1786)])
        self.assertEqual(contacts.describe(c), "6 down: left 3 (IDs 0,1,2), right 3 (IDs 3,4,5*); 1 beyond the touchpad's 5 (*)")

    def test_lifting_and_none(self):
        c = contacts.parse(contacts_payload(7, [(4, False, False, 1, 1), (5, True, True, 2, 2)]))
        self.assertEqual(contacts.describe(c), "1 down: left 0, right 1 (IDs 5)")
        self.assertIsNone(contacts.parse(bytes(28)))


class PerfReport(unittest.TestCase):
    def half(self, **kw):
        s = {k: 0 for page in perf.PAGES for k in page}
        s.update(ms=30000, loops=12000, loop_sum=30000000, scans=12000, scan_sum=30000000, i2c_n=3700, i2c_sum=3700 * 1600, pads=3 | 1 << 8, prewait=90, postwait=90)
        s.update(kw)
        s["native"] = bool(s["pads"] >> 8)
        return s

    def test_report(self):
        text = perf.report("USB half", self.half())
        self.assertIn("matrix scans      400/s", text)
        self.assertIn("pad read        123.3/s", text)
        self.assertIn("(contact read)", text)
        self.assertIn("unpaced", text)

    def test_baseline_build(self):
        text = perf.report("USB half", self.half(pads=0))
        self.assertIn("read + end of session", text)
        self.assertNotIn("touchpad", text)

    def test_frames_since_power_up_are_a_total(self):
        # The frame counter is never reset: a short window must not turn it into a rate.
        text = perf.report("USB half", self.half(ms=200, frames=6345), frames_total=True)
        self.assertIn("6345 frames since power-up", text)
        self.assertNotIn("frames/s", text)
        self.assertIn("frames/s", perf.report("USB half", self.half(frames=3000)))

    def test_slowest_gap(self):
        self.assertIn("slowest gap   none yet", perf.report("USB half", self.half()))
        parts = dict(gap_0=400, gap_1=900, gap_2=20500, gap_3=1650, gap_4=0, gap_5=30, gap_6=0)
        text = perf.report("USB half", self.half(gap=23480, gap_at_ms=12300, gap_passes=3, gap_pass_max=21900, gap_reads=1, **parts))
        self.assertIn("slowest gap   23.48 ms at 12.3 s: split sync 20.50, pad read 1.65, matrix 0.90, other 0.40 ms;", text)
        self.assertNotIn("USB", text.splitlines()[-1])  # under 50 us: left out
        self.assertIn("3 loop passes (longest 21.90 ms), 1 pad reads, 0 split fetches", text)
        self.assertEqual(len(perf.PAGE5), 13)

    def test_long_window_refused(self):
        self.assertIn("over 70 minutes", perf.report("USB half", self.half(ms=71 * 60 * 1000)))


class Parser(unittest.TestCase):
    def test_geometry(self):
        g = ptp_hid.geometry(10)
        self.assertEqual((g["gap"], g["xmax"], g["ymax"], g["pxmax"], g["pymax"]), (476, 32003, 12543, 6720, 2800))
        self.assertEqual((g["xoff"], g["yoff"]), (13716, 5376))
        g = ptp_hid.geometry(10, times=1)
        self.assertEqual((g["xmax"], g["ymax"], g["pxmax"], g["xoff"]), (4571, 1791, 960, 0))
        self.assertEqual(ptp_hid.geometry(0, times=1)["xmax"], 4095)

    def test_minimal_descriptor(self):
        # A 1-byte input report: 2 buttons and 6 padding bits.
        desc = bytes.fromhex("05010902a10185010509190129021500250175019502810295068103c0")
        d = ptp_hid.parse(desc)
        self.assertEqual(d.report_bytes("input", 1), 2)
        f, i = d.find("input", 0x090002, 1)[0]
        self.assertEqual(ptp_hid.read_field(bytes((1, 0b10)), f, i), 1)

    def test_rejects_unclosed(self):
        with self.assertRaises(ValueError):
            ptp_hid.parse(bytes.fromhex("05010902a101"))


if __name__ == "__main__":
    unittest.main()
