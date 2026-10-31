// Copyright 2025 Ira Cooper <ira@wakeful.net>
// SPDX-License-Identifier: GPL-2.0-or-later

#include "sval.h"
#include "quantum.h"
#include "eeprom.h"
#include <string.h>

#ifdef MOUSEKEY_ENABLE
#    include "mousekey.h"
// mousekey.c defines these two without declaring them in mousekey.h. With
// MK_KINETIC_SPEED the wheel interval is computed at run time, not set.
extern uint8_t mk_wheel_delay;
#    ifndef MK_KINETIC_SPEED
extern uint8_t mk_wheel_interval;
#    endif
#endif

#ifdef AUTO_SHIFT_ENABLE
#    include "process_auto_shift.h"
#endif

// Settings storage structure - matches Vial's qmk_settings_t for compatibility
typedef struct __attribute__((packed)) {
    uint16_t auto_shift_timeout;
    uint16_t osk_timeout;
    uint16_t mousekey_delay;
    uint16_t mousekey_interval;
    uint16_t mousekey_move_delta;
    uint16_t mousekey_max_speed;
    uint16_t mousekey_time_to_max;
    uint16_t mousekey_wheel_delay;
    uint16_t mousekey_wheel_interval;
    uint16_t mousekey_wheel_max_speed;
    uint16_t mousekey_wheel_time_to_max;
    uint16_t combo_term;
    uint16_t tapping_term;
    uint8_t  grave_esc_override;
    uint8_t  auto_shift;
    uint8_t  osk_tap_toggle;
    uint8_t  tapping_v2;
    uint16_t tap_code_delay;
    uint16_t tap_hold_caps_delay;
    uint8_t  tapping_toggle;
    uint8_t  migrations; // SVAL_QS_MIGRATED_* bits: one-time boot migrations already done (was unused, 0)
    uint16_t quick_tap_term;
    uint16_t flow_tap_term;
    uint16_t leader_timeout;
    uint8_t  leader_options; // bit 0 = per-key timing
    uint8_t  unused2;
} sval_qmk_settings_t;
_Static_assert(sizeof(sval_qmk_settings_t) == 44, "sval_qmk_settings_t must be 44 bytes");

// RAM copy of settings
static sval_qmk_settings_t settings;

// Forward declarations
static void sval_qmk_settings_load(void);
static void sval_qmk_settings_save(void);
static void sval_qmk_settings_apply(void);

// QSID definitions - must match GUI's qmk_settings.json
enum sval_qsid {
    QSID_GRAVE_ESC_OVERRIDE         = 1,
    QSID_COMBO_TERM                 = 2,
    QSID_AUTO_SHIFT                 = 3,
    QSID_AUTO_SHIFT_TIMEOUT         = 4,
    QSID_OSK_TAP_TOGGLE             = 5,
    QSID_OSK_TIMEOUT                = 6,
    QSID_TAPPING_TERM               = 7,
    QSID_TAPPING_V1                 = 8, // Legacy tapping flags
    QSID_MOUSEKEY_DELAY             = 9,
    QSID_MOUSEKEY_INTERVAL          = 10,
    QSID_MOUSEKEY_MOVE_DELTA        = 11,
    QSID_MOUSEKEY_MAX_SPEED         = 12,
    QSID_MOUSEKEY_TIME_TO_MAX       = 13,
    QSID_MOUSEKEY_WHEEL_DELAY       = 14,
    QSID_MOUSEKEY_WHEEL_INTERVAL    = 15,
    QSID_MOUSEKEY_WHEEL_MAX_SPEED   = 16,
    QSID_MOUSEKEY_WHEEL_TIME_TO_MAX = 17,
    QSID_TAP_CODE_DELAY             = 18,
    QSID_TAP_HOLD_CAPS_DELAY        = 19,
    QSID_TAPPING_TOGGLE             = 20,
    QSID_MAGIC                      = 21,
    QSID_PERMISSIVE_HOLD            = 22,
    QSID_HOLD_ON_OTHER_KEY          = 23,
    QSID_RETRO_TAPPING              = 24,
    QSID_QUICK_TAP_TERM             = 25,
    QSID_CHORDAL_HOLD               = 26,
    QSID_FLOW_TAP_TERM              = 27,
    QSID_LEADER_TIMEOUT             = 28,
    QSID_LEADER_PER_KEY_TIMING      = 29,
};

