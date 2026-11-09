// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the commit routine: update_commit.c's real step logic
// against the mock die of test_updater.c and the mock registers below
// (update_commit_host.h). Included by test_updater.c in the build with
// -DSVAL_TEST_REAL_COMMIT -DSVAL_UPDATE_TEST_HOOKS, after test_session.c, whose
// helpers drive the end-to-end cases.
//
// The commit never returns once it has started: a watchdog trigger, a test
// halt and a power cut each jump back here (commit_env / cut_env), and the
// die is then inspected as the boot ROM would find it. On every run:
//   (b) only [0, round_up(image_len, 64 KiB)) changed,
//   (c) nothing else did (the slot and settings included),
//   (d) page 0's boot2 CRC is invalid (the ROM goes to BOOTSEL), or the whole
//       new image is in place;
// and during staging (a) only the slot changed.

#include "update_commit.c"

// ---- mock registers and HAL ------------------------------------------------------------

uint32_t update_host_ram0[UPDATE_HOST_RAM0_WORDS];

enum { LAND_NONE = 0, LAND_RESET, LAND_HALT, LAND_CUT };

static jmp_buf commit_env;
static struct {
    uint32_t ctrl, load, scratch4, tick, wdsel, vtor, dma[DMA_CHANNELS];
} hw;
static const volatile uintptr_t *vtor_table;
static bool                      rom_bad, armed_ok, reset_ok, halted_fed, enabled_before_first_op;
static int                       resets, halts, faults, latches, feeds, reg_errors;
static uint8_t                   at_reset_buf_nonzero; // commit_sector_buf when the trigger was written

#define VTOR_TOKEN 0x20001000u // what the mock VTOR holds; the table pointer is kept beside it

static void hw_reset(void) {
    memset(&hw, 0, sizeof(hw));
    hw.ctrl    = 0x07000000u; // WATCHDOG_CTRL reset value: the pause bits set
    hw.tick    = WD_TICK_RUNNING | 12;
    vtor_table = NULL;
    rom_bad = armed_ok = reset_ok = halted_fed = enabled_before_first_op = false;
    resets = halts = faults = latches = feeds = reg_errors = 0;
}

uint32_t update_host_reg_rd(uint32_t addr) {
    if (addr == WD_CTRL) return hw.ctrl;
    if (addr == WD_LOAD) return hw.load;
    if (addr == WD_SCRATCH4) return hw.scratch4;
    if (addr == WD_TICK) return hw.tick;
    if (addr == PSM_WDSEL) return hw.wdsel;
    if (addr == PPB_VTOR) return hw.vtor;
    for (uint32_t ch = 0; ch < DMA_CHANNELS; ch++) {
        if (addr == DMA_CTRL_TRIG(ch)) return hw.dma[ch];
    }
    reg_errors++;
    return 0;
}

void update_host_reg_wr(uint32_t addr, uint32_t v) {
    if (addr == WD_LOAD) {
        hw.load = v;
        feeds++;
        wd_since_feed_us = 0;
    } else if (addr == WD_SCRATCH4) {
        hw.scratch4 = v;
    } else if (addr == PSM_WDSEL) {
        hw.wdsel = v;
    } else if (addr == PPB_VTOR) {
        hw.vtor = v;
    } else if (addr == WD_CTRL + REG_ALIAS_CLR) {
        hw.ctrl &= ~v;
    } else if (addr == WD_CTRL + REG_ALIAS_SET) {
        if (v & WD_CTRL_ENABLE) {
            // D9: PSM_WDSEL, scratch4, LOAD and the pause bits are all set up first.
            armed_ok                = hw.wdsel == PSM_WDSEL_ALL_BUT_OSC && hw.scratch4 == 0 && hw.load == WD_LOAD_VALUE && !(hw.ctrl & WD_CTRL_PAUSE) && irq_off;
            mock_wd_on              = true;
            enabled_before_first_op = flash_ops == 0 && noplog == 0;
        }
        hw.ctrl |= v;
        if (v & WD_CTRL_TRIGGER) {
            resets++;
            reset_ok             = hw.wdsel == PSM_WDSEL_ALL_BUT_OSC && hw.scratch4 == 0 && irq_off;
            at_reset_buf_nonzero = 0;
            for (uint32_t i = 0; i < SECTOR / 4; i++)
                at_reset_buf_nonzero |= commit_sector_buf[i] != 0;
            longjmp(commit_env, LAND_RESET);
        }
    } else {
        reg_errors++;
    }
}

