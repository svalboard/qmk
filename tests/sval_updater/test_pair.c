// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for updater protocol 4 (D41-D45): one approval for both halves
// (PAIR_MANIFEST, the pair ARM, the held approval), SETTINGS_RESET, and the
// no-chord dev build. Each case has its ID from PROTOCOL4_CASES.md, the list
// Keybard's mock board (tests/updater/protocol4-cases.md there) runs too.
// Included by test_updater.c after test_relay.c, whose helpers it uses.

static uint8_t  self_blob[UPDATE_MANIFEST_BYTES]; // this half's manifest, as PAIR_MANIFEST sends it
static uint8_t  arm_rsp[T_VALUE];                 // the last ARM's reply
static uint32_t pair_nonce;

// This half's image (right, fw 2002) and the other half's (left, TrackPoint,
// fw 2002): the same bytes, so the same SHA-512, with different manifests.
static void make_pair(uint32_t len) {
    make_update(len);
    blob_manifest()->fw_version = 2002;
    memcpy(self_blob, blob, sizeof(self_blob));
    make_slave_update(len);
}

static void pair_fresh(uint32_t len) {
    fresh(len);
    make_pair(len);
    presence_up();
    auto_shadow = true; // the relay's pause takes the staged image as this half's slot
}

static update_status_t load_pair(uint32_t client, const uint8_t *m) {
    uint8_t hand = req_hand;
    req_hand     = UPDATE_HAND_RIGHT; // [hand] is this half's own
    for (uint32_t off = 0; off < UPDATE_MANIFEST_BYTES; off += T_MPIECE) {
        uint8_t n = UPDATE_MANIFEST_BYTES - off < T_MPIECE ? (uint8_t)(UPDATE_MANIFEST_BYTES - off) : T_MPIECE;
        uint8_t a[2 + T_MPIECE];
        a[0] = (uint8_t)off;
        a[1] = n;
        memcpy(a + 2, m + off, n);
        update_status_t st = send(client, UPDATE_OP_PAIR_MANIFEST, a, 2 + n);
        if (st != UPDATE_OK) {
            req_hand = hand;
            return st;
        }
        CHECK_EQ(rsp[1], off + n);
    }
    req_hand = hand;
    return UPDATE_OK;
}

static update_status_t arm(uint32_t client, uint8_t mode) {
    update_status_t st = send(client, UPDATE_OP_ARM, &mode, 1);
    memcpy(arm_rsp, rsp, sizeof(arm_rsp));
    if (st == UPDATE_OK) session_nonce = get32le(rsp + 1);
    return st;
}

