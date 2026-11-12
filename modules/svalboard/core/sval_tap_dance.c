// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"

#ifdef TAP_DANCE_ENABLE

#    include "process_tap_dance.h"

// Dance state tracking
enum { SINGLE_TAP = 1, SINGLE_HOLD, DOUBLE_TAP, DOUBLE_HOLD, DOUBLE_SINGLE_TAP, MORE_TAPS };

static uint8_t                dance_state[SVAL_TAP_DANCE_ENTRIES];
static sval_tap_dance_entry_t td_entry;

static uint8_t dance_step(tap_dance_state_t *state) {
    if (state->count == 1) {
        if (state->interrupted || !state->pressed)
            return SINGLE_TAP;
        else
            return SINGLE_HOLD;
    } else if (state->count == 2) {
        if (state->interrupted)
            return DOUBLE_SINGLE_TAP;
        else if (state->pressed)
            return DOUBLE_HOLD;
        else
            return DOUBLE_TAP;
    }
    return MORE_TAPS;
}

static void on_dance(tap_dance_state_t *state, void *user_data) {
    uint16_t index = (uintptr_t)user_data;
    if (sval_get_tap_dance(index, &td_entry) != 0) return;
    if (!TD_ENABLED(td_entry)) return;
    uint16_t kc = td_entry.on_tap;
    if (kc) {
        if (state->count == 3) {
            sval_keycode_tap(kc);
            sval_keycode_tap(kc);
            sval_keycode_tap(kc);
        } else if (state->count > 3) {
            sval_keycode_tap(kc);
        }
    }
}

static void on_dance_finished(tap_dance_state_t *state, void *user_data) {
    uint16_t index = (uintptr_t)user_data;
    if (sval_get_tap_dance(index, &td_entry) != 0) return;
    if (!TD_ENABLED(td_entry)) return;
    dance_state[index] = dance_step(state);
    switch (dance_state[index]) {
        case SINGLE_TAP: {
            if (td_entry.on_tap) sval_keycode_down(td_entry.on_tap);
            break;
        }
        case SINGLE_HOLD: {
            if (td_entry.on_hold)
                sval_keycode_down(td_entry.on_hold);
            else if (td_entry.on_tap)
                sval_keycode_down(td_entry.on_tap);
            break;
        }
        case DOUBLE_TAP: {
            if (td_entry.on_double_tap) {
                sval_keycode_down(td_entry.on_double_tap);
            } else if (td_entry.on_tap) {
                sval_keycode_tap(td_entry.on_tap);
                sval_keycode_down(td_entry.on_tap);
            }
            break;
        }
        case DOUBLE_HOLD: {
            if (td_entry.on_tap_hold) {
                sval_keycode_down(td_entry.on_tap_hold);
            } else {
                if (td_entry.on_tap) {
                    sval_keycode_tap(td_entry.on_tap);
                    if (td_entry.on_hold)
                        sval_keycode_down(td_entry.on_hold);
                    else
                        sval_keycode_down(td_entry.on_tap);
                } else if (td_entry.on_hold) {
                    sval_keycode_down(td_entry.on_hold);
                }
            }
            break;
        }
        case DOUBLE_SINGLE_TAP: {
            if (td_entry.on_tap) {
                sval_keycode_tap(td_entry.on_tap);
                sval_keycode_down(td_entry.on_tap);
            }
            break;
        }
    }
}

