// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the fixes from the M2a stage 2 review (update_split.c):
// the slave's timeout race, the RPC lengths from the wire, a busy DMA channel
// at a page program, the other half with its interrupts off while it erases
// and commits (RPCs fail then), ABORT while it erases, a probe that finds it
// still COMMITTING, a lost COMMIT answer, and INFO flags bit5 after a relay until
// presence matches. Included by test_relay.c, before test_relay().

// The SlaveThread preempts housekeeping between its clock read and its lock:
// a request lands 1 ms later and sets rx_ms. The session must not be dropped
// as a 5 s timeout (now - rx_ms wrapping).
static int  race_rpcs;
static void rpc_before_lock(void) {
    if (race_rpcs++) return;
    sim_us += 1000;
    lock_depth++; // the callback runs with the lock (the SlaveThread holds it)
    CHECK_EQ(m_op(USPLIT_OP_STATUS), UPDATE_OK);
    lock_depth--;
}

static void test_relay_timeout_race(void) {
    relay_fresh(0x800);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    slave_ticks(10);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    race_rpcs = 0;
    lock_hook = rpc_before_lock;
    slave_hk();
    lock_hook = NULL;
    CHECK_EQ(race_rpcs, 1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    CHECK_EQ(lock_errors, 0);
}

// out_len and in_len come from the wire (rpc_info, CRC-8 only): never past
// the 32-byte RPC buffers.
static void test_relay_rpc_lengths(void) {
    relay_fresh(0x800);
    struct {
        uint8_t buf[USPLIT_MSG_MAX];
        uint8_t guard[255 - USPLIT_MSG_MAX];
    } o;
    uint8_t in[255];
    memset(in, 0, sizeof(in));
    uint8_t len = usplit_req_build(in, USPLIT_OP_STATUS, 0x21, NULL, 0);
    for (int k = 0; k < 2; k++) {
        memset(&o, 0xA5, sizeof(o));
        sim_slave = true;
        update_split_slave_rpc(in, k == 0 ? 255 : len, (uint8_t *)&o, 255);
        sim_slave     = false;
        bool guard_ok = true;
        for (size_t i = 0; i < sizeof(o.guard); i++)
            guard_ok &= o.guard[i] == 0xA5;
        CHECK(guard_ok);
    }
}

// A DMA channel busy when a fragment completes a page (the PMW SPI's TX
// source is in XIP, R6): BUSY, the fragment given back, nothing written; the
// relay sends it again and every page is programmed once.
static void test_relay_dma_busy(void) {
    relay_fresh(0x400);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    slave_ticks(10);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    int programs = program_ops;
    for (uint32_t off = 0; off < 216; off += 24)
        CHECK_EQ(m_page(off, 24), UPDATE_OK);
    CHECK_EQ(m_page(216, 24), UPDATE_OK);
    CHECK_EQ(get24le(mrs.payload), 240);
    dma_busy_pct = 100;
    CHECK_EQ(m_page(240, 16), UPDATE_BUSY); // completes the page
    CHECK_EQ(get24le(mrs.payload), 240);    // given back
    CHECK_EQ(program_ops, programs);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    dma_busy_pct = 0;
    CHECK_EQ(m_page(240, 16), UPDATE_OK);
    CHECK_EQ(get24le(mrs.payload), 256);
    CHECK_EQ(program_ops, programs + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);

    // A whole relay with the DMA often busy.
    relay_fresh(0x6000);
    dma_busy_pct = 40;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 120000), RELAY_VERIFIED);
    CHECK(dma_refusals > 0);
    CHECK(slot_is_img(img_len));
    CHECK_EQ(program_ops, (int)(img_len / 256));
    CHECK_EQ(sequence_errors, 0);
    dma_busy_pct = 0;
    update_relay_abort();
    for (int i = 0; i < 10; i++)
        pass();
    CHECK(!update_relay_busy());
}