static uint8_t status_flags2(uint32_t client) {
    CHECK_EQ(send(client, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    return rsp[23];
}

static bool hash4_is(const uint8_t *at, const uint8_t *m) {
    uint8_t h[UPDATE_SHA512_BYTES];
    crypto_sha512(h, m, UPDATE_MANIFEST_BYTES);
    return memcmp(at, h, 4) == 0;
}

// The other half's manifest, the pair manifest and the pair ARM, from IDLE.
static bool pair_armed(uint32_t client) {
    req_hand = UPDATE_HAND_LEFT;
    if (load_manifest(client) != UPDATE_OK) return false;
    if (load_pair(client, self_blob) != UPDATE_OK) return false;
    if (arm(client, UPDATE_ARM_MODE_PAIR) != UPDATE_OK) return false;
    pair_nonce = session_nonce;
    return updater_state() == UPDATE_STATE_CONFIRM_WAIT;
}

// From CONFIRM_WAIT (chord made or not needed) through the relay and the
// other half's commit, back to IDLE.
static bool pair_rest(uint32_t client) {
    req_hand = UPDATE_HAND_LEFT;
    if (send_nonce(client, UPDATE_OP_BEGIN, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    if (send_image(client, 0, img_len) != UPDATE_OK) return false;
    if (send_nonce(client, UPDATE_OP_END, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    if (send_commit(client, body_crc()) != UPDATE_ACCEPTED) return false;
    for (int i = 0; i < 60000 && updater_state() == UPDATE_STATE_RELAYING; i++) {
        pass();
        if (i % 200 == 0) send(client, UPDATE_OP_RELAY, NULL, 0);
    }
    if (updater_state() != UPDATE_STATE_RELAYED) return false;
    slave_commit_resets = true;
    if (send_commit(client, body_crc()) != UPDATE_ACCEPTED) return false;
    for (int i = 0; i < 10000 && updater_state() == UPDATE_STATE_SUBSIDE_COMMITTING; i++)
        pass();
    return updater_state() == UPDATE_STATE_IDLE && slave_commit_runs == 1;
}

// This half's own install, from IDLE to its ARM, with manifest m.
static update_status_t self_armed(uint32_t client, const uint8_t *m) {
    master_slot  = NULL; // this half stages in the die's slot again
    commit_avail = true;
    memcpy(blob, m, sizeof(blob));
    req_hand           = UPDATE_HAND_RIGHT;
    update_status_t st = load_manifest(client);
    if (st != UPDATE_OK) return st;
    return arm(client, UPDATE_ARM_MODE_SINGLE);
}

// ---- P4-01, P4-02: INFO and the single ARM -------------------------------------------------

static void test_p4_single(void) {
    // P4-01: protocol 4; not a no-chord build
    fresh(0x2000);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[2], 4);
    CHECK_EQ(rsp[21] & 0x80, 0);
    // P4-02: a single ARM is protocol 3's: no flags, no pair hash, blue, one chord
    make_update(0x2000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(arm(CLIENT_A, UPDATE_ARM_MODE_SINGLE), UPDATE_OK);
    CHECK(hash4_is(arm_rsp + 5, blob));
    for (int i = 9; i <= 13; i++)
        CHECK_EQ(arm_rsp[i], 0);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_NOT_CONFIRMED);
    chord();
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    // an ARM with no mode byte at all (a protocol 3 host's zeros) is the same
    fresh(0x2000);
    make_update(0x2000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[13], 0);
    // mode 2 and up: INVALID, nothing changes
    fresh(0x2000);
    make_update(0x2000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(arm(CLIENT_A, 2), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // P4-04: a pair ARM for this half's own session: INVALID
    CHECK_EQ(arm(CLIENT_A, UPDATE_ARM_MODE_PAIR), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
}

// ---- P4-03, P4-04: PAIR_MANIFEST and the pair ARM's refusals -------------------------------

static void test_p4_pair_refusals(void) {
    pair_fresh(0x3000);
    uint8_t piece[2 + T_MPIECE] = {0, T_MPIECE};
    memcpy(piece + 2, self_blob, T_MPIECE);
    // P4-03: no session: INVALID
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    // beside this half's own session: INVALID
    make_update(0x3000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_INVALID);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, 0), UPDATE_OK);
    // a session for the other hand
    make_pair(0x3000);
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    // [hand] = the other hand: INVALID
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_INVALID);
    req_hand = UPDATE_HAND_RIGHT;
    // another client: OTHER_CLIENT
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_OTHER_CLIENT);
    CHECK_EQ(rsp[1], 0);
    // out of order, an empty or too long piece, past the end: refused
    uint8_t late[2 + 4] = {T_MPIECE, 4};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, late, sizeof(late)), UPDATE_OUT_OF_ORDER);
    uint8_t empty[2] = {0, 0};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, empty, sizeof(empty)), UPDATE_INVALID);
    uint8_t big[2 + T_MPIECE] = {0, T_MPIECE + 1};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, big, sizeof(big)), UPDATE_INVALID);
    uint8_t past[2 + 4] = {106, 4};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, past, sizeof(past)), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // P4-04: a pair ARM with the pair manifest incomplete: INVALID, still loading
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_OK);
    CHECK_EQ(rsp[1], T_MPIECE);
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(arm(CLIENT_A, UPDATE_ARM_MODE_PAIR), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // offset 0 starts the pair manifest again; then the whole of it, and the ARM
    CHECK_EQ(load_pair(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm(CLIENT_A, UPDATE_ARM_MODE_PAIR), UPDATE_OK);
    // P4-03: after ARM: BUSY, nothing changes
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_PAIR_MANIFEST, piece, sizeof(piece)), UPDATE_BUSY);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);

    // P4-05: a pair ARM refused for the pair manifest's floors: ERROR latched
    // with that status before any chord is asked for
    pair_fresh(0x3000);
    self_dev.security_epoch                               = 2;
    blob_manifest()->security_epoch                       = 3; // the other half's image is fine
    ((sval_update_manifest_t *)self_blob)->security_epoch = 0;
    CHECK(!pair_armed(CLIENT_A));
    CHECK_EQ(arm_rsp[0], UPDATE_EPOCH);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    // and for its structure (BAD_IMAGE)
    pair_fresh(0x3000);
    ((sval_update_manifest_t *)self_blob)->image_len = 0x150;
    CHECK(!pair_armed(CLIENT_A));
    CHECK_EQ(arm_rsp[0], UPDATE_BAD_IMAGE);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    // P4-06: refused for the other manifest, as a single ARM
    pair_fresh(0x3000);
    blob_manifest()->image_len = 0x150;
    CHECK(!pair_armed(CLIENT_A));
    CHECK_EQ(arm_rsp[0], UPDATE_BAD_IMAGE);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

