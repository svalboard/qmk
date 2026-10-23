# Svalboard in-firmware updater

The updater lets a running Svalboard half replace its own firmware over USB,
without BOOTSEL or a UF2 copy. It is compiled in only with `SVAL_UPDATER=yes`
and is off in every default build. This page describes what is built today:
milestone M1 (one half, over its own USB), M2 (the half without USB, relayed
through the half with USB; see "The other half" below) and the build-info
record and version number from M3. The design and its decisions are in
[updater-plan.md](updater-plan.md); code is in `keyboards/svalboard/updater/`
(below, `kb/` is `keyboards/svalboard/`).

In short: the host sends a manifest and the user approves it on the keyboard.
The half then stages the raw image in spare flash, checks its hash and
structure, and on COMMIT copies it over the running firmware from RAM. Before
the first erase it makes the old image unbootable, and it writes the new
image's first page last. A power cut at any point leaves either the new image
or a board that comes up in BOOTSEL. It never leaves a half-written image that
the boot ROM would run.

This version (updater protocol 4) does not sign updates. See "Security"
below. Signing was built and tested in M3 and then deferred; that code is kept
under the tag `archive/fw-updater-signed`.

## Security

The updater checks that an image arrived intact (SHA-512 against the manifest
the user approved) and that it is structurally a firmware image it can boot
(manifest format and version, length, boot2, initial SP, reset vector, the
security epoch and storage floors). It does not check the image's side or
pointing device (D33): moving an MCU between hands or modules is a valid use,
so the host tools warn about a mismatch and ask instead. It does not check who
built the image. Anyone who can get a
user to make the chord can install any image, exactly as anyone who can get
them to drag a UF2 onto the board can.

## Installing your own firmware

- **BOOTSEL and a UF2 work for any build**, as they always have: hold BOOTSEL
  while plugging the half in (or tap reset twice within 500 ms) and copy the
  `.uf2` onto `RPI-RP2`. A release file is a pair UF2: copied onto one half,
  it installs that half and then the other one over the split cable
  ([pair-uf2.md](pair-uf2.md)).
- **The updater accepts your own builds too.** There is no key. Build with
  `SVAL_UPDATER=yes`, then make a `.svup` with `make_update.py` and send it
  with `sval_update.py` (or Keybard, once M4 ships). Both halves must run
  updater builds of the same release (see "One release on both halves").

```
make svalboard/right:sval SVAL_UPDATER=yes
N=svalboard_right_sval
K=svalboard/right
MU=keyboards/svalboard/tools/make_update.py
python3 -I $MU $N.uf2 -o $N.svup --kb $K
```

## Builds

| Flag | Effect |
|---|---|
| `SVAL_UPDATER=yes` | The updater: VIA channel `0x55`, staging slot, commit routine, LED states |
| `SVAL_UPDATE_TEST_HOOKS=yes` | Adds the `TEST_HALT` op and the commit halt points (hardware tests only) |
| `SVAL_UPDATE_RELEASE=yes` | Release updater build: DIAGNOSTIC images refused. Cannot be combined with the test hooks, `SVAL_KEYTEST` or `SVAL_HOST_BOOTLOADER`. Release CI builds its four images this way (see "Releases") |
| `SVAL_TEST_WIPE=yes` | Bench builds only, never released: a settings magic (`0xB1A0`) no release uses. Every build now resets settings on its first boot anyway (D39); the wipe magic also resets when the same image is flashed over a normal one |
| `SVAL_UPDATE_NO_CHORD=yes` | Dev builds only (D43): every ARM counts as confirmed, so installs need no chord (INFO flags bit 7; ARM flags PRE_APPROVED and NO_CHORD; no blue light). A release build refuses it (`rules.mk`, `#error`, release CI) |
| `SVAL_DEV_KEEP_SETTINGS=yes` | Dev builds only (D43): the first boot of a different build keeps stored settings (no D39 reset), except after a SETTINGS_RESET. A release build refuses it |
| `SVAL_NATIVE_PTP=no` | Builds the auto images without the native touchpad (on by default there; `keyboards/svalboard/docs/native-touchpad.md`) |
| `SVAL_TOUCH_DEV=yes`, `SVAL_PERF_STATS=yes` | Dev builds only: the touchpad dev command and the timing counters (raw HID 0xFC). A release build refuses both, and `check_release_elf.py` rejects their symbols |
| `SVAL_FW_VERSION=N` | The numeric version (D17), reported by INFO and presence. Default: `kb/updater/fw_version.txt` (D32). The older `EXTRAFLAGS=-DSVAL_FW_VERSION=N` still works |
| `SVAL_FW_VERSION_STRING=S` | The version string, at most 16 of `A-Z a-z 0-9 . _ + -` (INFO page 1). Default empty; release CI passes the tag |

Every updater build carries a 32-byte build-info record,
`sval_update_build_info` (`kb/updater/update_build_info.h`, version 3): magic
`SVBI`, which of the flags above it was built with (bit0 release, bit2 test
hooks, bit3 keytest, bit4 host bootloader, bit6 no-chord, bit7 keep settings (the
last two dev builds only, D43); bits 1 and 5 reserved, always 0), a
reserved byte, the updater protocol, `SVAL_FW_VERSION`,
`SVAL_FW_VERSION_STRING`, and the build's side, pointing device and keymap
(`sval` 1, `blank` 2, any other 0). `make_update.py` takes the manifest's
version from it, and warns about a `--kb` whose side or pointing device
contradicts it, asking before it writes the `.svup` (D33); with `--release` it
also refuses a record that is not a clean release build or whose version or
keymap does not match the manifest. `update_build_info.c` refuses to
compile a release build with test hooks, keytest, the host bootloader, no-chord
or keep settings.

`SVAL_UPDATER` cannot be combined with `SVAL_KEYTEST` (keytest injects key
events, so a host could fake the confirmation chord). Main's stack is
`0xAE0` bytes in updater builds (`USE_PROCESS_STACKSIZE`, `kb/rules.mk`): the
size the M1 and M2b hardware tests ran with. The hardware tests use plain
updater builds, or test-hooks builds for the halt points:

```
make svalboard/trackball/pmw3389/right:sval SVAL_UPDATER=yes
E=.build/svalboard_trackball_pmw3389_right_sval.elf
python3 -I keyboards/svalboard/tools/check_ram_funcs.py $E
```

## Flash map

The RP2040 boots from a 16 MiB W25Q128-class die. Offsets are flash offsets (the
XIP address is `0x10000000` plus the offset).

| Flash offset | Contents |
|---|---|
| `0x000000`-`0x0000FF` | Page 0: boot2. The boot ROM checks a CRC-32/MPEG-2 over bytes 0-251 against the word at 252; if it fails, it boots into USB (BOOTSEL) |
| `0x000100`-`0x15FFFF` | The rest of the firmware (vector table at `0x100`). `kb/flash_reservation.ld` keeps every image below `0x160000` (`SVAL_UPDATE_MAX_IMAGE`) |
| `0x160000`-`0x25FFFF` | Settings: QMK wear leveling, two copies of `0x80000` |
| `0x260000`-`0x26FFFF` | Free |
| `0x270000`-`0x270FFF` | Pair record: written by a pair UF2 copied in BOOTSEL, erased by the boot relay ([pair-uf2.md](pair-uf2.md)) |
| `0x271000`-`0x7FFFFF` | Free |
| `0x800000`-`0x95FFFF` | Staging slot (`SVAL_UPDATE_BASE`, `SVAL_UPDATE_SIZE`) |
| `0x960000`-`0xFFAFFF` | Free |
| `0xFFB000`-`0xFFCFFF` | Side records (one-image builds, `kb/side.h`) |
| `0xFFD000`-`0xFFDFFF` | Build reset record (D39): the hash of the image the settings were last reset for. Nothing else may use it |
| `0xFFE000`-`0xFFFFFF` | Board identity, two 4 KiB sectors |

Staging erases and programs only inside the slot. The commit erases and programs only inside
`[0, round_up(image_len, 64 KiB))`, which is at most `[0, 0x160000)`. Both limits
are checked again in the RAM routine just before each boot ROM call. Static asserts
in `update_flash.c` and `update_commit.c` pin the layout. Nothing writes the
settings or identity regions. On a die other than 16 MiB, the updater reports
UNAVAILABLE and touches nothing.

## The .svup file and the manifest

A `.svup` is the 108-byte manifest, then the raw image. Nothing comes between
them and nothing follows. The manifest (`kb/updater/update_manifest.h`):

