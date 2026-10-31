"""Host tests for the QMK settings store (modules/svalboard/core/sval_qmk_settings.c).

Run: python3 -m unittest discover -s tests/sval_storage -v

The production file is compiled with QMK and the EEPROM mocked. It covers the
leader timeout: the reset stored 0, because LEADER_TIMEOUT is the live variable
(post_config.h), and a timeout of 0 ends every sequence 1 ms after it starts.
It also covers the mouse key values: the wheel delay and interval are applied,
every value is held at the limit of the 8-bit variable it is applied to (a
stored 256 used to wrap to 0), and the one-time wheel interval migration.
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
CORE = ROOT / 'modules/svalboard/core'


def defaults():
    text = (CORE / 'post_config.h').read_text()
    return ''.join(f'#define {name} {value}\n' for name, value in re.findall(r'#\s*define (SVAL_DEFAULT_LEADER_\w+) (\d+)', text))


def settings_source():
    # Drop the quoted includes (QMK and Sval headers); the prelude mocks them.
    return re.sub(r'^#\s*include "[^"]+"\n', '', (CORE / 'sval_qmk_settings.c').read_text(), flags=re.M)


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define LEADER_ENABLE
#define MOUSEKEY_ENABLE
#define AUTO_SHIFT_ENABLE
#define TAPPING_TERM 200
#define MOUSEKEY_DELAY 10
#define MOUSEKEY_INTERVAL 20
#define MOUSEKEY_MOVE_DELTA 8
#define MOUSEKEY_MAX_SPEED 10
#define MOUSEKEY_TIME_TO_MAX 30
#define MOUSEKEY_WHEEL_DELAY 10
#define MOUSEKEY_WHEEL_INTERVAL 80
#define MOUSEKEY_WHEEL_MAX_SPEED 8
#define MOUSEKEY_WHEEL_TIME_TO_MAX 40
#define SVAL_QMK_SETTINGS_OFFSET 100
uint16_t sval_leader_timeout = 0; /* what sval_reset() leaves it at on a first boot */
#define LEADER_TIMEOUT (sval_leader_timeout)
typedef struct { int dummy; } keyrecord_t;
typedef union {
    uint32_t raw;
    struct {
        bool swap_control_capslock : 1, capslock_to_control : 1, swap_lalt_lgui : 1, swap_ralt_rgui : 1, no_gui : 1, swap_grave_esc : 1, swap_backslash_backspace : 1, nkro : 1, swap_lctl_lgui : 1, swap_rctl_rgui : 1, oneshot_enable : 1;
    };
} keymap_config_t;
keymap_config_t keymap_config;
static uint8_t eeprom[256];
static unsigned eeprom_writes;
void eeconfig_read_kb_datablock(void *d, uint32_t o, uint32_t n) { memcpy(d, eeprom + o, n); }
void eeconfig_update_kb_datablock(const void *d, uint32_t o, uint32_t n) { memcpy(eeprom + o, d, n); eeprom_writes++; }
void eeconfig_update_keymap(keymap_config_t *k) { (void)k; }
void clear_keyboard(void) {}
bool get_chordal_hold_default(keyrecord_t *a, keyrecord_t *b) { (void)a; (void)b; return true; }
bool is_flow_tap_key(uint16_t k) { (void)k; return true; }
uint8_t mk_delay, mk_interval, mk_max_speed, mk_time_to_max, mk_wheel_delay, mk_wheel_interval, mk_wheel_max_speed, mk_wheel_time_to_max;
static uint16_t autoshift_timeout;
static bool autoshift_on;
void set_autoshift_timeout(uint16_t t) { autoshift_timeout = t; }
void autoshift_enable(void) { autoshift_on = true; }
void autoshift_disable(void) { autoshift_on = false; }
bool get_autoshift_state(void) { return autoshift_on; }
'''

TESTS = r'''
#define STORED (*(sval_qmk_settings_t *)(eeprom + SVAL_QMK_SETTINGS_OFFSET))
static uint16_t get16(uint16_t qsid) { uint8_t b[4] = {0}; assert(sval_qmk_settings_get(qsid, b, sizeof b) == 0); return b[0] | b[1] << 8; }
static uint8_t get8(uint16_t qsid) { uint8_t b[4] = {0}; assert(sval_qmk_settings_get(qsid, b, sizeof b) == 0); return b[0]; }
static void set16(uint16_t qsid, uint16_t v) { uint8_t b[2] = {v & 0xFF, v >> 8}; assert(sval_qmk_settings_set(qsid, b, 2) == 0); }
static void set8(uint16_t qsid, uint8_t v) { uint8_t b[1] = {v}; assert(sval_qmk_settings_set(qsid, b, 1) == 0); }

static void boot_with(sval_qmk_settings_t stored) {
    STORED = stored;
    eeprom_writes = 0;
    sval_leader_timeout = 0;
    sval_qmk_settings_init();
}

/* Mouse keys: the wheel delay and interval are applied (Vial did; this firmware
   only stored them), with the delays in 10 ms units, and every value is held at
   the limit of the 8-bit variable it is applied to. */
static void mouse_keys(void) {
    sval_qmk_settings_reset();
    assert(mk_delay == MOUSEKEY_DELAY / 10 && mk_interval == MOUSEKEY_INTERVAL);
    assert(mk_wheel_delay == MOUSEKEY_WHEEL_DELAY / 10 && mk_wheel_interval == MOUSEKEY_WHEEL_INTERVAL);
    assert(mk_wheel_max_speed == MOUSEKEY_WHEEL_MAX_SPEED && mk_wheel_time_to_max == MOUSEKEY_WHEEL_TIME_TO_MAX);

    set16(14, 250);
    assert(mk_wheel_delay == 25 && get16(14) == 250);
    set16(15, 40);
    assert(mk_wheel_interval == 40 && get16(15) == 40);
    set16(15, 0); /* a 0 written with the setting working is kept: the fastest scroll */
    assert(mk_wheel_interval == 0 && get16(15) == 0);

    /* The edges: the largest value each variable holds is used as is. */
    set16(9, 2550);
    set16(10, 255);
    set16(12, 255);
    set16(13, 255);
    set16(14, 2550);
    set16(15, 255);
    set16(16, 255);
    set16(17, 255);
    assert(mk_delay == 255 && mk_interval == 255 && mk_max_speed == 255 && mk_time_to_max == 255);
    assert(mk_wheel_delay == 255 && mk_wheel_interval == 255 && mk_wheel_max_speed == 255 && mk_wheel_time_to_max == 255);
    assert(get16(9) == 2550 && get16(10) == 255 && get16(14) == 2550 && get16(15) == 255);

    /* One past the edge used to wrap: 256 became 0 (every scan), 2560 ms a delay of 0. */
    set16(9, 2560);
    set16(10, 256);
    set16(12, 256);
    set16(13, 300);
    set16(14, 10000);
    set16(15, 256);
    set16(16, 1000);
    set16(17, 65535);
    assert(mk_delay == 255 && mk_interval == 255 && mk_max_speed == 255 && mk_time_to_max == 255);
    assert(mk_wheel_delay == 255 && mk_wheel_interval == 255 && mk_wheel_max_speed == 255 && mk_wheel_time_to_max == 255);
    /* and is stored at the limit, so a host reads back what the board uses */
    assert(get16(9) == 2550 && get16(10) == 255 && get16(12) == 255 && get16(13) == 255);
    assert(get16(14) == 2550 && get16(15) == 255 && get16(16) == 255 && get16(17) == 255);

    /* A delay is applied in 10 ms units: 2559 ms is 255, not a wrap. */
    set16(9, 2559);
    assert(mk_delay == 255);
    set16(9, 30);
    assert(mk_delay == 3 && get16(9) == 30);

    /* An old stored value above the limit is held at the limit on boot, once. */
    sval_qmk_settings_t old = STORED;
    old.mousekey_interval       = 300; /* used to apply as 44 */
    old.mousekey_wheel_interval = 256;
    old.mousekey_delay          = 2600;
    boot_with(old);
    assert(eeprom_writes == 1);
    assert(STORED.mousekey_interval == 255 && STORED.mousekey_wheel_interval == 255 && STORED.mousekey_delay == 2550);
    assert(mk_interval == 255 && mk_wheel_interval == 255 && mk_delay == 255);
    old = STORED;
    boot_with(old);
    assert(eeprom_writes == 0);

    /* Step size (QSID 11) is the build's MOUSEKEY_MOVE_DELTA: a write has no effect,
       and a stored value from a layout (8) reads back as the step the board uses. */
    set16(11, 8 + MOUSEKEY_MOVE_DELTA);
    assert(get16(11) == MOUSEKEY_MOVE_DELTA);
    old = STORED;
    old.mousekey_move_delta = 3;
    boot_with(old);
    assert(eeprom_writes == 1 && get16(11) == MOUSEKEY_MOVE_DELTA);
}

/* Tapping toggle (20) and one-shot tap toggle (5): QMK counts at most 15 taps
   (tap.count is 4 bits), so a larger count could never be reached. */
static void tap_counts(void) {
    sval_qmk_settings_reset();
    set8(20, 15);
    set8(5, 15);
    assert(sval_tapping_toggle() == 15 && sval_oneshot_tap_toggle() == 15);
    set8(20, 16);
    set8(5, 50);
    assert(sval_tapping_toggle() == 15 && sval_oneshot_tap_toggle() == 15);
    assert(get8(20) == 15 && get8(5) == 15);
    set8(20, 0);
    assert(sval_tapping_toggle() == 1); /* 0 is still read as 1 */
    sval_qmk_settings_set_one_shot(5000, 100); /* Sval 0x0A */
    assert(sval_oneshot_tap_toggle() == 15 && STORED.osk_tap_toggle == 15);
    sval_qmk_settings_set_one_shot(5000, 0);   /* 0 still means no lock */
    assert(sval_oneshot_tap_toggle() == 0);
    sval_qmk_settings_t old = STORED;
    old.tapping_toggle = 100;
    old.osk_tap_toggle = 255;
    boot_with(old);
    assert(eeprom_writes == 1 && STORED.tapping_toggle == 15 && STORED.osk_tap_toggle == 15);
}

/* The one-time migration: a wheel interval of 0 stored by firmware that never
   applied it becomes the default on the first boot of this firmware; later
   boots keep a 0. */
static void wheel_migration(void) {
    sval_qmk_settings_reset();
    assert(STORED.migrations & 1); /* a reset needs no migration */
    sval_qmk_settings_t old = STORED;
    old.migrations              = 0; /* what earlier firmware left there */
    old.mousekey_wheel_interval = 0;
    old.mousekey_wheel_delay    = 0; /* a 0 delay works: kept */
    old.unused2                 = 0x5A;
    boot_with(old);
    assert(eeprom_writes == 1);
    assert(STORED.mousekey_wheel_interval == MOUSEKEY_WHEEL_INTERVAL && mk_wheel_interval == MOUSEKEY_WHEEL_INTERVAL);
    assert(STORED.mousekey_wheel_delay == 0 && mk_wheel_delay == 0 && (STORED.migrations & 1) && STORED.unused2 == 0x5A);

    /* After the migration a 0 is a choice and stays, through writes and boots. */
    set16(15, 0);
    old = STORED;
    boot_with(old);
    assert(eeprom_writes == 0 && STORED.mousekey_wheel_interval == 0 && mk_wheel_interval == 0);

    /* A board whose stored interval isn't 0 keeps it, and is only marked. */
    old.migrations              = 0;
    old.mousekey_wheel_interval = 120;
    boot_with(old);
    assert(eeprom_writes == 1 && STORED.mousekey_wheel_interval == 120 && mk_wheel_interval == 120 && (STORED.migrations & 1));
}

int main(void) {
    /* A settings reset writes the default leader timing, not the live variable (0 here). */
    sval_qmk_settings_reset();
    assert(STORED.leader_timeout == SVAL_DEFAULT_LEADER_TIMEOUT && SVAL_DEFAULT_LEADER_TIMEOUT == 300);
    assert(get16(28) == 300 && sval_leader_timeout == 300);
    assert(get8(29) == SVAL_DEFAULT_LEADER_PER_KEY_TIMING && sval_get_leader_per_key_timing());
    assert(get16(2) == 50 && get16(4) == 175 && get16(7) == 200 && get16(10) == 20);

    /* A second reset from a live 0 still writes 300 (a board at 0 used to stay at 0). */
    sval_leader_timeout = 0;
    sval_qmk_settings_reset();
    assert(STORED.leader_timeout == 300);

    /* Boot migration: a stored 0 (the old reset) becomes the default, once, and is saved. */
    sval_qmk_settings_t old = STORED;
    old.leader_timeout = 0;
    old.leader_options = 0;
    old.unused2 = 0x5A; /* the bytes around it are kept */
    boot_with(old);
    assert(eeprom_writes == 1);
    assert(STORED.leader_timeout == 300 && (STORED.leader_options & 1) && STORED.unused2 == 0x5A);
    assert(sval_leader_timeout == 300);
    old = STORED;
    boot_with(old);
    assert(eeprom_writes == 0 && STORED.leader_timeout == 300); /* nothing more on the next boot */

    /* A value the user chose is kept, per-key off included. */
    old.leader_timeout = 1000;
    old.leader_options = 0;
    boot_with(old);
    assert(eeprom_writes == 0 && STORED.leader_timeout == 1000 && !(STORED.leader_options & 1) && sval_leader_timeout == 1000);

    /* The other settings where 0 cannot work get their defaults; 0s that mean something stay. */
    old.combo_term = 0;
    old.tapping_term = 0;
    old.auto_shift_timeout = 0;
    old.mousekey_interval = 0;
    old.osk_timeout = 0;      /* one-shot: no timeout */
    old.flow_tap_term = 0;    /* Flow Tap off */
    old.tap_code_delay = 0;
    old.tapping_toggle = 0;   /* read as 1 */
    old.quick_tap_term = 0;   /* quick tap off */
    boot_with(old);
    assert(eeprom_writes == 1);
    assert(STORED.combo_term == 50 && STORED.tapping_term == 200 && STORED.auto_shift_timeout == 175 && STORED.mousekey_interval == 20);
    assert(STORED.osk_timeout == 0 && STORED.flow_tap_term == 0 && STORED.tap_code_delay == 0 && STORED.tapping_toggle == 0 && STORED.quick_tap_term == 0);
    assert(STORED.leader_timeout == 1000);
    assert(autoshift_timeout == 175 && mk_interval == 20);

    /* Writes: a 0 from an older Keybard is stored as the default; other values as sent. */
    set16(28, 0);
    assert(get16(28) == 300 && sval_leader_timeout == 300);
    set16(28, 750);
    assert(get16(28) == 750 && sval_leader_timeout == 750 && STORED.leader_timeout == 750);
    set16(2, 0);
    assert(get16(2) == 50);
    set16(7, 0);
    assert(get16(7) == 200);
    set16(6, 0);
    assert(get16(6) == 0); /* one-shot timeout 0 is allowed */

    mouse_keys();
    tap_counts();
    wheel_migration();
    printf("qmk settings: ok\n");
    return 0;
}
'''


class QmkSettings(unittest.TestCase):
    def test_leader_zero_defaults_and_mouse_key_limits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(PRELUDE + defaults() + settings_source() + TESTS)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unused-function', '-fsanitize=undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_reset_does_not_read_the_live_leader_variable(self):
        source = (CORE / 'sval_qmk_settings.c').read_text()
        reset = source[source.index('void sval_qmk_settings_reset(void)'):]
        reset = reset[:reset.index('\n}\n')]
        self.assertNotIn('LEADER_TIMEOUT;', reset.replace('SVAL_DEFAULT_LEADER_TIMEOUT;', ''))


if __name__ == '__main__':
    unittest.main()
