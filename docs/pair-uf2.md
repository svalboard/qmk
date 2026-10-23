# Pair UF2: one file for both halves

From x4 on, a release is two files, one per keymap:

| File | What it is |
|---|---|
| `svalboard_sval_<tag>.uf2` | The `svalboard/either` image with the `sval` keymap, as a pair UF2 |
| `svalboard_blank_<tag>.uf2` | The same with the `blank` keymap (no key bindings: the test image) |

There are no `.svup` files any more. Each file can be used in two ways:

- **By hand.** Put either half in BOOTSEL (hold BOOTSEL while plugging it in,
  or tap reset twice) and copy the file onto `RPI-RP2`. That half installs
  it, restarts, and then installs the same image on the other half over the
  split cable, with no chord.
- **In Keybard.** Keybard reads the manifest and the image out of the same
  file and installs both halves through the updater, protocol 4, unchanged
  (one chord for both halves, as for any install a host starts).

The image is the one-image build (`svalboard/either`, hand 2 in its manifest
and build info): each half takes its side at startup from the thumb strap,
then its stored side record (`SVSD`, 0xFFB000/0xFFC000), then a one-time
migration from the EEPROM handedness byte a per-side build left
(`keyboards/svalboard/side.h`, Sval `0x33`). So the half a file is copied onto
keeps its side, the other half keeps its side, and the file carries no side
at all. A half that has no side after the install types nothing and reports
it to Keybard (Sval `0x33`, the existing one-image behavior).

The code is `keyboards/svalboard/updater/update_pair.c` (the record and the
boot relay), `update_flash.c` (the record sector's flash routines) and
`keyboards/svalboard/tools/make_update.py` (`pair`, `verify-pair`).

## What the RP2040 boot ROM does with a UF2

Checked in the boot ROM's source, `raspberrypi/pico-bootrom-rp2040`
`bootrom/virtual_disk.c` and `async_task.c` (commit `ef22cd8`), not assumed:

- **A block counts only if** it has the three UF2 magics, the family-ID flag
  (`0x2000`) with family `0xE48BFF56` (RP2040), the not-main-flash flag
  (`0x1`) clear, and a 256-byte payload (`vd_write_block`). Any other block,
  a not-main-flash block or another family's included, is skipped and does
  not count. So such blocks could carry data for a host. The pair UF2 does
  not rely on this: Keybard's data is in a flash sector that the ROM writes
  like any other (below).
- **One transfer is all the blocks with one `num_blocks`.** A block with a
  different `num_blocks` starts a new transfer (`_update_current_uf2_info`).
  A block number seen before is ignored. When `num_blocks` distinct blocks
  have been written the ROM reboots; until then it stays in BOOTSEL. An
  interrupted copy therefore leaves whatever blocks it wrote in flash, and the
  half stays on `RPI-RP2` until a whole file arrives.
- **Erasing goes by block number, not by address.** The ROM erases the 4 KiB
  sector of a block's address when the first block of its group of 16 block
  numbers (`block_no * 256 / 4096`) arrives, and only then
  (`_write_uf2_page`, `cleared_pages`). A file whose block numbers 16k to
  16k + 15 do not all fall in one sector leaves pages unerased (programmed
  over old data) or erases pages written just before. The pair UF2 is
  therefore laid out in whole sectors: every group of 16 block numbers is
  exactly one sector, so it lands the same whatever order the host writes
  the blocks in (tested against a model of this code).
- Flash addresses `0x10000000`-`0x11000000` are accepted; on a die smaller
  than 16 MiB the chip itself wraps higher addresses.

## The file

A standard UF2. Every block: flags `0x00002000`, family `0xE48BFF56`,
payload 256 bytes, `block_no` 0 to N-1 in file order, `num_blocks` N. Two
regions, in this order:

| Blocks | Target address | Contents |
|---|---|---|
| 0 to M-1 | `0x10000000 + 256 * i` | The image (flash offset 0), then `0xFF` up to a whole sector. M = 16 × ceil(image_len / 4096) |
| M to M+15 | `0x10270000 + 256 * (i - M)` | The pair record sector: page 0 the record, pages 1-15 `0xFF` |

N = M + 16. The record comes last, so a copy that stops early never writes
it. Nothing else in flash is written: the file never touches the settings
(`0x160000`-`0x25FFFF`), the staging slot, the side records, the build reset
record or identity (`make_update.py verify-pair` and release CI check it).

### The pair record

Page 0 of the sector at flash offset `0x270000`, little-endian
(`update_pair_record_t`, `update_pair.h`):

| Offset | Field | Value |
|---|---|---|
| 0 | magic u32 | `"SVPU"` (`0x55505653`) |
| 4 | record version u16 | 1 |
| 6 | length u16 | 128 |
| 8 | kind u8 | 1: send this image to the other half |
| 9 | reserved | 3 bytes, 0 |
| 12 | image offset u32 | 0 (the image is the one at the start of flash) |
| 16 | manifest | the 108-byte updater manifest of the image ([updater.md](updater.md), "The .svup file and the manifest"): its SHA-512, length, version, floors |
| 124 | CRC u32 | CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no xorout) over bytes 0-123 |

