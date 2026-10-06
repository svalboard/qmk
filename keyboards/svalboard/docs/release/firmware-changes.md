# What Svalboard QMK changes, and why

[Launch compendium](README.md) · [Launch announcement](announcement.md)

Svalboard QMK builds on upstream QMK. Ordinary keycodes, layers, mod-taps, one-shot modifiers, and the underlying combo/tap-dance/override engines are QMK capabilities. This fork adds the dynamic configuration, persistence, host protocol, and Svalboard integration that make them usable together in Keybard.

This catalog compares firmware `f2a842b1d3e839cd35c8d85a1f8198014c269361` with upstream QMK `7a1bbf37c5c07da4ea0a162bb139083a46ef40a1`, its current merge base. The QMK-core inventory contains **16 added or modified files** in `quantum/` and `tmk_core/`; board code, community modules, build tooling, and tests are additional. Historical experiments and reverted implementations are not separate launch features.

## Dynamic configuration and host communication

| Fork change | Benefit | Implementation |
| --- | --- | --- |
| Sval community module | Makes tap dances, combos, overrides, alternate repeat, leaders, one-shot settings, and QMK settings configurable at runtime. | [`modules/svalboard/core/`](../../../../modules/svalboard/core/) |
| Firmware-generated compressed definition | Keybard learns matrix geometry, available feature counts, custom controls, and visual fragments from the firmware. Builds do not need a separate hand-maintained GUI definition. | [`sval_config.py`](../../../../modules/svalboard/core/sval_config.py), [`sval_compress.py`](../../../../modules/svalboard/core/sval_compress.py), [`sval_definition.c`](../../../../modules/svalboard/core/sval_definition.c) |
| 256 entries for each dynamic table | Provides substantial room for behaviors without recompiling to increase an individual table. Counts come from each maintained keymap's `sval.json`. | [`sval.json`](../../keymaps/sval/sval.json), [`sval.h`](../../../../modules/svalboard/core/sval.h) |
| 256 macro IDs | Extends the old 50-macro Svalboard configuration. Standard QMK macro keycodes address 0–127; the Sval hook handles 128–255. | [`config.h`](../../config.h), [`sval.c`](../../../../modules/svalboard/core/sval.c) |
| Extended macro execution | Executes 16-bit keycodes and binary delays as well as ordinary string actions, so a macro can include layer and other complex key actions. | [`sval.c`](../../../../modules/svalboard/core/sval.c), [`dynamic_keymap.c`](../../../../quantum/dynamic_keymap.c) |
| Alternate repeat integrated with QMK Repeat | Gives the editor's configurable alternate-repeat entries actual keyboard behavior. | [`sval_alt_repeat_key.c`](../../../../modules/svalboard/core/sval_alt_repeat_key.c) |
| Dynamic leader sequences | Stores up to five input keycodes and an output, with configurable timing. | [`sval_leader.c`](../../../../modules/svalboard/core/sval_leader.c) |
| One-shot timeout extension hook | Provides a core extension point for runtime timing; the current Sval override is disabled, so this is integration infrastructure rather than a working editor-controlled timeout. | [`action_util.c`](../../../../quantum/action_util.c), [`action_util.h`](../../../../quantum/action_util.h) |
| Dynamic introspection ownership | Lets Sval supply combo, tap-dance, and override tables without colliding with QMK's static-array implementations. Other builds retain the static path. | [`keymap_introspection.c`](../../../../quantum/keymap_introspection.c) |
| Correct community-module record hook | Ensures Sval's tap-dance processing and high macro IDs are reached during normal key processing. | [`sval.c`](../../../../modules/svalboard/core/sval.c) |
| Client-ID wrapper and renewal | Tags replies for the requesting editor, reducing cross-talk between sessions. A nonce bootstrap and expiring lease let Keybard renew its session. | [`client_wrapper.c`](../../../../modules/svalboard/core/client_wrapper.c), [`CLIENT_ID_PROTOCOL.md`](../../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md) |
| Dedicated Sval HID usage and detection prefix | Distinguishes the Sval client path from legacy Vial and keeps detection consistent as the board's identity changes. | [`rules.mk`](../../../../modules/svalboard/core/rules.mk), [`identity.c`](../../identity.c) |
| Protocol v3 and sparse table scans | Lets Keybard ask for the next occupied entry instead of retrieving all 256 slots in every table. Wide indices accommodate the expanded tables. | [`sval.h`](../../../../modules/svalboard/core/sval.h), [`sval.c`](../../../../modules/svalboard/core/sval.c) |
| Layer-state queries and updates | Lets compatible host tools inspect or change the active layer state. | [`sval.c`](../../../../modules/svalboard/core/sval.c) |
| Fragment selection storage | Saves supported cluster choices so a compatible editor can represent the board's physical layout. | [`sval_fragments.c`](../../../../modules/svalboard/core/sval_fragments.c) |
| Firmware label storage and sparse label reads | Provides fixed-size names for layers, tap dances, and macros to clients that use the label commands. | [`sval.h`](../../../../modules/svalboard/core/sval.h), [`sval.c`](../../../../modules/svalboard/core/sval.c) |

