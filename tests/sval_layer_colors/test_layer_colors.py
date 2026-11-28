"""The layer-color callbacks of the sval and blank keymaps, compiled on the host.

The LEDs must show the stored color of the layer in effect: the highest of the
active layers and the default layer. A default layer set with PDF() or Sval 0x2B
counts, and no rgblight overlay layer may cover the stored color.
No device or flash is accessed.

    python3 -m unittest discover -s tests/sval_layer_colors -v
"""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
KEYMAPS = ROOT / 'keyboards/svalboard/keymaps'


def function(source, name):
    start = source.rfind('\n', 0, re.search(r'\b' + name + r'\(', source).start()) + 1
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint32_t layer_state_t;
#define RGBLIGHT_LAYERS 16
static layer_state_t layer_state, default_layer_state = 1;
static int shown = -1; // the layer whose stored color sval_set_active_layer shows
static uint32_t overlays; // rgblight overlay layers that are on
static void sval_set_active_layer(uint32_t layer, bool save) { (void)save; shown = (int)layer; }
static uint8_t get_highest_layer(layer_state_t s) { uint8_t h = 0; for (uint8_t i = 0; i < 32; i++) if (s & (1u << i)) h = i; return h; }
static bool layer_state_cmp(layer_state_t s, uint8_t layer) { return s == 0 ? layer == 0 : (s & (1u << layer)) != 0; }
static void rgblight_set_layer_state(uint8_t layer, bool on) { if (on) overlays |= 1u << layer; else overlays &= ~(1u << layer); }
%s
/* QMK's order: the _user callback runs, then the global is stored. */
static void set_default(layer_state_t s) { default_layer_state = default_layer_state_set_user(s); }
static void set_layers(layer_state_t s) { layer_state = layer_state_set_user(s); }
static int fails;
static void expect(const char *what, int want) {
    if (shown != want || overlays) { printf("FAIL %%s: shown %%d (want %%d), overlays 0x%%x\n", what, shown, want, overlays); fails++; }
}
int main(void) {
    (void)layer_state_cmp; (void)rgblight_set_layer_state;
    set_layers(0); set_default(1u << 0); expect("boot, default layer 0", 0);
    set_default(1u << 2); expect("default layer set to 2 (PDF(2) / Sval 0x2B)", 2);
    set_layers(1u << 3); expect("layer 3 held over default 2", 3);
    set_layers(0); expect("layer 3 released, default 2", 2);
    /* Keybard edits layer 2's color: svalboard.c repaints only when the layer is sval_active_layer. */
    set_layers(0); expect("color edit refresh on default 2", 2);
    set_default(1u << 0); expect("default back to 0", 0);
    set_layers(1u << 1); expect("layer 1 held", 1);
    set_layers(0); expect("layer 1 released", 0);
    return fails != 0;
}
'''


class LayerColors(unittest.TestCase):
    def check(self, keymap):
        source = (KEYMAPS / keymap / 'keymap.c').read_text()
        body = function(source, 'default_layer_state_set_user') + '\n' + function(source, 'layer_state_set_user')
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'colors.c').write_text(HARNESS % body)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(path / 'colors.c'), '-o', str(path / 'colors')], check=True)
            run = subprocess.run([str(path / 'colors')], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, f'{keymap}:\n{run.stdout}')
        # The overlays QMK draws over the base color hide the stored one on both LEDs.
        self.assertFalse('rgblight_layers =' in source, f'{keymap}: rgblight overlay layers cover the stored layer colors')

    def test_sval_keymap(self):
        self.check('sval')

    def test_blank_keymap(self):
        self.check('blank')


if __name__ == '__main__':
    unittest.main()