uint32_t update_host_flash_rd32(uint32_t off) {
    if (!xip_on || connected) sequence_errors++; // no flash reads with XIP off
    if (off > UPDATE_HOST_FLASH_BYTES - 4 || off % 4) {
        sequence_errors++;
        return 0;
    }
    const uint8_t *p = update_host_flash + off;
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void update_host_primask(bool set) {
    irq_off = set;
}

uint32_t update_host_vtor_value(const volatile uintptr_t *table) {
    vtor_table = table;
    return VTOR_TOKEN;
}

bool update_host_rom_ptr_ok(void *p) {
    return p != NULL && !rom_bad;
}

void update_host_halt(bool fed) {
    halts++;
    halted_fed = fed;
    longjmp(commit_env, LAND_HALT);
}

void update_host_fault(void) {
    // A HardFault: the core fetches slot 3 of the table VTOR points at.
    faults++;
    if (hw.vtor != VTOR_TOKEN || vtor_table != commit_vectors) {
        reg_errors++;
        return;
    }
    ((void (*)(void))vtor_table[3])();
}

void update_host_spin(void) {
    fprintf(stderr, "commit spun after the watchdog trigger\n");
    abort();
}

// The LEDs: latched once, with interrupts still on; the commit's ROM ops
// begin after this (allow the firmware area from here on).
static uint32_t commit_allow_hi;
void            update_led_commit_latch(void) {
    latches++;
    if (irq_off) reg_errors++;
    allow_lo   = 0;
    allow_hi   = commit_allow_hi;
    multi_page = true;
}

// ---- set-up ---------------------------------------------------------------------------------

static uint8_t old_fw[SVAL_UPDATE_MAX_IMAGE];

static uint32_t round_block(uint32_t len) {
    uint32_t r = (len + 0xFFFFu) & ~0xFFFFu;
    return r > SVAL_UPDATE_MAX_IMAGE ? SVAL_UPDATE_MAX_IMAGE : r;
}

static const sval_update_manifest_t *manifest(void) {
    return (const sval_update_manifest_t *)blob;
}

// The new image differs from the running one even in boot2 (still valid), so
// a boot2 taken from the wrong place shows.
static void make_commit_image(uint32_t len) {
    make_update(len);
    img[200] ^= 0x5A;
    put32(img + 252, update_crc32_mpeg2(img, 252));
    sval_update_manifest_t *m = blob_manifest();
    crypto_sha512(m->sha512, img, len);
}

static void plant_ram0(void) {
    for (uint32_t i = 0; i < UPDATE_HOST_RAM0_WORDS; i++)
        update_host_ram0[i] = i * 2654435761u;
    for (uint32_t i = 5; i < UPDATE_HOST_RAM0_WORDS; i += 777)
        update_host_ram0[i] = 0xCAFEB0BAu;                      // stale double-tap magic
    update_host_ram0[0]                          = 0xCAFEB0BAu; // and at both ends
    update_host_ram0[UPDATE_HOST_RAM0_WORDS - 1] = 0xCAFEB0BAu;
}

static bool ram0_swept(void) {
    for (uint32_t i = 0; i < UPDATE_HOST_RAM0_WORDS; i++) {
        uint32_t want = (i >= 5 && (i - 5) % 777 == 0) || i == 0 || i == UPDATE_HOST_RAM0_WORDS - 1 ? 0 : i * 2654435761u;
        if (want == 0xCAFEB0BAu) want = 0;
        if (update_host_ram0[i] != want) return false;
    }
    return true;
}

// The board before a commit: the old firmware (valid boot2 = BOOT2_ROM) in
// the firmware area and the new image verified in the slot.
static void commit_setup(uint32_t len) {
    fresh(len);
    make_commit_image(len);
    for (uint32_t i = 0; i < SVAL_UPDATE_MAX_IMAGE; i++)
        old_fw[i] = (uint8_t)(rnd() | 0x01);
    memcpy(old_fw, BOOT2_ROM, 256);
    memcpy(update_host_flash, old_fw, sizeof(old_fw));
    memcpy(update_host_flash + SVAL_UPDATE_BASE, img, len);
    hw_reset();
    plant_ram0();
    commit_allow_hi = round_block(len);
    update_commit_test_halt(UPDATE_HALT_NONE, false);
    memset(commit_vectors, 0, sizeof(commit_vectors));
    snap();
}

static update_status_t run_st;
static uint32_t        run_crc_xor; // a COMMIT for some other image

static int commit_go(void) {
    int land = setjmp(commit_env);
    if (land == 0) {
        land = setjmp(cut_env);
        if (land == 0) {
            run_st = update_commit_run(manifest(), body_crc() ^ run_crc_xor);
            return LAND_NONE;
        }
        land = LAND_CUT;
    }
    // Whatever happened, the CPU is gone: put the mock CPU state back.
    irq_off    = false;
    xip_on     = true;
    connected  = false;
    mock_wd_on = false;
    return land;
}

static bool page0_valid(void) {
    return update_boot2_valid(update_host_flash);
}
static bool image_in_place(uint32_t len) {
    return memcmp(update_host_flash, img, len) == 0;
}
static bool all_is(uint32_t lo, uint32_t hi, uint8_t v) {
    for (uint32_t a = lo; a < hi; a++) {
        if (update_host_flash[a] != v) return false;
    }
    return true;
}

// (b), (c), (d), and every ROM op made in sequence (XIP off, interrupts off,
// legal arguments): an op out of sequence is refused by the mock die, so it
// must never pass unnoticed.
static void check_invariants(uint32_t len) {
    CHECK(unchanged_outside(0, round_block(len)));
    CHECK(!page0_valid() || image_in_place(len));
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
}

static bool nothing_started(void) {
    return erase_ops + program_ops == 0 && hw.vtor == 0 && hw.wdsel == 0 && !(hw.ctrl & WD_CTRL_ENABLE) && !irq_off && memcmp(before, update_host_flash, sizeof(before)) == 0;
}

static bool buf_zero(void) {
    for (uint32_t i = 0; i < SECTOR / 4; i++) {
        if (commit_sector_buf[i]) return false;
    }
    return true;
}

// ---- tests ------------------------------------------------------------------------------------

static void test_commit_happy(uint32_t len) {
    commit_setup(len);
    CHECK_EQ(commit_go(), LAND_RESET);
    // The new image, the rest of its last block erased, nothing else touched.
    CHECK(image_in_place(len));
    CHECK(page0_valid());
    CHECK(all_is(len, round_block(len), 0xFF));
    check_invariants(len);
    CHECK_EQ(sequence_errors, 0);
    CHECK_EQ(reg_errors, 0);
    // XIP came back through the running firmware's boot2, copied in step 0.
    CHECK(memcmp(commit_boot2, BOOT2_ROM, 256) == 0);
    // The RAM vector table: SP, then the RAM handler in all 47 other slots.
    CHECK_EQ(hw.vtor, VTOR_TOKEN);
    CHECK(vtor_table == commit_vectors);
    CHECK_EQ(commit_vectors[0], UPDATE_HOST_INITIAL_SP);
    for (int i = 1; i < VECTOR_SLOTS; i++)
        CHECK(commit_vectors[i] == (uintptr_t)commit_ram_reset);
    // The watchdog: armed in order before the first flash op, fed after every
    // ROM call, never more than its period of worst-case flash time unfed.
    CHECK(armed_ok);
    CHECK(enabled_before_first_op);
    CHECK(wd_gap_max_us <= UPDATE_COMMIT_WATCHDOG_US);
    CHECK(feeds >= noplog);
    // The reset: PSM_WDSEL set, scratch4 clear, PRIMASK set, buffers clean.
    CHECK_EQ(resets, 1);
    CHECK(reset_ok);
    CHECK_EQ(at_reset_buf_nonzero, 0);
    CHECK(ram0_swept());
    CHECK_EQ(latches, 1);

    // The ROM ops, in order: zero page 0; erase sector 0, the other 15 sectors
    // of block 0, the other blocks; program each sector (sector 0 from page
    // 1); page 0 last.
    uint32_t sectors = (len + SECTOR - 1) / SECTOR, blocks = round_block(len) / BLOCK;
    int      want = 1 + 16 + (int)(blocks - 1) + (int)sectors + 1;
    CHECK_EQ(noplog, want);
    if (noplog == want) {
        int k = 0;
        CHECK(oplog[k].op == 'P' && oplog[k].addr == 0 && oplog[k].count == 256 && oplog[k].zeros);
        k++;
        for (uint32_t s = 0; s < 16; s++, k++)
            CHECK(oplog[k].op == 'E' && oplog[k].addr == s * SECTOR && oplog[k].count == SECTOR);
        for (uint32_t b = 1; b < blocks; b++, k++)
            CHECK(oplog[k].op == 'E' && oplog[k].addr == b * BLOCK && oplog[k].count == BLOCK);
        for (uint32_t s = 0; s < sectors; s++, k++) {
            uint32_t from = s ? s * SECTOR : PAGE, to = (s + 1) * SECTOR < len ? (s + 1) * SECTOR : len;
            CHECK(oplog[k].op == 'P' && oplog[k].addr == from && oplog[k].count == to - from);
        }
        CHECK(oplog[k].op == 'P' && oplog[k].addr == 0 && oplog[k].count == 256 && !oplog[k].zeros);
    }
}

static void test_commit_refusals(void) {
    // Step 0 refuses with nothing touched: no ROM op, no register, IRQs on.
    struct {
        const char     *what;
        update_status_t want;
    } cases[] = {
        {"settings failing", UPDATE_UNAVAILABLE}, {"die not 16 MiB", UPDATE_UNAVAILABLE}, {"reserved byte", UPDATE_INVALID}, {"slot byte changed", UPDATE_BAD_HASH}, {"other CRC", UPDATE_BAD_HASH}, {"bad SP, hashed", UPDATE_BAD_IMAGE}, {"running boot2 bad", UPDATE_FLASH_ERR}, {"ROM pointer bad", UPDATE_UNAVAILABLE}, {"no watchdog tick", UPDATE_UNAVAILABLE}, {"epoch floor", UPDATE_EPOCH}, {"manifest hash differs", UPDATE_BAD_HASH},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        commit_setup(0x3000);
        switch (i) {
            case 0:
                port_failing = true;
                break;
            case 1:
                capacity = 0x15;
                probed   = false;
                break;
            case 2:
                blob_manifest()->reserved0 = 1;
                break; // the manifest check runs again
            case 3:
                update_host_flash[SVAL_UPDATE_BASE + 0x2345] ^= 0x04;
                break;
            case 4:
                run_crc_xor = 0x10000;
                break;
            case 5:
                put32(img + 0x100, 0x1FFFFFF0); // hashed as it is, but a bad SP
                memcpy(update_host_flash + SVAL_UPDATE_BASE, img, img_len);
                crypto_sha512(blob_manifest()->sha512, img, img_len);
                break;
            case 6:
                update_host_flash[100] ^= 0x01;
                break;
            case 7:
                rom_bad = true;
                break;
            case 8:
                hw.tick = 0;
                break;
            case 9:
                self_dev.security_epoch = 1;
                break;
            default:
                blob_manifest()->sha512[33] ^= 0x01;
                break; // the slot and its CRC as verified
        }
        snap();
        CHECK_EQ(commit_go(), LAND_NONE);
        CHECK_EQ(run_st, cases[i].want);
        CHECK(nothing_started());
        CHECK_EQ(latches, 0);
        CHECK(buf_zero());
        if (failures) fprintf(stderr, "  (refusal: %s)\n", cases[i].what);
        port_failing = false;
        run_crc_xor  = 0;
        capacity     = 0x18;
        probed       = false;
    }
    // Step 1: any DMA channel busy refuses with BUSY, after PRIMASK and before
    // anything else; PRIMASK is cleared again.
    for (uint32_t ch = 0; ch < DMA_CHANNELS; ch++) {
        commit_setup(0x3000);
        hw.dma[ch] = DMA_CTRL_BUSY | 0x21;
        CHECK_EQ(commit_go(), LAND_NONE);
        CHECK_EQ(run_st, UPDATE_BUSY);
        CHECK(nothing_started());
        CHECK_EQ(latches, 1);
        CHECK(buf_zero());
    }
    // Gentle (D33): an image for the other hand and another pointing device is
    // committed; step 0 checks only integrity and structure.
    commit_setup(0x3000);
    blob_manifest()->hand        = UPDATE_HAND_LEFT;
    blob_manifest()->pointing_id = UPDATE_POINTING_AZOTEQ;
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(image_in_place(0x3000));
    // DMA channels configured but idle are fine.
    commit_setup(0x3000);
    for (uint32_t ch = 0; ch < DMA_CHANNELS; ch++)
        hw.dma[ch] = 0x00F00021u;
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(image_in_place(0x3000));
    CHECK_EQ(update_commit_available(), true);
    capacity = 0x15;
    probed   = false;
    CHECK_EQ(update_commit_available(), false);
    capacity = 0x18;
    probed   = false;
}

// A cut after every ROM op of the commit, clean and torn (random bytes in the
// sector, block or pages the op touched).
static void test_commit_cuts(uint32_t len) {
    commit_setup(len);
    CHECK_EQ(commit_go(), LAND_RESET);
    int ops = flash_ops, bootsel = 0, booted = 0;
    for (int tear = 0; tear < 2; tear++) {
        for (int k = 1; k <= ops; k++) {
            commit_setup(len);
            cut_at   = k;
            cut_tear = tear;
            CHECK_EQ(commit_go(), LAND_CUT);
            check_invariants(len);
            if (page0_valid()) {
                booted++;
                CHECK(k == ops && !tear); // only once page 0 is fully written
            } else {
                bootsel++;
            }
            if (failures) {
                fprintf(stderr, "  (cut after op %d of %d, %s)\n", k, ops, tear ? "torn" : "clean");
                return;
            }
        }
    }
    CHECK_EQ(booted, 1);
    CHECK_EQ(bootsel, 2 * ops - 1);
    printf("commit 0x%x: cut after each of %d ROM ops, clean and torn: %d BOOTSEL, %d new image\n", (unsigned)len, ops, bootsel, booted);
}

// Step 5's one retry, and what happens when it is not enough; steps 2 and 6
// failing their read-back.
static void test_commit_retry(void) {
    const uint32_t len = 0x13000;
    // One byte of sector 3 does not take: that sector alone is erased and
    // programmed again, and the image boots.
    commit_setup(len);
    bad_at    = 3 * SECTOR + 0x123;
    bad_nth   = 1;
    bad_times = 1;
    CHECK(img[bad_at] != 0xFF);
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(image_in_place(len) && page0_valid());
    check_invariants(len);
    int retries = 0;
    for (int k = 0; k < noplog; k++)
        retries += oplog[k].op == 'E' && oplog[k].addr == 3 * SECTOR && oplog[k].count == SECTOR;
    CHECK_EQ(retries, 2); // step 3, then the retry
    // ... in sector 0 (pages 1-15): the same, and page 0 still goes last
    commit_setup(len);
    bad_at    = 0x480;
    bad_nth   = 1;
    bad_times = 1;
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(image_in_place(len) && page0_valid());
    check_invariants(len);
    CHECK(oplog[noplog - 1].op == 'P' && oplog[noplog - 1].addr == 0 && oplog[noplog - 1].count == 256);
    // Twice in a row: the retry fails too, page 0 stays erased, BOOTSEL.
    commit_setup(len);
    bad_at    = 9 * SECTOR + 0x40;
    bad_nth   = 1;
    bad_times = 2;
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(!page0_valid());
    CHECK(all_is(0, PAGE, 0xFF));
    CHECK(reset_ok);
    check_invariants(len);

    // Step 2: page 0 does not read back zero. Reset at once: one ROM op, the
    // rest of the old image untouched, and page 0 no longer valid.
    commit_setup(len);
    bad_at    = 16;
    bad_nth   = 1;
    bad_times = 1;
    CHECK(BOOT2_ROM[16] != 0);
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK_EQ(noplog, 1);
    CHECK(!page0_valid());
    CHECK(memcmp(update_host_flash + PAGE, old_fw + PAGE, SVAL_UPDATE_MAX_IMAGE - PAGE) == 0);
    check_invariants(len);
    // Step 6: page 0 does not read back as written: zeroed, then BOOTSEL.
    commit_setup(len);
    bad_at    = 40;
    bad_nth   = 2; // step 2's zero page is the first program covering it
    bad_times = 1;
    CHECK(img[40] != 0xFF);
    CHECK_EQ(commit_go(), LAND_RESET);
    CHECK(!page0_valid());
    CHECK(all_is(0, PAGE, 0x00));
    CHECK(memcmp(update_host_flash + PAGE, img + PAGE, len - PAGE) == 0);
    check_invariants(len);
}

// Step 6 takes page 0 from the slot again; if those bytes are no longer the
// ones step 0 checked, page 0 stays erased (BOOTSEL).
static void slot_page0_rots(int op) {
    if (op == 3) update_host_flash[SVAL_UPDATE_BASE + 50] ^= 0x08;
}

static void test_commit_page0_recheck(void) {
    commit_setup(0x13000);
    after_op = slot_page0_rots;
    CHECK_EQ(commit_go(), LAND_RESET);
    after_op = NULL;
    CHECK(!page0_valid());
    CHECK(all_is(0, PAGE, 0xFF));
    CHECK(memcmp(update_host_flash + PAGE, img + PAGE, 0x13000 - PAGE) == 0);
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
}

// Test hooks (M1 #8, #9, #10): where each halt leaves the die.
static void test_commit_halts(void) {
    const uint32_t len = 0x13000, sectors = (len + SECTOR - 1) / SECTOR;
    CHECK(!update_commit_test_halt(UPDATE_HALT_COUNT, true));
    CHECK(!update_commit_test_halt(0xFF, false));
    for (uint8_t point = UPDATE_HALT_INVALIDATED; point < UPDATE_HALT_COUNT; point++) {
        for (int fed = 0; fed < 2; fed++) {
            commit_setup(len);
            CHECK(update_commit_test_halt(point, fed));
            int land = commit_go();
            check_invariants(len);
            CHECK(armed_ok);
            if (point == UPDATE_HALT_FAULT || point == UPDATE_HALT_FAULT_ERASED) {
                // The HardFault goes through the RAM table to the reset (step 7).
                CHECK_EQ(land, LAND_RESET);
                CHECK_EQ(faults, 1);
                CHECK_EQ(halts, 0);
                CHECK(reset_ok);
                CHECK_EQ(at_reset_buf_nonzero, 0);
                CHECK(ram0_swept());
                if (point == UPDATE_HALT_FAULT) {
                    CHECK(all_is(0, PAGE, 0x00));
                    CHECK_EQ(noplog, 1);
                } else {
                    // sector 0, with the flash vector table at 0x100, is erased
                    CHECK(all_is(0, SECTOR, 0xFF));
                    CHECK_EQ(noplog, 2);
                    CHECK(memcmp(update_host_flash + SECTOR, old_fw + SECTOR, SVAL_UPDATE_MAX_IMAGE - SECTOR) == 0);
                }
                continue;
            }
            CHECK_EQ(land, LAND_HALT);
            CHECK_EQ(halted_fed, fed);
            // A halt leaves RAM as step 7 would: an unfed halt's watchdog reset
            // starts the next image from the same state as a real commit's.
            CHECK(buf_zero());
            CHECK(ram0_swept());
            CHECK_EQ(resets, 0);
            switch (point) {
                case UPDATE_HALT_INVALIDATED:
                    CHECK_EQ(noplog, 1);
                    CHECK(all_is(0, PAGE, 0x00));
                    CHECK(memcmp(update_host_flash + PAGE, old_fw + PAGE, SVAL_UPDATE_MAX_IMAGE - PAGE) == 0);
                    break;
                case UPDATE_HALT_FIRST_ERASE:
                    CHECK_EQ(noplog, 2);
                    CHECK(all_is(0, SECTOR, 0xFF));
                    CHECK(memcmp(update_host_flash + SECTOR, old_fw + SECTOR, SVAL_UPDATE_MAX_IMAGE - SECTOR) == 0);
                    break;
                case UPDATE_HALT_MID_PROGRAM:
                    CHECK(all_is(0, PAGE, 0xFF));
                    CHECK(memcmp(update_host_flash + PAGE, img + PAGE, (sectors / 2 + 1) * SECTOR - PAGE) == 0);
                    CHECK(all_is((sectors / 2 + 1) * SECTOR, round_block(len), 0xFF));
                    break;
                case UPDATE_HALT_LAST_SECTOR:
                    CHECK(all_is(0, PAGE, 0xFF));
                    CHECK(memcmp(update_host_flash + PAGE, img + PAGE, len - PAGE) == 0);
                    break;
                default: // UPDATE_HALT_PAGE0
                    CHECK(image_in_place(len) && page0_valid());
                    break;
            }
            CHECK(point == UPDATE_HALT_PAGE0 || !page0_valid());
        }
    }
    update_commit_test_halt(UPDATE_HALT_NONE, false);
}

// The RAM routines' own guards, called directly: commit_ram_flash makes no
// ROM call for anything outside [0, MAX_IMAGE) or not one sector / block
// erase or a page-aligned program of at most a sector; commit_ram_reset sets
// PSM_WDSEL and clears scratch4 itself (it is also the fault handler).
static void test_commit_guards(void) {
    commit_setup(0x3000);
    static uint8_t page[SECTOR + PAGE];
    struct {
        uint32_t off;
        bool     program;
        uint32_t len;
    } const bad[] = {
        {SVAL_UPDATE_MAX_IMAGE, false, SECTOR}, {SVAL_UPDATE_MAX_IMAGE - SECTOR, false, BLOCK}, {SVAL_UPDATE_MAX_IMAGE, true, PAGE}, {SVAL_UPDATE_MAX_IMAGE - PAGE, true, 2 * PAGE}, {0xFFFFF000u, false, SECTOR}, {0xFFFFFF00u, true, PAGE}, {SVAL_UPDATE_BASE, false, SECTOR}, {SVAL_UPDATE_BASE, true, PAGE}, {IDENTITY_SECTOR_A, false, SECTOR}, {WEAR_LEVELING_RP2040_FLASH_BASE, false, BLOCK}, {0, false, 0}, {0, true, 0}, {0, false, 0x2000}, {0, false, PAGE}, {0x800, false, SECTOR}, {SECTOR, false, BLOCK}, {0x80, true, PAGE}, {0, true, 0x180}, {0, true, SECTOR + PAGE},
    };
    irq_off = true;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK(!commit_ram_flash(bad[i].off, bad[i].program ? page : NULL, bad[i].len));
    irq_off = false;
    CHECK_EQ(noplog, 0);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
    // ... and the largest legal ones go through.
    allow_lo   = 0;
    allow_hi   = SVAL_UPDATE_MAX_IMAGE;
    multi_page = true;
    irq_off    = true;
    CHECK(commit_ram_flash(SVAL_UPDATE_MAX_IMAGE - BLOCK, NULL, BLOCK));
    CHECK(commit_ram_flash(SVAL_UPDATE_MAX_IMAGE - SECTOR, page, SECTOR));
    irq_off = false;
    CHECK_EQ(noplog, 2);
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);

    // The reset, from any state: WDSEL set again, scratch4 cleared.
    commit_setup(0x3000);
    hw.scratch4 = 0xB007C0D3u; // the boot ROM's 'boot to an address' magic
    hw.wdsel    = 0;
    for (uint32_t i = 0; i < SECTOR / 4; i++)
        commit_sector_buf[i] = 0xCAFEB0BAu;
    int land = setjmp(commit_env);
    if (land == 0) commit_ram_reset();
    CHECK_EQ(land, LAND_RESET);
    CHECK(reset_ok);
    CHECK_EQ(at_reset_buf_nonzero, 0);
    CHECK(ram0_swept());
    irq_off = false;
}