// tapping_v2 bit positions
#define TAPPING_PERMISSIVE_HOLD_BIT 0
#define TAPPING_HOLD_ON_OTHER_KEY_BIT 1
#define TAPPING_RETRO_TAPPING_BIT 2
#define TAPPING_CHORDAL_HOLD_BIT 3

// leader_options bit positions
#define LEADER_PER_KEY_TIMING_BIT 0

// auto_shift bit positions (QSID 3)
#define AUTO_SHIFT_ENABLE_BIT 0

// migrations bits
#define SVAL_QS_MIGRATED_WHEEL_BIT 0 // the wheel delay and interval are applied (see sval_qmk_settings_migrate)

// The largest value each setting's runtime variable holds. QMK's mouse key
// variables are 8-bit (mousekey.c): the intervals, speeds and times to max hold
// 0-255, and the delays, stored in ms, are applied in 10 ms units, so 0-2550 ms.
// A tap count stops at 15 (keyrecord_t tap.count is 4 bits), so a tapping
// toggle or one-shot tap toggle above 15 could never be reached. Larger stored
// values used to wrap (an interval of 256 became 0 and moved every scan); they
// are now held at these limits.
#define SVAL_MK_8BIT_MAX 255
#define SVAL_MK_DELAY_MAX_MS 2550
#define SVAL_TAP_COUNT_MAX 15
#define SVAL_AT_MOST(value, max) ((value) < (max) ? (value) : (max))

// Setting descriptor
typedef struct {
    uint16_t qsid;
    uint8_t  size;   // Size in bytes (0 = special handler)
    uint16_t offset; // Offset in settings struct
    uint8_t  bit;    // For bit fields
} sval_setting_desc_t;

// Setting descriptors - defines all supported QSIDs
static const sval_setting_desc_t setting_descs[] = {
    {QSID_GRAVE_ESC_OVERRIDE, 1, offsetof(sval_qmk_settings_t, grave_esc_override), 0},
    {QSID_COMBO_TERM, 2, offsetof(sval_qmk_settings_t, combo_term), 0},
#ifdef AUTO_SHIFT_ENABLE
    {QSID_AUTO_SHIFT, 1, offsetof(sval_qmk_settings_t, auto_shift), 0},
    {QSID_AUTO_SHIFT_TIMEOUT, 2, offsetof(sval_qmk_settings_t, auto_shift_timeout), 0},
#endif
    {QSID_OSK_TAP_TOGGLE, 1, offsetof(sval_qmk_settings_t, osk_tap_toggle), 0},
    {QSID_OSK_TIMEOUT, 2, offsetof(sval_qmk_settings_t, osk_timeout), 0},
    {QSID_TAPPING_TERM, 2, offsetof(sval_qmk_settings_t, tapping_term), 0},
#ifdef MOUSEKEY_ENABLE
    {QSID_MOUSEKEY_DELAY, 2, offsetof(sval_qmk_settings_t, mousekey_delay), 0},
    {QSID_MOUSEKEY_INTERVAL, 2, offsetof(sval_qmk_settings_t, mousekey_interval), 0},
    {QSID_MOUSEKEY_MOVE_DELTA, 2, offsetof(sval_qmk_settings_t, mousekey_move_delta), 0},
    {QSID_MOUSEKEY_MAX_SPEED, 2, offsetof(sval_qmk_settings_t, mousekey_max_speed), 0},
    {QSID_MOUSEKEY_TIME_TO_MAX, 2, offsetof(sval_qmk_settings_t, mousekey_time_to_max), 0},
    {QSID_MOUSEKEY_WHEEL_DELAY, 2, offsetof(sval_qmk_settings_t, mousekey_wheel_delay), 0},
    {QSID_MOUSEKEY_WHEEL_INTERVAL, 2, offsetof(sval_qmk_settings_t, mousekey_wheel_interval), 0},
    {QSID_MOUSEKEY_WHEEL_MAX_SPEED, 2, offsetof(sval_qmk_settings_t, mousekey_wheel_max_speed), 0},
    {QSID_MOUSEKEY_WHEEL_TIME_TO_MAX, 2, offsetof(sval_qmk_settings_t, mousekey_wheel_time_to_max), 0},
#endif
    {QSID_TAP_CODE_DELAY, 2, offsetof(sval_qmk_settings_t, tap_code_delay), 0},
    {QSID_TAP_HOLD_CAPS_DELAY, 2, offsetof(sval_qmk_settings_t, tap_hold_caps_delay), 0},
    {QSID_TAPPING_TOGGLE, 1, offsetof(sval_qmk_settings_t, tapping_toggle), 0},
    {QSID_MAGIC, 0, 0, 0}, // Special handler
    // Individual bit settings from tapping_v2
    {QSID_PERMISSIVE_HOLD, 1, offsetof(sval_qmk_settings_t, tapping_v2), TAPPING_PERMISSIVE_HOLD_BIT},
    {QSID_HOLD_ON_OTHER_KEY, 1, offsetof(sval_qmk_settings_t, tapping_v2), TAPPING_HOLD_ON_OTHER_KEY_BIT},
    {QSID_RETRO_TAPPING, 1, offsetof(sval_qmk_settings_t, tapping_v2), TAPPING_RETRO_TAPPING_BIT},
    {QSID_QUICK_TAP_TERM, 2, offsetof(sval_qmk_settings_t, quick_tap_term), 0},
    {QSID_CHORDAL_HOLD, 1, offsetof(sval_qmk_settings_t, tapping_v2), TAPPING_CHORDAL_HOLD_BIT},
    {QSID_FLOW_TAP_TERM, 2, offsetof(sval_qmk_settings_t, flow_tap_term), 0},
#ifdef LEADER_ENABLE
    {QSID_LEADER_TIMEOUT, 2, offsetof(sval_qmk_settings_t, leader_timeout), 0},
    {QSID_LEADER_PER_KEY_TIMING, 1, offsetof(sval_qmk_settings_t, leader_options), LEADER_PER_KEY_TIMING_BIT},
#endif
};

