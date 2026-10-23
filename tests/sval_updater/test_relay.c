// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for M2's store and forward (update_split.c, and updater.c's
// session for the other half): the relay from the half with USB against the
// real slave code over the simulated split link of test_session.c, with
// dropped, damaged, duplicated, stale and reordered requests, link loss,
// the slave's 5 s timeout, mailbox races, idempotent PAGE, a power cut on the
// slave after every flash operation, and the version mismatch a half-done
// pair update leaves. Included by test_updater.c after test_session.c (and
// after test_commit.c in the build with the real commit, where the slave's
// commit is cut after every flash operation too).

// An image for the other half: left, TrackPoint, fw 2002.
static void make_slave_update(uint32_t len) {
    make_update(len);
    sval_update_manifest_t *m = blob_manifest();
    m->hand                   = UPDATE_HAND_LEFT;
    m->pointing_id            = UPDATE_POINTING_TRACKPOINT;
    m->fw_version             = 2002;
}

// The relay alone: this half's slot holds the verified image (master_slot),
// the die's slot is the other half's (all 0x00: every sector needs erasing).
static void relay_fresh(uint32_t len) {
    fresh(len);
    make_slave_update(len);
    master_slot = img;
    snap();
}

static update_relay_info_t ri;
static void                relay_get(void) {
    update_relay_info(&ri);
}

// Invariants watched on every pass of a relay: the other half erases,
// verifies and commits only behind the pause, and the pause is never held
// while the image streams (both halves type then).
static int  pause_violations, stream_paused;
static void watch(void) {
    uint8_t st   = update_split_slave_state();
    bool    slow = st == UPDATE_STATE_VERIFYING_MANIFEST || st == UPDATE_STATE_ERASING || st == UPDATE_STATE_VERIFYING_IMAGE || st == UPDATE_STATE_COMMITTING;
    relay_get();
    if (slow && !link_paused && update_relay_busy()) pause_violations++;
    if (ri.phase == RELAY_PAGES && link_paused) stream_paused++;
}

// Erases happen behind the pause: checked at every ROM op (after_op).
static int  erase_unpaused;
static void erase_watch(int op) {
    (void)op;
    if (noplog > 0 && noplog <= (int)(sizeof(oplog) / sizeof(oplog[0])) && oplog[noplog - 1].op == 'E' && !link_paused) erase_unpaused++;
}

// Passes until the relay reaches phase (or ends), up to max_ms.
static uint8_t relay_until(uint8_t phase, uint32_t max_ms) {
    for (uint32_t i = 0; i < max_ms; i++) {
        relay_get();
        if (ri.phase == phase || ri.phase == RELAY_FAILED || ri.phase == RELAY_DONE || ri.phase == RELAY_IDLE) break;
        pass();
        watch();
    }
    relay_get();
    return ri.phase;
}

static bool slot_is_img(uint32_t len) {
    return memcmp(update_host_flash + SVAL_UPDATE_BASE, img, len) == 0;
}

// The other half's firmware area and everything but its slot are as before.
static bool only_slot_changed(void) {
    return unchanged_outside(SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE);
}

// ---- the relay, start to finish ------------------------------------------------------------

static void test_relay_happy(uint32_t len) {
    relay_fresh(len);
    pause_violations = stream_paused = erase_unpaused = 0;
    after_op                                          = erase_watch;
    CHECK(update_relay_start(blob, len, body_crc()));
    CHECK(link_paused); // from the start: the manifest check and erase follow the manifest
    CHECK(update_relay_busy());
    CHECK(updater_active());
    CHECK(!update_relay_start(blob, len, body_crc())); // one at a time
    uint64_t t0             = sim_us;
    uint64_t pages_max_pass = 0;
    for (int i = 0; i < 200000; i++) {
        relay_get();
        if (ri.phase == RELAY_VERIFIED || ri.phase == RELAY_FAILED) break;
        uint8_t  before = ri.phase;
        uint64_t p0     = sim_us;
        pass();
        if (before == RELAY_PAGES) {
            relay_get();
            if (ri.phase == RELAY_PAGES && sim_us - p0 - 1000 > pages_max_pass) pages_max_pass = sim_us - p0 - 1000;
        }
        watch();
        if (update_split_slave_state() == UPDATE_STATE_RECEIVING) CHECK_EQ(led_now, UPDATE_LED_PROGRESS); // the other half's LEDs (D24)
    }
    relay_get();
    CHECK_EQ(ri.phase, RELAY_VERIFIED);
    CHECK_EQ(ri.error, UPDATE_OK);
    CHECK_EQ(ri.acked, len);
    CHECK_EQ(ri.slave_state, UPDATE_STATE_VERIFIED);
    CHECK_EQ(ri.slave_to_erase, (len + 4095) / 4096);
    CHECK_EQ(ri.retries, 0);
    CHECK(!link_paused);
    CHECK_EQ(pause_calls, 2); // the manifest and erase; END and the verify
    CHECK_EQ(pause_violations, 0);
    CHECK_EQ(stream_paused, 0);
    CHECK_EQ(erase_unpaused, 0);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFIED);
    CHECK(slot_is_img(len));
    CHECK(only_slot_changed());
    CHECK_EQ(program_ops, (int)(len / 256));
    CHECK_EQ(sequence_errors, 0);
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(lock_errors, 0);
    CHECK_EQ(lock_depth, 0);
    // The per-pass budget while the image streams: SVAL_UPDATE_RELAY_PASS_MS,
    // plus at most one RPC and one page program.
    CHECK(pages_max_pass <= ((SVAL_UPDATE_RELAY_PASS_MS + 1) * 1000 + SIM_RPC_US + SIM_PAGE_PROGRAM_US)); // ms timer granularity
    uint64_t relay_ms = (sim_us - t0) / 1000;

    // Kept alive while the host decides: no timeout on the other half.
    for (int i = 0; i < 12000; i++)
        pass();
    relay_get();
    CHECK_EQ(ri.phase, RELAY_VERIFIED);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFIED);
    CHECK(!link_paused);

    // COMMIT: paused, the other half commits; the link stays paused for its
    // commit and reboot (R18), then one probe.
    slave_commit_resets = true;
    CHECK(update_relay_commit());
    CHECK(link_paused);
    CHECK(!update_relay_commit());
    uint64_t c0         = sim_us;
    uint64_t resumed_at = 0;
    for (int i = 0; i < 20000; i++) {
        pass();
        watch();
        relay_get();
        if (!link_paused && !resumed_at) resumed_at = sim_us;
        if (ri.phase == RELAY_DONE || ri.phase == RELAY_FAILED) break;
    }
    CHECK_EQ(ri.phase, RELAY_DONE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_commit_crc, body_crc());
    CHECK_EQ(slave_commit_len, len);
    CHECK_EQ(slave_reboots, 1);
    CHECK_EQ(slave_id.fw_version, 2002);
    CHECK(!link_paused);
    CHECK(resumed_at - c0 >= (uint64_t)(SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS) * 1000);
    CHECK_EQ(pause_violations, 0);
    CHECK_EQ(commit_runs, 0); // never this half's commit
    after_op = NULL;
    printf("relay 0x%x: %u RPCs, %llu ms to verified in simulated time\n", (unsigned)len, (unsigned)lk.n, (unsigned long long)relay_ms);
}

