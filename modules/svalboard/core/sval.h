// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "action.h"
#include "sval_qmk_settings.h"

// Include generated config (defines SVAL_*_ENTRIES and SVAL_*_ENABLE)
// This is generated from sval.json by sval_config.py
#if __has_include("sval_config.h")
#    include "sval_config.h"
#endif

// Macros 128-255: QMK's own macro keycodes stop at 127 (QK_MACRO_MAX), so the
// upper half lives in the unassigned block just below them.
#define SVAL_MACRO_HIGH_BASE 0x7680 // macro 128
#define SVAL_MACRO_HIGH_MAX 0x76FF  // macro 255

// Sval protocol version
// v2: 16-bit entry and label indices; LABEL_GET returns one label per request.
// v3: macro buffer commands with 32-bit offsets (the buffer passes 64 KB), and
//     TABLE_SCAN, so a host reads only the entries in use.
#define SVAL_PROTOCOL_VERSION 0x00000003

// Keyboard UID - use VIAL_KEYBOARD_UID for backwards compatibility with .vil files
#ifndef SVAL_KEYBOARD_UID
#    ifdef VIAL_KEYBOARD_UID
#        define SVAL_KEYBOARD_UID VIAL_KEYBOARD_UID
#    else
#        define SVAL_KEYBOARD_UID {0, 0, 0, 0, 0, 0, 0, 0}
#    endif
#endif

// Protocol prefix for 0xDF direct protocol
#define SVAL_PREFIX 0xDF

// USB serial number magic for GUI/web detection
#ifndef SERIAL_NUMBER
#    define SERIAL_NUMBER "sval:12345-00"
#endif

// Sval command IDs (0xDF protocol; additive capabilities advertised by GET_INFO)
enum sval_command_id {
    sval_cmd_get_info           = 0x00,
    sval_cmd_tap_dance_get      = 0x01,
    sval_cmd_tap_dance_set      = 0x02,
    sval_cmd_combo_get          = 0x03,
    sval_cmd_combo_set          = 0x04,
    sval_cmd_key_override_get   = 0x05,
    sval_cmd_key_override_set   = 0x06,
    sval_cmd_alt_repeat_key_get = 0x07,
    sval_cmd_alt_repeat_key_set = 0x08,
    sval_cmd_one_shot_get       = 0x09, // QMK settings 6 and 5 (one-shot timeout, tap toggle)
    sval_cmd_one_shot_set       = 0x0A, // as 0x09, writing them
    sval_cmd_save               = 0x0B,
    sval_cmd_reset              = 0x0C,
    sval_cmd_definition_size    = 0x0D,
    sval_cmd_definition_chunk   = 0x0E,
    // QMK Settings commands
    sval_cmd_qmk_settings_query = 0x10,
    sval_cmd_qmk_settings_get   = 0x11,
    sval_cmd_qmk_settings_set   = 0x12,
    sval_cmd_qmk_settings_reset = 0x13,
    // Leader commands
    sval_cmd_leader_get = 0x14,
    sval_cmd_leader_set = 0x15,
    // Layer state commands (32-bit masks; GET appends defaults with capability bit 6)
    sval_cmd_layer_state_get = 0x16,
    sval_cmd_layer_state_set = 0x17,
    // Fragment commands (hardware detection and EEPROM selection)
    sval_cmd_fragment_get_hardware   = 0x18,
    sval_cmd_fragment_get_selections = 0x19,
    sval_cmd_fragment_set_selections = 0x1A,
    // Label commands (user-defined names for layers, tap dances, etc.)
    sval_cmd_label_get   = 0x1B,
    sval_cmd_label_set   = 0x1C,
    sval_cmd_label_clear = 0x1D,

