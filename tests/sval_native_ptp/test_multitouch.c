// Host test: the native touchpad's contact path and split link
// (keyboards/svalboard/azoteq/multitouch.c) together with ptp.c.
//
//  - the half without USB: its news counter moves only on news, and stays
//    off its pointing report until a USB half asks for it ("hints ok");
//  - the snapshot reply fits the 32-byte RPC and decodes to the same frame;
//  - the USB half fetches the other pad at once on news, every BACKUP ms
//    while it has contacts or is not armed, every IDLE ms while it is quiet,
//    and only the heartbeat for a half with no snapshot or older firmware;
//  - nothing is fetched for a hand without a pad, or while an update runs
//    (and the other pad's contacts lift then);
//  - touchpad use counts as input activity; a chip reset is set up again,
//    never in a loop; the pad mouse is suppressed per hand;
//  - the dev command (owner) refuses everything but mouse and native.
#define SVAL_MT_HOST_TEST
#define SVAL_PTP_HOST_TEST
#define SVAL_TOUCH_DEV
#define SVAL_TOUCH_OWNER_DEFAULT 3
// The palm rejection threshold (VIA value 58 on the board): off unless a test sets it.
#include <stdint.h>
static uint8_t test_palm_at;
#define SVAL_PTP_PALM_AREA test_palm_at
#include "../../keyboards/svalboard/azoteq/multitouch.c" // brings mt_test_platform.h
#include "../../keyboards/svalboard/azoteq/ptp.c"

// keymaps/keymap_support.c on the board.
static int velocity_holds, velocity_on;
void       velocity_scroll_dev_hold(bool on) {
    velocity_holds++;
    velocity_on = on;
}

// ------------------------------------------------------------- helpers

static void raw_sample(uint8_t *raw, bool finger, uint16_t x, uint16_t y) {
    memset(raw, 0, 45);
    raw[0] = 8;
    if (finger) {
        raw[5]  = 1;      // one finger
        raw[10] = x >> 8; // slot 0: absolute X (big endian), Y, strength, area
        raw[11] = x & 0xFF;
        raw[12] = y >> 8;
        raw[13] = y & 0xFF;
        raw[15] = 40;
        raw[16] = 20;
    }
}

// The other half as the test plays it: the reply its firmware packs, built
// here independently (checked against pack_snapshot below).
static uint8_t  peer_seq, peer_epoch    = 1, peer_age;
static bool     peer_finger, peer_valid = true;
static uint16_t peer_x, peer_y;
static uint8_t  peer_last_req[3];

static void encode_peer(uint8_t *out) {
    memset(out, 0, 32);
    out[0] = 0xA7;
    out[1] = (peer_valid ? 1 : 0) | 2;
    out[2] = peer_age;
    out[4] = peer_finger ? 1 : 0;
    out[5] = peer_seq;
    out[6] = peer_epoch;
    if (peer_finger) {
        out[7]     = 1;
        uint32_t w = peer_x | (uint32_t)peer_y << 11 | 20u << 22;
        put32(out + 8, w);
    }
}
static void peer(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    assert(out_len == 32);
    memcpy(peer_last_req, in, in_len < 3 ? in_len : 3);
    if (in[0] != 0xA7) {
        memset(out, 0, 32);
        return;
    }
    encode_peer(out);
}
// Firmware without the op: Scan Lab answers an unknown op with zeros.
static void old_peer(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    (void)in_len;
    (void)in;
    memset(out, 0, out_len);
}

static uint8_t hint; // what the other half's pointing report carries
static bool    hint_on = true;

static void news(void) {
    hint = 0x80 | ((hint + 1) & 0x7F);
    peer_seq++;
}
static void set_peer(bool finger, uint16_t x, uint16_t y) {
    peer_finger = finger;
    peer_x      = x;
    peer_y      = y;
    news();
}

// One millisecond of the USB half's main loop.
static void tick(void) {
    clock_ms += 1;
    mt_test_us += 1000;
    ptp_test_us += 1000;
    sval_mt_remote_hint(hint_on ? hint : 0);
    sval_mt_task();
}
static void run(uint32_t ms) {
    while (ms--)
        tick();
}