// ---- end to end: the session, COMMIT, the commit ---------------------------------------------

static void commit_c6(uint8_t c[6]) {
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
}

// One whole update through updater.c, cut after ROM op cut_k (0: no cut).
// Returns where it landed.
static int full_update(uint32_t len, int cut_k, bool tear) {
    commit_setup(len);
    commit_allow_hi = round_block(len);
    cut_at          = cut_k;
    cut_tear        = tear;
    int land        = setjmp(commit_env);
    if (land == 0) {
        land = setjmp(cut_env);
        if (land == 0) {
            if (!to_verified(CLIENT_A)) return LAND_NONE;
            uint8_t c[6];
            commit_c6(c);
            if (send(CLIENT_A, UPDATE_OP_COMMIT, c, 6) != UPDATE_ACCEPTED) return LAND_NONE;
            for (int i = 0; i < 1000; i++)
                pass();
            return LAND_NONE;
        }
        land = LAND_CUT;
    }
    irq_off    = false;
    xip_on     = true;
    connected  = false;
    mock_wd_on = false;
    return land;
}

// The board rebooted into the old image after a cut during staging: a new
// session from the slot as the cut left it.
static int update_after_reboot(void) {
    updater_host_reset();
    probed   = false;
    cut_at   = 0;
    int land = setjmp(commit_env);
    if (land == 0) {
        if (to_verified(CLIENT_B)) {
            uint8_t c[6];
            commit_c6(c);
            if (send(CLIENT_B, UPDATE_OP_COMMIT, c, 6) == UPDATE_ACCEPTED) {
                for (int i = 0; i < 1000; i++)
                    pass();
            }
        }
        return LAND_NONE;
    }
    irq_off    = false;
    xip_on     = true;
    connected  = false;
    mock_wd_on = false;
    return land;
}

