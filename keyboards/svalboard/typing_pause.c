// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later
// Typing pause, Sval 0x3C (typing_pause.h, docs/protocol.md "Typing pause").
#include "quantum.h"
#include "host.h"
#include "host_driver.h"
#include "usb_main.h"
#include "client_wrapper.h"
#include "typing_pause.h"
#include <string.h>

static uint8_t        state = SVAL_TYPING_PAUSE_STATE_OFF;
static uint8_t        last_end;
static uint32_t       owner;         // client ID that turned the pause on
static uint32_t       owner_seen;    // its last packet
static uint32_t       drain_started; // RELEASING since
static uint32_t       current_client; // the client of the packet being dispatched
static host_driver_t *saved_driver;
static host_driver_t  pause_driver;
static bool           synthesizing; // our own releases pass the swallow
static matrix_row_t   swallow[MATRIX_ROWS];
static uint16_t       blocked_keyboard, blocked_extra, passed_mouse;
static uint8_t        released_keys, empty_sent;

static void bump(uint16_t *n) {
    if (*n != UINT16_MAX) ++*n;
}

// ---- the filter ----------------------------------------------------------------
// Off (state OFF) every call forwards, so a copy of this driver someone else
// took (keytest) still works once the pause has ended.
static void pause_send_keyboard(report_keyboard_t *r) {
    if (state != SVAL_TYPING_PAUSE_STATE_OFF) {
        bump(&blocked_keyboard);
        return;
    }
    if (saved_driver && saved_driver->send_keyboard) saved_driver->send_keyboard(r);
}
static void pause_send_nkro(report_nkro_t *r) {
    if (state != SVAL_TYPING_PAUSE_STATE_OFF) {
        bump(&blocked_keyboard);
        return;
    }
    if (saved_driver && saved_driver->send_nkro) saved_driver->send_nkro(r);
}
static void pause_send_extra(report_extra_t *r) {
    if (state != SVAL_TYPING_PAUSE_STATE_OFF) {
        bump(&blocked_extra);
        return;
    }
    if (saved_driver && saved_driver->send_extra) saved_driver->send_extra(r);
}
static void pause_send_mouse(report_mouse_t *r) {
    bump(&passed_mouse);
    if (saved_driver && saved_driver->send_mouse) saved_driver->send_mouse(r);
}

// An empty keyboard report straight to the host, past QMK's "only if changed"
// check, in the format the board is using now.
static void send_empty_report(void) {
#ifdef NKRO_ENABLE
    if (host_can_send_nkro() && keymap_config.nkro) {
        report_nkro_t empty = {0};
        host_nkro_send(&empty);
        ++empty_sent;
        return;
    }
#endif
    report_keyboard_t empty = {0};
    host_keyboard_send(&empty);
    ++empty_sent;
}

static void clear_all(void) {
    clear_oneshot_mods();
#ifndef NO_ACTION_ONESHOT
    clear_oneshot_layer_state(ONESHOT_PRESSED);
#endif
    clear_keyboard(); // mods, keys, weak mods, consumer and system 0, mouse keys
}

static bool usb_active(void) {
    return USB_DRIVER.state == USB_ACTIVE;
}

static bool start(uint32_t client) {
    if (!is_keyboard_master() || !usb_active() || !client_wrapper_valid_id(client)) return false;
    if (state == SVAL_TYPING_PAUSE_STATE_OFF) {
        host_driver_t *d = host_get_driver();
        if (!d) return false;
        // Nothing left held on the host: clear through the real driver first.
        clear_all();
        send_empty_report();
        if (d != &pause_driver) {
            saved_driver               = d;
            pause_driver               = *d; // LEDs and raw HID stay the same
            pause_driver.send_keyboard = pause_send_keyboard;
            pause_driver.send_nkro     = pause_send_nkro;
            pause_driver.send_extra    = pause_send_extra;
            pause_driver.send_mouse    = pause_send_mouse;
        }
        blocked_keyboard = blocked_extra = passed_mouse = 0;
        state = SVAL_TYPING_PAUSE_STATE_PAUSED;
        host_set_driver(&pause_driver);
    } else {
        state = SVAL_TYPING_PAUSE_STATE_PAUSED; // a renew during RELEASING keeps the pause
    }
    owner      = client;
    owner_seen = timer_read32();
    return true;
}

// Release every key held now inside the pause, and swallow its real release.
static void release_held(void) {
    released_keys = 0;
    synthesizing  = true;
    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        matrix_row_t held = matrix_get_row(row) & ~swallow[row];
        for (uint8_t col = 0; col < MATRIX_COLS && held; ++col) {
            matrix_row_t bit = (matrix_row_t)1 << col;
            if (!(held & bit)) continue;
            held &= ~bit;
            swallow[row] |= bit;
            action_exec(MAKE_KEYEVENT(row, col, false));
            if (released_keys != UINT8_MAX) ++released_keys;
        }
    }
    synthesizing = false;
}

