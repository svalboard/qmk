# Updater protocol 4: the contract between Keybard and the firmware

This page is the byte-level contract for updater protocol 4 (D41-D43 in
[updater-plan.md](updater-plan.md)): one chord for both halves, the settings
reset, and no-chord dev builds. The firmware implements it in
`keyboards/svalboard/updater/` and `sval_update.py`; Keybard implements it in
its updater client and its mock board. Neither side changes it without changing
this page and telling the other. Everything not listed here is protocol 3, as
[updater.md](updater.md) and the comment at the top of
`keyboards/svalboard/updater/updater.c` describe it.

## Framing (unchanged)

- One 64-byte raw HID report per request:
  `[0xDD][client_id u32 LE][0xFE][0x07 set | 0x08 get][0x55][op][value_data: 55 bytes]`.
  The reply is the same packet with `value_data` rewritten. `value_data[0]` is always the status.
- Below, `req[i]` and `rsp[i]` index `value_data`. `req[0]` is always `[hand]`.
- Hands: 0 = left, 1 = right, 0xFF = "this half" (INFO only). All multi-byte fields are
  little-endian.
- Status codes are unchanged: OK 0, INVALID 1, UNAVAILABLE 2, FLASH_ERR 3, ACCEPTED 4, BUSY 5,
  NOT_CONFIRMED 6, BAD_HASH 9, BAD_IMAGE 10, EPOCH 11, OUT_OF_ORDER 12, OVERRUN 13, TOO_LARGE 14,
  TIMEOUT 15, OTHER_CLIENT 16, UNSUPPORTED 17, STORAGE 18.
- States are unchanged: IDLE 0, MANIFEST_LOADING 1, CONFIRM_WAIT 2, VERIFYING_MANIFEST 3,
  ERASING 4, RECEIVING 5, VERIFYING_IMAGE 6, VERIFIED 7, COMMITTING 8, ERROR 9, RELAYING 10,
  RELAYED 11, SUBSIDE_COMMITTING 12.
- Presence (STATUS `rsp[22]`) is unchanged: NONE 0, MATCH 1, VERSION 2, SAME_HAND 3, INVALID 4.

## Version gate: updater protocol 4

- `UPDATE_PROTOCOL_VERSION` becomes **4**. Protocol 4 is protocol 3 plus everything on this
  page: PAIR_MANIFEST, pair ARM, the held approval, SETTINGS_RESET, the ARM reply flags,
  STATUS `rsp[23]`, and the INFO no-chord bit. Report and piece sizes are unchanged (64 / 52 / 50).
- **Keybard's install flow requires INFO page 0 `rsp[2] == 4` on the half with USB.** Any other
  value (vLaunch2 and Vial have no updater; vNext1 is protocol 3) takes the manual install (M).
  There's no separate capability byte: the protocol number is the one gate.
- The half without USB may report protocol 3 or 4 through the relay. The relay itself is
  unchanged. A pair update from Keybard needs both halves on protocol 4 after the update, which is
  always true for a carried release.
- `sval_update.py` speaks protocols 3 and 4. On 4 it uses the pair approval for `pair`.

## INFO page 0 (op 0x00): one new flag bit

`rsp[21]` flags: bit0 16 MiB die · bit1 settings writes failing · bit2 release build ·
bit3 test hooks · bit4 reserved · bit5 the other half's version differs ·
bit6 reserved · **bit7 no-chord developer build (D43)** (as already on `dev/autoflash`).
Everything else in page 0 and page 1 is unchanged.

The other half's INFO (hand = the other side) still carries flags bits 0-3 only. Keybard reads
the no-chord bit from the half with USB, since that's the half that would skip the chord.

## PAIR_MANIFEST (new op 0x0D): the USB half's own manifest, for one approval

- Request: `[hand = this half's own hand][off u8][n u8 1..52][n bytes]`, wrapped.
- Reply: `[status][filled u8]` (bytes of the 108-byte pair manifest loaded so far).
- Allowed only while a session **for the other hand** is in MANIFEST_LOADING, from that
  session's client. Otherwise INVALID (no session), OTHER_CLIENT, or BUSY (any later state).
  Refusals change nothing.
- Order: `MANIFEST(other) x3`, then `PAIR_MANIFEST(self) x3`, then `ARM(other, pair)`.

## ARM (op 0x02): new mode byte and reply flags

- Request: `[hand][mode u8]`. `mode 0` = single, as in protocol 3 (a missing byte reads as 0).
  `mode 1` = pair: only with hand = the other side, and with all 108 pair-manifest bytes loaded
  (else INVALID).
