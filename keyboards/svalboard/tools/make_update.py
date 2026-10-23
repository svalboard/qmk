#!/usr/bin/env python3
"""Build and check Svalboard updates (.svup) from QMK .uf2 files.

A .svup is the 108-byte manifest, then the raw image the updater stages and
copies to flash offset 0. Nothing is signed: the manifest's SHA-512 ties it to
the image, and the chord on the keyboard confirms the manifest. The manifest
layout and the image rules match keyboards/svalboard/updater/update_manifest.h
and update_image.c; see docs/updater.md.

    make_update.py IN.uf2 -o OUT.svup --kb svalboard/right \\
        [--keymap sval] [--version-string S] [--fw-version N] [--yes]

Release CI adds --release, which also checks that the image is a release
updater build (SVAL_UPDATE_RELEASE) for a release keymap, with no test hooks,
keytest or host bootloader, and that its build-info record says the same
version and keymap as the manifest:

    make_update.py IN.uf2 -o OUT.svup --kb svalboard/right --keymap sval \\
        --release [--expect-version v3] [--expect-fw-version N]

    make_update.py verify FILE.svup [--build release|plain]

    make_update.py pair IN.uf2 -o PAIR.uf2 --kb svalboard/either [make's options] [--svup OUT.svup]
    make_update.py verify-pair PAIR.uf2 [--build release|plain] [--svup OUT.svup]

pair builds the one .uf2 that installs both halves (docs/pair-uf2.md): the
image, then the pair record that sends it to the other half at the next boot.
verify-pair checks one, and --svup extracts what Keybard sends.

checks a .svup offline the way a build of that kind would before erasing
anything: the manifest rules, the hash and the image structure.

fw_version and the version string default to the image's build-info record
(SVAL_FW_VERSION, SVAL_FW_VERSION_STRING), so a manifest always says what the
firmware reports. A --kb whose side or pointing device is not the image's
(including an any-module "auto" image labeled as one module, or the reverse)
is a warning, not a refusal (D33): the tool asks (y/N) before writing the
.svup, and --yes skips the question; with no answer (no terminal) it writes
nothing. Refuses, with exit status 2: a UF2 that is malformed, for
another chip family, not starting at 0x10000000, with gaps or overlaps, or
ending beyond 0x10160000; an image whose boot2 CRC, initial SP or reset vector
is wrong; and flag and field combinations the firmware would refuse anyway.
"""
import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

# ---- format (update_manifest.h) ----------------------------------------------------

MANIFEST_MAGIC = 0x50555653  # "SVUP"
MANIFEST_VERSION = 2  # 1: protocol 1, signed (never released); refused
UPDATER_PROTO = 4  # 4: 3 plus one approval for both halves and SETTINGS_RESET; 3: 64-byte raw HID reports (updater.c)
MANIFEST_FMT = "<IHBBBBHHBBIII16s64s"
MANIFEST_BYTES = 108
assert struct.calcsize(MANIFEST_FMT) == MANIFEST_BYTES

HAND = {"left": 0, "right": 1, "either": 2}  # either: one image for both halves (SVAL_EITHER_HAND)
POINTING = {"none": 0, "trackpoint": 1, "pmw3360": 2, "pmw3389": 3, "azoteq": 4, "auto": 5}
KEYMAP = {"sval": 1, "blank": 2}  # the release keymaps (D22)
FLAG_RELEASE, FLAG_DIAGNOSTIC = 0x0001, 0x0002
STORAGE_FORMAT = 2  # SVAL_UPDATE_STORAGE_FORMAT in keyboards/svalboard/config.h

# ---- build info (update_build_info.h) ----------------------------------------------

BUILD_INFO_MAGIC = b"SVBI\x03"  # magic, then info_ver 3
BUILD_INFO_FMT = "<4sBBBBI16sBBBB"  # magic, ver, flags, reserved0, proto, fw, version, hand, pointing_id, keymap_id, reserved
BUILD_INFO_BYTES = 32
BI_HOST_TEST = 0xFF  # hand and pointing_id of a host-test build
assert struct.calcsize(BUILD_INFO_FMT) == BUILD_INFO_BYTES
BI_RELEASE, BI_TEST_HOOKS, BI_KEYTEST, BI_HOST_BOOTLOADER = 0x01, 0x04, 0x08, 0x10
BI_NO_CHORD, BI_DEV_KEEP = 0x40, 0x80  # dev builds only
BI_RESERVED = 0x02 | 0x20  # protocol 1's key flags, retired
BI_NOT_IN_RELEASE = {BI_TEST_HOOKS: "test hooks", BI_KEYTEST: "SVAL_KEYTEST", BI_HOST_BOOTLOADER: "SVAL_HOST_BOOTLOADER", BI_NO_CHORD: "SVAL_UPDATE_NO_CHORD", BI_DEV_KEEP: "SVAL_DEV_KEEP_SETTINGS"}
VERSION_RE = re.compile(r"^[A-Za-z0-9._+-]{0,16}$")

