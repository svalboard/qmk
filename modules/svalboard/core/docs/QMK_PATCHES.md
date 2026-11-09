# Svalboard QMK core patches

This is the full list of files outside `keyboards/` and `modules/` that differ from upstream QMK, why each one differs, and what to check when upstream changes it. Most board work lives in `keyboards/svalboard/` and `modules/svalboard/`; this list covers only the core.

To regenerate the list against the current merge base:

```bash
base=$(git merge-base HEAD qmk/master)
git diff --stat $base -- quantum tmk_core platforms drivers builddefs
```

As of the merge base `7a1bbf37c5` (QMK master, October 2, 2026), 60 core files differ: 59 under `quantum/`, `tmk_core/`, `platforms/` and `drivers/` (10 of them wear-leveling tests), plus `builddefs/common_features.mk`. The repo also carries Svalboard workflows in `.github/`, docs in `docs/`, scripts in `util/` and host tests in `tests/sval_*`; none of those are firmware.

No core file tests `SVAL_ENABLE`. Core code reads Svalboard behavior only through the switches below, which the Sval module sets in `modules/svalboard/core/sval_runtime_settings.h` (included from its `post_config.h`). With none of them defined, every patched file builds as upstream, apart from the opt-in features listed further down.

| Switch | Effect in core |
|---|---|
| `RUNTIME_QMK_SETTINGS` | `GET_*()` call getters instead of using compile-time constants (`quantum/sval_hooks.h`) |
| `LAYER_SWITCH_GET_LAYER_OVERRIDE` | `layer_switch_get_layer()` asks `layer_switch_get_layer_override()` first |
| `NO_KEYMAP_FEATURE_TABLES` | `keymap_introspection.c` leaves out the keymap-array versions of combos, tap dances and key overrides |

## Upstream merge checklist

1. Merge `qmk/master` (and `qmk/develop` before a breaking-changes cycle) in a scratch worktree first. Note every conflicted file and every patched file upstream touched, even when it merged cleanly: `comm -12 <(git diff --name-only $base qmk/master | sort) <(git diff --name-only $base HEAD | sort)`.
2. Keep each runtime-setting call site to its one changed line. Resolve a conflict by taking upstream's line and putting the `GET_*()` call back in it.
3. Search for new uses of the constants the getters replace. Any new runtime use in core must go through its getter:
   ```bash
   git grep -nE 'TAP_CODE_DELAY|TAP_HOLD_CAPS_DELAY|TAPPING_TOGGLE' -- quantum
   git grep -nE 'ONESHOT_TIMEOUT|ONESHOT_TAP_TOGGLE' -- quantum
   git grep -nE 'GRAVE_ESC_.*_OVERRIDE|AUTO_SHIFT_MODIFIERS' -- quantum
   ```
   A preprocessor test such as `#if TAP_CODE_DELAY > 0` also needs `defined(RUNTIME_QMK_SETTINGS) ||` in front, or the code is left out of Svalboard builds.
4. Check that `git grep -n SVAL_ENABLE -- quantum tmk_core platforms drivers builddefs` is still empty.
5. Build `svalboard/either:sval` (plain and with `SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes`), `svalboard/either:blank`, a trackball, a TrackPoint and an Azoteq build. Compare sizes and symbols with the builds from before the merge.
6. Run the host tests: `util/updater_test/run.sh`; `python3 -m unittest discover` in `tests/sval_storage`, `tests/sval_layers` and `tests/sval_runtime_tap` (`-p 'test_*.py'`); `tests/sval_pointing/test_detect.py`; and `make test:` for `sval_runtime_tap`, `sval_runtime_tap/auto_shift`, `sval_runtime_tap/combo`, `sval_runtime_tap/static`, `auto_shift`, `basic`, every `tap_hold_configurations/*`, `combo`, `tap_dance`, `caps_word`, and every `wear_leveling_*` suite.
7. Svalboard sets its own `EECONFIG_MAGIC_NUMBER` (`keyboards/svalboard/config.h`). If upstream lowers its value to the same or below Svalboard's, pick a new Svalboard value below both.
8. Exercise settings migration, interrupted updates, USB identity and split reconnects on a dev board before calling a merge release-ready.

