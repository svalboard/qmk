"""A small HID report-descriptor parser and the native touchpad's checks.

No dependencies. Used by ptp_desc_check.py (descriptor pulled from a built
ELF) and by tests/sval_native_ptp (descriptor from ptp_descriptor.h, and
input reports packed by ptp.c).
"""
from dataclasses import dataclass, field

MAIN = {0x8: "input", 0x9: "output", 0xB: "feature", 0xA: "collection", 0xC: "end"}


@dataclass
class Field:
    kind: str  # input / output / feature
    report_id: int
    bit: int  # offset in the report, after the report ID byte
    size: int
    count: int
    usages: list  # (page << 16 | usage) per element, or [] for constants
    constant: bool
    logical: tuple
    physical: tuple
    unit: int
    exponent: int
    path: tuple  # enclosing collections: (type, page << 16 | usage)


@dataclass
class Descriptor:
    fields: list = field(default_factory=list)
    collections: list = field(default_factory=list)  # (depth, type, usage, path)

    def report_bits(self, kind, report_id):
        ends = [f.bit + f.size * f.count for f in self.fields if f.kind == kind and f.report_id == report_id]
        return max(ends, default=0)

    def report_bytes(self, kind, report_id):
        bits = self.report_bits(kind, report_id)
        return (bits + 7) // 8 + (1 if report_id else 0)

    def report_ids(self, kind):
        return sorted({f.report_id for f in self.fields if f.kind == kind})

    def find(self, kind, usage, report_id=None):
        """Every (field, element index) carrying this full usage."""
        out = []
        for f in self.fields:
            if f.kind != kind or (report_id is not None and f.report_id != report_id):
                continue
            for i, u in enumerate(f.usages):
                if u == usage:
                    out.append((f, i))
        return out


def _signed(data):
    if not data:
        return 0
    v = int.from_bytes(data, "little")
    bits = 8 * len(data)
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def parse(desc):
    """Parse short items. Raises ValueError on anything malformed."""
    d = Descriptor()
    g = dict(page=0, lmin=0, lmax=0, pmin=0, pmax=0, exp=0, unit=0, size=0, count=0, rid=0)
    stack, local, path = [], [], []
    umin = umax = None
    offsets = {}
    i = 0
    while i < len(desc):
        b = desc[i]
        if b == 0xFE:
            raise ValueError("long items are not expected")
        n = (0, 1, 2, 4)[b & 3]
        typ, tag = (b >> 2) & 3, b >> 4
        data = bytes(desc[i + 1:i + 1 + n])
        if len(data) != n:
            raise ValueError(f"truncated item at {i}")
        u = int.from_bytes(data, "little")
        i += 1 + n

        def full(x):
            return x if n == 4 else (g["page"] << 16) | x

        if typ == 1:  # global
            if tag == 0x0:
                g["page"] = u
            elif tag == 0x1:
                g["lmin"] = _signed(data)
            elif tag == 0x2:
                g["lmax"] = _signed(data) if g["lmin"] < 0 else u
            elif tag == 0x3:
                g["pmin"] = _signed(data)
            elif tag == 0x4:
                g["pmax"] = _signed(data) if g["pmin"] < 0 else u
            elif tag == 0x5:
                g["exp"] = (u & 0xF) - 16 if u & 0x8 else u & 0xF  # 4-bit signed
            elif tag == 0x6:
                g["unit"] = u
            elif tag == 0x7:
                g["size"] = u
            elif tag == 0x8:
                if not 1 <= u <= 255:
                    raise ValueError("bad report ID")
                g["rid"] = u
            elif tag == 0x9:
                g["count"] = u
            elif tag == 0xA:
                stack.append(dict(g))
            elif tag == 0xB:
                g = stack.pop()
            else:
                raise ValueError(f"unknown global tag {tag:#x}")
        elif typ == 2:  # local
            if tag == 0x0:
                local.append(full(u))
            elif tag == 0x1:
                umin = full(u)
            elif tag == 0x2:
                umax = full(u)
            # designators and strings are ignored
        elif typ == 0:  # main
            kind = MAIN.get(tag)
            if kind is None:
                raise ValueError(f"unknown main tag {tag:#x}")
            if kind == "collection":
                usage = local[0] if local else 0
                path.append((u, usage))
                d.collections.append((len(path), u, usage, tuple(path)))
            elif kind == "end":
                if not path:
                    raise ValueError("end collection without a collection")
                path.pop()
            else:
                usages = list(local)
                if umin is not None and umax is not None:
                    usages += list(range(umin, umax + 1))
                constant = bool(u & 1)
                if not constant:
                    if not usages:
                        raise ValueError(f"data field without usage at {i}")
                    usages = (usages + [usages[-1]] * g["count"])[:g["count"]]
                else:
                    usages = []
                key = (kind, g["rid"])
                bit = offsets.get(key, 0)
                d.fields.append(Field(kind, g["rid"], bit, g["size"], g["count"], usages, constant, (g["lmin"], g["lmax"]), (g["pmin"], g["pmax"]), g["unit"], g["exp"], tuple(path)))
                offsets[key] = bit + g["size"] * g["count"]
            local, umin, umax = [], None, None
        else:
            raise ValueError("reserved item type")
    if path:
        raise ValueError("unclosed collection")
    return d


