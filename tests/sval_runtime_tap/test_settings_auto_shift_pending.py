"""Host test: changing the Auto Shift bits (QSID 3) ends a pending Auto Shift key.

Run: python3 -m unittest discover -s tests/sval_runtime_tap -p 'test_settings_auto_shift_pending.py' -v

A key held within the Auto Shift timeout is pending: QMK sends it on release
or, at the timeout, shifted from the matrix scan. If a category bit (no-alpha,
no-numeric, no-special) or the repeat bits change meanwhile, the release no
longer goes through Auto Shift, so the matrix scan later types the shifted key
after the release, or holds it repeating. Applying a changed Auto Shift byte
must end the pending key first, as switching Auto Shift off does
(autoshift_disable(), which ends it in quantum/process_keycode/process_auto_shift.c).

Built from modules/svalboard/core/sval_qmk_settings.c with the same stubs as
test_settings.py; the stubbed autoshift_disable() ends the pending key.
"""
import tempfile
import unittest

from test_settings import MODULE_API, MODULE_STUBS, QMK, SWITCHES, build, source

STUBS = MODULE_STUBS.replace(
    'static void autoshift_disable(void) { autoshift_enabled = false; }',
    'bool autoshift_pending;\nstatic void autoshift_disable(void) { autoshift_pending = false; autoshift_enabled = false; }',
)

MAIN = r'''
extern bool autoshift_pending;
static void put(uint16_t q, uint16_t value) {
 uint8_t bytes[] = {value & 255, value >> 8};
 assert(sval_qmk_settings_set(q, bytes, sizeof(bytes)) == 0);
}
int main(void) {
 sval_qmk_settings_reset();
 put(3, 0x01); assert(autoshift_enabled);
 /* "a" pressed and pending; the host sets no-alpha: the key is ended, Auto Shift stays on */
 autoshift_pending = true;
 put(3, 0x11);
 assert(!autoshift_pending && autoshift_enabled);
 /* the repeat bit too */
 autoshift_pending = true;
 put(3, 0x31);
 assert(!autoshift_pending && autoshift_enabled);
 /* other settings, or the same Auto Shift byte again, leave a pending key alone */
 autoshift_pending = true;
 put(4, 200); put(7, 222); put(3, 0x31);
 assert(autoshift_pending && autoshift_enabled);
 /* switching it off ends it, as before */
 put(3, 0x00);
 assert(!autoshift_pending && !autoshift_enabled);
 /* a reload applies the stored byte without ending anything new */
 put(3, 0x01); autoshift_pending = true;
 sval_qmk_settings_init();
 assert(autoshift_pending && autoshift_enabled);
 return 0;
}
'''


class AutoShiftPending(unittest.TestCase):
    def test_changed_bits_end_a_pending_key(self):
        hooks_h = source('quantum/sval_hooks.h')
        hooks_c = source('quantum/sval_hooks.c')
        module = source('modules/svalboard/core/sval_qmk_settings.c')
        auto_shift = '#define AUTO_SHIFT_ENABLE\n' + source('modules/svalboard/core/sval_qmk_settings.h') + source('modules/svalboard/core/sval_auto_shift.c')
        prefix = SWITCHES + QMK + hooks_h
        units = [prefix + hooks_c, prefix + STUBS + module, prefix + MODULE_API + auto_shift, prefix + MODULE_API + MAIN]
        self.assertIn('autoshift_pending', STUBS)
        with tempfile.TemporaryDirectory() as directory:
            build(directory, 'pending', units)


if __name__ == '__main__':
    unittest.main()