    sval_cmd_macro_buffer_size    = 0x1E, // v3: 32-bit offsets, the whole buffer
    sval_cmd_macro_buffer_get     = 0x1F,
    sval_cmd_macro_buffer_set     = 0x20,
    sval_cmd_table_scan           = 0x21, // v3: next used entry at or after an index
    sval_cmd_context_layer_set    = 0x26,
    sval_cmd_context_layer_status = 0x27,
    sval_cmd_context_layer_renew  = 0x28,
    sval_cmd_context_layer_clear  = 0x29,
    sval_cmd_storage_reset_clear  = 0x2A, // host has told the user about a storage reset (GET_INFO byte 18)
    sval_cmd_default_layer_set    = 0x2B, // advertised by sval_flag2_default_layer_set
    sval_cmd_pointing_hardware    = 0x2C, // advertised by sval_flag2_pointing_hardware
    sval_cmd_pointing_motion      = 0x2D, // advertised by sval_flag2_pointing_motion
    sval_cmd_pointing_contacts    = 0x2F, // advertised by sval_flag2_pointing_contacts
    sval_cmd_side                 = 0x33, // advertised by sval_flag2_side (either-hand builds)
    sval_cmd_error                = 0xFF,
};

// Feature capability flags (returned in protocol info)
enum sval_feature_flags {
    sval_flag_caps_word           = (1 << 0),
    sval_flag_layer_lock          = (1 << 1),
    sval_flag_oneshot             = (1 << 2),
    sval_flag_leader              = (1 << 3),
    sval_flag_context_layer       = (1 << 5), // context-layer commands available
    sval_flag_default_layer_state = (1 << 6), // 0x16 includes default_layer_state
    // Bit 4 is allocated by the context feature branches; bit 7 reserved.
};

// Second feature byte, GET_INFO byte 19 (after the storage flags in byte 18).
// Older firmware leaves it zero.
enum sval_feature_flags2 {
    sval_flag2_default_layer_set = (1 << 0), // 0x2B sets the saved default layer
    sval_flag2_pointing_hardware = (1 << 1), // 0x2C reports each half's side and pointing sensor
    sval_flag2_modifier_state    = (1 << 2), // 0x16 appends the board's modifiers, Caps Word and Caps Lock
    sval_flag2_pointing_motion   = (1 << 3), // 0x2D reports pointing motion since the last read
    // Bit 4 is allocated by the per-reader motion branch (0x2D reader ids).
    sval_flag2_pointing_contacts = (1 << 5), // 0x2F reports each touch contact on its pad
    sval_flag2_side              = (1 << 6), // 0x33 reads and sets which half is which (either-hand builds)
};

// Status byte after the modifiers in the 0x16 reply (sval_flag2_modifier_state).
enum sval_modifier_status {
    sval_status_caps_word = (1 << 0), // Caps Word is on
    sval_status_caps_lock = (1 << 1), // the host's Caps Lock LED is on
};

// Keyboard definition chunk size (fits a wrapped 32-byte report with its header,
// so a host written for those keeps working; 64-byte reports have room to spare)
#define SVAL_DEFINITION_CHUNK_SIZE 22

// Entry counts - set by sval_config.h from sval.json
// Features not in sval.json get 0 entries (disabled)
#ifndef SVAL_TAP_DANCE_ENTRIES
#    define SVAL_TAP_DANCE_ENTRIES 0
#endif

#ifndef SVAL_COMBO_ENTRIES
#    define SVAL_COMBO_ENTRIES 0
#endif

#ifndef SVAL_KEY_OVERRIDE_ENTRIES
#    define SVAL_KEY_OVERRIDE_ENTRIES 0
#endif

#ifndef SVAL_ALT_REPEAT_KEY_ENTRIES
#    define SVAL_ALT_REPEAT_KEY_ENTRIES 0
#endif

#ifndef SVAL_LEADER_ENTRIES
#    define SVAL_LEADER_ENTRIES 0
#endif

// Maximum fragment instances (fixed-size protocol buffer)
#define SVAL_FRAGMENT_MAX_INSTANCES 21

