# Updater protocol 4: the shared case list

The cases both sides run against the contract in
[docs/updater-protocol4.md](../../docs/updater-protocol4.md). The firmware runs
them here, in C against the real state machine and the simulated other half
(`test_pair.c`, built by `util/updater_test/run.sh`), and in Python against
`sval_update.py` (`test_sval_update_tool.py`, `test_no_chord_tool.py`).
Keybard runs the same IDs against its `FakeSvalboard` mock
(`tests/updater/protocol4-cases.md` there). Add a case to both lists, with the
next free ID, never renumber one.

| ID | Case | Here |
|---|---|---|
| P4-01 | INFO page 0 reports protocol 4; flags bit7 clear on a normal build | `test_p4_single` |
| P4-02 | A single ARM (mode 0, or no mode byte) is protocol 3's: flags 0, no pair hash, blue light, BEGIN needs the chord; mode 2 and up is INVALID and changes nothing | `test_p4_single` |
| P4-03 | PAIR_MANIFEST refusals change nothing: no session (INVALID), beside this half's own session (INVALID), `[hand]` = the other hand (INVALID), another client (OTHER_CLIENT), out of order (OUT_OF_ORDER), n of 0 or above 52 or past 108 bytes (INVALID), after ARM (BUSY) | `test_p4_pair_refusals` |
| P4-04 | A pair ARM needs the whole pair manifest (INVALID, still loading) and a session for the other hand (mode 1 on this half's own session: INVALID); offset 0 restarts the pair manifest | `test_p4_single`, `test_p4_pair_refusals` |
| P4-05 | A pair ARM refused for the pair manifest (its epoch below this half's floor: EPOCH; its structure: BAD_IMAGE) latches ERROR with that status before any chord is asked for, and holds no approval | `test_p4_pair_refusals` |
| P4-06 | A pair ARM refused for the other half's manifest latches ERROR, as a single ARM does | `test_p4_pair_refusals` |
| P4-07 | The pair ARM reply: nonce, 4 bytes of each manifest's SHA-512, flags PAIR; blue until the chord; nothing held before the chord, an approval held from the chord on | `test_p4_pair_one_chord` |
| P4-08 | One chord for both halves: after the relay is DONE the approval waits (STATUS byte 23 bit0); this half's ARM with that manifest is pre-approved (flags PRE_APPROVED and BY_PAIR, STATUS bit1, rainbow with no blue and no white flash), BEGIN is ACCEPTED at once, the install commits, and presence matches. The approval is used once: the next ARM needs the chord | `test_p4_pair_one_chord`, P4-25 |
| P4-09 | This half's ARM with another manifest gets no pre-approval (flags 0, NOT_CONFIRMED until the chord) and uses the approval up | `test_p4_held_rules` |
| P4-10 | A MANIFEST from another client, or for the other hand, clears the approval | `test_p4_held_rules` |
| P4-11 | REBIND with the pair nonce in IDLE moves the approval to the new client (a wrong nonce: INVALID); the old client's install then gets no pre-approval; a REBIND while this half's manifest loads moves that load too | `test_p4_held_rules` |
| P4-12 | ABORT clears it: in IDLE between the sessions, and of the pair session itself | `test_p4_held_rules` |
| P4-13 | An error clears it: the chord window passing, the split cable pulled mid-relay | `test_p4_held_rules` |
| P4-14 | The timeout: still held 90 s after DONE, gone by `SVAL_UPDATE_PAIR_HOLD_MS` (120 s, at most 180 s) | `test_p4_held_rules`, P4-25 |
| P4-15 | A reset of the half with USB clears it (RAM only) | `test_p4_held_rules` |
| P4-16 | REBIND while the other half commits moves the session and the approval; other ops are still BUSY | `test_p4_held_rules` |
| P4-17 | SETTINGS_RESET scope 1 in IDLE: the other half first, then this one (done `0x03`); a second one is BUSY; both halves reset within 500 ms | `test_p4_settings_reset`, hardware |
| P4-18 | SETTINGS_RESET is BUSY in a session, with an approval held, and while a relay runs; nothing changes | `test_p4_settings_reset` |
| P4-19 | SETTINGS_RESET with the other half silent (no presence, or no answer): UNAVAILABLE, nothing changed, no reset | `test_p4_settings_reset`, hardware |
| P4-20 | SETTINGS_RESET INVALID: a bad magic, scope 2, the other hand, unwrapped | `test_p4_settings_reset` |
| P4-21 | SETTINGS_RESET scope 0: this half only (done `0x01`) | `test_p4_settings_reset`, hardware |
| P4-22 | SETTINGS_RESET with the other half on protocol 3 (the split op unknown there): UNAVAILABLE, nothing changed. Also: a busy DMA channel there is retried; a record that cannot be written gives FLASH_ERR with the done mask of the halves that took it | `test_p4_settings_reset` |
| P4-23 | A no-chord dev build: INFO bit7; a single ARM is PRE_APPROVED and NO_CHORD with no blue; a pair ARM holds the approval from the ARM on; this half's install then has PRE_APPROVED, BY_PAIR and NO_CHORD; another manifest has PRE_APPROVED and NO_CHORD only | `test_pair_no_chord`, `test_no_chord_tool.py`, hardware |
| P4-24 | A release build with `SVAL_UPDATE_NO_CHORD` (or `SVAL_DEV_KEEP_SETTINGS`) fails at three gates: `rules.mk`, the `#error` guards, and release CI (`check_release_elf.py`, `make_update.py --release`) | `run.sh`, `test_make_update.py`, the WSL builds |
| P4-25 | `sval_update.py pair` on protocol 4: one chord, both codes shown at it, PAIR_MANIFEST carries this half's manifest, this half pre-approved by the pair, REBIND after every renewal (between the sessions too); with the approval gone (cleared, or timed out) it stops after the first half and never asks for a second chord | `test_sval_update_tool.py` |
| P4-26 | `sval_update.py pair` on a protocol 3 board: two chords, no PAIR_MANIFEST | `test_sval_update_tool.py` |
| P4-27 | `sval_update.py reset-settings`: scope 1 (or `--this-half`), waits for the board, reports the storage notice, `--ack` sends `0x2A`; refused on protocol 3; UNAVAILABLE with nothing changed when the other half is silent | `test_sval_update_tool.py`, hardware |
| P4-28 | SETTINGS_RESET mode 1 (D44): both halves take mode 1, the other half first (done `0x03`), both reset; no mode byte or mode 0 is the reset to defaults; scope 0 with mode 1 is this half only; mode 2 and up is INVALID; the other half on a protocol 4 build from before D44: UNAVAILABLE for mode 1 with nothing changed, mode 0 still works; the half with USB on a build from before D44 reads no mode byte, so mode 1 is its reset to defaults (the host's read-back catches it); `sval_update.py reset-settings --blank` sends mode 1 and reads every key back as KC_NO | `test_p4_blank_reset`, `test_sval_update_tool.py`, hardware |
| P4-29 | The blank mark (D44): mode 1's page clears the record and marks it over any record; mode 0's page zeroes the mark, so an unfinished blank reset becomes a reset to defaults; a mark over a zeroed mark area, over a valid record, or half a mark never counts | `test_build_reset.c` |
| P4-30 | A release build refuses an image whose own build-info record says no-chord (`0x40`) or kept settings (`0x80`) with BAD_IMAGE at the image check, before any commit, though its manifest has no DIAGNOSTIC (D45): this half's image; the other half's image on the half with USB (the held approval is cleared); and on a release half without USB behind a dev half with USB (the relay fails with BAD_IMAGE). A chord or release record, no record, or another `info_ver` is taken; a dev board takes a no-chord image; a record split across reads is found | `test_p4_image_build_check`, `test_release.c` |
| P4-31 | Keybard's layout choice before an install has three options: Keep my layout (the default: back up, install, restore), Reset to the default layout, and Wipe to a blank board (no backup needed; SETTINGS_RESET scope 1 mode 1 after the install, whatever bit 2 says; the keymap read back blank; `0x2A` last) | Keybard only |
| P4-32 | A pair update whose other half does not answer within 10 s after its commit stops before the half with USB (F9, Halves differ): that half is never armed and keeps its old release, and the halves-differ guidance follows. `sval_update.py pair` keeps going as before (a release that changes the split table may not answer an old half) | Keybard only |
| P4-33 | Faster uploads, CHUNK window: INFO byte 26 reports the window (bit0) and BEGIN reuse (bit1), byte 27 the window (16); a CHUNK missing from a window makes the rest OUT_OF_ORDER with the next offset and changes nothing; the host resends from there and END verifies; a repeat is acknowledged | `test_session_info_and_wrapper`, `test_fast_upload`, hardware (`fu_fault.py`) |
| P4-34 | Faster uploads, BEGIN reuse: after a session verified the image in the slot, BEGIN with flags bit0 and the same manifest skips the erase and stages every byte; END verifies the slot again and COMMIT installs it. Not reused: no flag, another manifest, a slot write since, a reset, per-hand manifests. A slot changed under the record is refused by END's SHA-512 and nothing is committed. A pair update of one image for both halves installs the relayed image on the half with USB with one upload | `test_fast_upload`, hardware (`fu_fault.py`) |
| P4-35 | Faster uploads, block erase: INFO byte 26 bit2; BEGIN with flags bit1 erases the slot in whole 64 KiB blocks from offset 0 and 4 KiB sectors for a tail shorter than a block, never past the image's last sector or outside the slot; STATUS says ERASING (CHUNK and END BUSY) throughout; ABORT lands between blocks. BEGIN without the flag (any older host) erases sectors only, as before | `test_session_block_erase`, hardware (`tp_install.py`) |