| Offset | Field | Rule |
|---|---|---|
| 0 | magic u32 | `SVUP` |
| 4 | manifest version u16 | 2. Version 1 (protocol 1, signed, never released) is refused with UNSUPPORTED |
| 6 | reserved u8 | 0, else INVALID (it was `key_id` in version 1) |
| 7 | hand | 0 left, 1 right. Reported, not checked (D33): the request's hand picks the half |
| 8 | pointing ID | 0 none, 1 TrackPoint, 2 PMW3360, 3 PMW3389, 4 Azoteq, 5 auto (every driver, `pointing_auto`). Reported, not checked (D33) |
| 9 | keymap ID | 1 sval, 2 blank; display only |
| 10 | flags u16 | bit0 RELEASE, bit1 DIAGNOSTIC; others 0. A release build refuses DIAGNOSTIC |
| 12 | security epoch u16 | at or above the device's floor (D19) |
| 14 | storage format | at or above the device's floor (D19) |
| 15 | updater protocol | of the image |
| 16 | image length u32 | `0x200`..`SVAL_UPDATE_MAX_IMAGE`, a multiple of 256 |
| 20 | fw version u32 | also sent in presence (D17) |
| 24 | reserved u32 | 0 |
| 28 | version string | 16 characters, NUL-padded |
| 44 | SHA-512 | of the image |

## Protocol

The updater is a keyboard-local VIA custom-value channel (`0x55`, `'U'`) inside
the Sval client wrapper:

```
[0xDD][client_id:4][0xFE] [id_custom_set_value 0x07 | get 0x08][0x55][op][value_data:55]
```

