#!/usr/bin/env python3
"""End-to-end host test for keyboards/svalboard/tools/sval_update.py.

    test_sval_update_tool.py LIB.so OUT_DIR

Drives the tool's update session against the firmware's own state machine
(updater.c, the chord, the slot flash and the image checks), built into LIB.so
by util/updater_test/run.sh with the mock die and a simulated clock from
test_updater.c. A fake HID device stands in for the board: it emulates the Sval
client wrapper and hands each wrapped VIA packet to updater_via_command().
"""
import contextlib
import ctypes
import io
import importlib.util
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("sval_update", TOOLS / "sval_update.py")
su = importlib.util.module_from_spec(spec)
spec.loader.exec_module(su)
mu = su.make_update

checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


lib = ctypes.CDLL(sys.argv[1])
lib.host_lib_reset.argtypes = [ctypes.c_int]
lib.host_via.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
lib.host_passes.argtypes = [ctypes.c_int]
lib.host_state.restype = ctypes.c_int
lib.host_slot.restype = ctypes.POINTER(ctypes.c_uint8)
lib.host_commit_runs.restype = ctypes.c_int
lib.host_commit_crc.restype = ctypes.c_uint32
lib.host_lib_split.argtypes = [ctypes.c_int]
lib.host_slave_commits.restype = ctypes.c_int
lib.host_slave_commit_crc.restype = ctypes.c_uint32
lib.host_master_reboots.restype = ctypes.c_int
lib.host_slave_fw.restype = ctypes.c_uint32
lib.host_master_fw.restype = ctypes.c_uint32
lib.host_slave_state.restype = ctypes.c_int
lib.host_link_paused.restype = ctypes.c_int
lib.host_chord_done.restype = ctypes.c_int
for _f in ("host_master_invalidations", "host_slave_invalidations", "host_master_soft_resets", "host_slave_soft_resets"):
    getattr(lib, _f).restype = ctypes.c_int

BOOT2 = bytes(int(h, 16) for h in __import__("re").findall(r"0x([0-9a-f]{2})", (Path(__file__).parent / "boot2_page0.h").read_text().split("#define BOOT2_PAGE0_BYTES")[1]))


def image(length):
    body = bytearray(BOOT2 + struct.pack("<II", 0x20041000, 0x100001F7))
    body += bytes((i * 29 + 3) & 0xFF for i in range(length - len(body)))
    return bytes(body)


class FakeBoard(su.Device):
    """The board behind HID: the client wrapper, then the firmware's updater.
    The user makes the chord once the tool waits for it (sleeps between polls)
    in CONFIRM_WAIT."""

    current = None
    storage = 0  # Sval GET_INFO byte 18, across reopens
    acked_resets = 0

    def __init__(self, allow_unwrapped=False):
        self.handle = None
        self.serial = "sval:fake"
        self.client = None
        self.round_trips = 0
        self.round_trip_s = 0.0
        self.issued = []
        self.waited = False
        self.chords = 0
        self.allow_unwrapped = allow_unwrapped
        self.ops = []  # (op, request after [hand]) of each wrapped updater packet
        self.sval_cmds = []
        FakeBoard.current = self

    def _exchange(self, packet, match):
        check(len(packet) <= su.REPORT, f"a packet of {len(packet)} bytes")
        packet = bytes(packet).ljust(su.REPORT, b"\0")
        lib.host_passes(2)  # the main loop runs between packets
        if packet[0] != su.WRAPPER:
            if not self.allow_unwrapped:
                raise AssertionError("the tool sent an unwrapped packet")
            via = ctypes.create_string_buffer(packet, su.REPORT)
            lib.host_via(via, 0)  # plain VIA: no client ID
            reply = via.raw[:su.REPORT]
            if not match(reply):
                raise AssertionError(f"reply does not match: {reply.hex()}")
            self.round_trips += 1
            return reply
        cid = packet[1:5]
        if cid == bytes(4):
            new = struct.pack("<I", 0x00510000 + len(self.issued) + 1)
            self.issued.append(new)
            reply = (packet[:25] + new + struct.pack("<H", 120)).ljust(su.REPORT, b"\0")
        elif cid not in self.issued:
            reply = (bytes([su.WRAPPER]) + cid + bytes([0xFF, 1])).ljust(su.REPORT, b"\0")
        elif packet[5] == su.SVAL_PREFIX:
            # Sval in the wrapper: GET_INFO's storage byte and 0x2A only. Bit 2
            # is set by a reset of this half after SETTINGS_RESET (D42).
            cmd = packet[6]
            body = bytearray(packet[5:])
            if cmd == su.SVAL_GET_INFO:
                if lib.host_master_soft_resets() > FakeBoard.acked_resets:
                    FakeBoard.storage |= 4
                body[18] = FakeBoard.storage
            elif cmd == su.SVAL_STORAGE_RESET_CLEAR:
                FakeBoard.storage &= ~4
                FakeBoard.acked_resets = lib.host_master_soft_resets()
            self.sval_cmds.append(cmd)
            reply = (packet[:5] + bytes(body))[:su.REPORT]
        elif packet[6] in (su.VIA_GET_LAYER_COUNT, su.VIA_KEYMAP_GET_BUFFER):
            # The dynamic keymap (VIA), for reset-settings --blank: KC_NO everywhere
            # once this half took a blank SETTINGS_RESET (D44), KC_A everywhere before.
            body = bytearray(packet[6:])
            if packet[6] == su.VIA_GET_LAYER_COUNT:
                body[1] = 16
            else:
                n = body[3]
                key = b"\0\0" if lib.host_master_reset_mode() == 1 else b"\0\x04"
                body[4:4 + n] = (key * n)[:n]
            reply = (packet[:6] + bytes(body))[:su.REPORT]
        else:
            # As client_wrapper.c: VIA gets the report shifted down 6 bytes (the
            # last 6 stale), and the reply loses its last 6 to the header.
            check(packet[5] == su.VIA_PROTO, "VIA inside the wrapper")
            self.ops.append((packet[8], packet[10:]))
            via = ctypes.create_string_buffer(packet[6:] + b"\xEE" * 6, su.REPORT)
            lib.host_via(via, struct.unpack("<I", cid)[0])
            check(via.raw[su.REPORT - 6:su.REPORT] == b"\xEE" * 6, "the updater wrote into the wrapper's 6 bytes")
            reply = bytes([su.WRAPPER]) + cid + bytes([su.VIA_PROTO]) + via.raw[:su.REPORT - 6]
        if lib.host_state() == 2 and self.waited and not lib.host_chord_done():  # CONFIRM_WAIT, no chord yet, the tool waits: the user makes it
            self.waited = False
            self.chords += 1
            lib.host_chord()
        if not match(reply):
            raise AssertionError(f"reply does not match: {reply.hex()}")
        self.round_trips += 1
        return reply