// The other half with its interrupts off while it erases (about 50 ms a
// sector, longer than the 20 ms serial timeout) and while it commits: every
// RPC then fails. The erase waits for a request between sectors, so the relay
// never hits its link timeout, however large the image; an ABORT while it
// erases keeps the link paused until the ABORT has landed, and no sector is
// erased with the link running.
static void test_relay_busy_slave(void) {
    const int             f0     = failures;
    static const uint32_t lens[] = {0x1E900, 0x40000}; // 125,184 B (the M2b images) and 64 sectors
    for (size_t k = 0; k < 2; k++) {
        relay_fresh(lens[k]);
        slave_irq_sim    = true;
        erase_unpaused   = 0;
        pause_violations = 0;
        after_op         = erase_watch;
        CHECK(update_relay_start(blob, img_len, body_crc()));
        uint64_t t0 = sim_us;
        CHECK_EQ(relay_until(RELAY_VERIFIED, 600000), RELAY_VERIFIED);
        CHECK_EQ(ri.error, UPDATE_OK);
        CHECK_EQ(ri.slave_to_erase, (img_len + 4095) / 4096);
        CHECK(slave_busy_rpcs > 0); // the erases did block the link
        CHECK_EQ(erase_unpaused, 0);
        CHECK_EQ(pause_violations, 0);
        CHECK(slot_is_img(img_len));
        printf("busy other half, 0x%x: %u sectors erased, %u RPCs failed while it was busy, %llu ms to verified\n", (unsigned)img_len, (unsigned)ri.slave_to_erase, (unsigned)slave_busy_rpcs, (unsigned long long)((sim_us - t0) / 1000));
        // and its commit, interrupts off for 1.5 s: the hold covers it
        slave_commit_resets = true;
        CHECK(update_relay_commit());
        uint64_t unpaused_busy = 0;
        for (int i = 0; i < 20000; i++) {
            pass();
            if (!link_paused && sim_us < slave_busy_until_us) unpaused_busy++;
            relay_get();
            if (ri.phase == RELAY_DONE || ri.phase == RELAY_FAILED) break;
        }
        CHECK_EQ(ri.phase, RELAY_DONE);
        CHECK(!ri.unconfirmed);
        CHECK_EQ(unpaused_busy, 0);
        CHECK_EQ(slave_commit_runs, 1);
        after_op = NULL;
    }

    // ABORT from the host (or the session timeout, the same call) while it erases.
    relay_fresh(0x40000);
    slave_irq_sim  = true;
    erase_unpaused = 0;
    after_op       = erase_watch;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    for (int i = 0; i < 600000; i++) {
        pass();
        relay_get();
        if (ri.slave_erased >= 10) break;
    }
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERASING);
    int erases = erase_ops;
    update_relay_abort();
    relay_get();
    CHECK_EQ(ri.phase, RELAY_ABORTING);
    CHECK(link_paused);
    CHECK(update_relay_busy());
    CHECK(!update_relay_start(blob, img_len, body_crc())); // not while it stops
    uint64_t a0 = sim_us;
    for (int i = 0; i < 5000 && update_relay_busy(); i++) {
        pass();
        if (!link_paused) CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    }
    relay_get();
    CHECK_EQ(ri.phase, RELAY_IDLE);
    CHECK(!link_paused);
    CHECK(sim_us - a0 < 300000); // the ABORT lands between two sectors
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    CHECK(erase_ops <= erases + 1); // at most the sector under way
    for (int i = 0; i < 3000; i++)
        pass();
    CHECK(erase_ops <= erases + 1);
    CHECK_EQ(erase_unpaused, 0);
    CHECK(only_slot_changed());
    after_op = NULL;
    if (failures != f0) fprintf(stderr, "  (busy other half)\n");
}

