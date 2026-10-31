# The Sval protocol

Svalboard QMK gives desktop apps a two-way connection to the keyboard over Raw HID, alongside its normal keyboard and mouse interfaces. Keybard uses it through the browser's WebHID; native apps can use any HID library. The keyboard runs its saved layout on its own, so no app has to stay open.

## What an app can do

**Read:** the board definition and geometry, every layer's key assignments, tap dances, combos, overrides, macros and the other behaviors, settings, active and default layers, and which physical keys are held.

**Write:** key assignments, behaviors, macros, pointing and other settings, and the active layer. Selecting a layer changes runtime state only; it doesn't rewrite the stored layout.

Host apps can read and control layer state.

## Sharing the keyboard between apps

Every message is one 64-byte report (it was 32 bytes before this release; apps take the size from the HID descriptor):

| `0xDD` | client ID (4 bytes) | `0xDF` Sval or `0xFE` VIA | payload |
| --- | --- | --- | --- |

An app asks for a client ID by sending a random 20-byte nonce with ID `0`. The keyboard echoes the nonce with a new ID and a lease of about two minutes. The app sends that ID with each request, the keyboard echoes it in the reply, and each app keeps only its own replies. Before the lease ends, the app asks for a new ID. The keyboard stores nothing per app, so a crashed app's ID simply expires.

Client IDs keep replies apart; they don't lock settings. Two apps editing the same thing can overwrite each other, so use one editor and let other tools read.

## Limits

- Apps poll for state; there is no push stream of key events, so a very short tap can fall between two polls.
- There is no notification when the layout changes. An app that caches the layout, such as a trainer, should reload after you edit.
- Whether two apps can open the keyboard at once depends on the operating system.

## Version 3

The launch firmware speaks Sval protocol version 3:
- 16-bit indices reach all 256 entries of each behavior table.
- Sparse reads skip empty entries.
- 32-bit macro offsets reach past 64 KiB.
- 32-bit layer masks report active and default layers.
- `GET_INFO` reports the protocol version, feature flags and the QMK keycode numbering the board uses.
- `GET_INFO` byte 18 reports storage events: bit 0, settings were reset because storage could not be read; bit 1, a change could not be saved since this boot; bit 2, settings were reset to the build's defaults because a different firmware build was installed. Bits 0 and 2 stay set until the host sends `0x2A`. Older firmware never sets bit 2.

Check feature flags before using an extension, send one request at a time per connection, and check the client ID and command in every reply.

## Pointing hardware

`POINTING_HARDWARE` (`0x2C`) tells a host which side each half thinks it is, which pointing module its firmware drives, and which module actually answered. Released firmware drives whichever module is attached: one image per side, which probes for the module at startup (flags bit 3). The per-module builds (`svalboard/trackball/pmw3360`, `pmw3389`, `trackpoint`, `azoteq`) still build but are not released; on one of those the wrong module stays dead with no other sign, so a host can use this report to tell the user. On an auto build, `built_for` `0` means no module has answered yet. The half looks again during its first 30 s (below), so a module that comes up or is plugged in by then starts on its own and the report changes; one plugged in later starts after a replug.

- Discovery: GET_INFO byte 19 (the second feature byte), bit 1 (`sval_flag2_pointing_hardware`). Older firmware leaves it zero.
- Request: `DF 2C`
- Response: `DF 2C <format> <this half: 5 bytes> <other half: 5 bytes>`. `format` is `1`; `0` means the board has nothing to report. "This half" is the one plugged into USB.
- Each half: `<flags> <built_for> <detected> <Product_ID> <Inverse_Product_ID>`.
  - `flags`: bit 0 the block holds a report, bit 1 the half is the left half, bit 2 the sensor driver initialized, bit 3 the firmware drives whichever module is attached (the plain `left`/`right` builds), so `built_for` is the module it chose after the probe, or `0` when none answered. Bit 5: the half has no side yet (one-image builds, see Side), and bit 1 means nothing.
  - `built_for`: `0` no pointing module, `1` PMW3360 trackball, `2` PMW3389 trackball, `5` Azoteq trackpad, `6` TrackPoint.
  - `detected`: `0` not checked yet, `1` PMW3360 (`42 BD`), `2` PMW3389 (`47 B8`), `3` no module answered, `4` the trackball bus answered with some other ID, `5` Azoteq (IDs: product number, high byte first), `6` TrackPoint (first ID byte: its PS/2 device ID).
  - Each build checks its own module at startup. When that module doesn't answer, or the build has none, the half checks the other modules' buses about 1.5 s after boot (PS/2, then I2C, then SPI), so `detected` names whatever is attached. Until then it reads `0` (no module build) or `3` (its own module missing). While nothing works, the half looks again, its own module first, then the other buses: 3, 6, 12 and 30 s after boot, and never after that, so a board with no pointing module isn't probed again. It stops sooner once a module works, once a per-module build finds a module it can't drive, or once a module that answers has failed to start four times. A half whose module works is never probed. So `detected`, `built_for` and bit 2 can change during a half's first 30 s, after a host has connected (the other half's block follows within a presence ping, 0.5 s); a host that shows `3` or `0` should read again until then.
