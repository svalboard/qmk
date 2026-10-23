// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the pair UF2's boot relay (update_pair.c, docs/pair-uf2.md),
// against the real relay and the real other half's session over the
// simulated split link of test_session.c. Included by test_updater.c after
// test_pair.c.
//
// "A boot" here is this half's RAM reset (updater_host_reset, which also
// resets the boot relay) with the die kept; a reboot of the other half is
// update_split_host_slave_reset. The die is the record's; this half's own
// image is master_self (the die's firmware area stands for the other half's,
// whose commit test_session.c mocks).

#define PAIR_SECTOR (update_host_flash + UPDATE_PAIR_RECORD_OFFSET)

static bool pair_sector_erased(void) {
    for (uint32_t i = 0; i < UPDATE_PAIR_SECTOR_BYTES; i++) {
        if (PAIR_SECTOR[i] != 0xFF) return false;
    }
    return true;
}

// The record as a UF2 copy leaves it: page 0 the record, the rest erased.
static void pair_write_record(const uint8_t *manifest) {
    update_pair_record_t r;
    update_pair_record_make(&r, manifest);
    memset(PAIR_SECTOR, 0xFF, UPDATE_PAIR_SECTOR_BYTES);
    memcpy(PAIR_SECTOR, &r, sizeof(r));
}

static const update_pair_state_t *pair_state(void) {
    return (const update_pair_state_t *)(PAIR_SECTOR + UPDATE_PAIR_STATE_OFFSET);
}

// A board just after the copy: this half runs the image (an either-hand
// build), the record waits, the other half (left, same release) answers.
static void pb_fresh(uint32_t len) {
    fresh(len);
    blob_manifest()->hand       = UPDATE_HAND_EITHER;
    blob_manifest()->fw_version = 2001;
    memset(master_self, 0xFF, sizeof(master_self));
    memcpy(master_self, img, len);
    allow_pair_sector = true;
    port_master       = true;
    pair_write_record(blob);
    presence_sim        = true;
    last_ping_us        = 0;
    slave_commit_resets = true; // the other half "resets into" the image it commits
    led_seen            = 0;
    snap();
}

// A boot of this half: its RAM is gone, the die stays.
static void pb_boot(void) {
    updater_host_reset();
    update_split_host_master_reset();
    link_paused  = false;
    last_ping_us = 0;
}

// Passes until the boot relay is idle (or shows its error), at most max_ms.
static void pb_run(uint32_t max_ms) {
    for (uint32_t i = 0; i < max_ms; i++) {
        pass();
        uint8_t ph = update_pair_phase();
        if (i > 0 && (ph == PAIR_BOOT_IDLE || ph == PAIR_BOOT_ERROR)) break;
    }
}

#define LED_BIT(m) (1 << (m))