static const uint8_t *latest(void) {
    return ptp_sent_count ? ptp_sent[ptp_sent_count - 1] : NULL;
}
// The USB half is the right one here, so the other pad is the left (pad 0).
static bool left_down(void) {
    const uint8_t *r = latest();
    if (!r) return false;
    for (int i = 0; i < r[33]; i++)
        if ((r[1 + 6 * i] & 2) && ptp_get16(r + 3 + 6 * i) < SVAL_PTP_PAD_STRIDE) return true;
    return false;
}

static bool ptp_paused(void) {
    uint8_t st[SVAL_PTP_STATUS_SIZE];
    sval_ptp_status(st);
    return st[4] & 128;
}

// ------------------------------------------------------------ the tests

static void test_other_half(void) {
    uint8_t raw[45], req[2] = {0xA7, 0}, reply[32];
    master    = false;
    left_half = true;
    clock_ms  = 10;
    // No USB half has asked: the news counter stays off the pointing report.
    raw_sample(raw, true, 300, 400);
    sval_mt_sample(raw, true);
    assert(sval_mt_split_hint() == 0);
    assert(sval_mt_rpc(2, req, 32, reply) && reply[0] == 0xA7 && (reply[1] & 1));
    assert(sval_mt_split_hint() == 0); // asked without "hints ok"
    req[1] = 1;
    assert(sval_mt_rpc(2, req, 32, reply));
    uint8_t h0 = sval_mt_split_hint();
    assert(h0 & 0x80);
    // The snapshot decodes to the frame the pad produced, and the test's own
    // encoder agrees with the firmware's.
    uint8_t frame[56];
    unpack_snapshot(reply, 0, frame);
    assert(frame[0] == 1 && frame[1] == 0 && frame[3] == 1);
    assert(ptp_get16(frame + 17) == 300 && ptp_get16(frame + 19) == 400 && frame[23] == 20 && frame[16] == snapshot[16]);
    peer_finger = true, peer_x = 300, peer_y = 400, peer_seq = reply[5], peer_epoch = reply[6], peer_age = reply[2];
    uint8_t mine[32];
    encode_peer(mine);
    mine[7] = reply[7]; // the lifetime ID is the firmware's
    assert(!memcmp(mine, reply, 32));
    // The largest coordinates survive the 11-bit packing.
    raw_sample(raw, true, 2047, 1791);
    sval_mt_sample(raw, true);
    assert(sval_mt_rpc(2, req, 32, reply));
    unpack_snapshot(reply, 0, frame);
    assert(ptp_get16(frame + 17) == 2047 && ptp_get16(frame + 19) == 1791);
    // The counter moves only on news: contacts, lifts and flags.
    uint8_t h = sval_mt_split_hint();
    sval_mt_sample(raw, true); // a stationary finger is still news
    assert(sval_mt_split_hint() != h);
    raw_sample(raw, false, 0, 0);
    sval_mt_sample(raw, true); // the lift is news
    h = sval_mt_split_hint();
    sval_mt_sample(raw, true);
    sval_mt_sample(raw, true);
    assert(sval_mt_split_hint() == h); // then quiet
    sval_mt_sample(raw, false);        // a failed read is news
    assert(sval_mt_split_hint() != h);
    for (int i = 0; i < 300; i++) {
        raw_sample(raw, true, 10, 10);
        sval_mt_sample(raw, true);
        assert(sval_mt_split_hint() & 0x80); // wraps, bit 7 stays
    }
    // A USB half that stops asking: the counter leaves the report after a second.
    clock_ms += 1001;
    assert(sval_mt_split_hint() == 0);
    // Other ops are not this file's.
    uint8_t other[3] = {0x10, 0, 0};
    assert(!sval_mt_rpc(3, other, 32, reply));
    puts("  other half: counter only after hints ok, only on news; 32-byte snapshot round-trips");
}

static void usb_half(void) {
    master    = true;
    left_half = false;
    connected = true;
    busy      = false;
    clock_ms += 1000;
    hands_with_pad = 3;
    other_half     = peer;
    peer_valid     = true;
    peer_age       = 2;
    sval_ptp_test_reset();
    sval_mt_init();
    uint8_t mode3[2] = {5, 3};
    assert(sval_ptp_set_report(3, 5, mode3, 2));
    pads_checked = 0;
    pad_mask = pad_hands = 0;
    remote_seen = remote_lost = remote_hints = remote_empty = last_ok = false;
    empty_replies                                                     = 0;
    uint8_t raw[45];
    raw_sample(raw, false, 0, 0);
    run(1);
    sval_mt_sample(raw, true); // the local pad shows all-up
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    sval_mt_sample(raw, true);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 2);
    assert(pads[0].armed && pads[1].armed && sval_ptp_pad_quiet(0) && remote_hints);
}