- The other half reports through the split link's half-second presence ping, so its block is zero while it is disconnected, for up to half a second after it connects, and when it runs firmware without this command.

## Main board

`MAIN_BOARD` (`0x38`) tells a host which main board each half has, and, on a SvalMCUMini V1.0, whether every key cluster on it is one the board can scan. The firmware decides both at startup, so one image runs on every main board.

- Discovery: GET_INFO byte 20 (the third feature byte), bit 0 (`sval_flag3_main_board`). Older firmware leaves it zero.
- Request: `DF 38`
- Response: `DF 38 <format> <this half: 5 bytes> <other half: 5 bytes> <board read us: u16> <cluster check us: u16>`. `format` is `1`. "This half" is the one plugged into USB; the two timings are its own boot cost (diagnostic).
- Each half: `<flags> <main_board> <gp3> <clusters: u16>`.
  - `flags`: bit 0 the block holds a report, bit 1 the half is the left half, bit 2 the cluster check ran, bit 3 a cluster this main board can't scan was found, bit 5 the half has no side yet (one-image builds), and bit 1 means nothing.
  - `main_board`: `0` not identified (the samples disagreed; the half keeps the regular scan timing), `1` SvalMCUMini V1.0, `2` V3.1 or V3.2, `3` V3.0 or V2B.
  - `gp3`: diagnostic. Bits 0-2 are the last sample's pull-down, pull-up and release reads; bits 4-7 how many of the 8 samples agreed.
  - `clusters`: 2 bits per row, row 0 (thumb) in bits 0-1, then index, middle, ring and pinky. `0` not checked, `1` lit within 60 us (a cluster made for this board), `2` dark at 60 us and lit by 400 us (an original cluster: it can't be scanned on a SvalMCUMini V1.0), `3` dark at both (no cluster, or its lit-at-rest keys held). Row 0 (thumb) is not checked and reads `0`.
- How the board is found: GP3 is read with the pull-down (high only when the V3.1/V3.2 op-amp drives it), then, if low, driven high for 5 us, released with no pull and read 20 us later (low on the Mini, whose R1 30k pulls it to ground; high when it floats). Eight samples must agree.
- The cluster check runs only on a SvalMCUMini V1.0, before the first scan, on the four finger rows. Other boards report `clusters` `0`. A row that reads `2` (an original cluster) has its keys muted, because its center would otherwise read as held; Scan Lab still sees the raw row.
- The other half reports through the split link's half-second presence ping, so its block is zero while it is disconnected, for up to half a second after it connects, and when it runs firmware without this command.

## Pointing fragments

The keyboard definition (`sval.json`, keymaps `sval` and `blank`) describes each hand's pointing module as a zero-key fragment, so a host can draw it in the layout. Fragments `pointing_none` (4), `pointing_trackball_pmw3360` (5), `pointing_trackball_pmw3389` (6), `pointing_touchpad_azoteq` (7), `pointing_trackpoint` (8) and `pointing_unknown` (9) have `"kle": []` and a `widget` object (`kind`, `module`, `sensor`, `w`, `h` in key units). Instances `left_pointing` (10) and `right_pointing` (11) have `hardware_detect: true`; their selection bytes are not used.

`FRAGMENT_GET_HARDWARE` (`0x18`) reports them from the same identity `POINTING_HARDWARE` returns, for whichever half is on that hand (the other half's only while it is connected): `detected` 1 → 5, 2 → 6, 3 → 4, 4 → 9, 5 → 7, 6 → 8. It reads `0xFF` while that hand isn't known yet: no half on that side, `detected` still `0`, or `3` before the boot probe of the other buses has run (for the other half, until 2.5 s after the USB half booted). A host can re-read `0x18` until both bytes settle. A hand that reads `4` (no module) can still change until that half has run for 30 s, if its search finds one.

## Pointing motion

`POINTING_MOTION` (`0x2D`) returns each hand's pointing motion since the previous read and the pointing modes in force, for a live picture of the modules. Reading clears the counts.

- Discovery: GET_INFO byte 19, bit 3 (`sval_flag2_pointing_motion`, `0x08`). Older firmware leaves it zero.
- Request: `DF 2D`
- Response: `DF 2D` then 24 bytes (offsets from the first of them). `format` `0` means no data.

| off | size | field |
|---|---|---|
| 0 | 1 | format = `1` |
| 1 | 1 | modes: bit 0 sniper (precision) active, bit 1 boost active, bit 2 auto-mouse layer on, bit 3 axis scroll lock in effect (the setting is on and the host isn't macOS), bit 4 natural scroll, bit 5 a scroll hold or scroll toggle key is active |
| 2 | 1 | ms since the previous read, up to 255; `0` on the first read after boot |
| 3 | 10 | left hand |
| 13 | 10 | right hand |
| 23 | 1 | axis lock: `0` free, `1` locked vertical, `2` locked horizontal (`0` while axis lock is off) |

Each hand:

| off | size | field |
|---|---|---|
| 0 | 1 | flags: bit 0 valid (that hand's driver started and its reports reach the USB half), bit 1 its motion is turned into scrolling, bit 2 a value below was clamped since the last read, bit 3 auto-mouse is enabled for this hand, bit 4 touch position valid, bits 5-6 finger count (0 unknown), bit 7 zero |
| 1 | 2 | dx, int16 little-endian (+right) |
| 3 | 2 | dy, int16 little-endian (+down) |
| 5 | 1 | h, int8 (wheel, +right) |
| 6 | 1 | v, int8 (wheel, +up) |
| 7 | 1 | buttons pressed since the last read (OR) |
| 8 | 1 | touch x, 0-255 across the pad (only with bit 4) |
| 9 | 1 | touch y, 0-255 down the pad (only with bit 4) |

- The counts are both halves' reports in the native split pointing path (`pointing_device_task_combined_kb`), after rotation and invert and before sniper, boost and scroll conversion, so they are raw sensor counts at the hand's CPI. Scroll-mode motion still shows in dx/dy with bit 1 set; h and v carry what the module itself reports as wheel (an Azoteq scroll gesture).
- Motion is counted only while a host app has read `POINTING_MOTION` within the last 1.5 s (`SVAL_HOST_ACTIVE_MS`). These reads don't count as host activity, so a host that keeps polling doesn't hold the board out of deep idle; a read can then take up to one deep-idle nap (20 ms) to answer.
- A hand that is not valid (no module, the driver didn't start, or the other half is disconnected) has an all-zero block.
- Touch positions and finger counts are not reported yet: bit 4 and bits 5-6 read `0`.

### Velocity Scroll

Builds with Velocity Scroll (`SV_VELOCITY_SCROLL`, and its toggle `SV_VELOCITY_SCROLL_TOGGLE`) add a 24-byte block at offsets 29-52, so a host can show it on, the offset from the anchor and the speed it sets, and draw the dead zone and the curve without knowing the build's tunables. Bytes 24-28 are left for the per-reader extension, so the two never overlap.

- Discovery: byte 29 reads `0x56` (`V`). Firmware without the block leaves it zero (the reply is zero-filled), and so does a client-wrapped 32-byte report, which has no room for it.
- The block is in every reply from such a build, on or not; only the state fields are zero while it is off.

| off | size | field |
|---|---|---|
| 29 | 1 | `0x56`: the block is present (format 1) |
| 30 | 1 | bit 0 Velocity Scroll is on (its key held or its toggle on), bit 1 a finger on a touchpad sets the offset (else the relative pointers: trackball, TrackPoint), bit 2 that touchpad is the right hand's, bit 3 the axis lock tunable is on, bits 4-5 the axis the lock picked: `1` vertical, `2` horizontal, `0` none yet |
| 31 | 2 | offset x, int16 little-endian, 0.01 mm (+right) |
| 33 | 2 | offset y, int16, 0.01 mm (+toward the user) |
| 35 | 2 | speed h, int16, 0.01 notch/s (+right) |
| 37 | 2 | speed v, int16, 0.01 notch/s (+up), before natural scroll |
| 39 | 2 | maximum speed, uint16, 0.01 notch/s |
| 41 | 2 | trackball dead zone, uint16, 0.01 mm per axis |
| 43 | 2 | trackball gain, uint16, 0.01 notch/s at 10 mm past the dead zone |
| 45 | 2 | trackball exponent, uint16, 0.001 |
| 47 | 2 | touchpad dead zone, uint16, 0.01 mm |
| 49 | 2 | touchpad gain, uint16, 0.01 notch/s |
| 51 | 2 | touchpad exponent, uint16, 0.001 |

- The offset is the one that sets the speed now. While bit 1 is set, it is a touchpad finger's position minus its anchor: where the finger was at the first sample after key-down, or where it touched down if it landed later. Otherwise it is the trackball's counts since key-down, in mm of ball surface, clamped per axis where the speed reaches its maximum.
- Per axis, speed = gain × ((|offset| − dead zone) / 10 mm) ^ exponent, capped at the maximum, zero inside the dead zone, with the offset's sign. The speeds are the ones the board last sent at (every 8 ms), after the axis lock.
- A notch is one wheel click; the board sends high-resolution units (120 a notch) where the computer takes them.

## Pointing contacts

`POINTING_CONTACTS` (`0x2F`) returns every touch contact down now, each on its own pad, for a live picture of the touchpads. With the native touchpad the computer owns the pads and `POINTING_MOTION` carries no touch positions, so this is how a host app draws the fingers. It is a snapshot: reading clears nothing, so any number of apps can poll it.

- Discovery: GET_INFO byte 19, bit 5 (`sval_flag2_pointing_contacts`, `0x20`). Firmware without the native touchpad leaves it zero.
- Request: `DF 2F`
- Response: `DF 2F` then 28 bytes (offsets from the first of them). `format` `0` means no data (the half without USB, or no native touchpad). Firmware before 6 contacts sent 24 bytes and at most 5; read `n` contacts, not a fixed count.

| off | size | field |
|---|---|---|
| 0 | 1 | format = `1` |
| 1 | 1 | bit 0 the touchpad is reporting to the computer (Input Mode 3 and the surface switch on), bit 1 the left pad is part of it, bit 2 the right pad is |
| 2 | 1 | ms since the report these contacts come from, up to 255 |
| 3 | 1 | contacts `n`, 0 to 6 |
| 4 | 4 × n | the contacts, then zeros |

Each contact:

| off | size | field |
|---|---|---|
| 0 | 1 | bits 0-3 contact ID (the touchpad's own, stable while the finger is down), bit 4 tip (down), bit 5 confidence (clear: a palm), bit 6 right pad, bit 7 the computer doesn't have this contact (see below) |
| 1 | 3 | `x \| y << 12`, 24-bit little-endian: `x` 0-2047 across that pad, `y` 0-1791 down it |

- The contacts are the last input report the computer took from the touchpad, so they are exactly what the operating system has. A contact with tip clear is lifting: that report is its last.
- The touchpad reports at most 5 contacts, the most a Windows Precision Touchpad allows. A finger that lands while 5 are down is left out of the touchpad for as long as it stays down. `POINTING_CONTACTS` still lists such fingers after the reported ones, at their live position, with bit 7 set, up to 6 contacts in all, so three fingers on each pad all show. Contacts with bit 7 have IDs of their own (also stable while the finger is down), so the touchpad's IDs are unchanged: bit 7 and the ID together identify a contact.
- Coordinates are per pad, in the touchpad's own orientation (the one the operating system sees), not the merged surface: the right pad does not carry the 2048 + gap offset.
- While the touchpad isn't reporting, or its last report is older than 150 ms (the computer stopped reading), `n` is `0`.

## Side

`SIDE` (`0x33`) tells a host which half is which on the one-image builds (`svalboard/either`, `svalboard/trackball/pmw3389/either`), and lets it set that. Those builds run the same image on both halves. Each half takes its side at startup from, in order: a thumb strap (a left thumb with R7 from LED_DIN to ground, or a right thumb with R7 from LED_DIN to the thumb row), the build's own side on a one-side build (below), the side stored on the half, or nothing ("unknown"). The per-hand builds fix the side when they are built and don't have this command.

One-side builds (the release files `svalboard_sval_left_<tag>.uf2` and `svalboard_sval_right_<tag>.uf2`, x4.2 on; `SVAL_BUILD_SIDE`) are the one-image build with a side of its own, and have this command. They run as their side on every start and store it (source `5`), so a later one-image build keeps it; a strap still wins over them. While one runs, SET of the other side and CLEAR answer `8`.

- Discovery: GET_INFO byte 19, bit 6 (`sval_flag2_side`).
- The stored side lives in its own flash record, apart from the settings. Settings resets, `EE_CLR`, storage format changes, UF2 copies and updates keep it; only a whole-chip erase clears it. Only this command, a thumb strap or a one-side build changes it.
- The first start of a one-image build on a half that ran a per-hand build adopts that build's side (source `3`, migrated). A half that has never had a per-hand build (a new board) starts unknown.
- A half with no side still enumerates and answers this protocol, but sends no keys and no pointing. Every half enumerates as the per-hand builds do (product ID `0x4044`, or the name the user gave the board).
- The split link comes up whatever the other half's side, so the half with USB learns it (POINTING_HARDWARE flags bit 5 means "no side"). While the other half has no side, or both halves claim the same side, neither half's keys or pointing count: which one is wrong isn't known, so nothing is guessed. The reply below reports it.
- A new side takes effect at the next start. Set bit 0 of `flags` to restart the half 0.1 s after the write.

**GET:** `DF 33 00` → `DF 33 00 <status> <format> <side> <source> <strap> <strap raw> <record side> <record source> <other flags> <other side> <pair> <link tx> <link how> <flags> <build side>`
- `status` `0`. `format` `1`.
- `side`: `0` left, `1` right, `FF` unknown. This is the side the half runs as now.
- `source`: `0` none, `1` factory (a host write), `2` Keybard (a host write), `3` migrated from a per-hand build, `4` thumb strap, `5` a one-side build (the left or right file; x4.2 on. Older firmware reads such a record as damaged).
- `strap`: the thumb strap read at startup: `0` no strap, `1` left, `2` anomaly, `3` not read on this build, `4` right. `strap raw`: bit 0 the pull-down read and bit 1 the pull-up read (thumb row on), bit 2 the pull-down read with the row off, bits 4-7 how many of 8 pull-up reads were high.
- `record side` and `record source`: the stored record as it is now (`FE`: no record, or no room for one). It differs from `side` after a write until the restart, and on a board whose strap overrode it.
- `other flags`: bit 0 the other half is connected, bit 1 it has reported its side. `other side` as `side`.
- `pair`: `0` fine (or no other half), `1` both halves claim the same side, `2` a connected half has no side, `3` the other half hasn't reported yet (up to 0.5 s after it connects).
- `link tx`: the GPIO this half transmits on over the split link (`0` or `1`; `FF` before the link starts). `link how`: `0` chosen by side, `1` found by listening for the other half, `2` the default for a half with no side, `3` swapped by the half with USB while the link was down (it tries the other pin pair every 2 s, so a half that transmits on its line still links).
- `flags`: bit 0 keys and pointing are dropped now, bit 1 the record differs from the running side (a restart is pending), bit 2 this half has room for the record (16 MB flash), bit 3 this half has USB, bit 4 this half runs a one-side build for the other side and its thumb strap won.
- `build side`: a one-side build's side (`0` left, `1` right), `FF` on the one-image builds. Firmware before x4.2 sends `0` here (the reply was zero-filled).

**SET:** `DF 33 01 <target> <side> <source> <flags>` → `DF 33 01 <status>`
- `target`: `0` this half (the one with USB), `1` the other half, over the split link.
- `side`: `0` left or `1` right. `source`: `1` factory or `2` Keybard.
- `status`: `0` OK, `1` invalid request, `2` no room for the record, `3` the write did not read back, `4` refused because the thumb strap says the other side, `5` the other half is not connected, `6` the other half did not answer (older firmware), `7` the other half took the request and saves it from its main loop: read its side again after it restarts, `8` refused because that half runs a one-side build for the other side (install the one-image firmware on it first).

**CLEAR:** `DF 33 02 <target> <flags>` → `DF 33 02 <status>`. Stores "unknown", so the half starts with no side (unless a strap says otherwise: then `4`; or the half runs a one-side build: then `8`). For testing and factory rework.

Packet formats: [client-ID protocol](../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [layer state](../../../modules/svalboard/core/docs/LAYER_STATE_PROTOCOL.md), [command definitions](../../../modules/svalboard/core/sval.h).

## Combo names

Combos can have names on the board, like layers, tap dances and macros: one name of up to 16 UTF-8 bytes per combo slot, read and written with the label commands (`LABEL_GET` `0x1B`, `LABEL_SET` `0x1C`, `LABEL_CLEAR` `0x1D`) with label type `3`.

- Discovery: GET_INFO byte 19, bit 7 (`sval_flag2_combo_labels`, `0x80`). Older firmware leaves it zero and answers label type `3` as an invalid type (`LABEL_SET` status `1`).
- Requests and replies are those of the other label types: `DF 1C 03 <index lo> <index hi> <16 bytes, null-padded>` → `DF 1C <status>` (`0` saved, `1` invalid type or UTF-8, `2` index out of range); `DF 1D 03 <index lo> <index hi>` clears one; `DF 1B 03 <start lo> <start hi>` returns the next named combo at or after `start`.
- The names are stored after the macro buffer, at the very end of the stored settings, with their own header. A board that updates to this firmware keeps everything it had and starts with no combo names; the macro buffer is 4104 bytes shorter (256 combos × 16 bytes, plus 8). A settings reset clears the names with everything else.

## Typing pause

`TYPING_PAUSE` (`0x3C`) stops the board's typing from reaching the computer while a host app's key tester is open. The keymap keeps running and layers work; only what would be typed is held back. The tester reads the switches with VIA's matrix state as before.

- Discovery: GET_INFO byte 20 (the third feature byte), bit 2 (`sval_flag3_typing_pause`, `0x04`). Older firmware leaves it zero, and answers `0x3C` with the command byte `0xFF` (unknown command). (Byte 20 bit 0 is `MAIN_BOARD`, and bit 1 is left for the board ID record branch.)
- Request: `DF 3C <op>`: `0` off, `1` on, `2` read the state. "On" also renews the pause, and the client that sends it owns the pause from then on. Any client may turn it off.
- While on, the half with USB sends no keyboard or NKRO reports (letters, Tab, Enter, Esc, arrows, F-keys, GUI, Ctrl, Alt, Shift) and no consumer or system reports (media, volume, sleep). The mouse report still goes out: buttons, motion and scroll. Both halves are covered, since the half with USB sends every report.
- Turning it on clears the modifiers, one-shot modifiers and keys, and sends an empty keyboard report first, so nothing is left held on the computer.
- Lease: only the owner's packets keep the pause on: any wrapped packet with its client ID, such as the tester's matrix reads, or another "on". When the owner sends nothing for 1.5 s, the pause ends by itself. Another app's traffic, or another client of the same app, never extends it, so a closed or crashed tester can't leave the board mute. A host whose client ID is renewed sends "on" again with the new ID. The pause also ends when USB leaves the active state (suspend, reset, unplug).
- Ending, by command, lease or USB: keys held at that moment are released inside the pause, and their real release is ignored later, so they never type. Output stays held back for 0.5 s more (state `2`) so a pending tap, tap dance or combo comes to nothing; presses in that time are ignored until released. Then an empty keyboard report goes to the computer and typing works again. An "on" during those 0.5 s keeps the pause.
- Response: `DF 3C` then 16 bytes (offsets from the first of them):

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | op, echoed |
| 1 | 1 | status: `0` done, `1` unknown op, `2` refused (not the half with USB, USB not active, or no valid client ID), `3` report too short |
| 2 | 1 | state: `0` off, `1` paused, `2` ending (output still held back) |
| 3 | 1 | how the last pause ended: `0` none yet, `1` command, `2` the owner went quiet, `3` USB |
| 4 | 1 | bit 0: the pause is on and the sender owns it |
| 5 | 2 | ms left on the lease, while paused |
| 7 | 2 | keyboard and NKRO reports held back since the pause began (saturates) |
| 9 | 2 | consumer and system reports held back |
| 11 | 2 | mouse reports sent through |
| 13 | 1 | keys released when the last pause ended |
| 14 | 1 | empty keyboard reports sent when turning on or ending, since boot (wraps) |
| 15 | 1 | keys whose release is still being ignored |

## Flicker latch

`FLICKER_LATCH` (`0x3D`) lets a key tester see every change of every switch, even a bounce that lasts one scan. Debounce hides those from the keymap and from VIA's matrix state, which reads the debounced matrix. Each half watches its raw rows on every scan, before debounce: a switch that went down since the last read sets its pressed-since bit, one that went up sets its released-since bit, and every change adds one to that switch's count (8 bits, stops at 255). Reading clears what it returns. Debounce and what gets typed are unchanged.

- Discovery: GET_INFO byte 20, bit 3 (`sval_flag3_flicker_latch`, `0x08`). Older firmware leaves it zero, and answers `0x3D` with the command byte `0xFF` (unknown command).
- The half with USB answers for both halves; it asks the other half over the split link on each read. A block the other half didn't send (not connected, or the link paused for an update) is flagged as missing.
- Request `DF 3D 00`: the raw rows and the latches of both halves; clears the latches. Response, offsets from the first byte after `3D`:

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | op `0`, echoed |
| 1 | 1 | status: `0` done, `1` unknown op, `2` not the half with USB, `3` report too short |
| 2 | 1 | bit 0: the left half's rows (0-4) are valid; bit 1: the right half's (5-9) |
| 3 | 1 | rows (`10`) |
| 4 | 10 | raw rows now, one byte per row, bit = column |
| 14 | 10 | pressed since the last read |
| 24 | 10 | released since the last read |

- Request `DF 3D 01 <half>` (`0` left, `1` right): that half's change counts, one byte per switch (row by row, 6 columns each), then cleared. Response: `[op 1] [status] [bit 0: valid] [half] [30 counts]`.
- Both need the 64-byte report (34 bytes after `3D`); a client-wrapped 32-byte report is answered with status `3`.
- A host that reads both ops on a schedule keeps a running count per switch; a switch's presses since the last read are its changes, halved, rounded up when it was up at the last read and down when it was down.

## Debounce mode

The optical keys have a saved debounce mode; Keybard's Scan Lab switches it. The thumb Double Down, the one mechanical switch, is debounced in both modes: 5 ms per key, deferred (a change is reported once it has held for 5 ms).

- **On** (the default): QMK's `sym_defer_g` on the optical keys, as release firmware types. Once no optical key on a half has changed for 5 ms, that half's optical keys are reported, all at once. A board that has never changed the setting reads on.
- **Off**: no debounce. A raw change reaches the keymap on the scan that sees it.

The setting is a VIA custom value, not a Sval command:

- Discovery: Scan Lab (VIA custom-value channel `0x53`) `STATUS` reply byte 23: bit 0, the firmware has this setting; bit 1, that half debounces its optical keys now. Older firmware sends 23 bytes, so the byte reads zero.
- Read: VIA `id_custom_get_value` (`0x08`), channel `0`, value ID `48` (`id_optical_debounce`): `1` on, `0` off.
- Write: `id_custom_set_value` (`0x07`), channel `0`, value ID `48`, `[1]` or `[0]`; then `id_custom_save` (`0x09`), channel `0`, stores it with the other keyboard settings. It works at once, saved or not.
- Both halves use the same mode. The half with USB owns the setting and sends its mode in the split presence ping (every 0.5 s; its last byte), as it sends the scan timing; the other half uses it and doesn't store it. Until the first ping, and with an older half on USB, the other half uses its own saved mode. Each half's `STATUS` bit 1 shows the mode it uses, so a host can see the other half follow.
- Stored in the last spare bit of the keyboard settings' first byte (`optical_debounce_off`, 0 = on), so the settings keep their size and layout stamp: updating to this firmware keeps them.

## Touchpad taps

The touchpad chip (Azoteq IQS5xx) makes clicks of its own while the pad is a mouse: a tap clicks, a two-finger tap right clicks, and a finger held still for the hold time (press and hold) presses the left button until it lifts, for dragging. This setting turns each one on or off and sets the hold time. It applies wherever the pads are a mouse: macOS, a BIOS or boot menu, every OS on builds without the native touchpad, and Windows and Linux until they switch the pads to touchpad mode. In touchpad mode the OS makes its own taps from the contacts, with its own touchpad settings, and the chip's clicks don't reach it (`SVAL_POINTER_OS_TOUCHPAD` in `0x2C`).

The setting is a VIA custom value, not a Sval command:

- Discovery: GET_INFO byte 20, bit 4 (`sval_flag3_pad_gestures`, `0x10`). Every build with the Azoteq driver sets it (the auto builds whatever module is attached; `0x2C` says which hand has a pad). Older firmware leaves it zero, and doesn't answer the values below.
- Values, channel `0`, read with `id_custom_get_value` (`0x08`) and written with `id_custom_set_value` (`0x07`):

| Value | Name | Data | Default |
|---|---|---|---|
| `51` | `id_pad_tap` | `1` a tap clicks, `0` it doesn't | `1` |
| `52` | `id_pad_two_finger` | `1` a two-finger tap right clicks, `0` it doesn't | `1` |
| `53` | `id_pad_hold` | `1` press and hold presses the left button, `0` it doesn't | `1` |
| `54` | `id_pad_hold_ms` | hold time, ms, 16-bit little-endian | `300` |

- The hold time is one of 20, 50, 100, 150, 200, 250, 300, 350, 400, 500, 600, 700, 800, 1000, 1200 or 1500 ms; a write takes the nearest (a tie, the shorter). Read it back for the stored value. Firmware before this held for 20 ms.
- A write works at once, on both pads: the half with USB writes its chip within a report cycle, and the other half gets the setting in the next presence ping (every 0.5 s; its last byte) and writes its own. `id_custom_save` (`0x09`) stores it with the other keyboard settings. A settings reset sets the defaults again.
- Defaults: all three on, hold 300 ms (`AZOTEQ_IQS5XX_HOLD_TIME`, QMK's driver default; firmware before this used 20 ms, so a finger resting still clicked almost at once). The stored byte records only changes from the defaults: its 0 means "the build's defaults", and the hold time's default is its own value (index 0), never a stored 20. A board that has never changed the setting writes nothing to the chip beyond what the driver's start writes from the build's defines, and after an update it holds for 300 ms. A self-built firmware that changes those defines keeps them until the setting is changed.
- The chip loses the setting when it restarts, so the firmware writes it again after every start of the chip (with the glass tuning), as long as it differs from the defaults.
- Stored in two bytes of their own after the keyboard settings' magic (the setting and its complement), not in the keyboard settings, which have no spare byte left: the settings keep their size and layout stamp, so updating to this firmware keeps them. Two bytes that don't match read as the defaults.
- Split: with an older half on USB, the other half keeps its own saved setting (the defaults, unless it once ran on USB with another). An older half without USB ignores the byte, and its pad keeps its build's taps.

## Scan timing

The board scans its keys with two saved waits per row, in microseconds: before reading the row (`id_scan_prewait_us`, VIA value `13`) and after it (`id_scan_postwait_us`, `14`). They are the timing the board runs, always; there is no other timing setting. Both are 16-bit little-endian, on channel `0`; `id_custom_save` (`0x09`) stores them. A Scan Lab sweep overrides them while it runs, and the updater raises them while it waits for its chord.

- Defaults: 90 / 90 µs on revision A boards; 45 / 5 µs on revision B and the Mini.
- A write of `0` stores 90 (older hosts wrote 0 to mean "the turbo table"). Read back after a write: the reply is what the board runs.
- Presets: 90, 60, 45, 30, 25, 20 and 15 µs, the same for both waits (the old turbo table, `sval_turbo_us`). A host offers them by writing both waits.
- `id_turbo_scan` (VIA value `9`) is kept for older hosts. A write of `0`-`6` sets both waits to that preset; other values change nothing. A read returns the preset both waits match, or `0xFF` when they match none. Scan Lab `STATUS` byte 21 reads the same.
- The `SV_TURBO_SCAN` key steps both waits to the first preset faster than the slower wait now, and from 15 µs (or faster) back to 90, and saves them. On a board at 45 / 5 µs it goes to 30 / 30.
- Upgrading: firmware before this stored `0` in a wait to mean "use the turbo table at the saved turbo index". At the first boot, a wait of `0` is filled in from the table with that index, and saved, so every board keeps the timing it ran. A board on turbo 6 with both waits at 0 reads 15 / 15 afterwards; one on turbo 0, 90 / 90; explicit waits are kept as they are. The settings keep their size and layout stamp, so nothing else resets. The byte that held the turbo index now holds the auto mouse layer timeout (below).
- Split: the half with USB sends both waits in the presence ping (every 0.5 s), as before, with the matching preset (0 for none) in the turbo byte. The other half uses them and doesn't store them. Waits are never 0 now, so an older half without USB runs them too. A new half without USB under an older half with USB that still sends 0 fills a 0 wait in from the turbo byte, as before.

## Auto mouse layer timeout

How long the auto mouse layer stays on after the pointer stops, in milliseconds.

- `id_automouse_timeout_ms` (VIA value `49`), 16-bit little-endian: 100 to 2000 ms. A write is clamped to that range and rounded to 10 ms; read it back for the stored value. Default 500.
- `id_automouse_no_timeout` (VIA value `50`): `1`, the layer stays on until it is turned off another way (the old "Infinite"); `0`, it times out after value 49. Value 49 keeps its time while this is on.
- `id_automouse_timeout` (VIA value `5`), kept for older hosts: the old choice, `0`-`5` = 200, 300, 400, 500, 800 ms, no timeout. A write sets value 49 to that time (or turns value 50 on for `5`); a read returns the nearest old choice. The `SV_MH_CHANGE_TIMEOUTS` key steps through the old choices, as before.
- Storage: the time is kept in 10 ms units in the byte that held the turbo index, and the old choice byte keeps the nearest old choice, so firmware from before this on the same settings still finds a valid one. Values 100-2000 fit in that byte (10-200); a turbo index left there (0-6) is cleared at the first boot.