Client IDs route communication; they are not exclusive edit locks, user authentication, or access control between editors. Two connected clients can still overwrite the same configuration.

## Storage, upgrades, and board identity

| Fork change | Benefit | Implementation |
| --- | --- | --- |
| 128 KiB logical settings on a 512 KiB flash backing region | Expands room for the feature tables and macro payload, with a large append log between consolidations. | [`config.h`](../../config.h) |
| New store below the legacy region | Leaves the old Vial store untouched for migration and downgrade. Later Sval edits are not copied back into that old store. | [`config.h`](../../config.h), [`migrate_vial.c`](../../migrate_vial.c) |
| 32-bit macro offsets and size | Removes the 64 KiB addressing ceiling for Sval-aware clients; macro IDs remain 8-bit while the count is represented in 16 bits. | [`dynamic_keymap.c`](../../../../quantum/dynamic_keymap.c), [`dynamic_keymap.h`](../../../../quantum/dynamic_keymap.h), [`nvm_dynamic_keymap.c`](../../../../quantum/nvm/eeprom/nvm_dynamic_keymap.c) |
| Geometry-based layout stamps | Preserves compatible data across builds made on different days. VIA keymaps/macros, Sval tables, and board-specific settings have their own validation. | [`layout_stamp.h`](../../../../quantum/layout_stamp.h), [`via.c`](../../../../quantum/via.c), [`sval.c`](../../../../modules/svalboard/core/sval.c), [`svalboard.c`](../../svalboard.c) |
| Separate stored keycode version | Allows supported numbering changes to be translated rather than changing the layout stamp and wiping the setup. | [`keycode_upgrade.h`](../../../../quantum/keycode_upgrade.h), [`nvm_via.c`](../../../../quantum/nvm/eeprom/nvm_via.c) |
| Keycode upgrades across all stored behaviors | Translates keymaps, encoder maps where enabled, tap dances, combos, overrides, alternate repeat, leaders, and extended macro actions. The current translation handles old steno codes. | [`dynamic_keymap.c`](../../../../quantum/dynamic_keymap.c), [`sval.c`](../../../../modules/svalboard/core/sval.c) |
| One-time migration of the supported shipped Vial layout | Carries the old core settings, mapping, macros, dynamic features, pointing settings, and layer colors into the new layout. Custom Svalboard keycodes are translated too. | [`migrate_vial.c`](../../migrate_vial.c) |
| Persistent pending/completed migration flags | Allocation failure or an interrupted copy can retry from the intact old store. A completed migration stays completed across a later settings wipe. | [`identity.h`](../../identity.h), [`identity.c`](../../identity.c), [`migrate_vial.c`](../../migrate_vial.c) |
| Persistent serial with `sval:` prefix | Distinguishes boards and preserves the serial through ordinary reflashes, avoiding a new host device identity every time. Existing browser grants still depend on browser/OS behavior. | [`identity.c`](../../identity.c), [`usb_descriptor.c`](../../../../tmk_core/protocol/usb_descriptor.c) |
| User-chosen USB product name | Makes multiple boards recognizable by name without changing their serial. | [`identity.c`](../../identity.c), [`usb_descriptor.c`](../../../../tmk_core/protocol/usb_descriptor.c) |
| Two checksummed identity records with a capacity check | Keeps an older identity copy while updating the other, and avoids touching the reserved identity region on an unsupported flash capacity. | [`identity.c`](../../identity.c) |
| Overflow-safe macro guards | Rejects requests that wrap a 32-bit offset, cross the buffer end, or exceed the packet payload. Lower storage guards also avoid wraparound; loops can span 64 KiB. | [`sval.c`](../../../../modules/svalboard/core/sval.c), [`nvm_dynamic_keymap.c`](../../../../quantum/nvm/eeprom/nvm_dynamic_keymap.c) |
| Wrapper-aware name chunks | Accounts for the reply/payload space consumed by wrapped VIA packets, avoiding truncated board-name transfers. | [`identity.c`](../../identity.c), [`client_wrapper.h`](../../../../modules/svalboard/core/client_wrapper.h) |