static void test_fetch_gating(void) {
    usb_half();
    assert(peer_last_req[0] == 0xA7 && peer_last_req[1] == 1); // asks for hints

    // Quiet: only the heartbeat.
    unsigned before = rpc_calls;
    run(1000);
    unsigned idle = rpc_calls - before;
    assert(idle >= 3 && idle <= 5);

    // Touch-down on the other pad: fetched on the next pass.
    set_peer(true, 50, 60);
    before = rpc_calls;
    tick();
    assert(rpc_calls == before + 1);
    run(9);
    assert(left_down());
    // Its contact is stamped with the snapshot's age: read 2 ms before the fetch.
    assert(pads[0].valid_us == ptp_test_us - 9000 - 2000);

    // A stationary finger: each new sample is fetched once.
    before = rpc_calls;
    for (int i = 0; i < 10; i++) {
        set_peer(true, 50, 60);
        run(8);
    }
    assert(rpc_calls - before == 10);

    // A lost counter change while a finger is down: the backup fetch runs.
    before = rpc_calls;
    run(40);
    assert(rpc_calls - before >= 40 / SVAL_PTP_REMOTE_BACKUP_MS - 1);

    // The lift is news and is delivered at once.
    set_peer(false, 0, 0);
    run(5);
    assert(!left_down());
    run(20);
    assert(sval_ptp_pad_quiet(0));

    // Finger down, then the link dies: lifted within the stale time.
    set_peer(true, 70, 80);
    run(9);
    assert(left_down());
    connected     = false;
    uint32_t died = clock_ms;
    while (left_down()) {
        tick();
        assert(clock_ms - died <= SVAL_PTP_REMOTE_STALE_MS + SVAL_PTP_REMOTE_BACKUP_MS + 10);
    }
    connected = true;
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    assert(pads[0].armed && sval_ptp_pad_quiet(0));
    puts("  fetch gating: news at once, heartbeat when quiet, backup while down, lifts, dead link lifts");
}

static void test_no_snapshot(void) {
    // The other half answers but has no sample (its pad not read yet): treat
    // it as no pad, heartbeat only.
    usb_half();
    peer_valid = false;
    news();
    run(20);
    unsigned before = rpc_calls;
    run(1000);
    assert(rpc_calls - before <= 5 && remote_empty);
    peer_valid = true;
    news();
    run(SVAL_PTP_REMOTE_IDLE_MS + 5);
    assert(!remote_empty);

    // Firmware without the op (Scan Lab's zeros): heartbeat only, nothing fed.
    usb_half();
    assert(sval_mt_suppress_hand(0)); // the other pad answered: the touchpad has it
    other_half = old_peer;
    run(SVAL_PTP_REMOTE_IDLE_MS + 5);
    before          = rpc_calls;
    unsigned frames = ptp_sent_count;
    run(1000);
    assert(rpc_calls - before <= 5 && remote_empty && !remote_hints && ptp_sent_count == frames);
    // Its pad is never handed to the touchpad: it stays a mouse.
    assert(!sval_mt_suppress_hand(0) && sval_mt_suppress_hand(1) && !sval_mt_os_touchpad(0));
    other_half = peer;
    puts("  no snapshot: a half with nothing to send, or older firmware, gets only the heartbeat");
}

static void test_no_pad_there(void) {
    // The other hand has a trackball: never fetched, never suppressed.
    usb_half();
    hands_with_pad = 2; // right only (the USB half)
    run(MT_PADS_CHECK_MS + 1);
    unsigned before = rpc_calls;
    run(2000);
    assert(rpc_calls == before);
    assert(!sval_mt_suppress_hand(0) && sval_mt_suppress_hand(1));
    // No pad anywhere: the touchpad sends nothing and suppresses nothing.
    hands_with_pad = 0;
    run(MT_PADS_CHECK_MS + 1);
    assert(!sval_mt_suppress_hand(0) && !sval_mt_suppress_hand(1) && !sval_ptp_wants_frames());
    hands_with_pad = 1; // the left pad only (the other half's)
    run(MT_PADS_CHECK_MS + 1);
    assert(sval_mt_suppress_hand(0) && !sval_mt_suppress_hand(1));
    hands_with_pad = 3;
    run(MT_PADS_CHECK_MS + 1);
    puts("  pads per hand: a hand without a pad is never fetched or suppressed");
}

