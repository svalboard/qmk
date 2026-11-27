#!/usr/bin/env python3
"""Check the release build artifacts before anything is published (M3).

    check_release_artifacts.py DIR --version TAG

DIR holds what release CI's build jobs uploaded, and nothing else: for each of
the 2 pair builds (svalboard/either with the sval and the blank keymap: one
image for both halves, D34, D40) exactly NAME.elf and the pair UF2
svalboard_KEYMAP_TAG.uf2 (docs/pair-uf2.md); for each of the 2 one-side builds
(svalboard/either with the sval keymap and SVAL_BUILD_SIDE left or right)
exactly NAME.elf and the plain UF2 svalboard_sval_SIDE_TAG.uf2. For each pair
build this checks, and exits 1 on any failure:

  - the pair UF2 has exactly the pair layout (make_update.parse_pair_uf2): the
    image from 0x10000000 in whole 4 KiB sectors, then the pair record sector,
    every block an ordinary RP2040 flash block numbered in order; it writes
    no protected flash (settings, staging slot, side records, build reset
    record, identity);
  - its image is the ELF's flash contents: the ELF's loadable segments at
    their load addresses, as objcopy -O binary writes them, padded with 0xFF
    to a whole 256-byte UF2 block. So the image published is the one
    check_release_elf.py and check_split_tables.py linted;
  - the record's manifest has the image's length and SHA-512, version TAG,
    hand 2 (either), the auto pointing build and the keymap of NAME, and the
    release checks of make_update.py --release pass (RELEASE set, reserved
    fields 0, a release updater build with no test hooks, keytest or host
    bootloader);
  - the image's build-info record names the same hand, pointing device and
    keymap.

For each one-side build:

  - the UF2 is one contiguous run of ordinary RP2040 flash blocks from
    0x10000000, numbered in order, inside the firmware area
    (make_update.uf2_to_image): no pair record, so nothing relays it to the
    other half, and no protected flash;
  - its image is the ELF's flash contents, as above;
  - its build-info record is a release updater build (the release checks of
    make_update.py --release, on a manifest made for it) for that side (hand
    0 or 1), the auto pointing build, the sval keymap and version TAG, with
    the same fw_version as the pair builds.

It prints "NAME=SHA512" for each image, for the run's log.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import make_update as mu  # noqa: E402

# The release builds: release.yml's build matrix. One image for both halves
# (svalboard/either: the side comes from the thumb strap or the stored side
# record), the auto build with every pointing driver (pointing_id auto, D34);
# keymaps sval and blank, the test image with no key bindings (D40).
RELEASE_KEYMAPS = ("sval", "blank")
RELEASE_BUILDS = [("svalboard/either", km) for km in RELEASE_KEYMAPS]
# The one-side builds (SVAL_BUILD_SIDE, keyboards/svalboard/side.h): the same
# image with its side fixed, so copying the file onto a half sets that half's
# side with no host software. Published as plain UF2 files for one half each.
ONE_SIDE_BUILDS = [("svalboard/either", "sval", side) for side in ("left", "right")]
PT_LOAD = 1


def build_name(kb, keymap, side=None):
    return kb.replace("/", "_") + "_" + (f"{side}_" if side else "") + keymap


def published_name(keymap, version, side=None):
    """A file as a release publishes it: the pair UF2 for both halves, one per
    keymap, or a one-side build's plain UF2."""
    return f"svalboard_{keymap}_{side}_{version}.uf2" if side else f"svalboard_{keymap}_{version}.uf2"