// The other half refuses: its own checks (D7), the commit, its settings. It
// never refuses for the image's hand or pointing device (D33): see
// test_relay_slave_mismatch.
static void test_relay_slave_refusals(void) {
    const int f0 = failures;
    static const struct {
        const char     *what;
        update_status_t want;
    } cases[] = {
        {"nonzero reserved byte", UPDATE_INVALID},
        {"bad hash", UPDATE_BAD_HASH},
        {"settings failing", UPDATE_UNAVAILABLE},
        {"version 1", UPDATE_UNSUPPORTED},
    };
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        relay_fresh(0x2000);
        sval_update_manifest_t *m = blob_manifest();
        switch (c) {
            case 0:
                m->reserved0 = 1;
                break; // version 1's key_id
            case 1:
                img[0x1234] ^= 1;
                break; // the image differs from the manifest's hash
            case 2:
                port_failing = true;
                break;
            case 3:
                m->manifest_ver = 1;
                break;
        }
        CHECK(update_relay_start(blob, img_len, body_crc()));
        uint8_t ph = relay_until(RELAY_VERIFIED, 60000);
        CHECK_EQ(ph, RELAY_FAILED);
        CHECK_EQ(ri.error, cases[c].want);
        CHECK(!link_paused);
        // the relay's ABORT cleared the other half; nothing outside its slot changed
        CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
        CHECK(only_slot_changed());
        if (c != 1) CHECK_EQ(erase_ops, 0); // refused before any erase
        CHECK_EQ(slave_commit_runs, 0);
        if (failures != f0) fprintf(stderr, "  (slave refusal: %s)\n", cases[c].what);
        port_failing = false;
    }
    // A commit the other half refuses at its step 0: the probe after the hold
    // finds it in ERROR (it did not reset), so the relay fails.
    relay_fresh(0x2000);
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
    slave_commit_result = UPDATE_BUSY;
    CHECK(update_relay_commit());
    CHECK_EQ(relay_until(RELAY_DONE, 20000), RELAY_FAILED);
    CHECK_EQ(ri.error, UPDATE_BUSY);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK(!link_paused);
    // A COMMIT for another image: refused, BAD_HASH; the relay fails before any commit.
    relay_fresh(0x2000);
    CHECK(update_relay_start(blob, img_len, body_crc() ^ 1));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_FAILED); // the other half's CRC differs at VERIFIED
    CHECK_EQ(ri.error, UPDATE_BAD_HASH);
    CHECK_EQ(slave_commit_runs, 0);
}

// ---- faults on the link ----------------------------------------------------------------

// Gentle (D33): the other half checks a relayed manifest itself (D7) but never
// refuses it for its hand or pointing device: such an image is verified there.
static void test_relay_slave_mismatch(void) {
    for (int c = 0; c < 3; c++) {
        relay_fresh(0x2000);
        sval_update_manifest_t *m = blob_manifest();
        if (c == 0) m->hand = UPDATE_HAND_RIGHT;
        if (c == 1) m->pointing_id = UPDATE_POINTING_PMW3360;
        if (c == 2) m->pointing_id = UPDATE_POINTING_AUTO;
        CHECK(update_relay_start(blob, img_len, body_crc()));
        CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
        CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFIED);
        CHECK_EQ(slave_commit_runs, 0);
    }
}

static void test_relay_faults(void) {
    const int f0      = failures;
    int       ok_runs = 0, clean_fails = 0;
    for (int run = 0; run < 24; run++) {
        bool hostile = run >= 12; // also replays of older requests (not seen on the real link: paranoia)
        relay_fresh(0x1800 + 0x100 * (uint32_t)run);
        rng              = 1000u + (uint32_t)run * 7919u;
        lk.drop_req      = 3;
        lk.drop_rsp      = 3;
        lk.flip_req      = 3;
        lk.flip_rsp      = 3;
        lk.dup           = 3;
        lk.stale         = 3;
        lk.replay        = hostile ? 2 : 0;
        pause_violations = stream_paused = 0;
        CHECK(update_relay_start(blob, img_len, body_crc()));
        uint8_t ph = relay_until(RELAY_VERIFIED, 300000);
        if (ph == RELAY_VERIFIED) {
            ok_runs++;
            CHECK(slot_is_img(img_len));
            // and the commit, through the same faults
            slave_commit_resets = true;
            CHECK(update_relay_commit());
            ph = relay_until(RELAY_DONE, 30000);
            CHECK(ph == RELAY_DONE || (hostile && ph == RELAY_FAILED));
            if (ph == RELAY_DONE) {
                CHECK_EQ(slave_commit_runs, 1);
                CHECK_EQ(slave_commit_crc, body_crc());
            }
        } else {
            // Only a hostile run may fail (an old ABORT or BEGIN replayed cancels
            // the session), and then cleanly.
            CHECK(hostile);
            CHECK_EQ(ph, RELAY_FAILED);
            clean_fails++;
            CHECK(!link_paused);
            for (int i = 0; i < 6000; i++)
                pass(); // the other half drops it by itself
            CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
            CHECK_EQ(slave_commit_runs, 0);
        }
        CHECK(only_slot_changed());
        CHECK_EQ(pause_violations, 0);
        CHECK_EQ(stream_paused, 0);
        CHECK_EQ(sequence_errors, 0);
        CHECK_EQ(lock_errors, 0);
        CHECK(slave_commit_runs <= 1);
        if (failures != f0) {
            fprintf(stderr, "  (fault run %d, phase %d, error %d, %u faults)\n", run, ri.phase, ri.error, (unsigned)lk.faults);
            return;
        }
    }
    CHECK(ok_runs >= 12);
    printf("faulty link: %d relays verified and committed, %d hostile runs failed cleanly\n", ok_runs, clean_fails);
}