Storage is shared. The macro buffer is the logical EEPROM remaining after core, board, feature-table, and keymap storage; its exact size is reported by the device. It is not a separate 128 KiB macro allocation.

Wear leveling reads from RAM and skips writes of unchanged data. Normal typing and pointer motion do not append settings records. Persistent configuration edits do; consolidation occurs when the roughly 384 KiB append area fills. Full erasure is synchronous and can pause servicing, so large repeated macro uploads are more likely to encounter the pause than a settled daily-driver layout.

## Svalboard integration

| Fork change | Benefit | Implementation |
| --- | --- | --- |
| Base, PMW3360, PMW3389, TrackPoint, and Azoteq variants | Builds the correct sensor and split transport configuration for the installed pointing hardware. | [`keyboards/svalboard/`](../../), [`release.yml`](../../../../.github/workflows/release.yml) |
| Independent pointing configuration and per-pointer automouse | Makes each pointer useful in its own role and controls which movement enters the mouse layer. | [`svalboard.c`](../../svalboard.c), [`keymap_support.c`](../../keymaps/keymap_support.c) |
| Threshold and decay for mouse-layer activation | Helps reject small unintended motion while retaining purposeful activation. Scroll movements participate in the activation logic. | [`keymap_support.c`](../../keymaps/keymap_support.c) |
| Sniper/Boost hold and toggle state | Offers both precise and fast movement, with held and toggled states that compose predictably. | [`keymap_support.c`](../../keymaps/keymap_support.c), [`axis_scale.c`](../../axis_scale.c) |
| Scrolling and shared high-resolution mouse endpoint integration | Carries cursor, buttons, and scrolling through the board's combined mouse report; includes scroll behavior improvements for Mac hosts. Host support still affects the result. | [`rules.mk`](../../rules.mk), [`keymap_support.c`](../../keymaps/keymap_support.c) |
| Split USB wake handling | Improves waking a suspended host from the split board. Host remote-wakeup permission still applies. | [`svalboard.c`](../../svalboard.c) |
| Scan Lab status, probes, and timing sweeps | Measures optical matrix settling, scan frame intervals, and sensor-LED duty from the host. | [`scanlab.c`](../../scanlab.c), [`matrix.c`](../../matrix.c) |
| Active/light-idle/deep-idle pacing | Reduces LED duty and lets users choose the wake-latency tradeoff. | [`matrix.c`](../../matrix.c), [`svalboard.c`](../../svalboard.c) |
| Optional sensor rest, RGB dimming, and CPU sleep | Reduces idle power in components beyond the optical sensor LEDs. Applicability depends on the pointing hardware and enabled options. | [`trackball.c`](../../trackball/trackball.c), [`power.c`](../../power.c), [`matrix.c`](../../matrix.c) |
| Selectable deep-idle clock and long naps | Offers deeper power savings while retaining USB/split communication and restoring active operation on input. | [`power.c`](../../power.c), [`matrix.c`](../../matrix.c) |
| Host-requested bootloader entry | Allows compatible maintenance tools to enter the bootloader through a board command. | [`svalboard.c`](../../svalboard.c) |
| Maintained Sval keymap and release build matrix | Produces the regular sensor/side combinations and base blank builds, with compressed definitions generated during compilation. | [`keymaps/sval/`](../../keymaps/sval/), [`release.yml`](../../../../.github/workflows/release.yml) |

## Complete QMK-core patch inventory

This is the core maintenance surface, separate from the board/module feature inventory above.

