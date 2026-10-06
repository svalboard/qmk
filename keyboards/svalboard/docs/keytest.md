# On-device key-event tests

Build with `SVAL_KEYTEST=yes` to drive the normal QMK action path from a host and capture its output. This uses the same keymap, settings, and feature implementations as the normal image. The instrumentation is absent unless enabled explicitly; it does not change EEPROM layout or the stored-keycode version.

A test addresses a matrix **row and column**, rather than calling `register_code()` with the desired answer. The firmware feeds press/release events to `action_exec()` with real firmware timestamps. Dynamic key lookup, layers, tapping decisions, combos, tap dances, overrides, macros, and release processing therefore run normally. The regular QMK tick continues to advance held/pending actions. This bypasses optical scanning, debounce, and physical split-event transport.

The observer replaces the QMK host driver during an explicit test session. It records the final keyboard/NKRO, mouse, and consumer/system reports instead of sending them to desktop applications. LEDs and Raw HID configuration transport remain connected. Physical matrix edges are suppressed during capture so they cannot contaminate synthetic sequences. Capture ends by rebooting into normal firmware, clearing pending action state. Outside a test session the instrumented image behaves normally.

## Build and connect

From the repository root, with the usual QMK environment active:

```sh
keyboards/svalboard/tools/build-keytest.sh svalboard/left sval
keyboards/svalboard/tools/build-keytest.sh svalboard/right sval
```

The resulting files are `.build/keytest/svalboard_left_sval_keytest.uf2` and `.build/keytest/svalboard_right_sval_keytest.uf2`. Sensor targets work the same way, for example `svalboard/trackball/pmw3389/left`. Direct equivalent: `qmk compile -kb svalboard/left -km sval -e SVAL_KEYTEST=yes`.

Install the appropriate image on the USB-connected half using the existing flashing procedure. Only that half needs instrumentation to inject events at positions on either half. The physical split/scanning paths are not being tested. After the first installation, `keytest.py bootloader` lets subsequent build/flash cycles enter the UF2 bootloader without pressing a key. The existing `tools/flash.sh` can copy the next image. An application reboot used for persistence testing does not enter the bootloader.

Use a Python environment with the **hidapi** package (the module it supplies is named `hid`):

```sh
python -m pip install hidapi
python keyboards/svalboard/tools/keytest.py list
python keyboards/svalboard/tools/keytest.py --serial YOUR_SERIAL info
```

The host needs access to the device's Raw HID interface, usage page `0xFF61`, usage `0x62`. On Linux this may require the usual HID udev permissions. Close Keybard and other configuration clients while testing; this runner serializes commands and does not coordinate concurrent writers. Selection is automatic only when exactly one matching device is present. `--serial` makes selection and post-reboot reconnection explicit.

## Verify application and persistence in one loop

This example tests matrix position `(0, 0)` on layer 0 with basic HID usage `0x04` (A):

```sh
python keyboards/svalboard/tools/keytest.py --serial YOUR_SERIAL \
  --output /tmp/keytest-result.json verify-binding \
  --row 0 --col 0 --layer 0 --keycode 0x04 \
  --backup /tmp/keytest-original.json
```

The runner:

1. Reads the original binding and creates a recovery file before writing anything. It refuses to overwrite an existing recovery file.
2. Starts isolated capture and selects the requested layer in RAM.
3. Writes the binding through ordinary VIA, reads it back, and injects a timed press/release.
4. Requires the expected key report followed by a release, rejecting unexpected keyboard output.
5. Reboots, reconnects to the same serial, reads the binding again, and repeats the behavioral assertion.
6. Restores the original binding, reboots again, and verifies that restoration persisted.

The command exits nonzero on a mismatch, timeout, queue/capture error, or rollback failure. The optional JSON output contains report timestamps and bytes on success, or a failure message on error. The backup remains available even after success. If the process or USB connection is interrupted, use it to restore the binding:

```sh
python keyboards/svalboard/tools/keytest.py --serial YOUR_SERIAL restore /tmp/keytest-original.json
```

If a connection drops during capture, queued events stop and held synthetic keys are released after 30 seconds without commands. Reports remain isolated until reboot so a delayed tap-dance or macro cannot type into the desktop afterward. End such a session with:

```sh
python keyboards/svalboard/tools/keytest.py --serial YOUR_SERIAL reboot
```

A successful write reply alone is never considered evidence that the behavior changed or survived reboot.

## Feature-specific scenarios

`keytest.py run scenario.json` supports sequences of ordinary VIA/Sval exchanges, timed events, report assertions, runtime layer/modifier assertions, and reboots. Sval packets beginning with `0xDF` are automatically wrapped in a freshly bootstrapped client session. VIA packets are sent bare. Configuration writes in a scenario persist intentionally; include restoration commands or use the Python API with a saved snapshot. The automatic backup/rollback above applies specifically to `verify-binding`.

For an existing A binding at `(0, 0)`:

```json
[
  {"op": "layer", "layer": 0},
  {"op": "events", "events": [
    {"row": 0, "col": 0, "pressed": true},
    {"row": 0, "col": 0, "pressed": false, "delay_ms": 30}
  ], "settle_ms": 300},
  {"op": "expect_tap", "usage": 4, "mods": 0},
  {"op": "reboot"},
  {"op": "layer", "layer": 0},
  {"op": "events", "events": [
    {"row": 0, "col": 0, "pressed": true},
    {"row": 0, "col": 0, "pressed": false, "delay_ms": 30}
  ], "settle_ms": 300},
  {"op": "expect_tap", "usage": 4, "mods": 0}
]
```