static void test_commit_end_to_end(void) {
    const uint32_t len = 0x3000;
    // TEST_HALT: the session's, in VERIFIED, a known point.
    commit_setup(len);
    CHECK(to_receiving(CLIENT_A));
    uint8_t h[6];
    put32(h, session_nonce);
    h[4] = UPDATE_HALT_PAGE0;
    h[5] = 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_TEST_HALT, h, 6), UPDATE_INVALID); // not verified yet
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_TEST_HALT, h, 6), UPDATE_OTHER_CLIENT);
    h[4] = UPDATE_HALT_COUNT;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_TEST_HALT, h, 6), UPDATE_INVALID);
    h[4] = UPDATE_HALT_LAST_SECTOR;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_TEST_HALT, h, 6), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_VERIFIED);
    CHECK_EQ(halt_point, UPDATE_HALT_LAST_SECTOR);
    CHECK(halt_fed);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x08, 0x08); // INFO reports the hooks
    // ABORT clears it
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    CHECK_EQ(halt_point, UPDATE_HALT_NONE);

    // A whole update: staged, verified, committed 100 ms after the ack, reset.
    CHECK_EQ(full_update(len, 0, false), LAND_RESET);
    CHECK(image_in_place(len) && page0_valid());
    check_invariants(len);
    CHECK_EQ(latches, 1);
    CHECK(reset_ok);
    int total = flash_ops;

    // A cut after every ROM op of staging and of the commit.
    int staging_cuts = 0, commit_cuts = 0;
    for (int tear = 0; tear < 2; tear++) {
        for (int k = 1; k <= total; k++) {
            CHECK_EQ(full_update(len, k, tear), LAND_CUT);
            if (latches == 0) {
                // (a): staging changed only the slot; the old firmware boots.
                staging_cuts++;
                CHECK(unchanged_outside(SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE));
                CHECK(memcmp(update_host_flash, old_fw, SVAL_UPDATE_MAX_IMAGE) == 0 && page0_valid());
                CHECK_EQ(outside_ops, 0);
                CHECK_EQ(sequence_errors, 0);
                // After the reboot a new session, from the dirty slot, succeeds.
                CHECK_EQ(update_after_reboot(), LAND_RESET);
                CHECK(image_in_place(len) && page0_valid());
                check_invariants(len);
            } else {
                commit_cuts++;
                check_invariants(len);
                CHECK(!page0_valid() || (k == total && !tear));
            }
            if (failures) {
                fprintf(stderr, "  (end to end, cut after op %d of %d, %s)\n", k, total, tear ? "torn" : "clean");
                return;
            }
        }
    }
    printf("end to end 0x%x: %d ROM ops; cuts: %d in staging (old image boots, retry succeeds), %d in the commit\n", (unsigned)len, total, staging_cuts, commit_cuts);
}

static void test_commit(void) {
    int            before_failures = failures;
    const uint32_t lens[]          = {0x200, 0x3000, 0xFF00, 0x10000, 0x10100, 0x13000, 0x30000, SVAL_UPDATE_MAX_IMAGE};
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        test_commit_happy(lens[i]);
        if (failures != before_failures) fprintf(stderr, "  (happy path 0x%x)\n", (unsigned)lens[i]);
    }
    test_commit_refusals();
    test_commit_guards();
    test_commit_cuts(0x13000);
    test_commit_cuts(0x200);
    test_commit_retry();
    test_commit_page0_recheck();
    test_commit_halts();
    test_commit_end_to_end();
    printf("commit tests: %s\n", failures == before_failures ? "pass" : "FAIL");
}