// ---- P4-07, P4-08: one chord for both halves -----------------------------------------------

static void test_p4_pair_one_chord(void) {
    const uint32_t len = 0x5000;
    pair_fresh(len);
    // P4-07: the reply carries both codes and PAIR; blue until the chord
    CHECK(pair_armed(CLIENT_A));
    CHECK(hash4_is(arm_rsp + 5, blob));
    CHECK(hash4_is(arm_rsp + 9, self_blob));
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PAIR);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
    CHECK_EQ(status_flags2(CLIENT_A), 0); // nothing held before the chord
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_NOT_CONFIRMED);
    led_seen = 0;
    chord(); // the one chord
    CHECK(led_seen & (1 << UPDATE_LED_CHORD));
    CHECK_EQ(status_flags2(CLIENT_A) & 0x01, 0x01); // held from the chord on
    CHECK(pair_rest(CLIENT_A));
    // P4-08: after the relay is DONE the approval waits for this half
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(status_flags2(CLIENT_A), 0x01);
    wait_ms(10000); // the host checks the other half meanwhile
    CHECK_EQ(status_flags2(CLIENT_A), 0x01);
    // this half: pre-approved by the pair, no blue and no white flash
    led_seen = 0;
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR);
    CHECK(hash4_is(arm_rsp + 5, self_blob));
    for (int i = 9; i <= 12; i++)
        CHECK_EQ(arm_rsp[i], 0);
    CHECK_EQ(status_flags2(CLIENT_A), 0x02); // used up; this session pre-approved
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x01, 0x01); // the chord counts as made
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(led_seen & ((1 << UPDATE_LED_CONFIRM) | (1 << UPDATE_LED_CHORD)), 0);
    master_commit_resets = true;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 300 && master_reboots == 0; i++)
        pass();
    CHECK_EQ(master_reboots, 1);
    CHECK_EQ(commit_runs, 1);
    for (int i = 0; i < 1200; i++)
        pass();
    CHECK_EQ(update_split_presence(), UPDATE_PRESENCE_MATCH); // both on 2002

    // The used-up approval approves nothing more: the same manifest again needs the chord.
    fresh(len);
    make_pair(len);
    presence_up();
    auto_shadow = true;
    CHECK(pair_armed(CLIENT_A));
    chord();
    CHECK(pair_rest(CLIENT_A));
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
}

// ---- P4-09 ... P4-16: what clears and moves the held approval ------------------------------

// A pair session with its chord, up to the approval waiting in IDLE.
static bool held_ready(uint32_t len) {
    pair_fresh(len);
    if (!pair_armed(CLIENT_A)) return false;
    chord();
    if (!pair_rest(CLIENT_A)) return false;
    return (status_flags2(CLIENT_A) & 0x01) == 0x01;
}

