// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "client_wrapper.h"
#include "quantum.h"
#include "via.h"
#include "dynamic_keymap.h"
#include "raw_hid.h"
#include "eeprom.h"
#include <string.h>
#include "print.h"
#include "layout_stamp.h"
#include "keycode_upgrade.h"
#include "send_string.h"
#include "wait.h"

ASSERT_COMMUNITY_MODULES_MIN_API_VERSION(1, 0, 0);

// Magic position for keycode execution
#define SVAL_MATRIX_MAGIC 240

// Global for keycode override during tap dance execution
uint16_t g_sval_magic_keycode_override;

// Label System v2: Fixed SVAL_LABEL_SIZE-byte UTF-8 storage arrays
char sval_td_labels[SVAL_TAP_DANCE_ENTRIES][SVAL_LABEL_SIZE];
char sval_macro_labels[DYNAMIC_KEYMAP_MACRO_COUNT][SVAL_LABEL_SIZE];
char sval_layer_labels[DYNAMIC_KEYMAP_LAYER_COUNT][SVAL_LABEL_SIZE];

// Internal EEPROM access functions - uses eeconfig_kb_datablock
static void sval_read_eeprom(uint16_t offset, void *buf, uint16_t size) {
    eeconfig_read_kb_datablock(buf, offset, size);
}

static void sval_write_eeprom(uint16_t offset, const void *buf, uint16_t size) {
    eeconfig_update_kb_datablock(buf, offset, size);
}

// Vial macro extension codes for 16-bit keycodes
#define VIAL_MACRO_EXT_TAP 5
#define VIAL_MACRO_EXT_DOWN 6
#define VIAL_MACRO_EXT_UP 7

static uint16_t decode_keycode(uint16_t kc) {
    // Map 0xFF01 => 0x0100; 0xFF02 => 0x0200, etc.
    if (kc > 0xFF00) return (kc & 0xFF) << 8;
    return kc;
}

// Bump only when the meaning of stored bytes changes without any offset, count
// or entry size changing, e.g. a field inside an entry is reinterpreted.
#ifndef SVAL_DATA_SCHEMA
#    define SVAL_DATA_SCHEMA 0
#endif

// Magic header for EEPROM validation: a layout stamp over every offset and entry
// size in the Sval data block, so the block resets exactly when it would
// otherwise be misread. It used to be the build timestamp to the second, which
// reset every combo, tap dance, override, leader, label and QMK setting on every
// firmware update, even a rebuild of identical source.
//
// The first two bytes are fixed and are not valid BCD, so an old timestamp magic
// can never be mistaken for a stamp.
static void sval_get_magic(uint8_t *magic) {
    const uint32_t values[] = {
        SVAL_TAP_DANCE_ENTRIES, sizeof(sval_tap_dance_entry_t), SVAL_COMBO_ENTRIES, sizeof(sval_combo_entry_t), SVAL_KEY_OVERRIDE_ENTRIES, sizeof(sval_key_override_entry_t), SVAL_ALT_REPEAT_KEY_ENTRIES, sizeof(sval_alt_repeat_key_entry_t), sizeof(sval_one_shot_t), SVAL_LEADER_ENTRIES, sizeof(sval_leader_entry_t), SVAL_MAGIC_OFFSET, SVAL_QMK_SETTINGS_SIZE, SVAL_FRAGMENT_SIZE, SVAL_LABEL_SIZE, DYNAMIC_KEYMAP_MACRO_COUNT, DYNAMIC_KEYMAP_LAYER_COUNT, SVAL_EEPROM_SIZE, SVAL_DATA_SCHEMA,
    };
    uint32_t stamp = layout_stamp(values, sizeof(values) / sizeof(values[0]));
    magic[0]       = 0xA5;
    magic[1]       = 0x5A;
    magic[2]       = (stamp >> 24) & 0xFF;
    magic[3]       = (stamp >> 16) & 0xFF;
    magic[4]       = (stamp >> 8) & 0xFF;
    magic[5]       = stamp & 0xFF;
}

static bool sval_eeprom_is_valid(void) {
    uint8_t stored[SVAL_MAGIC_SIZE];
    uint8_t expected[SVAL_MAGIC_SIZE];
    sval_read_eeprom(SVAL_MAGIC_OFFSET, stored, SVAL_MAGIC_SIZE);
    sval_get_magic(expected);
    return memcmp(stored, expected, SVAL_MAGIC_SIZE) == 0;
}

void sval_eeprom_set_valid(void) {
    uint8_t magic[SVAL_MAGIC_SIZE];
    sval_get_magic(magic);
    sval_write_eeprom(SVAL_MAGIC_OFFSET, magic, SVAL_MAGIC_SIZE);
}

// Translate every keycode the Sval data block and the macros store from keycode
// version `from` to the current numbering (keycode_upgrade.h). Called from the
// keyboard's via_keycodes_upgrade_kb() during via_init(), before sval_init()
// loads the tables. Idempotent, like every keycode upgrade.
#define UPGRADE(field)                                       \
    do {                                                     \
        uint16_t upgraded_ = keycode_upgrade((field), from); \
        if (upgraded_ != (field)) {                          \
            (field) = upgraded_;                             \
            changed = true;                                  \
        }                                                    \
    } while (0)