### Known future conflict: `store_or_get_action` (#25349)

Upstream `develop` commit `bf56a071d2` ("store_or_get_action - cache keycode on key press, return cached keycode on key release", #25349) adds a keycode cache (`KEYCODE_CACHE_ENABLE`) to `quantum/action_layer.c`, in the functions around `layer_switch_get_layer()`. A trial merge of `develop` applies it without a textual conflict, because the Svalboard hook is four lines inside `layer_switch_get_layer()`. When it reaches `master`:

- Check that a key press still resolves its layer through `layer_switch_get_layer()`, so the host's context layer is honored, and that the release still uses the layer the press used.
- Run `python3 -m unittest discover -s tests/sval_storage -p test_context_layer.py`. It compiles the production `action_layer.c` functions by name and fails if they are renamed or merged.
- Svalboard does not enable `KEYCODE_CACHE_ENABLE`. If it ever does, the cached keycode must be the one the context layer chose at press time.

## Patch list

Lines are added plus removed against the merge base. "Opt-in" means the change only takes effect when a keyboard defines the named option; otherwise the file behaves as upstream.

### Runtime QMK Settings (`quantum/sval_hooks.h`)

QMK Settings (Keybard's Typing behavior page) change these values at run time; upstream fixes them at compile time. Each use in core reads a `GET_*()` macro, following QMK's `GET_TAPPING_TERM()` pattern. Without `RUNTIME_QMK_SETTINGS` the macro is the constant, so the generated code is upstream's. With it, the macro calls a getter: `quantum/sval_hooks.c` holds weak defaults that return the constants, and `modules/svalboard/core/sval_qmk_settings.c` overrides them with the stored values.

| Setting (QSID) | Getter | Core sites |
|---|---|---|
| Tap code delay (18) | `get_tap_code_delay()` | `action.c`, `action_tapping.c`, `quantum.c`, `send_string.c`, `dynamic_keymap.c` (macro delay), `process_auto_shift.c`, `process_combo.c`, `process_quantum.c`, `process_tap_dance.c` |
| Caps Lock tap delay (19) | `get_tap_hold_caps_delay()` | `action.c`, `quantum.c` |
| Tapping toggle (20) | `get_tapping_toggle()` | `action.c`, `process_haptic.c`, `pointing_device_auto_mouse.c` |
| One-shot timeout (6) | `get_oneshot_timeout()` | `action_util.c` |
| One-shot tap toggle (5) | `get_oneshot_tap_toggle()` | `action.c` |
| Grave Escape overrides (1) | `get_grave_esc_overrides()` | `process_grave_esc.c` |
| Auto Shift modifiers (3, bit 1) | `get_auto_shift_modifiers()` | `process_auto_shift.c` |

These settings need no core patch, because QMK already has a hook or setter for them: tapping term, permissive hold, hold on other key, retro tapping, quick tap term, chordal hold and flow tap (the `*_PER_KEY` callbacks); combo term (`get_combo_term()`); the Auto Shift timeout (`set_autoshift_timeout()`); Auto Shift on and off (`autoshift_enable()`); the Auto Shift no-alpha, no-numeric and no-special bits (`get_auto_shifted_key()`); Auto Shift repeat and no-auto-repeat (`get_auto_shift_repeat()`, `get_auto_shift_no_auto_repeat()`); mouse keys (the `mk_*` variables); and the leader timeout (`LEADER_TIMEOUT` as a variable).

The module also defines `ONESHOT_TIMEOUT`, `ONESHOT_TAP_TOGGLE` and all four `GRAVE_ESC_*_OVERRIDE` when the keyboard doesn't, so QMK compiles that code in. The getters then supply the values, and the numbers are only the reset defaults. `sval_runtime_settings.h` stops the build if a keyboard sets `ONESHOT_TIMEOUT` to 0, `ONESHOT_TAP_TOGGLE` to 0 or 1, or `TAPPING_TOGGLE` to 0, since each would compile its runtime setting out.

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/sval_hooks.h` | 95 (new) | `GET_*()` macros, getter prototypes, Grave Escape bit values, the `layer_switch_get_layer_override()` prototype | New file, never conflicts. If upstream adds its own getter for one of these values, switch to it and drop ours |
| `quantum/sval_hooks.c` | 54 (new) | Weak defaults returning the compile-time constants | As above |
| `builddefs/common_features.mk` | 2 | `QUANTUM_SRC += $(QUANTUM_DIR)/sval_hooks.c`, on its own line after the core source list | Keep it outside upstream's list so edits to the list don't conflict |
| `quantum/action.h` | 1 | `#include "sval_hooks.h"` | Every user of the getters reaches them through `action.h` |
| `quantum/action.c` | 70 | One line per site for the delays and the tapping toggle. One-shot mods: the toggle and unlock tests also require `GET_ONESHOT_TAP_TOGGLE() > 1`. One-shot layer: the toggle block runs inside `if (GET_ONESHOT_TAP_TOGGLE() > 1) { ... break; }` and otherwise falls through to upstream's non-toggle code, because a stored 0 or 1 means no locking | The one-shot layer block is the only restructured code. Re-check it whenever upstream edits `OP_ONESHOT` |
| `quantum/action_util.c` | 6 | The three timed-out tests read `GET_ONESHOT_TIMEOUT()` and treat 0 as no timeout | One line each |
| `quantum/action_tapping.c` | 2 | Speculative hold delay | One line |
| `quantum/quantum.c` | 2 | `tap_code16()` delays | One line |
| `quantum/send_string/send_string.c`, `.h` | 17, 16 | Default delay from the getter. With `RUNTIME_QMK_SETTINGS` the interval type `send_string_interval_t` is `uint16_t`, so stored delays above 255 ms aren't cut short; otherwise it stays upstream's `uint8_t` | Keep every `send_string*_with_delay*()` signature on `send_string_interval_t` |
| `quantum/process_keycode/process_auto_shift.c` | 43 | Release delay from the getter, guarded by `defined(RUNTIME_QMK_SETTINGS) \|\| TAP_CODE_DELAY > 0`. The `#ifndef AUTO_SHIFT_MODIFIERS` press branch runs only when `GET_AUTO_SHIFT_MODIFIERS()` is false. With `RUNTIME_QMK_SETTINGS`, `autoshift_toggle()` and `autoshift_disable()` first finish a pending key (`autoshift_end_pending()`), so switching Auto Shift off from QMK Settings never sends that key shifted later | `autoshift_end()` and `autoshift_flags` are static: keep the helper next to them |
| `quantum/process_keycode/process_combo.c` | 8 | Key-buffer delay from the getter, with the same guard as Auto Shift | One line plus the guard |
| `quantum/process_keycode/process_grave_esc.c` | 8 | Each override also tests its bit in `GET_GRAVE_ESC_OVERRIDES()` | One line each |
| `quantum/process_keycode/process_haptic.c`, `process_quantum.c`, `process_tap_dance.c` | 2, 10, 4 | Tapping toggle and tap code delay from the getters | One line each |
| `quantum/pointing_device/pointing_device_auto_mouse.c` | 2 | The `TT()` tap count from the getter | One line |

### Macro text

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/send_string/send_string.c` | 3 (of the 17 above) | `send_char_with_delay()` skips bytes of 128 and above with no keypress. The ASCII lookup tables have 128 entries, so a UTF-8 character typed into a dynamic macro read past them and typed junk. Every macro and `send_string()` path reaches the tables through this function. Host test: `RuntimeTap.NonAsciiBytesAreSkippedWithoutAKeypress` in `tests/sval_runtime_tap` | Upstream PR candidate. Keep the check ahead of the table reads |

### Context layers

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/action_layer.c` | 4 | With `LAYER_SWITCH_GET_LAYER_OVERRIDE`, `layer_switch_get_layer()` first asks `layer_switch_get_layer_override()`, which returns a layer or `UINT8_MAX`. The Sval module's override (`sval_context_layer.c`) lets layers the user turned on win, then the host's context layer, then QMK's own lookup | See #25349 above |

### Dynamic combos, tap dances and key overrides

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/keymap_introspection.c` | 6 | `&& !defined(NO_KEYMAP_FEATURE_TABLES)` on the combo, tap dance and key override sections. Sval supplies `combo_count()`, `combo_get()` and the rest from EEPROM, and its keymaps have no `key_combos[]` or similar arrays | Upstream PR candidate: weak `*_count_raw()` and `*_get_raw()` would remove it |

### VIA storage and protocol

Shipped boards depend on this stored layout, so these changes can't be dropped without a migration.

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/via.c`, `via.h` | 91, 7 | The EEPROM magic is a layout stamp (a hash of what decides how the VIA region is read) instead of the build date, so ordinary updates keep keymaps and macros. A stored keycode version resets stored keycodes when QMK renumbers them (`via_keycodes_reset_kb()`). `via_payload_length()` lets a keyboard that wraps VIA commands shrink the usable report. Buffer get and set sizes are clamped to the report. The macro count reply carries a high byte | Check VIA command changes, `via_init()` ordering and the magic layout |
| `quantum/layout_stamp.h` | 30 (new) | The stamp hash | New file |
| `quantum/nvm/eeprom/nvm_eeprom_via_internal.h` | 7 | The keycode-version byte at `VIA_EEPROM_MAGIC_ADDR + 3`; layout options move to `+ 4` | Stored-data layout: never move these |
| `quantum/nvm/eeprom/nvm_via.c`, `quantum/nvm/nvm_via.h` | 8, 3 | Read and write the keycode version | |
| `quantum/nvm/eeprom/nvm_dynamic_keymap.c`, `quantum/nvm/nvm_dynamic_keymap.h` | 46, 3 | `nvm_dynamic_keymap_layout_stamp()`; 32-bit macro buffer offsets; with `DYNAMIC_KEYMAP_STAMP_MAX_ADDR`, the stamp uses the macro buffer's original end, so keeping combo names after the buffer (Sval `post_config.h`) doesn't reset keymaps | Check NVM addresses and the stamp inputs |
| `quantum/dynamic_keymap.c`, `.h` | 17, 8 | 16-bit macro count (at most 256), 32-bit buffer offsets, weak `dynamic_keymap_macro_send()`. The macro delay line is the tap code delay getter | |
| `quantum/raw_hid.c` | 2 | Weak `raw_hid_send()`, so the keyboard can wrap replies | |

### Wear leveling (settings storage)

See [settings storage](../../../../keyboards/svalboard/docs/settings-storage.md).

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `quantum/wear_leveling/wear_leveling.c`, `.h`, `wear_leveling_internal.h` | 257, 18, 10 | Loading retries `WEAR_LEVELING_READ_ATTEMPTS` times. Only an erased image and checksum count as never consolidated; upstream treats any checksum mismatch that way and replays the log over zeros. A store whose consolidated data can't be read in any copy is erased and reported by `wear_leveling_data_lost()`. `WEAR_LEVELING_COPIES` keeps mirrored copies: log entries go to every copy, consolidation rewrites one copy at a time, and loading prefers a copy that holds data and replays completely, repairing any copy that differs. `backing_store_erase_range()` is required with more than one copy | Keep the retries, the erased-only rule, data-lost reporting and copies whenever upstream changes this algorithm. Run every `make test:wear_leveling_*` |
| `quantum/wear_leveling/tests/*` (10 files) | 488 | Upstream tests that seeded an invalid checksum beside a never-consolidated image now leave it erased, as QMK writes it, and check nothing is reported lost. `wear_leveling_mirror` and `wear_leveling_mirror_large` are new | |
| `platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash.c` | 103 | ROM erase beyond `PICO_FLASH_SIZE_BYTES` with a JEDEC capacity check; uncached reads, so retries reread the flash; erasing one 64 KB block (or 4 KB sector) at a time, skipping erased ones, with interrupts restored in between; skipping program operations for all-erased batches; `backing_store_erase_range()` | Could move to the keyboard as `WEAR_LEVELING_DRIVER = custom` |

### Split link

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `platforms/chibios/drivers/vendor/RP/RP2040/serial_vendor.c` | 208 | Opt-in `SERIAL_PIO_RX_DMA`: DMA receive into a ring, so the PIO receive FIFO can't overrun. Opt-in `SERIAL_USART_PINS_RUNTIME`: pins chosen at run time (`serial_usart_pins_kb()`, `serial_transport_repin()`). Dev-only `SVAL_TOUCH_DEV` counters | |
| `platforms/chibios/drivers/serial_protocol.c` | 102 | With `SERIAL_PIO_RX_DMA`, per-transaction deadlines: short for QMK's per-scan transactions, long for RPCs whose slave callbacks take time, and `serial_transport_short_rpc()` for quick RPCs. Dev-only `SVAL_TOUCH_DEV` counters | |
| `quantum/split_common/split_util.c`, `split_connection.h` (new) | 45, 38 | Opt-in `SPLIT_MIN_DISCONNECT_MS`: the link counts as disconnected once failures have lasted this long, in place of `SPLIT_MAX_CONNECTION_ERRORS` of them, so short receive deadlines never disconnect early. Host test: `tests/sval_split` | |

### USB

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `tmk_core/protocol/usb_descriptor.c`, `.h` | 155, 27 | Opt-in `SVAL_NATIVE_PTP`: the native Precision Touchpad interface, last so every other interface keeps its number; its report descriptor lives in `keyboards/svalboard/azoteq/`. Opt-in `USB_PRODUCT_STRING_RUNTIME` with weak `usb_product_string()`. `SERIAL_NUMBER_PREFIX` in front of the hardware serial (hosts detect `sval:`). `RAW_EPSIZE` settable up to 64 | QMK has no hook for an extra USB interface, so the touchpad interface stays a fork patch. Check descriptor sizes and interface numbering |
| `tmk_core/protocol/usb_ptp.h` | 24 (new) | Touchpad interface constants | New file |
| `tmk_core/protocol/chibios/usb_main.c` | 70 | `SVAL_NATIVE_PTP`: class requests for the touchpad interface go to the keyboard's handlers, and `sval_ptp_usb_reset()` runs on USB reset. Raw HID receives whole packets, so a short packet never misframes the reports after it | |
| `tmk_core/protocol/chibios/usb_endpoints.c`, `.h` | 10, 10 | The `SVAL_NATIVE_PTP` endpoint | |
| `tmk_core/protocol/chibios/usb_driver.c`, `.h`, `usb_packet.h` (new) | 29, 1, 36 | `usb_endpoint_out_receive_packet()` for whole-packet receive. Dev-only `SVAL_PERF_STATS` timing around USB sends | Host test: `tests/sval_rawhid` |

### Sensors

| File | Lines | Change | Check on merge |
|---|---|---|---|
| `drivers/sensors/azoteq_iqs5xx.c` | 186 | Opt-in `SVAL_AZOTEQ_MULTITOUCH`: every contact read in the same communication window as the mouse data, one NACK retry after a low-power wake, fixed full resolution, reset acknowledgment. Gesture button fix: a failed read no longer leaves a button stuck | Could move into `keyboards/svalboard/azoteq` (the board already uses `POINTING_DEVICE_DRIVER = custom`). The gesture fix is an upstream PR candidate |
| `drivers/sensors/pmw33xx_common.c`, `.h` | 20, 6 | Six SQUAL and shutter bytes in the same burst when `sval_squal_wanted()` (weak) says a host is polling | Upstream PR candidate |

[Client ID protocol](CLIENT_ID_PROTOCOL.md) documents communication routing separately from this inventory.