static void on_dance_reset(tap_dance_state_t *state, void *user_data) {
    uint16_t index = (uintptr_t)user_data;
    if (sval_get_tap_dance(index, &td_entry) != 0) return;
    if (!TD_ENABLED(td_entry)) return;
    wait_ms(get_tap_code_delay());
    uint8_t st         = dance_state[index];
    state->count       = 0;
    dance_state[index] = 0;
    switch (st) {
        case SINGLE_TAP: {
            if (td_entry.on_tap) sval_keycode_up(td_entry.on_tap);
            break;
        }
        case SINGLE_HOLD: {
            if (td_entry.on_hold)
                sval_keycode_up(td_entry.on_hold);
            else if (td_entry.on_tap)
                sval_keycode_up(td_entry.on_tap);
            break;
        }
        case DOUBLE_TAP: {
            if (td_entry.on_double_tap) {
                sval_keycode_up(td_entry.on_double_tap);
            } else if (td_entry.on_tap) {
                sval_keycode_up(td_entry.on_tap);
            }
            break;
        }
        case DOUBLE_HOLD: {
            if (td_entry.on_tap_hold) {
                sval_keycode_up(td_entry.on_tap_hold);
            } else {
                if (td_entry.on_tap) {
                    if (td_entry.on_hold)
                        sval_keycode_up(td_entry.on_hold);
                    else
                        sval_keycode_up(td_entry.on_tap);
                } else if (td_entry.on_hold) {
                    sval_keycode_up(td_entry.on_hold);
                }
            }
            break;
        }
        case DOUBLE_SINGLE_TAP: {
            if (td_entry.on_tap) {
                sval_keycode_up(td_entry.on_tap);
            }
            break;
        }
    }
}

// Finish a dance on its release when no later tap can change the result, as
// the Vial firmware did: a dance with only Tap and Hold on its first release,
// and a dance with Double-tap on its second. Otherwise the dance waits for its
// term, and a key pressed before then interrupts it: a fast double-tap followed
// by another key gave Tap instead of Double-tap.
static void on_dance_release(tap_dance_state_t *state, void *user_data) {
    uint16_t index = (uintptr_t)user_data;
    if (state->finished) return;
    if (sval_get_tap_dance(index, &td_entry) != 0) return;
    if (!TD_ENABLED(td_entry)) return;
    bool tap_and_hold_only = td_entry.on_tap && td_entry.on_hold && !td_entry.on_double_tap && !td_entry.on_tap_hold;
    if (!((state->count == 1 && tap_and_hold_only) || (state->count == 2 && td_entry.on_double_tap))) return;
    // As process_tap_dance_action_on_dance_finished() (static in QMK) does.
    // process_tap_dance() then resets the dance on this release.
    state->pressed  = false;
    state->finished = true;
    add_weak_mods(state->weak_mods);
#    ifndef NO_ACTION_ONESHOT
    add_mods(state->oneshot_mods);
#    endif
    send_keyboard_report();
    on_dance_finished(state, user_data);
}

// Storage for tap dance actions
static tap_dance_action_t sval_tap_dance_actions[SVAL_TAP_DANCE_ENTRIES];

void sval_reload_tap_dance(void) {
    for (size_t i = 0; i < SVAL_TAP_DANCE_ENTRIES; ++i) {
        sval_tap_dance_actions[i].fn.on_each_tap       = on_dance;
        sval_tap_dance_actions[i].fn.on_dance_finished = on_dance_finished;
        sval_tap_dance_actions[i].fn.on_reset          = on_dance_reset;
        sval_tap_dance_actions[i].fn.on_each_release   = on_dance_release;
        sval_tap_dance_actions[i].user_data            = (void *)(uintptr_t)i;
    }
}

// Override the introspection functions
uint16_t tap_dance_count(void) {
    return SVAL_TAP_DANCE_ENTRIES;
}

tap_dance_action_t *tap_dance_get(uint16_t tap_dance_idx) {
    if (tap_dance_idx >= SVAL_TAP_DANCE_ENTRIES) {
        return NULL;
    }
    return &sval_tap_dance_actions[tap_dance_idx];
}

// Stub - tap dance processing is handled by QMK's standard mechanism
bool process_record_sval_tap_dance(uint16_t keycode, keyrecord_t *record) {
    return true; // Continue processing
}

#else
// Stubs when TAP_DANCE_ENABLE is not defined
void sval_reload_tap_dance(void) {}
bool process_record_sval_tap_dance(uint16_t keycode, keyrecord_t *record) {
    return true;
}
#endif