#define NUM_SETTINGS (sizeof(setting_descs) / sizeof(setting_descs[0]))

// Find setting descriptor by QSID
static const sval_setting_desc_t *find_setting(uint16_t qsid) {
    for (size_t i = 0; i < NUM_SETTINGS; i++) {
        if (setting_descs[i].qsid == qsid) {
            return &setting_descs[i];
        }
    }
    return NULL;
}

// Defaults for the settings where a stored 0 can only be a mistake: a combo
// term of 0 never fires a combo, a tapping term of 0 makes every tap-hold key a
// hold, an Auto Shift timeout of 0 shifts every key, a mouse key interval of 0
// moves the cursor every scan, and a leader timeout of 0 ends every sequence
// 1 ms after it starts. Each comes from the same constant the reset uses.
static uint16_t default_combo_term(void) {
#ifdef COMBO_TERM
    return COMBO_TERM;
#else
    return 50;
#endif
}

static uint16_t default_auto_shift_timeout(void) {
#ifdef AUTO_SHIFT_TIMEOUT
    return AUTO_SHIFT_TIMEOUT;
#else
    return 175;
#endif
}

static uint16_t default_mousekey_interval(void) {
#ifdef MOUSEKEY_INTERVAL
    return MOUSEKEY_INTERVAL;
#else
    return 20;
#endif
}

static uint16_t default_mousekey_wheel_interval(void) {
#ifdef MOUSEKEY_WHEEL_INTERVAL
    return MOUSEKEY_WHEEL_INTERVAL;
#else
    return 80;
#endif
}

// Lower a stored field to max, noting the change (the struct is packed, so no
// pointers to its fields).
#define HOLD_AT_MOST(field, max) \
    do {                         \
        if ((field) > (max)) {   \
            (field) = (max);     \
            changed = true;      \
        }                        \
    } while (0)

// LEADER_TIMEOUT is the live variable (post_config.h), so it can't be the
// default: SVAL_DEFAULT_LEADER_TIMEOUT is.
static uint8_t default_leader_options(void) {
#if SVAL_DEFAULT_LEADER_PER_KEY_TIMING
    return 1 << LEADER_PER_KEY_TIMING_BIT;
#else
    return 0;
#endif
}