// The link goes (the split cable pulled) in each phase: this half fails within the link
// timeout and resumes the link; the other half drops its session within 5 s
// of the last request it got; neither half's firmware changes.
static void test_relay_link_loss(void) {
    const int f0 = failures;
    // (RELAY_MANIFEST is a few RPCs inside one pass: covered by the power cuts and faults.)
    static const uint8_t phases[] = {RELAY_ERASE_WAIT, RELAY_PAGES, RELAY_VERIFY_WAIT, RELAY_VERIFIED, RELAY_COMMIT, RELAY_HOLD};
    for (size_t k = 0; k < sizeof(phases); k++) {
        relay_fresh(0x6000);
        CHECK(update_relay_start(blob, img_len, body_crc()));
        uint8_t want = phases[k];
        if (want == RELAY_COMMIT || want == RELAY_HOLD) {
            CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
            CHECK(update_relay_commit());
            if (want == RELAY_HOLD) relay_until(RELAY_HOLD, 1000);
        } else if (want == RELAY_PAGES) {
            relay_until(RELAY_PAGES, 60000);
            while (ri.phase == RELAY_PAGES && ri.acked < img_len / 2) {
                pass();
                relay_get();
            }
        } else {
            relay_until(want, 60000);
        }
        relay_get();
        CHECK_EQ(ri.phase, want);
        link_up                = false;
        uint64_t t0            = sim_us;
        uint64_t slave_idle_at = 0, failed_at = 0, resumed_at = 0;
        for (int i = 0; i < 12000; i++) {
            pass();
            relay_get();
            if (!resumed_at && !link_paused) resumed_at = sim_us;
            if (!failed_at && (ri.phase == RELAY_FAILED || ri.phase == RELAY_DONE)) failed_at = sim_us;
            if (!slave_idle_at && update_split_slave_state() == UPDATE_STATE_IDLE) slave_idle_at = sim_us;
        }
        if (want == RELAY_HOLD) {
            // COMMIT was taken: the probe goes unanswered, which is success (V)
            CHECK_EQ(ri.phase, RELAY_DONE);
            CHECK_EQ(slave_commit_runs, 1);
        } else if (want == RELAY_COMMIT) {
            // COMMIT never arrived. This half cannot tell that from a COMMIT
            // taken with its answer lost and the other half now writing, so
            // it holds the link paused for the whole hold after the link
            // timeout, and the unanswered probe ends it DONE, unconfirmed
            // (the host then checks presence). The other half never commits.
            CHECK_EQ(ri.phase, RELAY_DONE);
            CHECK(ri.unconfirmed);
            CHECK_EQ(slave_commit_runs, 0);
            CHECK(resumed_at - t0 >= (uint64_t)(SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS + SVAL_UPDATE_SPLIT_COMMIT_MS + SVAL_UPDATE_SPLIT_REBOOT_MS) * 1000);
            CHECK(update_split_awaiting_match()); // INFO flags bit5 (and the after-update light) until presence shows MATCH
            CHECK(slave_idle_at > 0);             // its session times out
        } else {
            CHECK_EQ(ri.phase, RELAY_FAILED);
            CHECK_EQ(ri.error, UPDATE_UNAVAILABLE);
            CHECK(failed_at - t0 <= (uint64_t)(SVAL_UPDATE_RELAY_LINK_TIMEOUT_MS + 100) * 1000);
            CHECK_EQ(slave_commit_runs, 0);
            // the other half: IDLE (TIMEOUT) 5 s after the last request it got
            CHECK(slave_idle_at > 0);
            CHECK(slave_idle_at - last_slave_rx_us >= (uint64_t)SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS * 1000);
            CHECK(slave_idle_at - last_slave_rx_us <= (uint64_t)SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS * 1000 + 2000);
            CHECK_EQ(led_now, UPDATE_LED_NONE); // its LEDs are back
        }
        CHECK(!link_paused);
        CHECK(only_slot_changed());
        // the link back: a new relay from the start works
        link_up = true;
        if (want != RELAY_HOLD) {
            slave_commit_resets = true;
            CHECK(update_relay_start(blob, img_len, body_crc()));
            CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
            CHECK(slot_is_img(img_len));
            CHECK(update_relay_commit());
            CHECK_EQ(relay_until(RELAY_DONE, 20000), RELAY_DONE);
            CHECK_EQ(slave_commit_runs, 1);
        }
        if (failures != f0) {
            fprintf(stderr, "  (link loss in phase %d)\n", want);
            return;
        }
    }
}

