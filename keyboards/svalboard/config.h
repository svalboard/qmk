/*
Copyright 2023 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once
// This is used by our user's keymaps for conditionals.
#define SVALBOARD

// Keyboard UID for profile matching (same as original Vial UID for .vil compatibility)
#define SVAL_KEYBOARD_UID {0x1B, 0x18, 0x7D, 0xF2, 0x21, 0xF6, 0x29, 0x48}

#define POINTING_DEVICE_HIRES_SCROLL_ENABLE 1
#define POINTING_DEVICE_HIRES_SCROLL_MULTIPLIER 120
#define WHEEL_EXTENDED_REPORT 1

#define AXIS_TYPE int16_t

//#define FORTY_FOUR_MM_TB
/* key matrix size */
// Rows are doubled-up
#define MATRIX_ROWS  10
#define MATRIX_COLS  6
#define PFET_ROWS
#define EE_HANDS
//#define DEBUG_MATRIX_SCAN_RATE
// Svalboard stores its config in VIA custom config area
// saved_values_t (~54 bytes) + 3 bytes magic = ~60 bytes, round to 100
#define VIA_EEPROM_CUSTOM_CONFIG_SIZE 100

// The raw HID interface (VIA, the Sval protocol and the client wrapper) uses
// 64-byte reports, the full-speed interrupt maximum, both ways. Hosts size their
// reports from the HID descriptor; the updater derives its packet layout from
// this (updater/updater.c).
#define RAW_EPSIZE 64


#define FLASH_LEN (16 * 1024 * 1024)
// Settings: QMK wear leveling, 128 KB logical on two mirrored 512 KB copies,
// directly after the firmware area (0x160000..0x25FFFF).
#define WEAR_LEVELING_BACKING_SIZE (512 * 1024)
#define WEAR_LEVELING_LOGICAL_SIZE (128 * 1024)
#define WEAR_LEVELING_RP2040_FLASH_BASE 0x160000
// Settings any earlier firmware left in this region don't match this magic, so
// QMK resets them all to defaults on the first boot. Decrement it (never reuse
// a value, and stay below QMK's own) whenever stored settings must all reset.
#ifdef SVAL_TEST_WIPE
// Test builds (-e SVAL_TEST_WIPE=yes): a magic no release uses, so the first boot after
// flashing over any other firmware resets every setting, keymap included, to the
// build's defaults. A blank build then sends nothing, whatever the MCU stored before.
#    define EECONFIG_MAGIC_NUMBER (uint16_t)0xB1A0
#else
#    define EECONFIG_MAGIC_NUMBER (uint16_t)0xFEE2
#endif
#define WEAR_LEVELING_COPIES 2