void sval_upgrade_keycodes(uint8_t from) {
    if (sval_eeprom_is_valid()) {
        for (uint16_t i = 0; i < SVAL_TAP_DANCE_ENTRIES; i++) {
            sval_tap_dance_entry_t e;
            bool                   changed = false;
            sval_get_tap_dance(i, &e);
            UPGRADE(e.on_tap);
            UPGRADE(e.on_hold);
            UPGRADE(e.on_double_tap);
            UPGRADE(e.on_tap_hold);
            if (changed) sval_set_tap_dance(i, &e);
        }
        for (uint16_t i = 0; i < SVAL_COMBO_ENTRIES; i++) {
            sval_combo_entry_t e;
            bool               changed = false;
            sval_get_combo(i, &e);
            for (uint8_t k = 0; k < 4; k++)
                UPGRADE(e.input[k]);
            UPGRADE(e.output);
            if (changed) sval_set_combo(i, &e);
        }
        for (uint16_t i = 0; i < SVAL_KEY_OVERRIDE_ENTRIES; i++) {
            sval_key_override_entry_t e;
            bool                      changed = false;
            sval_get_key_override(i, &e);
            UPGRADE(e.trigger);
            UPGRADE(e.replacement);
            if (changed) sval_set_key_override(i, &e);
        }
        for (uint16_t i = 0; i < SVAL_ALT_REPEAT_KEY_ENTRIES; i++) {
            sval_alt_repeat_key_entry_t e;
            bool                        changed = false;
            sval_get_alt_repeat_key(i, &e);
            UPGRADE(e.keycode);
            UPGRADE(e.alt_keycode);
            if (changed) sval_set_alt_repeat_key(i, &e);
        }
        for (uint16_t i = 0; i < SVAL_LEADER_ENTRIES; i++) {
            sval_leader_entry_t e;
            bool                changed = false;
            sval_get_leader(i, &e);
            for (uint8_t k = 0; k < 5; k++)
                UPGRADE(e.sequence[k]);
            UPGRADE(e.output);
            if (changed) sval_set_leader(i, &e);
        }
    }

    // Macros: only the 16-bit extension actions carry QMK keycodes. Walk the
    // buffer byte by byte; the extension holds two non-zero bytes, so the
    // rewrite never changes its length.
    uint32_t size = dynamic_keymap_macro_get_buffer_size();
    uint8_t  b[4];
    for (uint32_t i = 0; i + 1 < size; i++) {
        dynamic_keymap_macro_get_buffer(i, 2, b);
        if (b[0] != SS_QMK_PREFIX) continue;
        if (b[1] == SS_TAP_CODE || b[1] == SS_DOWN_CODE || b[1] == SS_UP_CODE) {
            i += 2;
        } else if (b[1] == SS_DELAY_CODE) {
            i += 3;
        } else if (b[1] >= VIAL_MACRO_EXT_TAP && b[1] <= VIAL_MACRO_EXT_UP && i + 3 < size) {
            dynamic_keymap_macro_get_buffer(i + 2, 2, &b[2]);
            uint16_t raw = b[2] | (b[3] << 8);
            uint16_t kc  = decode_keycode(raw);
            uint16_t up  = keycode_upgrade(kc, from);
            if (up != kc) {
                if ((up & 0xFF) == 0) up = 0xFF00 | (up >> 8);
                b[2] = up & 0xFF;
                b[3] = up >> 8;
                dynamic_keymap_macro_set_buffer(i + 2, 2, &b[2]);
            }
            i += 3;
        }
    }
}
#undef UPGRADE

void sval_init(void) {
    // Initialize client wrapper for multi-client support
    client_wrapper_init();

    // Check if EEPROM data is valid (matches current firmware version)
    if (!sval_eeprom_is_valid()) {
        // Reset all sval data to defaults
        sval_reset();
        sval_qmk_settings_reset();
        // Mark as valid
        sval_eeprom_set_valid();
    }

    sval_reload_tap_dance();
    sval_reload_combo();
    sval_reload_key_override();
    sval_reload_alt_repeat_key();
    sval_reload_leader();
    sval_reload_labels();
    sval_qmk_settings_init();
}

// Weak keyboard post-init hook
__attribute__((weak)) void keyboard_post_init_core_kb(void) {}

// Module hook for post-init (named after directory: svalboard/core)
void keyboard_post_init_core(void) {
    keyboard_post_init_core_kb();
    sval_init();
}

// Override QMK's get_oneshot_timeout for runtime configuration
// TEMPORARILY DISABLED - may be called before EEPROM ready
// uint16_t get_oneshot_timeout(void) {
//     sval_one_shot_t settings;
//     sval_get_one_shot(&settings);
//     return settings.timeout;
// }

// Get feature flags based on what's enabled
uint8_t sval_get_feature_flags(void) {
    uint8_t flags = 0;
#ifdef CAPS_WORD_ENABLE
    flags |= sval_flag_caps_word;
#endif
#ifdef LAYER_LOCK_ENABLE
    flags |= sval_flag_layer_lock;
#endif
#ifdef ONESHOT_ENABLE
    flags |= sval_flag_oneshot;
#endif
#ifdef LEADER_ENABLE
    flags |= sval_flag_leader;
#endif
    flags |= sval_flag_context_layer;
    return flags;
}

// Storage functions - Tap Dance
int sval_get_tap_dance(uint16_t index, sval_tap_dance_entry_t *entry) {
    if (index >= SVAL_TAP_DANCE_ENTRIES) return -1;
    sval_read_eeprom(SVAL_TAP_DANCE_OFFSET + index * sizeof(sval_tap_dance_entry_t), entry, sizeof(sval_tap_dance_entry_t));
    return 0;
}

int sval_set_tap_dance(uint16_t index, const sval_tap_dance_entry_t *entry) {
    if (index >= SVAL_TAP_DANCE_ENTRIES) return -1;
    sval_write_eeprom(SVAL_TAP_DANCE_OFFSET + index * sizeof(sval_tap_dance_entry_t), entry, sizeof(sval_tap_dance_entry_t));
    return 0;
}

// Storage functions - Combo
int sval_get_combo(uint16_t index, sval_combo_entry_t *entry) {
    if (index >= SVAL_COMBO_ENTRIES) return -1;
    sval_read_eeprom(SVAL_COMBO_OFFSET + index * sizeof(sval_combo_entry_t), entry, sizeof(sval_combo_entry_t));
    return 0;
}