static void stop(uint8_t reason) {
    if (state != SVAL_TYPING_PAUSE_STATE_PAUSED) return;
    last_end      = reason;
    release_held();
    state         = SVAL_TYPING_PAUSE_STATE_RELEASING;
    drain_started = timer_read32();
}

static void finish(void) {
    state = SVAL_TYPING_PAUSE_STATE_OFF;
    if (host_get_driver() == &pause_driver) host_set_driver(saved_driver);
    clear_all();
    send_empty_report();
}

bool sval_typing_pause_active(void) {
    return state != SVAL_TYPING_PAUSE_STATE_OFF;
}

void sval_typing_pause_client_packet(uint32_t client_id) {
    current_client = client_id;
    if (state == SVAL_TYPING_PAUSE_STATE_PAUSED && client_id == owner && client_wrapper_valid_id(client_id)) owner_seen = timer_read32();
}

void sval_typing_pause_task(void) {
    if (state == SVAL_TYPING_PAUSE_STATE_OFF) return;
    if (!usb_active()) {
        stop(SVAL_TYPING_PAUSE_END_USB);
    } else if (state == SVAL_TYPING_PAUSE_STATE_PAUSED && timer_elapsed32(owner_seen) >= SVAL_TYPING_PAUSE_LEASE_MS) {
        stop(SVAL_TYPING_PAUSE_END_LEASE);
    }
    if (state == SVAL_TYPING_PAUSE_STATE_RELEASING && timer_elapsed32(drain_started) >= SVAL_TYPING_PAUSE_DRAIN_MS) finish();
}

bool sval_typing_pause_pre_process(keyrecord_t *record) {
    if (synthesizing || !IS_KEYEVENT(record->event)) return true;
    uint8_t row = record->event.key.row, col = record->event.key.col;
    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) return true;
    matrix_row_t bit = (matrix_row_t)1 << col;
    if (swallow[row] & bit) {
        if (!record->event.pressed) swallow[row] &= ~bit;
        return false;
    }
    if (state == SVAL_TYPING_PAUSE_STATE_RELEASING && record->event.pressed) {
        swallow[row] |= bit; // a press while ending: never reaches the keymap
        return false;
    }
    return true;
}

static void put16(uint8_t *p, uint16_t n) {
    p[0] = n & 0xFF;
    p[1] = n >> 8;
}

// Request:  [op]  (after [0xDF][0x3C])
// Response: [op] [status] [state] [last end] [flags] [lease left ms:2]
//           [keyboard blocked:2] [extra blocked:2] [mouse passed:2]
//           [keys released at last end] [empty reports sent] [keys swallowed now]
void sval_typing_pause_command(uint8_t *data, uint8_t length) {
    if (length < 16) {
        if (length > 1) data[1] = SVAL_TYPING_PAUSE_TOO_SHORT;
        return;
    }
    uint8_t op = data[0];
    memset(data + 1, 0, length - 1);
    uint8_t st = SVAL_TYPING_PAUSE_OK;
    switch (op) {
        case SVAL_TYPING_PAUSE_ON:
            if (!start(current_client)) st = SVAL_TYPING_PAUSE_REFUSED;
            break;
        case SVAL_TYPING_PAUSE_OFF:
            stop(SVAL_TYPING_PAUSE_END_COMMAND); // from any client: ending is always allowed
            break;
        case SVAL_TYPING_PAUSE_STATUS:
            break;
        default:
            st = SVAL_TYPING_PAUSE_BAD_OP;
            break;
    }
    data[1] = st;
    data[2] = state;
    data[3] = last_end;
    data[4] = (state != SVAL_TYPING_PAUSE_STATE_OFF && owner == current_client) ? 1 : 0;
    uint32_t left = 0;
    if (state == SVAL_TYPING_PAUSE_STATE_PAUSED) {
        uint32_t e = timer_elapsed32(owner_seen);
        left       = e < SVAL_TYPING_PAUSE_LEASE_MS ? SVAL_TYPING_PAUSE_LEASE_MS - e : 0;
    }
    put16(data + 5, (uint16_t)left);
    put16(data + 7, blocked_keyboard);
    put16(data + 9, blocked_extra);
    put16(data + 11, passed_mouse);
    data[13] = released_keys;
    data[14] = empty_sent;
    uint8_t n = 0;
    for (uint8_t row = 0; row < MATRIX_ROWS; ++row) {
        for (matrix_row_t r = swallow[row]; r; r &= r - 1) ++n;
    }
    data[15] = n;
}

#ifndef SVAL_EITHER_HAND
void sval_rgb_idle_input(void); // rgb_idle.c

// The either-hand builds call this from side.c's pre_process_record_kb.
bool pre_process_record_kb(uint16_t keycode, keyrecord_t *record) {
    if (!sval_typing_pause_pre_process(record)) return false;
    if (IS_KEYEVENT(record->event) && record->event.pressed) sval_rgb_idle_input(); // undim before the keycode runs
    return pre_process_record_user(keycode, record);
}
#endif
