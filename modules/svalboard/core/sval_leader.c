// Copyright 2025 Ira Cooper <ira@wakeful.net>
// Copyright 2026 Google LLC
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "sval_qmk_settings.h"
#include "quantum.h"
#include "leader.h"

#ifdef SVAL_LEADER_ENABLE

enum {
    STATE_OFF,      // Leader inactive.
    STATE_ACTIVE,   // Accepting sequence keys.
};

enum { LEADER_MAX_LENGTH = 5 };

static struct {
    keyrecord_t record;
    uint16_t    match_keycode;
    uint16_t    timer;
    uint16_t    seq[LEADER_MAX_LENGTH];
    uint8_t     num_seq;
    uint8_t     num_matched;
    uint8_t     state;
} leader = {0};

// Dynamic timeout variable - referenced by LEADER_TIMEOUT macro
uint16_t sval_leader_timeout = SVAL_DEFAULT_LEADER_TIMEOUT;

// RAM cache of leader entries for fast matching
static sval_leader_entry_t leader_entries[SVAL_LEADER_ENTRIES];

void sval_reload_leader(void) {
    // Update timeout from settings
    sval_leader_timeout = sval_get_leader_timeout();

    for (uint16_t i = 0; i < SVAL_LEADER_ENTRIES; i++) {
        sval_get_leader(i, &leader_entries[i]);
    }
}

static void sval_leader_reset_timer(void) {
    leader.timer = timer_read() + sval_leader_timeout;
}

static void sval_leader_cancel(void) {
    if (leader.state != STATE_OFF) {
        dprintf("sval_leader: Canceled.\n");
        leader.state = STATE_OFF;
    }
}

static void set_finished_state(void) {
#    ifndef NO_DEBUG
    if (debug_enable && leader.match_keycode != KC_NO) {
        dprintf("sval_leader: Finished, output: %s\n", get_keycode_string(leader.match_keycode));
    }
#    endif // NO_DEBUG

    leader.state = STATE_OFF;
}

static void sval_leader_finish(void) {
    if (leader.state != STATE_ACTIVE) {
        return;
    }

    const uint8_t saved_mods = get_mods();
#    ifndef NO_ACTION_ONESHOT
    const uint8_t saved_oneshot_mods = get_oneshot_mods();
#    endif // NO_ACTION_ONESHOT
    if (leader.num_seq > leader.num_matched) {
        clear_mods();
#    ifndef NO_ACTION_ONESHOT
        clear_oneshot_mods();
#    endif // NO_ACTION_ONESHOT
        send_keyboard_report();
    }

    if (leader.match_keycode != KC_NO) {
        set_finished_state();
        sval_keycode_tap(leader.match_keycode);
    } else {
        sval_leader_cancel();
    }

    for (uint8_t i = leader.num_matched; i < leader.num_seq; ++i) {
        sval_keycode_tap(leader.seq[i]);
    }

    if (leader.num_seq > leader.num_matched) {
        set_mods(saved_mods);
#    ifndef NO_ACTION_ONESHOT
        set_oneshot_mods(saved_oneshot_mods);
#    endif // NO_ACTION_ONESHOT
        send_keyboard_report();
    }
}

// Returns true if this key event should be ignored by the
// process_record_sval_leader() handler.
static bool is_ignored_key(uint16_t keycode, keyrecord_t *record) {
    switch (keycode) {
        case QK_MOMENTARY ... QK_MOMENTARY_MAX:
        case QK_TO ... QK_TO_MAX:
        case QK_TOGGLE_LAYER ... QK_TOGGLE_LAYER_MAX:
        case QK_LAYER_TAP_TOGGLE ... QK_LAYER_TAP_TOGGLE_MAX:
#    ifndef NO_ACTION_TAPPING
        case QK_LAYER_TAP ... QK_LAYER_TAP_MAX:
        case QK_MOD_TAP ... QK_MOD_TAP_MAX:
            return record->tap.count == 0;
#    endif // NO_ACTION_TAPPING
#    ifndef NO_ACTION_ONESHOT
        case QK_ONE_SHOT_LAYER ... QK_ONE_SHOT_LAYER_MAX:
        case QK_ONE_SHOT_MOD ... QK_ONE_SHOT_MOD_MAX:
#    endif // NO_ACTION_ONESHOT
        case MODIFIER_KEYCODE_RANGE:
            return true;
    }
    return false;
}

static void sval_leader_set_match(uint16_t output) {
    leader.match_keycode = output;
    leader.num_matched   = leader.num_seq;
}

