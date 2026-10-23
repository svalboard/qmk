#!/usr/bin/env python3
"""Svalboard in-firmware updater, host side: the half with USB over USB (M1),
and the other half through it over the split link (M2).

Needs hidapi (pip install hidapi) and a board running an SVAL_UPDATER=yes build.
On Linux it uses hidapi's hidraw backend, which reports usage pages (the
default libusb backend does not, so it would find no board).
Close other configuration clients (Keybard) first: raw HID replies go to every
open handle.

    sval_update.py list
    sval_update.py info [--other]
    sval_update.py status
    sval_update.py relay
    sval_update.py diag [--clear]
    sval_update.py abort [--nonce N]
    sval_update.py update FILE.svup [--half usb|other] [--yes] [--delay S] [--rebind-every S]
                                    [--min-rebinds N] [--no-commit] [--halt POINT [--halt-unfed]]
    sval_update.py pair FILE1.svup [FILE2.svup] [--yes] [--delay S] [--rebind-every S]
    sval_update.py reset-settings [--this-half] [--blank] [--ack]
    sval_update.py storage [--ack]
    sval_update.py reject FILE.svup

update sends the manifest, prints the manifest hash you confirm, waits for the
chord (Index South + Middle South held for 1 s on the half with USB), then
erases, sends the image, verifies and commits.

The image's own hand picks the half, unless --half says which: usb (the half
with USB) or other. The firmware checks only an image's integrity and
structure (D33), never its hand or pointing device: an image built for the
other hand or for another pointing module (an any-module "auto" image on a
single-module build, or the reverse) is taken, since moving an MCU between
hands or modules is a valid use. So the tool warns about every such mismatch
and asks before sending anything (y/N); --yes skips the question. An image
for the other half (the one without USB) is staged and verified on the half with USB, then relayed to the
other half, which checks the manifest and hash itself (D7); a second COMMIT
makes it write the image and reset. The link to it pauses while it erases,
verifies and commits (both halves type while the image streams). The tool
then waits up to 10 s for the other half to answer with the new version. With
only one half updated the halves differ, which is not supported (V): both
halves show the rainbow, and red after 60 s if they still differ. pair does both: the other half first,
then the half with USB over USB (the chord twice, once per image), then waits
for the board to come back and for presence to show the same release on both
halves within 10 s. pair picks each image by the hand each half reports (an
either-hand image, hand 2, goes to both: give it once), and
asks once (or --yes) if either image names another pointing device than its
half runs. On updater protocol 4 (D41) the chord is made once for both: the
first session also carries this half's manifest (PAIR_MANIFEST), its ARM
shows both manifest hashes, and the chord approves both; this half's own
install is then pre-approved by the board, and the tool never asks for a
second chord (if the approval is gone it stops and says how to finish this
half). On protocol 3 the chord is made twice, as before. A no-chord dev build
(INFO flags bit7, D43) asks for no chord at all. relay prints the relay's
progress; info --other asks the other half.

reset-settings (protocol 4, D42) puts every stored setting on both halves back
to the defaults of the firmware they run (--this-half: only the half with
USB): the keymap, macros, combos, tap dances, overrides, QMK settings, layer
colors, pointing and scan settings. Identity (serial, board name) is kept.
--blank (D44) asks for a blank board instead: the same reset, then every key on
every layer KC_NO; the tool reads the keymap back afterwards and checks it.
Both halves reset; the tool waits for the board, then shows the storage notice
(Sval GET_INFO byte 18 bit 2) that a host clears with 0x2A once it has told the
user (--ack sends it). storage prints that byte, and --ack clears it.

The client ID is renewed every
--rebind-every seconds (50, as Keybard does) and the session moved to it with
REBIND; after each REBIND the old ID must be refused. The number of renewals is
printed; --min-rebinds N fails the run (before COMMIT) if there were fewer (M1
#16: use a short --rebind-every, e.g. 5, and --min-rebinds 3). --delay S waits
S seconds between chunks (slow-transfer test; the session times out after 30 s
without an op, so --delay 31 gives TIMEOUT).

reject (M1 #6) runs the protocol rejection matrix with FILE.svup, an image
this half accepts: unwrapped packets, a second client ID, wrong nonces,
BEGIN before the chord, an out-of-order CHUNK, END too early, ABORT and
REBIND from the wrong client, COMMIT with the wrong CRC, then an overrun. It
needs the chord twice and checks each status and the state after it. It never
commits. The image-level rejections are .svup files from make_update.py
(--unsafe-allow-bad-image) sent with update.

diag (M1 #13) reports how much of main's stack has never been used since boot
and the longest updater pass in ms; --clear restarts the longest-pass record.

--halt POINT needs an SVAL_UPDATE_TEST_HOOKS build (M1 #8-#10): the commit
stops for good at POINT (invalidated, first-erase, mid-program, last-sector,
page0; or fault / fault-erased for a HardFault after the invalidation or after
the first erase). A halt clears RAM as the commit's reset does, then spins
feeding the watchdog, so the board hangs until it is unplugged, or with
--halt-unfed it lets the watchdog reset it within 8 s. A fault point resets at
once (well under 1 s) through the RAM vector table; a reset only after about
8 s means the RAM vector table was not used. Up to last-sector the board comes
back as RPI-RP2 (page 0 is not valid); recover it with flash.sh.

The protocol is documented at the top of keyboards/svalboard/updater/updater.c.
This tool speaks updater protocols 3 and 4: 64-byte raw HID reports, with the
MANIFEST and CHUNK piece sizes taken from INFO. A board on protocol 2 (32-byte
reports) is refused: update it with the sval_update.py of its own release, or
copy the .uf2 in BOOTSEL mode.
"""
import argparse
import copy
import hashlib
import json
import secrets
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_update  # noqa: E402  (same directory: .svup parsing and the CRC)

CHANNEL = 0x55
SET_VALUE = 0x07
WRAPPER, VIA_PROTO, WRAPPER_ERROR = 0xDD, 0xFE, 0xFF
HAND_SELF = 0xFF
INFO, MANIFEST, ARM, BEGIN, CHUNK, END, STATUS, COMMIT, ABORT, REBIND, TEST_HALT, DIAG, RELAY, PAIR_MANIFEST, SETTINGS_RESET = range(15)
# ARM (protocol 4): the mode byte and the reply flags (update_manifest.h).
ARM_SINGLE, ARM_PAIR = 0, 1
ARM_PRE_APPROVED, ARM_BY_PAIR, ARM_NO_CHORD, ARM_PAIR_FLAG = 0x01, 0x02, 0x04, 0x08
SVAL_PREFIX, SVAL_GET_INFO, SVAL_STORAGE_RESET_CLEAR = 0xDF, 0x00, 0x2A
HALT_POINTS = {"invalidated": 1, "first-erase": 2, "mid-program": 3, "last-sector": 4, "page0": 5, "fault": 6, "fault-erased": 7}
# Protocol 3: one 64-byte report each way. The updater's value bytes are what is
# left after the client wrapper's 6 bytes and VIA's 3 (updater.c); INFO reports
# the piece sizes the board takes, which can be no larger than these.
REPORT = 64
VALUE_BYTES = REPORT - 6 - 3  # 55: [status] + 54 reply bytes
MANIFEST_PIECE_MAX = VALUE_BYTES - 3  # 52, after [hand][off][n]
CHUNK_PIECE_MAX = VALUE_BYTES - 5  # 50, after [hand][off u24][n]
# The half with USB: protocol 3, or 4 (one chord for both halves, SETTINGS_RESET).
USB_HALF_PROTOS = (3, 4)
# The other half is updated over the split cable, whose relay is the same in
# protocols 2, 3 and 4: a half on any of them can be updated through this one.
OTHER_HALF_PROTOS = (2, 3, 4)
OLD_FIRMWARE = ("a board on updater protocol 2 (32-byte reports) does not answer this tool: update it with "
                "the sval_update.py of its release, or copy the .uf2 in BOOTSEL mode")

