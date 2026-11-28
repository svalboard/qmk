"""PS/2 TrackPoint host tests: the RP2040 receiver (ps2_vendor.c) and the recalibrate command (trackpoint.c). No device is accessed."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def build_and_run(test, source, includes):
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / 'test'
        args = ['gcc', '-std=gnu11', '-Wall', '-Werror']
        for inc in includes:
            args += ['-I', str(inc)]
        subprocess.run(args + [str(HERE / source), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        test.assertIn('OK', result.stdout)


class Ps2ReceiverTests(unittest.TestCase):
    """ps2_vendor.c: a full queue never storms the interrupt (F1); lost or malformed frames resync the receiver (F11)."""
    def test_receiver_queue_and_resync(self):
        build_and_run(self, 'ps2_vendor_test.c', [HERE / 'vendor_stubs', ROOT / 'platforms/chibios/drivers/vendor/RP/RP2040'])


class TrackPointRecalibrateTests(unittest.TestCase):
    """trackpoint.c: recalibrate stops the stream, reads only its own ACKs and leaves the packets aligned (F8)."""
    def test_recalibrate_while_streaming(self):
        build_and_run(self, 'trackpoint_test.c', [HERE / 'tp_stubs', ROOT / 'modules/svalboard/pointing_device_ps2', ROOT / 'keyboards/svalboard'])


if __name__ == '__main__':
    unittest.main()