// Replace a 0 in a setting where 0 is never valid with its default, and hold
// each value at the largest its runtime variable can use. Returns true when
// something changed. Leader: a 0 timeout came from the reset bug (no Keybard
// screen allows it), so the board never had working leader timing, and the
// per-key option it was stored with gets the default too.
static bool sval_qmk_settings_repair(void) {
    bool changed = false;
    if (settings.combo_term == 0) {
        settings.combo_term = default_combo_term();
        changed             = true;
    }
    if (settings.tapping_term == 0) {
        settings.tapping_term = TAPPING_TERM;
        changed               = true;
    }
    if (settings.auto_shift_timeout == 0) {
        settings.auto_shift_timeout = default_auto_shift_timeout();
        changed                     = true;
    }
    if (settings.mousekey_interval == 0) {
        settings.mousekey_interval = default_mousekey_interval();
        changed                    = true;
    }
    if (settings.leader_timeout == 0) {
        settings.leader_timeout = SVAL_DEFAULT_LEADER_TIMEOUT;
        settings.leader_options = (settings.leader_options & ~(1 << LEADER_PER_KEY_TIMING_BIT)) | default_leader_options();
        changed                 = true;
    }
    HOLD_AT_MOST(settings.mousekey_delay, SVAL_MK_DELAY_MAX_MS);
    HOLD_AT_MOST(settings.mousekey_interval, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.mousekey_max_speed, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.mousekey_time_to_max, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.mousekey_wheel_delay, SVAL_MK_DELAY_MAX_MS);
    HOLD_AT_MOST(settings.mousekey_wheel_interval, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.mousekey_wheel_max_speed, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.mousekey_wheel_time_to_max, SVAL_MK_8BIT_MAX);
    HOLD_AT_MOST(settings.tapping_toggle, SVAL_TAP_COUNT_MAX);
    HOLD_AT_MOST(settings.osk_tap_toggle, SVAL_TAP_COUNT_MAX);
#ifdef MOUSEKEY_ENABLE
    // The step size (QSID 11) is QMK's compile-time MOUSEKEY_MOVE_DELTA
    // (info.json mousekey.move_delta), which no setting changes. The stored
    // value reads back as the step the board uses; a write is accepted for
    // older hosts and has no effect.
    if (settings.mousekey_move_delta != MOUSEKEY_MOVE_DELTA) {
        settings.mousekey_move_delta = MOUSEKEY_MOVE_DELTA;
        changed                      = true;
    }
#endif
    return changed;
}

// One-time boot migrations, each marked done in settings.migrations so it runs
// once. Returns true when something changed.
//
// Wheel: firmware before this one stored the wheel delay and interval (QSIDs 14
// and 15) but never applied them, so every board scrolled at the build's
// defaults whatever was stored. A stored wheel interval of 0 would now scroll
// every scan, a speed the board never had, so on the first boot of this
// firmware it becomes the default. After that a 0 is a value someone chose
// with the setting working, and it is kept.
static bool sval_qmk_settings_migrate(void) {
    if (settings.migrations & (1 << SVAL_QS_MIGRATED_WHEEL_BIT)) return false;
    if (settings.mousekey_wheel_interval == 0) settings.mousekey_wheel_interval = default_mousekey_wheel_interval();
    settings.migrations |= 1 << SVAL_QS_MIGRATED_WHEEL_BIT;
    return true;
}

// Initialize QMK settings
void sval_qmk_settings_init(void) {
    sval_qmk_settings_load();
    bool migrated = sval_qmk_settings_migrate();
    if (sval_qmk_settings_repair() || migrated) {
        sval_qmk_settings_save();
    }
    sval_qmk_settings_apply();
}

// Load settings from EEPROM
static void sval_qmk_settings_load(void) {
    eeconfig_read_kb_datablock(&settings, SVAL_QMK_SETTINGS_OFFSET, sizeof(settings));
}

// Save settings to EEPROM
static void sval_qmk_settings_save(void) {
    eeconfig_update_kb_datablock(&settings, SVAL_QMK_SETTINGS_OFFSET, sizeof(settings));
}

// Apply settings to QMK subsystems
static void sval_qmk_settings_apply(void) {
#ifdef AUTO_SHIFT_ENABLE
    set_autoshift_timeout(settings.auto_shift_timeout);
    if (settings.auto_shift & (1 << AUTO_SHIFT_ENABLE_BIT)) {
        autoshift_enable();
    } else if (get_autoshift_state()) {
        autoshift_disable();
    }
#endif

#ifdef MOUSEKEY_ENABLE
    // Held at the variables' limits here too, so no stored value can wrap. The
    // delays are stored in ms and applied in 10 ms units, as in Vial.
    mk_delay       = SVAL_AT_MOST(settings.mousekey_delay, SVAL_MK_DELAY_MAX_MS) / 10;
    mk_interval    = SVAL_AT_MOST(settings.mousekey_interval, SVAL_MK_8BIT_MAX);
    mk_max_speed   = SVAL_AT_MOST(settings.mousekey_max_speed, SVAL_MK_8BIT_MAX);
    mk_time_to_max = SVAL_AT_MOST(settings.mousekey_time_to_max, SVAL_MK_8BIT_MAX);
    mk_wheel_delay = SVAL_AT_MOST(settings.mousekey_wheel_delay, SVAL_MK_DELAY_MAX_MS) / 10;
#    ifndef MK_KINETIC_SPEED
    mk_wheel_interval = SVAL_AT_MOST(settings.mousekey_wheel_interval, SVAL_MK_8BIT_MAX);
#    endif
    mk_wheel_max_speed   = SVAL_AT_MOST(settings.mousekey_wheel_max_speed, SVAL_MK_8BIT_MAX);
    mk_wheel_time_to_max = SVAL_AT_MOST(settings.mousekey_wheel_time_to_max, SVAL_MK_8BIT_MAX);
#endif

#ifdef LEADER_ENABLE
    extern uint16_t sval_leader_timeout;
    sval_leader_timeout = settings.leader_timeout;
#endif
}