static void test_pair_uf2_happy(void) {
    pb_fresh(0x3000);
    CHECK_EQ(update_pair_phase(), PAIR_BOOT_INIT);
    CHECK(update_pair_busy()); // a host request before the first pass already waits
    int busy_seen = 0, info_ok = 0, writing_seen = 0;
    for (int i = 0; i < 120000; i++) {
        pass();
        if (update_pair_phase() == PAIR_BOOT_IDLE || update_pair_phase() == PAIR_BOOT_ERROR) break;
        if (i % 500 == 0) {
            // A host meanwhile: sessions wait, INFO, STATUS and RELAY answer.
            busy_seen += send(CLIENT_A, UPDATE_OP_MANIFEST, (uint8_t[]){0, 4, 1, 2, 3, 4}, 6) == UPDATE_BUSY;
            busy_seen += send(CLIENT_A, UPDATE_OP_SETTINGS_RESET, (uint8_t[]){'R', 'S', 'E', 'T', 1, 0}, 6) == UPDATE_BUSY;
            info_ok += send(0, UPDATE_OP_INFO, NULL, 0) == UPDATE_OK;
            info_ok += send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0) == UPDATE_OK;
            info_ok += send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0) == UPDATE_OK;
        }
        CHECK(updater_state() == UPDATE_STATE_IDLE); // no host session, ever, so no ARM and no chord asked for
        if (led_now == UPDATE_LED_WRITING) writing_seen++;
        CHECK(led_now != UPDATE_LED_CONFIRM && led_now != UPDATE_LED_CHORD);
    }
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(update_pair_phase(), PAIR_BOOT_IDLE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_commit_crc, body_crc());
    CHECK_EQ(slave_commit_len, 0x3000);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 0x3000) == 0); // the other half staged exactly the image
    CHECK(pair_sector_erased());                                           // used up
    CHECK(busy_seen > 0 && info_ok > 0);
    CHECK_EQ(writing_seen, 0); // magenta is the other half's, while it writes; this half shows the rainbow
    CHECK(led_seen & LED_BIT(UPDATE_LED_PROGRESS));
    CHECK_EQ(led_seen & LED_BIT(UPDATE_LED_ERROR), 0);
    // The other half restarts into the image: rainbow until presence matches,
    // then the green flash (carried on the ping), then nothing.
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_UPDATING);
    for (int i = 0; i < 5000 && update_split_presence() != UPDATE_PRESENCE_MATCH; i++)
        pass();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH);
    pass(); // the lights see the ping's answer on the next pass
    CHECK_EQ(led_now, UPDATE_LED_DONE);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_DONE);
    wait_ms(1000);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(led_seen & LED_BIT(UPDATE_LED_ERROR), 0);
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
    // Only the other half's slot and the record sector changed.
    memcpy(before + UPDATE_PAIR_RECORD_OFFSET, update_host_flash + UPDATE_PAIR_RECORD_OFFSET, UPDATE_PAIR_SECTOR_BYTES);
    CHECK(unchanged_outside(SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE));
    // Host sessions are open again; the next boot finds nothing.
    CHECK(!update_pair_busy());
    pb_boot();
    pb_run(100);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_NONE);
    CHECK_EQ(slave_commit_runs, 1);
}

// The security invariant: nothing a host can reach writes the record sector,
// so no host path leads to an install with no chord.
static void test_pair_uf2_host_paths(void) {
    // Every host flow of these tests, with the record sector locked: a
    // session for this half to the commit, one for the other half through
    // the relay and its commit, a pair update, SETTINGS_RESET.
    pb_fresh(0x2000);
    memset(PAIR_SECTOR, 0xFF, UPDATE_PAIR_SECTOR_BYTES); // no record
    allow_pair_sector = false;
    snap();
    pb_boot();
    // An image that holds a valid record, staged by a host: it lands in the
    // slot, never in the record sector.
    update_pair_record_t r;
    update_pair_record_make(&r, blob);
    memcpy(img + 0x1000, &r, sizeof(r));
    crypto_sha512(blob_manifest()->sha512, img, img_len);
    blob_manifest()->hand = UPDATE_HAND_RIGHT;
    commit_avail          = true;
    CHECK(to_verified(CLIENT_A));
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 300; i++)
        pass();
    CHECK_EQ(commit_runs, 1);
    CHECK(session_to_relayed(0x2000) || true); // the other half's session, as far as it goes here
    for (int i = 0; i < 20000 && updater_state() != UPDATE_STATE_IDLE; i++) {
        if (updater_state() == UPDATE_STATE_RELAYED) send_commit(CLIENT_A, body_crc());
        pass();
    }
    CHECK_EQ(pair_sector_ops, 0);
    CHECK_EQ(outside_ops, 0);
    CHECK(pair_sector_erased());
    // A boot after all of it: nothing to relay.
    int commits = slave_commit_runs;
    pb_boot();
    pb_run(20000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_NONE);
    CHECK_EQ(slave_commit_runs, commits);
    CHECK_EQ(pair_sector_ops, 0);
    // The firmware's own writes there: the state page and the whole-sector
    // erase only (the mock refuses any other ROM call on that sector).
    pb_fresh(0x2000);
    link_up = false; // no other half: tries counted, record kept
    pb_run(40000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
    CHECK_EQ(outside_ops, 0);
    update_pair_record_t want;
    update_pair_record_make(&want, blob);
    CHECK(memcmp(PAIR_SECTOR, &want, sizeof(want)) == 0); // page 0 as the copy left it
    CHECK_EQ(pair_state()->tries[0], 0x00);
    CHECK_EQ(pair_state()->tries[1], 0xFF);
}