// The other half on its own: a session the master stops feeding ends 5 s
// after its last request in every state but COMMITTING, keeping the reason,
// and its LEDs come back.
static void test_relay_slave_timeout(void) {
    const int f0 = failures;
    for (int k = 0; k < 5; k++) {
        relay_fresh(0x2000);
        uint8_t           want = 0;
        update_led_mode_t led  = UPDATE_LED_PROGRESS;
        switch (k) {
            case 0: { // MANIFEST_LOADING: one fragment
                uint8_t req[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
                slave_rpc(req, usplit_req_begin(req, 0x31, 0, blob, 26), out, USPLIT_MSG_MAX);
                last_slave_rx_us = sim_us;
                want             = UPDATE_STATE_MANIFEST_LOADING;
                break;
            }
            case 1: // RECEIVING
                CHECK(update_relay_start(blob, img_len, body_crc()));
                relay_until(RELAY_PAGES, 60000);
                pass();
                want = UPDATE_STATE_RECEIVING;
                break;
            case 2: // VERIFIED
                CHECK(update_relay_start(blob, img_len, body_crc()));
                relay_until(RELAY_VERIFIED, 60000);
                want = UPDATE_STATE_VERIFIED;
                break;
            case 3: { // ERROR: a manifest the other half refuses (a nonzero reserved byte)
                uint8_t bad[UPDATE_MANIFEST_BYTES];
                memcpy(bad, blob, sizeof(bad));
                bad[6] = 1;
                for (uint32_t off = 0; off < sizeof(bad); off += 26) {
                    uint8_t req[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
                    uint8_t n = sizeof(bad) - off < 26 ? (uint8_t)(sizeof(bad) - off) : 26;
                    slave_rpc(req, usplit_req_begin(req, (uint8_t)(0x50 + off), (uint8_t)off, bad + off, n), out, USPLIT_MSG_MAX);
                }
                last_slave_rx_us = sim_us;
                slave_hk();
                want = UPDATE_STATE_ERROR;
                led  = UPDATE_LED_ERROR;
                break;
            }
            default: // COMMITTING
                CHECK(update_relay_start(blob, img_len, body_crc()));
                relay_until(RELAY_VERIFIED, 60000);
                CHECK(update_relay_commit());
                update_relay_task();
                want = UPDATE_STATE_COMMITTING;
                led  = UPDATE_LED_WRITING;
                break;
        }
        CHECK_EQ(update_split_slave_state(), want);
        updater_task(); // the LEDs, as the other half shows them (this half is idle here)
        CHECK_EQ(led_now, led);
        if (want == UPDATE_STATE_COMMITTING) {
            // Never timed out once COMMIT is accepted (D23): 6 s later the commit still runs.
            sim_us += 6000000;
            slave_hk();
            CHECK_EQ(slave_commit_runs, 1);
            CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERROR); // the mock commit refused
            update_relay_abort();
            continue;
        }
        // this half goes silent (as if it froze, or the link went)
        uint64_t idle_at = 0;
        for (int i = 0; i < 8000; i++) {
            slave_hk();
            sim_us += 1000;
            if (!idle_at && update_split_slave_state() == UPDATE_STATE_IDLE) idle_at = sim_us;
        }
        CHECK(idle_at > 0);
        CHECK(idle_at - last_slave_rx_us >= (uint64_t)SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS * 1000);
        CHECK(idle_at - last_slave_rx_us <= (uint64_t)SVAL_UPDATE_SPLIT_SLAVE_TIMEOUT_MS * 1000 + 1000);
        uint8_t      req[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
        usplit_rsp_t rs;
        slave_rpc(req, usplit_req_build(req, USPLIT_OP_STATUS, 0x77, NULL, 0), out, USPLIT_MSG_MAX);
        CHECK_EQ(usplit_rsp_parse(out, usplit_rsp_len(USPLIT_OP_STATUS), USPLIT_OP_STATUS, 0x77, &rs), UPDATE_OK);
        CHECK_EQ(rs.payload[0], want == UPDATE_STATE_ERROR ? UPDATE_INVALID : UPDATE_TIMEOUT); // the reason stays
        updater_task();
        CHECK_EQ(led_now, UPDATE_LED_NONE);
        CHECK_EQ(slave_commit_runs, 0);
        CHECK(only_slot_changed());
        update_relay_abort();
        if (failures != f0) {
            fprintf(stderr, "  (slave timeout in state %d)\n", want);
            return;
        }
    }
}

// ---- the mailbox ----------------------------------------------------------------------------

// RPCs to the other half outside the relay (as the master would send them).
static uint8_t         mseq = 0x40;
static usplit_rsp_t    mrs;
static uint8_t         mout[USPLIT_MSG_MAX];
static update_status_t mrpc(uint8_t *req, uint8_t len) {
    slave_rpc(req, len, mout, USPLIT_MSG_MAX);
    if (usplit_rsp_parse(mout, usplit_rsp_len(req[0]), req[0], req[1], &mrs) != UPDATE_OK) return (update_status_t)0xEE;
    return (update_status_t)mrs.status;
}
static update_status_t m_op(uint8_t op) {
    uint8_t req[USPLIT_MSG_MAX];
    return mrpc(req, usplit_req_build(req, op, ++mseq, NULL, 0));
}
static update_status_t m_manifest(const uint8_t *manifest) {
    for (uint32_t off = 0; off < UPDATE_MANIFEST_BYTES; off += USPLIT_BEGIN_DATA_MAX) {
        uint8_t         n = UPDATE_MANIFEST_BYTES - off < USPLIT_BEGIN_DATA_MAX ? (uint8_t)(UPDATE_MANIFEST_BYTES - off) : USPLIT_BEGIN_DATA_MAX;
        uint8_t         req[USPLIT_MSG_MAX];
        update_status_t st = mrpc(req, usplit_req_begin(req, ++mseq, (uint8_t)off, manifest + off, n));
        if (st != UPDATE_OK) return st;
    }
    return UPDATE_OK;
}
static update_status_t m_page(uint32_t off, uint8_t n) {
    uint8_t req[USPLIT_MSG_MAX];
    return mrpc(req, usplit_req_page(req, ++mseq, off, img + off, n));
}
// Housekeeping passes, each after a STATUS poll as the master sends them (a
// sector is erased only after a request, SVAL_UPDATE_SPLIT_ERASE_GAP_MS ago).
static void slave_ticks(int n) {
    for (int i = 0; i < n; i++) {
        uint8_t req[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
        slave_rpc(req, usplit_req_build(req, USPLIT_OP_STATUS, ++mseq, NULL, 0), out, USPLIT_MSG_MAX);
        sim_us += SVAL_UPDATE_SPLIT_ERASE_GAP_MS * 1000;
        slave_hk();
        sim_us += 1000;
    }
}

// The race injected at the first unlock of the slave's housekeeping pass,
// right after it copied its job out: what the SlaveThread could do there.
static int     race_kind, race_fired;
static uint8_t other_blob[UPDATE_MANIFEST_BYTES];
static void    race(int n) {
    (void)n;
    if (race_fired || !race_kind) return;
    race_fired  = 1;
    unlock_hook = NULL;
    lock_depth++; // the callback runs with the lock (the SlaveThread holds it)
    CHECK_EQ(m_op(USPLIT_OP_ABORT), race_kind == 4 ? UPDATE_BUSY : UPDATE_OK);
    if (race_kind == 2) CHECK_EQ(m_manifest(other_blob), UPDATE_OK); // and a new session with another manifest
    lock_depth--;
}

static void test_relay_mailbox(void) {
    // 1: ABORT while the manifest check runs: the check's result is dropped,
    // nothing is erased.
    relay_fresh(0x2000);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFYING_MANIFEST);
    race_kind = 1, race_fired = 0;
    unlock_hook = race;
    slave_ticks(1);
    CHECK_EQ(race_fired, 1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    slave_ticks(100);
    CHECK_EQ(erase_ops, 0);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);

    // 2: ABORT and a new session with a manifest the other half refuses (a
    // nonzero reserved byte), while the good one is being checked: the stale
    // OK must not move the new session on; the new one is checked on its own
    // and refused.
    relay_fresh(0x2000);
    memcpy(other_blob, blob, sizeof(other_blob));
    other_blob[6] = 0x80;
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    race_kind = 2, race_fired = 0;
    unlock_hook = race;
    slave_ticks(1);
    CHECK_EQ(race_fired, 1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFYING_MANIFEST); // the new one, not ERASING
    slave_ticks(1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(m_op(USPLIT_OP_STATUS), UPDATE_OK);
    CHECK_EQ(mrs.payload[0], UPDATE_INVALID);
    CHECK_EQ(erase_ops, 0);

    // 3: ABORT while a sector erases: the erase lands (slot only) but the
    // session stays canceled.
    relay_fresh(0x3000);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    slave_ticks(1); // manifest checked: ERASING
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERASING);
    race_kind = 1, race_fired = 0;
    unlock_hook = race;
    slave_ticks(1);
    CHECK_EQ(erase_ops, 1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    slave_ticks(50);
    CHECK_EQ(erase_ops, 1);
    CHECK(only_slot_changed());

    // 4: ABORT during the last verify slice: never VERIFIED.
    relay_fresh(0x800);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    slave_ticks(10);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    for (uint32_t off = 0; off < img_len;) {
        uint8_t n = 256 - off % 256 < 24 ? (uint8_t)(256 - off % 256) : 24;
        CHECK_EQ(m_page(off, n), UPDATE_OK);
        off += n;
    }
    CHECK_EQ(m_op(USPLIT_OP_END), UPDATE_ACCEPTED);
    race_kind = 1, race_fired = 0;
    unlock_hook = race;
    slave_ticks(1); // 0x800 bytes: one slice
    CHECK_EQ(race_fired, 1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);

    // 5: ABORT once COMMIT is accepted: refused (BUSY), the commit runs.
    relay_fresh(0x800);
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
    CHECK(update_relay_commit());
    for (int i = 0; i < 5 && update_split_slave_state() != UPDATE_STATE_COMMITTING; i++)
        update_relay_task();
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_COMMITTING);
    race_kind = 4, race_fired = 0;
    unlock_hook = race;
    slave_ticks(200);
    CHECK_EQ(race_fired, 1);
    CHECK_EQ(slave_commit_runs, 1);
    race_kind   = 0;
    unlock_hook = NULL;
    update_relay_abort();
    CHECK_EQ(lock_errors, 0);
}

// PAGE and the other requests against the other half directly: idempotent by
// offset, every refusal leaves the session as it was.
static void test_relay_pages(void) {
    relay_fresh(0x400);
    CHECK_EQ(m_page(0, 24), UPDATE_INVALID); // no session
    CHECK_EQ(m_op(USPLIT_OP_END), UPDATE_INVALID);
    uint8_t req[USPLIT_MSG_MAX];
    CHECK_EQ(mrpc(req, usplit_req_commit(req, ++mseq, (uint16_t)body_crc())), UPDATE_INVALID);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    CHECK_EQ(m_page(0, 24), UPDATE_BUSY); // still checking the manifest
    // a repeat of a manifest fragment is acknowledged; BEGIN at 0 restarts only
    // over a half-sent manifest
    CHECK_EQ(mrpc(req, usplit_req_begin(req, ++mseq, 26, blob + 26, 26)), UPDATE_OK);
    CHECK_EQ(mrpc(req, usplit_req_begin(req, ++mseq, 0, blob, 26)), UPDATE_BUSY);
    slave_ticks(1);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERASING);
    CHECK_EQ(m_page(0, 24), UPDATE_BUSY);
    slave_ticks(5);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    int programs = program_ops;
    CHECK_EQ(m_page(24, 24), UPDATE_OUT_OF_ORDER); // ahead
    CHECK_EQ(get24le(mrs.payload), 0);
    CHECK_EQ(m_op(USPLIT_OP_END), UPDATE_OUT_OF_ORDER); // too early
    uint32_t off = 0;
    while (off < 256) {
        uint8_t n = 256 - off < 24 ? (uint8_t)(256 - off) : 24;
        CHECK_EQ(m_page(off, n), UPDATE_OK);
        CHECK_EQ(m_page(off, n), UPDATE_OK); // a repeat, new seq: acknowledged
        off += n;
        CHECK_EQ(get24le(mrs.payload), off);
    }
    CHECK_EQ(program_ops, programs + 1);  // the page, once
    CHECK_EQ(m_page(232, 24), UPDATE_OK); // the completing fragment again: not programmed again
    CHECK_EQ(m_page(0, 24), UPDATE_OK);
    CHECK_EQ(program_ops, programs + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // a retry of the same frame (same seq): answered from the cache
    uint8_t len = usplit_req_page(req, ++mseq, 256, img + 256, 24);
    CHECK_EQ(mrpc(req, len), UPDATE_OK);
    uint8_t first[USPLIT_MSG_MAX];
    memcpy(first, mout, sizeof(first));
    CHECK_EQ(mrpc(req, len), UPDATE_OK);
    CHECK(memcmp(first, mout, sizeof(first)) == 0);
    for (off = 280; off < img_len;) {
        uint8_t n = 256 - off % 256 < 24 ? (uint8_t)(256 - off % 256) : 24;
        CHECK_EQ(m_page(off, n), UPDATE_OK);
        off += n;
    }
    CHECK_EQ(program_ops, programs + 4);
    CHECK_EQ(m_page(img_len, 24), UPDATE_OVERRUN); // past the end: latched
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(m_op(USPLIT_OP_ABORT), UPDATE_OK);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);

    // END and COMMIT repeated (their answers lost, the cache overwritten):
    // acknowledged again, one commit.
    relay_fresh(0x400);
    CHECK_EQ(m_manifest(blob), UPDATE_OK);
    slave_ticks(10);
    for (off = 0; off < img_len;) {
        uint8_t n = 256 - off % 256 < 24 ? (uint8_t)(256 - off % 256) : 24;
        CHECK_EQ(m_page(off, n), UPDATE_OK);
        off += n;
    }
    CHECK_EQ(m_op(USPLIT_OP_END), UPDATE_ACCEPTED);
    CHECK_EQ(m_op(USPLIT_OP_END), UPDATE_ACCEPTED);
    slave_ticks(5);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(m_op(USPLIT_OP_STATUS), UPDATE_OK);
    CHECK_EQ(get16le(&mrs.payload[8]), body_crc() & 0xFFFF);
    CHECK_EQ(mrpc(req, usplit_req_commit(req, ++mseq, (uint16_t)(body_crc() ^ 1))), UPDATE_BAD_HASH);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(mrpc(req, usplit_req_commit(req, ++mseq, (uint16_t)body_crc())), UPDATE_ACCEPTED);
    CHECK_EQ(mrpc(req, usplit_req_commit(req, ++mseq, (uint16_t)body_crc())), UPDATE_ACCEPTED);
    CHECK_EQ(m_op(USPLIT_OP_ABORT), UPDATE_BUSY);
    slave_ticks(200);
    CHECK_EQ(slave_commit_runs, 1);

    // The stage-1 checks still hold: INFO, a damaged frame, random frames.
    relay_fresh(0x400);
    CHECK_EQ(m_op(USPLIT_OP_INFO), UPDATE_OK);
    CHECK_EQ(mrs.payload[1], UPDATE_HAND_LEFT);
    CHECK_EQ(mrs.payload[2], UPDATE_POINTING_TRACKPOINT);
    CHECK_EQ(mrs.payload[14], 0x01); // 16 MiB die; bit4 (protocol 1's test key) stays clear
    len = usplit_req_build(req, USPLIT_OP_INFO, ++mseq, NULL, 0);
    req[len - 1] ^= 1;
    CHECK_EQ(mrpc(req, len), UPDATE_INVALID);
    uint32_t r = 7;
    for (int t = 0; t < 20000; t++) {
        uint8_t in[USPLIT_MSG_MAX], o[USPLIT_MSG_MAX];
        uint8_t l = (uint8_t)(t % (USPLIT_MSG_MAX + 1));
        for (int i = 0; i < l; i++) {
            r     = r * 1103515245u + 12345u;
            in[i] = (uint8_t)(r >> 16);
        }
        if (l >= 1 && t % 3 == 0) in[0] %= USPLIT_OP_COUNT;
        if (l >= 4 && t % 2 == 0) {
            uint16_t c = usplit_crc16(in, l - 2);
            in[l - 2]  = c & 0xFF;
            in[l - 1]  = c >> 8;
        }
        slave_rpc(in, l, o, USPLIT_MSG_MAX);
        bool zero = true;
        for (int i = 0; i < USPLIT_MSG_MAX; i++)
            zero &= o[i] == 0;
        if (!zero) {
            CHECK(l >= 2 && o[1] < USPLIT_OP_COUNT);
            if (l >= 2 && o[1] < USPLIT_OP_COUNT) CHECK_EQ(usplit_rsp_parse(o, usplit_rsp_len(o[1]), o[1], o[2], &mrs), UPDATE_OK);
        }
        if (t % 500 == 0) slave_ticks(1);
    }
    CHECK(update_split_slave_state() != UPDATE_STATE_COMMITTING);
    CHECK_EQ(slave_commit_runs, 0);
    CHECK(only_slot_changed());
    CHECK_EQ(sequence_errors, 0);
}

// ---- power cuts on the other half ----------------------------------------------------------

// A relay with the other half's power cut after its ROM op cut_k (0: none),
// then rebooted; this half carries on. Returns the ROM ops made.
static int relay_with_cut(uint32_t len, int cut_k, bool tear, bool *cut_hit) {
    relay_fresh(len);
    cut_at   = cut_k;
    cut_tear = tear;
    *cut_hit = false;
    if (setjmp(cut_env) == 0) {
        CHECK(update_relay_start(blob, len, body_crc()));
        relay_until(RELAY_VERIFIED, 60000);
    } else {
        // the other half lost power mid-operation: its CPU state is gone
        *cut_hit   = true;
        sim_slave  = false;
        irq_off    = false;
        xip_on     = true;
        connected  = false;
        lock_depth = 0;
        cut_at     = 0;
        slave_reboot();
        relay_until(RELAY_VERIFIED, 60000);
    }
    return flash_ops;
}

static void test_relay_cuts(void) {
    const int      f0  = failures;
    const uint32_t len = 0x2000;
    bool           hit;
    int            total = relay_with_cut(len, 0, false, &hit);
    CHECK(!hit);
    relay_get();
    CHECK_EQ(ri.phase, RELAY_VERIFIED);
    update_relay_abort();
    CHECK(total > 8);
    int cuts = 0;
    for (int tear = 0; tear < 2; tear++) {
        for (int k = 1; k <= total; k++) {
            relay_with_cut(len, k, tear, &hit);
            CHECK(hit);
            cuts++;
            relay_get();
            // this half saw the other one lose its session: failed, link resumed
            CHECK_EQ(ri.phase, RELAY_FAILED);
            CHECK(!link_paused);
            // (a)/(c): only the other half's slot changed; its firmware boots
            CHECK(only_slot_changed());
            CHECK_EQ(outside_ops, 0);
            CHECK_EQ(sequence_errors, 0);
            CHECK_EQ(slave_commit_runs, 0);
            // and a new relay over the dirty slot succeeds
            update_relay_abort();
            CHECK(update_relay_start(blob, len, body_crc()));
            CHECK_EQ(relay_until(RELAY_VERIFIED, 60000), RELAY_VERIFIED);
            CHECK(slot_is_img(len));
            update_relay_abort();
            if (failures != f0) {
                fprintf(stderr, "  (cut after slave ROM op %d of %d, %s)\n", k, total, tear ? "torn" : "clean");
                return;
            }
        }
    }
    printf("slave power cuts: %d ROM ops in a relay, %d cuts, old image kept, retry succeeds\n", total, cuts);
}

// ---- updater.c: a session for the other half -----------------------------------------------

static update_status_t send_commit(uint32_t client, uint32_t crc) {
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = crc & 0xFF;
    c[5] = (crc >> 8) & 0xFF;
    return send(client, UPDATE_OP_COMMIT, c, 6);
}

// Presence from the pings, as svalboard.c does (MATCH with the same release).
static void presence_up(void) {
    presence_sim = true;
    last_ping_us = 0;
    for (int i = 0; i < 600; i++)
        pass();
}

static bool session_to_relayed(uint32_t len) {
    req_hand = UPDATE_HAND_LEFT;
    if (!to_verified(CLIENT_A)) return false;
    // the image is now in this half's slot; the die's slot is the other half's
    memcpy(shadow, update_host_flash + SVAL_UPDATE_BASE, len);
    master_slot = shadow;
    memset(update_host_flash + SVAL_UPDATE_BASE, 0x5A, SVAL_UPDATE_SIZE);
    snap();
    if (send_commit(CLIENT_A, body_crc()) != UPDATE_ACCEPTED) return false;
    if (updater_state() != UPDATE_STATE_RELAYING) return false;
    for (int i = 0; i < 60000 && updater_state() == UPDATE_STATE_RELAYING; i++) {
        pass();
        if (i % 200 == 0) send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0); // the host polls
    }
    return updater_state() == UPDATE_STATE_RELAYED;
}