Each packet is one 64-byte raw HID report (`RAW_EPSIZE` in `kb/config.h`), so
`value_data` is 64 - 6 - 3 = 55 bytes. Protocol 3 is protocol 2 (32-byte
reports, 23 value bytes) in 64-byte reports: the same ops and fields, larger
MANIFEST and CHUNK pieces, and the piece sizes at the end of INFO. A host sizes
its pieces from INFO and refuses any other protocol; a protocol 2 host cannot
talk to a protocol 3 board (its 32-byte reports don't fit the descriptor), and
`sval_update.py` refuses a protocol 2 board. The relay over the split cable is
unchanged, so either protocol's half can be updated through the other.
Protocol 4 (D41-D43) is protocol 3 plus one chord for both halves
(PAIR_MANIFEST, the ARM mode and flags, the held approval, STATUS byte 23), the
settings reset (SETTINGS_RESET) and INFO flags bit 7; the sizes are unchanged.
Its byte-level contract with Keybard is [updater-protocol4.md](updater-protocol4.md).
Keybard offers its own install flow only to a half with USB on protocol 4.
`sval_update.py` speaks protocols 3 and 4.

The reply is the same packet with `value_data` rewritten. `value_data[0]` is
always a status. Every request starts with `[hand]`: this half (0 left, 1 right),
the other half (M2, below), or `0xFF` for INFO only. Any other hand gets
UNSUPPORTED. Every op except INFO needs the client wrapper; an unwrapped packet
gets INVALID and changes nothing. Multi-byte fields are little-endian. The
byte layouts are in the comment at the top of `kb/updater/updater.c`. In short:

| Op | Request after `[hand]` | Reply after `[status]` / rule |
|---|---|---|
| `0x00 INFO` | page (0, the default) | state, protocol (4), slot base/size and max image (4 KiB units), JEDEC ID, pointing ID, hand, fw version, storage format, security epoch, flags (bit0 16 MiB die, bit1 settings writes failing, bit2 release build, bit3 test hooks, bit4 reserved, bit5 the other half's version differs, bit6 reserved, bit7 a no-chord dev build, D43), last error, then the largest CHUNK data (50), the largest MANIFEST piece (52) and the report size (64), for either half. Page 1 (this half only): `1`, fw version u32, the 16-character version string from the build-info record. Other pages: INVALID |
| `0x01 MANIFEST` | off, n ≤ 52, bytes | bytes filled, of 108. In order; offset 0 restarts. Bytes past 108 are INVALID. Ties the load to this client |
| `0x02 ARM` | mode (protocol 4: 0 single, the default; 1 pair) | nonce u32 and the first 4 bytes of SHA-512(manifest), which the host shows; then (protocol 4) the first 4 bytes of SHA-512(the pair manifest) for a pair ARM (else 0), and flags: bit0 PRE_APPROVED (BEGIN needs no chord, no blue light), bit1 BY_PAIR, bit2 NO_CHORD, bit3 PAIR. Needs all 108 bytes. Checks the manifest fields, binds the session to client ID + nonce, starts the 30 s chord window. BUSY, with nothing changed, while a Scan Lab sweep runs. A pair ARM also needs the whole pair manifest and checks it against this half (see "One chord for both halves") |
| `0x03 BEGIN` | nonce | ACCEPTED once the chord is made: the manifest is checked again, then the erase, one 4 KiB sector per main-loop pass |
| `0x04 CHUNK` | off u24, n ≤ 50, bytes | next offset. Strictly in order; an earlier chunk is acknowledged and not written again |
| `0x05 END` | nonce | ACCEPTED: SHA-512, structure checks and CRC of the staged image, 2 KiB per pass |
| `0x06 STATUS` | - | state, bytes staged, image length, erase progress, last error, reserved u16 (0), verify ms, longest sector erase ms, CRC low 16 bits once VERIFIED, flags, the other half by split presence (see below), then (protocol 4) flags2: bit0 an approval is held for this half's next install, bit1 this session was pre-approved |
| `0x07 COMMIT` | nonce, CRC low 16 | ACCEPTED; the commit runs from housekeeping 100 ms later. The CRC (CRC-32/MPEG-2 over image bytes `[0x100, len)`) must match the verified image |
| `0x08 ABORT` | nonce | From the session, or from anyone while ERROR is latched. Never erases |
| `0x09 REBIND` | nonce | Moves the session to the sender's client ID (Keybard renews its ID every 50 s); the old ID is refused after it. Protocol 4: with the pair session's nonce it also moves a held approval, in any state, IDLE included; it is also answered while the other half commits |
| `0x0A TEST_HALT` | nonce, point, fed | Test-hooks builds only, in VERIFIED: where the commit stops (see Test hooks). INVALID elsewhere |
| `0x0B DIAG` | clear | Main's stack: bytes never used since boot and its size; the longest `updater_task()` pass in ms and the state it began in; the last reset reason and updater breadcrumbs. `clear` 1 restarts the pass record. For M1 #13 |
| `0x0C RELAY` | - | M2: the relay to the other half: phase, that half's state and last error, bytes it acknowledged, image length, its erase progress, retries, relay ms, link paused, the relay's error, this half's state |
| `0x0D PAIR_MANIFEST` | off, n ≤ 52, bytes; `[hand]` is this half's own | Protocol 4 (D41): bytes filled of this half's 108-byte manifest, loaded beside a session for the other hand while it is in MANIFEST_LOADING, from that session's client. INVALID with no such session, OTHER_CLIENT, BUSY after its ARM; refusals change nothing |
| `0x0E SETTINGS_RESET` | `'R' 'S' 'E' 'T'`, scope (0 this half, 1 both), mode (0 defaults, 1 a blank board: D44) | Protocol 4 (D42): done mask (bit0 this half, bit1 the other half). Only in IDLE with no held approval and no relay, else BUSY. See "Settings reset" |

States: `IDLE 0, MANIFEST_LOADING 1, CONFIRM_WAIT 2, VERIFYING_MANIFEST 3,
ERASING 4, RECEIVING 5, VERIFYING_IMAGE 6, VERIFIED 7, COMMITTING 8, ERROR 9`,
and for a session for the other half `RELAYING 10, RELAYED 11,
SUBSIDE_COMMITTING 12`.
Status codes: `OK 0, INVALID 1, UNAVAILABLE 2, FLASH_ERR 3, ACCEPTED 4, BUSY 5,
NOT_CONFIRMED 6, WRONG_HW 7, BAD_HASH 9, BAD_IMAGE 10, EPOCH 11,
OUT_OF_ORDER 12, OVERRUN 13, TOO_LARGE 14, TIMEOUT 15, OTHER_CLIENT 16,
UNSUPPORTED 17, STORAGE 18` (`kb/updater/update_manifest.h`). Code 8 was
protocol 1's signature failure and code 7 (WRONG_HW) the pointing check
before D33: both are retired and never returned, and the
codes after it keep their numbers. A refused request (another client, a wrong
nonce, a request out of order) changes nothing. A failed update (a check
fails, a flash error, OVERRUN, a timeout, a refused commit) latches ERROR until
ABORT. A session with no op for 30 s times out.

**What the session binding protects against.** The client ID and nonce keep
well-behaved clients (two Keybard tabs, a stray tool) from crossing a session.
They do not stop a hostile one (R10: the client ID is not a secret). The client
ID is in every wrapped reply and the nonce in ARM's, and raw HID replies reach
every open handle, so another process with the device open can REBIND the
session to itself and then stall it, ABORT it, or choose when to COMMIT it. It
cannot change what is installed: the user confirmed the manifest hash with the
chord, and the manifest's SHA-512 binds the image to that manifest. The M1 #6
rows "a second client ID", "ABORT from another client" and "REBIND with the
wrong nonce" therefore test the binding against accidental cross-talk only.

**Confirmation.** After ARM, hold Index South + Middle South for 1 s on the half
with USB. On the right half that is matrix `[6,0]+[7,0]` (`M` and `,` on
the default keymap); on the left it is `[1,0]+[2,0]` (`V` and `C`). The chord is
read from the scanned matrix, never from keycodes, and both keys are hidden from
QMK while the updater waits. The host can set the matrix scan timing (the saved
pre/post waits, and Scan Lab sweeps), and a pre-wait that is too short can
misread keys. So while the chord is awaited every row is scanned with at least
Scan Lab's safe timing (500 µs before and after, `SVAL_UPDATE_CHORD_PREWAIT_US`
/ `POSTWAIT_US`), ARM is refused while a sweep runs, and Scan Lab refuses to
start a sweep or a probe while the updater is active.

**LEDs (D24).** One meaning per color. Red means only that something failed;
nothing you should simply wait through is ever red.

| Light | Meaning | When |
|---|---|---|
| Blue blink | Your turn | Make the chord |
| White double flash | Got it | The chord was recognized |
| Rainbow | Updating, keep plugged in | On both halves, from the start of an update until the halves match, including while one half waits for the other to restart after a relay |
| Magenta | Writing | Only on the half that is committing, where the light must hold one color while flash is written from RAM |
| Green double flash | Done | On both halves, once presence matches after an update or a relay |
| Red blink (1 Hz) | It failed: open Keybard | A refused or failed install, or halves that still differ 60 s after an update or relay finished. It stops after 30 s; INFO flags bit5 and Keybard keep reporting the problem |
| No light | Nothing for you to do | Also for a pair record kept waiting for an unplugged or absent other half |

After the chord the light goes straight to the rainbow (there is no separate
"approved" color). A pre-approved ARM (protocol 4: this half's install
approved by the pair's chord, or any ARM on a no-chord dev build) shows no
blue and no white flash. A failed session stays in ERROR until ABORT, but its
red stops after 30 s (`SVAL_UPDATE_RED_MS`).

How each half knows. The half with USB opens an "after update" window when a
relay ends done and on the first boot after its own commit (a marker in
watchdog scratch 0, set just before the commit runs; it survives the commit's
reset, not a power cut). It shows the rainbow until presence reads MATCH, then
the green flash. If it has been idle for `SVAL_UPDATE_AFTER_WAIT_MS` (60 s)
with the halves still different, it shows red for 30 s and then nothing. A
half on its own (presence never answered after its own install) just goes
dark. The half without USB cannot see presence, so the half with USB sends its
light on the 500 ms presence ping (one byte appended to `presence_rpc_t`;
`updater_split_light`), and the half without USB shows the same rainbow, the
green flash once, and red. After its own commit it shows the rainbow from boot
until the first ping. A half with USB from before this byte sends the shorter
ping, and the other half then shows nothing after an update. No host protocol
change, and the split message table is unchanged (the ping is a keyboard RPC
of variable length).

In a pair update from Keybard this means: blue, white, then the rainbow on
both halves through the relay, the other half's restart and this half's own
install (magenta on each half only while it writes), then green on both.

**Manifest checks** (at ARM, again at BEGIN, and at commit step 0). Magic
`SVUP`, version 2, reserved fields 0, known flags; a release build refuses
DIAGNOSTIC; the security epoch
and storage format are at or above this device's floors; `image_len` is between
`0x200` and `SVAL_UPDATE_MAX_IMAGE` and a multiple of 256. The hand and
pointing device are not checked (D33). On the staged image:
SHA-512 equals the manifest's, the boot2 CRC of page 0 is valid, the initial SP
is in `[0x20000000, 0x20042000]`, and the reset vector is odd and inside the
image. `kb/tools/make_update.py` refuses to build an image that would fail any
of these.

## The commit and the reset mechanism

`update_commit.c`. Step 0 runs from flash with interrupts on. Steps 1-7 are the
`commit_ram_*` functions, which run from RAM (`.time_critical`) with PRIMASK set.

0. **Re-check.** Refuse, with nothing changed, if settings writes are failing
   (`wear_leveling_write_failed()`), the die is not 16 MiB, the manifest fields
   no longer pass, the slot's SHA-512 or body CRC differs from what was verified,
   the head checks fail, the running boot2 (page 0) fails its CRC, a boot ROM flash
   function is not a ROM Thumb address, or the watchdog tick is not running. Take
   the commit's own copy of the running boot2 from page 0; it is never refilled.
   Look up the ROM flash functions. Show magenta, latch it with two WS2812
   flushes, and wait for that transfer to finish.
1. **Arm.** Set PRIMASK. If any of the 12 DMA channels is busy, clear PRIMASK
   and refuse with BUSY. Point VTOR at a 48-slot RAM vector table whose handlers
   are all the RAM reset routine, so any fault resets the board. Arm the watchdog,
   in this order: `PSM_WDSEL = 0x1FFFC` (everything but the oscillators; at its
   reset value 0, a watchdog reset would reset nothing), clear scratch4,
   `LOAD = 8 s × 2` (the counter drops 2 per µs), clear the pause-on-debug bits,
   set ENABLE.
2. **Invalidate.** Program page 0 to all zeros and read it back through the
   uncached alias. A zero page 0 fails the boot ROM's CRC, so from here on a cut
   ends in BOOTSEL. If the read-back is not all zero, reset at once: most of the
   old image is still there.
3. **Erase.** Sector 0, then the other 15 sectors of block 0 one at a time, then
   whole 64 KiB blocks up to `round_up(image_len, 64 KiB)`.
4. **Copy.** For each 4 KiB sector: slot → the commit's dedicated 4 KiB RAM
   buffer with XIP on, exit XIP, program (sector 0 from page 1), flush the cache,
   and re-enter XIP through the boot2 copy.
5. **Verify.** CRC-32/MPEG-2 over `[0x100, image_len)` read back uncached,
   against the CRC from VERIFYING_IMAGE. On a mismatch, every sector that
   differs from the slot is erased and programmed again, once. If the CRC still
   fails, page 0 stays erased and the board resets into BOOTSEL.
6. **Page 0.** Read the image's page 0 from the slot, check it against the CRC
   taken in step 0, program it, and read it back. If the read-back differs, page
   0 is programmed to zeros, so the boot ROM goes to BOOTSEL and never runs a
   damaged boot2.
7. **Reset.** Zero the sector buffer. Zero every word of SRAM0-3 that equals the
   double-tap magic `0xCAFEB0BA`, so the new image's noinit magic word cannot
   land on stale bytes and send it to BOOTSEL. Make sure PSM_WDSEL is set, clear
   scratch4 (so the boot ROM boots from flash and not to an address), and write
   the watchdog trigger.

The watchdog is fed after every boot ROM call. The worst single operation is a
64 KiB block erase, 2 s at the datasheet maximum, so the 8 s period leaves room.
Nothing in steps 1-7 calls flash. `kb/tools/check_ram_funcs.py` checks the linked
ELF: every branch in the RAM functions stays inside the RAM set or goes to the boot
ROM, every `blx rN` takes its target from the ROM pointer table or the boot2 copy,
there are no jump tables, no literals in flash, no double-tap magic, and the stack
use is static and at most 512 B. Run it on every `SVAL_UPDATER` build.

## Recovery

| Situation | What happens | Recovery |
|---|---|---|
| Cut or unplug before COMMIT (any staging state) | The firmware area was never touched; the old image boots. The slot is ignored and erased by the next BEGIN | None |
| Cut during the commit, before page 0 is written | Page 0 is zero or erased: the boot ROM boots into BOOTSEL and the `RPI-RP2` drive appears | Copy a UF2: `keyboards/svalboard/tools/flash.sh <image.uf2>`, or drag it onto `RPI-RP2` |
| Cut after page 0 is written | The new image boots | None |
| Fault or hang inside the commit | The RAM vector table or the watchdog (8 s) resets the board, into BOOTSEL until page 0 is written | As above |
| Any time | Hold BOOTSEL while plugging in, or tap the reset button twice within 500 ms | As above |

Settings (`0x160000`-`0x25FFFF`) and identity (`0xFFE000`-`0xFFFFFF`) are outside
everything the commit writes, so they survive all of these. The hardware tests
compare picotool dumps of both regions before and after.

## Test hooks

On a `SVAL_UPDATE_TEST_HOOKS=yes` build, `sval_update.py update FILE.svup --halt
POINT [--halt-unfed]` sends TEST_HALT after VERIFIED, then COMMIT. The commit
stops for good at:

| Point | Where | Page 0 afterwards (`picotool save -r 0x10000000 0x10000100`) | Expected (M1 #8 / #9) |
|---|---|---|---|
| `invalidated` (1) | N = -1: page 0 zeroed and read back, nothing erased | all `0x00` | BOOTSEL |
| `first-erase` (2) | N = 0: sector 0 erased | all `0xFF` | BOOTSEL |
| `mid-program` (3) | after programming the middle sector | all `0xFF` | BOOTSEL |
| `last-sector` (4) | every sector programmed, page 0 still erased | all `0xFF` | BOOTSEL |
| `page0` (5) | page 0 written and read back, before the reset | the new image's boot2 | the new image boots |
| `fault` (6) | a HardFault (`udf`) straight after the invalidation (M1 #10) | all `0x00` | BOOTSEL in well under 1 s |
| `fault-erased` (7) | a HardFault after sector 0 is erased, so the flash vector table is gone (M1 #10, R3) | all `0xFF` | BOOTSEL in well under 1 s |

In every case up to `last-sector` the boot2 CRC of page 0 is invalid and the
board comes up as `RPI-RP2`. Page 0 is all zeros only at N = -1: step 3 erases
sector 0 straight after the invalidation, so from N = 0 on it reads `0xFF`.

Before it stops, a halt clears RAM as step 7 does (the sector buffer and every
double-tap magic word), so an unfed halt's watchdog reset starts the next image
from the same RAM state as a real commit. It then spins with interrupts off and
feeds the watchdog, so it hangs until it is unplugged (M1 #8). With
`--halt-unfed` it stops feeding, and the watchdog resets it within 8 s (M1 #9,
#15). Steps 1-7's own reset path (the watchdog trigger in step 7) is exercised
only by a full commit (M1 #4, #14).

**Timing is the M1 #10 result.** The fault points reset through the RAM vector
table at once. If the RAM table were not in use, the fault would land in the old
firmware's handler (an endless loop, at `fault`) or lock up on the erased table
(at `fault-erased`), and only the watchdog would reset the board, about 8 s
later, into the same BOOTSEL. So a reset into `RPI-RP2` well under 1 s after the
LEDs turn magenta is a pass, and one after about 8 s is a failure.

## Hardware test notes (M1)

- **#6, protocol rows:** `sval_update.py reject GOOD.svup` sends each malformed
  or misdirected request (unwrapped packets, a second client ID, wrong nonces,
  BEGIN before the chord, an out-of-order CHUNK, END too early, ABORT and REBIND
  from the wrong client, COMMIT with the wrong CRC, an overrun) and checks the
  status and the state after each. It needs the chord twice and never commits.
  The 30 s stall is `update GOOD.svup --delay 31` (TIMEOUT).
- **#6, image rows:** `.svup` files from `make_update.py --unsafe-allow-bad-image`,
  sent with `update`. "Each gives its own status" means its expected status:
  a bad SHA-512 latches BAD_HASH at VERIFYING_IMAGE, and a COMMIT with the
  wrong CRC is refused with BAD_HASH while the session stays VERIFIED; the
  security epoch gives EPOCH and the storage format STORAGE.
- **#6, epoch below:** the device floor is 0 by default, so build the test board
  with `EXTRAFLAGS=-DSVAL_UPDATE_SECURITY_EPOCH=1` and send an image made with
  `--epoch 0`.
- **#6, release builds:** a release build (`SVAL_UPDATE_RELEASE=yes`) refuses
  an image made with `--diagnostic` (BAD_IMAGE at ARM). The host tests check
  the release rules themselves (`test_manifest_check`, `test_session.c` and
  `test_release.c`).
- **#13:** `sval_update.py diag` after a full update gives main's stack never
  used since boot (crt0 fills it with `0x55555555`; the count is a little
  optimistic if a used word holds the pattern) and the longest updater pass in
  ms with its state. The host tests' 50 ms check counts only simulated flash
  time; the CPU-bound passes (SHA-512 slices, erased checks, commit step 0)
  are measured here. `diag --clear` restarts the pass record.
- **#16:** `update GOOD.svup --rebind-every 5 --min-rebinds 3`. The default 50 s
  renewal may never fire during a fast transfer; `--min-rebinds` fails the run
  before COMMIT if fewer renewals happened, and the summary prints the count.

## The split link (M2a)

M2 updates the half without USB through the half with USB: stage 1 is the
link layer (below), stage 2 the relay (next section). All of it is in
`SVAL_UPDATER` builds only: a default build is unchanged.

**One release on both halves (V).** An `SVAL_UPDATER` build adds the
`KEYBOARD_UPDATE` split transaction, so its split table has one entry more
than a default build's. Both halves fold the transaction count into every
split handshake, so an `SVAL_UPDATER` half cannot talk to a default-build half
at all: flash both halves with `SVAL_UPDATER` builds of the same commit.
`kb/tools/check_split_tables.py` checks that every left/right pairing of a set
of builds has the same table (P1). The keyboard RPC entries of that table
are all zero in the ELF (they are filled at boot), so updater builds also
carry `sval_split_kb_ids` (`kb/updater/update_split.c`): each keyboard RPC's
ID with a tag, which the check compares too (the order of the IDs):

```
T=keyboards/svalboard/tools/check_split_tables.py
python3 -I $T --dir DIR_WITH_ELFS
python3 -I $T --dump BUILD.elf
```

**Presence with version (D17).** The half without USB answers the existing
500 ms presence ping (`KEYBOARD_SYNC_A`) with `presence_rsp_t`
(`kb/svalboard.h`): the 2-byte magic `SP` and the 5-byte pointer report
(Sval `0x2C`), then, in `SVAL_UPDATER` builds, 17 more bytes of
`update_presence_t`: magic `PV`, struct version 1, `fw_version` u32
(`SVAL_FW_VERSION`: `kb/updater/fw_version.txt`), the first 8 hex digits of
the git hash (0 for `SKIP_GIT` builds), updater protocol, hand, pointing ID,
flags (bit0 dirty tree, bit1 updater active), a reserved 0 and a CRC-8
(`kb/updater/update_split_wire.h`). That is 24 bytes in all; a static assert
in `kb/svalboard.c` keeps it inside the 32-byte RPC buffer. The pointer
report's layout is unchanged. The half with USB compares the updater block
with its own build and reports the result in STATUS byte 22
(`value_data[22]`):

| Code | Other half |
|---|---|
| 0 | no answer (or this is the half without USB) |
| 1 | same release: same `fw_version`, git hash and updater protocol, other hand |
| 2 | version differs |
| 3 | built for the same hand |
| 4 | the answer fails magic, version or CRC |

For 2-4, INFO flags bit5 is set until the halves match (V). The lights show it
only after an update: the rainbow while the after-update window is open, and
after 60 s idle red for 30 s, then nothing (see LEDs); bit5 stays set. The pointing
device may differ between halves (D18). Code 3 is what an image for the wrong
hand leads to (the firmware takes it, D33): the half with USB cannot reach the
other half through the updater until one of them is flashed for its hand.

**The split pause (D15).** `kb/split_pause.c` overrides QMK's weak
`matrix_scan()`. While the half with USB is paused, it skips the whole split
exchange (keys, layers, RGB sync, pointing, the watchdog ping), the presence
ping and the Scan Lab relay; only `KEYBOARD_UPDATE` RPCs cross the link. On
entry the other half's keys are released once and its last pointer report is
dropped; this half keeps typing. On resume the split watchdog is re-armed, so
a half that rebooted meanwhile is pinged again. Only the half with USB can
pause, and only while the link is up. The relay pauses the link around the
other half's slow work (below).

**`KEYBOARD_UPDATE`.** Frames of at most 32 bytes each way:
`[op][seq][payload][crc16]` and `[status][op][seq][state][payload][crc16]`
(CRC-16/CCITT-FALSE). Ops: `INFO 0, BEGIN 1` (the 108-byte manifest in
fragments of up to 26 bytes; a fragment past byte 108 is INVALID), `PAGE 2`
(image bytes, up to 24 per fragment, never across a 256-byte page),
`STATUS 3, END 4, COMMIT 5` (CRC low 16), `ABORT 6`. INFO's flags are INFO's
bits 0-3 and bit5 (dirty tree); bit4 is reserved. The layouts are in
`kb/updater/update_split_wire.h`. The slave answers a repeated frame (same seq
and CRC) from its cache without running it again, and a PAGE fragment it
already holds is acknowledged and not written again. A repeated END or COMMIT
whose cached answer is gone is acknowledged again (one verify, one commit).

## The other half (M2a, stage 2)

**From the host.** `hand` = the other half's side makes a session for the
other half. It runs on the half with USB, where the chord is made (D4). The
other half must answer presence as the same release or a different version
(else MANIFEST and ARM get UNAVAILABLE). ARM checks the manifest against this
build's floors; its side and pointing device are not checked (D33).
BEGIN, CHUNK and END then stage and verify the image in this half's slot
exactly as in M1. Then:

1. COMMIT (nonce, CRC low 16) in VERIFIED: state RELAYING. The image goes to
   the other half from this half's slot (below).
2. The other half holds the image and has verified it: state RELAYED.
3. COMMIT again: state SUBSIDE_COMMITTING. The other half commits and resets;
   this half keeps the link paused for its commit and reboot (2 s + 2.5 s,
   R18), probes it, and goes back to IDLE. This half never commits that
   image: the request's hand, not the manifest's, made it a session for the
   other half, and such a session never reaches COMMITTING.

ABORT works up to the second COMMIT, and tells the other half. A relay
failure latches ERROR with the reason: the other half's own refusal (INVALID,
UNSUPPORTED, BAD_HASH, EPOCH, FLASH_ERR, ...), or UNAVAILABLE when it
stopped answering for 2 s, or TIMEOUT when it stayed in COMMITTING past the
probes. The 30 s session timeout still applies while relaying and while
RELAYED (D23: a host that went away stops the relay before the copy). While a
session for one hand runs, the session ops for the other hand get BUSY. INFO
with the other hand asks the other half over `KEYBOARD_UPDATE` (the reply has
INFO's layout without the JEDEC ID; UNAVAILABLE with state `0xFF` if it does
not answer; BUSY while a relay runs). RELAY (`0x0C`) shows the relay's phase
and progress.

**The relay** (`kb/updater/update_split.c`, from `updater_task()` on the half
with USB):

| Phase | Link | What happens |
|---|---|---|
| start, manifest | paused | ABORT (clears an old session there), then BEGIN: the 108-byte manifest in 5 fragments |
| checking and erasing | paused | The other half checks the manifest against its own build itself (D7), then erases its slot a sector per pass; this half polls STATUS every 20 ms. A sector erase keeps the other half's interrupts off for about 50 ms, longer than the 20 ms serial timeout, so it erases a sector that needs it only after a request since its last erase and 5 ms after that request (`SVAL_UPDATE_SPLIT_ERASE_GAP_MS`): this half gets an answer between every two sectors, and an ABORT lands within a sector |
| sending | running | PAGE fragments read from this half's slot, at most 4 ms of RPCs per main-loop pass; both halves type between them |
| end, verifying | paused | END; the other half hashes its slot (SHA-512 against the manifest, structure, CRC); its CRC must equal this half's |
| verified | running | STATUS every 1 s keeps the other half's session alive until the host's COMMIT |
| commit, hold | paused | COMMIT; the other half commits 100 ms later from housekeeping with the M1 routine; the link stays paused 4.5 s. An answer of IDLE to COMMIT (it already rebooted, or lost the session), or no valid answer for 2 s (COMMIT may have been taken with its answer lost), also leads to the hold: the probe decides |
| probe | paused | STATUS. ERROR, or IDLE with an error: it did not commit (failure). COMMITTING: it has not begun its copy; probe again every 500 ms, and if it then falls silent (its copy) the whole hold again; at most 20 probes, then TIMEOUT. IDLE without an error: done. No answer (a new release may not talk to this one, V): done, flagged unconfirmed (RELAY flags bit1) |
| aborting | as it was | After an ABORT from the host, the session timeout, or a failure: ABORT once per pass until the other half answers (it may be erasing), for at most 2 s, and only then the link resumes. A relay whose link is already dead (no valid answer for 2 s) sends one ABORT and resumes at once |

A request without a valid answer is sent again unchanged (the other half
answers a retry from its cache); INVALID is accepted as a refusal only on the
fourth answer in a row, since a request damaged on the way is also answered
INVALID. No valid answer for 2 s fails the relay with UNAVAILABLE, then this
half sends one ABORT and resumes the link (in the commit phase it holds
instead, above).

After a relay ends done, INFO flags bit5 stays set until presence first reads
MATCH, also while presence reads NONE: a release that changes the split table
cannot talk to the old half at all, and would otherwise look like an unplugged
cable (V). Meanwhile the lights show the rainbow on both halves, then green at
MATCH, or red for 30 s once the halves still differ after 60 s idle (see LEDs).

**The other half** (`update_split_slave_rpc` in the SlaveThread,
`update_split_slave_task` from housekeeping):

- The callback answers every request at once. It programs one 256 B page when
  a PAGE completes a page (R16) and does nothing else slow. The callback runs
  in the SlaveThread, which can preempt the main thread in the middle of a
  DMA transfer, and the PMW SPI's DMA reads its TX source from XIP (R6): so
  the program is refused with BUSY while any DMA channel is busy, checked
  with interrupts off in the same section as the program, and the fragment
  is given back (the relay sends it again).
- The request and response lengths come from the wire (`rpc_info`, behind a
  CRC-8 only) and are clamped to the 32-byte RPC buffers.
- The manifest check, the erase, the image hash and the commit run in
  housekeeping, behind the pause. The two meet in a single-slot mailbox with a
  sequence number: the callback posts a job by changing the state, and moves
  the number with every new session and every cancel; housekeeping copies the
  job out under the split shared-memory lock (the callback holds the same
  lock), works on its copy, and posts the result only if the number and state
  are unchanged.
- No `KEYBOARD_UPDATE` request for 5 s drops the session (keeping any error as
  the reason, else TIMEOUT), and its LEDs go back to normal. Not once COMMIT
  is accepted.
- While its session runs it keeps full clock (no deep idle) and holds off the
  identity task, as `updater_active()` does on the half with USB.
- LEDs (D24): rainbow while it checks, erases, receives and verifies, magenta
  while it commits, red on an error (for 30 s). After that it follows the light
  the half with USB sends on the presence ping (see LEDs).
- An image is applied only after COMMIT, and only with the CRC of the image it
  verified.

`sval_on_reconnect` (a settings write) is skipped while an update runs (D21).

**From `sval_update.py`.** `update FILE.svup` picks the half from the image's
hand, or from `--half usb|other` (an MCU moved between hands). It refuses a
version-1 `.svup` and a board that reports another updater protocol, before
sending anything. An image built for another hand or pointing device than the
half runs (an auto image on a single-module build, or the reverse, included)
is never refused (D33): the tool prints a warning for each mismatch and asks
`[y/N]` before sending anything; `--yes` skips the question, and no answer
sends nothing. For the other half it waits for presence,
prints the other half's INFO, stages and verifies as in M1, relays (printing
the phases), commits the other half, then waits up to 10 s for it to answer
again and prints its version and presence; a fw that differs from the
manifest's (both nonzero) is an error. `pair A.svup B.svup` picks each image
by the hand each half reports, checks both against both halves' INFO
(protocol, release build, epoch, storage format, size) and sends nothing if
either would be refused, and asks once (or `--yes`) if either image names
another pointing device than its half runs; then it does the other half, then this half over USB (on
protocol 4 one chord, on this half, approves both: see "One chord for both
halves"; on protocol 3 the chord twice, both on this half), waits for the board to come back, and requires
presence to show the same release on both halves within 10 s and each half's
fw to equal its manifest's (when both are set). `relay` prints the RELAY op;
`info --other` asks the other half; `info` also prints this half's version
string (INFO page 1). A build reports the `SVAL_FW_VERSION` it was compiled
with (`kb/updater/fw_version.txt` unless `make ... SVAL_FW_VERSION=N`), and
`make_update.py` copies it from the image's build-info record into the
manifest, so the two agree unless `--fw-version` says otherwise (which it
refuses when the record has a nonzero number). For an A/B pair of test images
build twice with `SVAL_FW_VERSION=2101` and `2102`.

## One chord for both halves (protocol 4, D41)

A pair update asks for the chord once. The first session, for the other half,
also carries this half's own manifest:

1. MANIFEST (the other hand) in pieces, then PAIR_MANIFEST (this hand) in
   pieces, then ARM (the other hand, mode 1).
2. The pair ARM checks the other manifest against the other half, as a single
   ARM does, and the pair manifest against this half's own build and floors.
   Either refusal latches ERROR with its status, so the user is never asked to
   approve a file one of the halves would refuse. The reply carries both
   4-byte codes (`rsp[5..8]`, `rsp[9..12]`) and the PAIR flag; the host shows
   both at the one chord.
3. The chord, made once in that session, creates the **held approval** on this
   half: SHA-512 of the pair manifest, the session's client ID and its nonce,
   in RAM only. On a no-chord build it is created at the pair ARM.
4. The other half's install goes on as before (stage, verify, relay, its
   commit). Once the relay is DONE the approval waits for this half's install
   for `SVAL_UPDATE_PAIR_HOLD_MS` (120 s; the contract allows 90-180 s, and the
   host sends this half's ARM within 60 s). STATUS byte 23 bit0 says it is
   held.
5. This half's own MANIFEST and single ARM, from the bound client, with the
   very manifest that was approved: the ARM reply has PRE_APPROVED and
   BY_PAIR, the chord counts as made (no blue light, no white flash), and BEGIN
   is ACCEPTED at once. Any ARM for this half uses the approval up, matching
   or not: a mismatch gets the chord prompt instead.

REBIND with the pair session's nonce moves the approval to the sender in any
state, IDLE between the sessions included, and REBIND is answered while the
other half commits. A REBIND while this half's manifest loads moves that load
too. ABORT (in IDLE too), any ERROR in either session, a MANIFEST from another
client or for the other hand, a reset of this half and the timeout clear it.
Nothing may ask for the chord twice: a host that finds the approval gone
after the other half's install (STATUS bit0 clear, or no PRE_APPROVED) stops,
and the user finishes this half with an update of its own (one chord). A
one-half update keeps one chord, as in protocol 3. `sval_update.py pair` does
all this on protocol 4 and the old two-chord flow on protocol 3.

## Settings reset (protocol 4, D42)

`SETTINGS_RESET` (op `0x0E`, `[hand]` this half, `'R' 'S' 'E' 'T'`, scope 1 for
both halves or 0 for this one) puts every stored setting back to the defaults of
the firmware each half runs, through the same path as a different build (D39).

- Only in IDLE with no held approval, no relay and no reset already pending;
  else BUSY. A bad magic or scope, or the other hand: INVALID.
- Scope 1: the other half first, over a new `KEYBOARD_UPDATE` op (`0x07`). It
  refuses with BUSY while it holds a session, and programs one page over its
  build reset record (no erase, under 1 ms, in the SlaveThread, under the same
  DMA check as a PAGE). With no presence MATCH or VERSION, or no answer (a
  half on protocol 2 or 3 does not know the op), UNAVAILABLE and nothing
  changes anywhere.
- Then this half programs the same page over its own record. The record then
  reads all zeros, which is never a valid record.
- The reply's done mask says which halves took it (`0x03`, or `0x01` for
  scope 0). FLASH_ERR when a record could not be written; the mask says which
  half already took it.
- This half resets 100 ms after the reply, the other half 300 ms after it took
  the request: both within 500 ms.
- Each half's next boot finds no valid record and resets every stored setting:
  the keymap, macros, combos, tap dances, overrides, QMK settings and the
  keyboard's VIA custom values (layer colors, pointing and scan settings). The
  EEPROM reset erases the whole store, so the custom values fail their stamp
  and take the build's defaults. Identity (serial and board name) is kept, and
  so is the updater state. The new record carries the notice: GET_INFO byte 18
  bit 2 until `0x2A`. The USB product ID does not change.
- Dev keep builds (`SVAL_DEV_KEEP_SETTINGS`) skip the D39 reset, but honor a
  SETTINGS_RESET (they reset when the record reads all zeros).

The record is written through the boot ROM from RAM (`build_reset.c`), as
`identity.c` and `update_flash.c` do. The Pico SDK's `flash_range_program()`
hard-asserts any address above `PICO_FLASH_SIZE_BYTES` (2 MB in this build), and
the record is at 16 MB - 12 KB: an early build that called it stopped the half
without USB at its first SETTINGS_RESET (found on the test boards; nothing was
written).

### A blank board (D44)

Mode 1 (`req[6]`; 0 or a missing byte is the reset to defaults) asks for a
blank board: the same reset, then every key on every layer `KC_NO`.

- Each half programs the blank mark with the invalidation, in the same page
  program: `"BLNK"` and its complement in the 8 bytes after the record. Mode 0
  programs zeros there.
- At the next boot, pre-init finds a cleared record with the whole mark. The
  reset runs as for D39; then `build_reset_post_init()` writes `KC_NO` over the
  whole dynamic keymap (after `sval_init()`, which ran in the module post-init),
  and only then writes the record, which erases the sector and the mark. A
  power cut in between leaves the mark, and the next boot blanks again.
- The other half gets the mode as a fifth payload byte of `KEYBOARD_UPDATE`
  `0x07`. A protocol 4 half from before D44 refuses that payload (INVALID), so
  the request is UNAVAILABLE and nothing changes on either half.
- A half with USB on a protocol 4 build from before D44 ignores `req[6]`: mode
  1 is its reset to defaults, on both halves. The reply is the same, so the
  host reads the keymap back (`sval_update.py --blank` fails; Keybard: F20).
- The firmware does this, not the host, because it is atomic per half and
  covers the half without USB too. A host that wrote `KC_NO` after a reset to
  defaults would reach only the USB half's keymap, and a closed tab mid-write
  would leave part of the defaults behind.

`sval_update.py reset-settings [--this-half] [--blank] [--ack]` sends it, waits for the
board to come back, prints the storage notice, with `--blank` reads every key
back and checks it is `KC_NO`, and with `--ack` sends `0x2A`.
`sval_update.py storage [--ack]` prints that notice and can clear it. Keybard
sends scope 1 for "Reset to the default layout" when the install did not already reset
the settings (the image was identical, or only one half was installed), and scope 1
with mode 1 for "Wipe to a blank board" after every install.

## The image's own build info (D45)

A release build reads the build-info records in the image while it hashes it
(`update_build_scan`, `update_image.c`) and refuses a no-chord (`0x40`) or
kept-settings (`0x80`) image with BAD_IMAGE, whatever the manifest's flags say.
The half with USB checks either half's image at VERIFYING_IMAGE; the half
without USB checks a relayed image again. A record matches as `make_update.py`
matches it (`"SVBI"`, `info_ver` 3). The scanner compares the magic a byte at
a time, so it adds no `"SVBI"` literal of its own to the image. An image with no
record is taken.

## Dev builds with no chord (D43)

For firmware work, never released. `SVAL_UPDATE_NO_CHORD=yes` makes every ARM
count as confirmed: INFO flags bit 7, ARM flags PRE_APPROVED and NO_CHORD, no
blue light. A pair ARM on such a build holds the approval at once, so this
half's install has PRE_APPROVED, BY_PAIR and NO_CHORD.
`SVAL_DEV_KEEP_SETTINGS=yes` keeps stored settings across different builds
(no D39 reset), except after a SETTINGS_RESET.

```
util/sval_dev_flash.sh SERIAL [left|right|both] [keep|fresh|blank]
```

builds them (the `sval` keymap, or `SVAL_DEV_KEYMAP`; `SVAL_DEV_KEYMAP_DIR`
copies a private keymap in, never tracked) and installs them with
`sval_update.py --yes`: both halves with `pair`. Its `.svup` files are
DIAGNOSTIC, so a board on a release build refuses them; the first dev install
on such a board goes through BOOTSEL and the `.uf2`. Release builds refuse
both flags at three gates: `rules.mk` (`$(error)`), `#error` guards in
`update_build_info.c`, `update_gesture.c` and `build_reset.c`, and release CI
(`check_release_elf.py` refuses the symbols `update_no_chord` and
`sval_dev_keep_settings` and the build-info flags `0x40` and `0x80`;
`make_update.py --release` refuses the flags).

## What the half without USB stores

(The technical design's question 4.4.2: does "Put my layout back" restore
everything after an install?)

Each half has its own settings store in its own flash: the keymap, macros,
combos, tap dances, overrides, QMK settings and the keyboard's VIA custom
values. An install of a different build (D39), or SETTINGS_RESET, resets the
store on each half it reaches. While a half has no USB it uses almost none of
its own store:

- Keys, layers, macros, combos and every other behavior run on the half with
  USB, from that half's store. The half without USB only scans its matrix and
  sends it over the split cable.
- The scan and idle settings (turbo scan, the pre and post waits, the scan,
  idle and deep periods, the idle flags and the deep clock) come from the half
  with USB in every presence ping (`KEYBOARD_SYNC_A`, every 500 ms). Until the
  first ping after its boot, the half without USB scans with its own values,
  so for at most half a second after an install it scans with the defaults.
- The pointer settings (DPI, scroll and automouse) are applied on the half
  with USB, which drives the pointing over the split cable. Layer colors come
  from the half with USB too.
- Identity (serial and board name) is per half and is never reset.

So a `.svil` restored through the half with USB brings back everything the
keyboard does while USB stays on that half: nothing is lost on the other one.
The one gap: the other half's own store keeps the defaults. If the user later
moves the USB cable to that half, it starts from the defaults, and the layout
must be restored there too (Keybard can say so when it sees a half it has not
restored). No firmware change is needed for "Put my layout back".

## Hardware test notes (protocol 4)

On the trackball test boards (right half with USB, left half behind the split
cable), no-chord dev builds of this branch, 127,744-byte images, Windows
`sval_update.py` over hidapi:

| Step | Result |
|---|---|
| `pair` from a protocol 3 dev build to protocol 4 | Two sessions, no chord asked; 41.6 s in all. Settings reset on both halves (new build, D39), notice set, `0x2A` cleared it |
| `pair` again on protocol 4 (identical images) | One approval: the pair ARM showed both codes, and this half's ARM came back PRE_APPROVED, BY_PAIR and NO_CHORD (`0x07`), BEGIN at once. 40.7 s in all: the other half's image staged on the half with USB in 9.9 s (7.5 s of transfer, 2,587 reports, 2.0 ms each), relayed in 11.3 s (64 retries), its commit and hold about 4.5 s; this half's install 9.8 s; presence MATCH 4.0 s after the board came back. Settings kept (the identical image) |
| `reset-settings --this-half --ack` | Back on USB 5.3 s after the reply; a probe key and a layer color back to their defaults, notice set, `0x2A` cleared it; the serial kept |
| `reset-settings` (both) with the other half silent | UNAVAILABLE, nothing changed |
| Restore of the backed-up keymap | Identical to the backup |

Not yet run on hardware: `reset-settings` on both halves with the fixed build.
The first attempt, with the SDK flash call described under "Settings reset",
stopped the left half; it needs a power cycle (unplug and replug the right
half's USB) before the both-halves run.

## make_update.py

```
MU=keyboards/svalboard/tools/make_update.py
python3 -I $MU IN.uf2 -o OUT.svup --kb svalboard/right
python3 -I $MU verify OUT.svup --build plain
```

- `--kb` sets the side and pointing device: `svalboard/left` and
  `svalboard/right` are the auto builds (pointing ID 5); the single-module
  paths (`svalboard/trackball/pmw3389/right`, ...) still work. `--keymap`
  (display only) defaults to the image's build-info record, else `sval`.
- `--fw-version` and `--version-string` default to the record; a value that
  contradicts a set record is refused.
- A `--kb` whose side or pointing device is not the record's is a warning,
  not a refusal (D33): the tool asks `[y/N]` before writing, `--yes` skips
  the question, and no answer (no terminal) writes nothing.
- `--diagnostic` sets DIAGNOSTIC (Scan Lab, keytest or host-bootloader
  images). `--epoch` and `--storage-format` set the floors' fields.
- `--release` sets RELEASE and refuses anything that is not a clean release
  update: a manifest with flags other than exactly RELEASE, a keymap other
  than `sval` or `blank`, another updater protocol or an older storage format;
  an image without a build-info record, or whose record is not a release
  build, has test hooks, keytest, the host bootloader or a reserved bit set,
  reports another version than the manifest, or names another keymap (another
  side or pointing device is the warning above). `--expect-version`, `--expect-fw-version` and
  `--expect-sha512` (release only) also check the tag, the number and the
  image's hash.
- `--unsafe-allow-bad-image` builds an image that fails the structure checks,
  for rejection tests. Never with `--release`.
- `verify FILE.svup --build release|plain` checks a `.svup` offline as that
  kind of build would before erasing anything: the manifest rules, the hash
  and the structure. A release build also refuses DIAGNOSTIC.

`util/updater_test/run.sh` with `SVAL_RELEASE_SVUPS="a.svup:ok b.svup:refused"`
runs `.svup` files through the C code of a release build
(`tests/sval_updater/test_release.c`).

## Vendored code

`kb/updater/vendor/` holds SHA-512 only, cut without other changes from
Monocypher 4.0.2: `sha512.c` and `sha512.h`, with the dual BSD-2-Clause /
CC0-1.0 license in `LICENCE.md`. The rest of Monocypher is not in the tree.
`kb/updater/vendor/README.md` lists the source lines and the SHA-256 of each
file. The host tests check the FIPS 180-2 vectors and that the incremental and
one-shot forms agree.

## Release CI

**What a release is (D34, D40).** From x4 on, two files:
`svalboard/either:sval` and `svalboard/either:blank`, one image for both
halves (each half takes its side from its thumb strap or its stored side
record), the auto build, which carries every pointing driver and starts
whichever module is attached (pointing ID 5), and the native touchpad for
Azoteq pads (`keyboards/svalboard/docs/native-touchpad.md`). The `blank` image
has no key bindings: a test image for checking every key after a build. Each
is published as a pair UF2, `svalboard_<keymap>_<tag>.uf2`
([pair-uf2.md](pair-uf2.md)): copied onto either half in BOOTSEL it installs
both halves, and Keybard installs from the same file. No `.svup` is
published. The per-side builds (`svalboard/left`, `svalboard/right`) and the
per-module builds (`svalboard/trackball/...`, `trackpoint`, `azoteq`) still
build but are not released. `SVAL_TEST_WIPE` is for bench builds only:
a release updater build refuses to compile with it.

**Settings (D39).** The first boot of a different firmware build resets every stored setting to that build's defaults, on each half, whether it arrived through the updater or a `.uf2` (`kb/build_reset.c`; `keyboards/svalboard/docs/settings-storage.md`). Flashing the identical image keeps them. GET_INFO byte 18 bit 2 tells the host. Keybard and a command-line tool will save a `.svil` before flashing and load it afterwards (D35).

**Tag names (D41).** Two kinds of tag release, and nothing else:

| Tag | Matches | Release |
|---|---|---|
| `v3`, `v3.1` | `^v[0-9]+(\.[0-9]+)*$` | A full release, published as a **draft**: a person checks it and publishes it, and it then becomes the latest release |
| `x3`, `x3.1` | `^x[0-9]+(\.[0-9]+)*$` | A **prerelease**, for next.keybard.svalboard.com, published at once, so it never becomes the latest release |

Every other tag is refused by the first job (`vLaunch3`, `vNext1`, `v3-rc1`,
`x`, `3`), so it cannot publish. The same tag name marks the firmware,
Keybard and Keybard Host releases that go together: `x3` on all three, then
`v3`. Both kinds must name a commit on the `svalboard` branch, and only
`svalboard/qmk` publishes: on a fork the run stops at the first job. Older
releases (`vLaunch`, `vLaunch2`) keep their tags and still count for the
version check; new releases use `v` and `x` tags only.

**Versions (D17, D32).** Tags are names, so the number is committed in
`kb/updater/fw_version.txt` and the version string is the tag name. Raise the
number in the commit you tag. Release CI's first job
(`kb/tools/release_version.py`) fails unless the tag is a release tag (above)
and a valid version string (at most 16 of `A-Z a-z 0-9 . _ + -`), and the
committed number is greater than the highest one committed at any published
release that is not a draft or this tag (a release from before the file
existed counts as 0). Prereleases count: each `x` tag raises the number, and
the next `v` tag must be higher than every `x` tag before it.

**Graduating an x tag (D41).** A full release is a new build: after `x3` has
been tried on next.keybard, raise `fw_version.txt` in a commit on `svalboard`,
tag it `v3`, check the draft release, then publish it. Never edit an `x`
release into a full release by hand: it would become the latest release with
a number the next `v` tag must still exceed. A draft does not count until it
is published.

**Running a release again.** If a run fails, delete the tag's release if one
was made, then re-run the failed jobs, or start a new run on the tag:
`gh workflow run release.yml -R svalboard/qmk --ref x3 -f tag=x3`. The run
must be on the tag itself and name the same tag, or the first job stops.

**The jobs** (`.github/workflows/release.yml`, on `v<N>` and `x<N>` tags, or a
re-run by hand):

| Job | What it does |
|---|---|
| version | The check above. Outputs the version, the number, and whether the release is a prerelease or a draft |
| build | The 2 release builds (`svalboard/either`, sval and blank) as **release updater builds** (`SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes SVAL_FW_VERSION_STRING=<tag>`), through `build-firmware.yml` with `updater: true`, in the pinned image with the pinned toolchain (below). Each runs `check_ram_funcs.py`, then `make_update.py pair --release --expect-version <tag> --expect-fw-version <N>`, which writes the pair UF2, and `make_update.py verify-pair`. It uploads the pair UF2 and the ELF. No build job can write to the repository (`contents: read`, no Git credentials kept) |
| lint | `check_tag_commit.sh`: the tag still names the run's commit. `check_release_artifacts.py`: exactly the 2 builds' files; each pair UF2 has the pair layout and writes no protected flash; its image is its ELF's flash contents (as `objcopy -O binary` writes them); each manifest's hash, version, hand (2, either), pointing device and keymap, and each image's build-info record, match the build's name, and the release checks pass. `check_release_elf.py` on the 2 ELFs: an updater build whose build-info record says RELEASE and nothing else, one image for both halves (hand 2), the auto pointing build and a release keymap, with no test-hook, keytest or host-bootloader symbols. `check_split_tables.py --golden`: the builds have the same split message table (an `either` build pairs with itself), and each build's table equals `kb/tools/split_table_golden.json`, so the release links with the per-side releases already on boards |
| publish | Creates the release named after the tag (a draft for a `v` tag, a published prerelease for an `x` tag) with the 2 pair UF2 files and two lines on how to use them (copy onto `RPI-RP2`, or open in Keybard). Refuses if the tag already has a release |

Every job checks out the run's commit (`github.sha`), never the tag by name,
and `check_tag_commit.sh` stops the run from publishing under a tag that now
names another commit.

**Toolchain.** Updater builds in release CI use a pinned image,
`ghcr.io/qmk/qmk_cli@sha256:b7d7fa8f…` (`build-firmware.yml`), and download
Arm GNU Toolchain 13.2.rel1 (`arm-gnu-toolchain-13.2.rel1-x86_64-arm-none-eabi`,
SHA-256 `6cd1bbc1…5fdbb`, checked) and put it first on `PATH`; the build step
fails unless `arm-none-eabi-gcc -dumpversion` is `13.2.1`. That is the
toolchain every M1, M2b and M3 test used. The image's own GCC (15.2 in that
digest) turns `commit_ram_main` into an IPA-SRA clone (`commit_ram_main.isra.0`),
which `check_ram_funcs.py` refuses, and makes different images. Changing the
toolchain means rerunning the hardware commit tests (R28). Builds without
`updater: true` keep the unpinned image they always used.

**This changes what release CI publishes:** every `.uf2` on a release is now an
updater build, not a default build: larger images, still far below the
1,408 KiB cap. A default `make` (no flags) is unchanged. Halves running a
default build cannot talk to halves running an updater build (see "One
release on both halves"), so both halves must move to the same release, which
they always do.

**The golden split table** (`kb/tools/split_table_golden.json`) is the table of
the release updater builds: 15 transactions (QMK's 12, plus the three keyboard
RPCs `KEYBOARD_SYNC_A`, `KEYBOARD_SYNC_B`, `KEYBOARD_UPDATE`), each with its
buffer sizes and shared-memory offsets. A change to the split table in a
release must regenerate it in the same commit, and the release notes must say
that both halves need the new release:

```
T=keyboards/svalboard/tools/check_split_tables.py
G=keyboards/svalboard/tools/split_table_golden.json
python3 -I $T --write-golden $G BUILD.elf
```

**Checking a published release.** `kb/tools/m3/check_release.sh TAG [REPO]`
downloads its assets and runs `check_release_assets.py`: a prerelease for an
`x` tag and a full release (a draft until published) for a `v` tag, exactly 2
pair UF2 files and no `.svup`, each with the pair layout, the build's hand,
pointing device and keymap, and accepted by `make_update.py verify-pair
--build release`.

## Tools and tests

- `kb/tools/make_update.py`: UF2 → `.svup` (manifest, then the raw image), and
  `verify`; `pair` (UF2 → pair UF2) and `verify-pair` (`--svup` extracts what
  a host sends), [pair-uf2.md](pair-uf2.md). It refuses malformed UF2s and
  images the device would refuse.
- `kb/tools/sval_update.py`: the host tool (`list`, `info [--other]`, `status`,
  `relay`, `diag`, `abort`, `update`, `pair`, `reset-settings`, `storage`,
  `reject`), protocols 3 and 4. Close Keybard first: raw
  HID replies reach every open handle.
- `kb/tools/check_ram_funcs.py BUILD.elf`: the RAM-code check above.
- `kb/tools/check_split_tables.py`: the split-table check across left/right
  pairings (P1), above. With `--golden` it also checks a committed golden
  table.
- `kb/tools/release_version.py`: release CI's version check (D32).
- `kb/tools/check_release_elf.py`: release CI's lint of the release ELFs.
- `kb/tools/check_release_artifacts.py`: release CI's check that each pair
  UF2 holds its ELF's image, with matching manifests.
- `kb/tools/check_tag_commit.sh`: release CI's check that the tag still names
  the run's commit.
- `kb/tools/m3/check_release.sh`, `check_release_assets.py`: the check of a
  published release's assets.
- `.github/workflows/updater-tests.yml` runs `util/updater_test/run.sh` on
  pushes and pull requests that touch the updater, its tools or its tests.
- `util/updater_test/run.sh [UF2 ...]`: host tests under ASan and UBSan. They
  cover `make_update.py`, the manifest, image and SHA-512 checks, the session state
  machine, `sval_update.py` against the C state machine, the commit routine and
  a release build's rules. The commit tests run `update_commit.c`'s real step logic against a mock die and
  mock registers (`tests/sval_updater/update_commit_host.h`). They cut the power
  after every flash operation of staging and of the commit, both cleanly and
  torn (random bytes in the sector, block or pages the operation touched), and
  then check that staging changed only the slot, the commit changed only
  `[0, round_up(len, 64 KiB))`, nothing else changed, every boot ROM call came in
  order (interrupts off, XIP exited; the mock die refuses any other call), and
  page 0 is either invalid or the whole new image is in place.
  `test_split.c` covers the split link layer: the pause against mocks of QMK's
  matrix and transport (a key held on the other half at pause entry is
  released, the exchange stops, this half keeps typing, resume re-arms the
  split watchdog), the `KEYBOARD_UPDATE` frames (every single-bit error is
  caught, BEGIN bounded at 108 bytes), PAGE fragment assembly, and presence and the mismatch rules.
  `test_relay.c` (in the `test_updater` and `test_commit` builds) runs both
  halves' code in one process over a simulated split link
  (`tests/sval_updater/test_session.c`): the relay start to finish (the other
  half erases and verifies only behind the pause, the pause is never held
  while the image streams, the per-pass budget holds), the other half's own
  refusals (including a version-1 manifest and a nonzero reserved byte), faults on every RPC (dropped, damaged, duplicated, stale and
  reordered requests and answers), the link pulled in each phase (this half
  fails within 2 s and resumes the link, the other half drops its session 5 s
  after its last request, neither firmware changes), the 5 s timeout in each
  state, mailbox races (an ABORT or a new session between the copy-out and the
  result), idempotent PAGE, END and COMMIT, 20,000 random frames, a power cut
  on the other half after every flash operation of a relay (its old image
  boots, a new relay succeeds), the other half's commit with the real
  `update_commit.c` cut after every flash operation, `updater.c`'s session for
  the other half, and the version mismatch a half-done pair leaves (the rainbow,
  then green once this half is updated too; left alone, red after 60 s, then
  nothing). `test_relay_review.c` adds the
  review fixes: the slave's timeout race (a request between its clock read and
  its lock), RPC lengths past the buffers, a busy DMA channel at a page
  program, the other half with its interrupts off while it erases and commits
  (every RPC then fails; a 125,184 B and a 256 KiB relay still verify, and an
  ABORT while it erases lands between sectors with the link still paused),
  the probe finding it still COMMITTING, every COMMIT answer lost, and INFO
  flags bit5 until MATCH. `test_sval_update_tool.py` runs
  `sval_update.py update` with the other half's image and `pair` against the
  same simulation.
  `test_pair.c` runs the protocol 4 cases (`tests/sval_updater/PROTOCOL4_CASES.md`,
  the list Keybard's mock board runs too) against the same simulation: the pair
  ARM and its refusals, one chord for both halves, everything that clears or
  moves the held approval, and SETTINGS_RESET. A no-chord build of the same
  harness runs the no-chord cases, and `test_no_chord_tool.py` runs the tool
  against it; `run.sh` also checks that a release build refuses both dev flags.
