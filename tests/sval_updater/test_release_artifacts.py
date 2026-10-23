#!/usr/bin/env python3
"""Host tests for keyboards/svalboard/tools/check_release_artifacts.py (M3).

    test_release_artifacts.py OUT_DIR

Builds a synthetic release directory (for each of the 2 release builds: a
minimal ELF with one loadable segment at 0x10000000 and the pair UF2 from
make_update.py pair --release), checks that it passes, and that each kind of
substitution is refused.
"""
import contextlib
import importlib.util
import io
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))
import check_release_artifacts as cra  # noqa: E402
import make_update as mu  # noqa: E402

spec = importlib.util.spec_from_file_location("tmu", Path(__file__).parent / "test_make_update.py")
tmu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tmu)

VER = "vM3-test"
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def elf(flash, extra_ram=b""):
    """A 32-bit ARM ELF with a PT_LOAD for flash at 0x10000000 and, if given, one for
    .data loaded right after it in flash (paddr) but linked in RAM (vaddr)."""
    segs = [(0x10000000, 0x10000000, flash)]
    if extra_ram:
        segs.append((0x20000000, 0x10000000 + len(flash), extra_ram))
    phoff = 52
    off = phoff + 32 * len(segs)
    hdr = bytearray(b"\x7fELF\x01\x01\x01" + bytes(9))
    hdr += struct.pack("<HHIIIIIHHHHHH", 2, 40, 1, 0x100001F7, phoff, 0, 0x05000200, 52, 32, len(segs), 40, 0, 0)
    ph, body = b"", b""
    for vaddr, paddr, data in segs:
        ph += struct.pack("<8I", 1, off + len(body), vaddr, paddr, len(data), len(data), 5, 4)
        body += data
    return bytes(hdr) + ph + body


def image_for(kb, km, data_tail=b""):
    hand, pointing_id = mu.kb_fields(kb)
    body = bytearray(tmu.image(0x1800))
    rec = struct.pack(mu.BUILD_INFO_FMT, b"SVBI", 3, mu.BI_RELEASE, 0, mu.UPDATER_PROTO, 3, VER.encode().ljust(16, b"\0"), hand, pointing_id, mu.KEYMAP[km], 0)
    body[0x800:0x800 + len(rec)] = rec
    return bytes(body)


def make_pair(src, out, kb, km, raw=False):
    """make_update.py pair --release, as a release build job runs it."""
    return subprocess.run([sys.executable, "-I", str(TOOLS / "make_update.py"), "pair", str(src), *(["--raw"] if raw else []), "-o", str(out), "--kb", kb, "--keymap", km, "--release", "--expect-version", VER], capture_output=True, text=True)


def make_dir(d):
    d.mkdir(parents=True)
    for i, (kb, km) in enumerate(cra.RELEASE_BUILDS):
        n = cra.build_name(kb, km)
        flash = image_for(kb, km)
        data = bytes([i]) * 0x40 if i % 2 else b""  # one build with a .data segment
        img = flash + data
        img += b"\xff" * (-len(img) % 256)
        (d / f"{n}.elf").write_bytes(elf(flash, data))
        (d / "src.uf2").write_bytes(tmu.uf2(img))
        p = make_pair(d / "src.uf2", d / cra.published_name(km, VER), kb, km)
        (d / "src.uf2").unlink()
        check(p.returncode == 0, f"make_update.py pair --release {n}: {p.stdout}{p.stderr}")


def quiet_main(args):
    with contextlib.redirect_stdout(io.StringIO()):
        return cra.main(args)