static void test_relay_session(void) {
    // INFO for the other half, over KEYBOARD_UPDATE
    fresh(0x2000);
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_IDLE);
    CHECK_EQ(rsp[2], UPDATE_PROTOCOL_VERSION);
    CHECK_EQ(rsp[12], UPDATE_POINTING_TRACKPOINT);
    CHECK_EQ(rsp[13], UPDATE_HAND_LEFT);
    CHECK_EQ(get32le(rsp + 14), 2001);
    CHECK_EQ(rsp[21] & 0x1F, 0x01);
    CHECK_EQ(get16le(rsp + 3), SVAL_UPDATE_BASE / 4096);
    // the piece sizes are this half's: the other half's image is staged here first
    CHECK_EQ(rsp[23], T_CHUNK);
    CHECK_EQ(rsp[24], T_MPIECE);
    CHECK_EQ(rsp[25], T_REPORT);
    link_up = false;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(rsp[1], 0xFF);
    link_up = true;

    // a session needs the other half's presence: MATCH or VERSION
    static const struct {
        uint8_t         hand;
        uint32_t        fw;
        bool            answers;
        update_status_t want;
    } pres[] = {
        {UPDATE_HAND_LEFT, 2001, true, UPDATE_OK},
        {UPDATE_HAND_LEFT, 1999, true, UPDATE_OK},           // VERSION: the fix for a mismatch
        {UPDATE_HAND_RIGHT, 2001, true, UPDATE_UNAVAILABLE}, // SAME_HAND
        {UPDATE_HAND_LEFT, 2001, false, UPDATE_UNAVAILABLE}, // NONE
    };
    for (size_t i = 0; i < sizeof(pres) / sizeof(pres[0]); i++) {
        fresh(0x2000);
        make_slave_update(0x2000);
        slave_id.hand       = pres[i].hand;
        slave_id.fw_version = pres[i].fw;
        link_dead           = !pres[i].answers;
        presence_up();
        req_hand = UPDATE_HAND_LEFT;
        CHECK_EQ(load_manifest(CLIENT_A), pres[i].want);
        if (pres[i].want == UPDATE_OK) {
            CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
            CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
            // presence gone between MANIFEST and ARM would have refused ARM
        } else {
            CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
        }
    }
    // the other half's pointing device and hand are not checked (D33): an image
    // naming this half's pointing device, or the right hand, is armed for the left
    for (int c = 0; c < 2; c++) {
        fresh(0x2000);
        make_slave_update(0x2000);
        if (c == 0) blob_manifest()->pointing_id = UPDATE_POINTING_PMW3389; // this half's, not the other's
        if (c == 1) blob_manifest()->hand = UPDATE_HAND_RIGHT;              // the request's hand picks the half
        presence_up();
        req_hand = UPDATE_HAND_LEFT;
        CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
        CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    }

    // The whole session: staged and verified here, relayed, committed there.
    // The host's chunks (T_CHUNK, 50 B) are larger than the relay's PAGE
    // fragments (24 B, over the split cable's 32-byte RPC buffers): the relay
    // reads whole pages back from this half's slot, so the two never line up.
    _Static_assert(T_CHUNK > USPLIT_PAGE_DATA_MAX, "a host chunk spans more than one relay fragment");
    _Static_assert(USPLIT_MSG_MAX == 32, "the relay's messages are unchanged");
    const uint32_t len = 0x5000;
    fresh(len);
    make_slave_update(len);
    presence_up();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    CHECK(session_to_relayed(len));
    CHECK_EQ(commit_runs, 0);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], RELAY_VERIFIED);
    CHECK_EQ(rsp[2], UPDATE_STATE_VERIFIED);
    CHECK_EQ(get24le(rsp + 4), len);
    CHECK_EQ(get24le(rsp + 7), len);
    CHECK_EQ(rsp[20] & 1, 0); // the link runs
    CHECK_EQ(rsp[22], UPDATE_STATE_RELAYED);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, len) == 0); // the other half's slot
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    // the other hand's session ops are BUSY; this half's TEST_HALT too
    req_hand       = UPDATE_HAND_RIGHT;
    uint8_t m0[22] = {0, 20};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, m0, sizeof(m0)), UPDATE_BUSY);
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_RELAYED);
    req_hand = UPDATE_HAND_LEFT;
    // INFO for the other half while a relay runs: BUSY
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_BUSY);
    // a COMMIT for another image: refused, still RELAYED
    CHECK_EQ(send_commit(CLIENT_A, body_crc() ^ 1), UPDATE_BAD_HASH);
    CHECK_EQ(updater_state(), UPDATE_STATE_RELAYED);
    CHECK_EQ(send_commit(CLIENT_B, body_crc()), UPDATE_OTHER_CLIENT);
    // COMMIT: the other half commits; this half never does
    slave_commit_resets = true;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    CHECK_EQ(updater_state(), UPDATE_STATE_SUBSIDE_COMMITTING);
    CHECK(link_paused);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS); // the other half writes: magenta is its own light, not this half's
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], RELAY_HOLD);
    CHECK_EQ(rsp[20] & 1, 1);
    for (int i = 0; i < 10000 && updater_state() == UPDATE_STATE_SUBSIDE_COMMITTING; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_commit_crc, body_crc());
    CHECK_EQ(commit_runs, 0);
    CHECK(!link_paused);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_OK);

    // Partial pair: the other half is on 2002, this one on 2001. Presence shows
    // VERSION and INFO flags bit5 (V); the lights stay rainbow on both halves
    // (the ping carries it) while this half's own install is still to come.
    led_seen = 0;
    for (int i = 0; i < 1200; i++)
        pass();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_VERSION);
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x20, 0x20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[22], UPDATE_PRESENCE_VERSION);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_UPDATING);
    CHECK_EQ(led_seen & ((1 << UPDATE_LED_ERROR) | (1 << UPDATE_LED_DONE)), 0);
    // the other half can still be updated through this one (VERSION is allowed)
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(get32le(rsp + 14), 2002);
    // this half to 2002 over USB (M1): then MATCH, the green flash, and the LEDs go back
    make_update(len);
    blob_manifest()->fw_version = 2002;
    master_slot                 = NULL;
    commit_avail                = true;
    master_commit_resets        = true;
    req_hand                    = UPDATE_HAND_RIGHT;
    CHECK(to_verified(CLIENT_A));
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);
    for (int i = 0; i < 300 && master_reboots == 0; i++)
        pass();
    CHECK_EQ(master_reboots, 1);
    CHECK_EQ(master_id.fw_version, 2002);
    CHECK(led_seen & (1 << UPDATE_LED_WRITING));
    // (the mock commit returns before its "reset", which the real one never
    // does: the pass it returns in is not looked at)
    led_seen = 0;
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS); // the boot after its own commit: until the halves match
    for (int i = 0; i < 1200; i++)
        pass();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    CHECK(led_seen & (1 << UPDATE_LED_DONE));
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0); // never red along the way
    CHECK_EQ(led_now, UPDATE_LED_NONE);

    // The same partial pair left as it is: rainbow while idle for
    // SVAL_UPDATE_AFTER_WAIT_MS after the relay, then red for
    // SVAL_UPDATE_RED_MS, then nothing; INFO flags bit5 stays.
    fresh(len);
    make_slave_update(len);
    presence_up();
    CHECK(session_to_relayed(len));
    slave_commit_resets = true;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 10000 && updater_state() == UPDATE_STATE_SUBSIDE_COMMITTING; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    led_seen = 0;
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS - 1000);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_VERSION);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);
    wait_ms(1500);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_FAILED);
    wait_ms(SVAL_UPDATE_RED_MS);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_NONE);
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x20, 0x20);

    // ABORT while relaying: the other half is told, the link resumes.
    fresh(len);
    make_slave_update(len);
    presence_up();
    req_hand = UPDATE_HAND_LEFT;
    CHECK(to_verified(CLIENT_A));
    memcpy(shadow, update_host_flash + SVAL_UPDATE_BASE, len);
    master_slot = shadow;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 3000 && update_split_slave_state() != UPDATE_STATE_RECEIVING; i++)
        pass();
    for (int i = 0; i < 20; i++)
        pass();
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_RECEIVING);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    relay_get();
    CHECK_EQ(ri.phase, RELAY_ABORTING); // the ABORT goes out on the next pass
    pass();
    relay_get();
    CHECK_EQ(ri.phase, RELAY_IDLE);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    CHECK(!link_paused);
    CHECK(!update_relay_busy());
    CHECK(!updater_active());

    // COMMIT in VERIFIED with the link down: refused, still VERIFIED.
    fresh(len);
    make_slave_update(len);
    presence_up();
    req_hand = UPDATE_HAND_LEFT;
    CHECK(to_verified(CLIENT_A));
    memcpy(shadow, update_host_flash + SVAL_UPDATE_BASE, len);
    master_slot = shadow;
    link_up     = false;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    link_up = true;

    // The link drops while relaying: ERROR UNAVAILABLE, the link resumed;
    // ABORT from anyone clears it.
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 3000 && update_split_slave_state() != UPDATE_STATE_RECEIVING; i++)
        pass();
    link_up = false;
    for (int i = 0; i < 3000 && updater_state() == UPDATE_STATE_RELAYING; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_UNAVAILABLE);
    CHECK(!link_paused);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    link_up = true;

    // The host goes away in RELAYED: TIMEOUT after 30 s, and the other half's
    // session is ABORTed (D23: before its copy).
    fresh(len);
    make_slave_update(len);
    presence_up();
    CHECK(session_to_relayed(len));
    for (int i = 0; i < SVAL_UPDATE_SESSION_TIMEOUT_MS + 100; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_TIMEOUT);
    CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(slave_commit_runs, 0);
    CHECK(!link_paused);
    CHECK_EQ(commit_runs, 0);

    // A relay failure the other half reports (its manifest check refuses).
    fresh(len);
    make_slave_update(len);
    presence_up();
    slave_dev.security_epoch = 9; // the other half's floor is higher than this one's
    CHECK(!session_to_relayed(len));
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_EPOCH);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], RELAY_FAILED);
    CHECK_EQ(rsp[21], UPDATE_EPOCH);
    CHECK(!link_paused);
    CHECK_EQ(lock_errors, 0);
}

