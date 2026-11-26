"""Host tests for the Auto Mouse threshold (keyboards/svalboard/automouse_threshold.h).

Mouse motion and native touchpad motion add into one accumulator; both must
count in the mouse path's unit. The old_* functions below are the two paths
as they were before (svalboard 0a0892cd3b): the mouse path must trigger
exactly as it did, the pad path now on the mouse's scale. Activation Decay is
at least 10 ms, and a stored 0 reads as the 70 ms default.

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

/* The paths before (svalboard 0a0892cd3b). decay is in milliseconds, 0 never. */
static int32_t old_acc;
static uint16_t old_last;
static bool old_mouse(uint16_t now, uint16_t decay, int32_t counts, uint16_t dpi, uint16_t threshold) {
    if (counts <= 0) return false;
    int32_t units = (counts * 800 * 16) / dpi;
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

/* The paths now, gated as keymap_support.c gates them. decay is the stored setting. */
static int32_t acc;
static uint16_t last;
static bool new_mouse(uint16_t now, uint8_t decay, int32_t counts, uint16_t dpi, uint16_t threshold) {
    if (counts <= 0) return false;
    return sval_automouse_accumulate(&acc, &last, now, decay, sval_automouse_units(counts, dpi), threshold);
}
static bool new_pad(uint16_t now, uint8_t decay, int32_t counts, uint16_t threshold) {
    if (counts <= 0) return false;
    return sval_automouse_accumulate(&acc, &last, now, decay, sval_automouse_units(counts, PAD_CPI), threshold);
}
static void reset(void) {
    acc = old_acc = 0;
    last = old_last = 0;
}

/* Mouse reports until the layer turns on (n when it never does). */
static int mouse_steps(const int32_t *c, const uint16_t *t, int n, uint8_t decay, uint16_t dpi, uint16_t threshold, bool old) {
    reset();
    for (int i = 0; i < n; i++)
        if (old ? old_mouse(t[i], decay * 10, c[i], dpi, threshold) : new_mouse(t[i], decay, c[i], dpi, threshold)) return i;
    return n;
}
/* Equal pad reports of `counts`, 1 ms apart, until the layer turns on (0: never in 1000). */
static int pad_reports(int32_t counts, uint16_t threshold, bool old) {
    reset();
    for (int i = 1; i <= 1000; i++)
        if (old ? old_pad(i, 70, counts, threshold) : new_pad(i, 7, counts, threshold)) return i;
    return 0;
}

int main(void) {
    /* The mouse path is the reference: it triggers on exactly the same
       report as before, over random motion, thresholds, DPIs, decays and
       pauses, the timer wrapping too (decay 1 to 255: 0 now reads as 7). */
    srand(1);
    static const uint16_t dpis[] = {400, 800, 1000, 1200, 1600, 2400, 3200, 6400};
    for (int trial = 0; trial < 20000; trial++) {
        int32_t c[64];
        uint16_t t[64];
        uint16_t now = (uint16_t)rand();
        for (int i = 0; i < 64; i++) {
            c[i] = rand() % 6 == 0 ? 0 : rand() % (trial % 3 == 0 ? 60 : 8);
            now = (uint16_t)(now + rand() % 120);
            t[i] = now;
        }
        uint16_t threshold = rand() % 4 == 0 ? 0 : 1 + rand() % 200;
        uint8_t decay = 1 + rand() % (trial % 2 ? 20 : 255);
        uint16_t dpi = dpis[rand() % 8];
        assert(mouse_steps(c, t, 64, decay, dpi, threshold, true) == mouse_steps(c, t, 64, decay, dpi, threshold, false));
    }
    /* Mouse alone at 800 DPI, threshold 15: 14 counts stay off, 15 turn it on. */
    reset();
    assert(!new_mouse(0, 7, 14, 800, 15) && new_mouse(1, 7, 1, 800, 15));
    /* At 1600 DPI it takes 30 counts; at 3200 DPI single counts add up (4 units each). */
    reset();
    assert(!new_mouse(0, 7, 29, 1600, 15) && new_mouse(1, 7, 1, 1600, 15));
    reset();
    for (int i = 0; i < 59; i++) assert(!new_mouse(i, 7, 1, 3200, 15));
    assert(new_mouse(59, 7, 1, 3200, 15));
    /* Threshold 0: any motion, from either device. */
    reset();
    assert(new_mouse(0, 7, 1, 800, 0));
    reset();
    assert(new_pad(0, 7, 1, 0));
    /* No DPI reads as no motion rather than dividing by zero. */
    assert(sval_automouse_units(5, 0) == 0);

    /* The pad on the mouse's scale: 1210 counts are an inch, the same
       800 * 16 units as an inch of ball at any DPI. */
    assert(sval_automouse_units(PAD_CPI, PAD_CPI) == sval_automouse_units(800, 800));
    assert(sval_automouse_units(PAD_CPI, PAD_CPI) == sval_automouse_units(1600, 1600));
    assert(sval_automouse_units(1, PAD_CPI) == 10 && sval_automouse_units(2, PAD_CPI) == 21);
    /* One quick stroke, threshold 15: 23 counts as before (22 stay off). */
    reset();
    assert(!new_pad(0, 7, 22, 15));
    reset();
    assert(new_pad(0, 7, 23, 15));
    assert(pad_reports(23, 15, true) == 1 && pad_reports(23, 15, false) == 1);
    /* Slow strokes no longer lose the remainder of every report: 1-count
       reports never counted before and now turn it on after 24 (24 counts);
       2-count reports took 15 (30 counts) and now take 12 (24 counts);
       3-count reports took 15 (45 counts) and now take 8 (24 counts). */
    assert(pad_reports(1, 15, true) == 0 && pad_reports(1, 15, false) == 24);
    assert(pad_reports(2, 15, true) == 15 && pad_reports(2, 15, false) == 12);
    assert(pad_reports(3, 15, true) == 15 && pad_reports(3, 15, false) == 8);
    /* The pad never needs more motion than before, and always gets there. */
    for (uint16_t th = 1; th <= 100; th++)
        for (int32_t n = 1; n <= 40; n++) {
            int o = pad_reports(n, th, true), w = pad_reports(n, th, false);
            assert(w > 0 && (o == 0 || w <= o));
        }

    /* The failure: a ball nudge of 12 counts (800 DPI, under 15), then a
       2-count pad touch inside the decay window. It turned the layer on before. */
    reset();
    assert(!old_mouse(0, 200, 12, 800, 15));
    assert(old_pad(10, 200, 2, 15)); /* 192 + 1 >= 15: the bug */
    assert(!new_mouse(0, 20, 12, 800, 15));
    assert(!new_pad(10, 20, 2, 15)); /* 192 + 21 < 240 */
    assert(acc == 192 + 21);
    /* The other way: pad motion counted 1/16 toward the mouse check. */
    reset();
    assert(!old_pad(0, 200, 22, 15) && !old_mouse(10, 200, 1, 800, 15)); /* before: 14 + 16 < 240 */
    assert(!new_pad(0, 20, 22, 15));                                     /* 232 */
    assert(new_mouse(10, 20, 1, 800, 15));                               /* 248 */
    /* Half the threshold from each device, in either order, makes the
       threshold: 8 ball counts and 13 pad counts, threshold 16. */
    reset();
    assert(!new_mouse(0, 20, 8, 800, 16) && new_pad(10, 20, 13, 16));
    reset();
    assert(!new_pad(0, 20, 13, 16) && new_mouse(10, 20, 8, 800, 16));

    /* Activation Decay: at least 10 ms, a stored 0 reads as the 70 ms default. */
    assert(sval_automouse_decay_setting(0) == SVAL_AUTOMOUSE_DECAY_DEFAULT && SVAL_AUTOMOUSE_DECAY_DEFAULT == 7);
    assert(sval_automouse_decay_setting(1) == 1 && sval_automouse_decay_setting(255) == 255);
    assert(sval_automouse_decay_ms(0) == 70 && sval_automouse_decay_ms(1) == 10 && sval_automouse_decay_ms(255) == 2550);
    for (int d = 0; d < 256; d++) assert(sval_automouse_decay_ms((uint8_t)d) >= 10);
    /* A pause longer than the decay starts over; exactly the decay does not. */
    reset();
    assert(!new_mouse(0, 10, 10, 800, 15));
    assert(!new_pad(101, 10, 16, 15)); /* 101 ms > 100: the 160 dropped, 169 left */
    assert(acc == 169);
    assert(new_mouse(201, 10, 5, 800, 15)); /* exactly 100 ms: kept, 249 */
    /* Stored 0 forgets after 70 ms, where it once never forgot. */
    reset();
    assert(!new_mouse(0, 0, 10, 800, 15));
    assert(new_mouse(70, 0, 5, 800, 15)); /* 70 ms: kept */
    reset();
    assert(!new_mouse(0, 0, 10, 800, 15));
    assert(!new_mouse(71, 0, 5, 800, 15)); /* 71 ms: starts over */
    assert(acc == 80);
    reset();
    assert(!old_mouse(0, 0, 10, 800, 15) && old_mouse(60000, 0, 5, 800, 15)); /* before: never */
    /* The shortest decay, 10 ms. */
    reset();
    assert(!new_pad(0, 1, 16, 15) && new_mouse(10, 1, 5, 800, 15));
    reset();
    assert(!new_pad(0, 1, 16, 15) && !new_mouse(11, 1, 5, 800, 15));
    /* Measured across the timer's wrap. */
    reset();
    last = 65500;
    acc  = 160;
    assert(new_mouse(64, 10, 5, 800, 15)); /* 100 ms across the wrap: kept */
    reset();
    last = 65500;
    acc  = 160;
    assert(!new_mouse(65, 10, 5, 800, 15) && acc == 80); /* 101 ms: starts over */
    puts("OK");
    return 0;
}
'''


class AutoMouseThreshold(unittest.TestCase):
    def test_one_unit_and_decay_floor(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(TEST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-fno-sanitize-recover=undefined', '-I', str(KB), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            result = subprocess.run([str(path / 'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