// Query supported QSIDs greater than qsid_gt
// Response format: list of uint16_t QSIDs, terminated by 0xFFFF
void sval_qmk_settings_query(uint16_t qsid_gt, uint8_t *buffer, uint8_t length) {
    // Guard against invalid buffer size
    if (length < 2) {
        return;
    }

    memset(buffer, 0xFF, length);

    size_t buf_offset = 0;
    for (size_t i = 0; i < NUM_SETTINGS; i++) {
        if (setting_descs[i].qsid > qsid_gt) {
            // Ensure room for QSID (2 bytes) and terminator (2 bytes)
            if (buf_offset + 4 > length) break;
            buffer[buf_offset++] = setting_descs[i].qsid & 0xFF;
            buffer[buf_offset++] = (setting_descs[i].qsid >> 8) & 0xFF;
        }
    }
}

// Get a setting value
// Returns 0 on success, non-zero on error
int sval_qmk_settings_get(uint16_t qsid, uint8_t *buffer, uint8_t length) {
    // Special case: Magic settings (keymap_config)
    if (qsid == QSID_MAGIC) {
        if (length < 4) return -1;
        uint32_t flags = (keymap_config.swap_control_capslock << 0) | (keymap_config.capslock_to_control << 1) | (keymap_config.swap_lalt_lgui << 2) | (keymap_config.swap_ralt_rgui << 3) | (keymap_config.no_gui << 4) | (keymap_config.swap_grave_esc << 5) | (keymap_config.swap_backslash_backspace << 6) | (keymap_config.nkro << 7) | (keymap_config.swap_lctl_lgui << 8) | (keymap_config.swap_rctl_rgui << 9);
        memcpy(buffer, &flags, 4);
        return 0;
    }

    const sval_setting_desc_t *desc = find_setting(qsid);
    if (!desc) return -1;

    // Bit field settings (QSID 22-26 and 29)
    if ((qsid >= QSID_PERMISSIVE_HOLD && qsid <= QSID_CHORDAL_HOLD && qsid != QSID_QUICK_TAP_TERM) || qsid == QSID_LEADER_PER_KEY_TIMING) {
        if (length < 1) return -1;
        uint8_t *field = ((uint8_t *)&settings) + desc->offset;
        buffer[0]      = (*field >> desc->bit) & 1;
        return 0;
    }

    // Regular settings
    if (desc->size > length) return -1;
    memcpy(buffer, ((uint8_t *)&settings) + desc->offset, desc->size);
    return 0;
}