def elf_flash_image(path):
    """The flash contents of an ELF as objcopy -O binary writes them (loadable
    segments with file contents, at their physical load addresses, gaps zero),
    padded with 0xFF to a whole 256-byte block as the UF2 conversion does."""
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        raise mu.Refused(f"{path}: not a 32-bit little-endian ELF")
    e_phoff, = struct.unpack_from("<I", data, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    segs = []
    for i in range(e_phnum):
        p_type, p_offset, _vaddr, p_paddr, p_filesz, _memsz, _flags, _align = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        if p_type == PT_LOAD and p_filesz:
            segs.append((p_paddr, data[p_offset:p_offset + p_filesz]))
    if not segs:
        raise mu.Refused(f"{path}: no loadable segments")
    base = min(a for a, _ in segs)
    if base != mu.XIP_BASE:
        raise mu.Refused(f"{path}: flash contents start at {base:#x}, not {mu.XIP_BASE:#x}")
    out = bytearray(max(a + len(b) for a, b in segs) - base)
    for a, b in segs:
        out[a - base:a - base + len(b)] = b
    out += b"\xff" * (-len(out) % 256)
    return bytes(out)


def check_dir(d, version):
    """({name: sha512 hex}, [problems]) for the artifacts in d."""
    d = Path(d)
    problems, hashes = [], {}
    want = {build_name(kb, km): (kb, km) for kb, km in RELEASE_BUILDS}
    one_side = {build_name(kb, km, side): (kb, km, side) for kb, km, side in ONE_SIDE_BUILDS}
    expected_files = ({f"{n}.elf" for n in list(want) + list(one_side)} | {published_name(km, version) for _, km in want.values()} | {published_name(km, version, side) for _, km, side in one_side.values()})
    present = {p.name for p in d.iterdir()} if d.is_dir() else set()
    for extra in sorted(present - expected_files):
        problems.append(f"unexpected file {extra}: the directory must hold only the {len(want) + len(one_side)} "
                        "builds' ELFs, the pair UF2s and the one-side UF2s")
    for missing in sorted(expected_files - present):
        problems.append(f"missing {missing}")
    if problems:
        return hashes, problems
    fw_versions = set()
    for n, (kb, km) in sorted(want.items()):

        def bad(msg):
            problems.append(f"{n}: {msg}")

        try:
            data = (d / published_name(km, version)).read_bytes()
            raw, img = mu.parse_pair_uf2(data)
            for p in mu.pair_layout_problems(data):
                bad(p)
            if elf_flash_image(d / f"{n}.elf") != img:
                bad("the pair UF2's image is not the ELF's flash contents (objcopy -O binary)")
            m = mu.parse_manifest(raw)
            hand, pointing_id = mu.kb_fields(kb)
            ident = dict(hand=hand, pointing_id=pointing_id, keymap_id=mu.KEYMAP[km])
            if m["image_len"] != len(img) or m["sha512"] != hashlib.sha512(img).digest():
                bad("the manifest's image length or SHA-512 is not the image's")
            if mu.manifest_version(m) != version:
                bad(f"the manifest's version is {mu.manifest_version(m)!r}, not {version!r}")
            for k, v in ident.items():
                if m[k] != v:
                    bad(f"the manifest says {k} {m[k]}, {n} is {v}")
            info = mu.build_info(img)
            if info is None:
                bad("the image has no build-info record")
            else:
                for k, v in ident.items():
                    if info[k] != v:
                        bad(f"the image's build-info record says {k} {info[k]}, {n} is {v}")
            for p in mu.release_image_problems(raw, img):
                bad(p)
            hashes[n] = hashlib.sha512(img).hexdigest()
            fw_versions.add(m["fw_version"])
        except (mu.Refused, OSError, struct.error) as e:
            bad(str(e))
    for n, (kb, km, side) in sorted(one_side.items()):

        def bad(msg):
            problems.append(f"{n}: {msg}")

        try:
            img = mu.uf2_to_image((d / published_name(km, version, side)).read_bytes())
            if elf_flash_image(d / f"{n}.elf") != img:
                bad("the UF2's image is not the ELF's flash contents (objcopy -O binary)")
            for p in one_side_image_problems(img, kb, km, side, version, fw_versions):
                bad(p)
            hashes[n] = hashlib.sha512(img).hexdigest()
        except (mu.Refused, OSError, struct.error) as e:
            bad(str(e))
    return hashes, problems


def one_side_image_problems(img, kb, km, side, version, fw_versions=()):
    """Why a one-side build's image (from its plain UF2) is not that release file, or []."""
    _, pointing_id = mu.kb_fields(kb)
    ident = dict(hand=mu.HAND[side], pointing_id=pointing_id, keymap_id=mu.KEYMAP[km])
    info = mu.build_info(img)
    if info is None:
        return ["the image has no build-info record: not an updater build"]
    problems = [f"the image's build-info record says {k} {info[k]}, the {side} {km} build is {v}" for k, v in ident.items() if info[k] != v]
    if info["version"] != version:
        problems.append(f"the image's version is {info['version']!r}, not {version!r}")
    if fw_versions and info["fw_version"] not in fw_versions:
        problems.append(f"the image's fw_version is {info['fw_version']}, the pair builds' {sorted(fw_versions)}")
    # The release checks make_update.py --release runs, on the manifest a host would make for it.
    m = mu.build_manifest(img, flags=mu.FLAG_RELEASE, epoch=mu.SECURITY_EPOCH, storage_format=mu.STORAGE_FORMAT, fw_version=info["fw_version"], version=version, **ident)
    return problems + mu.release_image_problems(m, img)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dir")
    ap.add_argument("--version", required=True, help="the tag: the file names and the manifests' version")
    args = ap.parse_args(argv)
    hashes, problems = check_dir(args.dir, args.version)
    for p in problems:
        print(f"FAIL: {p}")
    if problems:
        return 1
    for n, h in sorted(hashes.items()):
        print(f"{n}={h}")
    print(f"ok: {len(hashes)} release builds: each pair UF2 has the pair layout and its ELF's flash contents, "
          f"each one-side UF2 is a plain image of its ELF; manifests and build-info records match the build "
          f"names; version {args.version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
