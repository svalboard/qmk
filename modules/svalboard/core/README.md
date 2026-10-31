# Sval: dynamic QMK configuration for Keybard

Sval is a QMK community module for configuring tap dances, combos, key overrides, alternate-repeat mappings, leader sequences, one-shot behavior, and supported QMK settings over USB. The matching Keybard client also edits keymaps/macros and exposes firmware-defined hardware menus.

Users: see the [Keybard guide](https://github.com/svalboard/keybard/blob/main/docs/guide.md). [Core patches](docs/QMK_PATCHES.md) lists this fork's changes to QMK itself.

## Build integration

Enable the module in your keymap's `keymap.json`:

```json
{
    "modules": ["svalboard/core"]
}
```

Place a `sval.json` definition in the same keymap directory. Its `sval` object determines feature counts; build scripts generate the feature flags, configuration header, and compressed definition. The maintained Svalboard keymaps configure 256 entries for each table:

```json
{
    "sval": {
        "tap_dance": 256,
        "combo": 256,
        "key_override": 256,
        "alt_repeat_key": 256,
        "leader": 256
    }
}
```

This example shows the count object, not a complete keyboard definition. Use the [maintained Svalboard definition](../../../keyboards/svalboard/keymaps/sval/sval.json) for the matrix, menus, custom keycodes, fragments, and other required fields. A missing `sval.json` is a build error.

The keyboard sets the layer/macro counts and storage capacity. The launch Svalboard build provides 16 layers, 256 macros, and 128 KiB of logical settings storage shared by the features and macro buffer. These are board settings, not the defaults for every keyboard using the module.

## Defaults and custom hooks

Default configuration includes `SVAL_DEFAULT_NKRO`, `SVAL_DEFAULT_PERMISSIVE_HOLD`, `SVAL_DEFAULT_CHORDAL_HOLD`, `SVAL_DEFAULT_HOLD_ON_OTHER_KEY`, and `SVAL_DEFAULT_RETRO_TAPPING`. A default flag or stored schema field alone does not establish that the corresponding upstream behavior is enabled and wired.

The module provides per-key `_sval` hooks for tapping behavior where implemented. Check [`sval_qmk_settings.c`](sval_qmk_settings.c) and [`sval_tap_dance.c`](sval_tap_dance.c) before overriding a QMK hook already owned by Sval.

Define `SVAL_KEYBOARD_UID` for file/device identification; a legacy `VIAL_KEYBOARD_UID` is accepted as a fallback. The UID identifies the definition family, while the board's persistent USB serial identifies the individual hardware.

## Storage and update behavior

| Section | Size |
| --- | --- |
| Tap dances | entry count × 10 bytes |
| Combos | entry count × 12 bytes |
| Key overrides | entry count × 12 bytes |
| Alternate repeat | entry count × 6 bytes |
| Leaders | entry count × 14 bytes |
| Sval validity stamp | 6 bytes |
| QMK settings | 44 bytes |
| Fragment selections | 21 bytes |
| Tap-dance labels | tap-dance count × 16 bytes |
| Macro labels | macro count × 16 bytes |
| Layer labels | layer count × 16 bytes |
| Storage-reset flag | 1 byte |

The implementation is in [`sval.h`](sval.h) and [`post_config.h`](post_config.h). Keymap/macro storage and board custom configuration occupy separate regions of the shared logical EEPROM.

Sval validity uses a **layout stamp**, not a build timestamp. Compatible updates preserve settings; changes to geometry or schema can invalidate the relevant data. VIA also stores the keycode version that wrote the keymap; when QMK renumbers keycodes, the stored keymap and the Sval tables are reset rather than translated.

A settings reset writes the QMK defaults, and a leader timeout of 300 ms with per-key timing on (`SVAL_DEFAULT_LEADER_TIMEOUT`, `SVAL_DEFAULT_LEADER_PER_KEY_TIMING`). Five settings can't work at 0: the combo term, tapping term, Auto Shift timeout, mouse key interval and leader timeout. A 0 stored in one of them is replaced with its default at boot and saved, and a host that writes 0 to one gets the default stored instead. Settings where 0 has a meaning keep it: the one-shot timeout (no timeout), the Flow Tap term (off), the quick tap term (off) and the tap code delay.

Every setting is held at the largest value the runtime variable it is applied to can use. QMK's mouse key variables are 8-bit: the intervals, speeds and times to max go up to 255, and the cursor and wheel delays, stored in ms and applied in 10 ms units, up to 2550 ms. QMK counts at most 15 taps, so the tapping toggle and the one-shot tap toggle go up to 15. A larger stored value is held at the limit at boot and on a write, so it can't wrap (an interval of 256 used to become 0 and move the cursor every scan). The mouse key step size (QSID 11) is QMK's compile-time `MOUSEKEY_MOVE_DELTA` (info.json `mousekey.move_delta`); a write is accepted and has no effect, and the setting reads back as the step the board uses.

The wheel delay and wheel interval (QSIDs 14 and 15) are applied, as Vial did. Earlier firmware (v4.3.3 and before) stored them without applying them, so on the first boot of firmware that applies them, a stored wheel interval of 0 (which would scroll every scan, a speed the board never had) becomes the default. The `migrations` byte of the settings block records that the migration ran; after it, a 0 is kept.

The firmware never reads configuration stored by other firmware, Vial included. Setups move between firmwares as layout files, which store keycodes by name.

## Host protocol and compatibility

Firmware advertises the `sval:` USB serial prefix and dedicated HID usage. The matching host bootstraps a client ID through wrapper prefix `0xDD`, then sends Sval commands under `0xDF` or wrapped VIA commands under `0xFE`. Unwrapped Sval commands are ignored. See [Client ID protocol](docs/CLIENT_ID_PROTOCOL.md).

The read-only layer query also advertises active/default-mask reporting through feature bit 6; see [layer-state wire compatibility](docs/LAYER_STATE_PROTOCOL.md). Older hosts retain the same active-mask offset.

`GET_INFO` also reports the QMK keycode numbering the firmware uses (`QMK_KEYCODES_VERSION`) as three bytes after the feature flags (major, minor, patch). Hosts number keycodes to match it; all zero means older firmware that does not report it, which was always 0.0.9.

The byte after those (response byte 18) is nonzero when the board reset its settings because storage could not be read. It stays set across reboots until the host sends `0x2A` (`sval_cmd_storage_reset_clear`), so a host should tell the user to reload their layout file, then clear it. Older firmware leaves it zero.

Current Sval protocol version **3** includes 16-bit table indices, sparse table/label reads, and 32-bit macro-buffer offsets. The full macro capacity requires a compatible Sval client. Legacy VIA macro commands retain their 16-bit addressing limit.

The maintained module name is `svalboard/core`, the regular keymap is `sval`, and definitions use `sval.json`. Earlier names in historical files or internal client identifiers are not an alternative supported mixed firmware/client pair. A generic Vial GUI is not the recommended client for this protocol.

## License

GPL-2.0-or-later. See the individual source headers for attribution.