// Fragment ID indicating no detection or no selection
#define SVAL_FRAGMENT_ID_NONE 0xFF

// Tap Dance entry structure (10 bytes)
// Enabled when custom_tapping_term bit 15 = 1
typedef struct __attribute__((packed)) {
    uint16_t on_tap;
    uint16_t on_hold;
    uint16_t on_double_tap;
    uint16_t on_tap_hold;
    uint16_t custom_tapping_term; // bit 15 = enabled, bits 0-14 = timing (ms)
} sval_tap_dance_entry_t;
_Static_assert(sizeof(sval_tap_dance_entry_t) == 10, "sval_tap_dance_entry_t must be 10 bytes");
// Check if tap dance entry is enabled (bit 15 of custom_tapping_term)
#define TD_ENABLED(entry) ((entry).custom_tapping_term & 0x8000)

// Term stored in every tap dance slot on reset, in ms. A term of 0 breaks a tap dance.
#ifndef SVAL_TAP_DANCE_DEFAULT_TERM
#    define SVAL_TAP_DANCE_DEFAULT_TERM 200
#endif

// Combo entry structure (12 bytes)
// Enabled when custom_combo_term bit 15 = 1
typedef struct __attribute__((packed)) {
    uint16_t input[4];          // Up to 4 trigger keys (0x0000 = unused)
    uint16_t output;            // Output keycode
    uint16_t custom_combo_term; // bit 15 = enabled, bits 0-14 = timing (ms)
} sval_combo_entry_t;
_Static_assert(sizeof(sval_combo_entry_t) == 12, "sval_combo_entry_t must be 12 bytes");

// Key Override entry structure (12 bytes)
// Enabled when options bit 7 = 1
typedef struct __attribute__((packed)) {
    uint16_t trigger;           // Trigger keycode
    uint16_t replacement;       // Replacement keycode
    uint32_t layers;            // Layer mask (bit per layer, 32 layers)
    uint8_t  trigger_mods;      // Required modifiers
    uint8_t  negative_mod_mask; // Modifiers that cancel override
    uint8_t  suppressed_mods;   // Modifiers to suppress
    uint8_t  options;           // Option flags (bit 7 = enabled)
} sval_key_override_entry_t;
_Static_assert(sizeof(sval_key_override_entry_t) == 12, "sval_key_override_entry_t must be 12 bytes");

// Key override option bits
enum sval_key_override_options {
    sval_ko_option_activation_trigger_down         = (1 << 0),
    sval_ko_option_activation_required_mod_down    = (1 << 1),
    sval_ko_option_activation_negative_mod_up      = (1 << 2),
    sval_ko_option_one_mod                         = (1 << 3),
    sval_ko_option_no_reregister_trigger           = (1 << 4),
    sval_ko_option_no_unregister_on_other_key_down = (1 << 5),
    // bit 6 reserved
    sval_ko_enabled = (1 << 7),
};

// Alt Repeat Key entry structure (6 bytes)
// Enabled when options bit 3 = 1
typedef struct __attribute__((packed)) {
    uint16_t keycode;      // Original keycode to match
    uint16_t alt_keycode;  // Alternate keycode to send on repeat
    uint8_t  allowed_mods; // Modifier mask for matching
    uint8_t  options;      // Option flags (bit 3 = enabled)
} sval_alt_repeat_key_entry_t;
_Static_assert(sizeof(sval_alt_repeat_key_entry_t) == 6, "sval_alt_repeat_key_entry_t must be 6 bytes");

// Alt repeat key option bits
enum sval_alt_repeat_key_options {
    sval_ark_option_default_to_alt        = (1 << 0),
    sval_ark_option_bidirectional         = (1 << 1),
    sval_ark_option_ignore_mod_handedness = (1 << 2),
    sval_ark_enabled                      = (1 << 3),
    // bits 4-7 reserved
};

