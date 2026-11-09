#!/usr/bin/env python3
"""Build a real non-Sval RP2040 keyboard with TT() and Grave Escape enabled."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='runtime_tap_', dir=ROOT / 'keyboards/handwired/onekey/keymaps') as directory:
    path = Path(directory)
    (path / 'keymap.c').write_text(
        '''#include QMK_KEYBOARD_H
#if defined(SVAL_ENABLE) || defined(RUNTIME_QMK_SETTINGS)
#error This validation keyboard must not enable Svalboard
#endif
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    LAYOUT_ortho_1x1(TT(1)), LAYOUT_ortho_1x1(QK_GRAVE_ESCAPE)
};
'''
    )
    (path / 'config.h'
     ).write_text('''#pragma once
#define TAPPING_TOGGLE 3
#define TAP_CODE_DELAY 11
#define TAP_HOLD_CAPS_DELAY 83
#define GRAVE_ESC_ALT_OVERRIDE
#define GRAVE_ESC_CTRL_OVERRIDE
#define GRAVE_ESC_GUI_OVERRIDE
#define GRAVE_ESC_SHIFT_OVERRIDE
''')
    result = subprocess.run(['make', '-j4', f'handwired/onekey/rp2040:{path.name}'], cwd=ROOT)
    raise SystemExit(result.returncode)