| File(s) | Change and purpose |
| --- | --- |
| `quantum/action_util.c`, `quantum/action_util.h` | Runtime one-shot timeout hook and Sval-aware timeout handling. |
| `quantum/dynamic_keymap.c`, `quantum/dynamic_keymap.h` | Weak macro executor for the Sval override, 256 macro count support, 32-bit buffer API, and keycode upgrades. |
| `quantum/keymap_introspection.c` | Exclude static combo/tap-dance/override introspection for Sval builds. |
| `quantum/raw_hid.c` | Weak send hook so replies can be wrapped with their client ID. |
| `quantum/layout_stamp.h` | Shared layout-hash utility for compatibility validation. |
| `quantum/keycode_upgrade.h` | Supported version checks and idempotent keycode translation. |
| `quantum/nvm/eeprom/nvm_dynamic_keymap.c` | Layout stamp, wider macro-store support, bounds guards, and wide iteration. |
| `quantum/nvm/nvm_dynamic_keymap.h` | Expose the keymap-region layout stamp. |
| `quantum/nvm/eeprom/nvm_eeprom_via_internal.h` | Reserve the stored keycode-version byte alongside VIA metadata. |
| `quantum/nvm/eeprom/nvm_via.c`, `quantum/nvm/nvm_via.h` | Read/write the stored keycode version. |
| `quantum/via.c`, `quantum/via.h` | Layout-based validity, upgrade orchestration and keyboard hook, wider macro count, and legacy size clamping. |
| `tmk_core/protocol/usb_descriptor.c` | Runtime product string and a prefix on hardware-derived serial descriptors. |

## Scope and compatibility

- **Migration:** exact supported source release `svalboard/vial-qmk v2025-11-01`, keymap `vial`. Unrecognized releases do not receive a speculative conversion.
- **Preservation:** stamps preserve compatible layouts; geometry/schema changes or unsupported keycode versions can still reset data. The steno translation table is not a universal future keycode migration engine.
- **Legacy clients:** legacy VIA macro commands retain 16-bit offsets and report at most 65,535 bytes. Full capacity requires the matching Sval client and protocol v3.
- **Identity:** persistent name/serial storage uses a reserved region on supported 16 MB flash. Names appear as the new USB product string after restarting the board.
- **Labels:** the firmware provides 16-byte UTF-8 slots. Keybard has a label reader service but the reviewed device-load path does not use it, so automatic cross-browser synchronization of cosmetic names is not a launch claim.
- **QMK settings:** the schema includes Chordal Hold and Flow Tap fields, but the reviewed module does not wire those values to corresponding runtime QMK hooks. Do not advertise those fields as working runtime behavior controls without a separate implementation check. The Sval override of `get_oneshot_timeout()` is commented out, and stored one-shot tap-toggle changes are not wired to the core behavior. Treat those as stored settings rather than verified runtime controls. Auto Shift likewise requires the feature to be compiled in; it is not enabled by the launch board definition.
- **Hardware diagnostics:** matrix testing reports physical key activity to an authorized host connection. Scan Lab's estimated current is a model; it is not a direct electrical measurement.
- **Power:** idle scan periods, long naps, and trackball rest modes can increase first-input latency. Power measurements describe a particular tested setup rather than universal specifications.

## Validation and maintenance

The targeted [host regression suite](../../../../tests/sval_storage/test_regressions.py) compiles production C handlers with mocked hardware. It exercises wrapped offsets, end-of-buffer boundaries, short packets, operations larger than 64 KiB, allocation failure, interrupted migration at multiple stages, completion ordering, and identity-save failure.

```sh
python3 -m unittest discover -s tests/sval_storage -v
qmk compile -kb svalboard/trackball/pmw3389/right -km sval
```

These commands passed for the overflow/recovery implementation in `77a0dca019`; a build and mocked host tests do not replace hardware migration/power-cycle checks or verification of every sensor variant. At release time, use the build matrix for artifacts and record the actual tested firmware/editor revisions.

When merging QMK updates, compare the 16-file core inventory again, verify the NVM metadata layout and keycode table, and build the maintained sensor/side variants. There are deliberately separate user-level benefits and implementation details here: an upstream QMK feature should not be described as invented by this fork merely because Keybard makes it easier to configure.
