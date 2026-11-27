"""Host tests for the key handling around Auto Mouse (keyboards/svalboard/keymaps/keymap_support.c).

The production process_record_kb, matrix_scan_kb and mouse_mode run against a
small model of QMK's layers, keymap and key records; the pointing hardware and
storage are stubbed. No board or flash is accessed.

Run: python3 -m unittest discover -s tests/sval_storage -v
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'keyboards/svalboard/keymaps/keymap_support.c'
TRNS_MOD_TAP = ROOT / 'keyboards/svalboard/trns_mod_tap.c'
HEADER = ROOT / 'keyboards/svalboard/keymaps/keymap_support.h'


def function(source, name):
    match = re.search(r'^[a-zA-Z_][^\n;{}]*\b' + name + r'\([^;\n]*\)\s*\{', source, re.M)
    start = match.start()
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def production():
    source = SOURCE.read_text()
    # The key handling: its state, then process_record_kb (the end of its body).
    start = source.index('int8_t mouse_keys_pressed = 0;')
    record = function(source, 'process_record_kb')
    end = source.index(record) + len(record)
    keycodes = re.search(r'^enum my_keycodes \{.*?^\};', HEADER.read_text(), re.S | re.M).group()
    trns = TRNS_MOD_TAP.read_text().replace('#include "trns_mod_tap.h"', '')
    return keycodes, trns + '\n' + source[start:end] + '\n' + function(source, 'matrix_scan_kb') + '\n' + function(source, 'mouse_mode') + '\n'


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "keycodes.h"
#include "modifiers.h"
#define TO(layer) (QK_TO | ((layer) & 0x1F))
#define MT(mod, kc) (QK_MOD_TAP | (((mod) & 0x1F) << 8) | ((kc) & 0xFF))
#define POINTING_DEVICE_AUTO_MOUSE_MH_ENABLE
#define MATRIX_ROWS 10
#define MATRIX_COLS 6
#define MH_AUTO_BUTTONS_LAYER 15
#define TIMEOUT_MS 100
#define dprint(s)
#define dprintf(...)

typedef uint32_t layer_state_t;
typedef struct { uint8_t col, row; } keypos_t;
typedef enum { TICK_EVENT = 0, KEY_EVENT = 1, COMBO_EVENT = 4 } keyevent_type_t;
typedef struct { keypos_t key; uint16_t time; keyevent_type_t type; bool pressed; } keyevent_t;
typedef struct { bool interrupted : 1; bool speculated : 1; bool reserved1 : 1; bool reserved0 : 1; uint8_t count : 4; } tap_t;
typedef struct { keyevent_t event; tap_t tap; uint16_t keycode; } keyrecord_t;
#define KEYEQ(keya, keyb) ((keya).row == (keyb).row && (keya).col == (keyb).col)
static inline bool IS_COMBOEVENT(const keyevent_t event) { return event.type == COMBO_EVENT; }

/* QMK's layers and keymap, as far as the key handling uses them. */
static layer_state_t layer_state = 1;
static uint16_t keymap[16][MATRIX_ROWS][MATRIX_COLS];
static uint8_t get_highest_layer(layer_state_t s) { uint8_t l = 0; while (s >>= 1) l++; return l; }
static void layer_on(uint8_t l) { layer_state |= (layer_state_t)1 << l; }
static void layer_off(uint8_t l) { layer_state &= ~((layer_state_t)1 << l); }
static void layer_move(uint8_t l) { layer_state = (layer_state_t)1 << l; }
static uint16_t keymap_key_to_keycode(uint8_t layer, keypos_t key) { return keymap[layer][key.row][key.col]; }
/* The highest layer on where the key is not KC_TRNS, as QMK's. */
static uint8_t layer_switch_get_layer(keypos_t key) {
    for (int l = 15; l > 0; l--)
        if ((layer_state >> l & 1) && keymap[l][key.row][key.col] != KC_TRNS) return l;
    return 0;
}
static uint16_t now;
static uint16_t timer_read(void) { return now; }
static uint16_t timer_elapsed(uint16_t t) { return (uint16_t)(now - t); }

/* Svalboard state and actions the handler touches. */
static struct { bool auto_mouse, left_scroll, right_scroll, natural_scroll; uint8_t mh_timer_index, automouse_10ms; } global_saved_values = {.auto_mouse = true};
static bool mouse_mode_enabled;
static uint16_t mh_auto_buttons_timer;
static int32_t automouse_accumulator;
static int tp_buttons;
static int sniper_hold_2, sniper_hold_3, sniper_hold_5, boost_hold_2, boost_hold_3, boost_hold_5;
static bool sniper_toggle_2, sniper_toggle_3, sniper_toggle_5, boost_toggle_2, boost_toggle_3, boost_toggle_5;
static bool scroll_hold, scroll_toggle;
static int vs_hold;
static bool process_record_user(uint16_t keycode, keyrecord_t *record) { return true; }
static void sval_key_log(uint16_t keycode, keyrecord_t *record) {}
static void increase_left_dpi(void) {}
static void decrease_left_dpi(void) {}
static void increase_right_dpi(void) {}
static void decrease_right_dpi(void) {}
static void write_eeprom_kb(void) {}
static void recalibrate_pointer(void) {}
static void mh_change_timeouts(void) {}
static void caps_word_toggle(void) {}
static void toggle_axis_scroll_lock(void) {}
static void check_layer_67(void) {}
static void update_sniper_divisor(void) {}
static void update_boost_multiplier(void) {}
static void sval_vs_key(int *hold, bool toggle, bool pressed) {}
static void velocity_scroll_update(void) {}
static bool velocity_scroll_active(void) { return false; }
static void output_keyboard_info(void) {}
static void change_turbo_scan(void) {}
static int32_t sval_automouse_timeout(uint8_t index, uint8_t ms10) { return TIMEOUT_MS; }
static bool automouse_layer_blocked(void) { return false; }
static void matrix_scan_user(void) {}
void mouse_mode(bool on);
'''

HARNESS = r'''
#define MOUSE_LAYER_ON() (!!(layer_state & ((layer_state_t)1 << MH_AUTO_BUTTONS_LAYER)))

/* A key event through process_record_kb, then the action QMK takes for the
 * few keycodes the tests use. The keycode is the one QMK resolved for it. */
static bool event(uint8_t row, uint8_t col, bool pressed, uint16_t keycode, keyevent_type_t type, uint8_t taps) {
    keyrecord_t record = {.event = {.key = {.col = col, .row = row}, .type = type, .pressed = pressed}, .keycode = keycode};
    record.tap.count = taps;
    bool more = process_record_kb(keycode, &record);
    if (more && pressed && keycode >= QK_TO && keycode <= QK_TO_MAX) layer_move(keycode & 0x1F);
    return more;
}
static void press(uint8_t row, uint8_t col, uint16_t keycode) { event(row, col, true, keycode, KEY_EVENT, 0); }
static void release(uint8_t row, uint8_t col, uint16_t keycode) { event(row, col, false, keycode, KEY_EVENT, 0); }
static void motion(void) { mouse_mode(true); }
static void wait(uint16_t ms) { now += ms; matrix_scan_kb(); }
'''


def run(body):
    keycodes, code = production()
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory)
        (path / 'test.c').write_text(PRELUDE + keycodes + '\n' + code + HARNESS + 'int main(void) {\n' + body + '\nreturn 0;\n}\n')
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-fsanitize=undefined', '-I', str(ROOT / 'quantum'), str(path / 'test.c'), '-o', str(path / 'test')], check=True)
        return subprocess.run([str(path / 'test')], capture_output=True, text=True)


class AutoMouseKeys(unittest.TestCase):
    def check(self, body):
        result = run(body)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_to_key_on_the_mouse_layer_lets_it_time_out(self):
        # F40: TO(0) on the mouse layer. Its press counted as a held mouse key,
        # and its release came after the layer was gone, so the count stayed at
        # 1 and the mouse layer never timed out again.
        self.check(
            r'''
            keymap[MH_AUTO_BUTTONS_LAYER][1][0] = TO(0);
            motion();
            press(1, 0, TO(0));
            assert(!MOUSE_LAYER_ON());
            release(1, 0, TO(0));
            wait(1);
            motion();
            assert(MOUSE_LAYER_ON());
            wait(TIMEOUT_MS + 1);
            assert(!MOUSE_LAYER_ON());
        '''
        )

    def test_transparent_mod_tap_on_the_mouse_layer_keeps_auto_mouse(self):
        # The F40 fix must not end Auto Mouse while a transparent mod-tap on the
        # mouse layer has lifted it for the length of its tap.
        self.check(
            r'''
            keymap[MH_AUTO_BUTTONS_LAYER][2][0] = MT(MOD_LSFT, KC_TRNS);
            motion();
            event(2, 0, true, MT(MOD_LSFT, KC_TRNS), KEY_EVENT, 1);
            assert(!MOUSE_LAYER_ON());
            wait(1);
            event(2, 0, false, MT(MOD_LSFT, KC_TRNS), KEY_EVENT, 1);
            assert(MOUSE_LAYER_ON() && mouse_mode_enabled);
            wait(TIMEOUT_MS + 1);
            assert(!MOUSE_LAYER_ON());
        '''
        )

    def test_combo_of_a_mouse_layer_key_keeps_the_mouse_layer(self):
        # F41: a combo has no key position (QMK gives it row 0, col 0), so it was
        # compared with the mouse layer's key at 0,0 and ended Auto Mouse.
        self.check(
            r'''
            keymap[MH_AUTO_BUTTONS_LAYER][0][0] = KC_A;
            keymap[MH_AUTO_BUTTONS_LAYER][3][2] = MS_BTN1;
            motion();
            event(0, 0, true, MS_BTN1, COMBO_EVENT, 0);
            assert(MOUSE_LAYER_ON() && mouse_keys_pressed == 1);
            event(0, 0, false, MS_BTN1, COMBO_EVENT, 0);
            assert(MOUSE_LAYER_ON() && mouse_keys_pressed == 0);
            /* A combo of a key that is not on the mouse layer still ends it. */
            event(0, 0, true, KC_B, COMBO_EVENT, 0);
            assert(!MOUSE_LAYER_ON());
        '''
        )

    def test_release_of_a_key_held_from_before_keeps_auto_mouse(self):
        # F44: a key pressed before the mouse layer came on, released after,
        # ended Auto Mouse. Only presses end it.
        self.check(
            r'''
            press(1, 1, KC_F);
            motion();
            release(1, 1, KC_F);
            assert(MOUSE_LAYER_ON() && mouse_mode_enabled && mouse_keys_pressed == 0);
            /* Pressing a key that is not on the mouse layer still ends it. */
            press(1, 1, KC_F);
            assert(!MOUSE_LAYER_ON());
        '''
        )

    def test_transparent_mod_tap_waits_for_its_own_release(self):
        # F43: while a transparent mod-tap's tap was out, the release of any
        # other key was taken for the mod-tap's: that key's own release handling
        # was skipped (here a Sniper hold stayed counted) and the layer came back
        # early.
        self.check(
            r'''
            layer_state = (1u << 0) | (1u << 1);
            press(3, 0, SV_SNIPER_2);
            assert(sniper_hold_2 == 1);
            event(4, 0, true, MT(MOD_LSFT, KC_TRNS), KEY_EVENT, 1);
            assert(layer_state == (1u << 0));
            release(3, 0, SV_SNIPER_2);
            assert(sniper_hold_2 == 0);
            assert(layer_state == (1u << 0));
            event(4, 0, false, MT(MOD_LSFT, KC_TRNS), KEY_EVENT, 1);
            assert(layer_state == ((1u << 0) | (1u << 1)) && !in_mod_tap && in_mod_tap_layer == -1);
        '''
        )


if __name__ == '__main__':
    unittest.main()