// In-firmware updater (updater/, built with SVAL_UPDATER=yes). Offsets from the
// start of the flash die. The staging slot sits in the free space between the
// settings (ending 0x260000) and the identity sectors (0xFFE000), and an image
// is at most the firmware area; updater/update_flash.c asserts both.
#define SVAL_UPDATE_BASE 0x800000
#define SVAL_UPDATE_SIZE 0x160000
#define SVAL_UPDATE_MAX_IMAGE 0x160000
// Downgrade floors (D19): an image whose manifest is below either is refused.
// Raise SVAL_UPDATE_SECURITY_EPOCH in a release that fixes a security problem,
// and SVAL_UPDATE_STORAGE_FORMAT when stored settings change incompatibly.
#ifndef SVAL_UPDATE_SECURITY_EPOCH
#    define SVAL_UPDATE_SECURITY_EPOCH 0
#endif
#define SVAL_UPDATE_STORAGE_FORMAT 2
// Confirmation chord (D4): Index South + Middle South, held, on the half with
// USB. Local matrix rows (each half scans five): right [6,0]+[7,0], left
// [1,0]+[2,0], which is local rows 1 and 2, column 0 on either side.
#define SVAL_UPDATE_CHORD_ROW_A 1
#define SVAL_UPDATE_CHORD_ROW_B 2
#define SVAL_UPDATE_CHORD_COL 0
#define SVAL_UPDATE_CHORD_HOLD_MS 1000
// While the chord is awaited, rows are scanned with at least these waits, Scan
// Lab's safe reference timing (scanlab.h; kb/matrix.c asserts it): the saved
// timings and Scan Lab sweeps are set by the host, and a short pre-wait can
// misread keys.
#define SVAL_UPDATE_CHORD_PREWAIT_US 500
#define SVAL_UPDATE_CHORD_POSTWAIT_US 500
#define SVAL_UPDATE_CONFIRM_WINDOW_MS 30000 // from ARM; NOT_CONFIRMED after it
#define SVAL_UPDATE_SESSION_TIMEOUT_MS 30000 // no op from the session for this long: TIMEOUT
#define SVAL_UPDATE_LED_VAL 96               // LED brightness while the updater shows its states (D24)
#ifndef SVAL_UPDATE_AFTER_WAIT_MS
#    define SVAL_UPDATE_AFTER_WAIT_MS 60000 // after an update: idle this long with the halves still differing, then red (D24)
#endif
#ifndef SVAL_UPDATE_RED_MS
#    define SVAL_UPDATE_RED_MS 30000 // every red updater light stops after this long (D24)
#endif
// M2, the relay to the other half (updater/update_split.c). The half without
// USB drops a session after this long with no KEYBOARD_UPDATE traffic.
#define SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS 5000
// It erases a sector (interrupts off for about 50 ms) only after a request
// from the master since the last one, and this long after it: the master
// gets an answer between every two sectors.
#define SVAL_UPDATE_SPLIT_ERASE_GAP_MS 5
// The half with USB: relay work per main-loop pass while the link runs (its
// own keys keep scanning between passes), how long it retries a request with
// no valid answer before the relay fails, how often it polls the other half
// while that half erases or verifies, and how often it pings a verified other
// half (so its timeout does not fire while the host decides to commit).
#define SVAL_UPDATE_RELAY_PASS_MS 4
#define SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS 2000
#define SVAL_UPDATE_RELAY_POLL_MS 20
#define SVAL_UPDATE_RELAY_KEEPALIVE_MS 1000
// After the other half accepts COMMIT, the link stays paused for its commit
// (M1 #13: about 1-2 s from COMMIT to the reset) plus 2.5 s for its reboot
// (R18: the 500 ms double-tap window and the 2 s USB detect).
// The boot relay of a pair UF2 (updater/update_pair.c, docs/pair-uf2.md): how
// long the half with USB waits for the other half's presence after boot, how
// many relays it tries in one boot and the pause between them, and how long
// it shows the red light after a record that was refused, damaged or tried
// too often (a record kept for the next boot shows no light).
#ifndef SVAL_UPDATE_PAIR_PEER_WAIT_MS
#    define SVAL_UPDATE_PAIR_PEER_WAIT_MS 15000
#endif
#ifndef SVAL_UPDATE_PAIR_RELAY_TRIES
#    define SVAL_UPDATE_PAIR_RELAY_TRIES 3
#endif
#ifndef SVAL_UPDATE_PAIR_RETRY_MS
#    define SVAL_UPDATE_PAIR_RETRY_MS 2000
#endif
#ifndef SVAL_UPDATE_PAIR_ERROR_MS
#    define SVAL_UPDATE_PAIR_ERROR_MS 30000
#endif
#define SVAL_UPDATE_SPLIT_COMMIT_MS 2000
#define SVAL_UPDATE_SPLIT_REBOOT_MS 2500
// A probe after the hold that finds the other half still COMMITTING probes
// again this much later (still paused).
#define SVAL_UPDATE_RELAY_PROBE_MS 500
#if defined(SVAL_UPDATER) && defined(SVAL_KEYTEST)
#    error "SVAL_UPDATER and SVAL_KEYTEST cannot be combined: keytest injects key events (R11)"
#endif
// Updater builds drive rgblight through a custom driver (updater/update_led.c),
// so drivers/led/ws2812.h no longer derives the WS2812 LED count from rgblight.
#if defined(SVAL_UPDATER) && !defined(WS2812_LED_COUNT)
#    define WS2812_LED_COUNT RGBLIGHT_LED_COUNT
#endif

// Identity (identity.c): the USB serial is "sval:" + the board's stored 8-byte
// serial as 16 hex digits, and the product string is the user's name when set.
// Every keymap uses it, so drop the module's fixed SERIAL_NUMBER literal.
#undef SERIAL_NUMBER
#define SERIAL_NUMBER_LENGTH (5 + 16) // "sval:" + 16 hex digits
#define USB_PRODUCT_STRING_RUNTIME
// wiring of each half
//Layout for svalboard v0 (different from lalboard_v2)
//1 2 3 4 5 6
//S E D N W None
//Both Thumbs (these are same as lalboard_v2)
//OL OU D IL MODE DOUBLE
//Knuckle Nail Down Pad Up Double
//#define THUMB_DOWN_ACTIVE_DARK
#define MATRIX_COL_PUSHED_STATES { 0, 0, 1, 0, 0, 0 }
#ifdef THUMB_DOWN_ACTIVE_DARK
    #define MATRIX_COL_PUSHED_STATES_THUMBS { 0, 0, 1, 0, 0, 0 }