// Appends a key to the buffer. Returns true on success, false if full.
static bool append_to_buffer(uint16_t keycode, keyrecord_t *record) {
    if (leader.num_seq >= LEADER_MAX_LENGTH) {
        sval_leader_cancel();
        return false;
    }

    leader.seq[leader.num_seq++] = keycode;
#    ifndef NO_DEBUG
    if (debug_enable) {
        dprintf("sval_leader: seq = { ");
        for (int8_t i = 0; i < leader.num_seq; ++i) {
            dprintf("%s ", get_keycode_string(leader.seq[i]));
        }
        dprintf("}\n");
    }
#    endif // NO_DEBUG

    return true;
}

// Searches for a matching leader sequence. Returns whether default handling
// should continue for this key event.
static bool find_sequence(uint16_t keycode, keyrecord_t *record) {
    bool partial_match = false;
    sval_leader_reset_timer();

    for (uint16_t i = 0; i < SVAL_LEADER_ENTRIES; ++i) {
        const sval_leader_entry_t *entry = &leader_entries[i];

        // Skip disabled entries
        if (!(entry->options & sval_leader_enabled)) {
            continue;
        }

        if (!memcmp(leader.seq, entry->sequence, sizeof(uint16_t) * leader.num_seq)) {
            if (leader.num_seq == LEADER_MAX_LENGTH || entry->sequence[leader.num_seq] == 0) { // Full match.
                sval_leader_set_match(entry->output);
            } else { // Partial match.
                partial_match = true;
            }
        }
    }

    if (leader.num_matched == leader.num_seq && record) {
        leader.record.event.key = record->event.key;
    }

    const uint8_t active_mods = (get_mods()
#    ifndef NO_ACTION_ONESHOT
                                 | get_oneshot_mods()
#    endif // NO_ACTION_ONESHOT
    );
    if (!partial_match || active_mods) {
        if (leader.num_matched == leader.num_seq) {
            // Current key completed a sequence unambiguously.
            sval_leader_finish();
        } else { // Current key follows a (possibly failed) leader sequence.
            const uint8_t saved_mods = get_mods();
#    ifndef NO_ACTION_ONESHOT
            const uint8_t saved_oneshot_mods = get_oneshot_mods();
#    endif // NO_ACTION_ONESHOT
            clear_mods();
#    ifndef NO_ACTION_ONESHOT
            clear_oneshot_mods();
#    endif // NO_ACTION_ONESHOT
            send_keyboard_report();

            --leader.num_seq;
            sval_leader_finish();

            set_mods(saved_mods);
#    ifndef NO_ACTION_ONESHOT
            set_oneshot_mods(saved_oneshot_mods);
#    endif // NO_ACTION_ONESHOT
            send_keyboard_report();

            if (!record) {
                sval_keycode_tap(keycode);
            }
            return true;
        }
    }

    return false;
}

static bool sval_leader_start_internal(keyrecord_t *record) {
    if (leader.state != STATE_ACTIVE) {
        sval_leader_cancel();
        leader.state          = STATE_ACTIVE;
        leader.num_seq        = 0;
        leader.num_matched    = 0;
        leader.record.event   = MAKE_KEYEVENT(0, 0, true);
        leader.record.keycode = KC_NO;
        leader.match_keycode  = KC_NO;
        memset(leader.seq, 0, LEADER_MAX_LENGTH * sizeof(uint16_t));
        dprintf("sval_leader: Started.\n");
    } else {
        append_to_buffer(QK_LEAD, record);
    }

    return find_sequence(QK_LEAD, record);
}

bool process_record_sval_leader(uint16_t keycode, keyrecord_t *record) {
    if (is_ignored_key(keycode, record)) {
        return true;
    } else if (!record->event.pressed) {
        return keycode != QK_LEAD;
    } else if (leader.state != STATE_ACTIVE) {
        if (keycode == QK_LEAD) {
            return sval_leader_start_internal(record);
        }
        return true;
    }

    keycode = get_tap_keycode(keycode);
    return leader.state != STATE_ACTIVE || !append_to_buffer(keycode, record) || find_sequence(keycode, record);
}

void housekeeping_task_sval_leader(void) {
    if (leader.state == STATE_ACTIVE && timer_expired(timer_read(), leader.timer)) {
        dprintf("sval_leader: Timed out.\n");
        sval_leader_finish();
    }
}

#else

// Stub when leader is disabled
void sval_reload_leader(void) {}

#endif // SVAL_LEADER_ENABLE