int sval_set_combo(uint16_t index, const sval_combo_entry_t *entry) {
    if (index >= SVAL_COMBO_ENTRIES) return -1;
    sval_write_eeprom(SVAL_COMBO_OFFSET + index * sizeof(sval_combo_entry_t), entry, sizeof(sval_combo_entry_t));
    return 0;
}

// Storage functions - Key Override
int sval_get_key_override(uint16_t index, sval_key_override_entry_t *entry) {
    if (index >= SVAL_KEY_OVERRIDE_ENTRIES) return -1;
    sval_read_eeprom(SVAL_KEY_OVERRIDE_OFFSET + index * sizeof(sval_key_override_entry_t), entry, sizeof(sval_key_override_entry_t));
    return 0;
}

int sval_set_key_override(uint16_t index, const sval_key_override_entry_t *entry) {
    if (index >= SVAL_KEY_OVERRIDE_ENTRIES) return -1;
    sval_write_eeprom(SVAL_KEY_OVERRIDE_OFFSET + index * sizeof(sval_key_override_entry_t), entry, sizeof(sval_key_override_entry_t));
    return 0;
}

// Storage functions - Alt Repeat Key
int sval_get_alt_repeat_key(uint16_t index, sval_alt_repeat_key_entry_t *entry) {
    if (index >= SVAL_ALT_REPEAT_KEY_ENTRIES) return -1;
    sval_read_eeprom(SVAL_ALT_REPEAT_KEY_OFFSET + index * sizeof(sval_alt_repeat_key_entry_t), entry, sizeof(sval_alt_repeat_key_entry_t));
    return 0;
}

int sval_set_alt_repeat_key(uint16_t index, const sval_alt_repeat_key_entry_t *entry) {
    if (index >= SVAL_ALT_REPEAT_KEY_ENTRIES) return -1;
    sval_write_eeprom(SVAL_ALT_REPEAT_KEY_OFFSET + index * sizeof(sval_alt_repeat_key_entry_t), entry, sizeof(sval_alt_repeat_key_entry_t));
    return 0;
}

// Storage functions - One-Shot
void sval_get_one_shot(sval_one_shot_t *settings) {
    sval_read_eeprom(SVAL_ONE_SHOT_OFFSET, settings, sizeof(sval_one_shot_t));
}

void sval_set_one_shot(const sval_one_shot_t *settings) {
    sval_write_eeprom(SVAL_ONE_SHOT_OFFSET, settings, sizeof(sval_one_shot_t));
}

// Storage functions - Leader
int sval_get_leader(uint16_t index, sval_leader_entry_t *entry) {
    if (index >= SVAL_LEADER_ENTRIES) return -1;
    sval_read_eeprom(SVAL_LEADER_OFFSET + index * sizeof(sval_leader_entry_t), entry, sizeof(sval_leader_entry_t));
    return 0;
}

int sval_set_leader(uint16_t index, const sval_leader_entry_t *entry) {
    if (index >= SVAL_LEADER_ENTRIES) return -1;
    sval_write_eeprom(SVAL_LEADER_OFFSET + index * sizeof(sval_leader_entry_t), entry, sizeof(sval_leader_entry_t));
    return 0;
}

// Storage functions - Labels (v2: fixed SVAL_LABEL_SIZE-byte arrays)

// Helper: Get pointer to label array and count for a given type
static char *sval_get_label_array(uint8_t label_type, uint16_t *count, uint16_t *eeprom_offset) {
    switch (label_type) {
        case sval_label_type_layer:
            *count         = DYNAMIC_KEYMAP_LAYER_COUNT;
            *eeprom_offset = SVAL_LAYER_LABEL_OFFSET;
            return (char *)sval_layer_labels;
        case sval_label_type_tap_dance:
            *count         = SVAL_TAP_DANCE_ENTRIES;
            *eeprom_offset = SVAL_TD_LABEL_OFFSET;
            return (char *)sval_td_labels;
        case sval_label_type_macro:
            *count         = DYNAMIC_KEYMAP_MACRO_COUNT;
            *eeprom_offset = SVAL_MACRO_LABEL_OFFSET;
            return (char *)sval_macro_labels;
        default:
            *count         = 0;
            *eeprom_offset = 0;
            return NULL;
    }
}

// Helper: Check if label is empty (all bytes are 0x00)
static bool sval_label_is_empty(const char *label) {
    for (uint8_t i = 0; i < SVAL_LABEL_SIZE; i++) {
        if (label[i] != 0x00) return false;
    }
    return true;
}

uint8_t sval_get_label(uint8_t label_type, uint16_t index, char *buffer, uint8_t buffer_size) {
    if (!buffer || buffer_size == 0) return 0;

    uint16_t count;
    uint16_t eeprom_offset;
    char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) {
        buffer[0] = '\0';
        return 0;
    }

    // Copy label from RAM array (exactly SVAL_LABEL_SIZE bytes)
    char *label = labels + (index * SVAL_LABEL_SIZE);

    // Find actual length (excluding trailing nulls)
    uint8_t len = 0;
    for (uint8_t i = 0; i < SVAL_LABEL_SIZE; i++) {
        if (label[i] != 0x00) {
            len = i + 1;
        }
    }

    // Copy to buffer (truncate if needed)
    if (len > buffer_size) len = buffer_size;
    if (len > 0) {
        memcpy(buffer, label, len);
    }
    if (len < buffer_size) {
        buffer[len] = '\0';
    }

    return len;
}

int sval_set_label(uint8_t label_type, uint16_t index, const char *string, uint8_t length) {
    if (!string) return -1;

    uint16_t count;
    uint16_t eeprom_offset;
    char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) return -1;

    // Truncate to SVAL_LABEL_SIZE bytes max
    if (length > SVAL_LABEL_SIZE) length = SVAL_LABEL_SIZE;

    // Get pointer to label slot
    char *label = labels + (index * SVAL_LABEL_SIZE);

    // Copy label data and null-pad remainder
    memcpy(label, string, length);
    if (length < SVAL_LABEL_SIZE) {
        memset(label + length, 0x00, SVAL_LABEL_SIZE - length);
    }

    // Write to EEPROM
    sval_write_eeprom(eeprom_offset + (index * SVAL_LABEL_SIZE), label, SVAL_LABEL_SIZE);

    return 0;
}

