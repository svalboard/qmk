"""Host tests for the settings area's record store (keyboards/svalboard/settings_area.c).

Run: python3 -m unittest discover -s tests/sval_storage -v

The production file is compiled against a RAM store. It covers a blank and a
foreign area, reading and writing records, the two slots, a power cut at every
byte of a save (each setting then reads as its old or its new value), unknown
and retired ids, records stored shorter or longer than this build's, a full
area, a damaged list, and the sequence number wrapping. It also checks the
capacity budget in settings_ids.h (tools/settings_budget.py).
"""
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
KB = ROOT / 'keyboards/svalboard'

HARNESS = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings_area.h"

#define SIZE 1024
static uint8_t mem[SIZE];
static long budget = -1; // bytes left before the power cut, -1 never
static long writes;
static jmp_buf cut;

static void rd(uint16_t off, void *buf, uint16_t len) { assert(off + len <= SIZE); memcpy(buf, mem + off, len); }
static void wr(uint16_t off, const void *buf, uint16_t len) {
    assert(off + len <= SIZE);
    const uint8_t *b = buf;
    for (uint16_t i = 0; i < len; i++) {
        if (mem[off + i] == b[i]) continue; // like eeprom_update_block
        if (budget == 0) longjmp(cut, 1);
        if (budget > 0) budget--;
        mem[off + i] = b[i];
        writes++;
    }
}
static settings_area_store_t store = {rd, wr, SIZE};

static int get(uint16_t id, uint8_t *v, uint16_t max) {
    uint16_t len = 0;
    memset(v, 0xEE, max);
    return settings_area_get(id, v, max, &len) ? len : -1;
}

static void test_blank_and_format(void) {
    memset(mem, 0, SIZE);
    assert(!settings_area_valid());
    uint8_t v[8];
    assert(get(1, v, 8) == -1);
    settings_area_format();
    assert(settings_area_valid());
    assert(get(1, v, 8) == -1);
    memset(mem, 0xA5, SIZE); // foreign bytes (an old macro buffer)
    assert(!settings_area_valid());
    assert(get(1, v, 8) == -1);
}

static void test_put_get_and_slots(void) {
    memset(mem, 0, SIZE);
    uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, v[8];
    assert(settings_area_put(SVAL_SET_POINTER, a, 8)); // formats a blank area
    assert(settings_area_valid());
    assert(get(SVAL_SET_POINTER, v, 8) == 8 && !memcmp(v, a, 8));
    writes = 0;
    assert(settings_area_put(SVAL_SET_POINTER, a, 8));
    assert(writes == 0); // unchanged: nothing written
    for (int round = 0; round < 600; round++) { // through several sequence wraps
        a[0] = (uint8_t)round;
        a[7] = (uint8_t)(round * 7);
        assert(settings_area_put(SVAL_SET_POINTER, a, 8));
        assert(get(SVAL_SET_POINTER, v, 8) == 8 && !memcmp(v, a, 8));
    }
    uint8_t c[48];
    for (int i = 0; i < 48; i++) c[i] = (uint8_t)(i * 3);
    assert(settings_area_put(SVAL_SET_LAYER_COLORS, c, 48));
    uint8_t w[48];
    assert(get(SVAL_SET_LAYER_COLORS, w, 48) == 48 && !memcmp(w, c, 48));
    assert(get(SVAL_SET_POINTER, v, 8) == 8 && !memcmp(v, a, 8));
}

// Cut the power at every byte of a save: afterwards each record reads as its
// old or its new value, and the next save works.
static void test_power_cut(void) {
    for (long at = 0;; at++) {
        memset(mem, 0, SIZE);
        uint8_t old[8] = {9, 9, 9, 9, 9, 9, 9, 9}, nw[8] = {1, 2, 3, 4, 5, 6, 7, 8}, other[16] = {7}, v[16];
        settings_area_put(SVAL_SET_POINTER, old, 8);
        settings_area_put(SVAL_SET_SCAN, other, 16);
        budget = at;
        int finished = 0;
        if (!setjmp(cut)) {
            settings_area_put(SVAL_SET_POINTER, nw, 8);
            finished = 1;
        }
        budget = -1;
        int n = get(SVAL_SET_POINTER, v, 8);
        assert(n == 8 && (!memcmp(v, old, 8) || !memcmp(v, nw, 8)));
        if (finished) assert(!memcmp(v, nw, 8));
        assert(get(SVAL_SET_SCAN, v, 16) == 16 && !memcmp(v, other, 16));
        uint8_t again[8] = {4, 4, 4, 4, 4, 4, 4, 4};
        assert(settings_area_put(SVAL_SET_POINTER, again, 8));
        assert(get(SVAL_SET_POINTER, v, 8) == 8 && !memcmp(v, again, 8));
        if (finished) break;
    }
    // A cut while a new record is added: it reads as missing or as the value.
    for (long at = 0;; at++) {
        memset(mem, 0, SIZE);
        uint8_t a[8] = {1}, b[2] = {5, 6}, v[8];
        settings_area_put(SVAL_SET_POINTER, a, 8);
        budget = at;
        int finished = 0;
        if (!setjmp(cut)) {
            settings_area_put(SVAL_SET_PAD_MODE, b, 2);
            finished = 1;
        }
        budget = -1;
        int n = get(SVAL_SET_PAD_MODE, v, 2);
        assert(n == -1 || (n == 2 && v[0] == 5 && v[1] == 6));
        if (finished) assert(n == 2);
        assert(get(SVAL_SET_POINTER, v, 8) == 8 && v[0] == 1);
        assert(settings_area_put(SVAL_SET_PAD_MODE, b, 2));
        assert(get(SVAL_SET_PAD_MODE, v, 2) == 2 && v[0] == 5);
        if (finished) break;
    }
}