class Proto3Board(FakeBoard):
    """The same state machine, reporting updater protocol 3 in this half's INFO: the
    tool must then take protocol 3's path (one chord per half, no PAIR_MANIFEST)."""
    def op(self, op, args=b"", hand=su.HAND_SELF, client=None):
        st, r = super().op(op, args, hand, client)
        if op == su.INFO and hand in (su.HAND_SELF, 1) and r[0] != 0xFF and not (args and args[0] == 1):
            r = bytes([r[0], 3]) + r[2:]
        return st, r


class Args:
    delay = 0.0
    rebind_every = 0.0
    min_rebinds = 0
    no_commit = False
    keep = False
    yes = False
    half = None


def args_with(**kw):
    a = Args()
    for k, v in kw.items():
        setattr(a, k, v)
    return a


class Answers:
    """Stands in for the y/N question: each call takes the next answer (EOFError for None)."""
    def __init__(self, *answers):
        self.answers = list(answers)
        self.asked = 0

    def __call__(self, prompt):
        self.asked += 1
        check("[y/N]" in prompt, f"the question: {prompt!r}")
        a = self.answers.pop(0) if self.answers else None
        if a is None:
            raise EOFError
        return a


def no_question(prompt):
    raise AssertionError(f"asked {prompt!r} with nothing to ask about")


def run_update(svup, commit_available, rebind_every=0.0, min_rebinds=0):
    lib.host_lib_reset(1 if commit_available else 0)
    board = FakeBoard()
    args = Args()
    args.rebind_every = rebind_every
    args.min_rebinds = min_rebinds
    session = su.Session(board, args)
    rc = session.run(svup)
    return rc, board, session


slept = [0.0]
real_monotonic = __import__("time").monotonic


def fake_monotonic():
    """The tool's deadlines run on the simulated time its sleeps add."""
    return real_monotonic() + slept[0]


def fake_sleep(s):
    """Simulated time while the tool polls; the fake user acts on the next poll."""
    slept[0] += s
    lib.host_passes(max(int(s * 1000), 1))
    if FakeBoard.current:  # the tool waits in CONFIRM_WAIT for a chord not made yet
        FakeBoard.current.waited = lib.host_state() == 2 and not lib.host_chord_done()