// Presence itself (update_split.c's wrappers): the other half fills the
// response, this half compares.
static void test_relay_presence(void) {
    fresh(0x2000);
    uint8_t out[32];
    memset(out, 0xEE, sizeof(out));
    sim_slave = true;
    update_split_presence_fill(out, 16); // too short: untouched
    for (int i = 0; i < 32; i++)
        CHECK_EQ(out[i], 0xEE);
    update_split_presence_fill(out, 17);
    sim_slave = false;
    for (int i = 17; i < 32; i++)
        CHECK_EQ(out[i], 0xEE);
    update_presence_t p;
    memcpy(&p, out, sizeof(p));
    CHECK_EQ(p.hand, UPDATE_HAND_LEFT);
    CHECK_EQ(p.flags & UPDATE_PRESENCE_FLAG_UPDATING, 0);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_NONE);
    update_split_presence_result(true, out, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    update_presence_t seen;
    CHECK_EQ(update_split_other(&seen), UPDATE_PRESENCE_MATCH);
    CHECK_EQ(seen.pointing_id, UPDATE_POINTING_TRACKPOINT);
    CHECK(!update_split_mismatch());
    uint8_t zeros[17] = {0};
    update_split_presence_result(true, zeros, 17);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_INVALID);
    CHECK(update_split_mismatch());
    update_split_presence_result(false, NULL, 0);
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_NONE);
    // a half in a session says so
    make_slave_update(0x2000);
    master_slot = img;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    relay_until(RELAY_PAGES, 60000);
    sim_slave = true;
    update_split_presence_fill(out, 17);
    sim_slave = false;
    memcpy(&p, out, sizeof(p));
    CHECK_EQ(p.flags & UPDATE_PRESENCE_FLAG_UPDATING, UPDATE_PRESENCE_FLAG_UPDATING);
    update_relay_abort();
}