// Leader entry structure (14 bytes)
// Enabled when options bit 15 = 1
typedef struct __attribute__((packed)) {
    uint16_t sequence[5]; // Up to 5 keys in order (0x0000 = unused/end)
    uint16_t output;      // Output keycode
    uint16_t options;     // bit 15 = enabled, bits 0-14 = reserved
} sval_leader_entry_t;
_Static_assert(sizeof(sval_leader_entry_t) == 14, "sval_leader_entry_t must be 14 bytes");

// Leader option bits
enum sval_leader_options {
    sval_leader_enabled = (1 << 15),
    // bits 0-14 reserved
};

// Label types - what kind of item the label describes
enum sval_label_type {
    sval_label_type_layer     = 0,
    sval_label_type_tap_dance = 1,
    sval_label_type_macro     = 2,
};

// Label System v2: Fixed 16-byte UTF-8 storage
// Label size constant - shared by all label types (layer, TD, macro)
#define SVAL_LABEL_SIZE 16

// Defaults for layer/macro counts if not defined
#ifndef DYNAMIC_KEYMAP_LAYER_COUNT
#    define DYNAMIC_KEYMAP_LAYER_COUNT 4
#endif

#ifndef DYNAMIC_KEYMAP_MACRO_COUNT
#    define DYNAMIC_KEYMAP_MACRO_COUNT 16
#endif

// Label storage arrays (v2: fixed SVAL_LABEL_SIZE-byte UTF-8 per entry)
extern char sval_td_labels[SVAL_TAP_DANCE_ENTRIES][SVAL_LABEL_SIZE];
extern char sval_macro_labels[DYNAMIC_KEYMAP_MACRO_COUNT][SVAL_LABEL_SIZE];
extern char sval_layer_labels[DYNAMIC_KEYMAP_LAYER_COUNT][SVAL_LABEL_SIZE];

// EEPROM layout constants - shared across all sval modules
#define SVAL_TAP_DANCE_OFFSET 0
#define SVAL_TAP_DANCE_SIZE (SVAL_TAP_DANCE_ENTRIES * sizeof(sval_tap_dance_entry_t))

#define SVAL_COMBO_OFFSET (SVAL_TAP_DANCE_OFFSET + SVAL_TAP_DANCE_SIZE)
#define SVAL_COMBO_SIZE (SVAL_COMBO_ENTRIES * sizeof(sval_combo_entry_t))

#define SVAL_KEY_OVERRIDE_OFFSET (SVAL_COMBO_OFFSET + SVAL_COMBO_SIZE)
#define SVAL_KEY_OVERRIDE_SIZE (SVAL_KEY_OVERRIDE_ENTRIES * sizeof(sval_key_override_entry_t))

#define SVAL_ALT_REPEAT_KEY_OFFSET (SVAL_KEY_OVERRIDE_OFFSET + SVAL_KEY_OVERRIDE_SIZE)
#define SVAL_ALT_REPEAT_KEY_SIZE (SVAL_ALT_REPEAT_KEY_ENTRIES * sizeof(sval_alt_repeat_key_entry_t))

#define SVAL_LEADER_OFFSET (SVAL_ALT_REPEAT_KEY_OFFSET + SVAL_ALT_REPEAT_KEY_SIZE)
#define SVAL_LEADER_SIZE (SVAL_LEADER_ENTRIES * sizeof(sval_leader_entry_t))

#define SVAL_MAGIC_SIZE 6
#define SVAL_MAGIC_OFFSET (SVAL_LEADER_OFFSET + SVAL_LEADER_SIZE)

#define SVAL_QMK_SETTINGS_OFFSET (SVAL_MAGIC_OFFSET + SVAL_MAGIC_SIZE)
// SVAL_QMK_SETTINGS_SIZE is defined in post_config.h (44 bytes)