int sval_clear_label(uint8_t label_type, uint16_t index) {
    uint16_t count;
    uint16_t eeprom_offset;
    char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);

    if (!labels || index >= count) return -1;

    // Get pointer to label slot and clear it
    char *label = labels + (index * SVAL_LABEL_SIZE);
    memset(label, 0x00, SVAL_LABEL_SIZE);

    // Write to EEPROM
    sval_write_eeprom(eeprom_offset + (index * SVAL_LABEL_SIZE), label, SVAL_LABEL_SIZE);

    return 0;
}

// Reload all label arrays from EEPROM into RAM
void sval_reload_labels(void) {
    // Load layer labels
    sval_read_eeprom(SVAL_LAYER_LABEL_OFFSET, sval_layer_labels, DYNAMIC_KEYMAP_LAYER_COUNT * SVAL_LABEL_SIZE);

    // Load tap dance labels
    sval_read_eeprom(SVAL_TD_LABEL_OFFSET, sval_td_labels, SVAL_TAP_DANCE_ENTRIES * SVAL_LABEL_SIZE);

    // Load macro labels
    sval_read_eeprom(SVAL_MACRO_LABEL_OFFSET, sval_macro_labels, DYNAMIC_KEYMAP_MACRO_COUNT * SVAL_LABEL_SIZE);
}

void sval_save(void) {
    // Data is written directly to EEPROM, nothing additional to flush
}

void sval_reset(void) {
    // Zero out all EEPROM storage
    uint8_t zero[16] = {0};
    for (uint16_t i = 0; i < SVAL_EEPROM_SIZE; i += sizeof(zero)) {
        uint16_t chunk = sizeof(zero);
        if (i + chunk > SVAL_EEPROM_SIZE) {
            chunk = SVAL_EEPROM_SIZE - i;
        }
        sval_write_eeprom(i, zero, chunk);
    }
    // Reset VIA dynamic keymap and macros
    dynamic_keymap_reset();
    dynamic_keymap_macro_reset();
    sval_reload_tap_dance();
    sval_reload_combo();
    sval_reload_key_override();
    sval_reload_alt_repeat_key();
    sval_reload_leader();
}

// Keycode execution helpers
void sval_keycode_down(uint16_t keycode) {
    g_sval_magic_keycode_override = keycode;

    if (keycode <= QK_MODS_MAX) {
        register_code16(keycode);
    } else {
        action_exec((keyevent_t){.type = KEY_EVENT, .key = (keypos_t){.row = SVAL_MATRIX_MAGIC, .col = SVAL_MATRIX_MAGIC}, .pressed = 1, .time = (timer_read() | 1)});
    }
}

void sval_keycode_up(uint16_t keycode) {
    g_sval_magic_keycode_override = keycode;

    if (keycode <= QK_MODS_MAX) {
        unregister_code16(keycode);
    } else {
        action_exec((keyevent_t){.type = KEY_EVENT, .key = (keypos_t){.row = SVAL_MATRIX_MAGIC, .col = SVAL_MATRIX_MAGIC}, .pressed = 0, .time = (timer_read() | 1)});
    }
}

void sval_keycode_tap(uint16_t keycode) {
    sval_keycode_down(keycode);
    wait_ms(TAP_CODE_DELAY);
    sval_keycode_up(keycode);
}

