"""Host tests for the runtime QMK Settings getters.

Run: python3 -m unittest discover -s tests/sval_runtime_tap -p 'test_settings.py' -v

Three programs are built from the production sources, with QMK mocked:

- upstream: quantum/sval_hooks.h without RUNTIME_QMK_SETTINGS. Every GET_*()
  is the compile-time constant, as in stock QMK, and no getter is compiled.
- module absent: quantum/sval_hooks.c with RUNTIME_QMK_SETTINGS and no
  overrides. The weak getters return the same constants.
- module present: quantum/sval_hooks.c linked with
  modules/svalboard/core/sval_qmk_settings.c and sval_auto_shift.c. Each QSID is set through
  sval_qmk_settings_set() and the getter QMK reads must return it.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CFLAGS = ['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-fsanitize=undefined']


def source(path):
    """A production file with its quoted includes dropped; the stubs replace them."""
    text = re.sub(r'^\s*#\s*include "[^"]*"[^\n]*', '', (ROOT / path).read_text(), flags=re.M)
    return text.replace('#pragma once', '')


# What the real QMK headers provide to these files.
QMK = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
typedef struct { uint8_t col, row; } keypos_t;
typedef struct { int unused; } keyrecord_t;
#define TAPPING_TERM 200
#ifndef TAPPING_TOGGLE
#    define TAPPING_TOGGLE 5
#endif
#ifndef TAP_CODE_DELAY
#    define TAP_CODE_DELAY 0
#endif
#ifndef TAP_HOLD_CAPS_DELAY
#    define TAP_HOLD_CAPS_DELAY 80
#endif
#define KC_TAB 0x2B
#define KC_ENT 0x28
#define AUTO_SHIFT_ALPHA 0x04 ... 0x1D
#define AUTO_SHIFT_NUMERIC 0x1E ... 0x27
#define AUTO_SHIFT_SYMBOLS 0x2D ... 0x38: case 0x64
'''

# The module's own build switches, exactly as post_config.h includes them.
SWITCHES = source('modules/svalboard/core/sval_runtime_settings.h')

MODULE_STUBS = r'''
#define AUTO_SHIFT_ENABLE
#define SVAL_QMK_SETTINGS_OFFSET 0
#define SVAL_DEFAULT_LEADER_TIMEOUT 300
#define SVAL_DEFAULT_LEADER_PER_KEY_TIMING 1
struct {
 unsigned raw, swap_control_capslock, capslock_to_control, swap_lalt_lgui,
 swap_ralt_rgui, no_gui, swap_grave_esc, swap_backslash_backspace, nkro,
 swap_lctl_lgui, swap_rctl_rgui, oneshot_enable;
} keymap_config;
static uint8_t storage[44];
bool autoshift_enabled = true;
uint16_t autoshift_timeout_val = 0;
static void set_autoshift_timeout(uint16_t timeout) { autoshift_timeout_val = timeout; }
static void autoshift_enable(void) { autoshift_enabled = true; }
static void autoshift_disable(void) { autoshift_enabled = false; }
static bool get_autoshift_state(void) { return autoshift_enabled; }
static void clear_keyboard(void) {}
static void eeconfig_update_keymap(void *p) { (void)p; }
static void eeconfig_read_kb_datablock(void *p, size_t off, size_t n) { assert(off==0 && n==44); memcpy(p, storage, n); }
static void eeconfig_update_kb_datablock(void *p, size_t off, size_t n) { assert(off==0 && n==44); memcpy(storage, p, n); }
static bool get_chordal_hold_default(keyrecord_t *a, keyrecord_t *b) { return true; }
static bool is_flow_tap_key(uint16_t code) { return true; }
bool get_custom_auto_shifted_key(uint16_t keycode, keyrecord_t *record) { return keycode == 0x2C; }
'''

MODULE_API = r'''
bool get_custom_auto_shifted_key(uint16_t keycode, keyrecord_t *record);
int sval_qmk_settings_set(uint16_t qsid, const uint8_t *data, uint8_t length);
int sval_qmk_settings_get(uint16_t qsid, uint8_t *buffer, uint8_t length);
void sval_qmk_settings_reset(void);
void sval_qmk_settings_init(void);
uint8_t sval_auto_shift(void);
uint16_t sval_get_combo_term(void);
uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record);
bool get_permissive_hold(uint16_t keycode, keyrecord_t *record);
bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record);
bool get_retro_tapping(uint16_t keycode, keyrecord_t *record);
uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record);
uint16_t get_flow_tap_term(uint16_t keycode, keyrecord_t *record, uint16_t prev_keycode);
bool get_auto_shifted_key(uint16_t keycode, keyrecord_t *record);
bool get_auto_shift_repeat(uint16_t keycode, keyrecord_t *record);
bool get_auto_shift_no_auto_repeat(uint16_t keycode, keyrecord_t *record);
extern bool autoshift_enabled;
extern uint16_t autoshift_timeout_val;
'''

PRESENT_MAIN = r'''
static void put(uint16_t q, uint16_t value) {
 uint8_t bytes[] = {value & 255, value >> 8};
 assert(sval_qmk_settings_set(q, bytes, sizeof(bytes)) == 0);
}
static void bit(uint16_t q, bool on) { uint8_t b = on; assert(sval_qmk_settings_set(q, &b, 1) == 0); }
int main(void) {
 keyrecord_t r = {0};
 sval_qmk_settings_reset();
 /* Reset defaults are the compile-time constants. */
 assert(GET_TAP_CODE_DELAY()==TAP_CODE_DELAY && GET_TAP_HOLD_CAPS_DELAY()==TAP_HOLD_CAPS_DELAY);
 assert(GET_TAPPING_TOGGLE()==TAPPING_TOGGLE && GET_ONESHOT_TAP_TOGGLE()==ONESHOT_TAP_TOGGLE);
 assert(GET_ONESHOT_TIMEOUT()==ONESHOT_TIMEOUT && GET_GRAVE_ESC_OVERRIDES()==0);
 assert(sval_auto_shift()==0 && !GET_AUTO_SHIFT_MODIFIERS() && !autoshift_enabled && autoshift_timeout_val==175);

 /* QSID 1: Grave Escape overrides, one bit at a time. */
 for (uint8_t b = 0; b < 4; b++) { put(1, 1 << b); assert(GET_GRAVE_ESC_OVERRIDES() == (1 << b)); }
 put(1, 0x0F); assert(GET_GRAVE_ESC_OVERRIDES() == 0x0F);
 assert(GRAVE_ESC_ALT_OVERRIDE_BIT==1 && GRAVE_ESC_CTRL_OVERRIDE_BIT==2 && GRAVE_ESC_GUI_OVERRIDE_BIT==4 && GRAVE_ESC_SHIFT_OVERRIDE_BIT==8);
 /* QSID 2: combo term. */
 put(2, 73); assert(sval_get_combo_term() == 73);
 /* QSID 3: Auto Shift bits. */
 put(3, 0x00);
 assert(!autoshift_enabled && !GET_AUTO_SHIFT_MODIFIERS());
 assert(get_auto_shifted_key(0x04,&r) && get_auto_shifted_key(0x1E,&r) && get_auto_shifted_key(0x2D,&r) && get_auto_shifted_key(KC_TAB,&r));
 assert(!get_auto_shifted_key(KC_ENT,&r) && get_auto_shifted_key(0x2C,&r)); /* custom keys still count */
 assert(!get_auto_shift_repeat(0x04,&r) && !get_auto_shift_no_auto_repeat(0x04,&r));
 put(3, 0x01); assert(autoshift_enabled);
 put(3, 0x02); assert(!autoshift_enabled && GET_AUTO_SHIFT_MODIFIERS());
 put(3, 0x04); assert(!get_auto_shifted_key(0x2D,&r) && !get_auto_shifted_key(KC_TAB,&r) && get_auto_shifted_key(0x04,&r));
 put(3, 0x08); assert(!get_auto_shifted_key(0x1E,&r) && get_auto_shifted_key(0x04,&r) && get_auto_shifted_key(0x2D,&r));
 put(3, 0x10); assert(!get_auto_shifted_key(0x04,&r) && get_auto_shifted_key(0x1E,&r));
 put(3, 0x20); assert(get_auto_shift_repeat(0x04,&r) && !get_auto_shift_no_auto_repeat(0x04,&r));
 put(3, 0x40); assert(!get_auto_shift_repeat(0x04,&r) && get_auto_shift_no_auto_repeat(0x04,&r));
 put(3, 0x7F); assert(sval_auto_shift()==0x7F && autoshift_enabled);
 /* QSID 4: Auto Shift timeout goes to QMK's own setter. */
 put(4, 215); assert(autoshift_timeout_val == 215);
 /* QSID 5 and 6: one-shot tap toggle and timeout. */
 put(5, 3); assert(GET_ONESHOT_TAP_TOGGLE() == 3);
 put(5, 0); assert(GET_ONESHOT_TAP_TOGGLE() == 0);
 put(5, 16); assert(GET_ONESHOT_TAP_TOGGLE() == 15);
 put(6, 1234); assert(GET_ONESHOT_TIMEOUT() == 1234);
 put(6, 0); assert(GET_ONESHOT_TIMEOUT() == 0);
 /* QSID 7, 25, 27: tapping, quick tap and flow tap terms, through QMK's own hooks. */
 put(7, 222); assert(get_tapping_term(0x04,&r) == 222);
 put(25, 111); assert(get_quick_tap_term(0x04,&r) == 111);
 put(27, 99); assert(get_flow_tap_term(0x04,&r,0x05) == 99);
 /* QSID 22-24: tap-hold bits, through QMK's own hooks. */
 bit(22,1); assert(get_permissive_hold(0x04,&r)); bit(22,0); assert(!get_permissive_hold(0x04,&r));
 bit(23,1); assert(get_hold_on_other_key_press(0x04,&r)); bit(23,0); assert(!get_hold_on_other_key_press(0x04,&r));
 bit(24,1); assert(get_retro_tapping(0x04,&r)); bit(24,0); assert(!get_retro_tapping(0x04,&r));
 /* QSID 18-20: tap code delay, caps delay, tapping toggle. */
 put(18, 300); assert(GET_TAP_CODE_DELAY() == 300);
 put(19, 400); assert(GET_TAP_HOLD_CAPS_DELAY() == 400);
 put(20, 3); assert(GET_TAPPING_TOGGLE() == 3);
 put(20, 0); assert(GET_TAPPING_TOGGLE() == 1); /* 0 is read as 1 */
 uint8_t raw = 255; assert(sval_qmk_settings_get(20, &raw, 1) == 0 && raw == 0);
 put(20, 16); assert(GET_TAPPING_TOGGLE() == 15); /* QMK counts at most 15 taps */
 /* Stored values survive a reload. */
 put(20, 3); put(1, 8); put(6, 777);
 sval_qmk_settings_init();
 assert(GET_TAP_CODE_DELAY()==300 && GET_TAP_HOLD_CAPS_DELAY()==400 && GET_TAPPING_TOGGLE()==3);
 assert(GET_GRAVE_ESC_OVERRIDES()==8 && GET_ONESHOT_TIMEOUT()==777 && sval_auto_shift()==0x7F && autoshift_timeout_val==215);
 put(18, 65535); put(19, 65535); sval_qmk_settings_init();
 assert(GET_TAP_CODE_DELAY()==65535 && GET_TAP_HOLD_CAPS_DELAY()==65535);
 /* The context-layer hook keeps its weak default when sval_context_layer.c is absent. */
 keypos_t k = {0, 0}; assert(layer_switch_get_layer_override(k) == UINT8_MAX);
 return 0;
}
'''

ABSENT_MAIN = r'''
int main(void) {
 keypos_t k = {0, 0};
 assert(GET_TAP_CODE_DELAY() == 11 && GET_TAP_HOLD_CAPS_DELAY() == 83);
 assert(GET_TAPPING_TOGGLE() == 3 && GET_ONESHOT_TIMEOUT() == 4321 && GET_ONESHOT_TAP_TOGGLE() == 4);
 assert(GET_GRAVE_ESC_OVERRIDES() == (GRAVE_ESC_ALT_OVERRIDE_BIT | GRAVE_ESC_SHIFT_OVERRIDE_BIT));
 assert(!GET_AUTO_SHIFT_MODIFIERS());
#ifdef LAYER_SWITCH_GET_LAYER_OVERRIDE
 assert(layer_switch_get_layer_override(k) == UINT8_MAX);
#endif
 (void)k;
 return 0;
}
'''

# A keyboard's config.h, for the upstream and module-absent programs.
KEYBOARD_CONFIG = r'''
#define TAP_CODE_DELAY 11
#define TAP_HOLD_CAPS_DELAY 83
#define TAPPING_TOGGLE 3
#define ONESHOT_TIMEOUT 4321
#define ONESHOT_TAP_TOGGLE 4
#define GRAVE_ESC_ALT_OVERRIDE
#define GRAVE_ESC_SHIFT_OVERRIDE
'''


def build(directory, name, units):
    files = []
    for i, text in enumerate(units):
        path = Path(directory) / f'{name}_{i}.c'
        path.write_text(text)
        files.append(str(path))
    out = str(Path(directory) / name)
    subprocess.run(CFLAGS + files + ['-o', out], check=True)
    subprocess.run([out], check=True)
    return subprocess.run(['nm', out], capture_output=True, text=True, check=True).stdout


class RuntimeGetters(unittest.TestCase):
    def setUp(self):
        self.hooks_h = source('quantum/sval_hooks.h')
        self.hooks_c = source('quantum/sval_hooks.c')

    def test_upstream_build_uses_constants(self):
        """Without RUNTIME_QMK_SETTINGS no getter is compiled; GET_*() are the constants."""
        with tempfile.TemporaryDirectory() as directory:
            table = build(directory, 'upstream', [KEYBOARD_CONFIG + QMK + self.hooks_h + self.hooks_c + ABSENT_MAIN])
        self.assertNotRegex(table, r'\bget_(tap_code_delay|tap_hold_caps_delay|tapping_toggle|oneshot_timeout|oneshot_tap_toggle|grave_esc_overrides|auto_shift_modifiers)\b')

    def test_module_absent_uses_weak_defaults(self):
        """With RUNTIME_QMK_SETTINGS but no module, the weak getters return the constants."""
        switches = '#define RUNTIME_QMK_SETTINGS\n#define LAYER_SWITCH_GET_LAYER_OVERRIDE\n'
        prefix = KEYBOARD_CONFIG + switches + QMK + self.hooks_h
        with tempfile.TemporaryDirectory() as directory:
            table = build(directory, 'absent', [prefix + self.hooks_c, prefix + ABSENT_MAIN])
        for getter in ['get_tap_code_delay', 'get_tap_hold_caps_delay', 'get_tapping_toggle', 'get_oneshot_timeout', 'get_oneshot_tap_toggle', 'get_grave_esc_overrides', 'get_auto_shift_modifiers', 'layer_switch_get_layer_override']:
            self.assertRegex(table, r' W ' + getter + r'\b')

    def test_module_present_returns_each_qsid(self):
        """The module's getters override the weak defaults and return each stored QSID."""
        module = source('modules/svalboard/core/sval_qmk_settings.c')
        auto_shift = '#define AUTO_SHIFT_ENABLE\n' + source('modules/svalboard/core/sval_qmk_settings.h') + source('modules/svalboard/core/sval_auto_shift.c')
        prefix = SWITCHES + QMK + self.hooks_h
        units = [prefix + self.hooks_c, prefix + MODULE_STUBS + module, prefix + MODULE_API + auto_shift, prefix + MODULE_API + PRESENT_MAIN]
        with tempfile.TemporaryDirectory() as directory:
            table = build(directory, 'present', units)
        for getter in ['get_tap_code_delay', 'get_tap_hold_caps_delay', 'get_tapping_toggle', 'get_oneshot_timeout', 'get_oneshot_tap_toggle', 'get_grave_esc_overrides', 'get_auto_shift_modifiers']:
            self.assertRegex(table, r' T ' + getter + r'\b', getter + ' must come from the module, not the weak default')
        self.assertRegex(table, r' W layer_switch_get_layer_override\b')


if __name__ == '__main__':
    unittest.main()