// Set a setting value
// Returns 0 on success, non-zero on error
int sval_qmk_settings_set(uint16_t qsid, const uint8_t *data, uint8_t length) {
    // Special case: Magic settings (keymap_config)
    if (qsid == QSID_MAGIC) {
        if (length < 4) return -1;
        uint32_t flags;
        memcpy(&flags, data, 4);

        clear_keyboard(); // Prevent stuck keys when changing NKRO

        keymap_config.swap_control_capslock    = (flags >> 0) & 1;
        keymap_config.capslock_to_control      = (flags >> 1) & 1;
        keymap_config.swap_lalt_lgui           = (flags >> 2) & 1;
        keymap_config.swap_ralt_rgui           = (flags >> 3) & 1;
        keymap_config.no_gui                   = (flags >> 4) & 1;
        keymap_config.swap_grave_esc           = (flags >> 5) & 1;
        keymap_config.swap_backslash_backspace = (flags >> 6) & 1;
        keymap_config.nkro                     = (flags >> 7) & 1;
        keymap_config.swap_lctl_lgui           = (flags >> 8) & 1;
        keymap_config.swap_rctl_rgui           = (flags >> 9) & 1;

        eeconfig_update_keymap(&keymap_config);
        return 0;
    }

    const sval_setting_desc_t *desc = find_setting(qsid);
    if (!desc) return -1;

    // Bit field settings
    if ((qsid >= QSID_PERMISSIVE_HOLD && qsid <= QSID_CHORDAL_HOLD && qsid != QSID_QUICK_TAP_TERM) || qsid == QSID_LEADER_PER_KEY_TIMING) {
        if (length < 1) return -1;
        uint8_t *field = ((uint8_t *)&settings) + desc->offset;
        if (data[0]) {
            *field |= (1 << desc->bit);
        } else {
            *field &= ~(1 << desc->bit);
        }
        sval_qmk_settings_save();
        return 0;
    }

    // Regular settings
    if (desc->size > length) return -1;
    memcpy(((uint8_t *)&settings) + desc->offset, data, desc->size);
    // A 0 from an older Keybard (its default layouts carried a leader timeout
    // of 0) is stored as the default, the same as on boot.
    sval_qmk_settings_repair();
    sval_qmk_settings_save();
    sval_qmk_settings_apply();
    return 0;
}

// Reset all settings to defaults
void sval_qmk_settings_reset(void) {
    memset(&settings, 0, sizeof(settings));
    settings.migrations = 1 << SVAL_QS_MIGRATED_WHEEL_BIT; // defaults need no migration

    // Set defaults
    settings.tapping_term = TAPPING_TERM;
    settings.combo_term   = default_combo_term();
#ifdef ONESHOT_TIMEOUT
    settings.osk_timeout = ONESHOT_TIMEOUT;
#else
    settings.osk_timeout = 5000;
#endif
#ifdef ONESHOT_TAP_TOGGLE
    settings.osk_tap_toggle = ONESHOT_TAP_TOGGLE;
#else
    settings.osk_tap_toggle = 5;
#endif
    settings.auto_shift_timeout = default_auto_shift_timeout();
#ifdef TAP_CODE_DELAY
    settings.tap_code_delay = TAP_CODE_DELAY;
#else
    settings.tap_code_delay = 10;
#endif
#ifdef TAP_HOLD_CAPS_DELAY
    settings.tap_hold_caps_delay = TAP_HOLD_CAPS_DELAY;
#else
    settings.tap_hold_caps_delay = 80;
#endif
#ifdef TAPPING_TOGGLE
    settings.tapping_toggle = TAPPING_TOGGLE;
#else
    settings.tapping_toggle = 5;
#endif
#ifdef QUICK_TAP_TERM
    settings.quick_tap_term = QUICK_TAP_TERM;
#else
    settings.quick_tap_term = TAPPING_TERM;
#endif

    // Tapping behavior defaults - keyboards can override via defines
    settings.tapping_v2 = 0;
#if defined(SVAL_DEFAULT_PERMISSIVE_HOLD) && SVAL_DEFAULT_PERMISSIVE_HOLD
    settings.tapping_v2 |= (1 << TAPPING_PERMISSIVE_HOLD_BIT);
#endif
#if defined(SVAL_DEFAULT_CHORDAL_HOLD) && SVAL_DEFAULT_CHORDAL_HOLD
    settings.tapping_v2 |= (1 << TAPPING_CHORDAL_HOLD_BIT);
#endif
#if defined(SVAL_DEFAULT_HOLD_ON_OTHER_KEY) && SVAL_DEFAULT_HOLD_ON_OTHER_KEY
    settings.tapping_v2 |= (1 << TAPPING_HOLD_ON_OTHER_KEY_BIT);
#endif
#if defined(SVAL_DEFAULT_RETRO_TAPPING) && SVAL_DEFAULT_RETRO_TAPPING
    settings.tapping_v2 |= (1 << TAPPING_RETRO_TAPPING_BIT);
#endif
#ifdef SVAL_DEFAULT_FLOW_TAP_TERM
    settings.flow_tap_term = SVAL_DEFAULT_FLOW_TAP_TERM;
#endif

#ifdef MOUSEKEY_ENABLE
    settings.mousekey_delay             = MOUSEKEY_DELAY;
    settings.mousekey_interval          = MOUSEKEY_INTERVAL;
    settings.mousekey_move_delta        = MOUSEKEY_MOVE_DELTA;
    settings.mousekey_max_speed         = MOUSEKEY_MAX_SPEED;
    settings.mousekey_time_to_max       = MOUSEKEY_TIME_TO_MAX;
    settings.mousekey_wheel_delay       = MOUSEKEY_WHEEL_DELAY;
    settings.mousekey_wheel_interval    = MOUSEKEY_WHEEL_INTERVAL;
    settings.mousekey_wheel_max_speed   = MOUSEKEY_WHEEL_MAX_SPEED;
    settings.mousekey_wheel_time_to_max = MOUSEKEY_WHEEL_TIME_TO_MAX;
#endif

    // Not LEADER_TIMEOUT: that is the live variable (post_config.h), which
    // sval_reset() has just loaded from zeroed memory, so the reset stored 0.
    settings.leader_timeout = SVAL_DEFAULT_LEADER_TIMEOUT;
    settings.leader_options = default_leader_options();

    sval_qmk_settings_save();
    sval_qmk_settings_apply();

    // Reset magic settings to defaults
    clear_keyboard();
    keymap_config.raw            = 0;
    keymap_config.oneshot_enable = 1;
#ifdef NKRO_ENABLE
#    if defined(SVAL_DEFAULT_NKRO)
    keymap_config.nkro = SVAL_DEFAULT_NKRO;
#    else
    keymap_config.nkro = 1; // Default NKRO on if supported
#    endif
#endif
    eeconfig_update_keymap(&keymap_config);
}

