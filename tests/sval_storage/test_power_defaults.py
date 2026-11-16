"""Host tests that pin every power-saving mode to off by default.

Run: python3 -m unittest discover -s tests/sval_storage -v

Each build resets the settings to the firmware defaults, so a default here is
what every board runs after an update. Every power-saving mode is opt-in:
the idle_flags bits (pointer rest, RGB dim, CPU sleep, low clock, long naps),
light and deep idle on revision B, and the touchpad's LP1 low-power mode.
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KB = ROOT / 'keyboards/svalboard'


def define(path, name):
    m = re.search(r'#\s*define ' + name + r'\s+(\S+)', (KB / path).read_text())
    if not m:
        raise AssertionError(f'{name} not found in {path}')
    return m.group(1)


class PowerDefaults(unittest.TestCase):
    def test_idle_flags_all_off(self):
        self.assertEqual(define('config.h', 'SVAL_IDLE_FLAGS_DEFAULT'), '0')

    def test_reset_uses_idle_flags_default(self):
        src = (KB / 'svalboard.c').read_text()
        self.assertRegex(src, r'global_saved_values\.idle_flags\s*=\s*SVAL_IDLE_FLAGS_DEFAULT;')
        self.assertRegex(src, r'global_saved_values\.scan_deep_clock_idx\s*=\s*0;')

    def test_settings_area_keeps_the_defaults(self):
        # The power settings live in the scan record of the settings area. A
        # board with no stored record (or a bad one) must read the defaults
        # above, not zeros: the defaults are set first, and the record is read
        # over a copy packed from them.
        src = (KB / 'svalboard.c').read_text()
        ids = (KB / 'settings_ids.h').read_text()
        self.assertRegex(ids, r'X\(SVAL_SET_SCAN,\s*\d+,\s*16\)')
        self.assertRegex(src, r'r\[14\]\s*=\s*global_saved_values\.idle_flags;')
        self.assertRegex(src, r'global_saved_values\.idle_flags\s*=\s*r\[14\];')
        self.assertRegex(src, r'put16\(r \+ 8, global_saved_values\.scan_idle_after_ms\);')
        self.assertRegex(src, r'scan_pack\(r\);\s*settings_area_get\(SVAL_SET_SCAN,')
        self.assertRegex(src, r'svalboard_saved_values_defaults\(\);\s*saved_values_load\(\);')

    def test_light_idle_off(self):
        # 0 = never; the light idle period matching the active period also means no change.
        self.assertEqual(define('config.h', 'SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS'), '0')
        self.assertEqual(define('config.h', 'SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS'), '1')

    def test_deep_idle_off(self):
        self.assertEqual(define('config.h', 'SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S'), '0')

    def test_touchpad_never_enters_lp1(self):
        self.assertEqual(define('azoteq/azoteq.c', 'SVAL_IQS_IDLE_MODE_TIMEOUT_S'), '255')


if __name__ == '__main__':
    unittest.main()