# ---- image rules (update_image.c, config.h) ---------------------------------------

XIP_BASE = 0x10000000
MAX_IMAGE = 0x160000  # SVAL_UPDATE_MAX_IMAGE; also the linker cap (flash_reservation.ld)
IMAGE_MIN, IMAGE_ALIGN = 0x200, 0x100
SP_MIN, SP_MAX = 0x20000000, 0x20042000
RP2040_FAMILY = 0xE48BFF56
UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
UF2_FLAG_NOT_MAIN_FLASH, UF2_FLAG_FAMILY = 0x00000001, 0x00002000


class Refused(Exception):
    pass


def crc32_mpeg2(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def boot2_valid(page0):
    return len(page0) >= 256 and crc32_mpeg2(page0[:252]) == struct.unpack_from("<I", page0, 252)[0]


def image_problems(image):
    """The device's structure checks (update_image_head_check). Empty when fine."""
    problems = []
    n = len(image)
    if n < IMAGE_MIN or n % IMAGE_ALIGN:
        problems.append(f"length {n:#x} is not a multiple of 256 of at least {IMAGE_MIN:#x}")
    if n > MAX_IMAGE:
        problems.append(f"length {n:#x} is above the maximum {MAX_IMAGE:#x}")
    if n < 0x108:
        return problems + ["too short for a vector table"]
    if not boot2_valid(image):
        problems.append("boot2 CRC (CRC-32/MPEG-2 over bytes 0-251) does not match the word at 252")
    sp, reset = struct.unpack_from("<II", image, 0x100)
    if not SP_MIN <= sp <= SP_MAX:
        problems.append(f"initial SP {sp:#010x} is outside [{SP_MIN:#x}, {SP_MAX:#x}]")
    if not reset & 1 or not XIP_BASE + 0x100 <= reset < XIP_BASE + n:
        problems.append(f"reset vector {reset:#010x} is not odd and inside the image")
    return problems


def uf2_to_image(data):
    """Raw image from a UF2, refusing anything but one contiguous run at XIP_BASE."""
    if not data or len(data) % 512:
        raise Refused(f"UF2 length {len(data)} is not a positive multiple of 512")
    count = len(data) // 512
    image = bytearray()
    for i in range(count):
        block = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, block_no, num_blocks, family = struct.unpack_from("<8I", block, 0)
        (m_end,) = struct.unpack_from("<I", block, 508)
        if (m0, m1, m_end) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
            raise Refused(f"block {i}: bad UF2 magic")
        if flags & UF2_FLAG_NOT_MAIN_FLASH:
            raise Refused(f"block {i}: marked not-main-flash")
        if not flags & UF2_FLAG_FAMILY or family != RP2040_FAMILY:
            raise Refused(f"block {i}: family {family:#010x} is not RP2040 ({RP2040_FAMILY:#010x})")
        if size != 256:
            raise Refused(f"block {i}: payload {size} bytes, expected 256")
        if block_no != i or num_blocks != count:
            raise Refused(f"block {i}: numbered {block_no} of {num_blocks}, expected {i} of {count}")
        expected = XIP_BASE + 256 * i
        if addr != expected:
            what = "starts" if i == 0 else "has a gap or overlap"
            raise Refused(f"block {i}: address {addr:#010x}, expected {expected:#010x} (image {what})")
        image += block[32:32 + 256]
    if XIP_BASE + len(image) > XIP_BASE + MAX_IMAGE:
        raise Refused(f"image ends at {XIP_BASE + len(image):#010x}, beyond {XIP_BASE + MAX_IMAGE:#010x}")
    return bytes(image)


def build_info(image):
    """The image's build-info record (update_build_info.h) as a dict, or None if it
    has none (a build without the updater)."""
    hits = []
    at = image.find(BUILD_INFO_MAGIC)
    while at >= 0:
        hits.append(at)
        at = image.find(BUILD_INFO_MAGIC, at + 1)
    if not hits:
        for old in (1, 2):
            if image.find(b"SVBI" + bytes([old])) >= 0:
                raise Refused(f"the image has a version-{old} build-info record (an updater build from before "
                              "protocol 2): rebuild it")
        return None
    if len(hits) > 1:
        raise Refused(f"the image has {len(hits)} build-info records (SVBI), expected one")
    if hits[0] + BUILD_INFO_BYTES > len(image):
        raise Refused("the build-info record runs past the end of the image")
    _, ver, flags, reserved0, proto, fw, version, hand, pointing_id, keymap_id, reserved = struct.unpack_from(BUILD_INFO_FMT, image, hits[0])
    text = version.rstrip(b"\0")
    if b"\0" in text or not VERSION_RE.match(text.decode("latin-1")):
        raise Refused(f"the build-info record's version string {version!r} is malformed")
    return dict(offset=hits[0], flags=flags, reserved0=reserved0, updater_proto=proto, fw_version=fw, version=text.decode("ascii"), hand=hand, pointing_id=pointing_id, keymap_id=keymap_id, reserved=reserved)


def build_info_identity_problems(info, m, fields=("hand", "pointing_id", "keymap_id")):
    """Where the manifest's fields disagree with the image's build-info record (a
    record from a host-test build, 0xFF, says nothing)."""
    problems = []
    for field in fields:
        if info[field] != BI_HOST_TEST and info[field] != m[field]:
            problems.append(f"the manifest says {field} {m[field]}, the image was built with {info[field]}")
    return problems


# ---- manifest ---------------------------------------------------------------------------


def build_manifest(image, *, hand, pointing_id, keymap_id, flags, epoch, storage_format, fw_version, version):
    vbytes = version.encode("ascii")
    if len(vbytes) > 16 or b"\0" in vbytes:
        raise Refused("--version-string must be at most 16 ASCII characters")
    return struct.pack(MANIFEST_FMT, MANIFEST_MAGIC, MANIFEST_VERSION, 0, hand, pointing_id, keymap_id, flags, epoch, storage_format, UPDATER_PROTO, len(image), fw_version, 0, vbytes.ljust(16, b"\0"), hashlib.sha512(image).digest())


def parse_manifest(raw):
    f = struct.unpack(MANIFEST_FMT, raw[:MANIFEST_BYTES])
    names = ("magic", "manifest_ver", "reserved0", "hand", "pointing_id", "keymap_id", "flags", "security_epoch", "storage_format", "updater_proto", "image_len", "fw_version", "reserved", "version", "sha512")
    return dict(zip(names, f))


def parse_svup(data):
    """(manifest bytes, image) from a .svup, checking only its framing."""
    if len(data) < MANIFEST_BYTES:
        raise Refused(".svup is too short")
    manifest, image = data[:MANIFEST_BYTES], data[MANIFEST_BYTES:]
    m = parse_manifest(manifest)
    if m["magic"] == MANIFEST_MAGIC and m["manifest_ver"] == 1:
        raise Refused("a version-1 .svup (protocol 1, never released): rebuild it with this make_update.py")
    if m["image_len"] != len(image):
        raise Refused(".svup image length does not match its manifest")
    return manifest, image


def manifest_version(m):
    return m["version"].rstrip(b"\0").decode("latin-1")


def kb_fields(kb):
    """hand and pointing_id from a keyboard path such as svalboard/trackball/pmw3389/right."""
    parts = kb.strip("/").split("/")
    if not parts or parts[0] != "svalboard" or parts[-1] not in HAND:
        raise Refused(f"--kb {kb!r}: expected svalboard/.../left, svalboard/.../right or svalboard/.../either")
    middle = parts[1:-1]
    table = {(): "auto",
             ("trackpoint",): "trackpoint",
             ("trackball", "pmw3360"): "pmw3360",
             ("trackball", "pmw3389"): "pmw3389",
             ("azoteq",): "azoteq"}
    if tuple(middle) not in table:
        raise Refused(f"--kb {kb!r}: unknown variant")
    return HAND[parts[-1]], POINTING[table[tuple(middle)]]


def versions_for(info, args):
    """fw_version and version string for the manifest: the image's build-info record
    unless given. A given value that contradicts a set record is refused: the
    firmware reports the record's (INFO, split presence)."""
    fw, text = args.fw_version, args.version_string
    if info:
        if fw is not None and info["fw_version"] and fw != info["fw_version"]:
            raise Refused(f"--fw-version {fw} but the image reports {info['fw_version']} (SVAL_FW_VERSION)")
        if text is not None and info["version"] and text != info["version"]:
            raise Refused(f"--version-string {text!r} but the image reports {info['version']!r} (SVAL_FW_VERSION_STRING)")
        fw = info["fw_version"] if fw is None else fw
        text = info["version"] if text is None else text
    return (0 if fw is None else fw), ("" if text is None else text)


def keymap_id_for(info, args):
    """keymap_id (display only, D18): --keymap-id, else --keymap, else the image's
    record, else sval."""
    if args.keymap_id is not None:
        return args.keymap_id
    if args.keymap is not None:
        return KEYMAP.get(args.keymap, 0)
    if info and info["keymap_id"] != BI_HOST_TEST:
        return info["keymap_id"]
    return KEYMAP["sval"]


def manifest_from_args(image, args):
    """The manifest for IN and the options."""
    if args.kb:
        hand, pointing_id = kb_fields(args.kb)
    else:
        hand, pointing_id = None, None
    if args.hand is not None:
        hand = HAND[args.hand]
    if args.pointing is not None:
        pointing_id = POINTING[args.pointing]
    if hand is None or pointing_id is None:
        raise Refused("give --kb, or both --hand and --pointing")

    flags = (FLAG_RELEASE if args.release else 0) | (FLAG_DIAGNOSTIC if args.diagnostic else 0)
    if args.release and args.diagnostic:
        raise Refused("a RELEASE image cannot be DIAGNOSTIC")
    info = build_info(image)
    if info and info["flags"] & (BI_NO_CHORD | BI_DEV_KEEP) and not args.release:
        # D43: a dev build is always DIAGNOSTIC, so a release board refuses it however it was packed.
        flags |= FLAG_DIAGNOSTIC
    keymap_id = keymap_id_for(info, args)
    if args.release and keymap_id not in KEYMAP.values():
        raise Refused(f"RELEASE images are release keymaps only ({', '.join(KEYMAP)})")
    fw_version, version = versions_for(info, args)
    if not 0 <= fw_version <= 0xFFFFFFFF or not 0 <= args.epoch <= 0xFFFF or not 0 <= args.storage_format <= 0xFF:
        raise Refused("--fw-version, --epoch or --storage-format out of range")
    manifest = build_manifest(image, hand=hand, pointing_id=pointing_id, keymap_id=keymap_id, flags=flags, epoch=args.epoch, storage_format=args.storage_format, fw_version=fw_version, version=version)
    return manifest


def identity_warnings(manifest, image):
    """Where --kb names another side or pointing device than the image's build-info
    record (D33: a warning the user confirms, never a refusal)."""
    info = build_info(image)
    if not info:
        return []
    m = parse_manifest(manifest)
    names = {"hand": {v: k for k, v in HAND.items()}, "pointing_id": {v: k for k, v in POINTING.items()}}
    out = []
    for field, what in (("hand", "side"), ("pointing_id", "pointing device")):
        if info[field] != BI_HOST_TEST and info[field] != m[field]:
            said = names[field].get(m[field], m[field])
            built = names[field].get(info[field], info[field])
            out.append(f"the manifest says {what} {said} ({field} {m[field]}), but the image was built for {built} "
                       f"({field} {info[field]}): --kb is not this image's")
    return out


def ask(prompt):
    return input(prompt)


def confirm(warnings, yes):
    """Print each warning and ask (y/N) unless --yes; a no (or no answer) refuses."""
    if not warnings:
        return
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    if yes:
        print("--yes: going ahead", file=sys.stderr)
        return
    try:
        answer = ask("Write the .svup anyway? [y/N] ")
    except EOFError:
        answer = ""
    if answer.strip().lower() not in ("y", "yes"):
        raise Refused("not confirmed: nothing was written (--yes skips the question)")


def read_image(args):
    data = Path(args.input).read_bytes()
    image = data if args.raw else uf2_to_image(data)
    if args.raw and (len(image) > MAX_IMAGE):
        raise Refused(f"image is {len(image):#x} bytes, above {MAX_IMAGE:#x}")
    return image


def check_structure(image, allow_bad):
    problems = image_problems(image)
    if problems:
        if not allow_bad:
            raise Refused("; ".join(problems))
        print("warning: building a structurally bad TEST image: " + "; ".join(problems), file=sys.stderr)


def describe(out, manifest, image):
    m = parse_manifest(manifest)
    return (
        f"{out}: {len(image)} B image, hand {m['hand']}, pointing {m['pointing_id']}, "
        f"flags {m['flags']:#x}, version {manifest_version(m)!r} ({m['fw_version']}), "
        f"manifest sha512 {hashlib.sha512(manifest).hexdigest()[:8]}, image sha512 {m['sha512'].hex()[:16]}"
    )


def release_image_problems(manifest, image):
    """Why a manifest and image are not a release update, or [] (make --release)."""
    m = parse_manifest(manifest)
    problems = []
    if m["magic"] != MANIFEST_MAGIC or m["manifest_ver"] != MANIFEST_VERSION or m["reserved0"] or m["reserved"]:
        problems.append(f"not a version-{MANIFEST_VERSION} manifest (magic, version or reserved fields)")
    if m["flags"] != FLAG_RELEASE:
        problems.append(f"flags {m['flags']:#x}: a release image has RELEASE set and DIAGNOSTIC (and all else) clear")
    if m["keymap_id"] not in KEYMAP.values():
        problems.append(f"keymap_id {m['keymap_id']} is not a release keymap ({', '.join(KEYMAP)})")
    if m["hand"] not in HAND.values() or m["pointing_id"] not in POINTING.values():
        problems.append(f"hand {m['hand']} or pointing_id {m['pointing_id']} is unknown")
    if m["updater_proto"] != UPDATER_PROTO or m["storage_format"] < STORAGE_FORMAT:
        problems.append(f"updater_proto {m['updater_proto']} or storage_format {m['storage_format']} is not this release's")
    if m["image_len"] != len(image):
        problems.append(f"image_len {m['image_len']} but the image is {len(image)} B")
    elif hashlib.sha512(image).digest() != m["sha512"]:
        problems.append("the image's SHA-512 does not match the manifest")
    problems += image_problems(image)
    try:
        info = build_info(image)
    except Refused as e:
        info, problems = None, problems + [str(e)]
    if info is None:
        problems.append("the image has no build-info record: not an updater build")
    else:
        if not info["flags"] & BI_RELEASE:
            problems.append("the image is not a release updater build (SVAL_UPDATE_RELEASE)")
        for bit, what in BI_NOT_IN_RELEASE.items():
            if info["flags"] & bit:
                problems.append(f"the image has {what}")
        if info["flags"] & BI_RESERVED or info["reserved0"] or info["reserved"]:
            problems.append(f"the build-info record has reserved bits set (flags {info['flags']:#x})")
        if info["updater_proto"] != UPDATER_PROTO:
            problems.append(f"the image's updater protocol is {info['updater_proto']}, expected {UPDATER_PROTO}")
        if info["fw_version"] != m["fw_version"] or info["version"] != manifest_version(m):
            problems.append(f"the manifest says {manifest_version(m)!r} ({m['fw_version']}), the image reports "
                            f"{info['version']!r} ({info['fw_version']})")
        if BI_HOST_TEST in (info["hand"], info["pointing_id"]) or info["keymap_id"] not in KEYMAP.values():
            problems.append(f"the image's build-info record has hand {info['hand']}, pointing_id {info['pointing_id']}, "
                            f"keymap_id {info['keymap_id']}: not a release keymap build for one half")
        # Side and pointing device: a confirmed warning in make (D33); the release
        # lint checks them against each build's name.
        problems += build_info_identity_problems(info, m, ("keymap_id",))
    return problems


# ---- make -----------------------------------------------------------------------------


def make(args):
    image = read_image(args)
    manifest = manifest_from_args(image, args)
    if args.release and args.unsafe_allow_bad_image:
        raise Refused("--unsafe-allow-bad-image is for test images, never --release")
    check_structure(image, args.unsafe_allow_bad_image)
    if args.release:
        m = parse_manifest(manifest)
        problems = release_image_problems(manifest, image)
        if args.expect_version is not None and manifest_version(m) != args.expect_version:
            problems.append(f"version {manifest_version(m)!r}, expected {args.expect_version!r}")
        if args.expect_fw_version is not None and m["fw_version"] != args.expect_fw_version:
            problems.append(f"fw_version {m['fw_version']}, expected {args.expect_fw_version}")
        if problems:
            raise Refused("not a release update: " + "; ".join(problems))
    elif args.expect_version is not None or args.expect_fw_version is not None:
        raise Refused("--expect-* go with --release")
    confirm(identity_warnings(manifest, image), args.yes)
    out = Path(args.output)
    out.write_bytes(manifest + image)
    print(describe(out, manifest, image))


# ---- verify ----------------------------------------------------------------------------------


def verify_svup(args):
    """A .svup checked as a build of the given kind would before it erases anything."""
    manifest, image = parse_svup(Path(args.svup).read_bytes())
    _verify_parts(manifest, image, args)


def _verify_parts(manifest, image, args):
    m = parse_manifest(manifest)
    release_build = args.build == "release"
    problems = []
    if m["magic"] != MANIFEST_MAGIC:
        problems.append("INVALID: bad magic")
    elif m["manifest_ver"] != MANIFEST_VERSION:
        problems.append(f"UNSUPPORTED: manifest version {m['manifest_ver']}, this build takes {MANIFEST_VERSION}")
    elif m["reserved0"] or m["reserved"] or m["flags"] & ~(FLAG_RELEASE | FLAG_DIAGNOSTIC):
        problems.append("INVALID: a reserved field or flag is set")
    if release_build and m["flags"] & FLAG_DIAGNOSTIC:
        problems.append("BAD_IMAGE: a DIAGNOSTIC image on a release build")
    if hashlib.sha512(image).digest() != m["sha512"]:
        problems.append("BAD_HASH: the image does not match the manifest's SHA-512")
    problems += ["BAD_IMAGE: " + p for p in image_problems(image)]
    info = build_info(image)
    what = (f"hand {m['hand']}, pointing {m['pointing_id']}, flags {m['flags']:#x}, "
            f"version {manifest_version(m)!r} ({m['fw_version']}), image {len(image)} B, "
            f"build info {'none' if info is None else hex(info['flags'])}")
    if problems:
        raise Refused(f"{args.svup}: a {args.build} build refuses it ({what}): " + "; ".join(problems))
    print(f"{args.svup}: a {args.build} build accepts it ({what})")


# ---- pair UF2 (docs/pair-uf2.md) -------------------------------------------------------------
#
# One .uf2 that installs both halves: the image (blocks from 0x10000000, padded
# with 0xFF to whole 4 KiB sectors), then the pair record sector at
# PAIR_RECORD_OFFSET (update_pair.h). Every block is an ordinary RP2040 flash
# block, numbered in order, so block numbers 16k..16k+15 always fill one
# sector: the RP2040 boot ROM erases the sector of a block's address when the
# first block of its group of 16 block numbers arrives (virtual_disk.c), so a
# layout with a partial sector before a jump would leave pages unerased.

PAIR_RECORD_OFFSET = 0x270000  # UPDATE_PAIR_RECORD_OFFSET
PAIR_MAGIC = 0x55505653  # "SVPU"
PAIR_RECORD_VERSION = 1
PAIR_KIND_RELAY_SELF = 1
PAIR_RECORD_FMT = "<IHHB3sI108s"  # then crc u32
PAIR_RECORD_BYTES = 128
SECTOR, PAGE = 4096, 256
PAGES_PER_SECTOR = SECTOR // PAGE
assert struct.calcsize(PAIR_RECORD_FMT) + 4 == PAIR_RECORD_BYTES
# Flash the file may never write: settings, staging slot, side records,
# build reset record, identity (update_flash.c asserts the firmware side).
PROTECTED = {"settings": (0x160000, 0x260000), "staging slot": (0x800000, 0x960000), "side records": (0xFFB000, 0xFFD000), "build reset record": (0xFFD000, 0xFFE000), "identity": (0xFFE000, 0x1000000)}


def pair_record(manifest):
    head = struct.pack(PAIR_RECORD_FMT, PAIR_MAGIC, PAIR_RECORD_VERSION, PAIR_RECORD_BYTES, PAIR_KIND_RELAY_SELF, b"\0\0\0", 0, manifest)
    return head + struct.pack("<I", crc32_mpeg2(head))


def parse_pair_record(raw):
    """The record's fields, refusing anything the firmware would (update_pair_record_check)."""
    magic, version, length, kind, reserved, image_offset, manifest = struct.unpack_from(PAIR_RECORD_FMT, raw)
    (crc,) = struct.unpack_from("<I", raw, PAIR_RECORD_BYTES - 4)
    if magic != PAIR_MAGIC:
        raise Refused("no pair record (magic)")
    if crc32_mpeg2(raw[:PAIR_RECORD_BYTES - 4]) != crc:
        raise Refused("the pair record's CRC does not match")
    if version != PAIR_RECORD_VERSION:
        raise Refused(f"pair record version {version}, this tool knows {PAIR_RECORD_VERSION}")
    if length != PAIR_RECORD_BYTES or kind != PAIR_KIND_RELAY_SELF or image_offset != 0 or reserved != b"\0\0\0":
        raise Refused("the pair record's length, kind, image offset or reserved bytes are wrong")
    return manifest


def uf2_block(addr, data, block_no, num_blocks):
    return (struct.pack("<8I", UF2_MAGIC0, UF2_MAGIC1, UF2_FLAG_FAMILY, addr, 256, block_no, num_blocks, RP2040_FAMILY) + data.ljust(476, b"\0") + struct.pack("<I", UF2_MAGIC_END))


def pair_uf2_bytes(manifest, image):
    """The pair UF2 for a manifest and its image."""
    m = parse_manifest(manifest)
    if m["hand"] != HAND["either"]:
        raise Refused(f"a pair UF2 needs an image for either hand (hand 2), not hand {m['hand']}: the other half "
                      "gets the same image")
    if m["image_len"] != len(image) or hashlib.sha512(image).digest() != m["sha512"]:
        raise Refused("the manifest does not describe this image")
    padded = image + b"\xff" * (-len(image) % SECTOR)
    record_sector = pair_record(manifest).ljust(SECTOR, b"\xff")
    pages = [(XIP_BASE + i, padded[i:i + PAGE]) for i in range(0, len(padded), PAGE)]
    pages += [(XIP_BASE + PAIR_RECORD_OFFSET + i, record_sector[i:i + PAGE]) for i in range(0, SECTOR, PAGE)]
    return b"".join(uf2_block(addr, data, n, len(pages)) for n, (addr, data) in enumerate(pages))


def parse_pair_uf2(data):
    """(manifest, image) from a pair UF2, refusing any layout other than pair_uf2_bytes's."""
    if not data or len(data) % 512:
        raise Refused(f"UF2 length {len(data)} is not a positive multiple of 512")
    count = len(data) // 512
    if count % PAGES_PER_SECTOR or count < 2 * PAGES_PER_SECTOR:
        raise Refused(f"{count} blocks: a pair UF2 is whole 4 KiB sectors, the image's and the record's")
    flash = {}
    for i in range(count):
        block = data[i * 512:(i + 1) * 512]
        m0, m1, flags, addr, size, block_no, num_blocks, family = struct.unpack_from("<8I", block, 0)
        (m_end,) = struct.unpack_from("<I", block, 508)
        if (m0, m1, m_end) != (UF2_MAGIC0, UF2_MAGIC1, UF2_MAGIC_END):
            raise Refused(f"block {i}: bad UF2 magic")
        if flags != UF2_FLAG_FAMILY or family != RP2040_FAMILY or size != 256:
            raise Refused(f"block {i}: flags {flags:#x}, family {family:#010x}, payload {size}: not an RP2040 flash block")
        if block_no != i or num_blocks != count:
            raise Refused(f"block {i}: numbered {block_no} of {num_blocks}, expected {i} of {count}")
        flash[addr - XIP_BASE] = block[32:32 + 256]
    img_pages = count - PAGES_PER_SECTOR
    expect = [i * PAGE for i in range(img_pages)] + [PAIR_RECORD_OFFSET + i * PAGE for i in range(PAGES_PER_SECTOR)]
    got = [struct.unpack_from("<I", data, i * 512 + 12)[0] - XIP_BASE for i in range(count)]
    if got != expect:
        raise Refused("block addresses are not the image from 0x10000000 in whole sectors, then the pair record sector")
    if img_pages * PAGE > MAX_IMAGE:
        raise Refused(f"the image area ends at {XIP_BASE + img_pages * PAGE:#010x}, beyond {XIP_BASE + MAX_IMAGE:#010x}")
    sector = b"".join(flash[PAIR_RECORD_OFFSET + i * PAGE] for i in range(PAGES_PER_SECTOR))
    if sector[PAIR_RECORD_BYTES:] != b"\xff" * (SECTOR - PAIR_RECORD_BYTES):
        raise Refused("the pair record sector is not 0xFF after the record")
    manifest = parse_pair_record(sector)
    m = parse_manifest(manifest)
    area = b"".join(flash[i * PAGE] for i in range(img_pages))
    n = m["image_len"]
    if not IMAGE_MIN <= n <= len(area) or len(area) - n >= SECTOR or n % IMAGE_ALIGN:
        raise Refused(f"the manifest's image_len {n:#x} does not fill the {len(area):#x}-byte image area")
    if area[n:] != b"\xff" * (len(area) - n):
        raise Refused("the image area is not 0xFF after the image")
    return manifest, area[:n]


def pair_writes(data):
    """[(lo, hi)] flash offset ranges a pair UF2's blocks write (whole sectors, as the boot ROM erases)."""
    count = len(data) // 512
    addrs = sorted(struct.unpack_from("<I", data, i * 512 + 12)[0] - XIP_BASE for i in range(count))
    out = []
    for a in addrs:
        lo = a - a % SECTOR
        if out and out[-1][1] >= lo:
            out[-1] = (out[-1][0], max(out[-1][1], lo + SECTOR))
        else:
            out.append((lo, lo + SECTOR))
    return out


def pair_layout_problems(data):
    """Where a pair UF2 would write protected flash, or []."""
    problems = []
    for lo, hi in pair_writes(data):
        for what, (plo, phi) in PROTECTED.items():
            if lo < phi and plo < hi:
                problems.append(f"it writes [{lo:#x}, {hi:#x}), which overlaps the {what} [{plo:#x}, {phi:#x})")
    return problems


def make_pair(args):
    """make_update.py pair IN.uf2 -o OUT.uf2 --kb svalboard/either [make's options]"""
    image = read_image(args)
    manifest = manifest_from_args(image, args)
    if args.release and args.unsafe_allow_bad_image:
        raise Refused("--unsafe-allow-bad-image is for test images, never --release")
    check_structure(image, args.unsafe_allow_bad_image)
    if args.release:
        m = parse_manifest(manifest)
        problems = release_image_problems(manifest, image)
        if args.expect_version is not None and manifest_version(m) != args.expect_version:
            problems.append(f"version {manifest_version(m)!r}, expected {args.expect_version!r}")
        if args.expect_fw_version is not None and m["fw_version"] != args.expect_fw_version:
            problems.append(f"fw_version {m['fw_version']}, expected {args.expect_fw_version}")
        if problems:
            raise Refused("not a release update: " + "; ".join(problems))
    elif args.expect_version is not None or args.expect_fw_version is not None:
        raise Refused("--expect-* go with --release")
    confirm(identity_warnings(manifest, image), args.yes)
    data = pair_uf2_bytes(manifest, image)
    problems = pair_layout_problems(data)
    if problems:
        raise Refused("; ".join(problems))
    out = Path(args.output)
    out.write_bytes(data)
    if args.svup:
        Path(args.svup).write_bytes(manifest + image)
    print(describe(out, manifest, image) + f", {len(data) // 512} UF2 blocks, pair record at {XIP_BASE + PAIR_RECORD_OFFSET:#010x}")


def verify_pair(args):
    """A pair UF2 checked as the boot relay and a build of the given kind would, and
    its layout: whole sectors, nothing outside the image area and the record sector."""
    data = Path(args.uf2).read_bytes()
    manifest, image = parse_pair_uf2(data)
    m = parse_manifest(manifest)
    problems = pair_layout_problems(data)
    if m["hand"] != HAND["either"]:
        problems.append(f"INVALID: hand {m['hand']}: the boot relay sends this image to the other half, so it must be for either hand")
    tmp = argparse.Namespace(svup=args.uf2, build=args.build)
    try:
        _verify_parts(manifest, image, tmp)
    except Refused as e:
        problems.append(str(e))
    if args.svup:
        Path(args.svup).write_bytes(manifest + image)
    if problems:
        raise Refused(f"{args.uf2}: " + "; ".join(problems))
    print(f"{args.uf2}: a pair UF2 a {args.build} build accepts ({len(image)} B image, version {manifest_version(m)!r} "
          f"({m['fw_version']}), writes " + ", ".join(f"[{lo:#x}, {hi:#x})" for lo, hi in pair_writes(data)) + ")")


# ---- command line ---------------------------------------------------------------------------


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv and argv[0] == "verify":
            ap = argparse.ArgumentParser(prog="make_update.py verify", description=verify_svup.__doc__, allow_abbrev=False)
            ap.add_argument("svup")
            ap.add_argument("--build", choices=("release", "plain"), default="release", help="release: SVAL_UPDATE_RELEASE (no DIAGNOSTIC images); plain: SVAL_UPDATER alone")
            verify_svup(ap.parse_args(argv[1:]))
            return 0
        if argv and argv[0] == "verify-pair":
            ap = argparse.ArgumentParser(prog="make_update.py verify-pair", description=verify_pair.__doc__, allow_abbrev=False)
            ap.add_argument("uf2")
            ap.add_argument("--build", choices=("release", "plain"), default="release")
            ap.add_argument("--svup", help="also write the manifest and image as a .svup (what Keybard sends)")
            verify_pair(ap.parse_args(argv[1:]))
            return 0
        pair = bool(argv) and argv[0] == "pair"
        if pair:
            argv = argv[1:]
        ap = argparse.ArgumentParser(prog="make_update.py pair" if pair else None, description=__doc__.split("\n\n")[0], allow_abbrev=False)
        ap.add_argument("input", help="QMK .uf2 (or raw .bin with --raw)")
        ap.add_argument("-o", "--output", required=True, help="output .svup (pair: the pair .uf2)")
        if pair:
            ap.add_argument("--svup", help="also write the .svup of the same manifest and image")
        ap.add_argument("--raw", action="store_true", help="input is a raw image starting at 0x10000000")
        ap.add_argument("--kb", help="keyboard path, e.g. svalboard/trackball/pmw3389/right (sets hand and pointing)")
        ap.add_argument("--hand", choices=sorted(HAND))
        ap.add_argument("--pointing", choices=sorted(POINTING))
        ap.add_argument("--keymap", help="keymap name, display only (default: the image's record, else sval)")
        ap.add_argument("--keymap-id", type=int)
        ap.add_argument("--version-string", help="display version, at most 16 ASCII characters "
                        "(default: the image's SVAL_FW_VERSION_STRING)")
        ap.add_argument("--fw-version", type=lambda s: int(s, 0), help="numeric version, u32 (default: the image's SVAL_FW_VERSION)")
        ap.add_argument("--epoch", type=lambda s: int(s, 0), default=0, help="security epoch (u16)")
        ap.add_argument("--storage-format", type=int, default=STORAGE_FORMAT)
        ap.add_argument("--release", action="store_true", help="set flags.RELEASE and run the release checks")
        ap.add_argument("--diagnostic", action="store_true", help="set flags.DIAGNOSTIC (scanlab/keytest/host-bootloader images)")
        ap.add_argument("--expect-version", help="--release: refuse unless the version string is this (the tag)")
        ap.add_argument("--expect-fw-version", type=int, help="--release: refuse unless fw_version is this")
        ap.add_argument("--yes", action="store_true", help="write the .svup without asking when --kb names another side or pointing device than the image")
        ap.add_argument("--unsafe-allow-bad-image", action="store_true", help="build an image that fails the structure checks (for rejection tests)")
        (make_pair if pair else make)(ap.parse_args(argv))
    except Refused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
