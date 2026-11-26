"""Host tests for layer keys on the auto mouse layer (keyboards/svalboard/automouse_keys.c).

The classifier is the firmware's own; around it, a small model of QMK's key
lookup (the highest active layer whose key is not KC_TRNS, fixed at press)
and of process_record_kb's mouse layer handling, so each case reads as a
user's key presses.

Run: python3 -m unittest discover -s tests/sval_storage -v
"""
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KB = ROOT / 'keyboards/svalboard'

TEST = r'''
#include <assert.h>
#include <string.h>
#include "automouse_keys.h"
#include "keycodes.h"
#define ML 15
#define B(n) ((uint32_t)1 << (n))
#define MO_(l) (QK_MOMENTARY | (l))
#define TO_(l) (QK_TO | (l))
#define TG_(l) (QK_TOGGLE_LAYER | (l))
#define DF_(l) (QK_DEF_LAYER | (l))
#define PDF_(l) (QK_PERSISTENT_DEF_LAYER | (l))
#define OSL_(l) (QK_ONE_SHOT_LAYER | (l))
#define TT_(l) (QK_LAYER_TAP_TOGGLE | (l))
#define LM_(l, m) (QK_LAYER_MOD | ((l) << 5) | (m))
#define LT_(l, k) (QK_LAYER_TAP | ((l) << 8) | (k))

/* Positions: 0 the thumb key, 1 a key that is a mouse button on the mouse layer. */
enum { THUMB, BUTTON, POSITIONS };
static uint16_t keymap[16][POSITIONS];
static uint32_t layer_state, default_layer_state;
static bool     mouse_on;
static int      held;
static uint8_t  source[POSITIONS];

static void mouse_mode(bool on) {
    if (on) {
        layer_state |= B(ML);
        mouse_on = true;
    } else {
        layer_state &= ~B(ML);
        mouse_on = false;
        held     = 0;
    }
}

static uint8_t lookup(int pos) {
    uint32_t s = layer_state | default_layer_state;
    for (int l = 15; l >= 0; l--)
        if ((s & B(l)) && keymap[l][pos] != KC_TRNS) return (uint8_t)l;
    return 0;
}

/* One key event: the mouse layer handling as in process_record_kb, then the
   layer action, as QMK runs it. Returns the keycode that ran (KC_NO if it was
   swallowed). */
static uint16_t key(int pos, bool pressed, bool tapped) {
    if (pressed) source[pos] = lookup(pos);
    uint16_t kc = keymap[source[pos]][pos];
    if (mouse_on && (layer_state & B(ML))) {
        bool lk = sval_automouse_layer_key(kc, tapped, ML);
        switch (sval_automouse_key(kc, keymap[ML][pos], pressed, false, lk, kc == KC_NO || kc == KC_TRNS)) {
            case SVAL_AM_KEY_NONE:
                break;
            case SVAL_AM_KEY_END_SWALLOW:
                mouse_mode(false);
                return KC_NO;
            case SVAL_AM_KEY_END:
                mouse_mode(false);
                break;
            case SVAL_AM_KEY_MOUSE:
                if (pressed) held++;
                else if (held > 0) held--;
                mouse_mode(true);
                break;
        }
    }
    if (IS_QK_MOMENTARY(kc) || (IS_QK_LAYER_TAP(kc) && !tapped)) {
        uint8_t l = IS_QK_MOMENTARY(kc) ? (kc & 0x1F) : ((kc >> 8) & 0xF);
        if (pressed) layer_state |= B(l);
        else layer_state &= ~B(l);
    } else if (IS_QK_TO(kc) && pressed) {
        layer_state = B(kc & 0x1F);
    }
    return kc;
}
static void tap(int pos) { key(pos, true, false); key(pos, false, false); }

static void reset(uint16_t mouse_thumb) {
    memset(keymap, 0, sizeof(keymap));
    for (int l = 1; l < 16; l++) keymap[l][THUMB] = keymap[l][BUTTON] = KC_TRNS;
    keymap[0][THUMB]  = MO_(1); /* the NAS key on the base layer */
    keymap[0][BUTTON] = KC_A;
    keymap[1][BUTTON] = KC_1;
    keymap[ML][BUTTON] = MS_BTN1;
    keymap[ML][THUMB]  = mouse_thumb;
    layer_state = 0;
    default_layer_state = B(0);
    mouse_mode(true); /* the pointer moved */
}

int main(void) {
    /* The classifier: layer keys to other layers, by type. */
    assert(sval_automouse_layer_key(MO_(1), false, ML));
    assert(sval_automouse_layer_key(TO_(1), false, ML));
    assert(sval_automouse_layer_key(TG_(4), false, ML));
    assert(sval_automouse_layer_key(DF_(2), false, ML));
    assert(sval_automouse_layer_key(PDF_(2), false, ML));
    assert(sval_automouse_layer_key(OSL_(3), false, ML));
    assert(sval_automouse_layer_key(TT_(3), false, ML));
    assert(sval_automouse_layer_key(TT_(3), true, ML));
    assert(sval_automouse_layer_key(LM_(1, 0x02), false, ML));
    assert(sval_automouse_layer_key(LT_(1, KC_ENTER), false, ML));
    assert(!sval_automouse_layer_key(LT_(1, KC_ENTER), true, ML)); /* tapped: Enter */
    /* Keys to the mouse layer itself stay mouse keys. */
    assert(!sval_automouse_layer_key(MO_(ML), false, ML));
    assert(!sval_automouse_layer_key(TG_(ML), false, ML));
    assert(!sval_automouse_layer_key(LT_(ML, KC_A), false, ML));
    assert(!sval_automouse_layer_key(LM_(ML, 0x01), false, ML));
    /* Not layer keys. */
    assert(!sval_automouse_layer_key(KC_A, false, ML));
    assert(!sval_automouse_layer_key(KC_NO, false, ML));
    assert(!sval_automouse_layer_key(KC_TRNS, false, ML));
    assert(!sval_automouse_layer_key(MS_BTN1, false, ML));
    assert(!sval_automouse_layer_key(QK_MOD_TAP | 0x0104, false, ML));

    /* MO(1) on the mouse layer: holds layer 1, which keys then read; its
       release goes back to the base layer. (It used to keep the mouse layer
       on top, so the key under it read MS_BTN1.) */
    reset(MO_(1));
    key(THUMB, true, false);
    assert(!mouse_on && layer_state == B(1));
    assert(key(BUTTON, true, false) == KC_1);
    key(BUTTON, false, false);
    key(THUMB, false, false);
    assert(layer_state == 0 && !mouse_on);

    /* Moving the pointer while it is held brings the mouse layer back over
       layer 1, as it does from the base layer's own MO(1); the release
       leaves it on. */
    reset(MO_(1));
    key(THUMB, true, false);
    mouse_mode(true);
    key(THUMB, false, false);
    assert(mouse_on && layer_state == B(ML));

    /* TO(1): moves to layer 1, and the mouse layer's state is cleared (it
       used to keep one key counted as held, so it never timed out again). */
    reset(TO_(1));
    key(THUMB, true, false);
    key(THUMB, false, false);
    assert(layer_state == B(1) && !mouse_on && held == 0);

    /* Blank (KC_NO): ends the mouse layer and does nothing else. */
    reset(KC_NO);
    assert(key(THUMB, true, false) == KC_NO);
    key(THUMB, false, false);
    assert(layer_state == 0 && !mouse_on);
    /* The next press is the base layer's MO(1). */
    key(THUMB, true, false);
    assert(layer_state == B(1));
    key(THUMB, false, false);

    /* Transparent: the base layer's MO(1) shows through, ends the mouse
       layer and holds layer 1 (unchanged). */
    reset(KC_TRNS);
    assert(key(THUMB, true, false) == MO_(1));
    assert(!mouse_on && layer_state == B(1));
    assert(key(BUTTON, true, false) == KC_1);
    key(BUTTON, false, false);
    key(THUMB, false, false);
    assert(layer_state == 0);

    /* LT(1, Enter) on the mouse layer: tapped, Enter, a mouse key as before;
       held, layer 1. */
    reset(LT_(1, KC_ENTER));
    key(THUMB, true, true);
    assert(mouse_on && held == 1);
    key(THUMB, false, true);
    assert(mouse_on && held == 0 && layer_state == B(ML));
    key(THUMB, true, false);
    assert(!mouse_on && layer_state == B(1));
    key(THUMB, false, false);
    assert(layer_state == 0);

    /* MO(15) on the mouse layer is still a mouse key. */
    reset(MO_(ML));
    key(THUMB, true, false);
    assert(mouse_on && held == 1);
    key(THUMB, false, false);
    assert(mouse_on && held == 0);

    /* A mouse button: counted while held, the mouse layer stays (unchanged). */
    reset(KC_TRNS);
    key(BUTTON, true, false);
    assert(mouse_on && held == 1);
    key(BUTTON, false, false);
    assert(mouse_on && held == 0);
    tap(BUTTON);
    assert(mouse_on);
    return 0;
}
'''


class AutoMouseLayerKeys(unittest.TestCase):
    def test_layer_keys_on_the_mouse_layer(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(TEST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-I', str(KB), '-I', str(ROOT / 'quantum'), str(path / 'test.c'), str(KB / 'automouse_keys.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