// 0xDF Protocol handler
// This function should be called from via_command_kb() in the keyboard code
bool sval_handle_command(uint8_t *data, uint8_t length) {
    // data[0] = 0xDF (SVAL_PREFIX) - already verified by caller
    // data[1] = command_id
    // data[2...] = payload

    uint8_t command_id = data[1];

    if (command_id >= sval_cmd_context_layer_set && command_id <= sval_cmd_context_layer_clear) {
        return sval_context_layer_command(data, length);
    }
    switch (command_id) {
        case sval_cmd_get_info: {
            // Response: [0xDF] [0x00] [ver0-3] [uid0-7] [flags]
            // Entry counts are now in sval.json (parsed from keyboard definition)
            uint8_t uid[] = SVAL_KEYBOARD_UID;
            data[2]       = SVAL_PROTOCOL_VERSION & 0xFF;
            data[3]       = (SVAL_PROTOCOL_VERSION >> 8) & 0xFF;
            data[4]       = (SVAL_PROTOCOL_VERSION >> 16) & 0xFF;
            data[5]       = (SVAL_PROTOCOL_VERSION >> 24) & 0xFF;
            memcpy(&data[6], uid, 8);
            data[14] = sval_get_feature_flags();
            break;
        }

        case sval_cmd_tap_dance_get: {
            // Request:  [0xDF] [0x01] [index lo] [index hi]
            // Response: [0xDF] [0x01] [index lo] [index hi] [10 bytes entry]
            uint16_t               idx   = data[2] | (data[3] << 8);
            sval_tap_dance_entry_t entry = {0};
            sval_get_tap_dance(idx, &entry);
            memcpy(&data[4], &entry, sizeof(entry));
            break;
        }

        case sval_cmd_tap_dance_set: {
            // Request: [0xDF] [0x02] [index lo] [index hi] [10 bytes entry]
            // Response: [0xDF] [0x02] [status]
            if (length < 14) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t               idx = data[2] | (data[3] << 8);
            sval_tap_dance_entry_t entry;
            memcpy(&entry, &data[4], sizeof(entry));
            data[2] = sval_set_tap_dance(idx, &entry) == 0 ? 0 : 1;
            sval_reload_tap_dance();
            break;
        }

        case sval_cmd_combo_get: {
            // Request:  [0xDF] [0x03] [index lo] [index hi]
            // Response: [0xDF] [0x03] [index lo] [index hi] [12 bytes entry]
            uint16_t           idx   = data[2] | (data[3] << 8);
            sval_combo_entry_t entry = {0};
            sval_get_combo(idx, &entry);
            memcpy(&data[4], &entry, sizeof(entry));
            break;
        }

        case sval_cmd_combo_set: {
            // Request: [0xDF] [0x04] [index lo] [index hi] [12 bytes entry]
            // Response: [0xDF] [0x04] [status]
            if (length < 16) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t           idx = data[2] | (data[3] << 8);
            sval_combo_entry_t entry;
            memcpy(&entry, &data[4], sizeof(entry));
            data[2] = sval_set_combo(idx, &entry) == 0 ? 0 : 1;
            sval_reload_combo();
            break;
        }

        case sval_cmd_key_override_get: {
            // Request:  [0xDF] [0x05] [index lo] [index hi]
            // Response: [0xDF] [0x05] [index lo] [index hi] [12 bytes entry]
            uint16_t                  idx   = data[2] | (data[3] << 8);
            sval_key_override_entry_t entry = {0};
            sval_get_key_override(idx, &entry);
            memcpy(&data[4], &entry, sizeof(entry));
            break;
        }

        case sval_cmd_key_override_set: {
            // Request: [0xDF] [0x06] [index lo] [index hi] [12 bytes entry]
            // Response: [0xDF] [0x06] [status]
            if (length < 16) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t                  idx = data[2] | (data[3] << 8);
            sval_key_override_entry_t entry;
            memcpy(&entry, &data[4], sizeof(entry));
            data[2] = sval_set_key_override(idx, &entry) == 0 ? 0 : 1;
            sval_reload_key_override();
            break;
        }

        case sval_cmd_alt_repeat_key_get: {
            // Request:  [0xDF] [0x07] [index lo] [index hi]
            // Response: [0xDF] [0x07] [index lo] [index hi] [6 bytes entry]
            uint16_t                    idx   = data[2] | (data[3] << 8);
            sval_alt_repeat_key_entry_t entry = {0};
            sval_get_alt_repeat_key(idx, &entry);
            memcpy(&data[4], &entry, sizeof(entry));
            break;
        }

        case sval_cmd_alt_repeat_key_set: {
            // Request: [0xDF] [0x08] [index lo] [index hi] [6 bytes entry]
            // Response: [0xDF] [0x08] [status]
            if (length < 10) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t                    idx = data[2] | (data[3] << 8);
            sval_alt_repeat_key_entry_t entry;
            memcpy(&entry, &data[4], sizeof(entry));
            data[2] = sval_set_alt_repeat_key(idx, &entry) == 0 ? 0 : 1;
            sval_reload_alt_repeat_key();
            break;
        }

        case sval_cmd_one_shot_get: {
            // Request: [0xDF] [0x09]
            // Response: [0xDF] [0x09] [timeout_lo] [timeout_hi] [tap_toggle]
            sval_one_shot_t settings = {0};
            sval_get_one_shot(&settings);
            data[2] = settings.timeout & 0xFF;
            data[3] = (settings.timeout >> 8) & 0xFF;
            data[4] = settings.tap_toggle;
            break;
        }

        case sval_cmd_one_shot_set: {
            // Request: [0xDF] [0x0A] [timeout_lo] [timeout_hi] [tap_toggle]
            // Response: [0xDF] [0x0A]
            sval_one_shot_t settings;
            settings.timeout    = data[2] | (data[3] << 8);
            settings.tap_toggle = data[4];
            sval_set_one_shot(&settings);
            break;
        }

        case sval_cmd_save: {
            // Request: [0xDF] [0x0B]
            // Response: [0xDF] [0x0B]
            sval_save();
            break;
        }

        case sval_cmd_reset: {
            // Request: [0xDF] [0x0C]
            // Response: [0xDF] [0x0C]
            sval_reset();
            break;
        }

        case sval_cmd_definition_size: {
            // Request: [0xDF] [0x0D]
            // Response: [0xDF] [0x0D] [size0] [size1] [size2] [size3]
            uint32_t size = sval_get_definition_size();
            data[2]       = size & 0xFF;
            data[3]       = (size >> 8) & 0xFF;
            data[4]       = (size >> 16) & 0xFF;
            data[5]       = (size >> 24) & 0xFF;
            break;
        }

        case sval_cmd_definition_chunk: {
            // Request: [0xDF] [0x0E] [offset_lo] [offset_hi] [size]
            // Response: [0xDF] [0x0E] [offset_lo] [offset_hi] [actual_size] [data...]
            uint16_t offset         = data[2] | (data[3] << 8);
            uint8_t  requested_size = data[4];

            // Clamp to maximum chunk size
            if (requested_size == 0 || requested_size > SVAL_DEFINITION_CHUNK_SIZE) {
                requested_size = SVAL_DEFINITION_CHUNK_SIZE;
            }

            // Validate offset to prevent overflow
            uint32_t definition_size = sval_get_definition_size();
            if (offset >= definition_size) {
                data[4] = 0;
                break;
            }

            uint8_t actual_size = sval_get_definition_chunk(offset, &data[5], requested_size);
            data[4]             = actual_size;
            break;
        }

        case sval_cmd_qmk_settings_query: {
            // Request: [0xDF] [0x10] [qsid_lo] [qsid_hi]
            // Response: [0xDF] [0x10] [qsid1_lo] [qsid1_hi] [qsid2_lo] ... [0xFF] [0xFF]
            uint16_t qsid_gt = data[2] | (data[3] << 8);
            sval_qmk_settings_query(qsid_gt, &data[2], length - 2);
            break;
        }

        case sval_cmd_qmk_settings_get: {
            // Request: [0xDF] [0x11] [qsid_lo] [qsid_hi]
            // Response: [0xDF] [0x11] [status] [value bytes...]
            uint16_t qsid = data[2] | (data[3] << 8);
            data[2]       = sval_qmk_settings_get(qsid, &data[3], length - 3);
            break;
        }

        case sval_cmd_qmk_settings_set: {
            // Request: [0xDF] [0x12] [qsid_lo] [qsid_hi] [value bytes...]
            // Response: [0xDF] [0x12] [status]
            uint16_t qsid = data[2] | (data[3] << 8);
            data[2]       = sval_qmk_settings_set(qsid, &data[4], length - 4);
            break;
        }

        case sval_cmd_qmk_settings_reset: {
            // Request: [0xDF] [0x13]
            // Response: [0xDF] [0x13]
            sval_qmk_settings_reset();
            break;
        }

        case sval_cmd_leader_get: {
            // Request:  [0xDF] [0x14] [index lo] [index hi]
            // Response: [0xDF] [0x14] [index lo] [index hi] [14 bytes entry]
            uint16_t            idx   = data[2] | (data[3] << 8);
            sval_leader_entry_t entry = {0};
            sval_get_leader(idx, &entry);
            memcpy(&data[4], &entry, sizeof(entry));
            break;
        }

        case sval_cmd_leader_set: {
            // Request: [0xDF] [0x15] [index lo] [index hi] [14 bytes entry]
            // Response: [0xDF] [0x15] [status]
            if (length < 18) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t            idx = data[2] | (data[3] << 8);
            sval_leader_entry_t entry;
            memcpy(&entry, &data[4], sizeof(entry));
            data[2] = sval_set_leader(idx, &entry) == 0 ? 0 : 1;
            sval_reload_leader();
            break;
        }

        case sval_cmd_layer_state_get: {
            // Request: [0xDF] [0x16]
            // Response: [0xDF] [0x16] [state0] [state1] [state2] [state3]
            uint32_t state = layer_state;
            data[2]        = state & 0xFF;
            data[3]        = (state >> 8) & 0xFF;
            data[4]        = (state >> 16) & 0xFF;
            data[5]        = (state >> 24) & 0xFF;
            break;
        }

        case sval_cmd_layer_state_set: {
            // Request: [0xDF] [0x17] [state0] [state1] [state2] [state3]
            // Response: [0xDF] [0x17]
            uint32_t new_state = data[2] | (data[3] << 8) | (data[4] << 16) | (data[5] << 24);
            layer_state_set(new_state);
            break;
        }

        case sval_cmd_fragment_get_hardware:
            return sval_handle_fragment_get_hardware(data, length);

        case sval_cmd_fragment_get_selections:
            return sval_handle_fragment_get_selections(data, length);

        case sval_cmd_fragment_set_selections:
            return sval_handle_fragment_set_selections(data, length);

        case sval_cmd_table_scan: {
            // TABLE_SCAN (v3): the next entry in use at or after a start index.
            // Request:  [0xDF] [0x21] [table] [start lo] [start hi]
            //   table: 0 tap dance, 1 combo, 2 key override, 3 alt-repeat key, 4 leader
            // Response: [0xDF] [0x21] [table] [found] [index lo] [index hi] [entry...]
            //   found = 0: none in use at or after start. Entries not returned are
            //   unused (all zero). A host reads a table by scanning from index + 1.
            uint8_t  table                              = data[2];
            uint16_t i                                  = data[3] | (data[4] << 8);
            bool     found                              = false;
            uint8_t  entry[sizeof(sval_leader_entry_t)] = {0}; // the largest entry
            for (; !found; i++) {
                memset(entry, 0, sizeof(entry));
                if (table == 0 && i < SVAL_TAP_DANCE_ENTRIES) {
                    sval_tap_dance_entry_t *e = (void *)entry;
                    sval_get_tap_dance(i, e);
                    found = e->on_tap || e->on_hold || e->on_double_tap || e->on_tap_hold;
                } else if (table == 1 && i < SVAL_COMBO_ENTRIES) {
                    sval_combo_entry_t *e = (void *)entry;
                    sval_get_combo(i, e);
                    found = e->input[0] || e->output;
                } else if (table == 2 && i < SVAL_KEY_OVERRIDE_ENTRIES) {
                    sval_key_override_entry_t *e = (void *)entry;
                    sval_get_key_override(i, e);
                    found = e->trigger || e->replacement;
                } else if (table == 3 && i < SVAL_ALT_REPEAT_KEY_ENTRIES) {
                    sval_alt_repeat_key_entry_t *e = (void *)entry;
                    sval_get_alt_repeat_key(i, e);
                    found = e->keycode || e->alt_keycode;
                } else if (table == 4 && i < SVAL_LEADER_ENTRIES) {
                    sval_leader_entry_t *e = (void *)entry;
                    sval_get_leader(i, e);
                    found = e->sequence[0] || e->output;
                } else {
                    break; // past the end of the table (or no such table)
                }
                if (found) break;
            }
            data[3] = found;
            data[4] = i & 0xFF;
            data[5] = i >> 8;
            memcpy(&data[6], entry, found ? sizeof(entry) : 0);
            break;
        }

        case sval_cmd_macro_buffer_size: {
            // Request:  [0xDF] [0x1E]
            // Response: [0xDF] [0x1E] [size u32 LE]
            uint32_t size = dynamic_keymap_macro_get_buffer_size();
            data[2]       = size & 0xFF;
            data[3]       = (size >> 8) & 0xFF;
            data[4]       = (size >> 16) & 0xFF;
            data[5]       = (size >> 24) & 0xFF;
            break;
        }

        case sval_cmd_macro_buffer_get:
        case sval_cmd_macro_buffer_set: {
            // Request:  [0xDF] [0x1F|0x20] [offset u32 LE] [count] [data... (set)]
            // Response: get [0xDF] [0x1F] [offset u32 LE] [count] [data...]
            //               a refused get echoes the offset with count 0
            //           set [0xDF] [0x20] [status] (0 = ok, 1 = out of range)
            if (length < 7) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint32_t offset = data[2] | (data[3] << 8) | ((uint32_t)data[4] << 16) | ((uint32_t)data[5] << 24);
            uint8_t  count  = data[6];
            uint32_t size   = dynamic_keymap_macro_get_buffer_size();
            if (count > length - 7 || offset > size || count > size - offset) {
                if (data[1] == sval_cmd_macro_buffer_get) {
                    data[6] = 0;
                } else {
                    data[2] = 1;
                }
                break;
            }
            if (data[1] == sval_cmd_macro_buffer_get) {
                dynamic_keymap_macro_get_buffer(offset, count, &data[7]);
            } else {
                dynamic_keymap_macro_set_buffer(offset, count, &data[7]);
                data[2] = 0;
            }
            break;
        }

        case sval_cmd_label_get: {
            // LABEL_GET (v2): the next non-empty label at or after a start index.
            // Request:  [0xDF] [0x1B] [type] [start lo] [start hi]
            // Response: [0xDF] [0x1B] [type] [found] [index lo] [index hi] [SVAL_LABEL_SIZE-byte label]
            //   found = 0: no non-empty label at or after start (index = count).
            //   A host scans by asking again from index + 1.
            if (length < 6 + SVAL_LABEL_SIZE) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint8_t  label_type = data[2];
            uint16_t start      = data[3] | (data[4] << 8);
            uint16_t count, eeprom_offset;
            char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);
            if (!labels) {
                data[1] = sval_cmd_error;
                return false;
            }
            uint16_t i = start;
            while (i < count && sval_label_is_empty(labels + (i * SVAL_LABEL_SIZE)))
                i++;
            data[3] = i < count;
            data[4] = i & 0xFF;
            data[5] = i >> 8;
            memset(&data[6], 0, SVAL_LABEL_SIZE);
            if (i < count) memcpy(&data[6], labels + (i * SVAL_LABEL_SIZE), SVAL_LABEL_SIZE);
            break;
        }

        case sval_cmd_label_set: {
            // LABEL_SET (v2)
            // Request: [0xDF] [0x1C] [type] [index lo] [index hi] [SVAL_LABEL_SIZE-byte label]
            // Response: [0xDF] [0x1C] [status]
            if (length < 5 + SVAL_LABEL_SIZE) { // 2 header + 3 params + label bytes
                data[1] = sval_cmd_error;
                return false;
            }

            uint8_t        label_type = data[2];
            uint16_t       index      = data[3] | (data[4] << 8);
            const uint8_t *label      = &data[5];

            // Validate UTF-8 sequences
            // Accept: valid UTF-8 (including multi-byte), 0x00 as null terminator
            // Reject: C0/C1 overlong encodings, F5+ invalid lead bytes,
            //         invalid continuation bytes, control chars 0x01-0x1F (except tab)
            {
                bool    in_null_tail = false;
                uint8_t i            = 0;
                while (i < SVAL_LABEL_SIZE) {
                    uint8_t c = label[i];
                    if (c == 0x00) {
                        // Null byte: everything after must also be null
                        in_null_tail = true;
                        i++;
                        continue;
                    }
                    if (in_null_tail) {
                        // Non-null byte after null — invalid
                        data[2] = 0x03;
                        return true;
                    }
                    if (c >= 0x01 && c <= 0x1F && c != 0x09) {
                        // Control characters (except tab) — reject
                        data[2] = 0x03;
                        return true;
                    }
                    if (c == 0x7F) {
                        // DEL — reject
                        data[2] = 0x03;
                        return true;
                    }
                    if (c <= 0x7F) {
                        // Valid single-byte ASCII (0x20-0x7E, 0x09)
                        i++;
                        continue;
                    }
                    // Multi-byte UTF-8 sequence
                    uint8_t expected_cont = 0;
                    if (c >= 0xC2 && c <= 0xDF) {
                        expected_cont = 1; // 2-byte sequence
                    } else if (c >= 0xE0 && c <= 0xEF) {
                        expected_cont = 2; // 3-byte sequence
                    } else if (c >= 0xF0 && c <= 0xF4) {
                        expected_cont = 3; // 4-byte sequence
                    } else {
                        // Invalid lead byte (0x80-0xC1, 0xF5+)
                        data[2] = 0x03;
                        return true;
                    }
                    // Check continuation bytes exist and are valid (0x80-0xBF)
                    if (i + expected_cont >= SVAL_LABEL_SIZE) {
                        // Truncated sequence — reject
                        data[2] = 0x03;
                        return true;
                    }
                    for (uint8_t j = 1; j <= expected_cont; j++) {
                        uint8_t cb = label[i + j];
                        if (cb < 0x80 || cb > 0xBF) {
                            data[2] = 0x03;
                            return true;
                        }
                    }
                    // Reject overlong 3-byte sequences (E0 80-9F xx)
                    if (c == 0xE0 && label[i + 1] < 0xA0) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject surrogates (ED A0-BF xx)
                    if (c == 0xED && label[i + 1] > 0x9F) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject overlong 4-byte sequences (F0 80-8F xx xx)
                    if (c == 0xF0 && label[i + 1] < 0x90) {
                        data[2] = 0x03;
                        return true;
                    }
                    // Reject codepoints above U+10FFFF (F4 90+ xx xx)
                    if (c == 0xF4 && label[i + 1] > 0x8F) {
                        data[2] = 0x03;
                        return true;
                    }
                    i += 1 + expected_cont;
                }
            }

            // Attempt to set label
            int result = sval_set_label(label_type, index, (const char *)label, SVAL_LABEL_SIZE);

            // Map result to status code
            if (result == 0) {
                data[2] = 0x00; // Success
            } else {
                // Determine error type by checking parameters
                uint16_t count;
                uint16_t eeprom_offset;
                char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);
                if (!labels) {
                    data[2] = 0x01; // Invalid type
                } else if (index >= count) {
                    data[2] = 0x02; // Index out of range
                } else {
                    data[2] = 0x01; // Generic error
                }
            }
            break;
        }

        case sval_cmd_label_clear: {
            // LABEL_CLEAR (v2)
            // Request: [0xDF] [0x1D] [type] [index lo] [index hi]
            // Response: [0xDF] [0x1D] [status]
            if (length < 5) {
                data[1] = sval_cmd_error;
                return false;
            }

            uint8_t  label_type = data[2];
            uint16_t index      = data[3] | (data[4] << 8);

            // Attempt to clear label
            int result = sval_clear_label(label_type, index);

            // Map result to status code
            if (result == 0) {
                data[2] = 0x00; // Success
            } else {
                // Determine error type by checking parameters
                uint16_t count;
                uint16_t eeprom_offset;
                char    *labels = sval_get_label_array(label_type, &count, &eeprom_offset);
                if (!labels) {
                    data[2] = 0x01; // Invalid type
                } else if (index >= count) {
                    data[2] = 0x02; // Index out of range
                } else {
                    data[2] = 0x01; // Generic error
                }
            }
            break;
        }

        default:
            // Unknown command - set error response
            data[1] = sval_cmd_error;
            return false;
    }

    return true;
}

