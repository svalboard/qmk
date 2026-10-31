"""Compile the production settings implementation with an in-memory EEPROM."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RuntimeSettings(unittest.TestCase):
    def test_defaults_saved_values_and_reload(self):
        source = (ROOT / 'modules/svalboard/core/sval_qmk_settings.c').read_text()
        source = re.sub(r'^\s*#\s*include[^\n]*', '', source, flags=re.M)
        stub = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#define TAPPING_TERM 200
#define TAPPING_TOGGLE 5
#define TAP_CODE_DELAY 0
#define TAP_HOLD_CAPS_DELAY 80
#define AUTO_SHIFT_ENABLE
#define SVAL_QMK_SETTINGS_OFFSET 0
#define TAPPING_PERMISSIVE_HOLD_BIT 0
#define TAPPING_HOLD_ON_OTHER_KEY_BIT 1
#define TAPPING_RETRO_TAPPING_BIT 2
#define TAPPING_CHORDAL_HOLD_BIT 3
#define LEADER_PER_KEY_TIMING_BIT 0
#define SVAL_DEFAULT_LEADER_TIMEOUT 300
#define SVAL_DEFAULT_LEADER_PER_KEY_TIMING 1
typedef struct { int unused; } keyrecord_t;
static struct {
 unsigned raw, swap_control_capslock, capslock_to_control, swap_lalt_lgui,
 swap_ralt_rgui, no_gui, swap_grave_esc, swap_backslash_backspace, nkro,
 swap_lctl_lgui, swap_rctl_rgui, oneshot_enable;
} keymap_config;
static uint8_t storage[44];
static bool autoshift_enabled = true;
static uint16_t autoshift_timeout_val = 0;
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
'''
        main = r'''
static void put(uint16_t q, uint16_t value) {
 uint8_t bytes[] = {value & 255, value >> 8};
 assert(sval_qmk_settings_set(q, bytes, sizeof(bytes)) == 0);
}
int main(void) {
 sval_qmk_settings_reset();
 assert(sval_tap_code_delay()==0 && sval_tap_hold_caps_delay()==80);
 assert(sval_tapping_toggle()==5 && sval_grave_esc_override()==0);
 assert(sval_auto_shift()==0 && !autoshift_enabled && autoshift_timeout_val==175);
 put(18,300); put(19,400); put(20,3); put(1,8); put(3,0x7F); put(4,215);
 assert(sval_tap_code_delay()==300 && sval_tap_hold_caps_delay()==400);
 assert(sval_tapping_toggle()==3 && sval_grave_esc_override()==8);
 assert(sval_auto_shift()==0x7F && autoshift_enabled && autoshift_timeout_val==215);
 memset(&settings,0,sizeof(settings)); autoshift_enabled=false; autoshift_timeout_val=0; sval_qmk_settings_init();
 assert(sval_tap_code_delay()==300 && sval_tap_hold_caps_delay()==400);
 assert(sval_tapping_toggle()==3 && sval_grave_esc_override()==8);
 assert(sval_auto_shift()==0x7F && autoshift_enabled && autoshift_timeout_val==215);
 put(3,0x7E); assert(sval_auto_shift()==0x7E && !autoshift_enabled);
 put(20,0); assert(sval_tapping_toggle()==1);
 put(20,15); assert(sval_tapping_toggle()==15);
 put(20,16); assert(sval_tapping_toggle()==15); /* QMK counts at most 15 taps */
 put(20,0);
 uint8_t raw=255; assert(sval_qmk_settings_get(20,&raw,1)==0 && raw==0);
 for (uint8_t bit=0;bit<4;bit++) { put(1,1<<bit); assert(sval_grave_esc_override()==(1<<bit)); }
 put(18,65535); put(19,65535);
 memset(&settings,0,sizeof(settings)); sval_qmk_settings_init();
 assert(sval_tap_code_delay()==65535 && sval_tap_hold_caps_delay()==65535);
 assert(sval_tapping_toggle()==1);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'test.c').write_text(stub + source + main)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fsanitize=undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)