// User hooks (*_sval suffix): override these for custom per-key logic
// For booleans: return -1 to use Sval's setting, 0 for false, 1 for true
// For uint16_t: return 0 to use Sval's setting, or a positive value to override
// User hooks are checked FIRST - they always beat Sval's settings
// Note: get_combo_term_sval is in sval_combo.c

__attribute__((weak)) uint16_t get_tapping_term_sval(uint16_t keycode, keyrecord_t *record) {
    return 0; // Default: use Sval's setting
}

__attribute__((weak)) int8_t get_permissive_hold_sval(uint16_t keycode, keyrecord_t *record) {
    return -1; // Default: use Sval's setting
}

__attribute__((weak)) int8_t get_hold_on_other_key_press_sval(uint16_t keycode, keyrecord_t *record) {
    return -1; // Default: use Sval's setting
}

__attribute__((weak)) int8_t get_retro_tapping_sval(uint16_t keycode, keyrecord_t *record) {
    return -1; // Default: use Sval's setting
}

__attribute__((weak)) uint16_t get_quick_tap_term_sval(uint16_t keycode, keyrecord_t *record) {
    return 0; // Default: use Sval's setting
}

__attribute__((weak)) int8_t get_chordal_hold_sval(uint16_t tap_hold_keycode, keyrecord_t *tap_hold_record, uint16_t other_keycode, keyrecord_t *other_record) {
    return -1; // Default: use Sval's setting
}

__attribute__((weak)) uint16_t get_flow_tap_term_sval(uint16_t keycode, keyrecord_t *record, uint16_t prev_keycode) {
    return 0; // Default: use Sval's setting
}

// Sval owns these functions - user hook is checked FIRST
uint16_t get_tapping_term(uint16_t keycode, keyrecord_t *record) {
    uint16_t user_term = get_tapping_term_sval(keycode, record);
    if (user_term > 0) {
        return user_term;
    }

#ifdef TAP_DANCE_ENABLE
    // Per-tap-dance custom timing from Sval
    if (keycode >= QK_TAP_DANCE && keycode <= QK_TAP_DANCE_MAX) {
        sval_tap_dance_entry_t td;
        if (sval_get_tap_dance(keycode & 0xFF, &td) == 0 && TD_ENABLED(td)) {
            uint16_t term = td.custom_tapping_term & 0x7FFF; // Mask off enabled bit
            if (term > 0) {
                return term;
            }
        }
    }
#endif

    return settings.tapping_term;
}

bool get_permissive_hold(uint16_t keycode, keyrecord_t *record) {
    int8_t user_val = get_permissive_hold_sval(keycode, record);
    if (user_val >= 0) return user_val;
    return settings.tapping_v2 & (1 << TAPPING_PERMISSIVE_HOLD_BIT);
}

bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record) {
    int8_t user_val = get_hold_on_other_key_press_sval(keycode, record);
    if (user_val >= 0) return user_val;
    return settings.tapping_v2 & (1 << TAPPING_HOLD_ON_OTHER_KEY_BIT);
}