#include "test_relay_review.c"

static void test_relay(void) {
    int before_failures = failures;
    test_relay_presence();
    test_relay_happy(0x3000);
    test_relay_happy(0x13000);
    test_relay_slave_refusals();
    test_relay_slave_mismatch();
    test_relay_pages();
    test_relay_mailbox();
    test_relay_slave_timeout();
    test_relay_link_loss();
    test_relay_faults();
    test_relay_cuts();
    test_relay_session();
    test_relay_review();
    printf("relay tests: %s\n", failures == before_failures ? "pass" : "FAIL");
}

#ifdef SVAL_TEST_REAL_COMMIT
// The other half's commit with the real update_commit.c, cut after every ROM
// operation of the relay and of the commit: (a) during staging only its slot
// changes and its old image boots; (b)-(d) during its commit as M1.
static int relay_commit_e2e(uint32_t len, int cut_k, bool tear) {
    relay_fresh(len);
    make_commit_image(len); // boot2 differs from the running one's
    sval_update_manifest_t *m = blob_manifest();
    m->hand                   = UPDATE_HAND_LEFT;
    m->pointing_id            = UPDATE_POINTING_TRACKPOINT;
    master_slot               = img;
    for (uint32_t i = 0; i < SVAL_UPDATE_MAX_IMAGE; i++)
        old_fw[i] = (uint8_t)(rnd() | 0x01);
    memcpy(old_fw, BOOT2_ROM, 256);
    memcpy(update_host_flash, old_fw, sizeof(old_fw));
    hw_reset();
    plant_ram0();
    commit_allow_hi = round_block(len);
    update_commit_test_halt(UPDATE_HALT_NONE, false);
    snap();
    cut_at   = cut_k;
    cut_tear = tear;
    int land = setjmp(commit_env);
    if (land == 0) {
        land = setjmp(cut_env);
        if (land == 0) {
            if (!update_relay_start(blob, len, body_crc())) return LAND_NONE;
            if (relay_until(RELAY_VERIFIED, 60000) != RELAY_VERIFIED) return LAND_NONE;
            if (!update_relay_commit()) return LAND_NONE;
            relay_until(RELAY_DONE, 20000);
            return LAND_NONE;
        }
        land = LAND_CUT;
    }
    sim_slave  = false;
    irq_off    = false;
    xip_on     = true;
    connected  = false;
    mock_wd_on = false;
    lock_depth = 0;
    return land;
}

