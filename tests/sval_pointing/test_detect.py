"""Run pointing_detect.c's probes against simulated modules on the host; no device is accessed."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


class PointingDetectTests(unittest.TestCase):
    def test_probes_find_each_module_and_leave_the_pins_released(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'detect'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Werror', '-DSVAL_DETECT_HOST_TEST', '-I',
                            str(HERE), '-I',
                            str(ROOT / 'keyboards/svalboard'),
                            str(ROOT / 'keyboards/svalboard/pointing_detect.c'),
                            str(HERE / 'sim.c'), '-o',
                            str(binary)],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


class PointerReprobeTests(unittest.TestCase):
    """pointer_reprobe.c: the runtime search's schedule, stopping after a find, never probing a working half."""
    def test_search_schedule_and_stopping(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'reprobe'
            subprocess.run([
                'gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DSVAL_REPROBE_HOST_TEST', '-I',
                str(HERE), '-I',
                str(ROOT / 'keyboards/svalboard'),
                str(ROOT / 'keyboards/svalboard/pointer_reprobe.c'),
                str(HERE / 'reprobe_test.c'), '-o',
                str(binary)
            ],
                           check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


class PointingMotionTests(unittest.TestCase):
    """pointing_motion.h: fragment ids for each detected module (0x18) and the 0x2D packing."""
    def test_fragment_mapping_and_motion_packing(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / 'sval_config.h'
            subprocess.run([sys.executable, '-I', str(ROOT / 'modules/svalboard/core/sval_config.py'), str(ROOT / 'keyboards/svalboard/keymaps/sval/sval.json'), str(header)], check=True, capture_output=True)
            binary = Path(directory) / 'motion'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', directory, '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'motion_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    def test_both_keymaps_define_the_pointing_instances(self):
        for keymap in ('sval', 'blank'):
            data = json.loads((ROOT / f'keyboards/svalboard/keymaps/{keymap}/sval.json').read_text())
            ids = [inst['id'] for inst in data['composition']['instances']]
            self.assertEqual(ids[10:], ['left_pointing', 'right_pointing'], keymap)
            for inst in data['composition']['instances'][10:]:
                self.assertTrue(inst['hardware_detect'])
                for opt in inst['fragment_options']:
                    frag = data['fragments'][opt['fragment']]
                    self.assertEqual(frag['kle'], [])
                    self.assertEqual(frag['widget']['kind'], 'pointing')
                    self.assertEqual(opt['matrix_map'], [])


class VelocityScrollTests(unittest.TestCase):
    """velocity_scroll.c: the speed curve, trackball offset and clamp, touchpad anchors, axis lock, output."""
    def test_velocity_scroll_math(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'velocity'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'velocity_test.c'), str(ROOT / 'keyboards/svalboard/velocity_scroll.c'), '-lm', '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    def test_pad_follow_math(self):
        # pad_follow.c: a grabbed touchpad finger as relative counts at the hand's DPI.
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'pad_follow'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'pad_follow_test.c'), str(ROOT / 'keyboards/svalboard/pad_follow.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    def test_axis_factor(self):
        # axis_scale.h: sniper and boost factors clamp to 255, never wrap.
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'axis_factor'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'axis_factor_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)

    def test_trackball_health_check(self):
        # trackball.c: the once-a-second check is read-only, catches a reset or stuck sensor,
        # leaves a half that never had a sensor to the search, and gives up on a gone one.
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'trackball_health'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(HERE / 'trackball_stubs'), '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'trackball_health_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # The PMW3360 driver's get_cpi writes Config1: the check must not use it.
            binary = Path(directory) / 'trackball_health_3360'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DSIM_PMW3360', '-I', str(HERE / 'trackball_stubs'), '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'trackball_health_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # The per-module build, which has the search's own retry (sval_pointer_retry_own).
            binary = Path(directory) / 'trackball_health_nonauto'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-DSIM_NONAUTO', '-I', str(HERE / 'trackball_stubs'), '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'trackball_health_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_keycodes_are_positions_30_and_31_in_both_keymaps(self):
        for keymap in ('sval', 'blank'):
            data = json.loads((ROOT / f'keyboards/svalboard/keymaps/{keymap}/sval.json').read_text())
            names = [k['name'] for k in data['customKeycodes']]
            self.assertEqual(names.index('SV_VELOCITY_SCROLL'), 30, keymap)
            self.assertEqual(names.index('SV_VELOCITY_SCROLL_TOGGLE'), 31, keymap)
            self.assertLessEqual(len(names), 32, keymap)  # QK_USER_31 is the last the GUI addresses

    def test_toggle_key_is_handled_like_the_hold_key(self):
        # keymap_support.c: both keys on press and release, and in the mouse layer's passthrough list.
        source = (ROOT / 'keyboards/svalboard/keymaps/keymap_support.c').read_text(encoding='utf-8')
        self.assertEqual(source.count('case SV_VELOCITY_SCROLL:\n            case SV_VELOCITY_SCROLL_TOGGLE:'), 2)
        passthrough = source[source.index('#define MOUSE_PASSTHROUGH_KEYCODE'):]
        passthrough = passthrough[:passthrough.index(')\n')]
        self.assertIn('SV_VELOCITY_SCROLL_TOGGLE', passthrough)


class PointingSensorTests(unittest.TestCase):
    """pointing_squal.h: trackball sensor bins and the POINTING_SENSOR (0x39) SAMPLES and STATS packing."""
    def test_squal_packing(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'squal'
            subprocess.run(['gcc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'keyboards/svalboard'), str(HERE / 'squal_test.c'), '-o', str(binary)], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('OK', result.stdout)


if __name__ == '__main__':
    unittest.main()