bool get_retro_tapping(uint16_t keycode, keyrecord_t *record) {
    int8_t user_val = get_retro_tapping_sval(keycode, record);
    if (user_val >= 0) return user_val;
    return settings.tapping_v2 & (1 << TAPPING_RETRO_TAPPING_BIT);
}

uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record) {
    uint16_t user_val = get_quick_tap_term_sval(keycode, record);
    if (user_val > 0) return user_val;
    return settings.quick_tap_term;
}

// Chordal Hold: when the setting is off, allow every chord to settle as a
// hold. That is QMK's "opposite hands" path, which adds no behaviour beyond
// the other tap-hold options, so it matches CHORDAL_HOLD not being built.
// When on, apply the opposite-hands rule using the keyboard's
// chordal_hold_layout (QMK generates one from the layout if the keyboard
// doesn't define its own).
bool get_chordal_hold(uint16_t tap_hold_keycode, keyrecord_t *tap_hold_record, uint16_t other_keycode, keyrecord_t *other_record) {
    int8_t user_val = get_chordal_hold_sval(tap_hold_keycode, tap_hold_record, other_keycode, other_record);
    if (user_val >= 0) return user_val;
    if (!(settings.tapping_v2 & (1 << TAPPING_CHORDAL_HOLD_BIT))) return true;
    return get_chordal_hold_default(tap_hold_record, other_record);
}

// Flow Tap: QMK treats a term of 0 as "no filtering", so a stored value of 0
// (the default) leaves Flow Tap off.
uint16_t get_flow_tap_term(uint16_t keycode, keyrecord_t *record, uint16_t prev_keycode) {
    uint16_t user_val = get_flow_tap_term_sval(keycode, record, prev_keycode);
    if (user_val > 0) return user_val;
    if (settings.flow_tap_term > 0 && is_flow_tap_key(keycode) && is_flow_tap_key(prev_keycode)) {
        return settings.flow_tap_term;
    }
    return 0;
}

// Getters for other modules to use
uint16_t sval_get_tapping_term(void) {
    return settings.tapping_term;
}

uint16_t sval_get_combo_term(void) {
    return settings.combo_term;
}

uint16_t sval_get_leader_timeout(void) {
    return settings.leader_timeout;
}

bool sval_get_leader_per_key_timing(void) {
    return settings.leader_options & (1 << LEADER_PER_KEY_TIMING_BIT);
}

// Runtime values for the QMK constants of the same names. quantum/ reads these
// through inline helpers in action.h, action_tapping.h and process_grave_esc.c
// when SVAL_ENABLE is defined.
uint16_t sval_tap_code_delay(void) {
    return settings.tap_code_delay;
}

uint16_t sval_tap_hold_caps_delay(void) {
    return settings.tap_hold_caps_delay;
}

// A toggle count of 0 would leave a held TT() layer on after release, so 0 is
// read as 1: one tap toggles, while a held key still releases its layer. A
// count above 15 is read as 15, the most taps QMK counts.
uint8_t sval_tapping_toggle(void) {
    return settings.tapping_toggle ? SVAL_AT_MOST(settings.tapping_toggle, SVAL_TAP_COUNT_MAX) : 1;
}

// One-shot keys: QMK settings 6 (timeout, 0 = none) and 5 (taps to lock; 0 or 1
// disables locking). Sval commands 0x09/0x0A read and write the same values.
uint16_t get_oneshot_timeout(void) {
    return settings.osk_timeout;
}

uint8_t sval_oneshot_tap_toggle(void) {
    return SVAL_AT_MOST(settings.osk_tap_toggle, SVAL_TAP_COUNT_MAX);
}

void sval_qmk_settings_set_one_shot(uint16_t timeout, uint8_t tap_toggle) {
    settings.osk_timeout    = timeout;
    settings.osk_tap_toggle = SVAL_AT_MOST(tap_toggle, SVAL_TAP_COUNT_MAX);
    sval_qmk_settings_save();
}

// Bits as Keybard writes them: 0 Alt, 1 Control, 2 GUI, 3 Shift.
uint8_t sval_grave_esc_override(void) {
    return settings.grave_esc_override;
}

// Bits as Keybard writes them: 0 enable, 1 modifiers, 2 no-special,
// 3 no-numeric, 4 no-alpha, 5 repeat, 6 no-auto-repeat.
uint8_t sval_auto_shift(void) {
    return settings.auto_shift;
}