static void test_update(void) {
    usb_half();
    set_peer(true, 90, 95);
    run(9);
    assert(left_down());
    busy            = true; // an update starts
    unsigned before = rpc_calls;
    run(50);
    assert(rpc_calls == before && !left_down() && ptp_paused());
    busy = false;
    set_peer(false, 0, 0);
    run(SVAL_PTP_REMOTE_IDLE_MS * 2);
    puts("  update: no split fetches, the other pad's contacts lift, reports pause");
}

static void test_activity(void) {
    usb_half();
    unsigned before = activity_calls;
    run(100);
    assert(activity_calls == before); // nothing down
    set_peer(true, 10, 20);
    run(9);
    before = activity_calls;
    run(20);
    assert(activity_calls - before >= 19);
    set_peer(false, 0, 0);
    run(20);
    puts("  activity: touchpad use is input activity");
}

static void test_reinit(void) {
    usb_half();
    uint8_t raw[45];
    raw_sample(raw, false, 0, 0);
    raw[3] = 0x80; // SHOW_RESET: the chip reset since init
    sval_mt_sample(raw, true);
    run(1);
    assert(reinit_calls == 1);
    sval_mt_sample(raw, true);
    run(100);
    assert(reinit_calls == 1); // not again within MT_REINIT_MIN_MS
    run(MT_REINIT_MIN_MS);
    assert(reinit_calls == 2);
    puts("  chip reset: set up again, at most every 5 s");
}

static void test_owner(void) {
    usb_half();
    uint8_t d[64] = {0};
    d[6]          = 0x42;
    d[7]          = 1;
    d[8]          = SVAL_TOUCH_OWNER_HOST; // retired
    sval_mt_command(d, 64);
    assert(d[6] == 0xC2 && d[7] == 2 && d[8] == SVAL_TOUCH_OWNER_NATIVE);
    memset(d, 0, 64);
    d[6] = 0x42;
    d[7] = 1;
    d[8] = SVAL_TOUCH_OWNER_MOUSE;
    sval_mt_command(d, 64);
    assert(d[7] == 0 && d[8] == SVAL_TOUCH_OWNER_MOUSE && (d[9] & 3) == 3);
    assert(d[10] == 0); // the native owner is off
    run(5);
    assert(!sval_ptp_suppress_mouse());
    memset(d, 0, 64);
    d[6] = 0x42;
    d[7] = 1;
    d[8] = SVAL_TOUCH_OWNER_NATIVE;
    sval_mt_command(d, 64);
    assert(d[7] == 0 && d[10] == 1 && d[10 + 16] == 3); // pads: left and right
    memset(d, 0, 64);
    d[6] = 0x42;
    d[7] = 2; // Velocity Scroll dev hold
    d[8] = 1;
    sval_mt_command(d, 64);
    assert(d[6] == 0xC2 && d[7] == 0 && (d[9] & 8) && velocity_holds == 1 && velocity_on);
    d[6] = 0x42;
    d[7] = 2;
    d[8] = 0;
    sval_mt_command(d, 64);
    assert(velocity_holds == 2 && !velocity_on && d[8] == SVAL_TOUCH_OWNER_NATIVE);
    puts("  owner command: mouse and native only; status v2 reports both pads; Velocity Scroll dev hold");
}

// ------------------------------------------------ a pinch over a flaky link
//
// One finger on each pad, both moving, as in a pinch across the hands. The
// other half samples its pad every 8 ms on its own clock and every sample is
// news. A failed split transaction blocks the USB half's main loop for the
// split timeout (20 ms) before it returns. Windows ends a pinch when either
// contact lifts, so the other pad's contact must stay down across short
// failures, and come back if it was lifted while the finger stayed down.

static uint32_t pinch_t0;
static bool     pinch_peer_down;
static unsigned pinch_zero_replies; // the next replies arrive as zeros (garbled or left over)