def read_field(report, f, index):
    """Value of element `index` of field f in a raw report (report ID byte first)."""
    start = 8 * (1 if f.report_id else 0) + f.bit + f.size * index
    v = int.from_bytes(bytes(report), "little") >> start
    v &= (1 << f.size) - 1
    if f.logical[0] < 0 and v & (1 << (f.size - 1)):
        v -= 1 << f.size
    return v


DIG, GD, BTN, VENDOR = 0x0D, 0x01, 0x09, 0xFF00


def U(page, usage):
    return page << 16 | usage


def geometry(gap_mm, times=7):
    """The pads' layout, and the surface the OS sees: `times` the pads' area on
    each axis (ptp_descriptor.h, SVAL_PTP_SURFACE_TIMES), the pads in its center."""
    g = (gap_mm * 2048 * 2 + 43) // (2 * 43)
    pxmax, pymax = 860 + 10 * gap_mm, 400
    return dict(gap=g, stride=2048 + g, xmax=times * (4096 + g) - 1, ymax=times * 1792 - 1, pxmax=times * pxmax, pymax=times * pymax, xoff=(times - 1) // 2 * (4096 + g), yoff=(times - 1) // 2 * 1792)


def check(desc, gap_mm=10):
    """Return (failures, summary) for the native touchpad descriptor."""
    fail = []
    geo = geometry(gap_mm)
    try:
        d = parse(desc)
    except Exception as exc:  # noqa: BLE001 - reported as a failure
        return [f"parse: {exc}"], {}

    def need(cond, msg):
        if not cond:
            fail.append(msg)

    tops = [c for c in d.collections if c[0] == 1]
    need([(c[1], c[2]) for c in tops] == [(1, U(DIG, 0x05)), (1, U(DIG, 0x0E))], f"top-level collections: {[(c[1], hex(c[2])) for c in tops]}")
    sizes = {("input", 1): 35, ("feature", 2): 2, ("feature", 3): 257, ("feature", 4): 2, ("feature", 5): 2, ("feature", 6): 2}
    for (kind, rid), want in sizes.items():
        got = d.report_bytes(kind, rid)
        need(got == want, f"{kind} report {rid}: {got} bytes, want {want}")
    need(d.report_ids("input") == [1], f"input report IDs {d.report_ids('input')}")
    need(d.report_ids("output") == [], "unexpected output reports")
    need(d.report_ids("feature") == [2, 3, 4, 5, 6], f"feature report IDs {d.report_ids('feature')}")

    fingers = [c for c in d.collections if c[1] == 2 and c[2] == U(DIG, 0x22) and c[3][0][1] == U(DIG, 0x05)]
    need(len(fingers) == 5, f"{len(fingers)} finger collections, want 5")

    def one(kind, usage, rid, count=None):
        hits = d.find(kind, usage, rid)
        if count is not None:
            need(len(hits) == count, f"{hex(usage)} in {kind} {rid}: {len(hits)}, want {count}")
        return hits

    layout = {}
    for name, usage, bits in (("confidence", U(DIG, 0x47), 1), ("tip", U(DIG, 0x42), 1), ("contact_id", U(DIG, 0x51), 8), ("x", U(GD, 0x30), 16), ("y", U(GD, 0x31), 16)):
        hits = one("input", usage, 1, 5)
        layout[name] = hits
        for f, idx in hits:
            need(f.size == bits, f"{name}: {f.size} bits, want {bits}")
    for f, _ in layout.get("contact_id", []):
        need(f.logical == (0, 15), f"contact id logical {f.logical}")
    for f, _ in layout.get("x", []):
        need(f.logical == (0, geo["xmax"]), f"X logical {f.logical}, want 0..{geo['xmax']}")
        need(f.physical == (0, geo["pxmax"]), f"X physical {f.physical}, want 0..{geo['pxmax']}")
        need(f.unit == 0x11 and f.exponent == -2, f"X unit {f.unit:#x} exponent {f.exponent}")
    for f, _ in layout.get("y", []):
        need(f.logical == (0, geo["ymax"]), f"Y logical {f.logical}")
        need(f.physical == (0, geo["pymax"]), f"Y physical {f.physical}")
        need(f.unit == 0x11 and f.exponent == -2, f"Y unit {f.unit:#x} exponent {f.exponent}")
    scan = one("input", U(DIG, 0x56), 1, 1)
    for f, _ in scan:
        need(f.size == 16 and f.unit == 0x1001 and f.exponent == -4, f"scan time {f.size} bits unit {f.unit:#x} exp {f.exponent}")
    count = one("input", U(DIG, 0x54), 1, 1)
    for f, _ in count:
        need(f.size == 8 and f.logical == (0, 5), f"contact count {f.size} bits logical {f.logical}")
    buttons = [one("input", U(BTN, b), 1, 1) for b in (1, 2, 3)]

    caps_max = one("feature", U(DIG, 0x55), 2, 1)
    caps_type = one("feature", U(DIG, 0x59), 2, 1)
    cert = one("feature", U(VENDOR, 0xC5), 3)
    need(len(cert) == 256, f"certification blob {len(cert)} bytes on page 0xFF00, want 256")
    wrong_page = d.find("feature", U(0x00FF, 0xC5), 3)
    need(not wrong_page, "certification blob on page 0x00FF (should be 0xFF00)")
    one("feature", U(DIG, 0x60), 4, 1)
    mode = one("feature", U(DIG, 0x52), 5, 1)
    for f, _ in mode:
        need(f.size == 8 and f.path[0][1] == U(DIG, 0x0E), "input mode not an 8-bit field in Configuration")
    one("feature", U(DIG, 0x57), 6, 1)
    one("feature", U(DIG, 0x58), 6, 1)

    # The byte layout ptp.c packs (DESIGN.md, input report table).
    def at(hits, i):
        f, idx = hits[i]
        return 8 + f.bit + f.size * idx  # bit offset in the report incl. the ID byte

    for i in range(5):
        try:
            need(at(layout["confidence"], i) == 8 * (1 + 6 * i), f"finger {i} confidence offset")
            need(at(layout["tip"], i) == 8 * (1 + 6 * i) + 1, f"finger {i} tip offset")
            need(at(layout["contact_id"], i) == 8 * (2 + 6 * i), f"finger {i} id offset")
            need(at(layout["x"], i) == 8 * (3 + 6 * i), f"finger {i} X offset")
            need(at(layout["y"], i) == 8 * (5 + 6 * i), f"finger {i} Y offset")
        except (IndexError, KeyError):
            break
    if scan:
        need(at(scan, 0) == 8 * 31, "scan time offset")
    if count:
        need(at(count, 0) == 8 * 33, "contact count offset")
    for b, hits in enumerate(buttons):
        if hits:
            need(at(hits, 0) == 8 * 34 + b, f"button {b + 1} offset")
    if caps_max and caps_type:
        need(at(caps_max, 0) == 8 and at(caps_type, 0) == 12, "caps nibbles")

    summary = dict(length=len(desc), input_bytes=d.report_bytes("input", 1), xmax=geo["xmax"], ymax=geo["ymax"], fingers=len(fingers), cert_page=hex(cert[0][0].usages[0] >> 16) if cert else None)
    return fail, summary


def decode_input(desc, report):
    """Decode one input report with the descriptor's own layout."""
    d = parse(desc)
    if report[0] != 1 or len(report) != d.report_bytes("input", 1):
        raise ValueError("not an input report 1")

    def val(usage, i=0):
        f, idx = d.find("input", usage, 1)[i]
        return read_field(report, f, idx)

    contacts = []
    for i in range(5):
        contacts.append(dict(confidence=val(U(DIG, 0x47), i), tip=val(U(DIG, 0x42), i), id=val(U(DIG, 0x51), i), x=val(U(GD, 0x30), i), y=val(U(GD, 0x31), i)))
    return dict(contacts=contacts, scan=val(U(DIG, 0x56)), count=val(U(DIG, 0x54)), buttons=sum(val(U(BTN, b)) << (b - 1) for b in (1, 2, 3)))