#else
    #define MATRIX_COL_PUSHED_STATES_THUMBS { 0, 0, 0, 0, 0, 0 }
#endif
#define DOUBLEDOWN_COL 5 // need a pullup on COL6

// Thumb handedness strap: the WS2812 data pin, which reaches the thumb's
// LED_DIN. A left thumb with R7 fitted pulls it to ground through 10k.
#ifndef SVAL_THUMB_HAND_PIN
    #define SVAL_THUMB_HAND_PIN GP19 // thumb LED_DIN; R7 strap on a left thumb
#endif
// The thumb row's FET gate (matrix row 0), on during the strap read: low is on.
#ifndef SVAL_THUMB_ROW_PIN
    #define SVAL_THUMB_ROW_PIN GP8
#endif

// Hardware revision strap. The pin is read with the internal pull-up enabled:
// open = revision A, bridged to ground = revision B ("flipfet"). Override the
// pin per build if the MCU board routes a different spare GPIO, or force a
// revision with -DSVAL_HW_REV_FORCE=1 on prototypes without the strap.
#ifndef SVAL_HW_REV_PIN
    #define SVAL_HW_REV_PIN GP22
#endif
// Revision B boots on explicit scan timing. Measured with the Scan Lab on a
// revision B board (see docs/scan-lab.md): centre keys settle in 21-27 us,
// side keys in 11-15 us, every line recovers within 1 us. 45 / 5 us leaves
// 1.7x margin on settle and 5x on recovery.
#ifndef SVAL_FLIPFET_DEFAULT_PREWAIT_US
    #define SVAL_FLIPFET_DEFAULT_PREWAIT_US 45
#endif
#ifndef SVAL_FLIPFET_DEFAULT_POSTWAIT_US
    #define SVAL_FLIPFET_DEFAULT_POSTWAIT_US 5
#endif
// Frame pacing: sensor LED duty = rows x (pre-wait + read) / period. 1 ms
// matches the USB poll interval; the idle period defaults to the same value
// so there is no latency trade-off unless a user chooses one.
#ifndef SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US
    #define SVAL_FLIPFET_DEFAULT_SCAN_PERIOD_US 1000
#endif
#ifndef SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS
    #define SVAL_FLIPFET_DEFAULT_IDLE_PERIOD_MS 1     // light idle: same as active by default
#endif
#ifndef SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS
    #define SVAL_FLIPFET_DEFAULT_IDLE_AFTER_MS 10000   // light idle also dims the RGB, so not during typing pauses
#endif
#ifndef SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S
    #define SVAL_FLIPFET_DEFAULT_DEEP_AFTER_S 0       // deep idle off by default
#endif
#ifndef SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS
    #define SVAL_FLIPFET_DEFAULT_DEEP_PERIOD_MS 0
#endif

// Idle power features (saved_values.idle_flags). Each is a runtime toggle (VIA ids 25-27) so
// their effect can be measured one at a time from the Scan Lab panel.
#define SVAL_IDLE_POINTER_REST 0x01   // let the trackball sensor use its own rest modes
#define SVAL_IDLE_RGB_DIM      0x02   // dim the RGB in light idle, off in deep idle
#define SVAL_IDLE_CPU_SLEEP    0x04   // sleep the core between paced frames instead of spinning
#define SVAL_IDLE_LOW_CLOCK    0x08   // deep idle: clk_sys from the USB PLL at 48 MHz, system PLL off
#define SVAL_IDLE_LONG_NAP     0x10   // deep idle: naps of SVAL_DEEP_NAP_*_US instead of 1 ms
#ifndef SVAL_IDLE_FLAGS_DEFAULT
    #define SVAL_IDLE_FLAGS_DEFAULT (SVAL_IDLE_POINTER_REST | SVAL_IDLE_RGB_DIM | SVAL_IDLE_CPU_SLEEP | SVAL_IDLE_LOW_CLOCK | SVAL_IDLE_LONG_NAP)
#endif
#ifndef SVAL_DEEP_NAP_MASTER_US
    #define SVAL_DEEP_NAP_MASTER_US 20000 // the sensor only reports every 100-500 ms in rest; USB replies wait at most this
#endif
#ifndef SVAL_DEEP_NAP_SLAVE_US
    #define SVAL_DEEP_NAP_SLAVE_US 4000   // must stay well inside the master's 20 ms split-transaction timeout
#endif
#ifndef SVAL_HOST_ACTIVE_MS
    #define SVAL_HOST_ACTIVE_MS 1500  // a host app is mid-conversation: full clock and 1 ms naps, so a
                                      // request/response round trip is not paced by the deep-idle nap