// After the hold the probe finds the other half still COMMITTING (it has not
// begun its copy): the link stays paused and it probes again; once it falls
// silent (its copy) the whole hold again; DONE only after it answers IDLE
// from its new image. Stuck in COMMITTING: a bounded number of probes, then
// TIMEOUT.
static void test_relay_probe_committing(void) {
    for (int stuck = 0; stuck < 2; stuck++) {
        relay_fresh(0x2000);
        slave_irq_sim = true;
        CHECK(update_relay_start(blob, img_len, body_crc()));
        CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
        slave_commit_resets = true;
        CHECK(update_relay_commit());
        relay_get();
        for (int i = 0; i < 20 && ri.phase == RELAY_COMMIT; i++) {
            update_relay_task(); // this half alone: the other half's housekeeping has not run yet
            sim_us += 1000;
            relay_get();
        }
        CHECK_EQ(ri.phase, RELAY_HOLD);
        CHECK_EQ(update_split_slave_state(), UPDATE_STATE_COMMITTING);
        slave_hk_frozen      = true; // its housekeeping is late
        uint64_t c0          = sim_us;
        uint64_t unfreeze_at = c0 + (uint64_t)(SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS + 1200) * 1000;
        uint64_t resumed_at = 0, unpaused_busy = 0;
        for (int i = 0; i < 40000; i++) {
            if (!stuck && sim_us >= unfreeze_at) slave_hk_frozen = false;
            pass();
            if (!link_paused && !resumed_at) resumed_at = sim_us;
            if (!link_paused && (sim_us < slave_busy_until_us || update_split_slave_state() == UPDATE_STATE_COMMITTING)) unpaused_busy++;
            relay_get();
            if (ri.phase == RELAY_DONE || ri.phase == RELAY_FAILED) break;
        }
        if (!stuck) {
            CHECK_EQ(ri.phase, RELAY_DONE);
            CHECK(!ri.unconfirmed);
            CHECK_EQ(slave_commit_runs, 1);
            CHECK_EQ(slave_reboots, 1);
            CHECK_EQ(unpaused_busy, 0);
            // paused past the late commit and a whole hold after it fell silent
            CHECK(resumed_at - unfreeze_at >= (uint64_t)(SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS) * 1000);
        } else {
            CHECK_EQ(ri.phase, RELAY_FAILED);
            CHECK_EQ(ri.error, UPDATE_TIMEOUT);
            CHECK_EQ(slave_commit_runs, 0);
            CHECK(resumed_at - c0 >= (uint64_t)(SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS) * 1000);
        }
        CHECK(!link_paused);
        slave_hk_frozen = false;
    }
}

// Every answer to COMMIT is lost, the other half takes it and commits with
// its interrupts off: no failure, no resume in the middle of its commit; the
// link timeout leads to the hold, and the probe finds it rebooted (DONE).
// Then INFO flags bit5 until presence shows MATCH, even through NONE (V).
static void test_relay_commit_answer_lost(void) {
    relay_fresh(0x2000);
    slave_irq_sim = true;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
    for (int i = 0; i < 900; i++)
        pass(); // most of a keepalive interval: COMMIT still gets the whole link timeout
    slave_commit_resets = true;
    drop_rsp_op         = USPLIT_OP_COMMIT;
    CHECK(update_relay_commit());
    uint64_t c0 = sim_us, unpaused_busy = 0, resumed_at = 0;
    for (int i = 0; i < 20000; i++) {
        pass();
        if (!link_paused && !resumed_at) resumed_at = sim_us;
        if (!link_paused && sim_us < slave_busy_until_us) unpaused_busy++;
        relay_get();
        if (ri.phase == RELAY_DONE || ri.phase == RELAY_FAILED) break;
    }
    CHECK_EQ(ri.phase, RELAY_DONE);
    CHECK(!ri.unconfirmed); // the probe found it IDLE from its new image
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_reboots, 1);
    CHECK_EQ(unpaused_busy, 0);
    CHECK(resumed_at - c0 >= (uint64_t)(SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS) * 1000);
    drop_rsp_op = -1;
    // INFO flags bit5 until presence matches (V), even with presence NONE
    CHECK(update_split_awaiting_match());
    CHECK(update_split_mismatch());
    presence_ping(); // the other half is on fw 2002, this one on 2001
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_VERSION);
    CHECK(update_split_awaiting_match());
    link_up = false;
    presence_ping();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_NONE);
    CHECK(update_split_mismatch()); // NONE, but still red
    link_up              = true;
    master_id.fw_version = 2002;
    presence_ping();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    CHECK(!update_split_awaiting_match());
    CHECK(!update_split_mismatch());
}

static void test_relay_review(void) {
    test_relay_timeout_race();
    test_relay_rpc_lengths();
    test_relay_dma_busy();
    test_relay_busy_slave();
    test_relay_probe_committing();
    test_relay_commit_answer_lost();
}