static void pinch_clocks(uint32_t ms) {
    clock_ms += ms;
    mt_test_us += ms * 1000u;
    ptp_test_us += ms * 1000u;
}
static uint32_t pinch_sample(void) {
    return (clock_ms - pinch_t0) / 8;
}
static void pinch_peer(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (pinch_zero_replies) {
        pinch_zero_replies--;
        memset(out, 0, out_len);
        return;
    }
    uint32_t k  = pinch_sample();
    peer_finger = pinch_peer_down;
    peer_x      = 1500 - (k % 1000);
    peer_y      = 900;
    peer_seq    = (uint8_t)k;
    peer_age    = (uint8_t)((clock_ms - pinch_t0) - k * 8);
    peer(in_len, in, out_len, out);
}

typedef struct {
    unsigned left_lifts; // left Tip = 0 (or gone) while the right stays down
    unsigned left_ids;   // distinct left contacts seen
    uint32_t blind_ms;   // longest time with the right down and no left contact
} pinch_result_t;

static void pinch_scan(unsigned from, pinch_result_t *res, bool *left_was, uint8_t *left_cid, uint32_t *blind_since) {
    for (unsigned n = from; n < ptp_sent_count; n++) {
        const uint8_t *r    = ptp_sent[n];
        bool           left = false, right = false;
        uint8_t        cid = 0xFF;
        for (int i = 0; i < r[33] && i < 5; i++) {
            if (!(r[1 + 6 * i] & 2)) continue;
            if (ptp_get16(r + 3 + 6 * i) < SVAL_PTP_PAD_STRIDE) {
                left = true;
                cid  = r[2 + 6 * i];
            } else {
                right = true;
            }
        }
        if (*left_was && !left && right) res->left_lifts++;
        if (left && cid != *left_cid) {
            res->left_ids++;
            *left_cid = cid;
        }
        if (right && !left) {
            if (*blind_since == 0) *blind_since = clock_ms + 1; // 0 means none
            uint32_t b = clock_ms + 1 - *blind_since;
            if (b > res->blind_ms) res->blind_ms = b;
        } else {
            *blind_since = 0;
        }
        *left_was = left;
    }
}

// Pinch for `ms`; at `fail_at` ms in, the next `fails` split transactions
// fail and then the next `zeros` replies come back as zeros.
static pinch_result_t pinch_ex(uint32_t ms, uint32_t fail_at, unsigned fails, unsigned zeros) {
    usb_half();
    ptp_sent_count  = 0; // only this pinch's reports
    rpc_blocked     = pinch_clocks;
    other_half      = pinch_peer;
    pinch_t0        = clock_ms;
    pinch_peer_down = true;
    uint8_t        raw[45];
    uint32_t       start = clock_ms, next_local = clock_ms + 3;
    unsigned       from = 0, k_right = 0;
    bool           left_was = false, failed = false;
    uint8_t        left_cid = 0xFF;
    uint32_t       blind    = 0;
    pinch_result_t res      = {0};
    while (clock_ms - start < ms) {
        if (!failed && clock_ms - start >= fail_at) {
            rpc_fail_next      = fails;
            pinch_zero_replies = zeros;
            failed             = true;
        }
        uint32_t k = pinch_sample();
        hint       = 0x80 | (k & 0x7F); // every sample of a finger down is news
        if ((int32_t)(clock_ms - next_local) >= 0) {
            raw_sample(raw, true, 500 + k_right++, 900); // the right pad, moving
            sval_mt_sample(raw, true);
            while ((int32_t)(clock_ms - next_local) >= 0)
                next_local += 8;
        }
        tick();
        pinch_scan(from, &res, &left_was, &left_cid, &blind);
        from = ptp_sent_count;
    }
    rpc_fail_next      = 0;
    pinch_zero_replies = 0;
    rpc_blocked        = NULL;
    other_half         = peer;
    pinch_peer_down    = false;
    set_peer(false, 0, 0);
    raw_sample(raw, false, 0, 0);
    sval_mt_sample(raw, true);
    run(SVAL_PTP_REMOTE_BACKUP_MS * 3);
    return res;
}

static pinch_result_t pinch(uint32_t ms, uint32_t fail_at, unsigned fails) {
    return pinch_ex(ms, fail_at, fails, 0);
}