def main():
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    good = out / "good"
    make_dir(good)
    hashes, problems = cra.check_dir(good, VER)
    check(cra.RELEASE_BUILDS == [("svalboard/either", "sval"), ("svalboard/either", "blank")], f"the release is one image for both halves, sval and blank (D34, D40): {cra.RELEASE_BUILDS}")
    check(all(mu.kb_fields(kb) == (mu.HAND["either"], mu.POINTING["auto"]) for kb, _ in cra.RELEASE_BUILDS), "release builds are either-hand and auto")
    check(sorted(p.name for p in good.iterdir()) == sorted(["svalboard_either_sval.elf", "svalboard_either_blank.elf", f"svalboard_sval_{VER}.uf2", f"svalboard_blank_{VER}.uf2"]), "two ELFs and two pair UF2s, no .svup")
    check(not problems and len(hashes) == 2, f"a clean directory passes: {problems}")
    check(quiet_main([str(good), "--version", VER]) == 0, "main passes a clean directory")
    check(quiet_main([str(good), "--version", "vOther"]) == 1, "another version refused")

    s, b = "svalboard_either_sval", "svalboard_either_blank"
    su, bu = cra.published_name("sval", VER), cra.published_name("blank", VER)

    def case(what, needle, edit):
        d = out / "case"
        if d.exists():
            shutil.rmtree(d)
        shutil.copytree(good, d)
        edit(d)
        _, probs = cra.check_dir(d, VER)
        check(any(needle in p for p in probs), f"{what}: refused with {needle!r}: {probs}")

    def flip(path, at):
        x = bytearray(path.read_bytes())
        x[at] ^= 1
        path.write_bytes(x)

    def repair(d, kb, km, edit_img=None):
        """The pair UF2 made again from an edited image, so its manifest matches the new image."""
        _, img = mu.parse_pair_uf2((d / cra.published_name(km, VER)).read_bytes())
        img = bytearray(img)
        if edit_img:
            edit_img(img)
        (d / "x.bin").write_bytes(img)
        p = make_pair(d / "x.bin", d / cra.published_name(km, VER), kb, km, raw=True)
        check(p.returncode == 0, f"re-made pair UF2: {p.stdout}{p.stderr}")
        (d / "x.bin").unlink()

    def flip_img(img):
        img[0x1000] ^= 1

    def replace_with(d, kb, km, as_km="sval"):
        """The sval release build's files swapped for another build's (a one-side, per-module or blank image)."""
        flash = image_for(kb, km)
        flash += b"\xff" * (-len(flash) % 256)
        (d / f"{s}.elf").write_bytes(elf(flash))
        (d / "x.uf2").write_bytes(tmu.uf2(flash))
        p = subprocess.run([sys.executable, "-I", str(TOOLS / "make_update.py"), "pair", str(d / "x.uf2"), "-o", str(d / su), "--kb", "svalboard/either", "--keymap", km, "--release", "--expect-version", VER, "--yes"], capture_output=True, text=True)
        (d / "x.uf2").unlink()
        return p

    case("a flipped image byte with a re-made manifest", "not the ELF's flash contents", lambda d: repair(d, "svalboard/either", "sval", flip_img))
    case("a flipped image byte", "BAD_HASH" if False else "SHA-512", lambda d: flip(d / su, 32 + 0x10))
    case("a damaged pair record", "CRC", lambda d: flip(d / su, (len((d / su).read_bytes()) // 512 - 16) * 512 + 32 + 50))
    case("another ELF", "not the ELF's flash contents", lambda d: shutil.copy(d / f"{b}.elf", d / f"{s}.elf"))
    case("a blank build as sval", "keymap_id 2", lambda d: [shutil.copy(d / f"{b}.elf", d / f"{s}.elf"), shutil.copy(d / bu, d / su)])
    case("an sval build as blank", "keymap_id 1", lambda d: [shutil.copy(d / f"{s}.elf", d / f"{b}.elf"), shutil.copy(d / su, d / bu)])
    case("a missing blank build", "missing", lambda d: (d / bu).unlink())
    case("a one-side image with an either manifest", "build-info record says hand 1", lambda d: replace_with(d, "svalboard/right", "sval"))
    case("a per-module build as the release", "pointing_id 3", lambda d: replace_with(d, "svalboard/trackball/pmw3389/right", "sval"))
    case("a per-side build's files", "unexpected file", lambda d: (d / "svalboard_right_sval.elf").write_bytes((d / f"{s}.elf").read_bytes()))
    case("a .svup beside them", "unexpected file", lambda d: (d / f"{s}_{VER}.svup").write_bytes(b"\0" * 200))
    case("a plain UF2 as the pair UF2", "sectors", lambda d: (d / su).write_bytes(tmu.uf2(mu.parse_pair_uf2((d / su).read_bytes())[1][:0x1700])))
    case("an extra artifact", "unexpected file", lambda d: shutil.copy(d / f"{s}.elf", d / "extra.elf"))
    case("a truncated pair UF2", "multiple of 512", lambda d: (d / su).write_bytes((d / su).read_bytes()[:-100]))

    # the ELF reader against a non-ELF and an ELF that does not start at XIP_BASE
    bad = out / "bad.elf"
    bad.write_bytes(b"not an elf")
    try:
        cra.elf_flash_image(bad)
        check(False, "a non-ELF is refused")
    except mu.Refused:
        check(True, "a non-ELF is refused")
    raw = bytearray(elf(b"\0" * 0x100))
    struct.pack_into("<I", raw, 52 + 12, 0x20000000)  # paddr of the one segment
    bad.write_bytes(raw)
    try:
        cra.elf_flash_image(bad)
        check(False, "an ELF outside flash is refused")
    except mu.Refused:
        check(True, "an ELF outside flash is refused")

    print(f"check_release_artifacts.py: {checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