The rest of page 0 is `0xFF`. Page 1 is the state page: the boot ROM leaves
it erased, and only the boot relay changes it, by clearing bits
(`update_pair_state_t`: three boot-try bytes, a "seen" byte, then the other
half's fw version, git hash and updater protocol at its first answer). Pages
2-15 are `0xFF`. A host reading the file sees page 1 erased.

**Versions.** The manifest's `fw_version` (the release number, D17) and
version string (the tag) say what the image is; `updater_proto` its updater
protocol. The record version is 1; a later record version is refused by this
firmware (UNSUPPORTED), and a host should refuse one it does not know.

**Left and right.** There are none in the file: the manifest's hand is 2
(either), the image's build-info record says the same, and each half keeps
its own side (above). The boot relay refuses a record whose manifest is for
one side (hand 0 or 1): sending a one-side image to the other half would
leave both halves on the same side. The per-side builds (`svalboard/left`,
`svalboard/right`) still build for development, but are not released and
cannot be made into a pair UF2.

### Reading it (Keybard)

1. Check the length is a multiple of 512 and every block as above (magics,
   flags exactly `0x2000`, family, payload 256, numbering 0..N-1 of N).
2. The last 16 blocks must target `0x10270000` + 256 × k; the others
   `0x10000000` + 256 × i, a multiple of 16 of them.
3. Parse the record from the first page of the last 16 blocks: magic,
   version 1, length 128, kind 1, image offset 0, reserved 0, CRC. The rest of
   that sector is `0xFF`.
4. The manifest's `image_len` must fall in the last sector of the image area,
   with `0xFF` after it; the image is the first `image_len` bytes of the area,
   and its SHA-512 must equal the manifest's. The manifest's hand is 2.
5. Install with the updater, protocol 4, for both halves with the same
   manifest and image: MANIFEST (the other hand), PAIR_MANIFEST (this hand),
   ARM mode 1, then the other half, then this half
   ([updater-protocol4.md](updater-protocol4.md)). The chord is made once.

`make_update.py verify-pair FILE.uf2 --svup OUT.svup` does steps 1-4 and
writes the `.svup` a host sends (`sval_update.py pair OUT.svup` installs it on
both halves).

## The boot relay

On every boot of a half with the updater (`update_pair_task`, from
`updater_task`):

1. With no record (the magic), nothing happens: one 4-byte read.
2. The half without USB erases the record (the cable moved).
3. The half with USB checks the record and its manifest against its own
   floors (security epoch, storage format, D19); a refusal erases it and shows
   the red light. It counts the boot in the state page; a record already
   tried on three boots is erased, with the red light.
4. It hashes its own image (SHA-512 over flash `[0, image_len)`, a 2 KiB slice
   per pass) and compares it with the manifest's. Another image (an
   interrupted copy, or a record left from an earlier file) erases the record,
   with no light: nothing the customer asked for went wrong.
5. It waits up to 15 s for the other half's presence on updater protocol 3
   or 4. With none, or a half without the updater, the record is kept for the
   next boot, with no light: nothing has failed yet.
6. At the other half's first answer it stores that half's fingerprint (fw
   version, git hash, updater protocol). A later boot that finds a different
   fingerprint erases the record (the other half was flashed meanwhile); a
   later boot that meets the other half for the first time does not send it
   an older release than it runs.