static void test_p4_held_rules(void) {
    const uint32_t len = 0x3000;
    // P4-09: a self ARM with another manifest needs the chord, and uses the approval up
    CHECK(held_ready(len));
    uint8_t other_self[UPDATE_MANIFEST_BYTES];
    memcpy(other_self, self_blob, sizeof(other_self));
    ((sval_update_manifest_t *)other_self)->fw_version = 2003;
    CHECK_EQ(self_armed(CLIENT_A, other_self), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_NOT_CONFIRMED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0); // gone

    // P4-10: a MANIFEST from another client clears it
    CHECK(held_ready(len));
    memcpy(blob, self_blob, sizeof(blob));
    master_slot = NULL;
    req_hand    = UPDATE_HAND_RIGHT;
    CHECK_EQ(load_manifest(CLIENT_B), UPDATE_OK);
    CHECK_EQ(status_flags2(CLIENT_B), 0);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);
    // ... and so does a MANIFEST for the other hand, from the bound client
    CHECK(held_ready(len));
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(status_flags2(CLIENT_A), 0);

    // P4-11: REBIND in IDLE with the pair nonce moves it to the new client
    CHECK(held_ready(len));
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, pair_nonce ^ 1), UPDATE_INVALID); // wrong nonce, no session
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, pair_nonce), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(status_flags2(CLIENT_B), 0x01);
    CHECK_EQ(self_armed(CLIENT_B, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR);
    // the old client's manifest, after the REBIND, would have cleared it
    CHECK(held_ready(len));
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, pair_nonce), UPDATE_OK);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);
    // a REBIND while this half's manifest loads moves that load too
    CHECK(held_ready(len));
    memcpy(blob, self_blob, sizeof(blob));
    master_slot                 = NULL;
    req_hand                    = UPDATE_HAND_RIGHT;
    uint8_t first[2 + T_MPIECE] = {0, T_MPIECE};
    memcpy(first + 2, blob, T_MPIECE);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, first, sizeof(first)), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_REBIND, pair_nonce), UPDATE_OK);
    CHECK_EQ(load_manifest(CLIENT_C), UPDATE_OK);
    CHECK_EQ(arm(CLIENT_C, UPDATE_ARM_MODE_SINGLE), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR);

    // P4-12: ABORT in IDLE clears it
    CHECK(held_ready(len));
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);
    // ... and ABORT of the pair session itself, before the relay
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    chord();
    CHECK_EQ(status_flags2(CLIENT_A) & 0x01, 0x01);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    CHECK_EQ(status_flags2(CLIENT_A), 0);

    // P4-13: an error in the pair session clears it (the chord window passing, a relay failure)
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    wait_ms(SVAL_UPDATE_CONFIRM_WINDOW_MS + 100);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    chord();
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 50; i++)
        pass();
    link_dead = true; // the split cable pulled mid-relay
    for (int i = 0; i < 10000 && updater_state() == UPDATE_STATE_RELAYING; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    link_dead = false;
    // ... and an error in this half's own session after it (the image hash)
    CHECK(held_ready(len));
    ((sval_update_manifest_t *)self_blob)->sha512[0] ^= 1; // matches nothing: approval used up, chord asked
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);

    // P4-14: the timeout: still held 90 s after DONE, gone by 180 s
    CHECK(held_ready(len));
    wait_ms(90000);
    CHECK_EQ(status_flags2(CLIENT_A), 0x01);
    wait_ms(SVAL_UPDATE_PAIR_HOLD_MS - 90000 + 10);
    CHECK_EQ(status_flags2(CLIENT_A), 0);
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], 0);

    // P4-15: a reset of this half clears it (RAM only)
    CHECK(held_ready(len));
    updater_host_reset();
    CHECK_EQ(status_flags2(CLIENT_A), 0);

    // P4-16: REBIND during the other half's commit moves the session and the approval
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    chord();
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 60000 && updater_state() == UPDATE_STATE_RELAYING; i++)
        pass();
    slave_commit_resets = true;
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    CHECK_EQ(updater_state(), UPDATE_STATE_SUBSIDE_COMMITTING);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, first, sizeof(first)), UPDATE_BUSY); // still nothing else
    for (int i = 0; i < 10000 && updater_state() == UPDATE_STATE_SUBSIDE_COMMITTING; i++)
        pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(self_armed(CLIENT_B, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR);
}

