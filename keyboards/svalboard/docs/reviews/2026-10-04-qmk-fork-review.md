# Svalboard QMK fork review

Review date: 2026-10-04. Reviewed firmware: [`9109ac8034`](https://github.com/morganvenable/sval-qmk/commit/9109ac8034fb61b22e3488b03d9297ac54f673fc). Baseline: upstream QMK merge base [`7a1bbf37c5`](https://github.com/qmk/qmk_firmware/commit/7a1bbf37c5c07da4ea0a162bb139083a46ef40a1). The local `upstream/master` points to that same commit. This review covers the local fork delta, not a comparison with a newly fetched upstream revision or an audit of the separate Keybard repository.

The design keeps most dynamic behavior in community modules and preserves the upstream engines, which is a good direction. Layout stamps, explicit keycode versions, overflow-safe macro bounds, and a persisted migration intent address real update and recovery problems. However, I would fix the privileged PR workflow, reset/migration data-loss paths, and disconnected runtime timing controls before the formal release. Successful compilation does not establish that editor settings change keyboard behavior.

After the second-pass audit, the review records **19 actionable issues: one P1, fifteen P2, and three P3**. P1 requires prompt attention; P2 is a concrete correctness problem with the trigger described below; P3 is a lower-priority correctness, hardening, or documentation issue. “Native probe” means the production C function was compiled on the host with mocked dependencies; it does not mean the issue was observed on a physical board. No production firmware changes were made during this review.

## Second-pass audit corrections

This revision audits the [original review](https://github.com/morganvenable/sval-qmk/blob/4b57f238f3ed3ffa00eb44cf373e54a2406baaf7/keyboards/svalboard/docs/reviews/2026-10-04-qmk-fork-review.md) against the same firmware revision. It corrects these material points:

- **Missed recovery failure, R19:** whole-keycode idempotence does not make two separate byte writes atomic. A new probe interrupts a keycode upgrade between writes; the next boot retains the corrupt value and marks the upgrade complete. The original positive recovery assessment was too broad.
- **Incorrect alternate-repeat interpretation, R14:** `allowed_mods` permits extra modifiers; it does not require them. The original reverse-only diagnosis and proposed reuse of the forward predicate were wrong. Both predicates and the default-alternate behavior need correction.
- **Reduced priority, R06 and R07:** malformed names require invalid raw protocol input; current Keybard uses `TextEncoder`. Current Keybard also renews IDs after at most 50 seconds and retries an expired session. These remain firmware defects, now P3, without demonstrated disruption in ordinary current-editor use.
- **Stronger evidence, R15 and R16:** preprocessing the maintained build confirms the missing timing gates; a complete valid-then-invalid-JSON incremental firmware build succeeds twice, confirming stale generation beyond the original miniature Make probe.
- **More precise limits:** the path ledger is an inventory, not proof of exhaustive behavioral coverage. The reset probe now models a post-reset edit being erased, and the identity probe starts with consistent RAM and visible names. Neither substitutes for hardware fault injection.

The remaining original findings retain their stated source-level basis and conditional triggers. The two existing storage tests still pass; the expanded 13-group diagnostic probe run also passes its assertions of faulty behavior. The original release build matrix is retained as earlier evidence, not represented as rerun during this audit. No firmware fixes are included.

## Scope and evidence

The complete delta is **143 files, 12,936 insertions, and 200 deletions**. Added board/module code is part of the comparison even when it had no upstream predecessor. [The coverage ledger](2026-10-04-coverage.csv) records every changed path, its change type, and review category. It does not establish that every execution path was tested. The table below lists inspected areas; hardware-dependent behavior and unexercised branches remain outside the validation claims.

| Area | Changed files | Review coverage |
| --- | ---: | --- |
| QMK core | 16 | Core patches, API declarations, storage addresses, initialization ordering, and relevant upstream hook gates |
| Sval module | 22 | Protocol framing, table caches, keycode execution, settings, labels, fragments, generation, integration, and protocol documentation |
| PS2 module | 4 | Streaming/remote paths, packet conversion, button state, configuration, and driver hooks |
| Board and keymaps | 88 | Matrix scanning, pacing, power controls, identity, migration, pointing transforms, split RPC, variants, definitions, keymaps, and flashing helper |
| Board documentation | 7 | User-facing claims, supported targets, update limits, and launch guidance |
| CI | 4 | Events, token privileges, checkout, build execution, artifact/release paths, and disabled upstream build workflow |
| Other | 2 | Ignore patterns and storage regression suite |

Validation performed:

- Existing suite: `python3 -m unittest discover -s tests/sval_storage -v` — **2 tests passed**. These compile production handlers with UBSan and mocked hardware; each includes multiple boundary/recovery scenarios.
- **All 12 release-matrix targets passed direct compile checks**: `sval` on both sides of base, TrackPoint, PMW3360, PMW3389, and Azoteq; `blank` on both base sides.
- A representative **non-Sval VIA build passed**: `qmk compile -kb handwired/onekey/rp2040 -km default -e VIA_ENABLE=yes`. This checks the shared core without the Sval module; it does not establish coverage for the entire upstream board set.
- **Three diagnostic targets passed**: base left/right `scanlab`, and PMW3389 right `scanlab`.
- Native characterization: [probe script](characterize-findings.py) and [observed output](2026-10-04-probes.txt). Thirteen probe groups reproduce the issues listed in that output. Their assertions describe the observed defects at the reviewed revision; they are diagnostic evidence, not correctness regressions to preserve after fixes.
- Maintained definitions pass the supplied fragment-schema validator, compress successfully, and fit their current 16-bit definition-chunk offset. The board layout contains 60 distinct, in-range matrix positions.
- Negative build checks reproduce omitted-feature linking failure and the missing-definition default-keymap failure. Both a Make-level check and a full incremental compile reproduce stale generation after invalid JSON.
- `bash -n keyboards/svalboard/tools/flash.sh` passes. The flashing helper was not used to flash a board.

An early Make failure can be printed as `[OK]` by QMK's mass-compile summary because the summary looks for particular error markers. I used the direct compile exit codes for the pass/fail statements above. Build logs also contain one clock-skew warning; hardware execution and a clean release-container rebuild remain separate checks.

## Findings

| ID | Priority | Issue |
| --- | --- | --- |
| R01 | P1 | [Run untrusted PR builds with a read-only token](#r01) |
| R02 | P2 | [Make a protocol reset valid before accepting new edits](#r02) |
| R03 | P2 | [Preserve migrated magic options when initializing Sval settings](#r03) |
| R04 | P2 | [Do not introduce macro terminators during legacy keycode translation](#r04) |
| R05 | P2 | [Roll back an unsuccessful board-name save](#r05) |
| R06 | P3 | [Validate Unicode scalar values in board names and USB strings](#r06) |
| R07 | P3 | [Make client-session expiry match the advertised TTL](#r07) |
| R08 | P2 | [Retain PS2 button state between streaming packets](#r08) |
| R09 | P2 | [Flush pending scroll movement on idle pointer frames](#r09) |
| R10 | P2 | [Exit the mouse layer when the host disables auto mouse](#r10) |
| R11 | P2 | [Widen pointer-scaling arithmetic and saturate reports](#r11) |
| R12 | P2 | [Prevent stacked sniper factors from wrapping](#r12) |
| R13 | P2 | [Release the keycode that an active tap dance actually pressed](#r13) |
| R14 | P2 | [Restore alternate-repeat modifier and default semantics](#r14) |
| R15 | P2 | [Enable the compile gates for advertised runtime timing controls](#r15) |
| R16 | P2 | [Fail builds when definition generation fails](#r16) |
| R17 | P2 | [Provide disabled-feature stubs when omitting optional modules](#r17) |
| R18 | P3 | [Remove or repair the documented default build command](#r18) |
| R19 | P2 | [Recover interrupted multi-byte keycode upgrades](#r19) |

<a id="r01"></a>

### R01 P1 Run untrusted PR builds with a read-only token

Sources: [.github/workflows/release.yml](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/.github/workflows/release.yml#L9), [.github/workflows/release.yml](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/.github/workflows/release.yml#L13), [.github/workflows/build-firmware.yml](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/.github/workflows/build-firmware.yml#L31).

The release workflow uses `pull_request_target` and `permissions: write-all`. Its reusable job checks out the PR head SHA with the repository token, then executes PR-controlled dependencies and build files through `pip`, `qmk setup`, and `make`. Checkout also retains credentials by default. When this workflow is enabled and allowed to run for an untrusted PR, that code runs with repository write privileges.

This is a confirmed unsafe workflow configuration, not evidence that the repository has been compromised. Repository-level event policies were not audited. [GitHub's security guidance](https://docs.github.com/en/actions/reference/security/securely-using-pull_request_target) explicitly describes this trust-boundary failure.

Use `pull_request` with `contents: read` for build jobs, disable credential persistence, and grant `contents: write` only to a trusted tag-release job. Verify the generated PR job permissions and fork behavior without executing an attack.

<a id="r02"></a>

### R02 P2 Make a protocol reset valid before accepting new edits

Sources: [modules/svalboard/core/sval.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval.c#L464), [modules/svalboard/core/sval.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval.c#L672), [modules/svalboard/core/sval.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval.c#L193).

`sval_cmd_reset` calls `sval_reset()`, which zeroes the entire Sval block, including its validity stamp. Neither that command nor the no-op `sval_save()` restores the stamp. A user can reset, configure new keys/macros/tables, receive successful replies, and reboot; `sval_init()` sees an invalid block and resets the keymap and macros again, discarding those edits.

The same reset leaves the label RAM caches untouched, so immediate label reads can return pre-reset labels even though EEPROM was cleared. The QMK settings RAM copy also remains unchanged until subsequent initialization.

A native probe of the production reset and init functions confirms two keymap resets and loss of a modeled edit made between reset and boot. Complete the reset transaction by applying settings defaults, refreshing all RAM caches, and writing the validity stamp last. Test reset → edit → save → reboot and immediate label/settings reads.

<a id="r03"></a>

### R03 P2 Preserve migrated magic options when initializing Sval settings

Sources: [keyboards/svalboard/migrate_vial.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/migrate_vial.c#L326), [keyboards/svalboard/migrate_vial.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/migrate_vial.c#L332), [modules/svalboard/core/sval_qmk_settings.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_qmk_settings.c#L394).

Migration copies the old core EEPROM settings, then calls `sval_qmk_settings_reset()`. That function clears `keymap_config.raw`, enables one-shots and the default NKRO state, and writes the result back to core EEPROM. It therefore overwrites the magic options just copied from the old firmware: modifier swaps, GUI disablement, Caps Lock remapping, and the previous NKRO/one-shot choices.

A probe using the real settings-reset function and QMK's bit-field type changes saved flags `0x0101` to `0x0480`. The existing migration test mocks this reset as an empty function, so it cannot detect the loss.

Separate Sval settings initialization from resetting core magic options, or restore the copied core options after initialization. Test migration with nondefault magic flags using the real reset logic.

<a id="r04"></a>

### R04 P2 Do not introduce macro terminators during legacy keycode translation

Sources: [keyboards/svalboard/migrate_vial.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/migrate_vial.c#L182), [keyboards/svalboard/migrate_vial.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/migrate_vial.c#L248).

An unassigned legacy custom keycode is intentionally translated to `KC_NO`. Inside a 16-bit macro extension, the encoding branch converts zero to `0xFF00`, whose low byte is still zero. Macro storage uses zero bytes as terminators, so this inserts a new macro boundary in the middle of the action. Subsequent macro IDs shift, and playback can stop early or interpret the remaining high byte as text.

The production translation probe converts `[01 05 14 7E 'X' 00 'Y' 00]` to an action containing `00 FF`. This requires an old macro containing an unsupported custom action; ordinary supported actions are unaffected.

Remove the entire unsupported action while repacking macro boundaries, or choose an explicitly harmless representable action with two nonzero encoded bytes. Verify that migration preserves the number and placement of macro terminators.

<a id="r05"></a>

### R05 P2 Roll back an unsuccessful board-name save

Sources: [keyboards/svalboard/identity.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/identity.c#L221).

`identity_set_name()` changes `current.name` and `current.name_len` before attempting the flash save. On failure it returns `IDENTITY_WRITE_FAILED` without rolling those fields back or updating `name_z`. Retrying the same name matches the already-mutated RAM record and returns `IDENTITY_OK` without another flash write. The visible name remains old, and a reboot restores the old persistent name.

The native probe confirms first status 3, retry status 0, only one save attempt, and the old visible name. Stage the record separately or restore the old record on failure; update the RAM and visible name only after a verified save. Exercise failed write → identical retry → successful save → reboot.

<a id="r06"></a>

### R06 P3 Validate Unicode scalar values in board names and USB strings

Sources: [keyboards/svalboard/identity.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/identity.c#L208), [tmk_core/protocol/usb_descriptor.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/tmk_core/protocol/usb_descriptor.c#L1218).

The name validator checks only UTF-8 byte shapes. It accepts overlong encodings, UTF-16 surrogate code points, and values above U+10FFFF. The runtime USB descriptor converter also lacks scalar-value checks, so those accepted names can produce NUL characters or malformed UTF-16 descriptors rather than the promised replacement characters.

The native probe accepts `C0 AF`, `ED A0 80`, and `F4 90 80 80`; all three are invalid UTF-8 text. Label validation in `sval.c` already rejects these classes correctly.

This requires malformed bytes sent through the protocol. The inspected [current Keybard name editor](https://github.com/svalboard/keybard/blob/61288ac4490d6c9fb211c765787f8398274d0ad0/src/services/identity.service.ts#L106) encodes JavaScript strings with `TextEncoder`, so ordinary name entry does not generate these invalid sequences. There is no demonstrated memory corruption or host enumeration failure here; P3 reflects that narrower impact.

Share a strict UTF-8 validator and apply equivalent checks in descriptor conversion. Test those invalid forms alongside BMP text, supplementary-plane characters, truncation, and maximum descriptor size.

<a id="r07"></a>

### R07 P3 Make client-session expiry match the advertised TTL

Sources: [modules/svalboard/core/client_wrapper.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/client_wrapper.c#L27), [modules/svalboard/core/client_wrapper.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/client_wrapper.c#L33).

IDs retain only the upper 16 timer bits, and validation rounds the current time to the same 65,536 ms boundary. The bootstrap advertises 120 seconds, but an ID issued near the end of a timer bucket expires after about 65.5 seconds; one issued at the start can last about 131.1 seconds. A client renewing according to the advertised TTL can receive an invalid-ID error much earlier than expected.

The probe issues an ID at 65,535 ms and finds it invalid at 131,072 ms, after only 65,537 ms. This is a protocol mismatch, but current Keybard already works around it: [its USB service](https://github.com/svalboard/keybard/blob/61288ac4490d6c9fb211c765787f8398274d0ad0/src/services/usb.service.ts#L353) renews at most 50 seconds after issuance and retries once after an invalid-ID response. That lowers the present release priority to P3. No ordinary-session failure in that client was reproduced.

Track issuance/expiry explicitly, or use a representation and advertised lifetime that guarantee the promised validity window. Test allocations across bucket boundaries and 32-bit timer wrap. Also skip reserved IDs when allocating.

<a id="r08"></a>

### R08 P2 Retain PS2 button state between streaming packets

Sources: [modules/svalboard/pointing_device_ps2/pointing_device_ps2.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/pointing_device_ps2/pointing_device_ps2.c#L144).

The streaming driver initializes both the outgoing report and PS/2 packet to zero on every poll. When `pbuf_has_data()` is false it still converts that zero packet and returns zero button bits. A held physical mouse button is therefore reported as released between packets. The selected TrackPoint configuration uses streaming mode.

The production driver probe sends a left-button packet, then an empty poll: button bits change from 1 to 0 while the physical button is still held. This can interrupt dragging and selection when buttons come from the PS/2 device; keyboard-generated mouse buttons are a separate path.

Keep the last physical button state until a valid packet changes it, and distinguish no packet/failed packet from an explicit button release. Test stationary holds, slow dragging, and incomplete packet delivery.

<a id="r09"></a>

### R09 P2 Flush pending scroll movement on idle pointer frames

Sources: [keyboards/svalboard/keymaps/keymap_support.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/keymap_support.c#L268), [keyboards/svalboard/keymaps/keymap_support.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/keymap_support.c#L333).

The combined pointer callback returns immediately whenever both XY reports are zero. That bypasses the scroll timer and accumulator flush. Movement arriving before the 10 ms deadline can stay buffered indefinitely once the user stops moving, and can appear during a later gesture instead.

A production-function probe buffers 9 wheel units, advances beyond the deadline, and supplies an idle frame. The output is zero and all 9 units remain buffered.

Run pending-scroll deadline handling even when the current frame has no new motion. Test a single short movement followed by idle frames, continuous movement, axis lock, and changing scroll mode with pending data.

<a id="r10"></a>

### R10 P2 Exit the mouse layer when the host disables auto mouse

Sources: [keyboards/svalboard/svalboard.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/svalboard.c#L659), [keyboards/svalboard/keymaps/keymap_support.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/keymap_support.c#L679).

The VIA setting handler changes `global_saved_values.auto_mouse` directly. If the mouse layer is active, setting that flag false leaves the layer active. Later `mouse_mode(false)` calls cannot remove it because that function wraps both activation and deactivation in `if (global_saved_values.auto_mouse)`. The keyboard toggle avoids this by turning mouse mode off before changing the flag; the host-setting path does not.

The probe sets an active mouse layer with the flag false and calls the real deactivation function: the layer remains set. Allow deactivation unconditionally, or deactivate before applying the host setting. Test disabling while active and while a mouse button is held.

<a id="r11"></a>

### R11 P2 Widen pointer-scaling arithmetic and saturate reports

Sources: [keyboards/svalboard/axis_scale.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/axis_scale.c#L46), [keyboards/svalboard/axis_scale.h](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/axis_scale.h#L29), [keyboards/svalboard/keymaps/keymap_support.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/keymap_support.c#L227).

Boost allows multipliers up to 255, while `axis_scale_t.remainder` and the function result are signed 16-bit values. `add_to_axis()` narrows the multiplied value into that remainder before producing a report. A normal positive delta can wrap into a negative result, making the cursor move backward at high boost or high sensor deltas.

The production arithmetic probe supplies +200 at multiplier 255 and gets −14,536. The mathematically correct +51,000 should be saturated to the report range, not wrapped. Wide reports are enabled for the maintained board.

Keep multiplication and remainders in a sufficiently wide signed type and clamp only at the report boundary. Cover both signs, maximum sensor deltas, combined reports, stacked boost, and scroll conversion.

<a id="r12"></a>

### R12 P2 Prevent stacked sniper factors from wrapping

Sources: [keyboards/svalboard/keymaps/keymap_support.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/keymap_support.c#L216).

The sniper divisor is accumulated in `uint8_t` with no overflow check. Holding and toggling each of the 2×, 3×, and 5× bindings computes 900 but stores 132. Other combinations wrap to zero; `set_div_axis()` ignores zero and leaves an unrelated previous divisor in force.

The native probe reproduces divisor 132 for the six normal hold/toggle states. Use a wider accumulator and an explicit maximum, or a representation capable of the supported stacked product. Test the complete normal toggle/hold combination space, plus duplicate bindings.

<a id="r13"></a>

### R13 P2 Release the keycode that an active tap dance actually pressed

Sources: [modules/svalboard/core/sval_tap_dance.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_tap_dance.c#L57), [modules/svalboard/core/sval_tap_dance.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_tap_dance.c#L112).

Dance finish reads the current stored entry and presses its selected output. Reset rereads the entry instead of remembering that pressed output. Editing A to B while A is held causes reset to release B and leave A held. Disabling the entry is worse: reset returns early without releasing anything.

The production callbacks reproduce A remaining held after changing the entry to B. Retain the resolved pressed output per active dance, or safely reset active dances before changing their entries. Test edit, disable, and reset during tap and hold states, including modifier outputs.

<a id="r14"></a>

### R14 P2 Restore alternate-repeat modifier and default semantics

Sources: [modules/svalboard/core/sval_alt_repeat_key.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_alt_repeat_key.c#L70).

The original review incorrectly called `allowed_mods` a requirement. In the [Vial reference implementation](https://github.com/morganvenable/vial-qmk/blob/dba7c729bdfaeb9ae7cafcd3134dc6ab12be111b/quantum/vial.c#L719), it permits additional held modifiers; required modifiers come from the binding's keycode. Sval migration copies this field unchanged. Sval's forward predicate instead requires every allowed bit and permits unlisted modifiers, while reverse matching ignores the mask altogether. Reusing the forward predicate in reverse, as originally suggested, would retain the wrong semantics.

The corrected probe uses `allowed_mods=0`: both A→B and B→A fire with Ctrl held even though Ctrl is disallowed. Allowing Shift then wrongly prevents unmodified A from matching. This can change the meaning of modified repeats, including migrated bindings.

The default-alternate option is also misimplemented: a matching entry returns its original key instead of its alternate, and an unrelated key never uses the entry as a fallback. The probe confirms both cases. The reference chooses the alternate when no more specific mapping matches and the modifiers are allowed.

Restore the complete matching contract: normalize modifier-bearing keycodes, distinguish required from permitted modifiers, apply handedness consistently to masks and inputs, and implement the default-alternate fallback. Test both directions, disallowed modifiers, unmodified use when a modifier is merely allowed, and fallback behavior. Do not interpret the old reverse-only probe as proof of the intended modifier semantics.

<a id="r15"></a>

### R15 P2 Enable the compile gates for advertised runtime timing controls

Sources: [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L15), [modules/svalboard/core/sval_qmk_settings.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_qmk_settings.c#L107), [modules/svalboard/core/sval_qmk_settings.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_qmk_settings.c#L172), [modules/svalboard/core/sval_tap_dance.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_tap_dance.c#L192), [modules/svalboard/core/sval_combo.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval_combo.c#L78).

The maintained build advertises settings that can be set and read back but do not control the corresponding QMK behavior. The most consequential problem is that the required C preprocessor gates are missing. `TAPPING_TERM_PER_KEY ?= yes` sets a Make variable, but this tree has no build rule translating that variable into `-DTAPPING_TERM_PER_KEY`. No Svalboard/module configuration enables `COMBO_TERM_PER_COMBO`, `PERMISSIVE_HOLD_PER_KEY`, `HOLD_ON_OTHER_KEY_PRESS_PER_KEY`, `RETRO_TAPPING_PER_KEY`, or `QUICK_TAP_TERM_PER_KEY` either.

QMK therefore uses its compile-time paths. The actual `svalboard/left:sval` ELF has no linked symbols for the runtime tapping/combo/hold callbacks; its linker map lists several under discarded sections. A second-pass `arm-none-eabi-gcc -dM -E quantum/action_tapping.c` using the maintained build’s recorded `cflags.txt` also confirms all six named gates are absent. Custom tap-dance terms are gated by the same missing tapping define. Merely providing these functions does not connect them to QMK.

There are additional stored-only fields: one-shot timeout/tap toggle, Chordal Hold, Flow Tap, grave-escape override, auto-shift enable flags, tap-code/caps/tapping-toggle delays, and mouse-key move delta/wheel delay/wheel interval. Auto-shift timeout is applied only when Auto Shift is compiled in, although it is always advertised. The release catalog already caveats some of these; the firmware's settings-query response still exposes them.

Enable and verify the appropriate QMK gates for implemented controls. Advertise only working settings until the other fields have runtime consumers. Add end-to-end behavioral tests: changing a value must change tapping/combo/hold decisions, not just EEPROM readback. Recheck migrated timing behavior, since copying those values alone is insufficient.

<a id="r16"></a>

### R16 P2 Fail builds when definition generation fails

Sources: [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L39), [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L57).

Both Python generators run through `$(shell ...)` with output discarded and their exit status ignored. Generated make files are optionally included. With files left from a successful build, malformed JSON or a failed schema check can leave the old files in place and allow the build to continue with stale counts or an old embedded definition. A clean build may fail later for a missing header, but incremental builds can conceal the error.

A Make-level probe runs the actual module rules with valid JSON, then replaces the input with invalid JSON. The second Make invocation still exits 0 and reports the previous header and `LEADER_ENABLE=yes`, with no diagnostic. The second-pass audit also builds a temporary keymap copied from `blank` with `qmk compile -kb svalboard/left -km review_audit_stale`, replaces its `sval.json` with `{ invalid JSON`, and repeats the same compile. Both full builds exit 0 and produce a UF2. The temporary keymap was removed afterward.

Use explicit generation targets with dependencies and checked exits, write output atomically, and remove/avoid using stale output on failure. Test invalid JSON and invalid fragment schemas after a successful build, as well as clean builds.

<a id="r17"></a>

### R17 P2 Provide disabled-feature stubs when omitting optional modules

Sources: [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L65), [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L81), [modules/svalboard/core/sval.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/sval.c#L206).

`sval.c` unconditionally calls the feature reload functions. The rules omit a feature's entire source file when it is absent from `sval.json`; the disabled-feature stubs live inside those same omitted files. Consequently, omitting a feature removes its stub as well as its implementation.

A temporary keymap derived from `blank`, with only `leader` and `alt_repeat_key` removed from the count object, fails linking with undefined references to `sval_reload_leader` and `sval_reload_alt_repeat_key`. All current release definitions include both, so those shipped targets pass.

Always compile the small stub-bearing files or place the disabled stubs in an always-built translation unit. Test each feature independently omitted on a keyboard whose base feature flags match the desired configuration.

<a id="r18"></a>

### R18 P3 Remove or repair the documented default build command

Sources: [keyboards/svalboard/readme.md](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/readme.md#L27), [keyboards/svalboard/keymaps/default/keymap.json](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/keyboards/svalboard/keymaps/default/keymap.json#L2), [modules/svalboard/core/rules.mk](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/modules/svalboard/core/rules.mk#L29).

The board README offers a vanilla `default` build, albeit marked unmaintained. That keymap enables the Sval module but supplies no `sval.json`, so `qmk compile -kb svalboard/left -km default` fails immediately with “Sval module requires sval.json in your keymap directory.” It also retains an old empty-keymap implementation.

Point users to the maintained `sval`/`blank` targets, or restore a genuinely supported default target. The failure was checked with a direct compile command; the QMK mass-compile console can display `[OK]` for this early Make failure, so its label alone is not proof of success.

<a id="r19"></a>

### R19 P2 Recover interrupted multi-byte keycode upgrades

Sources: [quantum/dynamic_keymap.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/quantum/dynamic_keymap.c#L80), [quantum/nvm/eeprom/nvm_dynamic_keymap.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/quantum/nvm/eeprom/nvm_dynamic_keymap.c#L121), [quantum/via.c](https://github.com/morganvenable/sval-qmk/blob/9109ac8034fb61b22e3488b03d9297ac54f673fc/quantum/via.c#L145).

Keycode-version upgrade rewrites a stored 16-bit keycode using two independent EEPROM byte updates, high byte first. For example, upgrading old steno mode `0x74F0` to `0x751C` can be interrupted after writing `0x75`, leaving `0x75F0`. The version byte correctly remains 7. However, on retry `0x75F0` is outside the translation's source range, so it is retained and version 9 is committed. The key's intended action is permanently lost unless restored from another source.

A new probe compiles the production VIA startup function, upgrade loop, translator, and NVM keycode reader/writer. Its EEPROM mock interrupts immediately after the first completed byte write, then reruns startup over the persisted bytes. Result: `0x75F0`, version 9, and no repair write. QMK's wear-leveling adapter performs each byte update as a separate write; it does not make the pair one transaction. The probe models interruption between successful calls, not torn physical flash programming or erase recovery.

This affects an upgrade containing a translated keycode when power is interrupted at that boundary. It does not imply ordinary typing or every migration is affected. The older byte-writing routine was already present upstream; the fork's new in-place translation/retry path exposes this failure. Retaining the version until last is necessary but insufficient, and idempotence over complete 16-bit values does not cover mixed-byte states.

Preserve recoverable source data or journal the upgrade so a retry can distinguish an unfinished value from a legitimate current value. A wider write alone is sufficient only if the backend guarantees its power-loss atomicity. Test every persistent write boundary through startup and version commit; extend that analysis to encoder bindings, table records, and macro keycodes rather than assuming their writes are atomic.

## Core patch assessment

The 16-file core patch surface is relatively compact. The broader complexity comes from interactions between startup, persistent state, QMK compile-time gates, and dynamically edited actions.

| Patch group | Assessment and maintenance obligation |
| --- | --- |
| `action_util.c/.h` | The weak runtime timeout hook preserves the compile-time fallback. Its Sval consumer remains disabled; adding the hook alone does not implement the setting. |
| `dynamic_keymap.c/.h` | Widened macro offsets and count return types support the intended capacity. The 256-count assertion matches 8-bit playback IDs. The weak playback hook allows Sval extensions without replacing the upstream engine globally. Every caller and host must use the widened API deliberately. |
| `keycode_upgrade.h` | The current steno translation is idempotent because destinations lie outside the source range. Unsupported versions reset rather than guessing. Whole-value idempotence does not protect against interrupted byte writes (R19). Future renumberings need explicit tables and an atomicity/recovery audit; a patch-version change alone is not a complete migration policy. |
| `layout_stamp.h` | Geometry hashing avoids time-based resets on equivalent rebuilds. Same-size field reordering or semantic changes still require a manual schema bump; the hash cannot discover those automatically. Its introductory comment still mentions keycode numbering even though numbering is now handled separately. |
| `keymap_introspection.c` | Sval intentionally owns dynamic combo/tap-dance/override introspection. Compile-time features and source selection must agree, including disabled configurations. |
| NVM implementation and headers | The macro bounds now use subtraction guards and avoid offset wrap. The VIA keycode-version byte shifts following addresses, so this is a persistent-layout change for every affected VIA build, not just a declaration change. |
| `via.c/.h` | Version-last upgrade ordering and invalid-first initialization are useful, but the in-place upgrade still loses partially written keycodes (R19). VIA has a 20-bit layout fingerprint after the reserved magic nibble, not a full 32-bit stored hash. Collision resistance and schema discipline should not be overstated. |
| `raw_hid.c` | Making send weak gives the wrapper one controlled response interception point. Wrapped VIA replies have six fewer usable bytes; individual handlers and clients must respect that capacity. |
| `usb_descriptor.c` | Prefixed hardware serials and runtime names are useful extensions. Current Svalboard serial sizing is consistent; generic builds with a truncated odd number of hex characters need separate boundary testing. UTF validation needs R06. |

The existing macro-overflow fix passed its boundary cases, including UINT32_MAX offsets and operations beyond 64 KiB. The existing tests still support the pending/checked intent ordering at the mocked operation boundaries they exercise. They do not establish recovery at every EEPROM byte boundary: R19 demonstrates a failure in the subsequent keycode-version upgrade. R03 and R04 are additional migration-content problems hidden by or outside those mocks.

## Storage and flash-write assessment

Ordinary keymap lookups, macro playback, matrix scans, sparse table/label reads, and pointer-report processing read state rather than saving it. Writes occur when configurations change, during resets/migration/version upgrades, and when identity or selected persistent keyboard options change. Board status is not routinely rewritten on every scan.

The firmware already uses QMK wear leveling and EEPROM update operations, so unchanged bytes need not generate log entries. Sval settings are written immediately; `sval_save()` adds no flush or consolidation. A configuration session can still produce many changed-byte writes because table entries, settings records, and macro uploads are written as they arrive. There is no measurement here of edit rates, flash erase counts, or erase latency on the daily-driver board, so a case for additional consolidation cannot be based on normal typing frequency alone.

The linker map, rather than the aggregate `size` BSS total, provides the useful RAM check: the base `sval` build has a heap range of approximately 88.6 KB before runtime allocations, and migration requests a 64 KB snapshot. The aggregate BSS-like total includes reserved heap/stack regions; treating it as entirely occupied globals would be misleading. This gives nominal space for migration, but allocator overhead, fragmentation, and runtime allocations still warrant checking actual heap headroom at migration entry. Current firmware text is roughly 94–97 KB for the sampled release variants and is comfortably below the settings-store start.

The two-copy identity records with CRC and readback, preserved legacy store, and completion-after-stamps ordering are sound structural choices. They still rely on actual flash behavior: the mocked suite does not simulate torn page writes, interrupted erase/consolidation, corrupted log entries, or an identity write succeeding partially.

## Board and protocol assessment

Matrix pacing uses a frame gate to skip scans until they are due; other timing and power paths still contain waits. This is not evidence of a universally nonblocking loop. The diagnostics have bounded reference capture and a hard probe-iteration cap, and suppress key events during sweeps. Split RPC routing, production/diagnostic keymap separation, and delayed acknowledged reboot are useful safeguards. I found no additional confirmed bounds problem in the current fixed-size Sval table/label packets.

Power control, sensor rest modes, USB wakeup, serial transactions, RGB restoration, and clock switching are hardware-dependent. The code deliberately keeps USB timing separate and adjusts active PIO dividers. Builds cannot establish transient clock-switch correctness, split-link reliability, wake latency, or compatibility across mixed sensor halves. Those need measurements on the actual release images.

The client wrapper is routing and stale-session isolation, not authentication or an exclusive configuration lock. Bare VIA remains accepted, sessions are not tracked as issued credentials, and multiple clients can mutate the same device state. That can be a valid local-host design; it should be described accurately. Name staging is shared between clients, so simultaneous name edits also need an ownership/serialization policy if that workflow is supported.

Legacy VIA is limited to its 16-bit macro address space and a one-byte macro-count reader sees zero for count 256. This is documented as requiring a compatible Sval client. Wrapped VIA handlers also see the original 32-byte length although only 26 bytes were unwrapped; requests that assume a full 28-byte data chunk can consume stale tail bytes or return a truncated reply. Add capacity checks to the wrapper/handlers and test actual client request sizes before broad VIA compatibility claims.

## Documentation and build assessment

The launch catalog usefully distinguishes upstream functionality from this fork's integration and already acknowledges incomplete one-shot/Chordal Hold/Flow Tap wiring. R15 is broader than those caveats: even several implemented runtime callbacks are not enabled in the maintained image. Fix those gates and align capability discovery with behavior before describing all queried settings as supported.

The release matrix covers maintained sensors and sides. It does not run the host regression suite, and there is no evidence here of broad upstream-board coverage. The upstream major-branch workflow was organization-gated already; its disabled state is not evidence that this fork lost previously active coverage. Add the storage regressions and behavioral settings tests to fork CI, plus representative non-Sval VIA builds when touching shared core APIs. Pin release build inputs sufficiently to make artifacts reproducible, and record both firmware and Keybard revisions with release validation.

Historical `pimoroni` and `default` targets are outside the maintained release matrix; their presence is not evidence of release support. The flashing helper passed shell parsing, but WSL PowerShell quoting, paths containing apostrophes, multiple bootloader volumes, and actual copy/reboot behavior were not exercised. Do not infer successful flashing from this review.

## Remaining validation before release

1. Fix R01 and verify that untrusted PR jobs cannot access a write token; keep publication privileges restricted to trusted release events.
2. Fix reset, migration content, interrupted keycode upgrades, and name-save recovery; test them with real defaults/settings code rather than no-op mocks.
3. Enable runtime timing hooks and test resulting typing behavior at several configured values, including values migrated from Vial.
4. Exercise scrolling, boosted/sniper motion, auto-mouse disablement, held PS/2 buttons, and editing active tap dances with host-visible reports.
5. Run a hardware migration matrix with nondefault keymaps, macros, tables, magic flags, and timing settings. Interrupt power during copying, stamp writes, identity writes, and wear-leveling consolidation; confirm the legacy source stays intact and a completed migration is never resurrected after a wipe.
6. Check identity/name persistence across ordinary flashes, reset/wipe operations, reconnects, enumeration, unsupported flash capacity, and failed writes.
7. Measure idle/wake timing, USB suspend/resume, both choices of USB-connected half, split reconnects, and mixed sensor setups through configured clock states.
8. Build the exact release artifacts in the intended container, run the tests in CI, and verify Keybard protocol/capability behavior against those artifacts. The separate editor was not fully reviewed in this firmware-fork audit.

No physical board was flashed, reset, or power-cycled as part of this review. No defect in this report is presented as hardware-confirmed unless explicitly stated; the evidence is source inspection, host characterization, or compilation.
