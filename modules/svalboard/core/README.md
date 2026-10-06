# Sval: dynamic QMK configuration for Keybard

Sval is a QMK community module for configuring tap dances, combos, key overrides, alternate-repeat mappings, leader sequences, one-shot behavior, and supported QMK settings over USB. The matching Keybard client also edits keymaps/macros and exposes firmware-defined hardware menus.

For the complete user-facing feature and benefit guide, read the [Svalboard QMK + Keybard launch compendium](../../../keyboards/svalboard/docs/release/README.md). The [fork-change catalog](../../../keyboards/svalboard/docs/release/firmware-changes.md) distinguishes upstream QMK features from this fork's integration and core patches.

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

Default configuration includes `SVAL_DEFAULT_NKRO`, `SVAL_DEFAULT_PERMISSIVE_HOLD`, `SVAL_DEFAULT_CHORDAL_HOLD`, `SVAL_DEFAULT_HOLD_ON_OTHER_KEY`, and `SVAL_DEFAULT_RETRO_TAPPING`. A default flag or stored schema field alone does not establish that the corresponding upstream behavior is enabled and wired; see the catalog's [scope notes](../../../keyboards/svalboard/docs/release/firmware-changes.md#scope-and-compatibility).

The module provides per-key `_sval` hooks for tapping behavior where implemented. Check [`sval_qmk_settings.c`](sval_qmk_settings.c) and [`sval_tap_dance.c`](sval_tap_dance.c) before overriding a QMK hook already owned by Sval.

Define `SVAL_KEYBOARD_UID` for file/device identification; a legacy `VIAL_KEYBOARD_UID` is accepted as a fallback. The UID identifies the definition family, while the board's persistent USB serial identifies the individual hardware.

## Storage and update behavior

| Section | Size |
| --- | --- |
| Tap dances | entry count × 10 bytes |
| Combos | entry count × 12 bytes |
| Key overrides | entry count × 12 bytes |
| Alternate repeat | entry count × 6 bytes |
| One-shot settings | 3 bytes |
| Leaders | entry count × 14 bytes |
| Sval validity stamp | 6 bytes |
| QMK settings | 44 bytes |
| Fragment selections | 21 bytes |
| Tap-dance labels | tap-dance count × 16 bytes |
| Macro labels | macro count × 16 bytes |
| Layer labels | layer count × 16 bytes |

The implementation is in [`sval.h`](sval.h) and [`post_config.h`](post_config.h). Keymap/macro storage and board custom configuration occupy separate regions of the shared logical EEPROM.

Sval validity uses a **layout stamp**, not a build timestamp. Compatible updates preserve settings; changes to geometry or schema can invalidate the relevant data. VIA also stores a keycode version so supported renumberings can be translated. This does not guarantee preservation across arbitrary firmware versions.

On Svalboard, [`migrate_vial.c`](../../../keyboards/svalboard/migrate_vial.c) implements the supported one-time shipped-Vial migration. The module alone does not provide a general Vial importer.

## Host protocol and compatibility

Firmware advertises the `sval:` USB serial prefix and dedicated HID usage. The matching host bootstraps a client ID through wrapper prefix `0xDD`, then sends Sval commands under `0xDF` or wrapped VIA commands under `0xFE`. Unwrapped Sval commands are ignored. See [Client ID protocol](docs/CLIENT_ID_PROTOCOL.md).

Current Sval protocol version **3** includes 16-bit table indices, sparse table/label reads, and 32-bit macro-buffer offsets. The full macro capacity requires a compatible Sval client. Legacy VIA macro commands retain their 16-bit addressing limit.

The maintained module name is `svalboard/core`, the regular keymap is `sval`, and definitions use `sval.json`. Earlier names in historical files or internal client identifiers are not an alternative supported mixed firmware/client pair. A generic Vial GUI is not the recommended client for this protocol.

## License

GPL-2.0-or-later. See the individual source headers for attribution.