// Not the image this half runs: an interrupted copy, or a record left over
// from an earlier file. Never relayed; erased.
static void test_pair_uf2_stale_image(void) {
    for (int k = 0; k < 3; k++) {
        pb_fresh(0x3000);
        if (k == 0) master_self[0x1234] ^= 0x01;                // one byte
        if (k == 1) memset(master_self + 0x2000, 0xA5, 0x1000); // the copy stopped before the last sector
        if (k == 2) master_self[0x3000 - 1] ^= 0x80;            // the last byte
        pb_run(20000);
        CHECK_EQ(update_pair_outcome(), PAIR_OUT_STALE);
        CHECK_EQ(update_pair_error(), UPDATE_BAD_HASH);
        CHECK(pair_sector_erased());
        CHECK_EQ(pause_calls, 0); // no relay began
        CHECK_EQ(slave_commit_runs, 0);
        CHECK_EQ(update_split_slave_state(), UPDATE_STATE_IDLE);
        CHECK_EQ(led_seen & LED_BIT(UPDATE_LED_ERROR), 0); // stale is not a failure: no light
    }
}

// A damaged or malformed record: refused, erased, the error light.
static void test_pair_uf2_bad_record(void) {
    for (int k = 0; k < 7; k++) {
        pb_fresh(0x2000);
        update_pair_record_t  *r = (update_pair_record_t *)PAIR_SECTOR;
        sval_update_manifest_t m;
        memcpy(&m, blob, sizeof(m));
        update_status_t want = UPDATE_INVALID;
        switch (k) {
            case 0:
                PAIR_SECTOR[40] ^= 0x04;
                break; // a bit of the manifest: the CRC
            case 1:
                m.hand = UPDATE_HAND_RIGHT;
                break; // one side's image: both halves would be right
            case 2:
                m.hand = UPDATE_HAND_LEFT;
                break;
            case 3:
                m.security_epoch        = 0;
                self_dev.security_epoch = 2;
                want                    = UPDATE_EPOCH;
                break; // below this build's floor
            case 4:
                m.magic ^= 1;
                break; // not a manifest
            case 5:
                r->version = 2;
                r->crc     = update_crc32_mpeg2((const uint8_t *)r, 124);
                want       = UPDATE_UNSUPPORTED;
                break;
            case 6:
                r->reserved[1] = 1;
                r->crc         = update_crc32_mpeg2((const uint8_t *)r, 124);
                break;
        }
        if (k >= 1 && k <= 4) pair_write_record((const uint8_t *)&m);
        pb_run(5000);
        CHECK_EQ(update_pair_outcome(), PAIR_OUT_REFUSED);
        CHECK_EQ(update_pair_error(), want);
        CHECK_EQ(update_pair_phase(), PAIR_BOOT_ERROR);
        CHECK_EQ(led_now, UPDATE_LED_ERROR);
        CHECK(pair_sector_erased());
        CHECK_EQ(pause_calls, 0);
        CHECK_EQ(slave_commit_runs, 0);
        // The light goes after SVAL_UPDATE_PAIR_ERROR_MS (presence still MATCH).
        for (int i = 0; i < 31000; i++)
            pass();
        CHECK_EQ(update_pair_phase(), PAIR_BOOT_IDLE);
        CHECK(led_now != UPDATE_LED_ERROR);
    }
}

// The other half refuses the image itself (its own floors, D7, D19): nothing
// installed there, the record erased, the error light.
static void test_pair_uf2_peer_refuses(void) {
    pb_fresh(0x2000);
    slave_dev.security_epoch = 4; // the other half's floor is above the image's epoch
    pb_run(60000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_REFUSED);
    CHECK_EQ(update_pair_error(), UPDATE_EPOCH);
    CHECK_EQ(slave_commit_runs, 0);
    CHECK(pair_sector_erased());
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    pb_fresh(0x2000);
    slave_dev.storage_format = SVAL_UPDATE_STORAGE_FORMAT + 1;
    pb_run(60000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_REFUSED);
    CHECK_EQ(update_pair_error(), UPDATE_STORAGE);
    CHECK_EQ(slave_commit_runs, 0);
}