#define SVAL_FRAGMENT_OFFSET (SVAL_QMK_SETTINGS_OFFSET + SVAL_QMK_SETTINGS_SIZE)
#define SVAL_FRAGMENT_SIZE SVAL_FRAGMENT_MAX_INSTANCES // 21 bytes

// Label System v2: Fixed SVAL_LABEL_SIZE-byte arrays per type
#define SVAL_TD_LABEL_OFFSET (SVAL_FRAGMENT_OFFSET + SVAL_FRAGMENT_SIZE)
#define SVAL_TD_LABEL_SIZE (SVAL_TAP_DANCE_ENTRIES * SVAL_LABEL_SIZE)

#define SVAL_MACRO_LABEL_OFFSET (SVAL_TD_LABEL_OFFSET + SVAL_TD_LABEL_SIZE)
#define SVAL_MACRO_LABEL_SIZE (DYNAMIC_KEYMAP_MACRO_COUNT * SVAL_LABEL_SIZE)

#define SVAL_LAYER_LABEL_OFFSET (SVAL_MACRO_LABEL_OFFSET + SVAL_MACRO_LABEL_SIZE)
#define SVAL_LAYER_LABEL_SIZE (DYNAMIC_KEYMAP_LAYER_COUNT * SVAL_LABEL_SIZE)

// Nonzero after settings were reset because storage could not be read, until the host clears it
#define SVAL_STORAGE_RESET_OFFSET (SVAL_LAYER_LABEL_OFFSET + SVAL_LAYER_LABEL_SIZE)
#define SVAL_STORAGE_RESET_SIZE 1

// Total EEPROM size (all sval storage areas)
#define SVAL_EEPROM_SIZE (SVAL_STORAGE_RESET_OFFSET + SVAL_STORAGE_RESET_SIZE)

// Public API
void sval_init(void);
// Mark the stored Sval data block invalid; sval_init() then resets it.
void sval_eeprom_invalidate(void);
// Whether settings were reset because storage could not be read, and the host has not yet cleared it.
// Keyboard hooks for GET_INFO byte 18 (weak, return 0) and for 0x2A.
uint8_t sval_storage_flags_kb(void);
void    sval_storage_reset_clear_kb(void);
bool    sval_storage_was_reset(void);
// Whether, since this boot, storage could not be opened or a change could not be saved to flash.
bool sval_storage_write_failed(void);

// Protocol handler for 0xDF commands
// Returns true if command was handled
bool sval_handle_command(uint8_t *data, uint8_t length);

// Storage API - Tap Dance
int sval_get_tap_dance(uint16_t index, sval_tap_dance_entry_t *entry);
int sval_set_tap_dance(uint16_t index, const sval_tap_dance_entry_t *entry);

// Storage API - Combo
int sval_get_combo(uint16_t index, sval_combo_entry_t *entry);
int sval_set_combo(uint16_t index, const sval_combo_entry_t *entry);

// Storage API - Key Override
int sval_get_key_override(uint16_t index, sval_key_override_entry_t *entry);
int sval_set_key_override(uint16_t index, const sval_key_override_entry_t *entry);

// Storage API - Alt Repeat Key
int sval_get_alt_repeat_key(uint16_t index, sval_alt_repeat_key_entry_t *entry);
int sval_set_alt_repeat_key(uint16_t index, const sval_alt_repeat_key_entry_t *entry);

// Storage API - One-Shot

// Storage API - Leader
int sval_get_leader(uint16_t index, sval_leader_entry_t *entry);
int sval_set_leader(uint16_t index, const sval_leader_entry_t *entry);

// Administrative functions
void sval_save(void);
void sval_reset(void);

// Get feature flags for protocol info response
uint8_t sval_get_feature_flags(void);
uint8_t sval_get_feature_flags2(void);

// Reload functions (called after settings change)
void sval_reload_tap_dance(void);
void sval_reload_combo(void);
void sval_reload_key_override(void);
void sval_reload_alt_repeat_key(void);
void sval_reload_leader(void);
void sval_reload_labels(void);