// ---- P4-17 ... P4-22: SETTINGS_RESET --------------------------------------------------------

static update_status_t settings_reset(uint32_t client, uint8_t scope) {
    uint8_t a[5] = {'R', 'S', 'E', 'T', scope};
    return send(client, UPDATE_OP_SETTINGS_RESET, a, sizeof(a));
}

static void test_p4_settings_reset(void) {
    // P4-17: scope 1 in IDLE: the other half first, then this one; both reset within 500 ms
    fresh(0x2000);
    presence_up();
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_OK);
    CHECK_EQ(rsp[1], 0x03);
    CHECK_EQ(slave_invalidations, 1);
    CHECK_EQ(master_invalidations, 1);
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_BUSY); // once
    uint64_t t0 = sim_us;
    for (int i = 0; i < 600 && (master_reboots == 0 || slave_reboots == 0); i++)
        pass();
    CHECK_EQ(master_reboots, 1);
    CHECK_EQ(slave_reboots, 1);
    CHECK(sim_us - t0 <= 500000);
    CHECK_EQ(master_soft_resets, 1);
    CHECK_EQ(slave_soft_resets, 1);
    // P4-21: scope 0: this half only
    fresh(0x2000);
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset(CLIENT_A, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], 0x01);
    CHECK_EQ(slave_invalidations, 0);
    for (int i = 0; i < 600 && master_reboots == 0; i++)
        pass();
    CHECK_EQ(master_reboots, 1);
    CHECK_EQ(slave_reboots, 0);
    // P4-20: INVALID: a bad magic, scope 2, the other hand, unwrapped
    fresh(0x2000);
    presence_up();
    uint8_t bad[5] = {'R', 'S', 'E', 'X', 1};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_SETTINGS_RESET, bad, sizeof(bad)), UPDATE_INVALID);
    CHECK_EQ(settings_reset(CLIENT_A, 2), UPDATE_INVALID);
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_INVALID);
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset(0, 1), UPDATE_INVALID);
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    // P4-18: BUSY in a session, with a held approval, while a relay runs
    make_update(0x2000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK(held_ready(0x3000));
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_BUSY);
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    // ... and BUSY from the other half while it holds a session of its own
    fresh(0x2000);
    presence_up();
    make_slave_update(0x2000);
    master_slot = img;
    CHECK(update_relay_start(blob, img_len, body_crc()));
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_BUSY); // the relay here
    update_relay_abort();
    for (int i = 0; i < 20; i++)
        pass();
    // P4-19: the other half silent: UNAVAILABLE, nothing changed anywhere
    fresh(0x2000);
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_UNAVAILABLE); // no presence
    presence_up();
    link_dead = true;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_UNAVAILABLE); // presence went stale, no answer
    link_dead = false;
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    for (int i = 0; i < 600; i++)
        pass();
    CHECK_EQ(master_reboots + slave_reboots, 0);
    // P4-22: the other half on protocol 3 does not know the op: UNAVAILABLE, nothing changed
    fresh(0x2000);
    presence_up();
    old_slave = true;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_UNAVAILABLE);
    old_slave = false;
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    // a busy DMA channel on the other half: new requests until it is free
    fresh(0x2000);
    presence_up();
    dma_busy_pct = 50;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_OK);
    dma_busy_pct = 0;
    CHECK_EQ(slave_invalidations, 1);
    // this half's record not written: FLASH_ERR, the other half already told
    fresh(0x2000);
    presence_up();
    master_invalidate_fails = true;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_FLASH_ERR);
    CHECK_EQ(rsp[1], 0x02);
    master_invalidate_fails = false;
    // the other half's record not written: FLASH_ERR, nothing done here
    fresh(0x2000);
    presence_up();
    slave_invalidate_fails = true;
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_FLASH_ERR);
    CHECK_EQ(rsp[1], 0);
    CHECK_EQ(master_invalidations, 0);
    slave_invalidate_fails = false;
    // the wire: a SETTINGS_RESET frame with a bad magic is INVALID on the other half
    uint8_t req[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
    uint8_t xs[4] = {'R', 'S', 'E', 'X'};
    CHECK_EQ(usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, 1, xs, 4), 0);
    uint8_t ok4[4] = {'R', 'S', 'E', 'T'};
    uint8_t n      = usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, 9, ok4, 4);
    CHECK_EQ(n, 8);
    CHECK_EQ(usplit_rsp_len(USPLIT_OP_SETTINGS_RESET), 6);
    req[3] = 'X'; // damaged: the CRC fails
    slave_rpc(req, n, out, 6);
    CHECK_EQ(out[0], UPDATE_INVALID);
}