def main():
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    su.time.sleep = fake_sleep
    su.time.monotonic = fake_monotonic
    su.ask = no_question  # a matching image is never asked about

    img = image(0x5100)
    manifest = mu.build_manifest(img, hand=1, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=7, version="tool-test")
    svup = out / "tool_test.svup"
    svup.write_bytes(manifest + img)
    # A .svup is the manifest, then the image; the tool reads it as (manifest, image).
    check(mu.parse_svup(svup.read_bytes()) == (manifest, img), "parse_svup: (manifest, image)")
    v1 = out / "tool_test_v1.svup"
    v1m = bytearray(manifest)
    v1m[4:6] = (1).to_bytes(2, "little")
    v1.write_bytes(bytes(v1m) + bytes(64) + img)
    lib.host_lib_reset(0)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Session(FakeBoard(), Args()).run(v1)
        check(False, "a version-1 .svup was sent")
    except mu.Refused as exc:
        check("version-1" in str(exc), f"a version-1 .svup: {exc}")
    check(lib.host_state() == 0, "a version-1 .svup: nothing sent")

    # A pair UF2, as releases ship (docs/pair-uf2.md): the tool takes the manifest
    # and the image from its record, as Keybard's loadFirmwareFile does.
    either = mu.build_manifest(img, hand=2, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=7, version="tool-test")
    pair_uf2 = out / "tool_test_pair.uf2"
    pair_uf2.write_bytes(mu.pair_uf2_bytes(either, img))
    check(su.load_firmware(pair_uf2) == (either, img), "a pair .uf2: (manifest, image) from its record")
    rc, board, _ = run_update(pair_uf2, commit_available=False)
    check(rc == 3 and bytes(lib.host_slot()[:len(img)]) == img, f"a pair .uf2 installs: exit {rc}")

    def refused(path, kind, what):
        lib.host_lib_reset(0)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                su.Session(FakeBoard(), Args()).run(path)
            check(False, f"{what} was sent")
        except su.FirmwareFileError as exc:
            check(exc.kind == kind, f"{what}: refused as {exc.kind}, expected {kind}")
            check(str(exc) == su.FILE_MESSAGES[kind], f"{what}: message {exc}")
        check(lib.host_state() == 0, f"{what}: nothing sent")

    # A plain .uf2 (one half's file, no pair record): Keybard's refusal and words.
    padded = img + b"\xff" * (-len(img) % 256)
    pages = [padded[i:i + 256] for i in range(0, len(padded), 256)]
    plain = out / "tool_test_plain.uf2"
    plain.write_bytes(b"".join(mu.uf2_block(mu.XIP_BASE + i * 256, pg, i, len(pages)) for i, pg in enumerate(pages)))
    refused(plain, "no-pair", "a plain .uf2")
    check("drag the file onto the RPI-RP2 drive" in su.FILE_MESSAGES["no-pair"], "the plain .uf2 message says how to install by hand")
    # Damaged pair UF2s: a record byte changed (CRC), an image byte changed (SHA-512 in the
    # manifest is checked by the board, so here the layout: a block cut short), a block renumbered.
    data = bytearray(pair_uf2.read_bytes())
    rec = len(data) - 16 * 512 + 32
    bad_crc = bytearray(data)
    bad_crc[rec + 20] ^= 0x01
    damaged = out / "tool_test_damaged.uf2"
    damaged.write_bytes(bytes(bad_crc))
    refused(damaged, "damaged", "a pair .uf2 with a changed record")
    damaged.write_bytes(bytes(data[:-100]))
    refused(damaged, "damaged", "a cut-short .uf2")
    renum = bytearray(data)
    renum[512 + 20] ^= 0x01
    damaged.write_bytes(bytes(renum))
    refused(damaged, "damaged", "a .uf2 with a renumbered block")

    # the stub refuses COMMIT: exit 3, image staged and verified
    rc, board, _ = run_update(svup, commit_available=False)
    check(rc == 3, f"stub commit: exit {rc}")
    check(lib.host_state() == 7, f"VERIFIED after the stub refused, state {lib.host_state()}")
    slot = bytes(lib.host_slot()[:len(img)])
    check(slot == img, "slot holds the image")
    check(lib.host_commit_runs() == 0, "no commit ran")

    # protocol 3: the pieces are the sizes INFO gives, the largest that fit a 64-byte report
    check(su.REPORT == 64 and su.CHUNK_PIECE_MAX == 50 and su.MANIFEST_PIECE_MAX == 52, "protocol 3 sizes")
    manifests = [r for op, r in board.ops if op == su.MANIFEST]
    chunks = [r for op, r in board.ops if op == su.CHUNK]
    check([m[1] for m in manifests] == [52, 52, 4], f"MANIFEST pieces {[m[1] for m in manifests]}")
    check(max(c[3] for c in chunks) == 50 and len(chunks) == -(-len(img) // 50), f"{len(chunks)} CHUNKs")

    # with a commit routine: COMMIT accepted, and it runs 100 ms later with the image CRC
    rc, board, _ = run_update(svup, commit_available=True)
    check(rc == 0, f"commit: exit {rc}")
    lib.host_passes(150)
    check(lib.host_commit_runs() == 1, "the commit ran once")
    check(lib.host_commit_crc() & 0xFFFFFFFF == mu.crc32_mpeg2(img[0x100:]), "commit got the image CRC over [0x100, len)")

    # client ID renewal with REBIND on every op, and the old ID refused each time
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc, board, session = run_update(svup, commit_available=False, rebind_every=1e-9)
    check(log.getvalue().count("old ID refused (OTHER_CLIENT)") == len(board.issued) - 1, "every renewal checked the old ID")
    check(rc == 3, f"rebind run: exit {rc}")
    check(len(board.issued) > 100, f"renewed {len(board.issued)} times")
    check(session.renewals == len(board.issued) - 1, f"renewals counted: {session.renewals}")
    check(bytes(lib.host_slot()[:len(img)]) == img, "slot holds the image after renewals")
    # M1 #16: --min-rebinds fails a run with too few renewals, before COMMIT
    with contextlib.redirect_stdout(io.StringIO()):
        rc, _, session = run_update(svup, commit_available=True, rebind_every=1e-9, min_rebinds=3)
    check(rc == 0 and session.renewals >= 3, f"enough renewals: exit {rc}, {session.renewals}")
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            run_update(svup, commit_available=True, rebind_every=0.0, min_rebinds=1)
        check(False, "--min-rebinds 1 with no renewals passed")
    except su.UpdaterError as exc:
        check("only 0 client ID renewals" in str(exc), f"min-rebinds: {exc}")
    check(lib.host_state() == 7 and lib.host_commit_runs() == 0, "no COMMIT after a --min-rebinds failure")

    # reject (M1 #6): the protocol rejection matrix against the firmware's state machine
    lib.host_lib_reset(1)
    board = FakeBoard(allow_unwrapped=True)
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Rejects(board, svup).run()
    text = log.getvalue()
    check(rc == 0, f"reject: exit {rc}\n{text}")
    check(text.count("  PASS  ") == 26 and "FAIL" not in text, f"reject: {text.count('  PASS  ')} passed\n{text}")
    check(board.chords >= 2, f"reject: chord made {board.chords} times")
    check(lib.host_state() == 0 and lib.host_commit_runs() == 0, "reject: IDLE afterwards, nothing committed")
    for what in (
        "unwrapped ARM: INVALID", "BEGIN before the chord: NOT_CONFIRMED", "CHUNK ahead of the next offset: OUT_OF_ORDER", "END too early: OUT_OF_ORDER", "REBIND with the wrong nonce: OTHER_CLIENT", "COMMIT with the wrong CRC: BAD_HASH",
        "ABORT from a second client: OTHER_CLIENT", "CHUNK past the end (overrun): OVERRUN, state ERROR"
    ):
        check(what in text, f"reject: no '{what}'")
    # reject's own check: the right status in the wrong state is a failure
    lib.host_lib_reset(0)
    rj = su.Rejects(FakeBoard(), svup)
    rj.hand = 1
    with contextlib.redirect_stdout(io.StringIO()):
        rj.expect("probe", (su.OK, bytes(22)), su.OK, su.ST_RECEIVING)
        rj.expect("probe", (su.OK, bytes(22)), su.INVALID)
        rj.expect("probe", (su.OK, bytes(22)), su.OK, su.ST_IDLE)
    check(rj.failed == 2 and rj.passed == 1, f"reject's check: {rj.passed} passed, {rj.failed} failed")
    # reject refuses to start on a busy board
    lib.host_lib_reset(0)
    board = FakeBoard(allow_unwrapped=True)
    board.op(su.MANIFEST, bytes([0, 20]) + bytes(20), hand=1)
    try:
        su.Rejects(board, svup).run()
        check(False, "reject ran on a busy board")
    except su.UpdaterError as exc:
        check("ABORT first" in str(exc), f"reject on a busy board: {exc}")

    # info and status decode against the firmware's layout
    lib.host_lib_reset(0)
    board = FakeBoard()
    info = su.decode_info(*board.op(su.INFO))
    check(info["hand"] == "right" and info["pointing"] == "pmw3389" and info["jedec"] == "ef4018", f"info {info}")
    check(info["slot_base"] == "0x800000" and info["slot_size"] == "0x160000" and info["max_image"] == "0x160000", f"info {info}")
    check(info["storage_format"] == 2 and info["flash_16mib"] and not info["settings_writes_failing"], f"info {info}")
    check(info["protocol"] == mu.UPDATER_PROTO == 4 and not info["release_build"], f"info {info}")
    check((info["chunk_max"], info["manifest_max"], info["report_bytes"]) == (50, 52, 64), f"info sizes {info}")
    check(su.check_protocol(info) == (52, 50), "piece sizes from INFO")
    # a board on another protocol, or with piece sizes that do not fit, is refused before any session op
    check(su.check_protocol({**info, "protocol": 3}) == (52, 50), "protocol 3 boards still taken")
    for change, want in ((dict(protocol=2), "protocol 2"), (dict(protocol=5), "protocol 5"), (dict(chunk_max=51), "do not fit"), (dict(chunk_max=0), "do not fit"), (dict(manifest_max=53), "do not fit"), (dict(report_bytes=32), "32-byte")):
        try:
            su.check_protocol({**info, **change})
            check(False, f"check_protocol took {change}")
        except su.UpdaterError as exc:
            check(want in str(exc), f"check_protocol {change}: {exc}")

    class OldBoard(FakeBoard):
        """INFO from protocol 2 firmware, as far as this tool could read it."""
        def op(self, op, args=b"", hand=su.HAND_SELF, client=None):
            st, r = super().op(op, args, hand, client)
            return (st, bytes([r[0], 2]) + r[2:]) if op == su.INFO else (st, r)

    lib.host_lib_reset(0)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Session(OldBoard(), Args()).run(svup)
        check(False, "a protocol 2 board was updated")
    except su.UpdaterError as exc:
        check("protocol 2" in str(exc) and "BOOTSEL" in str(exc), f"protocol 2 board: {exc}")
    check(lib.host_state() == 0, "protocol 2 board: nothing sent")
    lib.host_lib_reset(0)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Rejects(OldBoard(allow_unwrapped=True), svup).run()
        check(False, "reject ran on a protocol 2 board")
    except su.UpdaterError as exc:
        check("protocol 2" in str(exc), f"reject, protocol 2 board: {exc}")
    check(not info["other_half_mismatch"], f"info {info}")
    diag = su.decode_diag(*board.op(su.DIAG, b"\0", hand=1))
    check(diag["status"] == "OK" and diag["stack_size"] == 0xAE0 and diag["stack_unused"] == 0x2A4, f"diag {diag}")
    st = su.decode_status(*board.op(su.STATUS, hand=1))
    check(st["state"] == "IDLE" and st["status"] == "OK", f"status {st}")
    check(st["other_half"] == "none" and st["other_half_id"] == 0, f"status {st}")
    st, r = board.op(su.ABORT, bytes(4), hand=1)
    check(st == su.OK, "ABORT in IDLE")

    # an image for the other half with no other half connected: refused before any op
    bad = mu.build_manifest(img, hand=0, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=7, version="left")
    badf = out / "left.svup"
    badf.write_bytes(bad + img)
    lib.host_lib_reset(0)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.Session(FakeBoard(), Args()).run(badf)
        check(False, "an image for an absent other half accepted")
    except su.UpdaterError as exc:
        check("presence none" in str(exc), f"no other half: {exc}")
    check(lib.host_state() == 0, "no session without the other half")

    # ---- M2: the other half, through this one ----
    left_m = mu.build_manifest(img, hand=0, pointing_id=1, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=9, version="left-9")
    left = out / "left9.svup"
    left.write_bytes(left_m + img)
    right_m = mu.build_manifest(img, hand=1, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=9, version="right-9")
    right = out / "right9.svup"
    right.write_bytes(right_m + img)
    crc = mu.crc32_mpeg2(img[0x100:])

    # update with the other half's image: staged here, relayed, committed there
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    board = FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Session(board, Args()).run(left)
    text = log.getvalue()
    check(rc == 0, f"other half: exit {rc}\n{text}")
    check(lib.host_slave_commits() == 1, f"the other half committed {lib.host_slave_commits()} times")
    check(lib.host_slave_commit_crc() == crc, "the other half committed the image's CRC")
    check(lib.host_slave_fw() == 9 and lib.host_master_fw() == 2001, f"fw {lib.host_slave_fw()} / {lib.host_master_fw()}")
    check(lib.host_commit_runs() == 0 and lib.host_master_reboots() == 0, "this half did not commit")
    check(lib.host_state() == 0 and not lib.host_link_paused(), "IDLE, link running afterwards")
    for what in ("for the other (left) half", "relaying to the other half", "relayed:", "the other half took COMMIT", "the other half answers again: fw 9, presence version differs", "halves now run different releases"):
        check(what in text, f"other half: no '{what}' in\n{text}")
    check("the left half without USB" in text and "on the right half, the one with USB" in text, f"chord text\n{text}")
    rl = su.decode_relay(*board.op(su.RELAY, hand=0))
    check(rl["phase"] == "done" and rl["acked"] == len(img) and rl["image_len"] == len(img), f"relay {rl}")
    st = su.decode_status(*board.op(su.STATUS, hand=1))
    check(st["other_half"] == "version differs", f"status {st}")
    inf = board.op(su.INFO, hand=0)
    check(su.decode_info(*inf)["fw_version"] == 9 and su.decode_info(*inf)["hand"] == "left", f"info --other {inf}")

    # --no-commit stops at VERIFIED here, before any relay
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    a = Args()
    a.no_commit = True
    with contextlib.redirect_stdout(io.StringIO()):
        rc = su.Session(FakeBoard(), a).run(left)
    check(rc == 0 and lib.host_state() == 7 and lib.host_slave_state() == 0, "no-commit: VERIFIED here, nothing relayed")

    # Gentle (D33): another pointing device for the other half is a warning and a
    # question, never a refusal. No, or no answer (EOF): nothing is sent.
    wrong_m = mu.build_manifest(img, hand=0, pointing_id=3, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=9, version="left-3389")
    wrong = out / "left3389.svup"
    wrong.write_bytes(wrong_m + img)
    for answers in (("n",), ("",), (None,)):
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        su.ask = q = Answers(*answers)
        try:
            with contextlib.redirect_stdout(io.StringIO()) as log:
                su.Session(FakeBoard(), Args()).run(wrong)
            check(False, f"another pointing device sent without a yes ({answers})")
        except su.UpdaterError as exc:
            check("not confirmed" in str(exc) and q.asked == 1, f"no yes: {exc}")
        text = log.getvalue()
        check("warning: the image drives pointing pmw3389; the left half's build drives trackpoint" in text, f"warning\n{text}")
        check(lib.host_state() == 0 and lib.host_slave_state() == 0, "not confirmed: nothing sent")
    # yes at the question, or --yes: the other half takes it, verifies it and commits it
    for args, answers in ((Args(), ("y",)), (args_with(yes=True), ())):
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        su.ask = q = Answers(*answers) if answers else no_question
        with contextlib.redirect_stdout(io.StringIO()) as log:
            rc = su.Session(FakeBoard(), args).run(wrong)
        text = log.getvalue()
        check(rc == 0 and lib.host_slave_commits() == 1, f"another pointing device, confirmed: exit {rc}\n{text}")
        check(("--yes: going ahead" in text) == bool(args.yes), f"--yes noted\n{text}")
    su.ask = no_question

    # an image for the other hand, sent to the half with USB (--half usb, an MCU moved
    # between hands): warned, asked, then committed; the auto image is a pointing mismatch too
    auto_left_m = mu.build_manifest(img, hand=0, pointing_id=5, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=9, version="left-auto")
    auto_left = out / "left_auto.svup"
    auto_left.write_bytes(auto_left_m + img)
    lib.host_lib_reset(1)
    su.ask = q = Answers("y")
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.Session(FakeBoard(), args_with(half="usb")).run(auto_left)
    text = log.getvalue()
    check(rc == 0 and q.asked == 1, f"--half usb: exit {rc}, asked {q.asked}\n{text}")
    lib.host_passes(150)
    check(lib.host_commit_runs() == 1, "--half usb: this half committed the left image")
    check("warning: the image was built for the left hand; this half runs a right build" in text, f"hand\n{text}")
    check("the image drives pointing auto (any module); the right half's build drives pmw3389" in text, f"auto\n{text}")
    check("for the other" not in text, f"--half usb stays on this half\n{text}")
    su.ask = no_question

    # pair: the other half, then this one; the board comes back and presence matches
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    su.reopen = lambda serial, timeout: FakeBoard()
    board = FakeBoard()
    sessions = []
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.pair(board, Args(), [right, left], sessions)
    text = log.getvalue()
    check(rc == 0, f"pair: exit {rc}\n{text}")
    check(lib.host_slave_commits() == 1 and lib.host_master_reboots() == 1, "pair: each half committed once")
    check(lib.host_slave_fw() == 9 and lib.host_master_fw() == 9, f"pair: fw {lib.host_slave_fw()} / {lib.host_master_fw()}")
    check(text.index("1/2: the other (left) half") < text.index("2/2: this (right) half"), "pair: the other half first")
    check("pair: presence match" in text, f"pair:\n{text}")
    # INFO and presence both report the build's fw_version (the sim's reboot sets it from the manifest)
    check("this half fw 9, the other half fw 9" in text and "EXTRAFLAGS=-DSVAL_FW_VERSION=N" not in text, f"pair:\n{text}")
    # P4-25: protocol 4, one chord for both halves (D41)
    check("the chord is made once" in text and text.index("the chord is made once") < text.index("1/2"), f"pair:\n{text}")
    check(board.chords == 1, f"pair, protocol 4: the chord made {board.chords} times")
    check(sessions[0].arm_flags == su.ARM_PAIR_FLAG and sessions[1].arm_flags == su.ARM_PRE_APPROVED | su.ARM_BY_PAIR, f"pair, protocol 4: ARM flags {sessions[0].arm_flags:#x}, {sessions[1].arm_flags:#x}")
    pm = [r for op, r in board.ops if op == su.PAIR_MANIFEST]
    check(b"".join(r[2:2 + r[1]] for r in pm) == right_m and [r[1] for r in pm] == [52, 52, 4], "pair: PAIR_MANIFEST carries this half's manifest")
    hashes = [ln.split()[2] for ln in text.splitlines() if "MANIFEST HASH" in ln]
    check(hashes[:2] == [__import__("hashlib").sha512(left_m).hexdigest()[:8], __import__("hashlib").sha512(right_m).hexdigest()[:8]], f"pair: both codes shown at the one chord {hashes}")
    check("pre-approved by the pair update" in text and "approved by the pair (ARM flags 0x3)" in text, f"pair:\n{text}")
    check(text.count("chord recognized") == 1, f"pair: one chord\n{text}")
    check(len(sessions) == 2, "pair: two sessions")

    # pair where this half refused its install (BUSY, say): it holds ERROR and
    # never restarted, while the other half took the image. Presence matches
    # and the fw_version numbers match, but the tool must fail, not report success.
    class RefusedBoard(FakeBoard):
        def op(self, op, args=b"", hand=su.HAND_SELF, client=None):
            st, r = super().op(op, args, hand, client)
            if op == su.STATUS and hand in (su.HAND_SELF, 1):
                r = bytes([su.ST_ERROR]) + r[1:11] + bytes([5]) + r[12:]  # ERROR, last error BUSY
            return st, r

    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    su.reopen = lambda serial, timeout: RefusedBoard()
    board = FakeBoard()
    sessions = []
    with contextlib.redirect_stdout(io.StringIO()) as log:
        try:
            rc = su.pair(board, Args(), [right, left], sessions)
            refused = None
        except su.UpdaterError as e:
            rc, refused = 1, str(e)
    check(refused is not None and "did not install its image" in refused and "BUSY" in refused, f"pair with this half's install refused: exit {rc}, error {refused}\n{log.getvalue()}")
    # this half came back running another build's version string: the install
    # did not take; a build with no version string of its own is not compared;
    # the image's own version string is a success. Each case must read page 1.
    for shown, fails in ((b"other-build", True), (b"", False), (b"right-9", False)):
        asked = []

        class OtherBuild(FakeBoard):
            def op(self, op, args=b"", hand=su.HAND_SELF, client=None):
                if op == su.INFO and bytes(args[:1]) == b"\x01" and hand == su.HAND_SELF:
                    asked.append(1)
                    return su.OK, bytes([1]) + struct.pack("<I", 9) + shown.ljust(16, b"\0") + bytes(64)
                return super().op(op, args, hand, client)

        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        su.reopen = lambda serial, timeout: OtherBuild()
        with contextlib.redirect_stdout(io.StringIO()) as log:
            try:
                rc, err = su.pair(FakeBoard(), Args(), [right, left], []), None
            except su.UpdaterError as e:
                rc, err = 1, str(e)
        if fails:
            check(err is not None and "install did not take" in err and "other-build" in err, f"pair, this half runs another build: exit {rc}, error {err}\n{log.getvalue()}")
        else:
            check(rc == 0 and err is None, f"pair, this half reports {shown!r}: exit {rc}, error {err}\n{log.getvalue()}")
        check(bool(asked), f"pair, {shown!r}: INFO page 1 was read")
    # P4-26: protocol 3 (the same board reporting 3): the chord twice, no PAIR_MANIFEST
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    su.reopen = lambda serial, timeout: Proto3Board()
    board = Proto3Board()
    sessions = []
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.pair(board, Args(), [right, left], sessions)
    text = log.getvalue()
    check(rc == 0 and lib.host_slave_commits() == 1 and lib.host_master_reboots() == 1, f"pair, protocol 3: exit {rc}\n{text}")
    check("the chord is made twice" in text and board.chords == 2, f"pair, protocol 3: {board.chords} chords\n{text}")
    check(not [op for op, _ in board.ops if op == su.PAIR_MANIFEST], "pair, protocol 3: no PAIR_MANIFEST")
    check(sessions[1].arm_flags == 0, "pair, protocol 3: no flags read")
    su.reopen = lambda serial, timeout: FakeBoard()
    # P4-25: client ID renewals everywhere, the one between the sessions moving the approval
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    board = FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.pair(board, args_with(rebind_every=1e-9), [right, left], [])
    text = log.getvalue()
    check(rc == 0 and board.chords == 1 and "the approval moved with REBIND" in text, f"pair with renewals: exit {rc}, {board.chords} chords\n{text[-2000:]}")

    # F9: the approval gone between the halves: the tool stops, never asks for a second chord

    class ClearsApproval(FakeBoard):
        cleared = False

        def op(self, op, args=b"", hand=su.HAND_SELF, client=None):
            if op == su.STATUS and not self.cleared and lib.host_slave_commits() == 1 and lib.host_state() == 0:
                self.cleared = True
                super().op(su.ABORT, bytes(4), hand=1)  # in IDLE: clears the held approval
            return super().op(op, args, hand, client)

    for how in ("abort", "timeout"):
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        board = ClearsApproval() if how == "abort" else FakeBoard()
        if how == "timeout":
            real_run = su.Session.run

            def slow_run(self, path, *a, **kw):
                if kw.get("by_pair"):
                    lib.host_passes(181000)  # past the held approval's window
                return real_run(self, path, *a, **kw)

            su.Session.run = slow_run
        try:
            with contextlib.redirect_stdout(io.StringIO()) as log:
                su.pair(board, Args(), [right, left], [])
            check(False, f"pair went on without the approval ({how})")
        except su.UpdaterError as exc:
            check(("approval for this half is gone" in str(exc) or "not pre-approved" in str(exc)) and "one chord" in str(exc), f"pair, approval {how}: {exc}")
        finally:
            if how == "timeout":
                su.Session.run = real_run
        check(board.chords == 1 and lib.host_commit_runs() == 0 and lib.host_slave_commits() == 1, f"pair, approval {how}: {board.chords} chords, this half not installed\n{log.getvalue()}")
    # pair asks once, before anything is sent, when an image names another pointing
    # device than its half runs (D33): no stops it before the left half is touched,
    # --yes goes ahead with both
    right_wrong_m = mu.build_manifest(img, hand=1, pointing_id=1, keymap_id=1, flags=0, epoch=0, storage_format=2, fw_version=9, version="right-tp")
    right_wrong = out / "right_tp.svup"
    right_wrong.write_bytes(right_wrong_m + img)
    for imgs in ([right_wrong, left], [right, wrong]):
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        su.ask = q = Answers("n")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                su.pair(FakeBoard(), Args(), imgs, [])
            check(False, f"pair with {imgs[0].name} + {imgs[1].name} sent without a yes")
        except su.UpdaterError as exc:
            check("not confirmed" in str(exc) and "nothing was sent" in str(exc) and q.asked == 1, f"pair precheck: {exc}")
        check(lib.host_state() == 0 and lib.host_slave_state() == 0 and lib.host_slave_commits() == 0, "pair precheck: nothing started on either half")
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        su.ask = no_question
        with contextlib.redirect_stdout(io.StringIO()) as log:
            rc = su.pair(FakeBoard(), args_with(yes=True), imgs, [])
        check(rc == 0 and lib.host_slave_commits() == 1 and lib.host_master_reboots() == 1, f"pair --yes with {imgs[0].name} + {imgs[1].name}: exit {rc}\n{log.getvalue()}")
    # the board's view of an image: the checks update_manifest_check() makes
    base_info = dict(hand_id=1, hand="right", pointing_id=3, pointing="pmw3389", release_build=False, protocol=3, security_epoch=1, storage_format=2, max_image="0x160000")
    base_m = dict(flags=0, hand=1, pointing_id=3, security_epoch=1, storage_format=2, image_len=0x1000, fw_version=5)
    check(su.image_refusal(base_m, base_info) is None, "image_refusal: a fitting image")
    check(su.image_mismatches(base_m, base_info) == [], "image_mismatches: a fitting image")
    # another hand or pointing device is never a refusal (D33), only a warning
    for change, info_change, want in ((dict(hand=0), {}, ["left hand"]), (dict(pointing_id=2), {}, ["pmw3360"]), (dict(pointing_id=5), {}, ["auto (any module)"]), ({}, dict(pointing_id=5, pointing="auto"), ["build drives auto (any module)"]),
                                      (dict(hand=0, pointing_id=1), {}, ["left hand", "trackpoint"])):
        m2, i2 = {**base_m, **change}, {**base_info, **info_change}
        check(su.image_refusal(m2, i2) is None, f"image_refusal takes {change} {info_change}")
        got = su.image_mismatches(m2, i2)
        check(len(got) == len(want) and all(w in g for w, g in zip(want, got)), f"image_mismatches {change} {info_change}: {got}")
    for change, info_change, want in (({}, dict(protocol=1), "UNSUPPORTED"), ({}, dict(protocol=2), "UNSUPPORTED"), (dict(flags=2), dict(release_build=True), "BAD_IMAGE"), (dict(security_epoch=0), {}, "EPOCH"), (dict(storage_format=1), {}, "STORAGE"),
                                      (dict(image_len=0x161000), {}, "TOO_LARGE")):
        got = su.image_refusal({**base_m, **change}, {**base_info, **info_change})
        check(got is not None and got.startswith(want), f"image_refusal {change} {info_change}: {got}")
    for change, info_change in ((dict(flags=1), {}), ({}, dict(release_build=True)), (dict(flags=1), dict(release_build=True)), (dict(flags=2), {})):
        got = su.image_refusal({**base_m, **change}, {**base_info, **info_change})
        check(got is None, f"image_refusal takes {change} {info_change}: {got}")
    # the other half, over the split cable: protocol 2 or 3 (the relay is the same)
    for proto, ok in ((1, False), (2, True), (3, True), (4, True), (5, False)):
        got = su.image_refusal(base_m, {**base_info, "protocol": proto}, other=True)
        check((got is None) == ok, f"image_refusal, the other half on protocol {proto}: {got}")
    # the half with USB: protocol 3 or 4
    for proto, ok in ((2, False), (3, True), (4, True), (5, False)):
        got = su.image_refusal(base_m, {**base_info, "protocol": proto})
        check((got is None) == ok, f"image_refusal, this half on protocol {proto}: {got}")
    check(not su.fw_differs(5, dict(fw_version=5)) and su.fw_differs(4, dict(fw_version=5)), "fw_differs")
    check(not su.fw_differs(0, dict(fw_version=5)) and not su.fw_differs(5, dict(fw_version=0)), "fw_differs: 0 is unset")
    # pair refuses two images for the same half, before any op
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    try:
        su.pair(FakeBoard(), Args(), [right, right], [])
        check(False, "pair with two right images")
    except su.UpdaterError as exc:
        check("one image for each half" in str(exc), f"pair: {exc}")
    check(lib.host_state() == 0, "pair refused: nothing started")

    # P4-27: reset-settings (D42): both halves invalidate and reset, the board comes
    # back with the storage notice, and --ack (0x2A) clears it
    for this_half, ack in ((False, True), (True, False)):
        lib.host_lib_reset(1)
        lib.host_lib_split(1)
        lib.host_passes(1200)  # presence up
        FakeBoard.storage, FakeBoard.acked_resets = 0, 0
        su.reopen = lambda serial, timeout: FakeBoard()
        board = FakeBoard()
        with contextlib.redirect_stdout(io.StringIO()) as log:
            rc = su.reset_settings(board, args_with(this_half=this_half, ack=ack))
        text = log.getvalue()
        check(rc == 0, f"reset-settings: exit {rc}\n{text}")
        check(lib.host_master_invalidations() == 1 and lib.host_master_soft_resets() == 1, f"reset-settings: this half\n{text}")
        want = 0 if this_half else 1
        check(lib.host_slave_invalidations() == want and lib.host_slave_soft_resets() == want, f"reset-settings: the other half\n{text}")
        check(("the other (left) half and this (right) half" in text) != this_half, f"reset-settings: halves\n{text}")
        check("storage notice: settings reset to this build's defaults" in text, f"reset-settings: notice\n{text}")
        check(("0x2A sent; storage notice now: none" in text) == ack, f"reset-settings: ack\n{text}")
        rs = [r for op, r in board.ops if op == su.SETTINGS_RESET]
        check(rs == [b"RSET" + bytes([0 if this_half else 1]) + bytes(len(rs[0]) - 5)] if rs else False, f"SETTINGS_RESET request {rs}")
    # P4-28: reset-settings --blank (D44): mode 1 to both halves, and the keymap read back blank
    lib.host_lib_reset(1)
    lib.host_lib_split(1)
    lib.host_passes(1200)
    FakeBoard.storage, FakeBoard.acked_resets = 0, 0
    board = FakeBoard()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        rc = su.reset_settings(board, args_with(this_half=False, ack=True, blank=True))
    text = log.getvalue()
    check(rc == 0, f"reset-settings --blank: exit {rc}\n{text}")
    check(lib.host_master_reset_mode() == 1 and lib.host_slave_reset_mode() == 1, f"--blank: mode 1 on both halves\n{text}")
    check("reset to a blank board" in text and "960 keys, 0 not KC_NO" in text, f"--blank: text\n{text}")
    rs = [r for op, r in board.ops if op == su.SETTINGS_RESET]
    check(rs and rs[0][:6] == b"RSET\x01\x01", f"--blank request {rs}")
    # the other half silent: UNAVAILABLE, nothing changed, nothing waited for
    lib.host_lib_reset(1)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            su.reset_settings(FakeBoard(), args_with(this_half=False, ack=False))
        check(False, "reset-settings with no other half")
    except su.UpdaterError as exc:
        check("UNAVAILABLE" in str(exc) and "nothing changed" in str(exc), f"reset-settings, no other half: {exc}")
    check(lib.host_master_invalidations() == 0 and lib.host_master_soft_resets() == 0, "no other half: nothing reset")
    # a protocol 3 board: refused before any op
    lib.host_lib_reset(1)
    try:
        su.reset_settings(Proto3Board(), args_with(this_half=True, ack=False))
        check(False, "reset-settings on protocol 3")
    except su.UpdaterError as exc:
        check("protocol 4" in str(exc), f"reset-settings, protocol 3: {exc}")
    # storage --ack
    FakeBoard.storage = 4
    FakeBoard.acked_resets = lib.host_master_soft_resets()
    with contextlib.redirect_stdout(io.StringIO()) as log:
        su.storage(FakeBoard(), args_with(ack=True))
    check("byte 18 = 0x04" in log.getvalue() and "now: none" in log.getvalue(), f"storage --ack\n{log.getvalue()}")

    print(f"sval_update.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