static void test_lengths_and_ids(void) {
    memset(mem, 0, SIZE);
    uint8_t v[16];
    // Unknown id (newer firmware) between known ones: skipped.
    uint8_t u[20] = {42};
    assert(settings_area_put(900, u, 20));
    uint8_t s[2] = {3, 4};
    assert(settings_area_put(SVAL_SET_PAD_PALM, s, 2));
    assert(get(SVAL_SET_PAD_PALM, v, 2) == 2 && v[0] == 3);
    // Stored shorter than asked (older firmware): the front is filled, len says so.
    uint8_t short_rec[5] = {1, 2, 3, 4, 5};
    assert(settings_area_put(SVAL_SET_SCAN, short_rec, 5));
    memset(v, 0xEE, 16);
    uint16_t len = 0;
    assert(settings_area_get(SVAL_SET_SCAN, v, 16, &len) && len == 5);
    assert(v[4] == 5 && v[5] == 0xEE);
    // Stored longer (newer firmware): cut to what this build asks for.
    assert(get(900, v, 4) == 20 && v[0] == 42);
    // Grows past its entry's capacity: the entry is retired and a new one used.
    uint8_t big[30];
    for (int i = 0; i < 30; i++) big[i] = (uint8_t)i;
    assert(settings_area_put(SVAL_SET_PAD_PALM, big, 30));
    uint8_t w[30];
    assert(get(SVAL_SET_PAD_PALM, w, 30) == 30 && !memcmp(w, big, 30));
    assert(get(900, v, 4) == 20 && v[0] == 42);
    assert(!settings_area_put(0, v, 1) && !settings_area_put(SVAL_SETTINGS_RETIRED_ID, v, 1));
}

static void test_full_and_damaged(void) {
    memset(mem, 0, SIZE);
    uint8_t blob[200] = {1}, v[8];
    int added = 0;
    for (uint16_t id = 1000; settings_area_put(id, blob, 200); id++) added++;
    assert(added == 2); // 1024 bytes: two 412-byte entries, no third
    assert(get(1000, v, 8) == 200 && get(1001, v, 8) == 200);
    // A list with a capacity running past the end: read as the end, no crash.
    memset(mem, 0, SIZE);
    settings_area_format();
    uint8_t a[8] = {1};
    settings_area_put(SVAL_SET_POINTER, a, 8);
    mem[8 + 2] = 0xFF; mem[8 + 3] = 0x7F; // the first entry's capacity
    assert(get(SVAL_SET_POINTER, v, 8) == -1);
    // Random bytes behind a valid header: never a crash or a read out of bounds.
    srand(7);
    for (int round = 0; round < 2000; round++) {
        for (int i = 8; i < SIZE; i++) mem[i] = (uint8_t)rand();
        if (round & 1) { mem[8] = 1; mem[9] = 0; }
        get(SVAL_SET_POINTER, v, 8);
        settings_area_put(SVAL_SET_SCAN, a, 8);
    }
}

int main(void) {
    settings_area_use(&store);
    test_blank_and_format();
    test_put_get_and_slots();
    test_power_cut();
    test_lengths_and_ids();
    test_full_and_damaged();
    puts("settings area: ok");
    return 0;
}
'''


class SettingsArea(unittest.TestCase):
    def test_record_store(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / 'test.c').write_text(HARNESS)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-I', str(KB), str(path / 'test.c'), str(KB / 'settings_area.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_budget(self):
        result = subprocess.run([sys.executable, str(KB / 'tools/settings_budget.py')], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('settings area:', result.stdout)


if __name__ == '__main__':
    unittest.main()
