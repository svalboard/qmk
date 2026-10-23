# Plan: in-firmware updater (sval-qmk#7 / keybard#24), revision 4

Status: **plan for your review. No feature code has been written.** Sources: [morganvenable/sval-qmk#7](https://github.com/morganvenable/sval-qmk/issues/7), [svalboard/keybard#24](https://github.com/svalboard/keybard/issues/24), the WSL clone of `sval-qmk` (branch `feat/fw-updater`, cut from upstream `svalboard` at `7e0af09`) and a read-only reading of `keybard` (`main`). Paths are relative to each repo root, and firmware paths under `keyboards/svalboard/` are shortened to `kb/`. Every firmware citation was checked against `7e0af09`, and every pico-sdk and ChibiOS citation against the checked-out submodules (pico-sdk `d0c5cac`). **UNVERIFIED** marks anything not confirmed in code; two items remain, both datasheet-only: which blocks an AIRCR `SYSRESETREQ` resets on RP2040, and what state peripherals keep across a watchdog reset. Revision 2 took in two adversarial reviews, and revision 3 recorded your answers from the [decision page](https://claude.ai/artifact/WsV3uM75FY2iQ1p1XEsGsX). **Revision 4 applies the M0 findings:** upstream `7e0af09` removed `kb/storage/` and moved settings back to QMK wear leveling, so the storage-layer pieces are replaced; the commit routine gains the watchdog, boot2 and DMA safety fixes; a `REBIND` op answers the client-ID expiry; D12 is open again. The Appendix lists what changed.

**v1 ships without signing.** Signing (Ed25519 manifest signatures, a test key, two release keys, a CI signing job in an approval-gated environment) was built and tested through M3 and then deferred for the first version. That code is kept under the tag `archive/fw-updater-signed`. v1 is updater protocol 2: a `.svup` is the 108-byte manifest, then the image; byte 6 of the manifest (`key_id` in version 1) is reserved and must be 0; status 8 (`BAD_SIG`) is retired. The decision record below keeps the signing decisions, each marked deferred. `docs/updater.md` describes what is built.

---

## 1. Summary

- Add an updater that is compiled in only when a build flag is set. It stages a raw image in spare flash (signed in the plan; unsigned in v1), verifies it, **invalidates the old image first** (page 0 programmed to zeros), then copies the new image from RAM and writes page 0 last.
- **M1** proves this on **one test board half over USB**, driven by `kb/tools/sval_update.py`. No split traffic is involved.
- **M2** adds the split relay (store-and-forward) and the version handshake, and needs the split test board. **M3** adds release CI and release gating (CI signing was built in M3 and deferred for v1). **M4** adds the Keybard UI.
- **"Ready to test on hardware"** means: the right test board half running an `SVAL_UPDATER=yes` build, flashed once by UF2, on which the host tool can (1) rotate between images A and B, (2) get every bad-input case rejected with its own status code, (3) show the LED states, and (4) through a test-hooks build that halts mid-commit, come up as `RPI-RP2` and be recovered with `kb/tools/flash.sh`, with the settings and identity regions identical **byte for byte** to a dump taken before the test.

### Flash map (7e0af09, `kb/docs/settings-storage.md:5-11`)

| Flash offset | Contents |
|---|---|
| `0x000000`-`0x15FFFF` | Firmware (`kb/flash_reservation.ld:2` keeps it below `0x160000`) |
| `0x160000`-`0x1DFFFF` | Settings, wear-leveling copy 0 |
| `0x1E0000`-`0x25FFFF` | Settings, wear-leveling copy 1 (`kb/config.h:44-54`) |
| `0x260000`-`0xFFDFFF` | Free. The staging slot `0x800000`-`0x95FFFF` (D8) lies here |
| `0xFFE000`-`0xFFFFFF` | Board identity (`kb/identity.c:37-38`) |

### Corrections to the issue text

| Issue says | Code says | Plan |
|---|---|---|
| "Write sector 0 last" | Sector 0 also holds the vector table, which follows `.boot2` (`platforms/chibios/boards/common/ld/RP2040_FLASH_TIMECRIT.ld:44,98-102`). The ROM checks CRC-32/MPEG-2 (poly `0x04C11DB7`, init `0xFFFFFFFF`, no reflection, no xorout) over bytes 0-251 against the little-endian word at 252, tries 128 times, then falls back to USB boot (pico-bootrom `_flash_boot`; `lib/pico-sdk/src/rp2_common/boot_stage2/pad_checksum:39-43`). Test vector: page 0 of the current build gives `d58f0b07` | Zero page 0 first, write page 0 last |
| End with `watchdog_reboot()` | That is a flash function (pico-sdk `watchdog.c:77`), and it is not even linked into this firmware | Direct register writes from RAM (D10) |
| Fragment detection knows the side and the sensor | `sval_fragment_detect()` is a weak stub (`modules/svalboard/core/sval_fragments.c:40-44,74-83`). Handedness is `EE_HANDS` (`kb/config.h:37`), and the running build forces it on every boot (`quantum/split_common/split_util.c:155-172`; `-DINIT_EE_HANDS_LEFT` at `kb/left/rules.mk:1`) | The check is "image side and variant == running build's side and variant". It does **not** detect the hardware (Risk R8) |
| Erase may trip the split watchdog | The split watchdog only acts at boot (`split_util.c:82-120`), but a slave that reboots after an update needs a re-ping (D15) | The real limits are the 20 ms handshake timeout (`platforms/chibios/drivers/serial_usart.h:43-44`) and 10 errors to disconnect (`split_util.c:46-48`) |
| Image about 95 KB | The linker caps it at `0x10160000` (`kb/flash_reservation.ld:2`). The largest release image today is 97,268 B (trackball/pmw3360/right) | Slot and checks sized to 0x160000 |
| Raising RPC buffers is harmless | An old slave trusts `m2s_length` from the wire (`quantum/split_common/transactions.c:1050-1057`). Both halves XOR `NUM_TOTAL_TRANSACTIONS` into every handshake (`serial_protocol.c:59-61,69,136`) | Not an issue: both halves always run the same release (V), so buffer sizes always match. CI checks that every supported pairing of builds produces an identical split message table (P1) |

---

## 2. Milestones

### M0: setup, hardware facts, measurements (no firmware feature)

- [x] Clone [morganvenable/sval-qmk](https://github.com/morganvenable/sval-qmk) **with submodules** into Ubuntu WSL (`~/GitHub/sval-qmk`), add `upstream` = `svalboard/qmk`, and create branch `feat/fw-updater` from `upstream/svalboard` (`7e0af09`).
- [x] Re-check every pico-sdk and ChibiOS claim marked UNVERIFIED against the checked-out submodules. Done: 39 claims checked, 26 confirmed, 11 refuted (applied in rev 4), 2 still unverified (datasheet-only, listed in the header).
- [x] Build all 12 release builds (5 variants × 2 sides with `sval`, plus left and right with `blank`). All pass with Arm GNU 13.2.1; the largest image is 97,268 B, far below `0x160000`.
- [x] On the right test board (D1): BOOTSEL by hand works, and the board has a reachable reset button. Flashed `svalboard/trackball/pmw3389/right:sval` from `feat/fw-updater` (upstream `7e0af09`, no firmware changes) with `picotool load`, which verifies what it writes. `flash.sh` can't be used here: a drive attached to WSL doesn't mount as `RPI-RP2`. The board typed and the trackball worked after the flash.
- [x] Flash size and ID. picotool 2.3.1 reports RP2040 rev B2, flash size `16384K` (so the capacity byte is `0x18` and the `kb/identity.c:41,87-92` check passes) and flash unique ID `0xE465B0621725122B`. It does not print the raw JEDEC manufacturer bytes; the firmware reads those at runtime.
- [x] Baseline dumps through usbipd + WSL picotool 2.3.1 (needs `LD_LIBRARY_PATH` set to its runtime, and a udev rule giving `plugdev` access to `2e8a:0003`, installed as `/etc/udev/rules.d/61-rp2040-bootsel.rules`). Dumps of the whole chip, firmware, settings `0x10160000`-`0x10260000`, slot `0x10800000`-`0x10960000` and identity `0x10FFE000`-`0x11000000`, with SHA-256 sums, are in `~/m0-baseline/20261008-110824-*` in WSL. The slot was blank (all `0xFF`). Settings, slot and identity were byte-for-byte unchanged both straight after the flash and after a full boot and typing test.
- [x] Audit every DMA channel user for any source address in XIP. Result in R6: WS2812 reads RAM; the PMW SPI TX source `dummytx` is in XIP; I2C uses no DMA; core 1 never starts.

Exit: the fork builds; image size, JEDEC ID, BOOTSEL and reset access, picotool and the DMA audit are all recorded; the Section 3 decisions are answered.

### M1: one half updates itself over USB (no split test board)

**Scope:** staging slot, async state machine, manifest with structural and signature checks (test key; the signature check was removed for v1), session binding, matrix-level gesture tied to a manifest hash, RAM commit with zero-first invalidation, a RAM vector table, a fed watchdog, LEDs, and STATUS. `hand` must equal the local side, otherwise `UNSUPPORTED`. There is **no split traffic**.

**Files** (new code in `kb/updater/` unless a path is given)

| Path | Purpose |
|---|---|
| `updater.c/.h` | State machine: `IDLE → MANIFEST_LOADING → CONFIRM_WAIT → VERIFYING_MANIFEST → ERASING → RECEIVING → VERIFYING_IMAGE → VERIFIED → COMMITTING`, plus `ERROR`. HID handler replies at once; all slow work runs in `updater_task()` from housekeeping, one block or verify slice per pass |
| `update_flash.c` | Erase and program for the slot only, with its own slot range guard. Follows the ROM-lookup pattern in `kb/identity.c:71-84` / `platforms/chibios/drivers/wear_leveling/wear_leveling_rp2040_flash.c:182-192` |
| `update_commit.c` | RAM-only commit (`__no_inline_not_in_flash_func`), RAM vector table, watchdog feed, reset. Compiled with `-fno-jump-tables -fno-tree-loop-distribute-patterns -fstack-usage`; no `switch`; no calls except to ROM pointers and listed RAM symbols. Owns a dedicated 4 KiB static sector buffer and its own 256 B boot2 copy |
| `update_image.c` | Structural checks on staged page 0 and the vectors (see the checks below) |
| `update_gesture.c` | Chord detection (D4) from the debounced local matrix (`matrix_get_row`, `quantum/matrix.h:65`), never from keycodes |
| `update_led.c` | LED states (D24): blue blink, white double flash on chord, rainbow, magenta, green double flash when done, red 1 Hz 50/50 for 30 s. How rgblight is held off is decided by the D20 experiment |
| `update_manifest.h` | Manifest struct, status codes, op codes |
| `vendor/` | Monocypher 4.0.2 for Ed25519 and SHA-512 (D7), BSD-2/CC0, not GPL. **v1:** only SHA-512 is kept, cut into `vendor/sha512.c/.h` (`LICENCE.md` kept); Ed25519 went with signing |
| `kb/rules.mk` | `SVAL_UPDATER ?= no`, following `SVAL_KEYTEST` (`kb/rules.mk:46-50`). `SVAL_UPDATE_TEST_HOOKS ?= no` is a separate flag. `#error` if `SVAL_KEYTEST` and `SVAL_FIRMWARE_UPDATE` are both set (R11). `SVAL_UPDATER` builds set `USE_PROCESS_STACKSIZE = 0xC00` (main's stack is 2 KiB by default, `platforms/chibios/platform.mk:8-9`; the Ed25519 check needs 1,856 B; the extra 1 KiB fits in the free part of SRAM4; as built, `0xAE0`, M1-a, kept in v1). First D20 build check: `RGBLIGHT_DRIVER = custom` with a gating `rgblight_driver`; that build must then set `WS2812_DRIVER_REQUIRED = yes` itself, because only the `ws2812` driver sets it (`builddefs/common_features.mk:331-332`) |
| `kb/config.h` | `SVAL_UPDATE_BASE/SIZE/MAX_IMAGE`, gesture and LED constants |
| `updater.c` asserts | `MAX_IMAGE % 0x10000 == 0 && MAX_IMAGE <= WEAR_LEVELING_RP2040_FLASH_BASE`. Slot base and size 64 KiB aligned, base ≥ `WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES` (= `0x260000`), end ≤ `IDENTITY_SECTOR_A` (next to `kb/config.h:44-54`) |
| `kb/svalboard.c` | Call `updater_task()` from `housekeeping_task_kb` (`:503`). Add the channel case in `via_custom_value_command_kb` (`:614-631`). Skip `sval_rgb_idle_task` and `identity_task` while active |
| Settings hold-off | QMK's eeprom / wear-leveling path (including its multi-second consolidation, `kb/docs/settings-storage.md:37`) needs no hold-off for durability: writes go straight through (D12). The updater refuses `ARM`, and the commit refuses in step 0, while `wear_leveling_write_failed()` is latched (qmk#12) |
| `kb/matrix.c` | No low clock or deep idle while active: use the same gate as `scanlab_active()` in the pacing code (`kb/matrix.c:266-307`, gate at `:268`) |
| `modules/svalboard/core/client_wrapper.c/.h` | Add `client_wrapper_current_id()` getter (`wrapper_client_id` is static at `:19`, set at `:167`). This is a shared-module edit (D3) |
| `kb/tools/sval_update.py` | hidapi host tool modeled on `kb/tools/keytest.py` |
| `kb/tools/make_update.py` | UF2 → raw image. Refuses a family-ID mismatch, a start address other than `0x10000000`, gaps, or an end beyond `0x10160000`. Builds the manifest (and signed it, before v1) |
| `kb/tools/check_ram_funcs.py` | Disassembles every symbol in the commit's `.time_critical` set. Fails on any branch or call whose final target (veneers and literal `ldr pc` followed) is neither in the allow-list nor in ROM (<0x4000), and on any `blx rN` other than the ROM pointers. Also fails on `-fstack-usage` > 512 B |
| `tests/sval_updater/`, `util/updater_test/run.sh` | Host tests under ASan and UBSan in a new standalone harness, modeled on the `util/durable_storage_test/run.sh` that `7e0af09` removed (see `7d71434`) (flash mock: see the host tests below) |
| `docs/updater.md` | Protocol, flash map, reset mechanism, recovery |

**Host protocol** (keyboard-local VIA channel, D2a). A packet is `[0xDD][client_id:4][0xFE]` + VIA (`modules/svalboard/core/client_wrapper.c:157-176`; overhead `client_wrapper.h:39`). Unwrapped packets also reach VIA (`modules/svalboard/core/sval.c:990-1003`), so **every op except INFO returns `INVALID` unless `client_wrapper_in_via()` is true** (`client_wrapper.c:50-52`). That leaves 23 bytes of `value_data` (same framing as `kb/scanlab.c:381-410`).

| Op | Request | Response / rule |
|---|---|---|
| `0x00 INFO` | `[hand]` | status, state, protocol version, slot base and size, max image, JEDEC ID (3 B), variant ID, hand, numeric version, storage format |
| `0x01 MANIFEST` | `[hand][off u8][n][≤20 B]` | Fills a 108 B RAM buffer: the manifest (172 B with its 64 B signature in protocol 1). Allowed only in IDLE or MANIFEST_LOADING |
| `0x02 ARM` | `[hand]` | Needs a complete manifest. Returns `nonce u32` (from ROSC, as `identity.c` does; not secret) and the first 4 B of `sha512(manifest)`, which the host shows the user. Binds the session to `client_id` and nonce. Enters CONFIRM_WAIT and the LED blinks |
| `0x03 BEGIN` | `[hand][nonce]` | `ACCEPTED` straight away once the gesture is done. Housekeeping then runs VERIFYING_MANIFEST (structure fields, hand, variant, epoch; and the signature, before v1) and ERASING (one 64 KiB block per pass, only blocks with any non-0xFF byte, `len` rounded up) |
| `0x04 CHUNK` | `[hand][off u24 LE][n ≤ 18][data]` | status + next offset. Strictly in order. A duplicate earlier offset is acknowledged again without being rewritten. Refused until ERASING is done. Nonce is checked through the session's `client_id`, because there is no room for it in the packet (D3) |
| `0x05 END` | `[hand][nonce]` | `ACCEPTED`. Housekeeping runs VERIFYING_IMAGE: SHA-512 over the slot read through `XIP_NOCACHE_NOALLOC_BASE`, the structure checks, and a CRC32 of the image kept for the commit |
| `0x06 STATUS` | `[hand]` | state, bytes staged, erase progress, last error, verify ms, erase ms max |
| `0x07 COMMIT` | `[hand][nonce][crc32 lo16]` | Needs VERIFIED. The `crc32 lo16` must match the verified image, so a stray COMMIT cannot fire against the wrong image. This is not a security check. The device acks, and the commit starts 100 ms later from housekeeping (`kb/scanlab.c:65-87` pattern) |
| `0x08 ABORT` | `[hand][nonce]` | Accepted from the bound session, or from anyone while `ERROR` is latched. Resets state and LEDs. **No erase**: BEGIN erases anyway |
| `0x09 REBIND` | `[hand][nonce]` | Re-binds a live session to the sender's client ID when the nonce matches; the old ID is no longer accepted. Keybard sends it straight after each 50 s client-ID renewal, before its next updater op (D3). Wrong nonce: `OTHER_CLIENT`. No session: `INVALID` |

Every op must answer within 50 ms (host-tested). Keybard fails a command at 1000 ms (`keybard src/services/usb.service.ts:739-742`). The session times out after 30 s with no op. Status codes extend the existing sets (`kb/identity.h:20-25`; the enum at `kb/keytest.c:18`): `OK, ACCEPTED, INVALID, UNAVAILABLE, BUSY, NOT_CONFIRMED, WRONG_HW, BAD_SIG (retired in v1, never returned), BAD_HASH, BAD_IMAGE, EPOCH, OUT_OF_ORDER, OVERRUN, TOO_LARGE, TIMEOUT, OTHER_CLIENT, UNSUPPORTED, FLASH_ERR`.

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;           // 'SVUP'
    uint16_t manifest_ver;    // 1 in the plan; 2 in v1 (version 1 is refused)
    uint8_t  reserved0;       // 0 (key_id in version 1: 0 = test, 1/2 = release keys)
    uint8_t  hand;            // 0 left, 1 right
    uint8_t  pointing_id;     // this half's pointing device only (D18): 0 none, 1 TrackPoint,
                              // 2 trackball PMW3360, 3 trackball PMW3389, 4 Azoteq, 5 any (one build
                              // with every driver, pointing_auto); append-only
    uint8_t  keymap_id;       // 1 sval, 2 blank: display only, never checked (D18)
    uint16_t flags;           // bit0 RELEASE, bit1 DIAGNOSTIC (scanlab/keytest/host-bootloader)
    uint16_t security_epoch;  // refuse < device epoch (D19)
    uint8_t  storage_format;  // refuse < current v2 format (D19)
    uint8_t  updater_proto;   // updater protocol of the image
    uint32_t image_len;       // 0x200..MAX_IMAGE, multiple of 256
    uint32_t fw_version;      // numeric, also used in split presence (D17)
    char     version[16];     // NUL-padded display string
    uint8_t  sha512[64];      // over image_len bytes
} sval_update_manifest_t;     // 108 B (as built, with a reserved u32 before version). Version 1 put a
                              // 64 B Ed25519 signature after it; in v1 a .svup is the manifest, then the image
```

**Structure checks on the device** (at VERIFYING_MANIFEST where possible, at VERIFYING_IMAGE, and again at commit step 0): the boot2 CRC of staged page 0 is valid (CRC-32/MPEG-2 over bytes 0-251, stored little-endian at 252; the host tool computes the same and a host test pins it with the `d58f0b07` vector); the initial SP at 0x100 lies in `[0x20000000, 0x20042000]`; the reset vector is odd and inside `[0x10000100, 0x10000000+image_len)`; `image_len` is a multiple of 256 and ≤ MAX_IMAGE; a release build refuses `flags.DIAGNOSTIC` (and, before v1, `key_id 0`).

**Commit routine** (RAM only, PRIMASK set, ROM functions through `rom_func_lookup_inline` as in `kb/identity.c:71-84`, no SDK flash wrappers, no `memcpy`/`memset`; buffer = a dedicated 4 KiB static sector buffer, not shared with QMK; process stack 2 KiB (3 KiB in `SVAL_UPDATER` builds) and exception stack 1 KiB, both in SRAM4, `RP2040_FLASH_TIMECRIT.ld:34,68-72`. The commit runs only from housekeeping, and on a slave only behind the split pause)

0. With IRQs on: refuse with no erase if `wear_leveling_write_failed()` is latched (D12); hold off the identity task; re-run the structure checks and SHA-512 on the slot; take the commit's own 256 B RAM copy of boot2 from page 0 and check its CRC (every boot2 copy is filled from `BOOT2_ROM`, which *is* page 0, `platforms/chibios/vendors/RP/stage2_bootloaders.c:10,151`; after step 2 it would copy zeros, so the copy is taken here and never refilled; do not rely on the `identity.c:68,171` copy, which exists only if `identity_init` ran); show the "writing" color, latch it with `ws2812_flush()` (twice), then let the last WS2812 transfer finish (≥ `WS2812_TRST_US`; its DMA reads RAM, `ws2812_vendor.c:138,297`, so it is not stopped, and aborting it would corrupt the color).
1. Set PRIMASK. Read every DMA channel's BUSY bit; if any channel is busy, clear PRIMASK and refuse the commit with `BUSY` and **no erase** (the PMW SPI TX source `dummytx` is in XIP; it is idle whenever housekeeping runs, R6). Fill all 48 slots of a RAM vector table, aligned to 256 B, with SP and the RAM fault handler (in `.time_critical`; it clears scratch4 and triggers the watchdog), and point VTOR at it (VTOR normally points at the flash table: `platforms/chibios/vendors/RP/RP2040.mk:13`; `RP2040_FLASH_TIMECRIT.ld:44-45`). Arm the watchdog (D9) in this order: write `PSM_WDSEL = 0x1FFFC` (at `0x40010008`; `PSM_WDSEL_BITS & ~(ROSC|XOSC)` as `_watchdog_enable` does at `watchdog.c:39-40`, which this firmware never links; WDSEL resets to 0, `psm.h:299-304`, and with 0 a watchdog reset resets nothing); clear scratch4; `WATCHDOG_LOAD = period_µs × 2` (erratum E1: the counter drops 2 per µs, `watchdog.c:55-59`; ≤ `0xFFFFFF` ≈ 8.39 s; ≥ 2 × the worst block erase); clear the `PAUSE_DBG0/1` and `PAUSE_JTAG` bits (`WATCHDOG_CTRL` resets to `0x07000000`, `regs/watchdog.h:22`); set ENABLE.
2. **Invalidate:** program page 0 to all `0x00`, then read it back through the NOCACHE alias. If it is not all zero, reset (the old image is still mostly intact; R1). Test halt point `N=-1` is here.
3. Erase sector 0 (4 KiB), then the rest of block 0, then the other 64 KiB blocks up to `round_up(image_len, 64 KiB)`, clamped to MAX_IMAGE. The RAM range guard rejects any address outside `[0, MAX_IMAGE)`. Feed the watchdog after every ROM call by writing `WATCHDOG_LOAD = period_µs × 2`.
4. For each 4 KiB sector: copy slot → RAM sector buffer with XIP on, exit XIP, program its pages (sector 0 starts at page 1), flush the cache, re-enter XIP through the boot2 copy taken in step 0 (never refilled). Feed the watchdog.
5. Compute CRC32 over `[0x100, image_len)` through NOCACHE and compare with the CRC32 from VERIFYING_IMAGE. On a mismatch, **erase the bad sector, program it again, compare again**, once. If it still fails, leave page 0 zero, then reset (→ BOOTSEL).
6. Program page 0 from the RAM buffer and read it back. Feed the watchdog.
7. Reset (D10): zero every RAM buffer that held image bytes (at least every word equal to `0xCAFEB0BA`), because the double-tap magic is noinit RAM (`.ram0.bootloader_magic`, `platforms/chibios/bootloaders/rp2040.c:32-33`) and could land inside the old buffer in the new image's layout, sending it to BOOTSEL. Confirm `PSM_WDSEL` is still set, clear scratch4, then write the watchdog trigger. Never call flash.

**M1 hardware tests** (the test board half from D1, USB only; picotool dumps before and after every row marked D)

| # | Step | Expected |
|---|---|---|
| 0 | M0 checks done: BOOTSEL by hand works, JEDEC ID recorded, baseline dump | Recorded |
| 1 | `flash.sh` the `SVAL_UPDATER=yes` build | Normal boot. `info` shows slot, JEDEC ID, variant, version |
| 2 | `make_update.py` builds A and B (differing only in version), signed with the test key | `.svup` files produced. `make_update.py` refuses a doctored UF2 with a gap or the wrong base |
| 3 | `update B` with no gesture | `NOT_CONFIRMED` after the 30 s window. Firmware unchanged |
| 4 (D) | `update B` with the gesture | LED states in order. `info` shows B. Dumps identical |
| 5 | Remap the gesture keys over VIA, then `update A` | The gesture still works (it is matrix-level). Remapped keycodes are not emitted during CONFIRM_WAIT |
| 6 | Reject matrix: wrong hand, wrong variant, `len > max`, `len` not a multiple of 256, a flipped signature byte, a bad SHA, bad boot2 CRC, bad SP or reset vector (signed with the test key), epoch below, storage format below, `key_id 0` on a release-flagged build, out-of-order chunk, overrun, END too early, an unwrapped packet, a second client ID, ABORT from another client, REBIND with the wrong nonce, COMMIT with the wrong crc, a 30 s stall | Each gives its expected status (M1 review: `sval_update.py reject` drives the protocol rows; epoch below needs a test board built with `EXTRAFLAGS=-DSVAL_UPDATE_SECURITY_EPOCH=1`; until M3 a release build has no keys, so the `key_id 0` row only shows it accepts nothing; see `docs/updater.md`). Error LED, then normal after ABORT |
| 7 | Unplug USB during ERASING, RECEIVING and VERIFIED | Old image boots. The slot is ignored |
| 8 (D) | `SVAL_UPDATE_TEST_HOOKS` build, halt at N = −1 (after zero, before erase), 0 (after first erase), mid-program, last sector, and after page 0. Unplug and replug each time | N ≤ last sector: `RPI-RP2` appears. `picotool save -r 0x10000000 0x10000100` shows page 0 zeros at N = −1 and all `0xFF` from N = 0 on (step 3 erases sector 0 first; M1 review); either way its boot2 CRC is invalid. `flash.sh` recovers. After page 0: B boots. Dumps identical |
| 9 (D) | Same halts with the watchdog armed (D9a) and no unplug | Before page 0: resets into BOOTSEL by itself within the watchdog period. After page 0: boots B |
| 10 | Test-hooks build with an injected HardFault after invalidation, and after the first erase (flash vector table erased; M1 review) | Resets into BOOTSEL quickly through the RAM vector table: well under 1 s. A reset only after the ~8 s watchdog period is a failure (the RAM table was not used) |
| 11 | Two quick taps of the **RUN/reset button** within 500 ms (`platforms/chibios/bootloaders/rp2040.c:39-55`; magic is in RAM, so unplugging does not work) | BOOTSEL. Skip if there is no reset button (M0) |
| 12 | Slow transfer (`--delay`) idle for > 10 s | No dimming, no deep idle, LEDs held |
| 13 | Measure: flash and RAM cost, HID round trip, transfer time, verify ms, max erase ms per block, commit ms, stack margin of the Ed25519 verify (stack canary: `sval_update.py diag`, which also gives the longest updater pass) | Logged. These decide D11; D13 is measured in M2b |
| 14 | Commit an image pair both ways (A→B and B→A) | Each boots its new image, never BOOTSEL (stale double-tap magic, step 7) |
| 15 | Run the D9a halt test with SWD detached, or with `PAUSE_DBG` cleared | Resets into BOOTSEL within the watchdog period (the watchdog does not freeze) |
| 16 | `update B` with the host tool renewing its client ID and sending REBIND after each (`--rebind-every 5 --min-rebinds 3`: at 50 s a fast transfer may never renew; M1 review) | Update completes; the old ID is refused after REBIND |

**Host tests (no hardware):** state ordering; every op answers within 50 ms; manifest bounds; REBIND (right nonce, wrong nonce, old ID refused); Ed25519 vectors; structure checks; boot2 CRC vectors (CRC-32/MPEG-2, `d58f0b07`, all-`0x00` page `7065399a` and all-`0xFF` page `0b8fd31a` both failing). The flash mock models a torn erase or program as random values in the affected sector or page, injects a cut after **every** flash op, and asserts on every op:
(a) during staging only `[SLOT_BASE, SLOT_BASE+SIZE)` changes;
(b) during the commit only `[0, round_up(image_len, 64 KiB))` changes;
(c) nothing else ever changes;
(d) after any cut, the boot2 CRC of page 0 is invalid **or** the whole new image is present.

**Exit:** every hardware row passes on the test board half; the host tests pass; `check_ram_funcs.py` is clean; you have reviewed the measurements in #13 and the answers to D11 and D13.

**M1 hardware results (2026-10-08).** Right test board (`E465B0621725122B`) and, as a second unit, the left test board (`E464B4B293221323`), both RP2040 B2 with a 16 MiB Winbond flash (`ef4018`). Builds from `feat/fw-updater` with `SVAL_UPDATER=yes SVAL_UPDATE_TEST_KEY=yes` (and `SVAL_UPDATE_TEST_HOOKS=yes` for #8-#10), the host tool on Windows Python with hidapi, and picotool through usbipd for the dumps. These results are for protocol 1, with signing; the test key and the `BAD_SIG` row are gone in v1, and the rest of the code they exercised is unchanged.

| # | Result |
|---|---|
| 1 | Pass on both halves |
| 3 | Pass four times: blue for 30 s, then red `NOT_CONFIRMED`; ABORT clears it |
| 4 | Pass, A→B on the right; settings and identity identical byte for byte to the dumps before the test and to the M0 baseline; the slot holds exactly B. Typing and pointing come back without a power cycle (R24) |
| 5 | Skipped (your call) |
| 6 | Pass: the 26 protocol cases (`reject`) and the image cases `UNSUPPORTED` (hand), `WRONG_HW`, `STORAGE`, `BAD_SIG`, `BAD_HASH`, `BAD_IMAGE` (boot2, SP, reset vector). Not run: epoch below (needs a floor-1 build), too large and not whole pages (`make_update.py` refuses them; host-tested) |
| 7 | Pass for an unplug while VERIFIED and while RECEIVING: the old image boots, the slot is ignored. ERASING not hit: the window is about 1.5 s; covered by the 102 staging cuts in the host tests |
| 8 | Pass (short set): halt after invalidation and at the last sector, then unplug: BOOTSEL, page 0 all `0x00` and all `0xFF`; settings and identity unchanged |
| 9 | Pass (short set): halt after page 0 with the watchdog unfed: B boots by itself about 13 s later |
| 10 | Pass: `fault` and `fault-erased` both reset into BOOTSEL in about 1 s (the RAM vector table, not the 8 s watchdog) |
| 11 | Pass |
| 12 | Covered by the 30 s chord waits and 17 s transfers: no dimming or deep idle |
| 13 | Transfer 118 KB in 16.7-17.7 s (about 6,600 round trips at 1.6-1.8 ms), signature 148-151 ms, image verify about 485 ms, sector erase 50-53 ms, about 1-2 s from COMMIT to reboot. Main stack: 780 B never used out of 2,784 after a full update. The commit ends with a "watchdog force" reset |
| 14 | Pass both ways on the right (A→B→A) and on the left (2001→2002→2001); never BOOTSEL. Left settings and identity identical to its pre-test backup |
| 15 | Not run separately: no SWD probe is attached, so #9 already ran with the debugger pause bits irrelevant |
| 16 | Pass: 4 renewals mid-transfer, the old ID refused each time |

Open: once, after a picotool reflash, the board sat in CONFIRM_WAIT (STATUS showed it) but the LEDs stayed on the normal color instead of blinking blue. Not seen again in the next eight sessions. The first run of #3 also rebooted the board once mid-wait and was not reproduced; DIAG now records the reset reason and the updater state before a reset, to catch either if it recurs.

### M2: split relay and version handshake (M2a without the test board, M2b on the test board)

**Scope:** store-and-forward relay (D14) over a new `KEYBOARD_UPDATE` split message, presence with version (mismatch detection only), a split pause done in the keyboard layer (D15), slave mailbox, inactivity timeout, LEDs on both halves.

| Path | Purpose |
|---|---|
| `kb/svalboard.h:57-68`, `kb/svalboard.c:437-452` | The presence request layout stays the same. `kb_sync_listener` now fills the ≤32 B response (`transport.h:32`): magic, struct version, `fw_version u32`, short git hash, updater protocol, hand, pointing_id, crc8. The master passes `sizeof(presence_rpc_t)` (17 B) as the response length (`kb/svalboard.c:519-520`), so the caller changes too. **As built on the current upstream:** upstream's own reply (`presence_rsp_t`: a 2-byte magic and the 5-byte pointer report, Sval `0x2C`) comes first, unchanged, and updater builds append these 17 bytes after it: 24 B, static-asserted to fit the 32 B RPC buffer. On a version mismatch: Keybard warns, and the half with USB shows the red error LED until fixed (V). Today nothing is written to `out_data`, so the stale bytes fail magic and crc |
| `kb/matrix.c` (D15) | Override the weak `matrix_scan()` (`quantum/matrix_common.c:164`): while paused, skip `matrix_post_scan()` (`matrix_common.c:91-117`) and call `matrix_scan_kb()` directly. On pause entry, clear the slave rows once and report a change, as the disconnect path does (`matrix_common.c:100-108`), so a slave key held at pause entry is released. On resume, call `split_watchdog_update(false)` (`quantum/split_common/split_util.h:14`). The pause also covers the presence RPC (`kb/svalboard.c:513-529`) and the Scan Lab relay (`kb/scanlab.c:403`); M0 confirmed nothing else sends split messages. Refuse to pause while the link is disconnected. The pause flag defaults to false, because `bootmagic_scan` calls `matrix_scan` before init (`quantum/bootmagic/bootmagic.c:63-67`). `kb/matrix.c:124-125` declares the core `raw_matrix` and `matrix` with `ROWS_PER_HAND` rows: fix them to `MATRIX_ROWS`, or put the override in its own file. **No QMK core change.** The copied lines get a comment pointing at the core original, checked on each QMK merge |
| `updater/update_split.c` | **Master:** after the M1 path has staged and fully verified the subside image in its own slot, relays it from flash with a per-pass time budget. **Slave:** the `KEYBOARD_UPDATE` callback programs one 256 B page per RPC itself (at most ~3 ms). It runs in the HIGHPRIO SlaveThread (1 KiB stack, `platforms/chibios/drivers/serial_protocol.c:17-38`), not in housekeeping; the 20 ms budget is the master's wait for `GET_RPC_RESP_DATA`, and the master sends nothing else between `EXECUTE_RPC` and `GET_RPC_RESP_DATA` (`transactions.c:1035-1046`). The callback is idempotent by offset and seq. Erase, SHA, signature and commit run in slave housekeeping behind the pause, through a single-slot mailbox with a seq number; housekeeping takes the shmem lock when it copies out. The slave runs at full clock during an update (deep idle lowers the clock and takes long naps, `kb/matrix.c:276-292`, `kb/power.c:100`) |
| `updater.c` | `hand` = other side means: stage locally, then relay. `0xFF` if the subside does not answer (`kb/scanlab.c:399-407`) |
| `kb/svalboard.c:533` | Gate `sval_on_reconnect` (D21) |
| Slave | Abort after 5 s with no `KEYBOARD_UPDATE` traffic and restore the LEDs. An image is applied only after COMMIT |

Split ops: `INFO, BEGIN (async erase, paused), PAGE(off,seq,crc16,data), STATUS, END (async verify), COMMIT, ABORT`. Each page goes as several RPCs carrying ≤ 23 B of data (32 − op 1 − off 3 − n 1 − crc 4; `quantum/split_common/transport.h:28`), assembled in the slave's page buffer and programmed on the last fragment. Both halves are the same release, so there is no size negotiation. The subside checks the manifest and the image hash itself (D7).

**Update order (V):** the half without USB is relayed, verified and committed first. Straight after its COMMIT, the half with USB runs the M1 path on itself over USB. It does not wait to hear from the other half, which may not be able to talk to the old firmware. After both have rebooted (about 2.5 s: 500 ms double-tap window at `rp2040.c:50`; 2 s USB detect at `split_util.c:66-75`), presence must show the same version on both halves within 10 s. If the other half doesn't answer, Keybard tells the user to plug USB into it and copy the `.uf2`.

**M2a (no test board):** host tests for relay, mailbox, idempotent PAGE, slave timeout; CI check (P1) that every supported left/right build pairing produces an identical split message table, including each entry's buffer sizes and shmem offsets.
**M2a stage 1 (link layer), built 2026-10-08:** the D15 pause (`kb/split_pause.c`, its own file so `kb/matrix.c`'s five-row declarations and the default builds stay as they are), the `KEYBOARD_UPDATE` wire format and a slave handler that checks every frame and answers retries from a cache (`kb/updater/update_split_wire.h`, `update_split.c`), presence with version and mismatch reporting (STATUS byte 22, INFO flags bit5, red LED while idle), `kb/tools/check_split_tables.py` (P1), and host tests. All of it is `SVAL_UPDATER` only; the 12 default builds are byte-identical to M1's. BEGIN carries the signed manifest in fragments (there is no separate manifest op; 108 B with no signature since v1); PAGE fragments carry 24 data bytes with a CRC-16 over the whole frame. Stage 2: the master relay, the slave mailbox, programming, the 5 s slave timeout and D21. See `docs/updater.md`.
**M2a stage 2 (relay), built 2026-10-08:** `hand` = the other side runs the M1 session on the half with USB against the other half's side and pointing device (from presence), then COMMIT relays (state `RELAYING` 10, `RELAYED` 11) and a second COMMIT commits the other half (`SUBSIDE_COMMITTING` 12, link paused 2 s + 2.5 s). The relay pauses the link for the other half's manifest check, signature, erase, verify and commit, and runs it for the PAGE stream (4 ms of RPCs per pass). The other half programs pages in the callback, does its slow work in housekeeping through a single-slot mailbox with a sequence number (copy-out under the shared-memory lock), verifies the signature itself (D7; the manifest and hash only, in v1), drops a session after 5 s without traffic, keeps full clock and shows the D24 LEDs. D21 is done. New RELAY op (`0x0C`); `sval_update.py update` takes the other half's image, `pair` does both halves. Host tests: `tests/sval_updater/test_relay.c`. See `docs/updater.md`, "The other half".
**M2a stage 2 review fixes, 2026-10-08:** the slave's 5 s timeout reads its clock under the shared-memory lock and compares signed (a request landing between the read and the lock wrapped `now - rx_ms`); RPC lengths from the wire are clamped to 32 B; the slave's page program is refused with BUSY while a DMA channel is busy (R6, below); the slave erases a sector only after a request since its last erase (it cannot answer for about 50 ms per sector, so the master's 2 s link timeout raced the erase and a single ABORT was usually lost); a failed or aborted relay keeps the link paused and sends ABORT until the other half answers (`RELAY_ABORTING`, phase 13); a lost COMMIT answer or an IDLE answer to COMMIT leads to the hold, not a failure; a probe that finds the other half still COMMITTING probes again (bounded) instead of reporting done; DONE without any answer is flagged unconfirmed; after a relay the red LED stays until presence reads MATCH (also through NONE); `pair` checks both images against both halves before sending anything; P1 checks the order of the keyboard RPC IDs through `sval_split_kb_ids` (updater builds).
**M2b (test board):** update the subside, then the main half; pull the split cable mid-relay (the slave aborts within 5 s, both halves stay on the old image, LEDs restore); cut subside power mid-commit (BOOTSEL, recovered over its own USB); a forced version mismatch (Keybard warning plus red LED on the half with USB); measure the RPC round trip and the subside transfer time (target < 30 s); type on the subside during the relay and check no keys are lost outside paused windows, and that a key held at pause entry is released.

**M2b results, 2026-10-08** (images built at 875623fd5a; right test board on USB, the left test board powered and linked over the split cable only):

| Step | Result |
|---|---|
| 1 Baseline (both halves at 2001 over their own USB) | Pass: presence MATCH, `info --other` reads the left half |
| 2 Dry run (`--no-commit`) | Pass: VERIFIED on the right half, the left half untouched. An `abort` from a new tool process is refused (OTHER_CLIENT, M1 design); the session times out after 30 s and ABORT then clears it |
| 3 Left half 2001 → 2002 | Pass. Relay 11.4 s for 125,696 B (target < 30 s), 71 retries, 31 sectors erased. LEDs as specified; the left half types and points after the update with no power cycle (R24). The held-key release at pause entry was not checked |
| 4 Version mismatch | Pass: red 1 Hz on the half with USB, `version differs`; updating the right half to 2002 gives MATCH and clears it |
| 5 `pair` back to 2001 | Pass on the firmware side (relay 11.1 s, 74 retries; both halves 2001, MATCH). The tool crashed reopening the right half while it was still re-enumerating: fixed in ebb16743c3. `pair` with two right-half images refuses and sends nothing |
| 6 Split cable pulled mid-relay | Pass for the power cut: pulled at about 90% of the PAGE stream; the left half kept its old image and boots it. **Pulling the split cable also browns out the half with USB** (its reset reason is power-on/brown-out, twice), so link loss with the master still running (6a, 6c) cannot be produced on this hardware; it is covered by the host tests only. A relay afterwards succeeds (11.1 s, 64 retries) |
| 7 Left half power cut in the commit window | Two cuts, both safe: one after the copy (boots the new image), one before it (boots the old image). The 1-2 s copy itself was not hit by hand; the same routine was cut mid-copy in M1 with test hooks. A deliberate mid-copy cut on the left half needs a TEST_HOOKS left build |

Measurements: relay 11.1-11.4 s for 125,696 B over 6 runs, 64-78 retries each (most during the erase, about 2-3 per sector); USB to the half with USB 17-20 s (mean round trip 1.5-2.2 ms). D13 (split message size) needs no change at these numbers.

Findings:
- The post-COMMIT probe got no answer on every successful relay: the left half is still rebooting at 4.5 s, so DONE is always "unconfirmed" and the host's version check decides. The tool now says so and waits for presence other than NONE before reporting (it printed a false "different releases" note). Firmware unchanged; retiming or dropping the probe is open.
- The split cable carries the left half's power, and pulling it resets the half with USB too: a cable pulled while the half with USB commits its own image interrupts that commit (the M1 BOOTSEL fallback applies).

**Exit:** P1 passes; every M2b case passes; the measurements are reviewed.

### M3: signing, CI, release gating

**History note.** Everything below up to "M3 for v1" was built and tested with signing: Ed25519 signatures, a test key, two DRY RUN release keys and a signing job. Signing was then deferred for v1, and that code is kept under the tag `archive/fw-updater-signed` (its `docs/updater.md` has the "Release signing" section and the fork runbook). What v1 keeps from M3 is in "M3 for v1" at the end of this section.

| Path | Purpose |
|---|---|
| `.github/workflows/build-firmware.yml` | Also produces the raw image and the manifest. A signing job runs in an environment that needs approval, and **signs only release keymaps** with `flags.DIAGNOSTIC` clear (D22) |
| `.github/workflows/release.yml` | Publishes `*.svup` next to each `.uf2` |
| `.github/workflows/updater-tests.yml` | Host tests, a new workflow |
| Lint | Release updater builds: no test key, two release key slots, no test hooks, no `SVAL_KEYTEST`, no `SVAL_HOST_BOOTLOADER`. Golden transaction table matches the last release, including the per-ID buffer size and shmem offset fields of `split_transaction_table` dumped from each ELF |
| Version | `SVAL_FW_VERSION` (numeric + string) from the tag (D17) |

**Exit:** a dry-run tag on the fork produces a signed `.svup` that the M1 tool accepts with the release key and refuses with the test key. The "last manual update" release notes are drafted, following the release wording rules.

**M3 built, 2026-10-09 (everything but the outward steps).** See `docs/updater.md` at `archive/fw-updater-signed`, "Release signing".

| Piece | As built |
|---|---|
| Keys (D30) | Two DRY RUN Ed25519 keypairs; private halves outside the repository (`~/m3-keys/` in WSL, mode 600). Public halves in `kb/updater/update_release_keys.h` (`SVAL_UPDATE_RELEASE_KEYS_DRY_RUN 1`), the only file to change for the production keys. **Every** updater build accepts `key_id 1` and `2`, including `SVAL_UPDATE_TEST_KEY` builds (so a test board can take a signed release image); release builds have no test key. INFO flags bit6 says DRY RUN |
| Build-info record | `sval_update_build_info` (32 B since the review fixes, `kb/updater/update_keys.h`) in every updater build: RELEASE, TEST_KEY, TEST_HOOKS, KEYTEST, HOST_BOOTLOADER and DRY RUN bits, release key slots, updater protocol, `SVAL_FW_VERSION`, the version string, and the build's side, pointing device and keymap. `update_keys.c` also `#error`s on a release build with `SVAL_KEYTEST` or `SVAL_HOST_BOOTLOADER` |
| `make_update.py` | `unsigned` (build job: raw image + unsigned manifest, no key), `sign` (signing job: key from a file or an environment variable; the refusals listed in `docs/updater.md`; sets `key_id` to the slot its key fills; checks its own signature), `verify` (offline check as a release, test or plain build). The manifest's version comes from the image's build-info record. A pure-Python Ed25519 verifier joins the signer |
| Release CI | `release.yml`: version → 12 release updater builds (`build-firmware.yml` with `updater: true`) → lint → publish `.uf2` → sign (environment `release-signing`) and publish `.svup`. Dry-run tags were prereleases (retired by D41: `v` and `x` tags). Details in `docs/updater.md` |
| Lint | `kb/tools/check_release_elf.py` (build-info record, the two key slots, no test key bytes, no test-hook, keytest or host-bootloader symbols) and `check_split_tables.py --golden kb/tools/split_table_golden.json` |
| Version (proposed D32) | `kb/updater/fw_version.txt` (now 3), wired through `rules.mk` into `SVAL_FW_VERSION`; the tag is `SVAL_FW_VERSION_STRING`, readable through INFO page 1. `kb/tools/release_version.py` is the CI check |
| Host tests | `test_release.c` (a release build's key set; optional `.svup` files through `SVAL_RELEASE_SVUPS`), `test_release_version.py`, and new cases in `test_make_update.py`, `test_updater.c` and `test_session.c`. `.github/workflows/updater-tests.yml` runs `run.sh` |
| Release notes | `docs/updater-release-notes-draft.md` |
| Fork setup (D31) | A runbook in `docs/updater.md`, "Fork setup and the dry run"; the main session runs it |

**M3 local results, 2026-10-09** (WSL, Arm GNU 13.2.1; nothing on hardware, nothing pushed as a tag):

| Check | Result |
|---|---|
| 12 default builds, `SKIP_GIT=yes`, `objcopy -O binary` | Byte-identical to `~/m1-baseline-bins`, with `SOURCE_DATE_EPOCH` pinned to the baseline's day: QMK's `command.c` embeds `__DATE__`, which is the only difference on another day |
| 12 release updater builds (`SVAL_UPDATER=yes SVAL_UPDATE_RELEASE=yes`) | All build; images 121,856-126,464 B; `check_ram_funcs.py` OK on all 12 (`update_commit.c` unchanged) |
| Lint | `check_release_elf.py`: 12 of 12 pass; a test-key, a test-hooks and a plain updater build each fail with the expected reasons. A release build with `SVAL_HOST_BOOTLOADER=1` does not compile. P1: 36 of 36 pairings match, 12 of 12 match the golden table (15 transactions); default-build ELFs fail it (14 transactions, no keyboard RPC IDs) |
| CI steps by hand | The version job against the fork's real releases (`vM3-dryrun1`: fw 3, previous vRC2 at 0, prerelease; `vLaunch3` refused for the DRY RUN keys), the build job's `unsigned` step, the lint job, the sign loop with the key in an environment variable, and the verify loop: 12 `.svup` files, all accepted by `verify --build release` |
| End to end | A release image signed with DRY RUN key 1 and key 2: accepted by the C code of a release build (`test_release.c`, 15 files); the same image signed with the test key: refused (BAD_SIG) by a release build, accepted by a test build; a test-key signature relabelled `key_id 1`: refused; the signer refuses the test key and the test-key, test-hooks and plain updater builds |
| Host tests | `util/updater_test/run.sh` passes under ASan and UBSan (make_update.py 149 checks, release_version.py 21, test_updater 255,380, sval_update.py 17,774, test_commit 1,120,565, split 639, release key set 20) |
| Workflows | actionlint 1.7.7 with shellcheck: only info-level findings in steps that predate M3; yamllint clean apart from the existing indentation style |

Not done here (the main session's outward steps, D31): the `release-signing` environment and secret, the dry-run tag and its run on GitHub, and the exit check on hardware (a test board on a release updater build takes the dry run's `.svup` with `sval_update.py` and refuses a test-key image).

**M3 review fixes, 2026-10-09.** A review of the M3 build found eight problems; all eight were real and are fixed:

| Finding | Fix |
|---|---|
| Blocker: in CI's container (GCC 15.2) `check_ram_funcs.py` fails on all 12 release builds (`commit_ram_main` becomes the clone `commit_ram_main.isra.0`), so every release run would fail | `build-firmware.yml`, for updater builds only: the image pinned by digest, and Arm GNU 13.2.rel1 downloaded, checked by SHA-256 and put first on `PATH`; the build fails unless `arm-none-eabi-gcc -dumpversion` is 13.2.1. `update_commit.c` and `check_ram_funcs.py` unchanged |
| The release image was unpinned, so published images were not the tested ones (R28) | As above. The 12 builds in that image with that toolchain are byte-identical to the local ones (`SKIP_GIT=yes`) |
| Checkouts used `ref: github.ref`, so a tag moved during the approval wait would change what the signing job runs | Every release checkout uses `github.sha`; `kb/tools/check_tag_commit.sh` (lint and sign) fails if the tag no longer names the run's commit; the `release tags` ruleset (D31 step 1) blocks moving or deleting `v*` tags except by admins; the runbook tells the approver to check the commit |
| The DRY RUN guard was one flag: a half-done swap (flag 0, a DRY RUN key left) would pass every gate | `kb/tools/retired_release_keys.txt` lists the DRY RUN public keys outside the header; `make_update.py release_keys()` refuses a production header (flag 0) holding any of them, so `release_version.py`, `check_release_elf.py`, `sign` and `verify` all stop it. Host tests cover the flag-only and one-slot swaps |
| The signer could not tell whether the image was the one the lint checked, and the image carried no side, pointing device or keymap | `kb/tools/check_release_artifacts.py` (lint): each `.img` is its ELF's flash contents and its `.uf2`'s image, the manifests and build-info records match the build names, the file set is exactly the 12 builds; the 12 SHA-512s go to the signing job as a job output, and `sign` requires `--expect-sha512`. Build-info record version 2 adds side, pointing device and keymap, cross-checked by `unsigned` and `sign` |
| The build jobs had `write-all` and kept Git credentials | `release.yml` defaults to `contents: read`; only `publish` and `sign` get `contents: write`; every checkout uses `persist-credentials: false` |
| The release notes draft said an interrupted update always ends in bootloader mode | Now: it keeps its old firmware or starts in bootloader mode |
| `.uf2` files went public before signing; a rejected signing run left a public UF2-only release | `publish` makes a draft (not public); `sign` checks the draft holds exactly the linted `.uf2` files, uploads the `.svup` files, checks there are 24, and only then publishes it |

Results after the fixes (WSL, Arm GNU 13.2.1, `SOURCE_DATE_EPOCH` pinned as before):

| Check | Result |
|---|---|
| 12 default builds | Byte-identical to `~/m1-baseline-bins` |
| 12 release updater builds | All build (121,600-126,208 B); `check_ram_funcs.py` 12 of 12; `update_commit.c` unchanged since 594f5e7118 |
| The same 12 in the pinned CI image with the pinned toolchain | `check_ram_funcs.py` 12 of 12; images byte-identical to the local builds |
| Lint | `check_release_artifacts.py` 12 of 12, and it refuses: a flipped image byte, a left image as right, a blank image as sval, an extra file, a missing `.uf2`, a `.uf2` or ELF that is not the image. `check_release_elf.py` 12 of 12. P1 36 of 36 pairings, golden 12 of 12. The M2b left builds (2001, 2002) pair with the release right (15 transactions) |
| Signing | The sign loop with DRY RUN key 1 from an environment variable and the lint's hashes: 12 signed, `verify --build release` 12 of 12. Refused: an image that is not the linted one, a left image as right, a blank image as sval, a header with DRY_RUN 0 and the DRY RUN keys (also refused by `check_release_elf.py`) |
| End to end | The 12 release `.svup` files accepted, and a test-key `.svup` of the pmw3389 right image refused, by a release build's C code (`test_release.c`, 46 checks); `verify --build test` accepts the test-key file |
| `check_tag_commit.sh` | Against the fork's annotated tag vRC2: accepts its own commit, refuses another (read-only `gh api`) |
| Host tests | `run.sh` passes under ASan and UBSan |

**M3 for v1: signing deferred.** v1 ships unsigned. Removed: the Ed25519 check and the rest of Monocypher (only SHA-512 stays, in `vendor/sha512.c/.h`), the test key and `SVAL_UPDATE_TEST_KEY`, the release keys header and the retired-keys list, the signing job, its environment and secret, `make_update.py`'s key options and its `unsigned` and `sign` steps, and the fork-setup and secret scripts. The format moved to protocol 2: the manifest travels alone (108 B), byte 6 is reserved and must be 0, version-1 manifests are refused as UNSUPPORTED, status 8 is retired, INFO flags bits 4 and 6 and STATUS bytes 13-14 are reserved. The build-info record moved to `kb/updater/update_build_info.c/.h` as version 3 (byte 6 reserved; flags 0x02 and 0x20 reserved). Kept: the manifest checks, the chord on the manifest hash, the image SHA-512 at verify, at commit step 0 and on the half without USB, the CRC bound to COMMIT, the structure checks, the RAM commit, the split pause, the LEDs, the `SVAL_UPDATE_RELEASE` guards and the build-info lint. `docs/updater.md`, "Security", says what this does and does not protect.

Release CI for v1 (`release.yml`): version (`release_version.py` and `kb/updater/fw_version.txt`; dry-run tags were prereleases, now `v` and `x` tags (D41); no key checks) → 12 release updater builds (2 since D34: `svalboard/left:sval` and `svalboard/right:sval`, the auto builds; 4 since D40, with `svalboard/left:blank` and `svalboard/right:blank`) in the pinned image with the pinned toolchain, each running `check_ram_funcs.py` and `make_update.py --release`, which writes the `.svup` next to the `.uf2` → lint (`check_tag_commit.sh`, `check_release_artifacts.py`, `check_release_elf.py`, `check_split_tables.py --golden`) → publish: one job creates the release directly (not a draft) with the 12 `.uf2` and 12 `.svup` files. No signing job, environment or secret. `kb/tools/m3/check_release.sh` checks a published release's assets.

### M4: Keybard UI (fork `morganvenable/keybard`, PR to `svalboard/keybard`)

- New `src/services/updater.service.ts`, a sibling of `scanlab.service.ts`.
- A send path with **no** silent re-bootstrap retry (`usb.service.ts:700-722`). Client-ID renewal keeps its normal 50 s period (`usb.service.ts:17,490,586-596`); after each renewal the updater service sends `REBIND [hand][nonce]` before its next op (D3).
- UF2 unpacking, browser-side signature check (deferred with signing), the manifest hash shown before the gesture, progress from STATUS, subside first, ≥ 10 s reconnect wait.
- An image built for another hand or pointing device than the half reports is a warning the user confirms, never a refusal (D33).
- **Layout backup (D35):** before flashing, Keybard saves the board's layout (keymap, macros, combos, tap dances, overrides and the other stored settings it can read). After the update it offers to restore that backup or to start from the new firmware's defaults.
- Test on [keybard-test](https://morganvenable.github.io/keybard-test/) before production.

---

## 3. Decision record (answered 2026-10-08, revised in rev 4)

Your answers from the [decision page](https://claude.ai/artifact/WsV3uM75FY2iQ1p1XEsGsX), applied throughout this revision. Your notes are quoted.

### Policy

| # | Decision |
|---|---|
| **V** | **Both halves always run the same release.** Mixed versions are not a supported configuration. The half without USB is updated first. Straight after its commit, the half with USB updates itself over USB without waiting to hear from the other half; both are checked once both have rebooted. On a version mismatch, Keybard warns and the half with USB shows the red error LED until it's fixed. Consequences: no transaction-table freeze (old D16 dropped), no buffer-size negotiation, no RPC bounds-check patch. |
| **P1** (new, follows from D18) | Halves of one board can run **different builds** of the same release (e.g. left trackball, right TrackPoint). CI checks that every supported left/right pairing of builds produces an identical split message table (`NUM_TOTAL_TRANSACTIONS`, the order of IDs, and each entry's buffer sizes and shmem offsets). Each side uses its own table's sizes on the wire (`serial_protocol.c:75-76,87-88,142-150`), so a struct size change with the same IDs would desync the halves. A pointing-device option that adds a split message on one side would fail this check. All 12 release builds matched; since D34 a release has one left and one right build, so this is the one pairing. |

### Answered

| # | Decision | Answer |
|---|---|---|
| D1 | M1 test unit | **Right test board half**, which has a reset button, so M1 #11 is in. M0 still checks its flash chip is 16 MiB |
| D2 | Host channel | **Keyboard-local VIA channel**, like Scan Lab. The updater enforces the client wrapper itself, through a small getter in `client_wrapper.c` |
| D3 | Session identity | Client ID plus the ARM nonce. **Revised in rev 4:** Keybard keeps its normal 50 s client-ID renewal and, after each renewal, sends `REBIND [hand][nonce]`, which re-binds the session to the new ID. Pausing renewal would not work: the firmware rejects IDs older than 120 s, measured on `timer & 0xFFFF0000`, so an ID really lives 65.5-131 s (`client_wrapper.c:24-47,136-138`), and a session (30 s gesture wait, erase, about 5,300 CHUNKs, verify) can outlive it |
| D4 | Confirmation | **Index South + Middle South, held 1 s**, on whichever half has USB: right `[6,0]`+`[7,0]` (`M` and `,` on the default keymap), left mirrored `[1,0]`+`[2,0]` (`V` and `C`). Read from the matrix, never keycodes; both keys are swallowed while waiting. Your note: *"flash the LEDs or something to register this event"*. So when the chord is recognized, the LEDs flash bright white twice (about 300 ms) before moving to the next state |
| D5 | Chord binding | The chord approves one specific image (its manifest hash) |
| D6 | Signatures in M1 | Ed25519 with a test key from the start. **Deferred for v1:** built and tested, then removed (`archive/fw-updater-signed`) |
| D7 | Crypto | Monocypher on both halves (`crypto_ed25519_check`); revisited for M2 once M1 has timed it. **v1:** the signature part is deferred; each half still checks the manifest and the image's SHA-512 itself |
| D8 | Staging slot | `0x800000`, size `0x160000`. It lies inside the free region `0x260000`-`0xFFDFFF` (7e0af09 map); asserted against `WEAR_LEVELING_RP2040_FLASH_BASE + WEAR_LEVELING_BACKING_SIZE * WEAR_LEVELING_COPIES` and `IDENTITY_SECTOR_A` |
| D9 | Watchdog during commit | On, fed after every flash operation. Arming sets `PSM_WDSEL` first, loads `period_µs × 2` and clears `PAUSE_DBG` (step 1) |
| D10 | Reset | Watchdog-triggered reset from RAM, with `PSM_WDSEL` set. Not `mcu_reset` (AIRCR `SYSRESETREQ`, `platforms/chibios/bootloaders/rp2040.c:16-18`), which runs from flash and whose reset scope on RP2040 is UNVERIFIED |
| D12 | Settings before commit | **Answered after rev 4:** no flush. Settings writes go straight through to flash (`sval_save()` is a no-op; eeconfig and dynamic-keymap write through), so there is nothing to flush. The one failure mode is a whole boot where the wear-leveling backing store failed to initialize and every write silently stayed in the RAM cache. [svalboard/qmk#12](https://github.com/svalboard/qmk/pull/12) latches that as `wear_leveling_write_failed()` (Sval `GET_INFO` byte 18 bit 1). The updater refuses `ARM` with `UNAVAILABLE`, and the commit refuses in step 0 with no erase, while that latch is set. **qmk#12 is a prerequisite for M1.** |
| D14 | Relay model | Store and forward |
| D15 | Pausing the split link | **Svalboard overrides the weak `matrix_scan()`** and skips the split exchange while paused. No QMK core change. Pause entry clears the slave rows once; resume calls `split_watchdog_update(false)` so a rebooted slave is re-pinged |
| D17 | Version in presence | 4-byte version number plus short git hash in presence; full string through INFO |
| D18 | Hardware ID | **One field per component, per half.** Each half's image names only its own pointing device (`pointing_id`) plus its side, and each half's image drives only its own device. Switching to a different pointing device is allowed after a Keybard confirmation naming both. The keymap (`sval`/`blank`) is recorded for display only and never checked. **Revised by D33:** the firmware checks neither field; the confirmation is the host's |
| D19 | Downgrades | Any signed image at or above the security floor and the v2 settings format (v1: any image, signing deferred) |
| D20 | LED takeover | A quick experiment in M1 decides between holding off rgblight output and overriding the WS2812 flush. First build check: `RGBLIGHT_DRIVER = custom` (`common_features.mk:312`; `rgblight_drivers.c:6-14`); fallback `rgblight_disable_noeeprom()` (`rgblight.h:326`) |
| D21 | Settings write on reconnect | Skipped during an update; the general flash-wear fix becomes a separate issue |
| D22 | Signing scope | Release keymaps of every variant only. **v1:** signing deferred; release CI still makes `.svup` files for the release keymaps only |
| D23 | Computer sleeps mid-update | Abort before the copy; ignore once the copy has started |
| D24 | LED colors | As proposed: blue blink (waiting for the chord), white double flash (chord recognized, from D4), rainbow (erasing and receiving), solid magenta (writing), red (error). Your note: *"1Hz 50/50 duty cycle"*, so the error blink is 500 ms on, 500 ms off. Fixed brightness about 96. Revised 2026-10-23 (owner-approved): one meaning per color, red only for failure; rainbow on both halves until they match, magenta only during a commit from RAM, green double flash when done, red for 30 s only for a failed install or halves still different 60 s after an update, no light for a kept pair record; the solid blue "approved" state is gone. See `docs/updater.md`, LEDs |
| D25 | Build flags | `SVAL_UPDATER` and a separate `SVAL_UPDATE_TEST_HOOKS`, both off by default |
| D26 | Flash chip not 16 MiB | Updater reports "unavailable"; Keybard falls back to the guided manual copy |
| D27 | M1 host tool | Python, modeled on `keytest.py` |

### Answered after the M1 code review (2026-10-08)

| # | Question | Answer |
|---|---|---|
| M1-a | Main stack is 0xAE0 (2,784 B), not 0xC00: SRAM4 cannot hold 3 KiB | Accept. Measure the high-water mark with the `DIAG` op on the test board (M1 #13); move the stack only if the margin is short |
| M1-b | The Ed25519 check and commit step 0's slot hash each block the main loop for longer than 50 ms (estimated 0.1-0.3 s and about 0.25 s per 100 KB) | Accept: once per update, under Keybard's 1000 ms, at moments nothing needs the keyboard. Measure in M1 #13. Signatures stay: they stop host software from installing its own firmware. **Superseded for v1:** signing deferred, so the Ed25519 pass is gone; the slot hash remains |
| M1-c | Conservative commit changes: per-sector erase in block 0, per-sector compare and retry in step 5, page 0 zeroed when step 6's read-back fails, 8 s watchdog, full SRAM0-3 magic sweep | Accept |
| D28 | Where the copy runs: in the running app from RAM (as built), after a reboot into an early-boot copier, or a permanent stage-1 bootloader that resumes after a power cut (outside review) | **Keep as built**, answered after the M1 hardware results |
| M1-d | Storage-format floor after `kb/storage/` was removed | `SVAL_UPDATE_STORAGE_FORMAT = 2`, a plain number raised by hand when a release changes saved settings incompatibly; not tied to `EECONFIG_MAGIC_NUMBER` |

### Answered for M3 (2026-10-09)

D29, D30 and D31 are about signing and are **deferred for v1**; the answers are kept as the record of what was built (`archive/fw-updater-signed`).

| # | Question | Answer |
|---|---|---|
| D29 | Where the signing key lives in CI | (Deferred for v1: no signing job, environment or secret.) A secret (`SVAL_UPDATE_SIGNING_KEY`) in a GitHub environment named `release-signing` that needs Morgan's approval for each run. The build jobs never see it; only a separate signing job in that environment does, and it signs only release keymaps (D22) with `flags.DIAGNOSTIC` clear, refusing anything else |
| D30 | Keys for M3 | (Deferred for v1: no keys in any build.) Throwaway DRY RUN keys in both release slots (`key_id 1`, `2`), compiled into release updater builds; the production keys are generated later and swapped in (the public keys in `kb/updater/update_release_keys.h`, and the secret). Release builds carry no test key. The swap is a one-file change, documented in `docs/updater.md` |
| D31 | Fork setup | (Deferred for v1 with signing. Dry-run tags are retired by D41: `x` tags are the trial run.) Done by the main session from the runbook in `docs/updater.md`: the environment, the secret, and a dry-run `v*` tag (such as `vM3-dryrun1`) whose release is a prerelease. Dry-run tags never produce a normal release |

### Answered 2026-10-12 (pointing_auto, svalboard/qmk#21)

| # | Question | Answer |
|---|---|---|
| D33 | Should the firmware refuse an image whose hand or pointing device differs from the running build (UNSUPPORTED, WRONG_HW)? | **No: warn, don't refuse.** The firmware enforces integrity and structure only (hash, manifest format and version, length, boot2, SP and reset vector, epoch and storage floors), on the half with USB and on the half reached over the split cable. Moving an MCU between hands, or between pointing modules, is a valid use case. The manifest keeps `hand` and `pointing_id`, and INFO and presence still report them; the request's hand picks the half. `sval_update.py` and `make_update.py` print a warning for every mismatch (auto against a single-module build, either way, included) and ask (y/N); `--yes` skips the question. `pair` still picks each image by the hand each half reports. WRONG_HW (7) is retired, like BAD_SIG |
| D34 | Release matrix | **Two images:** `svalboard/left:sval` and `svalboard/right:sval`, the auto builds with every pointing driver, as release updater builds. The per-module and `blank` builds are no longer released (they still build). **Revised by D40:** the `blank` builds are released again `SVAL_TEST_WIPE` is for bench builds only, never a release |
| D38 | Release tag names and graduation | **Superseded by D41.** Release CI accepts `vLaunch<N>` (stable), `vNext<N>` (prerelease) and dry-run tags only; any other `v*` tag is refused. `vLaunch<N>.<M>` names Keybard and Keybard Host releases only, never firmware. The tagged commit must be on `svalboard`, and only `svalboard/qmk` publishes. A vNext graduates by a rebuild: `fw_version.txt` raised above every vNext, a dry run, then the `vLaunch<N>` tag. A vNext release is never edited into a non-prerelease |
| D36 | Bleeding-edge firmware | **Superseded by D41** (`x` tags replace `vNext`). `vNext<N>` tags (`vNext1`) publish as prereleases, for next.keybard, so they never become the latest release. Their numbers count: the next stable release must be higher than every `vNext` before it. Dry runs still use up no number. The 64-byte build goes out this way first, and becomes a stable release after user feedback on flashing |
| D35 | Settings across an update (M4) | Keybard backs up the board's layout before flashing, and afterwards offers to restore it or to start from the defaults |
| D39 | Settings when the firmware changes | **Reset to the new build's defaults.** Supersedes the earlier aim that settings survive updates. The first boot of a different image (an FNV-1a hash of the image in flash, compared with a record at `0xFFD000`, outside the settings and below the identity sectors) resets every stored setting on that half; the identical image keeps them; Vial and older Svalboard QMK, which never write the record, count as different. Board identity and updater state are kept. GET_INFO byte 18 bit 2 tells the host until `0x2A`. No record on a die under 16 MB: settings kept (the EECONFIG magic and Sval stamp still reset an incompatible layout). A record that can't be written resets again next boot and sets byte 18 bit 1. Keybard (M4) and a command-line tool save a `.svil` before flashing and restore it after. Dev `keep` builds (fork) skip the check |
| D41 | Release tags: `v` and `x` | **Replaces D36 and D38, and the dry-run tags of D31 and D32.** `v<N>[.<M>...]` (`v3`, `v3.1`) builds a full release, published as a draft for a person to publish; `x<N>[.<M>...]` (`x3`, `x3.1`) builds a prerelease for next.keybard.svalboard.com, published at once. Any other tag is refused. Both must be on `svalboard`. The same tag name marks the firmware, Keybard and Keybard Host releases. `x` numbers count, so a `v` tag is a rebuild with `fw_version.txt` raised. `vLaunch` and `vLaunch2` keep their releases. A failed run is re-run on the tag (`workflow_dispatch` with the same tag) |
| D40 | Blank test images | **Released again.** Every release also publishes `svalboard/left:blank` and `svalboard/right:blank` (keymap ID 2), the auto builds with no key bindings, as release updater builds with their `.svup` files, so a customer can flash a test image and check every key after building. A release is four images; the lint checks every left/right pairing, sval or blank on either side. Revises D34 |

### Answered 2026-10-15 (Firmware and Settings, round 2)

Updater protocol 4. The byte-level contract Keybard and the firmware share is
[updater-protocol4.md](updater-protocol4.md); `docs/updater.md` describes it
with the rest of the protocol.

| # | Question | Answer |
|---|---|---|
| D41 | One chord for both halves | **One chord approves a pair update.** The first session also carries the USB half's own manifest (PAIR_MANIFEST, op `0x0D`), and its ARM (mode 1) checks both manifests against both halves before any chord is asked for, so the user never approves a file either half would refuse. The chord creates a held approval in RAM: SHA-512 of the USB half's manifest, the client ID and the nonce. It lasts 120 s after the relay is DONE (the contract allows 90-180 s; Keybard sends the second ARM within 60 s). The USB half's own ARM with that very manifest, from the bound client, is then pre-approved (ARM flags PRE_APPROVED and BY_PAIR, no blue light, BEGIN at once), and any ARM for that half uses the approval up. REBIND moves it in any state, IDLE included, and is now also answered while the other half commits. ABORT, any error, a MANIFEST from another client or for the other hand, a reset and the timeout clear it. STATUS byte 23 reports it. Nothing may ask for the chord twice: a host that finds the approval gone stops and offers to finish the USB half with its own chord. A one-half update keeps one chord, as before. The protocol number itself (4) is the gate, not a capability byte: tools that speak only protocol 3 refuse protocol 4 boards |
| D42 | Settings reset from the host | **SETTINGS_RESET (op `0x0E`), through the D39 path.** Only in IDLE, with no held approval and no relay. The other half first (a new KEYBOARD_UPDATE op, `0x07`), then this half invalidate their build reset record with one page program (no erase: under 1 ms, so the half without USB does it in the SlaveThread), and both reset within 500 ms. Each half's next boot resets every stored setting exactly as for a different build: the keymap, macros, combos, tap dances, overrides, QMK settings and the keyboard's VIA custom values (layer colors, pointing and scan settings), which a QMK EEPROM reset erases with the rest. Identity and the updater state are kept, and GET_INFO byte 18 bit 2 is set until `0x2A`. With no answer from the other half (unplugged, or on protocol 2 or 3, which do not know the op), UNAVAILABLE and nothing changes. Dev keep builds honor it. `sval_update.py reset-settings` sends it; Keybard sends it for "Reset to the default layout" when the install did not already reset settings |
| D43 | No-chord dev builds upstreamed | **`SVAL_UPDATE_NO_CHORD=yes` is in the tree, for development only.** Every ARM counts as confirmed (INFO flags bit 7; ARM flags PRE_APPROVED and NO_CHORD; no blue light). Its build-info record has flag `0x40`, and `SVAL_DEV_KEEP_SETTINGS` (keep installs) `0x80`. Release builds refuse both at three gates: a `rules.mk` `$(error)`, `#error` guards in `update_build_info.c`, `update_gesture.c` and `build_reset.c`, and release CI (`check_release_elf.py` on the symbols and the record, `make_update.py --release` on the record). `util/sval_dev_flash.sh` builds them and marks their `.svup` files DIAGNOSTIC, so a board on a release build refuses them (the first dev install on such a board goes through BOOTSEL). The per-build dev reset token the fork's dev branch had is not carried over: D39 replaced it |
| D44 | A blank board from the host | **SETTINGS_RESET mode 1 (`req[6]`; 0, or a missing byte, is D42's reset to defaults).** Each half programs the blank mark (`"BLNK"` and its complement, the 8 bytes after the build reset record) with the record's invalidation, in one page program. Its next boot resets every stored setting as D39 does, then writes `KC_NO` over every key of every layer, then records the image (which erases the mark). The firmware does it, not the host, because it is atomic per half and covers both halves: a host writing `KC_NO` after a reset reaches only the USB half's keymap, and a closed tab mid-write leaves part of the old defaults. The other half takes the mode as a fifth byte of KEYBOARD_UPDATE `0x07`; a protocol 4 half from before D44 answers INVALID, so the request is UNAVAILABLE and nothing changes. Mode 0 programs zeros over the mark bytes, so an unfinished blank reset never turns a reset to defaults into a blank one. `sval_update.py reset-settings --blank` reads the keymap back |
| D45 | The image's own build info on a release board | **A release build reads the build-info records in the image it is about to install, and refuses a no-chord (`0x40`) or kept-settings (`0x80`) image with BAD_IMAGE, whatever the manifest says.** The scan runs while the image is hashed (VERIFYING_IMAGE), on the half with USB for either half's image and again on the half without USB after the relay. It matches a record as `make_update.py` does (`"SVBI"`, `info_ver` 3), so the scanner never plants a record of its own; an image with no record is taken. The check cannot run at ARM, since the image arrives after the chord: it comes after the chord and before anything is committed. It stops tool-made and hand-made manifests; a record patched out on purpose is the chord's to stop |

### Proposed, needs your confirmation

| # | Proposal |
|---|---|
| D32 | **Version numbers.** (Tag names and prereleases revised by D41: `v` and `x` tags, `x` numbers count, no dry-run tags.) Launch tags are names (`vLaunch2`), not numbers, so `SVAL_FW_VERSION` comes from a committed file, `kb/updater/fw_version.txt` (one integer; 3 now, so vLaunch and vLaunch2 count as 1 and 2), and the version string is the tag name (at most 16 characters). Raise the number in the commit you tag. Release CI fails if it is not greater than the number committed at the previous published release (the newest non-draft, non-prerelease release that is not this tag; a release from before the file existed counts as 0). Prereleases, including dry runs, are not compared, so they never use up a number. A tag containing `dryrun` or `dry-run` is a dry run: its release is a prerelease, and while the release keys are the DRY RUN keys every other tag is refused (that refusal went with signing in v1) |

### Still open, by design

| # | Decided when |
|---|---|
| D11 | USB chunk pacing: after the M1 #13 measurements |
| D13 | Split message size: after the M2b measurements. Size negotiation is no longer needed (V) |
| D16 | Dropped (V). Replaced by P1 |

---

## 4. Risks and mitigations

| # | Risk | Mitigation / test |
|---|---|---|
| R1 | Power cut during erase leaves a valid boot2 with garbage behind it (boot loop; only physical BOOTSEL helps) | Zero page 0 first and check it (step 2). A zeroed or blank page 0 fails the CRC, so the ROM falls through to USB boot; a partial zero-program leaves a valid CRC only by chance (about 2^-32). Mock tears; M1 #8 N=−1 and N=0 |
| R2 | Branch into flash while XIP is off (veneers, `blx rN`, libgcc case helpers) | Flags, no switch, `check_ram_funcs.py` with an allow-list and veneer following. Only the divider and int64 helpers are wrapped (`RP2040.mk:85-98`) |
| R3 | Fault while the vector table is in erased flash | RAM VTOR table, all 48 slots → watchdog reset. M1 #10 |
| R4 | Commit takes longer than the watchdog period, or the watchdog resets nothing | Feed after each ROM op; load ≥ 2 × the worst-case op, ≤ 8.39 s; `PSM_WDSEL` set before arming. Max op time measured in #13; M1 #9 and #15 |
| R5 | Stack overflow in RAM code or in the Ed25519 verify (gone in v1) | Dedicated sector buffer; `-fstack-usage` ≤ 512 B checked; main's stack raised in `SVAL_UPDATER` builds (`0xAE0`, kept in v1), margin measured with a canary (M1 #13) |
| R6 | DMA or core 1 reads XIP during flash ops | Core 1 never starts (`platforms/chibios/boards/GENERIC_RP_RP2040/configs/mcuconf.h:36`). DMA users are WS2812 (RAM source, `ws2812_vendor.c:138,297`) and the PMW SPI, whose TX source `dummytx` is in XIP (`lib/chibios/os/hal/ports/RP/LLD/SPIv1/hal_spi_lld.c:55,312,396`) but idle whenever housekeeping runs (SPI calls are synchronous, `platforms/chibios/drivers/spi_master.c:463-467`). I2C uses no DMA. The PMW SROM upload is CPU byte writes (`drivers/sensors/pmw33xx_common.c:131-133`). Mitigation: commit only from housekeeping, BUSY check and refusal with no erase in step 1. **M2 exception:** the half without USB programs relayed pages in the SlaveThread (R16), which can preempt the main thread while its SPI DMA runs (`spiReceive` suspends it). So `update_split_port_program_page()` checks every DMA channel's BUSY bit with interrupts off, in the same section as the page program, and refuses with BUSY (the fragment is given back and sent again). Erase and commit on that half still run from housekeeping |
| R7 | Malformed image boots into garbage | Device-side structure checks (M1); `make_update.py` refusals; M1 #6 |
| R8 | Wrong-hand image passes (a half already on the wrong build) | Documented: the check is build-vs-image only. Recovery is a normal UF2. Option: record the side in identity at manufacture (deferred) |
| R9 | Updater erases settings or identity (offset vs XIP mixups; flash map in `kb/docs/settings-storage.md:5-11`) | Static asserts, RAM range guard, mock invariants (a)-(c), picotool dumps byte for byte |
| R10 | Unauthenticated ops or session hijack from a web page | Wrapper required, session bound, ABORT bound with no erase, gesture tied to the hash, signature (deferred in v1: the chord is the only gate on what is installed, see `docs/updater.md`, "Security"), compiled off by default. The client ID is not a secret (D3) |
| R11 | Host forges the gesture (`kb/keytest.c:76,104` `action_exec`) | Matrix-level detection; `#error` on KEYTEST + UPDATER |
| R12 | Downgrade to a vulnerable, pre-v2 or diagnostic image | Epoch, storage floor, DIAGNOSTIC flag in release builds (M3 signing scope deferred in v1) |
| R13 | Every HID op times out (> 1000 ms) | Async BEGIN and END; 50 ms host test |
| R14 | Two builds that are meant to pair (e.g. left trackball, right TrackPoint) disagree on the split message table | P1 CI check across supported pairings, sizes and offsets included |
| R15 | One half sends a larger RPC than the other expects | Cannot happen: both halves are the same release (V), and P1 checks pairings |
| R16 | Slave misses RX while IRQs are off for a page program | Program inside the RPC callback, in the SlaveThread (master idle), so there is no async IRQ-off window. The RX FIFO is 8 deep (joined, `serial_vendor.c:386`) at 1 Mbaud (`kb/config.h:170`): about 80 µs of headroom. M2b measures errors |
| R17 | Abandoned slave session; callback/housekeeping race | 5 s slave timeout; mailbox with seq; shmem lock on copy-out |
| R18 | Master stalls for about 2 s after subside COMMIT | Stay paused for commit time + 2.5 s. M2b measures |
| R19 | Keybard renews the client ID mid-session, or the ID expires | D3: normal renewal plus `REBIND` with the nonce. Host tests; M1 #16 |
| R20 | Updater LEDs overwritten by rgblight sync | D20 |
| R21 | Deep idle or clock change mid-update | Forced full clock on both halves. M1 #12 |
| R22 | Estimates wrong (HID and RPC round trips, erase time, verify ms) | Measured in M1 #13 and M2b. Plan revisited before M2 and M4 |
| R23 | Stale double-tap magic: after the watchdog reset the new image finds `0xCAFEB0BA` in noinit RAM (old image bytes) and enters BOOTSEL. Deterministic for a given image pair | Zero the image buffers before the trigger (step 7). M1 #14 |
| R24 | Peripherals keep state across the watchdog reset: external sensors stay powered (UNVERIFIED, datasheet only) | The new image initializes them as at power-up. M1 #4 and M2b check typing and pointing after an update with no power cycle |
| R25 | The signing key leaks from CI (deferred with signing in v1) | The key is only in the `release-signing` environment's secret, and only the signing step's environment holds it; only the signing job runs in that environment, and each run needs approval (D29). The signing job runs the run's own commit (`github.sha`, never the tag by name), checks the tag still names it, and a ruleset stops `v*` tags moving, so the approved commit is the one that runs; approve only commits whose code you trust. The build jobs cannot write to the repository. A leaked key is retired by a release signed with the other slot that replaces it, and added to `retired_release_keys.txt` |
| R26 | A normal release goes out signed with the DRY RUN keys (deferred with signing in v1), or a dry run becomes a normal release | Release CI refuses every non-dry-run tag while `SVAL_UPDATE_RELEASE_KEYS_DRY_RUN` is 1; dry-run tags are always published as prereleases (`release_version.py`, host-tested). A half-done swap (flag 0 with a DRY RUN key in either slot) is refused by every release tool through `retired_release_keys.txt` |
| R27 | A release ships a split table its other half cannot talk to | `check_split_tables.py` on every pairing of the release ELFs plus the committed golden table, in release CI before publishing |
| R28 | Release images built with another toolchain than the tested one (the RAM commit code changes with the compiler) | Release updater builds use a pinned image and Arm GNU 13.2.rel1, checked by SHA-256 and version; changing either means rerunning `check_ram_funcs.py` and the hardware commit tests. The M3 exit check runs the CI-built files |
| R29 | The published image is not the linted one (a compromised build job, an extra artifact). v1: the lint checks each `.svup`'s image against its ELF and `.uf2` | `check_release_artifacts.py`: image = ELF flash contents = `.uf2` image, exact file set, manifests and build-info records match the names; the signer takes the lint's SHA-512s from a job output (`--expect-sha512`) and cross-checks side, pointing device and keymap against the image |

---

## 5. Out of scope / deferred

- keybard#24 options A (guided copy) and B (WebUSB PICOBOOT).
- A/B slots, rollback, monotonic anti-rollback counters, encryption, delta updates.
- Runtime hardware detection (fragments stay a stub); recording the side in identity (R8).
- `sval_on_reconnect` wear fix (D21b); the stale `SVAL_HOST_BOOTLOADER` comment and `docs/scan-lab.md` reference (`kb/scanlab.h:39`) as separate issues.
- Svalboard's `bootmagic_lite()` override at `kb/svalboard.c:551` is dead code: core now calls `bootmagic()` and the weak `bootmagic_scan`, so bootmagic is live in release builds. A separate issue, unrelated to the updater.
- Keybard Paranoid support; updating boards that still run Vial firmware.

## 6. Where the work happens

- **Firmware:** the WSL clone of [morganvenable/sval-qmk](https://github.com/morganvenable/sval-qmk) (with submodules), branch `feat/fw-updater` from `upstream/svalboard`, then a PR to `svalboard/qmk`. The Windows reference clone (`GitHub/svalboard-qmk`) stays read-only.
- **Builds:** Ubuntu WSL only. Do not run concurrent `make` on the same target (they share `.build`). CI: only the Svalboard release and lint workflows, plus the new updater host-test workflow.
- **Hardware:** the test board only (D1), **never the daily-driver right half**. Recovery is BOOTSEL + `kb/tools/flash.sh`.
- **Keybard (M4):** a worktree of `GitHub/keybard-fork` from `upstream/main`, a PR to `svalboard/keybard`, keybard-test first.
- **Gates:** all decisions are answered (Section 3). [svalboard/qmk#12](https://github.com/svalboard/qmk/pull/12) must be merged (or the branch rebased onto it) before M1 code. M0 hardware results come to you before M1 code starts; M1 #13 numbers come to you before M2.

---

## Appendix: reviewer notes

**Revision 4 (M0 findings, against `7e0af09`):**

- Storage layer replaced: flash map, asserts, dump ranges, ROM-call and JEDEC pattern (`kb/identity.c`, `wear_leveling_rp2040_flash.c`), dedicated 4 KiB sector buffer, settings hold-off, new test harness and CI workflow.
- Commit safety: `PSM_WDSEL` set before arming, `LOAD = µs × 2`, `PAUSE_DBG` cleared; boot2 copy taken in step 0 and never refilled; commit only from housekeeping, refused with no erase if any DMA channel is busy; WS2812 left to finish; full 48-slot RAM vector table; image buffers zeroed before the trigger.
- Design gaps: `REBIND` op (D3); main's stack 3 KiB for Ed25519; slave rows cleared on pause entry and split watchdog re-pinged on resume (D15); P1 compares sizes and offsets.
- D12 reopened, then answered: refuse while `wear_leveling_write_failed()` is latched (qmk#12); no flush. Citations updated; UNVERIFIED markers resolved except AIRCR reset scope and peripheral state after a watchdog reset.

All blockers and major points from both reviews were checked against the source and accepted. Spot-checked: `client_wrapper.c` has 182 lines; `sval.c:990-1003` lets unwrapped packets fall through to VIA; `identity.c:37-38,41` hold the identity sectors and the `0x18` check; RPC buffers are 32 B (`transport.h:28,32`); `usb.service.ts:17` renews every 50 s; `kb/config.h:155` enables double-tap unconditionally; `keytest.c:76,104` calls `action_exec`.

| Point | Note |
|---|---|
| "Reserve IDs in M1 so M1 and M2 builds interoperate" | Accepted, with a caveat: a board running an M1 build cannot talk to a half on today's release. Superseded in revision 3: halves always run the same release (V), so no IDs are reserved ahead of time |
| "Program pages inside the RPC callback" | Accepted for M2, but the claim that the master sends nothing else during the callback rests on `transaction_rpc_exec` being sequential (`transactions.c:1035-1046`). Measured in M2b, not assumed |
| "PIO RX FIFO overflows during each program" | The RX FIFO is 8 deep (joined, `serial_vendor.c:386`) at 1 Mbaud (`kb/config.h:170`), about 80 µs of headroom. The real IRQ-off windows are short (`serial_vendor.c:188-192,219-231,274-285`). Mitigated anyway (R16) |
| "Don't call rgblight_disable (persists)" | Correct, but `rgblight_disable_noeeprom()` exists (`rgblight.h:326`), so it is listed as D20(c) |
| Sector buffer location | Rev 3 reused the store cache. That cache is gone in `7e0af09`; rev 4 uses a dedicated 4 KiB static buffer rather than QMK's `wear_leveling.cache`, so the updater does not depend on QMK internals |
| "Retry once" | The reviewers were right: NOR flash needs an erase before reprogramming. Step 5 now erases, programs and compares again |
| Wrong citations | Fixed in rev 2: client_wrapper `:157-176/:105/:19/:167`; `RP2040.mk:85-98` is the divider/int64 wrap, not memcpy; full mcuconf path. Rev 4 re-checked every citation against `7e0af09` and the checked-out submodules |