__attribute__((weak)) void sval_host_packet_kb(void) {}

// Override via_command_kb to intercept wrapper and Sval protocol
bool via_command_kb(uint8_t *data, uint8_t length) {
    // Every host packet reaches this hook, including the ones VIA handles below.
    sval_host_packet_kb();
    switch (data[0]) {
        case WRAPPER_PREFIX: // 0xDD - Client ID wrapper
            return client_wrapper_receive(data, length);

        case SVAL_PREFIX: // 0xDF - Legacy Sval - REJECTED (wrapper required)
            return true;  // "Handled" by ignoring

        default:
            return false; // Let VIA handle
    }
}

// Process record hook for Sval features. Community-module hooks are named
// after the module's directory (svalboard/core), not its module_name.
bool process_record_core(uint16_t keycode, keyrecord_t *record) {
    if (!process_record_sval_tap_dance(keycode, record)) {
        return false;
    }
    if (keycode >= SVAL_MACRO_HIGH_BASE && keycode <= SVAL_MACRO_HIGH_MAX) {
        if (record->event.pressed) dynamic_keymap_macro_send(128 + (keycode - SVAL_MACRO_HIGH_BASE));
        return false;
    }
    return true;
}

// Override keymap_key_to_keycode to handle magic position for tap dance/combo execution
uint16_t keymap_key_to_keycode(uint8_t layer, keypos_t key) {
    if (key.row == SVAL_MATRIX_MAGIC && key.col == SVAL_MATRIX_MAGIC) {
        return g_sval_magic_keycode_override;
    }
    // Use dynamic keymap for normal keys
    return dynamic_keymap_get_keycode(layer, key.row, key.col);
}

