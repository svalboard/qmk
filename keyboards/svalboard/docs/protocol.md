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

Builds with Velocity Scroll (`SV_VELOCITY_SCROLL`) add a 24-byte block at offsets 29-52, so a host can show the key held, the offset from the anchor and the speed it sets, and draw the dead zone and the curve without knowing the build's tunables. Bytes 24-28 are left for the per-reader extension, so the two never overlap.

- Discovery: byte 29 reads `0x56` (`V`). Firmware without the block leaves it zero (the reply is zero-filled), and so does a client-wrapped 32-byte report, which has no room for it.
- The block is in every reply from such a build, held or not; only the state fields are zero while the key is up.

| off | size | field |
|---|---|---|
| 29 | 1 | `0x56`: the block is present (format 1) |
| 30 | 1 | bit 0 Velocity Scroll is held, bit 1 a finger on a touchpad sets the offset (else the relative pointers: trackball, TrackPoint), bit 2 that touchpad is the right hand's, bit 3 the axis lock tunable is on, bits 4-5 the axis the lock picked: `1` vertical, `2` horizontal, `0` none yet |
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

`SIDE` (`0x33`) tells a host which half is which on the one-image builds (`svalboard/either`, `svalboard/trackball/pmw3389/either`), and lets it set that. Those builds run the same image on both halves. Each half takes its side at startup from, in order: a thumb strap (a left thumb with R7 from LED_DIN to ground, or a right thumb with R7 from LED_DIN to the thumb row), the side stored on the half, or nothing ("unknown"). The per-hand builds fix the side when they are built and don't have this command.

- Discovery: GET_INFO byte 19, bit 6 (`sval_flag2_side`).
- The stored side lives in its own flash record, apart from the settings. Settings resets, `EE_CLR`, storage format changes, UF2 copies and updates keep it; only a whole-chip erase clears it. Only this command or a thumb strap changes it.
- The first start of a one-image build on a half that ran a per-hand build adopts that build's side (source `3`, migrated). A half that has never had a per-hand build (a new board) starts unknown.
- A half with no side still enumerates and answers this protocol, but sends no keys and no pointing. Every half enumerates as the per-hand builds do (product ID `0x4044`, or the name the user gave the board).
- The split link comes up whatever the other half's side, so the half with USB learns it (POINTING_HARDWARE flags bit 5 means "no side"). While the other half has no side, or both halves claim the same side, neither half's keys or pointing count: which one is wrong isn't known, so nothing is guessed. The reply below reports it.
- A new side takes effect at the next start. Set bit 0 of `flags` to restart the half 0.1 s after the write.

**GET:** `DF 33 00` → `DF 33 00 <status> <format> <side> <source> <strap> <strap raw> <record side> <record source> <other flags> <other side> <pair> <link tx> <link how> <flags>`
- `status` `0`. `format` `1`.
- `side`: `0` left, `1` right, `FF` unknown. This is the side the half runs as now.
- `source`: `0` none, `1` factory (a host write), `2` Keybard (a host write), `3` migrated from a per-hand build, `4` thumb strap.
- `strap`: the thumb strap read at startup: `0` no strap, `1` left, `2` anomaly, `3` not read on this build, `4` right. `strap raw`: bit 0 the pull-down read and bit 1 the pull-up read (thumb row on), bit 2 the pull-down read with the row off, bits 4-7 how many of 8 pull-up reads were high.
- `record side` and `record source`: the stored record as it is now (`FE`: no record, or no room for one). It differs from `side` after a write until the restart, and on a board whose strap overrode it.
- `other flags`: bit 0 the other half is connected, bit 1 it has reported its side. `other side` as `side`.
- `pair`: `0` fine (or no other half), `1` both halves claim the same side, `2` a connected half has no side, `3` the other half hasn't reported yet (up to 0.5 s after it connects).
- `link tx`: the GPIO this half transmits on over the split link (`0` or `1`; `FF` before the link starts). `link how`: `0` chosen by side, `1` found by listening for the other half, `2` the default for a half with no side, `3` swapped by the half with USB while the link was down (it tries the other pin pair every 2 s, so a half that transmits on its line still links).
- `flags`: bit 0 keys and pointing are dropped now, bit 1 the record differs from the running side (a restart is pending), bit 2 this half has room for the record (16 MB flash), bit 3 this half has USB.

**SET:** `DF 33 01 <target> <side> <source> <flags>` → `DF 33 01 <status>`
- `target`: `0` this half (the one with USB), `1` the other half, over the split link.
- `side`: `0` left or `1` right. `source`: `1` factory or `2` Keybard.
- `status`: `0` OK, `1` invalid request, `2` no room for the record, `3` the write did not read back, `4` refused because the thumb strap says the other side, `5` the other half is not connected, `6` the other half did not answer (older firmware), `7` the other half took the request and saves it from its main loop: read its side again after it restarts.

**CLEAR:** `DF 33 02 <target> <flags>` → `DF 33 02 <status>`. Stores "unknown", so the half starts with no side (unless a strap says otherwise: then `4`). For testing and factory rework.

Packet formats: [client-ID protocol](../../../modules/svalboard/core/docs/CLIENT_ID_PROTOCOL.md), [layer state](../../../modules/svalboard/core/docs/LAYER_STATE_PROTOCOL.md), [command definitions](../../../modules/svalboard/core/sval.h).