STATUS_NAMES = ["OK", "INVALID", "UNAVAILABLE", "FLASH_ERR", "ACCEPTED", "BUSY", "NOT_CONFIRMED", "WRONG_HW", "BAD_SIG", "BAD_HASH", "BAD_IMAGE", "EPOCH", "OUT_OF_ORDER", "OVERRUN", "TOO_LARGE", "TIMEOUT", "OTHER_CLIENT", "UNSUPPORTED", "STORAGE"]
STATE_NAMES = ["IDLE", "MANIFEST_LOADING", "CONFIRM_WAIT", "VERIFYING_MANIFEST", "ERASING", "RECEIVING", "VERIFYING_IMAGE", "VERIFIED", "COMMITTING", "ERROR", "RELAYING", "RELAYED", "SUBSIDE_COMMITTING"]
OK, INVALID, ACCEPTED, NOT_CONFIRMED, BAD_HASH = 0, 1, 4, 6, 9
OUT_OF_ORDER, OVERRUN, OTHER_CLIENT, UNSUPPORTED = 12, 13, 16, 17
ST_IDLE, ST_MANIFEST_LOADING, ST_CONFIRM_WAIT, ST_RECEIVING, ST_VERIFIED, ST_ERROR = 0, 1, 2, 5, 7, 9
ST_RELAYING, ST_RELAYED, ST_SUBSIDE_COMMITTING = 10, 11, 12
# relay_phase_t (keyboards/svalboard/updater/update_split.h)
RELAY_PHASES = ["idle", "start", "manifest", "checking and erasing", "sending", "end", "verifying", "verified", "commit", "committing", "probe", "done", "failed", "aborting"]
PRESENCE_NONE, PRESENCE_MATCH, PRESENCE_VERSION = 0, 1, 2
HANDS = {0: "left", 1: "right"}
HAND_EITHER = 2  # an image for both halves (make_update.py --kb svalboard/either)
HAND_NAMES = {**HANDS, HAND_EITHER: "either", 0xFF: "unknown"}
POINTING = {v: k for k, v in make_update.POINTING.items()}


def status_name(code):
    return STATUS_NAMES[code] if code < len(STATUS_NAMES) else f"status {code}"


# The other half by split presence (update_presence_status_t; M2, V).
PRESENCE_NAMES = ["none", "match", "version differs", "same hand", "invalid answer"]


def presence_name(code):
    return PRESENCE_NAMES[code] if code < len(PRESENCE_NAMES) else f"presence {code}"


def state_name(code):
    return STATE_NAMES[code] if code < len(STATE_NAMES) else f"state {code}"


class UpdaterError(RuntimeError):
    pass


class ClientIdExpired(RuntimeError):
    pass


HID_HELP = ("needs hidapi (pip install hidapi); on Linux it uses hidapi's hidraw backend, "
            "which reports usage pages, and needs read/write access to /dev/hidraw*")


def hid_backend(platform=sys.platform):
    # The hidapi wheel's default Linux backend is libusb, which reports no usage
    # pages (so the filter below matches nothing) and can detach the kernel
    # keyboard driver. Use hidraw there, as Keybard Host does.
    if platform.startswith("linux"):
        import hidraw
        return hidraw
    import hid
    return hid


def devices():
    hid = hid_backend()
    return [d for d in hid.enumerate() if d.get("usage_page") == 0xFF61 and d.get("usage") == 0x62]


class Device:
    def __init__(self, serial=None):
        hid = hid_backend()
        matches = [d for d in devices() if serial is None or d.get("serial_number") == serial]
        if len(matches) != 1:
            raise RuntimeError(f"Expected one Svalboard, found {len(matches)}; use --serial (see list)")
        self.serial = matches[0].get("serial_number")
        self.handle = hid.device()
        self.handle.open_path(matches[0]["path"])
        self.client = None
        self.round_trips = 0
        self.round_trip_s = 0.0

    def close(self):
        if self.handle:
            self.handle.close()
            self.handle = None

    def _exchange(self, packet, match):
        packet = bytes(packet).ljust(REPORT, b"\0")
        if len(packet) != REPORT:
            raise ValueError("packet longer than one report")
        # hidapi: report ID 0 (the raw interface has none), then the report
        if self.handle.write(b"\0" + packet) != REPORT + 1:
            raise RuntimeError(f"Incomplete HID write ({OLD_FIRMWARE})")
        t0 = time.monotonic()
        deadline = t0 + 2.0
        while time.monotonic() < deadline:
            reply = bytes(self.handle.read(REPORT, 200))
            if len(reply) == REPORT and match(reply):
                self.round_trips += 1
                self.round_trip_s += time.monotonic() - t0
                return reply
        raise RuntimeError(f"No reply to {packet[:12].hex()} ({OLD_FIRMWARE})")

    def new_client(self):
        """A fresh client ID from the Sval client wrapper, without adopting it."""
        nonce = secrets.token_bytes(20)
        r = self._exchange(bytes([WRAPPER]) + bytes(4) + nonce, lambda r: r[0] == WRAPPER and r[1:5] == bytes(4) and r[5:25] == nonce)
        cid = r[25:29]
        if cid in (bytes(4), b"\xFF" * 4):
            raise RuntimeError("Invalid client bootstrap response")
        return cid

    def bootstrap(self):
        """A fresh client ID, used for the ops from now on."""
        self.client = self.new_client()
        return self.client

    def unwrapped_op(self, op, args=b"", hand=HAND_SELF):
        """One updater op as a plain VIA packet, without the client wrapper (reject tests)."""
        body = bytes([SET_VALUE, CHANNEL, op, hand]) + bytes(args)
        if len(body) > 3 + VALUE_BYTES:
            raise ValueError("updater request too long")
        r = self._exchange(body, lambda r: r[0:3] == body[:3])
        return r[3], r[4:3 + VALUE_BYTES]

    def op(self, op, args=b"", hand=HAND_SELF, client=None):
        """One updater op in the client wrapper: (status, the 54 reply bytes after it)."""
        cid = client or self.client or self.bootstrap()
        body = bytes([SET_VALUE, CHANNEL, op, hand]) + bytes(args)
        if len(body) > 3 + VALUE_BYTES:
            raise ValueError("updater request too long")
        r = self._exchange(bytes([WRAPPER]) + cid + bytes([VIA_PROTO]) + body, lambda r: r[0] == WRAPPER and r[1:5] == cid and (r[5] == WRAPPER_ERROR or r[6:9] == body[:3]))
        if r[5] == WRAPPER_ERROR:
            raise ClientIdExpired(f"client wrapper error {r[6]} for client {cid.hex()}")
        return r[9], r[10:REPORT]

    def via(self, body):
        """One plain VIA command in the client wrapper: the reply from its command byte on."""
        cid = self.client or self.bootstrap()
        body = bytes(body)
        r = self._exchange(bytes([WRAPPER]) + cid + bytes([VIA_PROTO]) + body, lambda r: r[0] == WRAPPER and r[1:5] == cid and (r[5] == WRAPPER_ERROR or r[6] == body[0]))
        if r[5] == WRAPPER_ERROR:
            raise ClientIdExpired(f"client wrapper error {r[6]} for client {cid.hex()}")
        return r[6:REPORT]

    def sval(self, cmd, args=b""):
        """One Sval command (0xDF) in the client wrapper: the reply after [0xDF][cmd]."""
        cid = self.client or self.bootstrap()
        body = bytes([SVAL_PREFIX, cmd]) + bytes(args)
        r = self._exchange(bytes([WRAPPER]) + cid + body, lambda r: r[0] == WRAPPER and r[1:5] == cid and (r[5] == WRAPPER_ERROR or r[5:7] == body[:2]))
        if r[5] == WRAPPER_ERROR:
            raise ClientIdExpired(f"client wrapper error {r[6]} for client {cid.hex()}")
        return r[7:REPORT]


def storage_flags(dev):
    """Sval GET_INFO byte 18: bit0 storage unreadable (reset), bit1 a change could not be saved,
    bit2 settings reset to this build's defaults (D39, D42), until 0x2A."""
    r = dev.sval(SVAL_GET_INFO)
    return r[16]


VIA_GET_LAYER_COUNT, VIA_KEYMAP_GET_BUFFER = 0x11, 0x12
KEYMAP_KEYS = 10 * 6  # MATRIX_ROWS * MATRIX_COLS (keyboards/svalboard/config.h)
KEYMAP_PIECE = 28  # bytes per VIA get-buffer request, well inside the wrapped report


def keymap_bytes(dev):
    """The whole dynamic keymap through VIA: every layer, 2 bytes per key, big-endian."""
    layers = dev.via(bytes([VIA_GET_LAYER_COUNT]))[1]
    size = layers * KEYMAP_KEYS * 2
    out = bytearray()
    for off in range(0, size, KEYMAP_PIECE):
        n = min(KEYMAP_PIECE, size - off)
        r = dev.via(bytes([VIA_KEYMAP_GET_BUFFER, off >> 8, off & 0xFF, n]))
        out += r[4:4 + n]
    return layers, bytes(out)