7. The relay (`update_relay_start_self`): the M2 relay, reading this half's
   own image instead of the staging slot. The other half checks the manifest
   and the image itself (D7) against its own floors, as for any relay. When it
   holds the image verified, COMMIT goes at once: no host and no chord. After
   DONE the record is erased. A link failure is tried again after 2 s, three
   relays per boot; then the record is kept for the next boot. A refusal by
   the other half erases the record, with the red light.

While the boot relay works (steps 3-7), the updater answers INFO, STATUS,
RELAY and DIAG, and every other op with BUSY: a host cannot start a session
meanwhile. RELAY shows the relay's progress as for a host's relay. The
protocol is unchanged.

**Lights** (D24, the table in `docs/updater.md`): the half with USB shows the
rainbow once the other half answers and can take the image, while it hashes,
waits and sends, and through the other half's write and restart (magenta is
only on the half that writes). Green on both halves once presence matches.
Red at 1 Hz for 30 s only for a record that was refused, damaged or tried too
often, or when the halves still differ 60 s after the relay. A record kept
for an unplugged or absent other half shows nothing. The other half shows its
own session's colors (rainbow, magenta), then follows the light the half with
USB sends on the presence ping.

**Timing on the test boards:** about 20 s from the first boot to DONE for a
152 KiB image (relay 15 s, the other half's commit and restart about 5 s),
and presence matches about 6 s later.

## Flash layout

| Flash offset | Contents |
|---|---|
| `0x000000`-`0x15FFFF` | Firmware area (the image) |
| `0x160000`-`0x25FFFF` | Settings (wear leveling, two copies) |
| `0x260000`-`0x26FFFF` | Free (a gap after the settings) |
| `0x270000`-`0x270FFF` | **Pair record** (one 4 KiB sector) |
| `0x271000`-`0x7FFFFF` | Free |
| `0x800000`-`0x95FFFF` | Updater staging slot |
| `0xFFB000`-`0xFFCFFF` | Side records (one-image builds) |
| `0xFFD000`-`0xFFDFFF` | Build reset record (D39) |
| `0xFFE000`-`0xFFFFFF` | Identity |

The record sector shares no 4 KiB erase sector with anything else;
`update_flash.c` asserts it against every other region.

## Security and stability

### No host-reachable path to an install with no chord

The boot relay is the only code that installs without a chord, and it starts
only from a valid record in the record sector that names the image this half
runs. Who can write that sector:

- **The boot ROM**, from a UF2 copied onto `RPI-RP2`: the whole sector,
  record page included.
- **The boot relay itself**: `update_pair_sector_erase()` (the whole sector)
  and `update_pair_state_program()` (page 1 only, clearing bits). Their RAM
  routine takes no address: both addresses are constants, so no caller can
  move them, and neither can program page 0. Firmware can only destroy a
  record, never make one.
- **Nothing else.** The updater's staging erases and programs only inside its
  slot (`0x800000`-`0x95FFFF`) and the commit only inside
  `[0, SVAL_UPDATE_MAX_IMAGE)`, both checked again next to every boot ROM call;
  settings, identity, the side records and the build reset record are fixed
  sectors of their own. `update_flash.c` asserts that the record sector
  overlaps none of them, and `util/updater_test/run.sh` checks that
  `update_relay_start_self` and the record offset are used nowhere else.

So the updater over USB, Keybard, and the Sval and VIA commands can stage or
install an image, but never write a record: an image that carries record
bytes lands in the slot or the firmware area. And a record never names
something a host chose: it must match the image this half runs, which the
same copy wrote.

Tested: every host flow of the host tests (a session for this half to its
commit, a session for the other half through the relay and its commit, a
pair update, SETTINGS_RESET) with an image that carries a valid record runs
with the record sector locked in the mock die, and no ROM operation touches
it; the next boot relays nothing. On the test boards: an image with a valid
record for itself, staged over USB and left uncommitted, then installed over
USB, followed by a restart, relays nothing.

### BOOTSEL needs the customer

The rule that copying a file in BOOTSEL is consent holds only if no host can
put a release build into BOOTSEL. In release builds:

- VIA `id_bootloader_jump` (`0x0B`) has no handler in `quantum/via.c`: it
  answers `id_unhandled`.
- Scan Lab's reboot into the bootloader (REBOOT_ARM, REBOOT_GO) needs
  `SVAL_HOST_BOOTLOADER`, a dev flag. A release build refuses it at compile
  time (`update_build_info.c`), and release CI refuses its symbols
  (`check_release_elf.py`: `reboot_pending`, `reboot_due_ms`). Keytest's
  reboot needs `SVAL_KEYTEST`, refused the same way.
- No release keymap binds `QK_BOOT`, and bootmagic is off (`info.json`).
- What is left needs a hand on the board: the BOOTSEL button, a double tap
  of reset, or a key a host has bound to `QK_BOOT` through VIA, pressed by the
  customer.

Anyone who does get a half into BOOTSEL can already write any firmware to it,
and that firmware could do anything to the other half. A pair UF2 adds no
reach a BOOTSEL copy did not already have.

### The record is bound to the copy that wrote it

A record relays only the image that is running, by SHA-512 over flash
`[0, image_len)`. It is erased after DONE, after any refusal, and when it
does not match. A record kept because the other half did not answer is
bound to that half: its fingerprint at the first answer must stay the same,
and a later boot never sends an older release to a half it meets for the
first time. A boot of this half without USB erases it, and three boots of the
half with USB use it up.

Tested (host): a byte of the image changed, its last sector left from another
image, its last byte changed: never relayed, erased. The other half flashed
by hand to a newer release between boots: erased, never relayed. A later
boot meeting a newer other half first: erased. The first boot sending an
older release (the customer's choice): installed. A boot without USB:
erased. On the test boards: a copy interrupted after the record sector and
100 image blocks, then a whole plain UF2 of another image: no relay.

### A damaged or interrupted copy

The record is written last and checked by its CRC; the image by SHA-512
against the record's manifest; the other half checks the SHA-512 again, and
the structure and CRC of what it received. An interrupted copy never reboots
the half (the ROM waits for every block); a later whole copy of another file
leaves a record that does not match, which is erased. Tested: a damaged
record (CRC, version, a reserved byte, one-side hands, a manifest below this
build's epoch), refused with the light and erased, on the host and on the test
boards (a pair UF2 with one manifest byte flipped in its record: this half
installed, the other half left alone).

### Flash layout and smaller dies

The record sector shares no erase sector with the settings, the staging slot,
the side records, the build reset record or identity (asserted). It lies in
the first 4 MiB. The flash map assumes the 16 MiB part every shipped board
has (confirmed; the test boards read JEDEC `ef4018`); the updater checks the capacity
byte (`0x18`) and reports UNAVAILABLE on any other die (D26). On another die:

- The boot relay does nothing (it needs the updater's flash check), so the
  record is inert, and Keybard's guided copy applies, as for any install.
- 4 MiB and 8 MiB: the file's writes land where they say; nothing wraps.
- 2 MiB or less: `0x270000` would wrap to `0x070000`, inside the firmware
  area. Such a part cannot run this firmware anyway: the settings at
  `0x160000`-`0x25FFFF` would wrap onto the firmware the same way.

### The relay

- **Retries:** three relays per boot after a link failure, 2 s apart; three
  boots in all. A refusal by the other half is final (erased).
- **Typing:** the relay runs in housekeeping as a host's relay does. The half
  with USB keeps typing throughout; the other half's keys pause while it
  checks, erases, verifies and commits (D15), and work while the image
  streams.
- **Power cut:** on the other half, the existing safe states: before its
  COMMIT its old image boots; during its commit it comes up in BOOTSEL (the M1
  commit, unchanged). On this half: the record survives, and the next boot
  tries again with the same fingerprint (tested on the host: a cut halfway
  through the image stream, then DONE at the next boot). A half left in
  BOOTSEL does not answer, so the record is kept and then used up; the
  customer recovers that half by copying the file onto it.
- **The other half without the updater, on protocol 2, or not answering:** no
  relay, no light, the record kept for the next boot (bounded). A half on
  firmware without the updater cannot even link to an updater build (V).
- **Floors:** the other half checks the manifest against its own security
  epoch and storage format floors (D7, D19), as for any relay; nothing skips
  them. Tested: an epoch and a storage format below the other half's floors:
  refused, erased, nothing installed.
- **Sides:** the relay never looks at sides (one image for both halves). The
  other half keeps its stored side, or migrates it from its EEPROM on its first
  boot of a one-image build.

### Migrating per-side boards

A half that ran a per-side build has its side in the EEPROM handedness byte.
`sval_side_init()` reads it in `keyboard_pre_init_kb`, before
`build_reset_pre_init()` formats the settings for the new build (D39), and
stores it in the side record. That holds for the half the file was copied
onto (its first boot after the copy) and for the other half (its first boot
after the relay's commit): the commit does not touch the settings. On the test
boards, both halves had no side record (a test-only build erased them) and a
per-side left and right build: after one copy onto the right half, it ran the
image as right (source 3, migrated), and the left half as left.

## Tests

Host (`util/updater_test/run.sh`):

- `tests/sval_updater/test_pair_uf2.c`: the boot relay against the real relay
  and the real other half over the simulated split link: the whole flow (no
  session, no chord, host ops BUSY meanwhile, the record erased, only the slot
  and the record sector changed), the security invariant, stale images,
  damaged records, the other half refusing, absent or on protocol 2, the
  bounded boots, the fingerprint rules, a boot without USB, sides, a power cut
  mid-relay, and `make_update.py`'s file copied as the boot ROM does and then
  booted.
- `tests/sval_updater/test_make_update.py`: `pair` and `verify-pair`, the
  layout (every group of 16 block numbers one sector), a model of the boot
  ROM's UF2 path in file order and in shuffled orders, that a layout without
  whole sectors breaks it, an interrupted copy, refusals.
- `tests/sval_updater/test_release_artifacts.py`: the release lint of the two
  pair UF2 files.

On the test boards (right half with USB, left half behind the split cable),
no-chord dev builds with `SVAL_HOST_BOOTLOADER` so the half could be put in
BOOTSEL from the host, the file copied onto `RPI-RP2` from Windows:

| Test | Result |
|---|---|
| Per-side left and right builds (fw 2401, no side records), the pair UF2 (fw 2402) copied onto the right half | Right on 2402 in 4 s, relay DONE at 26 s, left on 2402, presence match at 32 s; right side R (migrated), left side L |
| A restart of the right half afterwards | No relay: the record was used up |
| A pair UF2 (2403) whose record has one manifest byte flipped | Right on 2403, no relay, left stays on 2402 |
| An interrupted copy (the record sector of 2404 and 100 image blocks), then a whole plain UF2 of 2403 | The half stayed on `RPI-RP2` after the partial copy; after the whole one, no relay, left stays on 2402 |
| Over USB: a 2404 image carrying a valid record for itself, staged and left uncommitted, then a restart | No relay |
| Over USB: the same image installed on the right half | Right on 2404, no relay, left stays on 2402 |
| Keybard's path: `verify-pair --svup` of the 2405 pair UF2 (identical to the build's own `.svup`), `sval_update.py pair` | Both halves on 2405 with one approval, presence match; sides kept |
| The 2405 pair UF2 copied onto the right half again | Relayed again (DONE at 24 s), both on 2405, presence match |
| The left half's side cleared (Sval `0x33` CLEAR), then the 2405 pair UF2 copied onto the right half | Relayed (DONE); afterwards the left half still has no side: the right half reports it (other side `FF`, pair `2`) and drops keys and pointing (flags bit 0). Setting the left side again brought both back |

Not run on hardware: the other half unplugged or without the updater (nobody
was at the boards to unplug it), and the error light's colors (not observable
remotely).

## Open questions

- **The error light** is settled: red only for a refused, damaged or
  tried-too-often record, 30 s, and none for a kept record (see Lights).
- **Keybard's reporting** of a kept record: the protocol is unchanged, so
  Keybard cannot read the record from the board; it sees the halves'
  versions differ (presence) and can offer the install.
- **The same release, another keymap** (sval on one half, blank in the file):
  the relay always installs the file's image, even when presence already
  matches, so the other half ends on the file's keymap. It costs one relay
  when the file was copied onto both halves by hand.
- **`fw_version.txt`** is not raised here; the x4 release commit raises it.
