"""Host tests for the Auto Mouse threshold (keyboards/svalboard/automouse_threshold.h).

Mouse motion and native touchpad motion add into one accumulator; both must
count in the same unit. The old_* functions below are the two paths as they
were before the shared unit, kept to show a single device still triggers at
exactly the same motion, and that the mixed case no longer triggers early.

Run: python3 -m unittest discover -s tests/sval_storage -p 'test_automouse_threshold.py' -v
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KB = ROOT / 'keyboards/svalboard'

TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "automouse_threshold.h"

#define PAD_CPI ((2048 * 254 + 215) / 430) /* SVAL_PTP_COUNTS_PER_INCH: 1210 */

/* The paths before the shared unit (svalboard 0a0892cd3b). */
static int32_t old_acc;
static uint16_t old_last;
static bool old_mouse(uint16_t now, uint16_t decay, int32_t counts, uint16_t dpi, uint16_t threshold) {
    int32_t units = counts > 0 ? (counts * 800 * 16) / dpi : 0;
    if (counts <= 0) return false;
    if (decay > 0 && (uint16_t)(now - old_last) > decay) old_acc = 0;
    old_last = now;
    old_acc += units;
    return threshold == 0 || old_acc >= (int32_t)threshold * 16;
}
static bool old_pad(uint16_t now, uint16_t decay, int32_t counts, uint16_t threshold) {
    int32_t movement = (counts * 800) / PAD_CPI;
    if (movement <= 0) return false;
    if (decay > 0 && (uint16_t)(now - old_last) > decay) old_acc = 0;
    old_last = now;
    old_acc += movement;
    return threshold == 0 || old_acc >= threshold;
}

/* The paths now, gated as keymap_support.c gates them. */
static int32_t acc;
static uint16_t last;
static bool new_mouse(uint16_t now, uint16_t decay, int32_t counts, uint16_t dpi, uint16_t threshold) {
    if (counts <= 0) return false;
    return sval_automouse_accumulate(&acc, &last, now, decay, sval_automouse_mouse_units(counts, dpi), threshold);
}
static bool new_pad(uint16_t now, uint16_t decay, int32_t counts, uint16_t threshold) {
    int32_t units = sval_automouse_pad_units(counts, PAD_CPI);
    if (units <= 0) return false;
    return sval_automouse_accumulate(&acc, &last, now, decay, units, threshold);
}
static void reset(void) { acc = old_acc = 0; last = old_last = 0; }

/* Motion reports until the layer turns on (or n when it never does). */
static int mouse_steps(const int32_t *c, int n, uint16_t dpi, uint16_t threshold, bool old) {
    reset();
    for (int i = 0; i < n; i++)
        if (old ? old_mouse(i, 0, c[i], dpi, threshold) : new_mouse(i, 0, c[i], dpi, threshold)) return i;
    return n;
}
static int pad_steps(const int32_t *c, int n, uint16_t decay, const uint16_t *t, uint16_t threshold, bool old) {
    reset();
    for (int i = 0; i < n; i++)
        if (old ? old_pad(t[i], decay, c[i], threshold) : new_pad(t[i], decay, c[i], threshold)) return i;
    return n;
}

int main(void) {
    /* Mouse alone at 800 DPI, threshold 15: 14 counts stay off, 15 turn it on. */
    reset();
    assert(!new_mouse(0, 0, 14, 800, 15));
    assert(new_mouse(1, 0, 1, 800, 15));
    /* At 1600 DPI it takes 30 counts. */
    reset();
    assert(!new_mouse(0, 0, 29, 1600, 15));
    assert(new_mouse(1, 0, 1, 1600, 15));
    /* Sub-count motion at high DPI still adds up (3200 DPI: 4 units a count). */
    reset();
    for (int i = 0; i < 59; i++) assert(!new_mouse(i, 0, 1, 3200, 15));
    assert(new_mouse(59, 0, 1, 3200, 15));
    /* Threshold 0: any motion. */
    reset();
    assert(new_mouse(0, 0, 1, 800, 0));
    reset();
    assert(new_pad(0, 0, 2, 0));
    /* No DPI reads as no motion rather than dividing by zero. */
    assert(sval_automouse_mouse_units(5, 0) == 0);

    /* Pad alone, threshold 15: 1210 counts are an inch, 800 reference counts.
       15 reference counts need 23 pad counts (23 * 800 / 1210 = 15). */
    reset();
    assert(sval_automouse_pad_units(1, PAD_CPI) == 0);
    assert(sval_automouse_pad_units(2, PAD_CPI) == 16);
    assert(sval_automouse_pad_units(1210, PAD_CPI) == 800 * 16);
    assert(!new_pad(0, 0, 22, 15));
    reset();
    assert(new_pad(0, 0, 23, 15));

    /* Each path alone triggers on exactly the same report as before, over
       random motion, thresholds and DPIs; the pad also with random decay gaps. */
    srand(1);
    static const uint16_t dpis[] = {400, 800, 1000, 1200, 1600, 2400, 3200, 6400};
    for (int trial = 0; trial < 20000; trial++) {
        int32_t c[64];
        uint16_t t[64];
        uint16_t now = (uint16_t)rand(); /* the timer wraps too */
        for (int i = 0; i < 64; i++) {
            c[i] = rand() % 6 == 0 ? 0 : rand() % (trial % 3 == 0 ? 60 : 8);
            now = (uint16_t)(now + rand() % 120);
            t[i] = now;
        }
        uint16_t threshold = rand() % 4 == 0 ? 0 : 1 + rand() % 200;
        uint16_t dpi = dpis[rand() % 8];
        uint16_t decay = rand() % 3 == 0 ? 0 : 10 * (1 + rand() % 20);
        assert(mouse_steps(c, 64, dpi, threshold, true) == mouse_steps(c, 64, dpi, threshold, false));
        assert(pad_steps(c, 64, decay, t, threshold, true) == pad_steps(c, 64, decay, t, threshold, false));
    }

    /* The failure: a ball nudge of 12 counts (800 DPI, under 15), then a
       2-count pad touch inside the decay window. It turned the layer on before. */
    reset();
    assert(!old_mouse(0, 200, 12, 800, 15));
    assert(old_pad(10, 200, 2, 15)); /* 192 + 1 >= 15: the bug */
    assert(!new_mouse(0, 200, 12, 800, 15));
    assert(!new_pad(10, 200, 2, 15)); /* 192 + 16 < 240 */
    assert(acc == 13 * 16);
    /* The other way: pad motion counted 1/16 toward the mouse check. 14
       reference counts of pad, then one ball count, makes 15: on. */
    reset();
    assert(!new_pad(0, 200, 22, 15)); /* 22 * 800 / 1210 = 14 */
    assert(new_mouse(10, 200, 1, 800, 15));
    assert(!old_pad(0, 200, 22, 15) && !old_mouse(10, 200, 1, 800, 15)); /* before: 14 + 16 < 240 */

    /* The same physical motion counts the same from either device: one inch. */
    assert(sval_automouse_pad_units(PAD_CPI, PAD_CPI) == sval_automouse_mouse_units(800, 800));
    assert(sval_automouse_pad_units(PAD_CPI, PAD_CPI) == sval_automouse_mouse_units(1600, 1600));
    /* Half the threshold from each device, in either order, makes the threshold. */
    /* Threshold 16: 8 ball counts (800 DPI) and 13 pad counts (8 reference counts). */
    reset();
    assert(!new_mouse(0, 200, 8, 800, 16));
    assert(new_pad(10, 200, 13, 16));
    reset();
    assert(!new_pad(0, 200, 13, 16));
    assert(new_mouse(10, 200, 8, 800, 16));

    /* Decay: a gap longer than decay starts over; exactly decay does not;
       decay 0 never does; it is measured across the timer's wrap. */
    reset();
    assert(!new_mouse(0, 100, 10, 800, 15));
    assert(!new_pad(101, 100, 16, 15)); /* over 100 ms: 10 dropped, 10 left */
    assert(acc == 10 * 16);
    assert(new_mouse(201, 100, 5, 800, 15)); /* exactly 100 ms: kept, 15 */
    reset();
    assert(!new_pad(0, 0, 16, 15));
    assert(new_mouse(60000, 0, 5, 800, 15)); /* no decay: 10 + 5 */
    reset();
    last = 65500;
    acc = 10 * 16;
    assert(new_mouse(50, 100, 5, 800, 15)); /* 86 ms across the wrap: kept */
    reset();
    last = 65500;
    acc = 10 * 16;
    assert(!new_pad(65, 100, 16, 15)); /* 101 ms across the wrap: starts over */
    assert(acc == 10 * 16);
    puts("OK");
    return 0;
}
'''


class AutoMouseThreshold(unittest.TestCase):
    def test_one_unit_for_mouse_and_pad(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(TEST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=undefined', '-I', str(KB), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