def storage_text(flags):
    out = [n for b, n in ((1, "storage reset (unreadable)"), (2, "a change could not be saved"), (4, "settings reset to this build's defaults")) if flags & b]
    return ", ".join(out) or "none"


def decode_info(st, r):
    return dict(
        status=status_name(st),
        state=state_name(r[0]),
        protocol=r[1],
        slot_base=f"{struct.unpack_from('<H', r, 2)[0] * 4096:#x}",
        slot_size=f"{struct.unpack_from('<H', r, 4)[0] * 4096:#x}",
        max_image=f"{struct.unpack_from('<H', r, 6)[0] * 4096:#x}",
        jedec=r[8:11].hex(),
        pointing=POINTING.get(r[11], r[11]),
        hand=HANDS.get(r[12], r[12]),
        hand_id=r[12],
        pointing_id=r[11],
        fw_version=struct.unpack_from("<I", r, 13)[0],
        storage_format=r[17],
        security_epoch=struct.unpack_from("<H", r, 18)[0],
        flash_16mib=bool(r[20] & 1),
        settings_writes_failing=bool(r[20] & 2),
        release_build=bool(r[20] & 4),
        test_hooks=bool(r[20] & 8),
        other_half_mismatch=bool(r[20] & 32),
        no_chord=bool(r[20] & 0x80),
        last_error=status_name(r[21]),
        chunk_max=r[22],
        manifest_max=r[23],
        report_bytes=r[24]
    )


def piece_sizes(info):
    """(MANIFEST piece, CHUNK data) sizes from a protocol 3 INFO, checked against
    what fits this tool's reports."""
    if info["report_bytes"] != REPORT:
        raise UpdaterError(f"the board reports {info['report_bytes']}-byte raw HID reports, this tool sends {REPORT}")
    if not (1 <= info["manifest_max"] <= MANIFEST_PIECE_MAX and 1 <= info["chunk_max"] <= CHUNK_PIECE_MAX):
        raise UpdaterError(f"the board's piece sizes (MANIFEST {info['manifest_max']}, CHUNK {info['chunk_max']}) "
                           f"do not fit {REPORT}-byte reports")
    return info["manifest_max"], info["chunk_max"]


def check_protocol(info):
    """The half with USB must speak one of this tool's protocols; returns its piece sizes."""
    if info["protocol"] not in USB_HALF_PROTOS:
        raise UpdaterError(f"the board speaks updater protocol {info['protocol']}, this tool "
                           f"{' and '.join(map(str, USB_HALF_PROTOS))}: {OLD_FIRMWARE}")
    return piece_sizes(info)


def image_refusal(m, info, other=False):
    """Why a half would refuse this manifest, as far as its INFO shows (None: no reason seen).
    The same order of checks as update_manifest_check() (update_image.c): integrity and
    structure only, never the hand or pointing device (D33; see image_mismatches). other:
    the half without USB, reached over the split cable, whose relay protocols 2 to 4 share."""
    protos = OTHER_HALF_PROTOS if other else USB_HALF_PROTOS
    if info["protocol"] not in protos:
        return (f"UNSUPPORTED: the half speaks updater protocol {info['protocol']}, this tool "
                f"{', '.join(map(str, protos))} (flash it with BOOTSEL and the .uf2)")
    if info["release_build"] and m["flags"] & make_update.FLAG_DIAGNOSTIC:
        return "BAD_IMAGE: a DIAGNOSTIC image on a release build"
    if m["security_epoch"] < info["security_epoch"]:
        return f"EPOCH: image epoch {m['security_epoch']}, the half's floor {info['security_epoch']}"
    if m["storage_format"] < info["storage_format"]:
        return f"STORAGE: image storage format {m['storage_format']}, the half's floor {info['storage_format']}"
    if m["image_len"] > int(info["max_image"], 16):
        return f"TOO_LARGE: {m['image_len']} B, the half takes at most {info['max_image']}"
    return None


def pointing_text(pid):
    if pid == make_update.POINTING["auto"]:
        return "auto (any module)"
    return POINTING.get(pid, f"pointing {pid}")


def image_mismatches(m, info):
    """Where the image was built for another hand or pointing device than the half runs,
    as warnings. The firmware takes such an image (D33); the tool asks first."""
    out = []
    if m["hand"] != info["hand_id"] and m["hand"] != HAND_EITHER:
        side = HANDS.get(m["hand"], m["hand"])
        out.append(f"the image was built for the {side} hand; this half runs a {info['hand']} build. "
                   f"Afterwards its updater and split presence report it as the {side} half")
    if m["pointing_id"] != info["pointing_id"]:
        want, have = pointing_text(m["pointing_id"]), pointing_text(info["pointing_id"])
        note = ""
        if m["pointing_id"] != make_update.POINTING["auto"]:
            note = f"; only a {want} module will work on that half afterwards"
        out.append(f"the image drives pointing {want}; the {info['hand']} half's build drives {have}{note}")
    return out


def ask(prompt):
    return input(prompt)


def confirm_mismatches(warnings, args, where=""):
    """Print each mismatch and ask (y/N), unless --yes. Refusing sends nothing."""
    if not warnings:
        return
    for w in warnings:
        print(f"warning: {where}{w}")
    if getattr(args, "yes", False):
        print("--yes: going ahead")
        return
    try:
        answer = ask("Flash it anyway? [y/N] ")
    except EOFError:
        answer = ""
    if answer.strip().lower() not in ("y", "yes"):
        raise UpdaterError("not confirmed: nothing was sent (--yes skips the question)")


def fw_differs(seen, m):
    """A half reports another fw than the manifest it was given (both nonzero: updater builds
    take SVAL_FW_VERSION from updater/fw_version.txt, and make_update.py copies it into the manifest)."""
    return seen != m["fw_version"] and seen != 0 and m["fw_version"] != 0


FW_NOTE = ("(a build reports the SVAL_FW_VERSION it was compiled with, from updater/fw_version.txt or "
           "make SVAL_FW_VERSION=N; make_update.py copies it from the image unless --fw-version says otherwise)")


def relay_phase_name(code):
    return RELAY_PHASES[code] if code < len(RELAY_PHASES) else f"phase {code}"


def decode_relay(st, r):
    """The RELAY op: the relay to the other half (M2)."""
    return dict(
        status=status_name(st),
        phase=relay_phase_name(r[0]),
        phase_id=r[0],
        other_state=state_name(r[1]),
        other_state_id=r[1],
        other_error=status_name(r[2]),
        other_error_id=r[2],
        acked=r[3] | r[4] << 8 | r[5] << 16,
        image_len=r[6] | r[7] << 8 | r[8] << 16,
        other_sectors_erased=struct.unpack_from("<H", r, 9)[0],
        other_sectors_to_erase=struct.unpack_from("<H", r, 11)[0],
        retries=struct.unpack_from("<H", r, 13)[0],
        relay_ms=struct.unpack_from("<I", r, 15)[0],
        link_paused=bool(r[19] & 1),
        unconfirmed=bool(r[19] & 2),
        error=status_name(r[20]),
        error_id=r[20],
        state=state_name(r[21]),
        state_id=r[21]
    )


def decode_diag(st, r):
    return dict(status=status_name(st), stack_unused=struct.unpack_from("<H", r, 0)[0], stack_size=struct.unpack_from("<H", r, 2)[0], longest_pass_ms=struct.unpack_from("<H", r, 4)[0], longest_pass_state=state_name(r[6]), **decode_crumbs(r))


OP_NAMES = ["INFO", "MANIFEST", "ARM", "BEGIN", "CHUNK", "END", "STATUS", "COMMIT", "ABORT", "REBIND", "TEST_HALT", "DIAG", "RELAY", "PAIR_MANIFEST", "SETTINGS_RESET"]