#endif
#ifndef SVAL_IDLE_RGB_LIGHT_DIV
    #define SVAL_IDLE_RGB_LIGHT_DIV 4   // light idle brightness = awake brightness / this
#endif
#ifndef SVAL_SLEEP_MAX_US
    #define SVAL_SLEEP_MAX_US 1000      // longest single nap between frames; keeps USB and pointer polling responsive
#endif
#ifndef SVAL_SLEEP_MIN_US
    #define SVAL_SLEEP_MIN_US 60        // below this the timer round trip costs more than it saves
#endif
// Host-initiated reboot into the bootloader (Scan Lab REBOOT_ARM / REBOOT_GO).
// Off unless a keymap or build enables it; the scanlab keymap does.
#ifndef SVAL_HOST_BOOTLOADER
    #define SVAL_HOST_BOOTLOADER 0
#endif
// Thumb-row pushed states on revision B. Identical to revision A until the
// settle probe says otherwise; the probe reports each key's polarity.
#ifndef MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET
    #define MATRIX_COL_PUSHED_STATES_THUMBS_FLIPFET MATRIX_COL_PUSHED_STATES_THUMBS
#endif

#define SERIAL_DEBUG
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET
#define RP2040_BOOTLOADER_DOUBLE_TAP_RESET_TIMEOUT 500 // Timeout window in ms in which the double tap can occur.

// Macro count (Sval entry counts are now in each keymap's sval.json)
// 256 macros: QMK's macro keycodes (0x7700-0x777F) reach macro 127; the Sval
// module maps 0x7680-0x76FF to macros 128-255 (SVAL_MACRO_HIGH_BASE).
#define DYNAMIC_KEYMAP_MACRO_COUNT 256

// Sval defaults for Svalboard - sane mod-tap experience
#define SVAL_DEFAULT_NKRO 1
#define SVAL_DEFAULT_PERMISSIVE_HOLD 1
#define SVAL_DEFAULT_CHORDAL_HOLD 1

#define USB_MAX_POWER_CONSUMPTION 500
#define USB_SUSPEND_WAKEUP_DELAY 500
#define SERIAL_USART_SPEED 1000000

#define MOUSE_EXTENDED_REPORT
#define SPLIT_POINTING_ENABLE
#define POINTING_DEVICE_COMBINED
#define POINTING_DEVICE_AUTO_MOUSE_MH_ENABLE
#define POINTING_DEVICE_TASK_THROTTLE_MS 1


// Avoid slave-slave deadlock due to missing USB_VBUS_PIN.
//
// End result of enabling this: when you plug the keyboard to a finnicky USB
// hub, KVM, or a machine that boots slowly (ECC RAM), the keyboard no longer
// needs to be reset to come to life.
#define SPLIT_WATCHDOG_ENABLE

// WS2812-fu here:
//  pretty lights
//  https://docs.qmk.fm/#/feature_rgblight?id=configuration
#define WS2812_DI_PIN GP19
#define RGBLED_SPLIT { 1, 1 }
#define RGBLIGHT_LAYERS_RETAIN_VAL
#define RGBLIGHT_LAYERS DYNAMIC_KEYMAP_LAYER_COUNT
#define RGBLIGHT_DEFAULT_SAT 0 // white?
#define RGBLIGHT_LIMIT_VAL 255
#define RGBLIGHT_DEFAULT_VAL 128
#define RGBLIGHT_SLEEP // don't annoy when host asleep
#define RGBLIGHT_MAX_LAYERS 16 //DYNAMIC_KEYMAP_LAYER_COUNT
#define RGBLIGHT_VAL_STEP 10
#define RGBLIGHT_LED_COUNT 2

#ifdef SVAL_UPDATER
// KEYBOARD_UPDATE: the updater's split relay (updater/update_split_wire.h).
// Appended, so the IDs before it keep their numbers; but the transaction
// count changes, and both halves fold it into every split handshake, so an
// SVAL_UPDATER half and a half without it cannot talk at all (P1:
// keyboards/svalboard/tools/check_split_tables.py).
#    define SPLIT_TRANSACTION_IDS_KB KEYBOARD_SYNC_A, KEYBOARD_SYNC_B, KEYBOARD_UPDATE
#else
#    define SPLIT_TRANSACTION_IDS_KB KEYBOARD_SYNC_A, KEYBOARD_SYNC_B
#endif

#define PERMISSIVE_HOLD

#define MOUSEKEY_WHEEL_DELTA 120

#define OS_DETECTION_KEYBOARD_RESET

#define PMW33XX_LIFTOFF_DISTANCE 0x00