static void test_pinch_flaky_link(void) {
    // A clean link: the left contact never lifts, one contact all along.
    pinch_result_t r = pinch(2000, 0, 0);
    assert(r.left_lifts == 0 && r.left_ids == 1 && r.blind_ms == 0);
    // Failed split transactions, 20 ms each, at every phase of the 8 ms
    // sample period.
    for (unsigned fails = 1; fails <= 6; fails++) {
        unsigned lifts = 0, worst_blind = 0;
        for (uint32_t phase = 0; phase < 8; phase++) {
            r = pinch(1500, 500 + phase, fails);
            lifts += r.left_lifts;
#ifndef PINCH_REPORT_ONLY
            assert(sval_ptp_lost_down(0) == r.left_lifts);
#endif
            if (r.blind_ms > worst_blind) worst_blind = r.blind_ms;
        }
        printf("  pinch, %u failed split transaction%s in a row: left lifted in %u of 8 phases, longest without the left contact %u ms\n", fails, fails == 1 ? "" : "s", lifts, worst_blind);
#ifndef PINCH_REPORT_ONLY
        // Up to five failures (100 ms blocked) the contact holds.
        if (fails <= 5) assert(lifts == 0 && worst_blind == 0);
#endif
    }
    // A reply that comes back as zeros (no checksum on the split RPC reply):
    // one or two in a row are failed fetches, not a pad that went away.
    {
        unsigned lifts = 0;
        for (unsigned zeros = 1; zeros < MT_EMPTY_REPLIES; zeros++) {
            r = pinch_ex(1500, 500, 0, zeros);
            lifts += r.left_lifts;
            printf("  pinch, %u zeroed split repl%s in a row: left lifted %u time%s\n", zeros, zeros == 1 ? "y" : "ies", r.left_lifts, r.left_lifts == 1 ? "" : "s");
        }
#ifndef PINCH_REPORT_ONLY
        assert(lifts == 0);
#endif
    }
#ifndef PINCH_REPORT_ONLY
    // A link that stays down: the left contact lifts within the remote stale
    // time, and when the link comes back with the finger still down it is
    // reported again (a new contact) instead of staying lost until the
    // finger lifts.
    r = pinch(3000, 500, 40); // 800 ms of failures
    assert(r.left_lifts == 1 && r.left_ids == 2);
    assert(sval_ptp_lost_down(0) == 1 && sval_ptp_lost_down(1) == 0); // counted for the dev status
    assert(r.blind_ms < 40 * 20 + 50);
    puts("  pinch over a flaky link: the other pad's contact holds across failed split transactions, and returns after a lost link");
#endif
}

// Palm rejection on the mouse path: latched from the first contact above
// the threshold until every finger lifts; off by default.
static void test_palm_latch(void) {
    uint8_t raw[45];
    test_palm_at = 0;
    raw_sample(raw, true, 500, 500);
    raw[16] = 40; // a big contact
    sval_mt_sample(raw, true);
    assert(!sval_mt_palm()); // off: nothing is a palm
    test_palm_at = 10;
    raw_sample(raw, true, 500, 500);
    raw[16] = 8; // a fingertip
    sval_mt_sample(raw, true);
    assert(!sval_mt_palm());
    raw[16] = 11; // it spreads past the threshold
    sval_mt_sample(raw, true);
    assert(sval_mt_palm());
    raw[16] = 6; // and shrinks again: still a palm until it lifts
    sval_mt_sample(raw, true);
    assert(sval_mt_palm());
    sval_mt_sample(raw, false); // a failed read changes nothing
    assert(sval_mt_palm());
    raw_sample(raw, false, 0, 0);
    sval_mt_sample(raw, true);
    assert(!sval_mt_palm()); // lifted
    raw_sample(raw, true, 500, 500);
    raw[16] = 30;
    sval_mt_sample(raw, true);
    assert(sval_mt_palm());
    test_palm_at = 0; // turned off while down: no palm
    assert(!sval_mt_palm());
    raw_sample(raw, false, 0, 0);
    sval_mt_sample(raw, true);
    puts("  palm rejection: off by default; latched above the threshold until the lift");
}

int main(void) {
    puts("native PTP split and contact path tests:");
    test_other_half();
    test_fetch_gating();
    test_no_snapshot();
    test_no_pad_there();
    test_update();
    test_activity();
    test_reinit();
    test_owner();
    test_pinch_flaky_link();
    test_palm_latch();
    puts("native PTP split and contact path: all passed");
    return 0;
}