// Keycode execution helpers
// Called once for every raw HID packet the host sends, before any dispatch.
// Weak no-op in the module; a keyboard overrides it to notice host activity
// (for example to leave a low-power idle while a config app is talking).
void sval_host_packet_kb(void);

// POINTING_HARDWARE (0x2C): the keyboard fills out[0..length-1] (already zeroed)
// with its report. Weak no-op in the module; out[0] = 0 means nothing to report.
void sval_pointing_hardware_kb(uint8_t *out, uint8_t length);

// POINTING_MOTION (0x2D): the keyboard fills out[0..length-1] (already zeroed) with
// the motion since the previous read, then clears it. Weak no-op in the module;
// out[0] = 0 means no data.
void sval_pointing_motion_kb(uint8_t *out, uint8_t length);

// POINTING_CONTACTS (0x2F): the keyboard fills out[0..length-1] (already zeroed)
// with the touch contacts down now. Weak no-op in the module; out[0] = 0 means
// no data.
void sval_pointing_contacts_kb(uint8_t *out, uint8_t length);

// SIDE (0x33, SVAL_EITHER_HAND builds only): the keyboard handles data after
// [0xDF] [0x33] in place: [op] [status] [...].
void sval_side_command(uint8_t *data, uint8_t length);

void sval_keycode_down(uint16_t keycode);
void sval_keycode_up(uint16_t keycode);
void sval_keycode_tap(uint16_t keycode);

// Keyboard definition functions
uint32_t sval_get_definition_size(void);
uint8_t  sval_get_definition_chunk(uint16_t offset, uint8_t *buffer, uint8_t max_size);

// Weak keyboard hook for post-init
void keyboard_post_init_core_kb(void);

// Tap dance process_record hook
bool process_record_sval_tap_dance(uint16_t keycode, keyrecord_t *record);

// Fragment detection and selection functions
// These are weak - keyboard can override for hardware-specific detection

// Get hardware-detected fragment ID for an instance (weak, default returns 0xFF)
// Return 0xFF if no detection available for this instance
uint8_t sval_fragment_detect(uint8_t instance_idx);

// Get number of fragment instances from keyboard definition
// This is extracted from the JSON at build time
uint8_t sval_fragment_get_instance_count(void);

// Get EEPROM-stored selection for an instance
uint8_t sval_fragment_get_selection(uint8_t instance_idx);

// Set EEPROM-stored selection for an instance
void sval_fragment_set_selection(uint8_t instance_idx, uint8_t fragment_id);

// Protocol handlers for fragment commands
bool sval_handle_fragment_get_hardware(uint8_t *data, uint8_t length);
bool sval_handle_fragment_get_selections(uint8_t *data, uint8_t length);
bool sval_handle_fragment_set_selections(uint8_t *data, uint8_t length);

// Storage API - Labels (v2: fixed SVAL_LABEL_SIZE-byte UTF-8 storage)
// Get label for a specific type+index (returns actual length, 0 if empty)
// Buffer receives exactly SVAL_LABEL_SIZE bytes from storage (null-padded if shorter)
uint8_t sval_get_label(uint8_t label_type, uint16_t index, char *buffer, uint8_t buffer_size);
// Set label for a specific type+index (max SVAL_LABEL_SIZE bytes, truncated if longer)
// Returns 0 on success, -1 on error (invalid type/index)
int sval_set_label(uint8_t label_type, uint16_t index, const char *string, uint8_t length);
// Clear label for a specific type+index (sets all SVAL_LABEL_SIZE bytes to 0x00)
// Returns 0 on success, -1 on error (invalid type/index)
int sval_clear_label(uint8_t label_type, uint16_t index);

// Volatile application layer, independent of manual layer_state.
uint8_t sval_context_layer(void);
bool    sval_context_layer_command(uint8_t *data, uint8_t length);