def decode_crumbs(r):
    """Why the chip last reset, and what the updater was doing just before (watchdog scratch 0-3)."""
    if len(r) < 20:
        return {}
    f = r[7]
    why = [n for b, n in ((1, "power-on/brown-out"), (2, "RUN pin"), (4, "debugger"), (8, "watchdog timer"), (16, "watchdog force")) if f & b] or ["none of POR/RUN/watchdog: software or core reset"]
    out = dict(last_reset=why)
    if not f & 0x80:
        out["before_reset"] = "no breadcrumbs (first boot, power-on or RUN pin)"
        return out
    c1, c2, c3 = struct.unpack_from("<III", r, 8)
    op, state, st = c1 >> 24, (c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF
    out["before_reset"] = dict(last_op=OP_NAMES[op] if op < len(OP_NAMES) else f"op {op}", state=state_name(state), op_status="still inside the op" if st == 0xFF else status_name(st), op_count=c1 & 0xFF, last_op_ms=c2, last_pass_ms=c3)
    return out


def decode_status(st, r):
    return dict(
        status=status_name(st),
        state=state_name(r[0]),
        state_id=r[0],
        staged=r[1] | r[2] << 8 | r[3] << 16,
        image_len=r[4] | r[5] << 8 | r[6] << 16,
        sectors_erased=struct.unpack_from("<H", r, 7)[0],
        sectors_to_erase=struct.unpack_from("<H", r, 9)[0],
        last_error=status_name(r[11]),
        last_error_id=r[11],
        verify_ms=struct.unpack_from("<H", r, 14)[0],
        erase_ms_max=struct.unpack_from("<H", r, 16)[0],
        crc_lo16=struct.unpack_from("<H", r, 18)[0],
        chord_made=bool(r[20] & 1),
        session_bound=bool(r[20] & 2),
        settings_writes_failing=bool(r[20] & 4),
        other_half=presence_name(r[21]),
        other_half_id=r[21],
        # protocol 4 (D41); 0 on protocol 3
        approval_held=bool(r[22] & 1),
        pre_approved=bool(r[22] & 2)
    )


class Session:
    def __init__(self, dev, args):
        self.dev = dev
        self.args = args
        self.hand = None
        self.nonce = None
        self.renewed = time.monotonic()
        self.renewals = 0
        self.arm_flags = 0
        self.pair_nonce = None

    def need(self, what, got, *want):
        st, r = got
        if st not in want:
            extra = ""
            if st != OK:
                s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                extra = f" (state {s['state']}, last error {s['last_error']})"
            raise UpdaterError(f"{what}: {status_name(st)}{extra}")
        return r

    def status(self):
        return decode_status(*self.dev.op(STATUS, hand=self.hand))

    def nonce_op(self, op, extra=b""):
        return self.dev.op(op, struct.pack("<I", self.nonce) + extra, hand=self.hand)

    def renew(self):
        """New client ID, then REBIND; the old ID must then be refused (D3, M1 #16)."""
        old = self.dev.client
        self.dev.bootstrap()
        self.need("REBIND", self.nonce_op(REBIND), OK)
        # A repeat of the first byte is harmless to a live session; from the old ID it must be refused.
        try:
            st, _ = self.dev.op(CHUNK, bytes([0, 0, 0, 1, self.image[0]]), hand=self.hand, client=old)
            how = status_name(st)
        except ClientIdExpired:
            st, how = OTHER_CLIENT, "expired at the client wrapper"
        if st != OTHER_CLIENT:
            raise UpdaterError(f"old client ID {old.hex()} was not refused after REBIND: {how}")
        print(f"  renewed client ID {old.hex()} -> {self.dev.client.hex()}; old ID refused ({how})")
        self.renewed = time.monotonic()
        self.renewals += 1

    def maybe_renew(self):
        if self.args.rebind_every and time.monotonic() - self.renewed >= self.args.rebind_every:
            self.renew()

    def poll(self, until, what, timeout):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            self.maybe_renew()
            s = self.status()
            if s["state_id"] == ST_ERROR:
                raise UpdaterError(f"{what}: {s['last_error']}")
            if until(s):
                return s
            if s["state"] != last:
                last = s["state"]
            time.sleep(0.2)
        raise UpdaterError(f"{what}: no progress in {timeout} s")

    def run(self, path, confirm_other=True, confirmed=False, pair_with=None, by_pair=False):
        """One half's install. pair_with (protocol 4, the other half's session in pair): this
        half's 108-byte manifest, sent with PAIR_MANIFEST so one chord approves both (D41).
        by_pair: this half's install after that; the board must pre-approve it, and the tool
        never asks for a second chord."""
        data = Path(path).read_bytes()
        manifest, image = make_update.parse_svup(data)
        self.image = image
        m = make_update.parse_manifest(manifest)
        mhash = hashlib.sha512(manifest).digest()[:4]
        crc_body = make_update.crc32_mpeg2(image[0x100:])
        version = m["version"].rstrip(b"\0").decode("ascii", "replace")

        info = decode_info(*self.dev.op(INFO))
        board_hand = info["hand_id"]
        manifest_piece, chunk_piece = check_protocol(info)
        # The image's own hand picks the half, this one or the other one through it (M2),
        # unless --half says which.
        half = getattr(self.args, "half", None)
        if half == "usb":
            self.other = False
        elif half == "other":
            self.other = True
        else:
            self.other = m["hand"] in HANDS and m["hand"] == board_hand ^ 1
        self.hand = board_hand ^ 1 if self.other else board_hand
        if getattr(self.args, "halt", None) and not info["test_hooks"]:
            raise UpdaterError("--halt needs an SVAL_UPDATE_TEST_HOOKS build on the board")
        if getattr(self.args, "halt", None) and self.other:
            raise UpdaterError("--halt is for the half with USB only")
        print(f"board: {info['hand']} half, pointing {info['pointing']}, JEDEC {info['jedec']}, state {info['state']}, "
              f"status {info['status']}")
        target = self.other_half(m) if self.other else info
        if not confirmed:
            confirm_mismatches(image_mismatches(m, target), self.args)
        print(f"image: {version!r} ({m['fw_version']}), {len(image)} B, flags {m['flags']:#x}" + (f", for the other ({HANDS[self.hand]}) half" if self.other else ""))

        t_start = time.monotonic()
        blob = manifest
        for off in range(0, len(blob), manifest_piece):
            piece = blob[off:off + manifest_piece]
            r = self.need("MANIFEST", self.dev.op(MANIFEST, bytes([off, len(piece)]) + piece, hand=self.hand), OK)
            if r[0] != off + len(piece):
                raise UpdaterError(f"MANIFEST: board has {r[0]} bytes, expected {off + len(piece)}")
        if pair_with is not None:
            # Protocol 4 (D41): this half's manifest beside the other half's, then a pair ARM.
            for off in range(0, len(pair_with), manifest_piece):
                piece = pair_with[off:off + manifest_piece]
                r = self.need("PAIR_MANIFEST", self.dev.op(PAIR_MANIFEST, bytes([off, len(piece)]) + piece, hand=board_hand), OK)
                if r[0] != off + len(piece):
                    raise UpdaterError(f"PAIR_MANIFEST: board has {r[0]} bytes, expected {off + len(piece)}")
        mode = ARM_PAIR if pair_with is not None else ARM_SINGLE
        r = self.need("ARM", self.dev.op(ARM, bytes([mode]), hand=self.hand), OK)
        self.nonce = self.pair_nonce = struct.unpack_from("<I", r, 0)[0]
        self.renewed = time.monotonic()
        if r[4:8] != mhash:
            raise UpdaterError(f"ARM: board's manifest hash {r[4:8].hex()} differs from this file's {mhash.hex()}")
        flags = r[12] if info["protocol"] >= 4 else 0
        self.arm_flags = flags
        pair_hash = None
        if pair_with is not None:
            pair_hash = hashlib.sha512(pair_with).digest()[:4]
            if r[8:12] != pair_hash or not flags & ARM_PAIR_FLAG:
                raise UpdaterError(f"ARM: board's pair manifest hash {r[8:12].hex()} (flags {flags:#x}) differs from "
                                   f"this half's file's {pair_hash.hex()}")
        if by_pair and not flags & ARM_PRE_APPROVED:
            # Never a second chord (D41): the approval lapsed or was cleared.
            raise UpdaterError("ARM: this half's install was not pre-approved by the pair (the approval was cleared "
                               "or timed out); nothing was installed on this half. Finish it with: update FILE.svup "
                               "(one chord)")
        usb_half = info["hand"]
        print()
        print(f"  MANIFEST HASH  {mhash.hex()}   ({version}, {len(image)} bytes" + (f", for the {HANDS[self.hand]} half without USB)" if self.other else ")"))
        if pair_hash is not None:
            print(f"  MANIFEST HASH  {pair_hash.hex()}   (this {usb_half} half, the one with USB, installed next)")
        if flags & ARM_BY_PAIR:
            print("  pre-approved by the pair update: installing at once")
            print()
        elif flags & ARM_PRE_APPROVED or info["no_chord"]:
            print()  # a no-chord dev build (D43)
        else:
            print(f"  Check {'these match' if pair_hash else 'this matches'} what you expect, then confirm on the keyboard: hold Index South + Middle South")
            print(f"  for 1 second on the {usb_half} half, the one with USB (M and , on the right; C and V on the left).")
            print("  The LED blinks blue." + (" This one chord approves both halves." if pair_hash else ""))
            print()
        self.poll(lambda s: s["chord_made"], "waiting for the chord", 35)
        if not flags & ARM_PRE_APPROVED and not info["no_chord"]:
            print("chord recognized")
        self.need("BEGIN", self.nonce_op(BEGIN), ACCEPTED)
        s = self.poll(lambda s: s["state_id"] == ST_RECEIVING, "verifying the manifest and erasing", 120)
        print(f"manifest checked; erased {s['sectors_to_erase']} sectors, longest {s['erase_ms_max']} ms")

        t_send = time.monotonic()
        off, step = 0, max(len(image) // 20, 1)
        next_report = step
        while off < len(image):
            self.maybe_renew()
            n = min(chunk_piece, len(image) - off)
            st, r = self.dev.op(CHUNK, struct.pack("<I", off)[:3] + bytes([n]) + image[off:off + n], hand=self.hand)
            nxt = r[0] | r[1] << 8 | r[2] << 16
            if st == OK:
                off = nxt
            elif st == OUT_OF_ORDER and nxt <= len(image):
                print(f"  resync: board expects offset {nxt}, not {off}")
                off = nxt
            else:
                self.need(f"CHUNK at {off}", (st, r), OK)
            if off >= next_report:
                print(f"  {off * 100 // len(image)}% ({off} B)")
                next_report += step
            if self.args.delay:
                time.sleep(self.args.delay)
        t_sent = time.monotonic()
        self.need("END", self.nonce_op(END), ACCEPTED)
        s = self.poll(lambda s: s["state_id"] == ST_VERIFIED, "verifying the image", 60)
        if s["crc_lo16"] != crc_body & 0xFFFF:
            raise UpdaterError(f"board's image CRC {s['crc_lo16']:#06x} differs from this file's {crc_body & 0xFFFF:#06x}")
        summary = dict(
            transfer_s=round(t_sent - t_send, 2),
            total_s=round(time.monotonic() - t_start, 2),
            round_trips=self.dev.round_trips,
            mean_round_trip_ms=round(1000 * self.dev.round_trip_s / max(self.dev.round_trips, 1), 2),
            verify_ms=s["verify_ms"],
            erase_ms_max=s["erase_ms_max"],
            rebinds=self.renewals
        )
        print("verified: " + json.dumps(summary))
        min_rebinds = getattr(self.args, "min_rebinds", 0) or 0
        if self.renewals < min_rebinds:
            raise UpdaterError(f"only {self.renewals} client ID renewals with REBIND, --min-rebinds {min_rebinds} "
                               "(use a shorter --rebind-every)")
        if self.args.no_commit:
            print("--no-commit: leaving the verified image staged (ABORT to clear the session)")
            return 0
        if self.other:
            return self.relay_and_commit(crc_body, m, confirm_other)
        if getattr(self.args, "halt", None):
            fed = not self.args.halt_unfed
            self.need("TEST_HALT", self.nonce_op(TEST_HALT, bytes([HALT_POINTS[self.args.halt], int(fed)])), OK)
            print(f"test hook: the commit will halt at {self.args.halt}, "
                  f"{'feeding the watchdog (unplug to end it)' if fed else 'not feeding the watchdog (reset within 8 s)'}")
        st, r = self.nonce_op(COMMIT, struct.pack("<H", crc_body & 0xFFFF))
        if st == UNSUPPORTED:
            print("COMMIT: UNSUPPORTED - this build cannot commit; the image is staged and verified")
            return 3
        self.need("COMMIT", (st, r), ACCEPTED)
        print("committing: the board writes the new image and resets")
        return 0

    # ---- the other half (M2) ----

    def wait_presence(self, timeout):
        """STATUS until split presence shows the other half (or timeout); returns the last STATUS."""
        deadline = time.monotonic() + timeout
        while True:
            s = decode_status(*self.dev.op(STATUS, hand=self.hand))
            if s["other_half_id"] != 0 or time.monotonic() >= deadline:
                return s
            time.sleep(0.25)

    def other_half(self, m):
        """The half without USB must answer presence as the same release or another version, and INFO."""
        s = self.wait_presence(5)
        if s["other_half_id"] not in (PRESENCE_MATCH, PRESENCE_VERSION):
            raise UpdaterError(f"the other half: presence {s['other_half']}; it must be connected and run an "
                               "SVAL_UPDATER build (if it does not answer, plug USB into it and copy the .uf2)")
        st, r = self.dev.op(INFO, hand=self.hand)
        if r[0] == 0xFF:
            raise UpdaterError(f"the other half does not answer INFO ({status_name(st)})")
        oi = decode_info(st, r)
        print(f"other half: {oi['hand']}, pointing {oi['pointing']}, fw {oi['fw_version']}, state {oi['state']}, "
              f"status {oi['status']}, presence {s['other_half']}")
        return oi

    def relay(self):
        return decode_relay(*self.dev.op(RELAY, hand=self.hand))

    def relay_and_commit(self, crc_body, m, confirm_other):
        crc = struct.pack("<H", crc_body & 0xFFFF)
        self.need("COMMIT (relay)", self.nonce_op(COMMIT, crc), ACCEPTED)
        print("relaying to the other half, which checks the manifest and the hash itself")
        deadline = time.monotonic() + 180
        last = None
        while True:
            self.maybe_renew()
            rl = self.relay()
            if rl["state_id"] == ST_ERROR:
                s = self.status()
                raise UpdaterError(f"relay: {s['last_error']} (relay {rl['phase']}, {rl['error']}; other half "
                                   f"{rl['other_state']}, {rl['other_error']})")
            if rl["state_id"] == ST_RELAYED:
                break
            if rl["state_id"] != ST_RELAYING:
                raise UpdaterError(f"relay: unexpected state {rl['state']}")
            if rl["phase_id"] == 4:
                now = f"sending {rl['acked'] * 100 // max(rl['image_len'], 1) // 5 * 5}%"
            elif rl["phase_id"] == 3 and rl["other_sectors_to_erase"]:
                now = f"the other half erases ({rl['other_sectors_erased']}/{rl['other_sectors_to_erase']} sectors)"
            elif rl["phase_id"] == 3:
                now = "the other half checks the manifest"
            else:
                now = rl["phase"]
            if now != last:
                print(f"  {now}")
                last = now
            if time.monotonic() > deadline:
                raise UpdaterError(f"relay: no end in 180 s ({rl['phase']})")
            time.sleep(0.2)
        print("relayed: " + json.dumps(dict(relay_ms=rl["relay_ms"], retries=rl["retries"], bytes=rl["acked"])))
        # No REBIND from here: in SUBSIDE_COMMITTING only INFO, STATUS and RELAY are answered.
        self.need("COMMIT (other half)", self.nonce_op(COMMIT, crc), ACCEPTED)
        print("the other half writes the image and resets; the split link stays paused meanwhile")
        deadline = time.monotonic() + 30
        while True:
            s = self.status()
            if s["state_id"] == ST_ERROR:
                rl = self.relay()
                raise UpdaterError(f"the other half's commit: {s['last_error']} (other half {rl['other_state']}, "
                                   f"{rl['other_error']})")
            if s["state_id"] == ST_IDLE:
                break
            if time.monotonic() > deadline:
                raise UpdaterError(f"the other half's commit: no end in 30 s ({s['state']})")
            time.sleep(0.2)
        rl = self.relay()
        if rl["unconfirmed"]:
            # Expected: the hold usually ends before the other half has rebooted.
            print("the other half took COMMIT (not yet confirmed: it was still rebooting); checking its version")
        else:
            print("the other half took COMMIT")
        self.nonce = None
        if not confirm_other:
            return 0
        return self.confirm_other(m)

    def confirm_other(self, m):
        """Up to 10 s (after its reboot) for the other half to answer again; its version and presence."""
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            st, r = self.dev.op(INFO, hand=self.hand)
            if r[0] != 0xFF:
                seen = decode_info(st, r)["fw_version"]
                s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                # Presence reads NONE until its first ping after the reboot.
                while s["other_half_id"] == PRESENCE_NONE and time.monotonic() < deadline:
                    time.sleep(0.25)
                    s = decode_status(*self.dev.op(STATUS, hand=self.hand))
                print(f"the other half answers again: fw {seen}, presence {s['other_half']}")
                if fw_differs(seen, m):
                    raise UpdaterError(f"the other half reports fw {seen}, the image's manifest says fw "
                                       f"{m['fw_version']}: the update did not take")
                if seen != m["fw_version"]:
                    print(f"warning: the image's manifest says fw {m['fw_version']} {FW_NOTE}")
                if s["other_half_id"] != PRESENCE_MATCH:
                    print("note: the halves now run different releases, which is not supported (V): the "
                          "lights show the rainbow, then red after 60 s, until it is updated too (use pair)")
                return 0
            time.sleep(0.5)
        print("warning: the other half did not answer within 10 s. A new release may only talk to a matching half: "
              "update the half with USB too (pair). If it still does not answer, plug USB into it and copy the .uf2.")
        return 4

    def abort(self):
        if self.nonce is not None:
            try:
                st, _ = self.nonce_op(ABORT)
                print(f"ABORT: {status_name(st)}", file=sys.stderr)
            except Exception as exc:  # the board may be gone
                print(f"ABORT failed: {exc}", file=sys.stderr)


def reopen(serial, timeout):
    """The board again after it reset (USB comes back with the new firmware)."""
    deadline = time.monotonic() + timeout
    while True:
        try:
            return Device(serial)
        except (RuntimeError, OSError, IOError):
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def pair_images(images, board):
    """[(path, manifest, hand)] for the other half, then this one: each image by its
    hand, an either-hand image (hand 2) for a half with none of its own; one
    either-hand image alone serves both."""
    if board not in HANDS:
        raise UpdaterError("pair: this half has no side yet; set it first (Sval 0x33, SIDE)")
    other = [(p, m) for p, m in images if m["hand"] == board ^ 1]
    this = [(p, m) for p, m in images if m["hand"] == board]
    either = [(p, m) for p, m in images if m["hand"] == HAND_EITHER]
    if len(images) == 1 and either:
        either = either * 2
    if not other and either:
        other = [either.pop(0)]
    if not this and either:
        this = [either.pop(0)]
    if len(other) != 1 or len(this) != 1 or either:
        raise UpdaterError(f"pair needs one image for each half; this half is {HANDS.get(board, board)}, the images are for " + ", ".join(HAND_NAMES.get(m["hand"], str(m["hand"])) for _, m in images))
    return [(other[0][0], other[0][1], board ^ 1), (this[0][0], this[0][1], board)]


def with_half(args, half):
    """A copy of args that names the half a session updates."""
    a = copy.copy(args)
    a.half = half
    return a


def pair(dev, args, paths, sessions):
    """Both halves: the other one through this one, then this one over USB; then presence must match (V)."""
    images = []
    for p in paths:
        manifest, _ = make_update.parse_svup(Path(p).read_bytes())
        images.append((Path(p), make_update.parse_manifest(manifest)))
    board = decode_info(*dev.op(INFO))["hand_id"]
    halves = pair_images(images, board)
    other, this = [halves[0][:2]], [halves[1][:2]]
    # Both images are checked against both halves before anything is sent:
    # a refusal of the second image after the first half committed would
    # leave the halves on different releases.
    deadline = time.monotonic() + 5
    while True:
        s = decode_status(*dev.op(STATUS, hand=board))
        if s["other_half_id"] in (PRESENCE_MATCH, PRESENCE_VERSION) or time.monotonic() >= deadline:
            break
        time.sleep(0.25)
    if s["other_half_id"] not in (PRESENCE_MATCH, PRESENCE_VERSION):
        raise UpdaterError(f"pair: the other half: presence {s['other_half']}; it must be connected and run an "
                           "SVAL_UPDATER build")
    st, r = dev.op(INFO, hand=board ^ 1)
    if r[0] == 0xFF:
        raise UpdaterError(f"pair: the other half does not answer INFO ({status_name(st)})")
    infos = {board: decode_info(*dev.op(INFO)), board ^ 1: decode_info(st, r)}
    check_protocol(infos[board])
    for p, m, h in halves:
        why = image_refusal(m, infos[h], other=h != board)
        if why:
            raise UpdaterError(f"pair: {p.name} would be refused by the {HANDS[h]} half ({why}); nothing was sent")
    # The hands match by construction; another pointing device is a warning (D33), asked once.
    confirm_mismatches([f"{p.name}: {w}" for p, m, h in halves for w in image_mismatches(m, infos[h])], args)
    one_approval = infos[board]["protocol"] >= 4  # D41
    if infos[board]["no_chord"]:
        pass  # a no-chord dev build (D43): nothing to say
    elif one_approval:
        print("pair: the chord is made once, on this half (the one with USB): it approves both images")
    else:
        print("pair: the chord is made twice, both times on this half (the one with USB): once for each image")
    print(f"== 1/2: the other ({HANDS[board ^ 1]}) half, {other[0][0].name} ==")
    # Each session is told its half: an either-hand image (hand 2) names neither,
    # and the image's own hand would otherwise pick this half for both.
    sessions.append(Session(dev, with_half(args, "other")))
    this_manifest = make_update.parse_svup(this[0][0].read_bytes())[0] if one_approval else None
    rc = sessions[-1].run(other[0][0], confirm_other=False, confirmed=True, pair_with=this_manifest)
    if rc != 0:
        return rc
    if one_approval:
        first = sessions[-1]
        # The held approval waits for this half's install (at least 90 s after
        # the relay is DONE). A renewed client ID moves it with REBIND first.
        if args.rebind_every and time.monotonic() - first.renewed >= args.rebind_every:
            old = dev.client
            dev.bootstrap()
            first.need("REBIND (the held approval)", dev.op(REBIND, struct.pack("<I", first.pair_nonce), hand=board), OK)
            print(f"  renewed client ID {old.hex()} -> {dev.client.hex()}; the approval moved with REBIND")
        s = decode_status(*dev.op(STATUS, hand=board))
        if not s["approval_held"]:
            raise UpdaterError(f"pair: the other ({HANDS[board ^ 1]}) half is installed, but the approval for this half is "
                               f"gone; the halves now differ. Finish this half with: update {this[0][0].name} (one chord)")
    # Straight on, without waiting to hear from the other half: it may not be
    # able to talk to this half's old firmware (V).
    print(f"== 2/2: this ({HANDS[board]}) half, {this[0][0].name} ==")
    sessions.append(Session(dev, with_half(args, "usb")))
    rc = sessions[-1].run(this[0][0], confirmed=True, by_pair=one_approval)
    if rc != 0:
        return rc
    if one_approval:
        print(f"pair: this half was approved by the pair (ARM flags {sessions[-1].arm_flags:#x})")
    serial = dev.serial
    dev.close()
    print("waiting for the board to come back")
    dev2 = reopen(serial, 30)
    try:
        t0 = time.monotonic()
        deadline = t0 + 10
        s = None
        while time.monotonic() < deadline:
            try:
                s = decode_status(*dev2.op(STATUS, hand=board))
            except (RuntimeError, OSError, IOError):
                # Opened while the board was still resetting: open it again.
                dev2.close()
                dev2 = reopen(serial, max(1, deadline - time.monotonic()))
                continue
            if s["other_half_id"] == PRESENCE_MATCH:
                break
            time.sleep(0.5)
        if s is None or s["other_half_id"] != PRESENCE_MATCH:
            raise UpdaterError(f"presence did not show the same release on both halves within 10 s "
                               f"({s['other_half'] if s else 'no STATUS'}); if the other half does not answer, "
                               "plug USB into it and copy the .uf2")
        mine = decode_info(*dev2.op(INFO))["fw_version"]
        st, r = dev2.op(INFO, hand=board ^ 1)
        theirs = decode_info(st, r)["fw_version"] if r[0] != 0xFF else None
        print(f"pair: presence {s['other_half']} after {time.monotonic() - t0:.1f} s; this half fw {mine}, "
              f"the other half fw {theirs}")
        if (theirs is None or fw_differs(mine, this[0][1]) or fw_differs(theirs, other[0][1])):
            raise UpdaterError(f"pair: the manifests say fw {this[0][1]['fw_version']} (this half) and "
                               f"{other[0][1]['fw_version']} (the other half), the halves report {mine} and {theirs}")
        if mine != this[0][1]["fw_version"] or theirs != other[0][1]["fw_version"]:
            print(f"warning: the manifests say fw {this[0][1]['fw_version']} and {other[0][1]['fw_version']} {FW_NOTE}")
        return 0
    finally:
        dev2.close()


def reset_settings(dev, args):
    """SETTINGS_RESET (protocol 4, D42): both halves (or this one) back to their build's defaults."""
    info = decode_info(*dev.op(INFO))
    board = info["hand_id"]
    if info["protocol"] < 4:
        raise UpdaterError(f"reset-settings needs updater protocol 4; the board speaks {info['protocol']}")
    scope = 0 if args.this_half else 1
    blank = bool(getattr(args, "blank", False))
    st, r = dev.op(SETTINGS_RESET, b"RSET" + bytes([scope, 1 if blank else 0]), hand=board)
    done = r[0]
    halves = [n for b, n in ((2, f"the other ({HANDS[board ^ 1]}) half"), (1, f"this ({info['hand']}) half")) if done & b]
    if st != OK:
        why = {
            5: "the updater is busy (a session, a held approval, or a reset already pending)",
            2: "the other half does not answer (connected, and on updater protocol 4?" + (" A blank board also needs a build from after D44 there" if blank else "") + ")",
            1: "a blank board needs a firmware build from after D44" if blank else ""
        }.get(st, "")
        raise UpdaterError(f"SETTINGS_RESET: {status_name(st)}" + (f": {why}" if why else "") + (f"; already reset: {', '.join(halves)}" if halves else "; nothing changed"))
    what = "reset to a blank board (every key KC_NO)" if blank else "reset to defaults"
    print(f"SETTINGS_RESET: OK, {' and '.join(halves)} {what} on the next boot; waiting for the board")
    serial = dev.serial
    t0 = time.monotonic()
    dev.close()
    time.sleep(1.0)  # both halves reset within 500 ms of the reply
    dev2 = reopen(serial, 30)
    try:
        deadline = time.monotonic() + 15
        while True:
            try:
                flags = storage_flags(dev2)
                break
            except (RuntimeError, OSError, IOError):
                if time.monotonic() >= deadline:
                    raise
                dev2.close()
                time.sleep(0.5)
                dev2 = reopen(serial, max(1, deadline - time.monotonic()))
        back_s = time.monotonic() - t0
        s = decode_status(*dev2.op(STATUS, hand=board))
        while scope == 1 and s["other_half_id"] == PRESENCE_NONE and time.monotonic() < deadline:
            time.sleep(0.25)
            s = decode_status(*dev2.op(STATUS, hand=board))
        print(f"back after {back_s:.1f} s; storage notice: {storage_text(flags)}; the other half: {s['other_half']}")
        if not flags & 4:
            raise UpdaterError("the board did not report the settings reset (GET_INFO byte 18 bit 2)")
        if blank:
            layers, km = keymap_bytes(dev2)
            set_keys = sum(1 for i in range(0, len(km), 2) if km[i] or km[i + 1])
            print(f"keymap: {layers} layers, {len(km) // 2} keys, {set_keys} not KC_NO")
            if set_keys:
                raise UpdaterError(f"the board is not blank: {set_keys} keys are not KC_NO")
        if args.ack:
            dev2.sval(SVAL_STORAGE_RESET_CLEAR)
            flags = storage_flags(dev2)
            print(f"0x2A sent; storage notice now: {storage_text(flags)}")
            if flags & 4:
                raise UpdaterError("0x2A did not clear the notice")
        return 0
    finally:
        dev2.close()


def storage(dev, args):
    flags = storage_flags(dev)
    print(f"storage notice: {storage_text(flags)} (byte 18 = {flags:#04x})")
    if args.ack:
        dev.sval(SVAL_STORAGE_RESET_CLEAR)
        flags = storage_flags(dev)
        print(f"0x2A sent; storage notice now: {storage_text(flags)} (byte 18 = {flags:#04x})")
    return 0


class Rejects:
    """M1 #6, the protocol rows: each malformed or misdirected request must get
    its expected status and leave the state as it was (or latch ERROR, for an
    overrun). Two sessions, each confirmed with the chord; nothing is committed."""
    def __init__(self, dev, path):
        self.dev = dev
        manifest, self.image = make_update.parse_svup(Path(path).read_bytes())
        self.blob = manifest
        self.crc_body = make_update.crc32_mpeg2(self.image[0x100:])
        self.passed = self.failed = 0

    def state(self):
        return decode_status(*self.dev.op(STATUS, hand=self.hand))

    def expect(self, what, got, want, state=None):
        st, r = got
        s = self.state()
        ok = st == want and (state is None or s["state_id"] == state)
        self.passed += ok
        self.failed += not ok
        print(f"  {'PASS' if ok else 'FAIL'}  {what}: {status_name(st)}, state {s['state']}" + ("" if ok else f" (expected {status_name(want)}" + ("" if state is None else f", {state_name(state)}") + ")"))
        return r

    def load(self, client):
        for off in range(0, len(self.blob), self.mp):
            piece = self.blob[off:off + self.mp]
            st, _ = self.dev.op(MANIFEST, bytes([off, len(piece)]) + piece, hand=self.hand, client=client)
            if st != OK:
                raise UpdaterError(f"MANIFEST: {status_name(st)}")

    def chunk(self, client, off, n, data=None):
        data = self.image[off:off + n] if data is None else data
        return self.dev.op(CHUNK, struct.pack("<I", off)[:3] + bytes([n]) + bytes(data).ljust(n, b"\0"), hand=self.hand, client=client)

    def send_image(self, client, end):
        off = 0
        while off < end:
            n = min(self.cp, end - off)
            st, _ = self.chunk(client, off, n)
            if st != OK:
                raise UpdaterError(f"CHUNK at {off}: {status_name(st)}")
            off += n

    def nonce_op(self, op, nonce, client, extra=b""):
        return self.dev.op(op, struct.pack("<I", nonce & 0xFFFFFFFF) + extra, hand=self.hand, client=client)

    def poll(self, until, what, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.state()
            if s["state_id"] == ST_ERROR:
                raise UpdaterError(f"{what}: {s['last_error']}")
            if until(s):
                return s
            time.sleep(0.2)
        raise UpdaterError(f"{what}: no progress in {timeout} s")

    def arm(self, a):
        r = self.expect("ARM from the loading client", self.dev.op(ARM, hand=self.hand, client=a), OK, ST_CONFIRM_WAIT)
        return struct.unpack_from("<I", r, 0)[0]

    def chord(self, a, nonce):
        print("  -> confirm on the keyboard now: hold Index South + Middle South for 1 s on this half")
        self.poll(lambda s: s["chord_made"], "waiting for the chord", 35)
        self.expect("BEGIN after the chord", self.nonce_op(BEGIN, nonce, a), ACCEPTED)
        self.poll(lambda s: s["state_id"] == ST_RECEIVING, "verifying the manifest and erasing", 120)

    def run(self):
        info = decode_info(*self.dev.op(INFO))
        self.hand = info["hand_id"]
        self.mp, self.cp = check_protocol(info)
        if info["state"] != "IDLE":
            raise UpdaterError(f"the board is in {info['state']}; ABORT first")
        a = self.dev.bootstrap()
        b = self.dev.new_client()

        print("session 1: unwrapped packets, a second client, wrong nonces, order")
        self.expect("unwrapped INFO", self.dev.unwrapped_op(INFO), OK, ST_IDLE)
        self.expect("unwrapped MANIFEST", self.dev.unwrapped_op(MANIFEST, bytes([0, self.mp]) + self.blob[:self.mp], hand=self.hand), INVALID, ST_IDLE)
        self.load(a)
        self.expect("MANIFEST from a second client", self.dev.op(MANIFEST, bytes([0, self.mp]) + self.blob[:self.mp], hand=self.hand, client=b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        self.expect("ARM from a second client", self.dev.op(ARM, hand=self.hand, client=b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        self.expect("unwrapped ARM", self.dev.unwrapped_op(ARM, hand=self.hand), INVALID, ST_MANIFEST_LOADING)
        self.expect("ABORT from a second client", self.nonce_op(ABORT, 0, b), OTHER_CLIENT, ST_MANIFEST_LOADING)
        nonce = self.arm(a)
        self.expect("REBIND with the wrong nonce", self.nonce_op(REBIND, nonce ^ 1, b), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN with the wrong nonce", self.nonce_op(BEGIN, nonce ^ 1, a), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN from a second client", self.nonce_op(BEGIN, nonce, b), OTHER_CLIENT, ST_CONFIRM_WAIT)
        self.expect("BEGIN before the chord", self.nonce_op(BEGIN, nonce, a), NOT_CONFIRMED, ST_CONFIRM_WAIT)
        self.chord(a, nonce)
        r = self.expect("CHUNK ahead of the next offset", self.chunk(a, self.cp, self.cp), OUT_OF_ORDER, ST_RECEIVING)
        if r[0] | r[1] << 8 | r[2] << 16:
            self.failed += 1
            print("  FAIL  ... and it did not report offset 0 as the next one")
        self.expect("END too early", self.nonce_op(END, nonce, a), OUT_OF_ORDER, ST_RECEIVING)
        self.expect("CHUNK from a second client", self.chunk(b, 0, self.cp), OTHER_CLIENT, ST_RECEIVING)
        self.expect("unwrapped CHUNK", self.dev.unwrapped_op(CHUNK, bytes([0, 0, 0, self.cp]) + self.image[:self.cp], hand=self.hand), INVALID, ST_RECEIVING)
        self.expect("ABORT with the wrong nonce", self.nonce_op(ABORT, nonce ^ 1, a), OTHER_CLIENT, ST_RECEIVING)
        self.expect("ABORT from a second client", self.nonce_op(ABORT, nonce, b), OTHER_CLIENT, ST_RECEIVING)
        self.send_image(a, len(self.image))
        self.expect("END", self.nonce_op(END, nonce, a), ACCEPTED)
        self.poll(lambda s: s["state_id"] == ST_VERIFIED, "verifying the image", 60)
        crc = struct.pack("<H", self.crc_body & 0xFFFF)
        bad_crc = struct.pack("<H", (self.crc_body ^ 1) & 0xFFFF)
        self.expect("COMMIT with the wrong CRC", self.nonce_op(COMMIT, nonce, a, bad_crc), BAD_HASH, ST_VERIFIED)
        self.expect("COMMIT from a second client", self.nonce_op(COMMIT, nonce, b, crc), OTHER_CLIENT, ST_VERIFIED)
        self.expect("ABORT from the session", self.nonce_op(ABORT, nonce, a), OK, ST_IDLE)

        print("session 2: overrun")
        self.load(a)
        nonce = self.arm(a)
        self.chord(a, nonce)
        # all but the last byte, then 2 bytes from there: one more than is left
        self.send_image(a, len(self.image) - 1)
        self.expect("CHUNK past the end (overrun)", self.chunk(a, len(self.image) - 1, 2, self.image[-1:] + b"\0"), OVERRUN, ST_ERROR)
        print("  -> the LEDs should now blink red for 30 s (error latched)")
        self.expect("ABORT from anyone while ERROR is latched", self.nonce_op(ABORT, 0, b), OK, ST_IDLE)
        print(f"reject: {self.passed} passed, {self.failed} failed")
        return 0 if self.failed == 0 else 1


def main():
    ap = argparse.ArgumentParser(epilog=HID_HELP, description=__doc__.split("\n\n")[0])
    ap.add_argument("--serial", help="exact device serial from list")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list")
    inf = sub.add_parser("info")
    inf.add_argument("--other", action="store_true", help="the other half, over the split link (M2)")
    sub.add_parser("status")
    sub.add_parser("relay", help="the relay to the other half: phase and progress (M2)")
    dg = sub.add_parser("diag", help="stack never used since boot and the longest updater pass (M1 #13)")
    dg.add_argument("--clear", action="store_true", help="restart the longest-pass record after reading it")
    ab = sub.add_parser("abort", help="ABORT: clears a latched error from any client, or a session given its nonce")
    ab.add_argument("--nonce", type=lambda s: int(s, 0), default=0)
    rj = sub.add_parser("reject", help="the protocol rejection matrix (M1 #6); needs the chord twice, never commits")
    rj.add_argument("svup", type=Path, help="an image this half accepts")
    up = sub.add_parser("update")
    up.add_argument("svup", type=Path)
    up.add_argument("--half", choices=("usb", "other"), help="the half to update (default: the image's own hand picks it); for an MCU moved between hands")
    up.add_argument("--yes", action="store_true", help="flash an image built for another hand or pointing device without asking")
    up.add_argument("--delay", type=float, default=0.0, help="seconds between chunks")
    up.add_argument("--rebind-every", type=float, default=50.0, help="renew the client ID and REBIND every S seconds (0: never)")
    up.add_argument("--min-rebinds", type=int, default=0, help="fail before COMMIT unless at least N renewals happened (M1 #16)")
    up.add_argument("--no-commit", action="store_true", help="stop once the image is verified")
    up.add_argument("--keep", action="store_true", help="on failure, leave the session for inspection instead of ABORTing")
    up.add_argument("--halt", choices=sorted(HALT_POINTS, key=HALT_POINTS.get), help="test-hooks builds: stop the commit at this point for good (M1 #8-#10)")
    up.add_argument("--halt-unfed", action="store_true", help="with --halt: let the watchdog reset the board (M1 #9)")
    pr = sub.add_parser("pair", help="update both halves: the other one through this one, then this one (M2)")
    pr.add_argument("svup", type=Path, nargs="+", help="one image for each half, in any order, or one either-hand image for both")
    pr.add_argument("--yes", action="store_true", help="flash images built for another pointing device without asking")
    pr.add_argument("--delay", type=float, default=0.0, help="seconds between chunks")
    pr.add_argument("--rebind-every", type=float, default=50.0, help="renew the client ID and REBIND every S seconds (0: never)")
    pr.add_argument("--keep", action="store_true", help="on failure, leave the session for inspection instead of ABORTing")
    rs = sub.add_parser("reset-settings", help="protocol 4: every stored setting on both halves back to defaults (D42)")
    rs.add_argument("--this-half", action="store_true", help="only the half with USB")
    rs.add_argument("--blank", action="store_true", help="a blank board: then every key on every layer KC_NO (D44)")
    rs.add_argument("--ack", action="store_true", help="afterwards send 0x2A, which clears the storage notice")
    sg = sub.add_parser("storage", help="the storage notice (Sval GET_INFO byte 18)")
    sg.add_argument("--ack", action="store_true", help="send 0x2A, which clears bit 2 (settings reset)")
    args = ap.parse_args()

    if args.command == "list":
        print(json.dumps([{k: d.get(k) for k in ("serial_number", "product_string", "vendor_id", "product_id")} for d in devices()], indent=2))
        return 0
    if args.command == "pair":
        args.min_rebinds, args.no_commit, args.halt, args.halt_unfed = 0, False, None, False
    if args.command == "update" and args.halt_unfed and not args.halt:
        ap.error("--halt-unfed needs --halt")
    if args.command == "update" and args.delay >= 30:
        print("warning: --delay of 30 s or more lets the session time out", file=sys.stderr)
    dev = Device(args.serial)
    try:
        if args.command == "info" and not args.other:
            info = decode_info(*dev.op(INFO))
            # INFO page 1 (M3): the version string. Firmware from before M3
            # ignores the page and answers page 0, whose first byte is the state.
            st1, r1 = dev.op(INFO, bytes([1]))
            if st1 == OK and r1[0] == 1 and struct.unpack_from("<I", r1, 1)[0] == info["fw_version"]:
                info["version"] = r1[5:21].rstrip(bytes(1)).decode("ascii", "replace")
            print(json.dumps(info, indent=2))
            return 0
        hand = decode_info(*dev.op(INFO))["hand_id"]
        if args.command == "info":
            st, r = dev.op(INFO, hand=hand ^ 1)
            print(json.dumps(decode_info(st, r) if r[0] != 0xFF else dict(status=status_name(st), answer="none"), indent=2))
            return 0 if st == OK else 1
        if args.command == "relay":
            print(json.dumps(decode_relay(*dev.op(RELAY, hand=hand)), indent=2))
            return 0
        if args.command == "status":
            print(json.dumps(decode_status(*dev.op(STATUS, hand=hand)), indent=2))
            return 0
        if args.command == "diag":
            print(json.dumps(decode_diag(*dev.op(DIAG, bytes([1 if args.clear else 0]), hand=hand)), indent=2))
            return 0
        if args.command == "abort":
            st, r = dev.op(ABORT, struct.pack("<I", args.nonce), hand=hand)
            print(f"ABORT: {status_name(st)}, state {state_name(r[0])}")
            return 0 if st == OK else 1
        if args.command in ("reset-settings", "storage"):
            try:
                return reset_settings(dev, args) if args.command == "reset-settings" else storage(dev, args)
            except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 1
        if args.command == "reject":
            try:
                return Rejects(dev, args.svup).run()
            except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                return 1
        if args.command == "pair":
            sessions = []
            try:
                return pair(dev, args, args.svup, sessions)
            except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                if sessions and not args.keep:
                    sessions[-1].abort()
                return 1
        session = Session(dev, args)
        try:
            return session.run(args.svup)
        except (UpdaterError, ClientIdExpired, RuntimeError) as exc:
            print(f"error: {exc}", file=sys.stderr)
            if not args.keep:
                session.abort()
            return 1
        except KeyboardInterrupt:
            if not args.keep:
                session.abort()
            raise
    finally:
        dev.close()


if __name__ == "__main__":
    sys.exit(main())