// ---- P4-28: SETTINGS_RESET mode 1, a blank board (D44) ---------------------------------------

static update_status_t settings_reset_mode(uint32_t client, uint8_t scope, uint8_t mode) {
    uint8_t a[6] = {'R', 'S', 'E', 'T', scope, mode};
    return send(client, UPDATE_OP_SETTINGS_RESET, a, sizeof(a));
}

static void test_p4_blank_reset(void) {
    // both halves take mode 1, the other half first; both reset as for mode 0
    fresh(0x2000);
    presence_up();
    req_hand = UPDATE_HAND_RIGHT;
    CHECK_EQ(settings_reset_mode(CLIENT_A, 1, 1), UPDATE_OK);
    CHECK_EQ(rsp[1], 0x03);
    CHECK_EQ(slave_reset_mode, 1);
    CHECK_EQ(master_reset_mode, 1);
    for (int i = 0; i < 600 && (master_reboots == 0 || slave_reboots == 0); i++)
        pass();
    CHECK_EQ(master_reboots, 1);
    CHECK_EQ(slave_reboots, 1);
    // no mode byte (a protocol 4 host from before D44) and mode 0: defaults on both halves
    fresh(0x2000);
    presence_up();
    CHECK_EQ(settings_reset(CLIENT_A, 1), UPDATE_OK);
    CHECK_EQ(slave_reset_mode, 0);
    CHECK_EQ(master_reset_mode, 0);
    fresh(0x2000);
    presence_up();
    CHECK_EQ(settings_reset_mode(CLIENT_A, 1, 0), UPDATE_OK);
    CHECK_EQ(slave_reset_mode, 0);
    CHECK_EQ(master_reset_mode, 0);
    // scope 0, mode 1: this half only
    fresh(0x2000);
    CHECK_EQ(settings_reset_mode(CLIENT_A, 0, 1), UPDATE_OK);
    CHECK_EQ(rsp[1], 0x01);
    CHECK_EQ(master_reset_mode, 1);
    CHECK_EQ(slave_invalidations, 0);
    // mode 2 and up: INVALID, nothing changed
    fresh(0x2000);
    presence_up();
    for (unsigned m = 2; m < 256; m += 37)
        CHECK_EQ(settings_reset_mode(CLIENT_A, 1, (uint8_t)m), UPDATE_INVALID);
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    // the other half on a protocol 4 build from before D44 (no mode byte there):
    // UNAVAILABLE for mode 1, nothing changed on either half; mode 0 still works
    fresh(0x2000);
    presence_up();
    slave_pre_blank = true;
    CHECK_EQ(settings_reset_mode(CLIENT_A, 1, 1), UPDATE_UNAVAILABLE);
    CHECK_EQ(master_invalidations + slave_invalidations, 0);
    for (int i = 0; i < 600; i++)
        pass();
    CHECK_EQ(master_reboots + slave_reboots, 0);
    CHECK_EQ(settings_reset_mode(CLIENT_A, 1, 0), UPDATE_OK);
    CHECK_EQ(slave_reset_mode, 0);
    slave_pre_blank = false;
    // the wire: 'RSET' and an optional mode 0 or 1; anything else is refused when built
    uint8_t req[USPLIT_MSG_MAX];
    uint8_t m1[5] = {'R', 'S', 'E', 'T', 1}, m2[5] = {'R', 'S', 'E', 'T', 2}, m6[6] = {'R', 'S', 'E', 'T', 1, 0};
    CHECK_EQ(usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, 3, m1, 5), 9);
    CHECK_EQ(usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, 3, m2, 5), 0);
    CHECK_EQ(usplit_req_build(req, USPLIT_OP_SETTINGS_RESET, 3, m6, 6), 0);
}