- A pair ARM checks the other half's manifest against the other half (as today) **and** the pair
  manifest against this half's device and floors (`update_manifest_check` with
  `update_device_self`). Either failure latches ERROR with that status, as a single ARM does
  today. So the user is never asked to approve a file that either half would refuse.
- Reply: `rsp[1..4]` nonce u32 · `rsp[5..8]` the first 4 bytes of SHA-512(the session's 108-byte
  manifest) · `rsp[9..12]` the first 4 bytes of SHA-512(the pair manifest) (mode 1; zeros
  otherwise) · `rsp[13]` **ARM flags**:
  - bit0 `PRE_APPROVED`: BEGIN will be accepted with no chord, and no blue light is shown;
  - bit1 `BY_PAIR`: pre-approved by a held pair approval (see "The held approval");
  - bit2 `NO_CHORD`: pre-approved because this is a no-chord developer build;
  - bit3 `PAIR`: this ARM is a pair ARM, and the chord approves both manifests.
- LEDs (firmware's job; Keybard's light swatches assume it): a normal ARM blinks blue at 2 Hz until the chord.
  A pre-approved ARM (bit0) shows no blue and no white flash: the light goes straight to the rainbow.

## The held approval (D41)

- **Created** when the chord is made in a pair session. The USB half stores
  SHA-512(pair manifest), the session's client ID and the session nonce, in RAM only. On a
  no-chord build it's created at the pair ARM, since the chord counts as made.
- **Used** by this half's own single ARM (`hand = self`, mode 0) whose 108-byte manifest has
  that exact SHA-512, from the bound client: the ARM reply has bit0 and bit1 set, state
  CONFIRM_WAIT with the chord counted as made, and BEGIN returns ACCEPTED at once. The approval
  is then cleared (used once).
- **Moved** by REBIND: in any state, including IDLE between the two sessions, `REBIND [nonce u32]`
  with the pair session's nonce moves the held approval to the sender's client ID. Keybard
  renews its client ID every 50 s, so it must REBIND right after each renewal, as it does inside
  a session.
- **Cleared** by ABORT, by any ERROR in either session, by a reset of the USB half, by a
  manifest from a client other than the bound one, and by a timeout: the firmware keeps it for
  **at least 90 s** after the relay reaches DONE (this firmware keeps it 120 s; never more than 180 s).
  Keybard must send this half's ARM within 60 s of DONE.
- A one-half update (Advanced, or `Finish update`) uses a single ARM: one chord, as today.
- **STATUS `rsp[23]`** (new) flags2: bit0 an approval is held for this half's next install ·
  bit1 the current session was pre-approved. Bytes 0-22 are unchanged.
  (`rsp[21]` bit0 "chord made" still means what it does in protocol 3.)

## SETTINGS_RESET (new op 0x0E) (D42)

- Request: `[hand = this half's own hand]['R' 'S' 'E' 'T'][scope u8: 0 this half, 1 both halves][mode u8]`,
  wrapped. Keybard always sends scope 1. `mode` (D44): 0 the firmware's defaults (a missing
  byte reads as 0), 1 a blank board.
- Allowed only in IDLE with no held approval (else BUSY). A bad magic, scope or mode gives INVALID.
  With scope 1 and the other half absent or refusing, it gives UNAVAILABLE and nothing changes.
  If the other half was present but its answers were lost, it gives TIMEOUT: the other half
  may have taken the request and will reset, and this half changed nothing. A host that
  gets TIMEOUT sends SETTINGS_RESET again once the board is back.