// Override dynamic_keymap_macro_send to support extended keycodes and binary delay
void dynamic_keymap_macro_send(uint8_t id) {
    if (id >= dynamic_keymap_macro_get_count()) {
        return;
    }

    // Check the last byte of the buffer for validity
    uint32_t macro_size = dynamic_keymap_macro_get_buffer_size();
    uint8_t  last_byte;
    dynamic_keymap_macro_get_buffer(macro_size - 1, 1, &last_byte);
    if (last_byte != 0) {
        return;
    }

    // Find the start of macro N by counting null terminators
    uint32_t offset = 0;
    while (id > 0) {
        if (offset >= macro_size) {
            return;
        }
        uint8_t byte;
        dynamic_keymap_macro_get_buffer(offset, 1, &byte);
        if (byte == 0) {
            --id;
        }
        ++offset;
    }

    // Process macro bytes
    char data[4] = {0, 0, 0, 0};
    while (1) {
        if (offset >= macro_size) {
            break;
        }
        memset(data, 0, sizeof(data));
        dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t *)&data[0]);
        if (data[0] == 0) {
            break;
        }
        if (data[0] == SS_QMK_PREFIX) {
            if (offset >= macro_size) break;
            dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t *)&data[1]);
            if (data[1] == 0) break;
            if (data[1] == SS_TAP_CODE || data[1] == SS_DOWN_CODE || data[1] == SS_UP_CODE) {
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t *)&data[2]);
                if (data[2] != 0) send_string(data);
            } else if (data[1] == VIAL_MACRO_EXT_TAP || data[1] == VIAL_MACRO_EXT_DOWN || data[1] == VIAL_MACRO_EXT_UP) {
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t *)&data[2]);
                if (data[2] != 0) {
                    if (offset >= macro_size) break;
                    dynamic_keymap_macro_get_buffer(offset++, 1, (uint8_t *)&data[3]);
                    if (data[3] != 0) {
                        uint16_t kc;
                        memcpy(&kc, &data[2], sizeof(kc));
                        kc = decode_keycode(kc);
                        switch (data[1]) {
                            case VIAL_MACRO_EXT_TAP:
                                sval_keycode_tap(kc);
                                break;
                            case VIAL_MACRO_EXT_DOWN:
                                sval_keycode_down(kc);
                                break;
                            case VIAL_MACRO_EXT_UP:
                                sval_keycode_up(kc);
                                break;
                        }
                    }
                }
            } else if (data[1] == SS_DELAY_CODE) {
                uint8_t d0, d1;
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, &d0);
                if (offset >= macro_size) break;
                dynamic_keymap_macro_get_buffer(offset++, 1, &d1);
                if (d0 == 0 || d1 == 0) break;
                int ms = (d0 - 1) + (d1 - 1) * 255;
                wait_ms(ms);
            }
        } else {
            send_string_with_delay(data, TAP_CODE_DELAY);
        }
    }
}