// (b)-(d) for the other half's commit, where the relay has also filled its slot.
static void relay_invariants(uint32_t len) {
    uint32_t fw_end = round_block(len), slot = SVAL_UPDATE_BASE, slot_end = SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE;
    CHECK(memcmp(before + fw_end, update_host_flash + fw_end, slot - fw_end) == 0);
    CHECK(memcmp(before + slot_end, update_host_flash + slot_end, UPDATE_HOST_FLASH_BYTES - slot_end) == 0);
    CHECK(memcmp(update_host_flash + slot, img, len) == 0); // the relayed image, untouched by the commit
    CHECK(!page0_valid() || image_in_place(len));
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
}

static void test_relay_real_commit(void) {
    const int      f0  = failures;
    const uint32_t len = 0x3000;
    CHECK_EQ(relay_commit_e2e(len, 0, false), LAND_RESET);
    CHECK(image_in_place(len) && page0_valid());
    relay_invariants(len);
    CHECK_EQ(latches, 1);
    CHECK(reset_ok);
    CHECK(link_paused); // this half was holding the pause when the other one reset
    int total   = flash_ops;
    int staging = 0, commit = 0;
    for (int tear = 0; tear < 2; tear++) {
        for (int k = 1; k <= total; k++) {
            CHECK_EQ(relay_commit_e2e(len, k, tear), LAND_CUT);
            if (latches == 0) {
                staging++;
                CHECK(only_slot_changed());
                CHECK(memcmp(update_host_flash, old_fw, SVAL_UPDATE_MAX_IMAGE) == 0 && page0_valid());
            } else {
                commit++;
                relay_invariants(len);
                CHECK(!page0_valid() || (k == total && !tear));
            }
            CHECK_EQ(outside_ops, 0);
            CHECK_EQ(sequence_errors, 0);
            if (failures != f0) {
                fprintf(stderr, "  (relay + slave commit, cut after op %d of %d, %s)\n", k, total, tear ? "torn" : "clean");
                return;
            }
        }
    }
    printf("relay + real slave commit 0x%x: %d ROM ops; cuts: %d in staging (old image boots), %d in the commit\n", (unsigned)len, total, staging, commit);
}
#endif
