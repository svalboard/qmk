// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the relay's speed work (docs/updater.md, "The relay"):
// dropouts in the page phase that the relay waits out and resumes from, one
// that outlasts it, the erase polled about once per sector, and the main
// loop's longest pass while the pages stream. Included by test_relay.c.

// Passes until ms of simulated time have gone by.
static void pass_for_ms(uint32_t ms) {
    uint64_t end = sim_us + (uint64_t)ms * 1000;
    while (sim_us < end) {
        pass();
        watch();
    }
}

// The link drops for ms in the middle of the page phase, then comes back.
static void test_relay_dropout(uint32_t ms) {
    const int f0 = failures;
    relay_fresh(0x13000);
    pause_violations = stream_unpaused = 0;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_PAGES, 60000), RELAY_PAGES);
    while (ri.phase == RELAY_PAGES && ri.acked < img_len / 2) {
        pass();
        relay_get();
    }
    uint32_t acked = ri.acked;
    link_up        = false;
    pass_for_ms(ms);
    link_up = true;
    relay_get();
    if (ms < SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS) {
        // Waited out: the relay carries on from where the other half stopped.
        CHECK_EQ(ri.phase, RELAY_PAGES);
        CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
        CHECK_EQ(ri.error, UPDATE_OK);
        CHECK(ri.resumes >= 1);
        CHECK(slot_is_img(img_len));
        CHECK_EQ(program_ops, (int)(img_len / 256)); // nothing written twice
        CHECK_EQ(stream_unpaused, 0);
        CHECK_EQ(pause_violations, 0);
        CHECK(update_relay_commit());
        slave_commit_resets = true;
        CHECK_EQ(relay_until(RELAY_DONE, 20000), RELAY_DONE);
        CHECK_EQ(slave_commit_runs, 1);
    } else {
        // Longer than the relay waits: it failed cleanly, with the record.
        CHECK_EQ(ri.phase, RELAY_FAILED);
        CHECK_EQ(ri.error, UPDATE_UNAVAILABLE);
        CHECK(!link_paused);
        CHECK_EQ(slave_commit_runs, 0);
        update_relay_fail_t f;
        update_relay_last_failure(&f);
        CHECK_EQ(f.phase, RELAY_PAGES);
        CHECK(f.off >= acked);
        CHECK(f.since_ok_ms >= SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS);
        // The other half drops its session by its own timeout; then a new
        // relay from the start works.
        pass_for_ms(SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS);
        CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
        CHECK(update_relay_start(blob, img_len, body_crc()));
        CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
        CHECK(slot_is_img(img_len));
        update_relay_last_failure(&f);
        CHECK(f.after_read);
        CHECK_EQ(f.after_error, UPDATE_TIMEOUT); // it stayed up and heard nothing
        update_relay_abort();
        relay_until(RELAY_IDLE, 5000);
    }
    CHECK(only_slot_changed());
    if (failures != f0) fprintf(stderr, "  (dropout of %u ms)\n", (unsigned)ms);
}

// The other half with its interrupts off while it erases: the relay polls
// about once per sector (a poll after SVAL_UPDATE_RELAY_ERASE_POLL_MS, and
// the polls that still land in an erase fail at the short deadline).
static void test_relay_erase_polls(void) {
    relay_fresh(0x1E900);
    slave_irq_sim = true;
    quick_rpcs    = 0;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    uint32_t n0 = lk.n;
    CHECK_EQ(relay_until(RELAY_PAGES, 60000), RELAY_PAGES);
    uint32_t polls = lk.n - n0, sectors = ri.slave_to_erase;
    CHECK(quick_rpcs > 0);
    // A sector here takes the mock die's erase plus the simulated time with
    // interrupts off; the relay adds at most a short poll and a pass to it.
    const uint32_t sector_ms = (SIM_SECTOR_ERASE_US + SIM_ERASE_US) / 1000;
    CHECK(ri.erase_ms <= sectors * (sector_ms + 10));
    CHECK(polls <= sectors * (2 + SIM_ERASE_US / 4000) + 10);
    CHECK_EQ(relay_until(RELAY_VERIFIED, 600000), RELAY_VERIFIED);
    CHECK(slot_is_img(img_len));
    printf("erase polls, 0x%x: %u sectors, %u RPCs, %u ms erasing\n", (unsigned)img_len, (unsigned)sectors, (unsigned)polls, (unsigned)ri.erase_ms);
    update_relay_abort();
    relay_until(RELAY_IDLE, 5000);
}

// While the pages stream, the main loop still gets a pass at least every
// SVAL_UPDATE_RELAY_STREAM_MS plus one RPC, also when RPCs fail: under
// Keybard's 50 ms (R13).
static void test_relay_stream_pass(void) {
    relay_fresh(0x13000);
    lk.drop_req = 5;
    lk.drop_rsp = 5;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_PAGES, 60000), RELAY_PAGES);
    uint64_t longest = 0;
    while (ri.phase == RELAY_PAGES) {
        uint64_t t0 = sim_us;
        pass();
        if (sim_us - t0 - 1000 > longest) longest = sim_us - t0 - 1000;
        relay_get();
    }
    CHECK(longest <= 50000);
    CHECK(longest >= SVAL_UPDATE_RELAY_STREAM_MS * 1000); // the budget is used
    memset(&lk, 0, sizeof(lk));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
    CHECK(slot_is_img(img_len));
    printf("stream: longest pass %llu us with 5%% of requests and answers lost\n", (unsigned long long)longest);
    update_relay_abort();
    relay_until(RELAY_IDLE, 5000);
}

static void test_relay_speed(void) {
    test_relay_dropout(1000);
    test_relay_dropout(3000);
    test_relay_dropout(6000);
    test_relay_erase_polls();
    test_relay_stream_pass();
}
