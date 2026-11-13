"""Host tests for the Auto Mouse layers setting (keyboards/svalboard/automouse_layers.c).

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
#include "automouse_layers.h"
#define B(n) ((uint32_t)1 << (n))
int main(void) {
    uint8_t rec[4], mode;
    uint16_t mask;
    /* Old firmware never wrote the record: erased or zeroed bytes read as Everywhere. */
    memset(rec, 0xFF, 4);
    assert(!sval_automouse_layers_record_read(rec, &mode, &mask) && mode == 0 && mask == 0);
    memset(rec, 0x00, 4);
    assert(!sval_automouse_layers_record_read(rec, &mode, &mask) && mode == 0 && mask == 0);
    /* Round trip, both modes, every mask bit. */
    sval_automouse_layers_record_write(1, 0x8421, rec);
    assert(sval_automouse_layers_record_read(rec, &mode, &mask) && mode == 1 && mask == 0x8421);
    sval_automouse_layers_record_write(0, 0xFFFF, rec);
    assert(sval_automouse_layers_record_read(rec, &mode, &mask) && mode == 0 && mask == 0xFFFF);
    /* A corrupt byte fails the check. */
    rec[1] ^= 0x10;
    assert(!sval_automouse_layers_record_read(rec, &mode, &mask) && mode == 0 && mask == 0);
    /* An out-of-range mode written as 1. */
    sval_automouse_layers_record_write(7, 2, rec);
    assert(rec[0] == 0);

    /* The current layer: the highest active one, the mouse layer (15) left out, the default counted. */
    assert(sval_automouse_current_layer(0, B(0), 15) == 0);
    assert(sval_automouse_current_layer(B(3), B(0), 15) == 3);
    assert(sval_automouse_current_layer(B(3) | B(15), B(0), 15) == 3);
    assert(sval_automouse_current_layer(0, B(2), 15) == 2);
    assert(sval_automouse_current_layer(B(1) | B(4), B(0), 15) == 4);

    /* Everywhere: never blocked, whatever the mask. */
    assert(!sval_automouse_layer_blocked(0, 0xFFFF, B(3), B(0), 15));
    /* Selected: blocked only on a disabled current layer. */
    assert(sval_automouse_layer_blocked(1, B(3), B(3), B(0), 15));
    assert(!sval_automouse_layer_blocked(1, B(3), B(2), B(0), 15));
    assert(!sval_automouse_layer_blocked(1, B(3), 0, B(0), 15));
    assert(sval_automouse_layer_blocked(1, B(0), 0, B(0), 15));
    /* With the mouse layer on over a disabled layer, it is still that layer. */
    assert(sval_automouse_layer_blocked(1, B(3), B(3) | B(15), B(0), 15));
    /* A held layer above a disabled one is what counts. */
    assert(!sval_automouse_layer_blocked(1, B(3), B(3) | B(5), B(0), 15));
    /* Only the mouse layer's bit set: never blocks. */
    assert(!sval_automouse_layer_blocked(1, B(15), B(15), B(0), 15));
    return 0;
}
'''


class AutoMouseLayers(unittest.TestCase):
    def test_record_and_gate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(TEST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-I', str(KB), str(path / 'test.c'), str(KB / 'automouse_layers.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
