#!/usr/bin/env python3
"""Host tests for the either-hand image in the updater tools.

    test_either_hand.py

make_update.py takes --kb svalboard/either and svalboard/trackball/pmw3389/either
(hand 2), and sval_update.py's pair sends one either-hand image to both halves,
still pairs per-hand images by hand, refuses a half with no side, and does not
warn that an either-hand image names "another hand".
"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "keyboards/svalboard/tools"
sys.path.insert(0, str(TOOLS))


def load(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


mu = load("make_update")
su = load("sval_update")
checks = failures = 0


def check(cond, what):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL: {what}", file=sys.stderr)


def raises(fn, exc):
    try:
        fn()
    except exc:
        return True
    return False


# make_update.py: the either targets.
check(mu.HAND["either"] == 2, "either is hand 2 (UPDATE_HAND_EITHER)")
check(mu.kb_fields("svalboard/either") == (2, mu.POINTING["auto"]), "svalboard/either: either hand, auto pointing")
check(mu.kb_fields("svalboard/trackball/pmw3389/either") == (2, mu.POINTING["pmw3389"]), "pmw3389/either")
check(mu.kb_fields("svalboard/left") == (0, mu.POINTING["auto"]), "svalboard/left unchanged")
check(mu.kb_fields("svalboard/trackball/pmw3389/right") == (1, mu.POINTING["pmw3389"]), "pmw3389/right unchanged")
check(raises(lambda: mu.kb_fields("svalboard/both"), mu.Refused), "an unknown last part is refused")


def manifest(hand):
    return dict(hand=hand, pointing_id=mu.POINTING["auto"])


L, R, E = manifest(0), manifest(1), manifest(2)

# pair: one either image serves both halves, from either side.
for board in (0, 1):
    halves = su.pair_images([("e.svup", E)], board)
    check([h for _, _, h in halves] == [board ^ 1, board], f"either image: the other half, then this one (board {board})")
    check(all(m is E for _, m, _ in halves), "the same file for both halves")
# Per-hand images still go by hand.
halves = su.pair_images([("l.svup", L), ("r.svup", R)], 1)
check([(p, h) for p, _, h in halves] == [("l.svup", 0), ("r.svup", 1)], "per-hand images by hand")
# An either image beside a per-hand one fills the half without its own.
halves = su.pair_images([("e.svup", E), ("r.svup", R)], 1)
check([(p, h) for p, _, h in halves] == [("e.svup", 0), ("r.svup", 1)], "either fills the other half")
# Refusals.
check(raises(lambda: su.pair_images([("e.svup", E)], 0xFF), su.UpdaterError), "a half with no side is refused")
check(raises(lambda: su.pair_images([("l.svup", L)], 1), su.UpdaterError), "one per-hand image is not a pair")
check(raises(lambda: su.pair_images([("e.svup", E), ("e2.svup", E), ("r.svup", R)], 1), su.UpdaterError), "a spare image is refused")
check(raises(lambda: su.pair_images([("l.svup", L), ("l2.svup", L)], 1), su.UpdaterError), "two images for one hand")

# No "built for the other hand" warning for an either image.
info = dict(hand_id=1, hand="right", pointing_id=mu.POINTING["auto"])
check(su.image_mismatches(E, info) == [], "no hand warning for an either image")
check(len(su.image_mismatches(L, info)) == 1, "a left image on the right half still warns")

print(f"test_either_hand: {checks} checks, {failures} failures")
sys.exit(1 if failures else 0)