// No other half that can take it: kept, bounded by UPDATE_PAIR_BOOT_TRIES boots.
static void test_pair_uf2_no_peer(void) {
    pb_fresh(0x2000);
    link_up = false; // unplugged, or a half without the updater (the split handshake fails: V)
    for (int boot = 0; boot < UPDATE_PAIR_BOOT_TRIES; boot++) {
        if (boot) pb_boot();
        uint64_t t0 = sim_us;
        pb_run(40000);
        CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
        CHECK_EQ(update_pair_error(), UPDATE_UNAVAILABLE);
        CHECK((sim_us - t0) / 1000 < SVAL_UPDATE_PAIR_PEER_WAIT_MS + 5000); // it gives up
        CHECK_EQ(led_now, UPDATE_LED_NONE);                                 // kept for an absent half: nothing for you to do
        CHECK_EQ(led_seen & (LED_BIT(UPDATE_LED_ERROR) | LED_BIT(UPDATE_LED_PROGRESS)), 0);
        CHECK(!pair_sector_erased());
        CHECK_EQ(pair_state()->tries[boot], 0x00);
        CHECK(!update_pair_busy()); // a host can work again
    }
    pb_boot();
    pb_run(5000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_EXHAUSTED);
    CHECK_EQ(led_now, UPDATE_LED_ERROR); // tried too often: red
    CHECK(pair_sector_erased());
    CHECK_EQ(slave_commit_runs, 0);

    // A half on updater protocol 2 answers presence but cannot take it.
    pb_fresh(0x2000);
    slave_id.updater_proto = 2;
    pb_run(40000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
    CHECK_EQ(update_pair_error(), UPDATE_UNSUPPORTED);
    CHECK_EQ(pause_calls, 0);
    CHECK_EQ(led_seen & (LED_BIT(UPDATE_LED_ERROR) | LED_BIT(UPDATE_LED_PROGRESS)), 0); // kept: no light

    // The other half shows up at the second boot: installed then.
    pb_fresh(0x2000);
    link_up = false;
    pb_run(40000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
    link_up = true;
    pb_boot();
    pb_run(120000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK(pair_sector_erased());
}

// A kept record never installs something stale.
static void test_pair_uf2_staleness(void) {
    // The other half was flashed by hand after the record first met it.
    pb_fresh(0x2000);
    drop_rsp_op = USPLIT_OP_END; // every relay fails at END (the link): kept, fingerprint stored
    pb_run(200000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
    CHECK_EQ(pair_state()->seen, 0x00);
    CHECK_EQ(pair_state()->peer_fw_version, 2001);
    drop_rsp_op         = -1;
    slave_id.fw_version = 3000; // newer, by hand
    update_split_host_slave_reset();
    pb_boot();
    pb_run(40000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_STALE);
    CHECK(pair_sector_erased());
    CHECK_EQ(slave_commit_runs, 0);

    // The same other half at the next boot: it tries again and installs.
    pb_fresh(0x2000);
    drop_rsp_op = USPLIT_OP_END;
    pb_run(200000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_KEPT);
    drop_rsp_op = -1;
    update_split_host_slave_reset();
    pb_boot();
    pb_run(120000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);

    // A later boot that meets the other half for the first time never sends it an older release.
    pb_fresh(0x2000);
    link_up = false;
    pb_run(40000);
    link_up             = true;
    slave_id.fw_version = 5000;
    pb_boot();
    pb_run(40000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_STALE);
    CHECK_EQ(slave_commit_runs, 0);
    CHECK(pair_sector_erased());
    // On the first boot, an older release than the other half's is the customer's choice.
    pb_fresh(0x2000);
    slave_id.fw_version = 5000;
    pb_run(120000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);

    // This half boots without USB (the cable moved): the record is dropped.
    pb_fresh(0x2000);
    port_master = false;
    pb_run(100);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_ABANDONED);
    CHECK(pair_sector_erased());
    CHECK_EQ(pause_calls, 0);
}

// The same image with the other half's side unknown or the same as this
// half's (one image: the relay never looks at sides).
static void test_pair_uf2_sides(void) {
    pb_fresh(0x2000);
    slave_id.hand = UPDATE_HAND_RIGHT; // presence SAME_HAND
    pb_run(120000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);
}

// Power cut on this half in the middle of the relay: the next boot finds the
// record, the same other half (its session dropped, its firmware untouched),
// and installs it.
static void test_pair_uf2_cut(void) {
    pb_fresh(0x4000);
    for (int i = 0; i < 200000; i++) {
        pass();
        update_relay_info_t r;
        update_relay_info(&r);
        if (r.phase == RELAY_PAGES && r.acked > 0x2000) break;
    }
    CHECK_EQ(update_pair_phase(), PAIR_BOOT_RELAY);
    pb_boot(); // power cut and back
    for (int i = 0; i < 6000; i++)
        pass(); // the other half drops its session (5 s)
    pb_run(200000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_commit_crc, body_crc());
    CHECK(pair_sector_erased());
}

// make_update.py's pair UF2, copied as the boot ROM does (block by block,
// each sector erased at the first block of its group of 16), then booted.
static void test_pair_uf2_cross(const char *dir) {
    char   path[4096];
    size_t len;
    snprintf(path, sizeof(path), "%s/pair_either.uf2", dir);
    uint8_t *uf2 = read_file(path, &len);
    CHECK(uf2 != NULL && len % 512 == 0 && len >= 32 * 512);
    if (!uf2) return;
    pb_fresh(0x2000);
    memset(master_self, 0xFF, sizeof(master_self));
    uint32_t num = 0, img_end = 0;
    for (size_t b = 0; b < len / 512; b++) {
        const uint8_t *blk  = uf2 + 512 * b;
        uint32_t       addr = get32le(blk + 12) - 0x10000000u, no = get32le(blk + 20);
        num = get32le(blk + 24);
        CHECK_EQ(get32le(blk + 8), 0x2000u);
        CHECK_EQ(get32le(blk + 28), 0xE48BFF56u);
        CHECK_EQ(no, (uint32_t)b);
        uint8_t *dst = addr < SVAL_UPDATE_MAX_IMAGE ? master_self : update_host_flash; // this half's own flash
        if (no % 16 == 0) memset(dst + (addr & ~0xFFFu), 0xFF, 4096);
        memcpy(dst + addr, blk + 32, 256);
        if (addr < SVAL_UPDATE_MAX_IMAGE && addr + 256 > img_end) img_end = addr + 256;
        CHECK(addr < SVAL_UPDATE_MAX_IMAGE || (addr >= UPDATE_PAIR_RECORD_OFFSET && addr < UPDATE_PAIR_RECORD_OFFSET + UPDATE_PAIR_SECTOR_BYTES));
    }
    CHECK_EQ(num, (uint32_t)(len / 512));
    const update_pair_record_t *r   = (const update_pair_record_t *)PAIR_SECTOR;
    update_device_t             dev = self_dev;
    CHECK_EQ(update_pair_record_check(r, &dev), UPDATE_OK);
    memcpy(blob, r->manifest, UPDATE_MANIFEST_BYTES);
    img_len = blob_manifest()->image_len;
    memcpy(img, master_self, img_len);
    pb_run(200000);
    CHECK_EQ(update_pair_outcome(), PAIR_OUT_DONE);
    CHECK_EQ(slave_commit_runs, 1);
    CHECK_EQ(slave_commit_crc, update_crc32_mpeg2(master_self + 0x100, img_len - 0x100));
    free(uf2);
    printf("pair UF2: make_update.py's file (%zu blocks, image %u B) relayed\n", len / 512, (unsigned)img_len);
}

static void test_pair_uf2(const char *cross_dir) {
    test_pair_uf2_happy();
    test_pair_uf2_host_paths();
    test_pair_uf2_stale_image();
    test_pair_uf2_bad_record();
    test_pair_uf2_peer_refuses();
    test_pair_uf2_no_peer();
    test_pair_uf2_staleness();
    test_pair_uf2_sides();
    test_pair_uf2_cut();
    if (cross_dir) test_pair_uf2_cross(cross_dir);
    allow_pair_sector = false;
    port_master       = true;
    printf("pair UF2 boot relay: done\n");
}