- Reply: `[status][done u8: bit0 this half, bit1 the other half]`. On OK, each half has
  invalidated its build reset record (D39). The other half is told over the split link first.
  Within 500 ms of the reply, both halves reset. On the next boot of each half, the D39 path
  resets **every** stored setting (keymap, macros, combos, tap dances, overrides, QMK settings,
  and the keyboard's VIA custom values: layer colors, pointing and scan settings), exactly as for
  a different build. Identity (serial and board name) and the updater state are kept.
- **Mode 1, a blank board (D44).** The same reset, then every key on every layer is `KC_NO`
  (`0x0000`), on both halves. There are no combos, tap dances, macros, key overrides, leader
  sequences or alt-repeat keys, as after any reset, and the layer colors, pointing and scan
  values are the firmware's defaults. Each half marks its build reset record for a blank
  board (8 bytes after the record, in the same page program), so the request survives the
  reset: the next boot resets to defaults, writes `KC_NO` over the keymap, then records the
  image. A power cut before that record write leaves the mark, so the next boot blanks again.
  The other half gets the mode over the split link (KEYBOARD_UPDATE `0x07` with a fifth byte).
  A half on a protocol 4 build from before D44 answers that INVALID, so the request is
  UNAVAILABLE and nothing changed on either half.
- **A half with USB on a protocol 4 build from before D44 does not read the mode byte.** It
  takes mode 1 as the reset to defaults, on both halves (the other half gets protocol 4's
  4-byte payload). Nothing in the reply tells the two apart, so a host reads the keymap back
  after the reset: `sval_update.py reset-settings --blank` fails, and Keybard stops on F20.
  Keybard always sends mode 1 to the build it has just installed.
- After the reconnect, Sval GET_INFO storage flags (`svilInfo[17]` in Keybard, response byte 18
  on the wire) has **bit 2 set**, until Keybard sends `0x2A`. This is the same notice a new build gives.
- The USB product ID doesn't change, so Keybard reopens the device with no chooser.

## Developer builds (D43)

- `SVAL_UPDATE_NO_CHORD=yes`: every ARM counts as confirmed. INFO flags bit7 is set, and ARM
  replies set bit0 and bit2. Refused with a release build at three gates: a `rules.mk`
  `$(error)`, `#error` guards, and release CI (`check_release_elf.py`, `make_update.py --release`).
- Its build-info record (magic `SVBI`, 32 bytes in the image's `.rodata`) has flag
  `UPDATE_BUILD_NO_CHORD 0x40` at byte 5. Keybard detects a no-chord image in a chosen file by finding
  the record (`"SVBI"`, `info_ver` 3 at byte 4) in the image and testing byte 5 & 0x40.
  Images of a no-chord (or keep-settings) build are always flagged DIAGNOSTIC (manifest flags bit1):
  `make_update.py` sets the flag itself when the build-info record has 0x40 or 0x80, so a release
  board refuses them however they were packed. Keybard shows that refusal as a check row (EPOCH-like refusals → the sheet).
- **The image is checked too (D45).** A release build reads the build-info records in the image
  itself while it hashes it (VERIFYING_IMAGE, on the half with USB for either half's image, and
  again on the half without USB when the image is relayed to it). It refuses an image whose record
  has `0x40` (no chord) or `0x80` (kept settings) with BAD_IMAGE, whatever the manifest's flags
  say, so a hand-made manifest without DIAGNOSTIC cannot put a no-chord image on a release board.
  The refusal comes after the chord (the image arrives after ARM) but before anything is
  committed, and it clears a held approval like any error. A record is matched as `make_update.py`
  matches it (`"SVBI"`, `info_ver` 3); an image with no record (Vial, vLaunch2) is taken. This
  stops files made from dev builds by the tools or by hand-made manifests. It cannot stop someone
  who patches the record out of a dev image on purpose: the chord is what stops that.

## The flow Keybard runs (the contract's sequence)

Both halves, USB on half U, the other half O. Every op is wrapped with Keybard's current client
ID. After any client-ID renewal, the next op is REBIND (nonce) whenever a session or a held
approval exists.

```
pre:   INFO(0xFF) page 0: protocol == 4, bit0 set, bit1 clear, state IDLE
       (state ERROR: send ABORT, then re-read INFO)
       INFO page 1: tag of U.  STATUS: presence MATCH or VERSION within 5 s.
       INFO(O): state != 0xFF (else offer "update U only")
       pre-check both .svup manifests against both INFOs (floors, size, release vs DIAGNOSTIC,
       hand and pointing warnings), and check SHA-256 against the carried pins
       backup (Keybard only), Host paused, Scan Lab closed, Web Lock held
O:     MANIFEST(O) x3 -> PAIR_MANIFEST(U) x3 -> ARM(O, mode 1)
       if !(flags & PRE_APPROVED): show the approval (U blinks blue); poll STATUS every 250 ms
         until rsp[21] bit0 (<= 35 s; ARM's 30 s window -> NOT_CONFIRMED latched -> F4)
       BEGIN(nonce) -> ACCEPTED; poll STATUS until RECEIVING (erase)
       CHUNK [off u24][n][<=50 B] in order until staged == image_len (OUT_OF_ORDER: resync to rsp offset)
       END(nonce) -> poll until VERIFIED; STATUS rsp[19..20] == Keybard's CRC-32/MPEG-2 low 16 bits
       COMMIT(nonce, crc16) -> RELAYING; poll RELAY until RELAYED
       COMMIT(nonce, crc16) -> SUBSIDE_COMMITTING; poll STATUS until IDLE (~4.5 s hold + probes)
       poll INFO(O) until it answers (<= 10 s); presence VERSION is expected here
       no answer in 10 s: stop here, before U (F11). U keeps its old release and works;
         the halves-differ guidance follows (decision of 2026-10-15)
U:     STATUS rsp[23] bit0 must be set (else F9 "Halves differ" -> Finish update with its own chord)
       MANIFEST(U) x3 -> ARM(U, mode 0): expect flags bit0|bit1 -> BEGIN at once
       CHUNKs -> END -> VERIFIED, CRC check -> COMMIT -> the device disconnects within ~2 s
after: reconnect (same PID: navigator.hid connect / getDevices, up to 30 s)
       INFO page 0 both halves: fw_version == both manifests; INFO page 1 tag; presence MATCH <= 10 s
       Sval GET_INFO storage flags:
         "Put my layout back": import preflight + syncWithKeyboard, whatever bit 2 says
         "Reset to the default layout": if bit 2 is clear, or only one half was installed:
            SETTINGS_RESET(scope 1) -> reconnect again -> bit 2 now set
         "Wipe to a blank board" (D44): always SETTINGS_RESET(scope 1, mode 1) -> reconnect
            again -> bit 2 now set -> read the keymap back: every key KC_NO. No backup is
            needed before the install for this choice
       send 0x2A last, once the restore succeeded or the defaults are confirmed
```

A one-half update is the same, without the pair parts: for O, a single ARM(O) with one chord;
for U, a single ARM(U) with one chord (or pre-approved on a no-chord build).

## Timing assumptions both sides hold to

- Every op answers within 50 ms (Keybard times out at 1000 ms).
- The session timeout is 30 s without an op. Keybard polls STATUS at 250 ms or faster while a
  session exists. The CHUNK loop is promise-driven, with no timers.
- The chord window is 30 s from ARM. The held approval lasts at least 90 s after relay DONE.
- SETTINGS_RESET resets within 500 ms. The board is back within 10 s (Windows: up to 30 s).

## The mock board and the host tests prove the same contract

Keybard's `FakeSvalboard` (TypeScript) implements everything above, including the relay, the held
approval and its clearing rules, SETTINGS_RESET with a simulated reconnect, latched errors, and
injectable faults (unplug at each stage, NOT_CONFIRMED, BAD_HASH, OTHER_CLIENT, a lapsed held
approval, the other half silent). The firmware's host tests (`tests/sval_updater/`) cover the same cases in C
against the real state machine. **The shared case list** lives in the contract file each side
commits beside its tests: `tests/sval_updater/PROTOCOL4_CASES.md` (firmware) and
`tests/updater/protocol4-cases.md` (Keybard), with the same IDs (P4-01 ...). Each side runs every case.

## What this firmware does beyond the minimum

Additions that keep within the contract, which Keybard may rely on:

- **The held approval lasts 120 s** after the relay reaches DONE
  (`SVAL_UPDATE_PAIR_HOLD_MS`, `updater.h`).
- **Any ARM for the half with USB uses the approval up**, whether or not its
  manifest matches. A host that sent the wrong file gets the chord prompt (no
  PRE_APPROVED flag) and must not try again for a pre-approval: it stops on F9.
- **REBIND is answered in SUBSIDE_COMMITTING** too (protocol 3 answered BUSY),
  so a client ID renewed while the other half commits keeps the session and the
  approval. Keybard may still REBIND only after IDLE.
- **A REBIND with the pair nonce while this half's manifest loads** moves that
  load to the new client as well.
- **SETTINGS_RESET statuses**: OK; INVALID (bad magic, scope above 1, mode above
  1, the other hand, or unwrapped); BUSY (a session, a held approval, a relay, a reset
  already pending, or the other half in a session of its own, or its DMA busy
  for six tries in a row); UNAVAILABLE (no presence MATCH or VERSION, a refusal
  from the other half, or a half on protocol 2 or 3 there, which does not know
  the split op; nothing changed); TIMEOUT (no answer, or only damaged ones, from
  the other half: it may have taken the request and reset; this half changed
  nothing); FLASH_ERR (a record could not be written: the
  done mask says which half already took it). The done mask is `0x03` for both
  halves, `0x01` for scope 0.
- **The other half resets 300 ms after it takes SETTINGS_RESET**, this half
  100 ms after its reply. On the test boards the half with USB was back on USB
  5.3 s after the reply.