Available steps:

| Operation | Fields and meaning |
| --- | --- |
| `exchange` | `request`: byte array; optional `expect`: exact reply prefix. Uses existing configuration protocols, including their normal saving behavior. |
| `layer` | `layer`: temporarily set the default layer and clear active layers. Does not save the layer selection. |
| `events` | `events`: array of `{row, col, pressed, delay_ms}`; delay is measured from the previous event's actual execution, or RUN for the first event. Optional `settle_ms` (default 300), `timeout` in seconds (default 20). Collects reports while waiting. |
| `expect_tap` | `usage`, optional `mods`: require that keyboard output and a subsequent all-up report. |
| `expect_report` | `match`: require at least one captured record with all specified fields. Useful for mouse buttons, consumer usages, or held modifier states. |
| `state` | Optional `expect` subset of `layers`, `default_layers`, `mods`, `weak_mods`. Values are layer bitmasks/modifier masks. |
| `reboot` | Reboot normally, reconnect, and start fresh capture. |

A batch can end with a key held, allowing a subsequent settings/table write followed by a release batch. This can test edits to active tap dances. Multi-key batches cover chords and combos; varying hold durations covers tap/hold boundaries. Set `settle_ms` to exceed the feature's configured timeout, rather than assuming 300 ms covers every possible setting. The observer reports actual firmware execution times so scheduling delays are visible.

The Python `Device` methods can also be imported for custom assertions. `events()` stages a batch then starts it; `records(cursor)` drains available reports; `collect()` waits for completion plus a settling interval. Do not run multiple readers against one capture stream.

## Diagnostic protocol v1

Channel `0x54`. INFO uses read-only VIA custom-get `0x08`; all other operations use custom-set `0x07`. The runner requires a successful INFO probe before sending a diagnostic write, so probing older firmware cannot change its settings. Write request: `[07 54 op args...]`, padded to 32 bytes. Reply: `[07 54 op status result...]`. Multi-byte diagnostic fields are little-endian; ordinary VIA keycodes retain their existing big-endian format. All diagnostic reads and writes fit the 26 usable bytes of wrapped VIA. Invalid/short packets never consume its stale tail.

Statuses: 0 OK, 1 invalid request/transition, 2 inactive, 3 busy, 4 queue full, 5 missing record, 6 aborted.

| Op | Request arguments | Result after status |
| --- | --- | --- |
| 0 INFO | none | version u8, flags u8 (active/running/aborted bits 0/1/2), rows u8, cols u8, event capacity u8, report capacity u8, queued u8, first sequence u32, next sequence u32, lost reports u32, layer count u8 |
| 1 BEGIN | ASCII `TEST` | no payload; rejects a second session or any physically held key |
| 2 ENQUEUE | delay_ms u16, row u8, col u8, pressed u8 | stages one event; rejects duplicate down/unmatched up, out-of-range positions, and delays over 60,000 ms |
| 3 RUN | none | starts the staged batch; cannot append while running |
| 4 READ | sequence u32, offset u8 | kind u8, total length u8, timestamp_ms u32, sequence u32, offset u8, up to 11 payload bytes |
| 5 CLEAR | none | clears captured records/counters; rejects a queued/running batch |
| 6 ABORT | none | releases held synthetic keys and cancels queue; capture remains isolated |
| 7 REBOOT | mode u8: 0 normal, 1 bootloader | acknowledges, then resets after at least 100 ms; requires a captured session |
| 8 STATE | none | active layers u32, default layers u32, mods u8, weak mods u8 |
| 9 SELECT_LAYER | layer u8 | RAM-only selection; rejects held/queued events |
| 10 ACK | next unread sequence u32 | frees records below the acknowledged sequence |

Record kinds and payloads:

- 1 keyboard: modifier byte followed by six HID usage bytes.
- 2 NKRO: modifier byte followed by 30 bitmap bytes (usage `n` is bit `n % 8` of bitmap byte `n / 8`).
- 3 mouse: buttons u8, X/Y/horizontal wheel/vertical wheel as four signed 16-bit values.
- 4 extra: QMK report ID u8, system/consumer usage u16.
- 5 input: row u8, column u8, pressed u8, timestamped immediately before `action_exec()`.

The queue holds 32 events and capture holds 64 records with explicit overflow accounting. The runner acknowledges drained records. A burst that overwrites unread records fails the test instead of silently losing evidence. Chunk long tests or increase the compile-time capacity deliberately when profiling large macros; a macro that blocks the main loop can fill the buffer before the host can drain it.

## What this establishes

Captured reports establish what QMK submitted to its host-driver boundary, not that the operating system received or interpreted a physical USB report. Hardware scans, debounce, sensor motion, USB enumeration, electrical behavior, and power-loss recovery still need separate tests. Steno/MIDI/joystick output paths are not captured. Ordinary action side effects still occur: do not assume capture makes a reset, EEPROM-clear, or persistent custom keycode harmless.

A normal reboot/readback test establishes persistence across MCU reset. It does not simulate unplugging the board during flash programming. The instrumentation also does not fix the runtime-setting gates identified by the fork review; it supplies a way to demonstrate those failures and verify later fixes behaviorally.

Host regression command:

```sh
python3 -m unittest discover -s tests/sval_keytest -v
```

These tests compile the complete instrumentation C file against a fake clock, driver, and action executor, checking timing, bounds, isolation, overflow, and cleanup. Python tests check decoding, behavioral assertions, and persistence-failure rollback. They validate the test instrument; actual QMK feature behavior must be exercised on an instrumented board. No hardware result should be inferred from the native mocks.
