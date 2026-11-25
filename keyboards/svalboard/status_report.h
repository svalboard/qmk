// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// The board's status report, in one place for both of its outputs:
// - STATUS_REPORT (Sval 0x3F) returns it as data, for a host app to show
//   (docs/protocol.md, "Status report").
// - SV_OUTPUT_STATUS types it as text. The text uses only characters that sit
//   on the same keys on the common QWERTY and QWERTZ layouts (US, UK, German,
//   Swiss, Nordic, Spanish, Italian, Portuguese): letters other than y and z,
//   digits, space, comma, period and Enter. send_string() types US key
//   positions, so any other character comes out as something else on one of
//   those layouts. AZERTY moves digits and more letters; there, read the
//   report in Keybard.
//
// Pure: no QMK headers, so the host tests build it on their own.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SVAL_STATUS_FORMAT 1

// Request [0xDF] [0x3F] [page] [arg]; the reply's data after [0xDF] [0x3F]:
// [page] [format] [page data...]. format 0: nothing to report (or an unknown page).
#define SVAL_STATUS_PAGE_STATE 0 // the fields below, packed (SVAL_STATUS_STATE_LEN bytes)
#define SVAL_STATUS_PAGE_BUILD 1 // the build text from byte [arg]: [total] [offset] [n] [n bytes]

// Page 0, offsets from the page byte.
enum sval_status_offset {
    SVAL_STATUS_OFF_PAGE        = 0,
    SVAL_STATUS_OFF_FORMAT      = 1,
    SVAL_STATUS_OFF_FLAGS_A     = 2,  // SVAL_STATUS_A_*
    SVAL_STATUS_OFF_FLAGS_B     = 3,  // SVAL_STATUS_B_*
    SVAL_STATUS_OFF_HOST_OS     = 4,  // QMK os_variant_t: 0 not known, 1 Linux, 2 Windows, 3 macOS, 4 iOS
    SVAL_STATUS_OFF_LEFT_CPI    = 5,  // u16
    SVAL_STATUS_OFF_RIGHT_CPI   = 7,  // u16
    SVAL_STATUS_OFF_AM_TIMEOUT  = 9,  // u16 ms (0 with SVAL_STATUS_B_AM_NO_TIMEOUT)
    SVAL_STATUS_OFF_BOOST_HOLD  = 11, // 3 bytes: Boost 2x, 3x, 5x keys held
    SVAL_STATUS_OFF_BOOST_TG    = 14, // bit 0 Boost 2x toggled on, bit 1 3x, bit 2 5x
    SVAL_STATUS_OFF_BOOST_MULT  = 15, // the Boost multiplier in force (1: off)
    SVAL_STATUS_OFF_PREWAIT     = 16, // u16 us
    SVAL_STATUS_OFF_POSTWAIT    = 18, // u16 us
    SVAL_STATUS_OFF_PERIOD      = 20, // u16 us (0: unpaced)
    SVAL_STATUS_OFF_IDLE_PERIOD = 22, // u16 ms
    SVAL_STATUS_OFF_IDLE_AFTER  = 24, // u16 ms (0: never)
    SVAL_STATUS_OFF_DEEP_PERIOD = 26, // u16 ms
    SVAL_STATUS_OFF_DEEP_AFTER  = 28, // u16 s (0: never)
    SVAL_STATUS_OFF_FRAME_US    = 30, // u32 measured frame
    SVAL_STATUS_OFF_LED_US      = 34, // u16 measured LED on time
    SVAL_STATUS_OFF_STAGE       = 36, // idle stage now: 0 active, 1 light idle, 2 deep idle
    SVAL_STATUS_OFF_IDLE_FLAGS  = 37, // SVAL_IDLE_* (config.h): 0x01 pointer rest, 0x02 RGB dim, 0x04 CPU sleep, 0x08 low clock, 0x10 long naps
    SVAL_STATUS_OFF_DEEP_MHZ    = 38, // configured deep idle clock, MHz
    SVAL_STATUS_OFF_CLOCK_MHZ   = 39, // clock now, MHz
    SVAL_STATUS_STATE_LEN       = 40,
};

enum sval_status_flags_a {
    SVAL_STATUS_A_LEFT_SCROLL      = 1 << 0, // Scroll Mode on the left pointer
    SVAL_STATUS_A_RIGHT_SCROLL     = 1 << 1,
    SVAL_STATUS_A_AXIS_LOCK        = 1 << 2, // the Axis Scroll Lock setting (not applied on a Mac)
    SVAL_STATUS_A_NATURAL_SCROLL   = 1 << 3,
    SVAL_STATUS_A_AUTO_MOUSE       = 1 << 4, // Auto Mouse on
    SVAL_STATUS_A_LEFT_AUTO_MOUSE  = 1 << 5, // the left pointer turns Auto Mouse on
    SVAL_STATUS_A_RIGHT_AUTO_MOUSE = 1 << 6,
    SVAL_STATUS_A_MAC              = 1 << 7, // the board handles the computer as a Mac (is_mac)
};

enum sval_status_flags_b {
    SVAL_STATUS_B_HW_REV_B         = 1 << 0, // hardware revision B (flipfet), else A
    SVAL_STATUS_B_OPTICAL_DEBOUNCE = 1 << 1, // the optical keys are debounced now
    SVAL_STATUS_B_STORAGE_RESET    = 1 << 2, // settings were reset: storage could not be read
    SVAL_STATUS_B_WRITE_FAILED     = 1 << 3, // a change could not be saved since this boot
    SVAL_STATUS_B_AM_NO_TIMEOUT    = 1 << 4, // Auto Mouse has no timeout
};

typedef struct {
    uint8_t  flags_a, flags_b, host_os;
    uint16_t left_cpi, right_cpi;
    int32_t  am_timeout_ms; // -1: no timeout
    uint8_t  boost_hold[3]; // 2x, 3x, 5x
    uint8_t  boost_toggle;  // bit 0 2x, bit 1 3x, bit 2 5x
    uint8_t  boost_mult;
    uint16_t prewait_us, postwait_us, period_us;
    uint16_t idle_period_ms, idle_after_ms, deep_period_ms, deep_after_s;
    uint32_t frame_us;
    uint16_t led_us;
    uint8_t  stage, idle_flags, deep_mhz, clock_mhz;
} sval_status_t;

// Page 0 into out (len bytes, at least SVAL_STATUS_STATE_LEN; the rest is
// zeroed). Returns the bytes written, 0 if out is too short.
uint8_t sval_status_pack(const sval_status_t *s, uint8_t *out, uint8_t len);

// Page 1: from byte offset of build, into out (len bytes). Returns the bytes written.
uint8_t sval_status_pack_build(const char *build, uint8_t offset, uint8_t *out, uint8_t len);

// Whether c types the same on the layouts above (see the top of this file).
bool sval_status_safe_char(char c);

// Line `line` of the typed report into buf (NUL-terminated, ending in '\n'),
// every character safe. The build text is cleaned of unsafe characters.
// Returns false when there is no such line (the report has ended).
// Lines that don't apply (the storage notices) come back empty ("").
bool sval_status_text_line(const sval_status_t *s, const char *build, uint8_t line, char *buf, size_t n);