// ---- P4-30: a release build reads the image's own build-info record (D45) --------------------

// Put a build-info record ("SVBI", info_ver 3, flags) at img[at] and seal the
// manifest over the new bytes; the manifest's flags stay as they are (no DIAGNOSTIC).
static void plant_record(uint32_t at, uint8_t flags) {
    const uint8_t rec[6] = {'S', 'V', 'B', 'I', UPDATE_BUILD_INFO_VERSION, flags};
    memcpy(img + at, rec, sizeof(rec));
    crypto_sha512(blob_manifest()->sha512, img, img_len);
}

static uint8_t last_error(uint32_t client) {
    CHECK_EQ(send(client, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    return rsp[12];
}

// This half's install from IDLE to the end of the image check: VERIFIED or ERROR.
static update_state_t self_install_to_check(void) {
    req_hand = UPDATE_HAND_RIGHT;
    if (!to_receiving(CLIENT_A)) return UPDATE_STATE_IDLE;
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    return updater_state();
}

static void test_p4_image_build_check(void) {
    const uint32_t len = 0x3000;
    // a release board, this half: a no-chord image is refused after its hash
    // checks out, before anything is committed, though its manifest has no DIAGNOSTIC
    struct {
        uint8_t flags;
        bool    release;
        bool    refused;
    } cases[] = {
        {UPDATE_BUILD_NO_CHORD, true, true},   {UPDATE_BUILD_DEV_KEEP, true, true}, {UPDATE_BUILD_NO_CHORD | UPDATE_BUILD_TEST_HOOKS, true, true}, {0, true, false}, // a chord build
        {UPDATE_BUILD_RELEASE, true, false},                                                                                                                         // a release build
        {UPDATE_BUILD_NO_CHORD, false, false},                                                                                                                       // a dev board takes a dev image
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh(len);
        self_dev.release_build = cases[i].release;
        plant_record(0x1235, cases[i].flags); // anywhere, any alignment
        CHECK_EQ(blob_manifest()->flags & UPDATE_FLAG_DIAGNOSTIC, 0);
        update_state_t st = self_install_to_check();
        CHECK_EQ(st, cases[i].refused ? UPDATE_STATE_ERROR : UPDATE_STATE_VERIFIED);
        if (cases[i].refused) {
            CHECK_EQ(last_error(CLIENT_A), UPDATE_BAD_IMAGE);
            CHECK_EQ(commit_runs, 0);
        }
    }
    // no record at all (Vial, vLaunch2): taken
    fresh(len);
    self_dev.release_build = true;
    CHECK_EQ(self_install_to_check(), UPDATE_STATE_VERIFIED);
    // a record split across two verify slices (2048 bytes) and two 256-byte reads
    fresh(len);
    self_dev.release_build = true;
    plant_record(2048 - 3, UPDATE_BUILD_NO_CHORD);
    CHECK_EQ(self_install_to_check(), UPDATE_STATE_ERROR);
    CHECK_EQ(last_error(CLIENT_A), UPDATE_BAD_IMAGE);
    // two records, one of them no-chord: refused
    fresh(len);
    self_dev.release_build = true;
    plant_record(0x0400, UPDATE_BUILD_RELEASE);
    plant_record(0x2000, UPDATE_BUILD_NO_CHORD);
    CHECK_EQ(self_install_to_check(), UPDATE_STATE_ERROR);
    // a record of another info_ver is not this check's (make_update.py's match too)
    fresh(len);
    self_dev.release_build = true;
    plant_record(0x0400, UPDATE_BUILD_NO_CHORD);
    img[0x0404] = 2;
    crypto_sha512(blob_manifest()->sha512, img, img_len);
    CHECK_EQ(self_install_to_check(), UPDATE_STATE_VERIFIED);

    // the other half's image, from a release half with USB: refused at this
    // half's verify, before the relay starts, and the pair approval is gone
    pair_fresh(0x5000);
    self_dev.release_build = true;
    plant_record(0x1235, UPDATE_BUILD_NO_CHORD);
    CHECK(pair_armed(CLIENT_A));
    chord();
    CHECK_EQ(status_flags2(CLIENT_A) & 0x01, 0x01);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(last_error(CLIENT_A), UPDATE_BAD_IMAGE);
    CHECK_EQ(status_flags2(CLIENT_A) & 0x01, 0); // the held approval went with the error
    CHECK_EQ(slave_commit_runs, 0);

    // a dev half with USB relaying to a release half: the other half checks
    // the image itself (D7) and the relay fails with its refusal
    pair_fresh(0x5000);
    slave_dev.release_build = true;
    plant_record(0x1235, UPDATE_BUILD_NO_CHORD);
    CHECK(pair_armed(CLIENT_A));
    chord();
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(send_commit(CLIENT_A, body_crc()), UPDATE_ACCEPTED);
    for (int i = 0; i < 60000 && updater_state() == UPDATE_STATE_RELAYING; i++) {
        pass();
        if (i % 200 == 0) send(CLIENT_A, UPDATE_OP_RELAY, NULL, 0);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(last_error(CLIENT_A), UPDATE_BAD_IMAGE);
    CHECK_EQ(slave_commit_runs, 0);

    // the scan by itself: byte at a time and all at once agree
    update_build_scan_t a, b;
    update_build_scan_init(&a);
    update_build_scan_init(&b);
    for (uint32_t i = 0; i < img_len; i++)
        update_build_scan(&a, img + i, 1);
    update_build_scan(&b, img, img_len);
    CHECK_EQ(a.records, 1);
    CHECK_EQ(b.records, 1);
    CHECK_EQ(a.flags, UPDATE_BUILD_NO_CHORD);
    CHECK_EQ(b.flags, a.flags);
}

static void test_pair(void) {
    int before_failures = failures;
    test_p4_single();
    test_p4_pair_refusals();
    test_p4_pair_one_chord();
    test_p4_held_rules();
    test_p4_settings_reset();
    test_p4_blank_reset();
    test_p4_image_build_check();
    printf("protocol 4 tests: %s\n", failures == before_failures ? "pass" : "FAIL");
}

// ---- P4-23: a no-chord dev build (SVAL_UPDATE_NO_CHORD, D43) ---------------------------------

static void test_pair_no_chord(void) {
    int before_failures = failures;
    // INFO bit7
    fresh(0x3000);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x80, 0x80);
    // a single ARM: pre-approved, no blue, BEGIN at once
    make_update(0x3000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    led_seen = 0;
    CHECK_EQ(arm(CLIENT_A, UPDATE_ARM_MODE_SINGLE), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_NO_CHORD);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(led_seen & ((1 << UPDATE_LED_CONFIRM) | (1 << UPDATE_LED_CHORD)), 0);
    CHECK_EQ(status_flags2(CLIENT_A), 0x02);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    // a pair ARM: the approval is held from the ARM on (the chord counts as made)
    const uint32_t len = 0x5000;
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PAIR | UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_NO_CHORD);
    CHECK_EQ(status_flags2(CLIENT_A), 0x03);
    CHECK(pair_rest(CLIENT_A));
    CHECK_EQ(status_flags2(CLIENT_A), 0x01);
    // this half: by the pair, and a no-chord build (bits 0, 1 and 2)
    CHECK_EQ(self_armed(CLIENT_A, self_blob), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_BY_PAIR | UPDATE_ARM_NO_CHORD);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    // another manifest for this half: still no chord on this build, but not by the pair
    pair_fresh(len);
    CHECK(pair_armed(CLIENT_A));
    CHECK(pair_rest(CLIENT_A));
    uint8_t other_self[UPDATE_MANIFEST_BYTES];
    memcpy(other_self, self_blob, sizeof(other_self));
    ((sval_update_manifest_t *)other_self)->fw_version = 2003;
    CHECK_EQ(self_armed(CLIENT_A, other_self), UPDATE_OK);
    CHECK_EQ(arm_rsp[13], UPDATE_ARM_PRE_APPROVED | UPDATE_ARM_NO_CHORD);
    printf("protocol 4, no-chord build: %s\n", failures == before_failures ? "pass" : "FAIL");
}
