// Host tests for the native Precision Touchpad unifier and feature reports
// (keyboards/svalboard/azoteq/ptp.c). Run with util/test_native_ptp.sh.
// Writes every report it "sends" to argv[1], which test_reports.py decodes
// again with the descriptor's own layout.
#define SVAL_PTP_HOST_TEST
// The palm rejection threshold, a setting on the board (VIA value 58): a
// variable here, so a test can turn it off and back on.
#include <stdint.h>
static uint8_t test_palm_at = 200;
#define SVAL_PTP_PALM_AREA test_palm_at
// The touchpad mode (VIA value 59): built-in gestures when set.
#include <stdbool.h>
static bool test_builtin;
#define SVAL_PTP_BUILTIN test_builtin
#define SVAL_PTP_KEY_BUTTONS 1
// Longer than the keepalive, so the keepalive scenario sees a live pad.
#define SVAL_PTP_STALE_MS 120
#include "../../keyboards/svalboard/azoteq/ptp.c"

typedef struct {
    uint8_t  slot, life;
    uint16_t x, y;
    uint8_t  area;
} touch_t;

static uint16_t seqs[2];
static uint32_t epochs[2] = {1, 1};
static unsigned mark;

static void feedn(uint8_t pad, uint8_t flags, int n, const touch_t *t) {
    uint8_t f[56] = {0};
    f[0]          = 1;
    f[1]          = pad;
    f[2]          = flags;
    for (int i = 0; i < n; i++) {
        uint8_t *s = f + 16 + 8 * t[i].slot;
        f[3] |= 1u << t[i].slot;
        s[0] = t[i].life;
        ptp_put16(s + 1, t[i].x);
        ptp_put16(s + 3, t[i].y);
        ptp_put16(s + 5, 100);
        s[7] = t[i].area ? t[i].area : 20;
    }
    ptp_put16(f + 4, seqs[pad]++);
    ptp_put16(f + 6, 56);
    ptp_put32(f + 12, epochs[pad]);
    sval_ptp_feed(f, ptp_test_us);
}
#define FEED(pad, ...)                                        \
    do {                                                      \
        touch_t t_[] = {__VA_ARGS__};                         \
        feedn(pad, 0, (int)(sizeof(t_) / sizeof(t_[0])), t_); \
    } while (0)
static void up(uint8_t pad) {
    feedn(pad, 0, 0, NULL);
}
static void flagged(uint8_t pad, uint8_t flags) {
    feedn(pad, flags, 0, NULL);
}
static void run(uint32_t us) {
    ptp_test_us += us;
    sval_ptp_task();
}
static void set_feature(uint8_t id, uint8_t value) {
    uint8_t b[2] = {id, value};
    assert(sval_ptp_set_report(3, id, b, 2));
}

// Report accessors.
static const uint8_t *last(void) {
    assert(ptp_sent_count);
    return ptp_sent[ptp_sent_count - 1];
}
static unsigned new_reports(void) {
    unsigned n = ptp_sent_count - mark;
    mark       = ptp_sent_count;
    return n;
}
static uint8_t count_of(const uint8_t *r) {
    return r[33];
}
static uint8_t flags_of(const uint8_t *r, int i) {
    return r[1 + 6 * i];
}
static uint8_t id_of(const uint8_t *r, int i) {
    return r[2 + 6 * i];
}
static uint16_t x_of(const uint8_t *r, int i) {
    return ptp_get16(r + 3 + 6 * i);
}
static uint16_t y_of(const uint8_t *r, int i) {
    return ptp_get16(r + 5 + 6 * i);
}
static uint16_t scan_of(const uint8_t *r) {
    return ptp_get16(r + 31);
}
static int slot_with_id(const uint8_t *r, uint8_t id) {
    for (int i = 0; i < r[33]; i++)
        if (id_of(r, i) == id) return i;
    return -1;
}
static int slot_at(const uint8_t *r, uint16_t x, uint16_t y) {
    for (int i = 0; i < r[33]; i++)
        if (x_of(r, i) == x && y_of(r, i) == y) return i;
    return -1;
}
// Every report: unused slots are zero and IDs in one frame are distinct.
static void well_formed(const uint8_t *r) {
    assert(r[0] == 1 && r[33] <= 5 && (r[34] & ~7) == 0);
    for (int i = r[33]; i < 5; i++)
        for (int b = 0; b < 6; b++)
            assert(r[1 + 6 * i + b] == 0);
    for (int i = 0; i < r[33]; i++) {
        assert((flags_of(r, i) & ~3) == 0 && id_of(r, i) < 16);
        assert(x_of(r, i) <= SVAL_PTP_XMAX && y_of(r, i) <= SVAL_PTP_YMAX);
        for (int j = 0; j < i; j++)
            assert(id_of(r, i) != id_of(r, j));
    }
}

static void fresh(const char *name) {
    for (unsigned i = mark; i < ptp_sent_count; i++)
        well_formed(ptp_sent[i]);
    ptp_scenario = name;
    sval_ptp_test_reset();
    sval_ptp_set_pads(3); // a pad on each hand
    ptp_test_busy = false;
    ptp_test_us += 1000000;
    mark = ptp_sent_count;
}

// Native owner, host wrote Input Mode 3, both pads armed with an all-up sample.
static void start(const char *name) {
    fresh(name);
    sval_ptp_set_native(true);
    set_feature(5, 3);
    run(10000);
    up(0);
    up(1);
    run(10000);
    assert(new_reports() == 0);
}

static void test_features(void) {
    fresh("features");
    const uint8_t *d = NULL;
    assert(sval_ptp_get_report(3, 2, &d) == 2 && d[0] == 2 && d[1] == 0x25);
    assert(sval_ptp_get_report(3, 3, &d) == 257 && d[0] == 3 && d[1] == 0xFC && d[256] == 0xC2);
    assert(sval_ptp_get_report(3, 4, &d) == 2 && d[0] == 4 && d[1] == 0);
    assert(sval_ptp_get_report(3, 5, &d) == 2 && d[0] == 5 && d[1] == 0);
    assert(sval_ptp_get_report(3, 6, &d) == 2 && d[0] == 6 && d[1] == 3);
    assert(sval_ptp_get_report(1, 1, &d) == 35 && d[0] == 1);
    assert(sval_ptp_get_report(3, 1, &d) == 0 && sval_ptp_get_report(3, 7, &d) == 0);
    assert(sval_ptp_get_report(1, 2, &d) == 0 && sval_ptp_get_report(2, 1, &d) == 0);
    set_feature(5, 3);
    assert(sval_ptp_get_report(3, 5, &d) == 2 && d[1] == 3);
    set_feature(4, 3); // only bit 0 is latency
    assert(sval_ptp_get_report(3, 4, &d) == 2 && d[1] == 1);
    set_feature(6, 0xFD);
    assert(sval_ptp_get_report(3, 6, &d) == 2 && d[1] == 1);
    uint8_t ok[2] = {5, 0}, mismatch[2] = {4, 0}, caps[2] = {2, 0x25}, in[2] = {1, 0};
    assert(!sval_ptp_set_report(3, 5, ok, 1) && !sval_ptp_set_report(3, 5, ok, 3)); // length
    assert(!sval_ptp_set_report(3, 5, mismatch, 2));                                // ID in data
    assert(!sval_ptp_set_report(3, 2, caps, 2));                                    // read only
    assert(!sval_ptp_set_report(3, 3, caps, 2) && !sval_ptp_set_report(1, 1, in, 2));
    assert(!sval_ptp_set_report_allowed(2, 5, 2) && sval_ptp_set_report_allowed(3, 6, 2));
    assert(sval_ptp_get_report(3, 5, &d) == 2 && d[1] == 3); // refused writes changed nothing
    sval_ptp_usb_reset();
    assert(sval_ptp_get_report(3, 5, &d) == 2 && d[1] == 0);
    assert(sval_ptp_get_report(3, 4, &d) == 2 && d[1] == 0);
    assert(sval_ptp_get_report(3, 6, &d) == 2 && d[1] == 3);
    puts("  feature reports: caps 0x25, 257-byte blob, latency, input mode, switches, refusals, reset");
}

static void test_negotiation(void) {
    fresh("negotiation");
    sval_ptp_set_native(true);
    assert(!sval_ptp_suppress_mouse() && sval_ptp_wants_frames() && !sval_ptp_active());
    FEED(0, {0, 1, 100, 100});
    run(10000);
    assert(new_reports() == 0); // mouse until the host writes 3
    set_feature(5, 3);
    assert(sval_ptp_suppress_mouse() && sval_ptp_active()); // at once, before the task
    run(10000);
    FEED(0, {0, 1, 110, 100});
    run(10000);
    assert(new_reports() == 0); // finger was down at the switch: not armed
    up(0);
    FEED(0, {0, 2, 200, 300});
    FEED(1, {0, 1, 50, 50}); // right pad never showed all-up
    run(10000);
    assert(new_reports() == 1 && count_of(last()) == 1);
    assert(flags_of(last(), 0) == 3 && x_of(last(), 0) == 200 && y_of(last(), 0) == 300);
    puts("  negotiation: mouse until Input Mode 3, suppress at once, each pad arms on all-up");
}

static void test_one_finger(void) {
    start("one-finger");
    FEED(0, {0, 1, 500, 600});
    run(5000);
    assert(new_reports() == 1);
    uint8_t a = id_of(last(), 0);
    assert(count_of(last()) == 1 && flags_of(last(), 0) == 3);
    FEED(0, {0, 1, 520, 610});
    run(8000);
    assert(new_reports() == 1 && x_of(last(), 0) == 520 && y_of(last(), 0) == 610 && id_of(last(), 0) == a);
    up(0);
    run(8000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 1);
    assert(id_of(last(), 0) == a && x_of(last(), 0) == 520); // lift at the last position
    run(8000);
    run(200000);
    up(0);
    run(8000);
    assert(new_reports() == 0); // no keepalive with nothing down
    FEED(0, {0, 2, 10, 10});
    run(8000);
    assert(new_reports() == 1 && id_of(last(), 0) != a); // round robin: not reused at once
    assert(sval_ptp_take_motion(0) == 30 && sval_ptp_take_motion(0) == 0 && sval_ptp_take_motion(1) == 0);
    puts("  one finger: down, move, one lift frame at the last position, then silence; new ID");
}

static void test_two_pads(void) {
    start("two-pads");
    FEED(0, {1, 1, 100, 200});
    FEED(1, {0, 1, 100, 200});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 2);
    assert(slot_at(last(), 100, 200) >= 0);
    assert(slot_at(last(), 100 + 2048 + SVAL_PTP_GAP, 200) >= 0);
    assert(SVAL_PTP_GAP == 476 && SVAL_PTP_XMAX == 4571);
    // Both pads down: a frame with only the left pad's new sample waits for
    // the right pad's (one period at most), so it carries both fresh.
    FEED(0, {1, 1, 110, 200});
    run(1000);
    run(3000);
    assert(new_reports() == 0 && merge_waits > 0);
    FEED(1, {0, 1, 105, 200});
    run(1000);
    assert(new_reports() == 1 && slot_at(last(), 110, 200) >= 0 && slot_at(last(), 105 + 2048 + SVAL_PTP_GAP, 200) >= 0);
    // The other pad's sample never comes: the frame goes out at the deadline.
    FEED(0, {1, 1, 120, 200});
    run(1000);
    assert(new_reports() == 0);
    run(SVAL_PTP_MERGE_US);
    assert(new_reports() == 1 && slot_at(last(), 120, 200) >= 0);
    puts("  two pads: one frame, right pad offset by 2048 + gap, a frame waits for the other pad's sample");
}

static void test_frame_cap(void) {
    start("frame-cap");
    FEED(0, {1, 1, 100, 200});
    run(5000);
    assert(new_reports() == 1);
    // One pad: a new sample 1 ms later waits until 4 ms have passed.
    FEED(0, {1, 1, 110, 200});
    run(1000);
    assert(new_reports() == 0);
    run(3000);
    assert(new_reports() == 1 && slot_at(last(), 110, 200) >= 0);
    puts("  frame cap: at most one frame every 4 ms");
}

static void test_cross_gap(void) {
    start("cross-gap");
    FEED(0, {0, 1, 2047, 800});
    run(5000);
    uint8_t a = id_of(last(), 0);
    new_reports();
    up(0);
    FEED(1, {0, 5, 0, 800});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 2);
    int old = slot_with_id(last(), a), nu = old == 0 ? 1 : 0;
    assert(old >= 0 && flags_of(last(), old) == 1 && x_of(last(), old) == 2047);
    assert(flags_of(last(), nu) == 3 && x_of(last(), nu) == 2048 + SVAL_PTP_GAP && id_of(last(), nu) != a);
    assert(x_of(last(), nu) - x_of(last(), old) == SVAL_PTP_GAP + 1);
    FEED(1, {0, 5, 5, 800});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 3);
    puts("  crossing the gap: lift on the left edge, new contact past the gap, distinct IDs");
}

static void test_five_contacts(void) {
    start("five-contacts");
    // Both pads stay down here and only one is fed at a time, so each frame
    // goes out at the other-pad deadline (SVAL_PTP_MERGE_US).
    FEED(0, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 300, 100});
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 5);
    uint16_t sixth_x = 300 + 2048 + SVAL_PTP_GAP;
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 300, 100});
    run(9000); // past the wait for the other pad
    assert(new_reports() == 1 && count_of(last()) == 5 && slot_at(last(), sixth_x, 100) < 0);
    FEED(0, {1, 2, 200, 100}, {2, 3, 300, 100}); // first contact lifts
    run(9000);                                   // past the wait for the other pad
    assert(new_reports() == 1 && count_of(last()) == 5 && slot_at(last(), sixth_x, 100) < 0);
    assert(flags_of(last(), slot_at(last(), 100, 100)) == 1);
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 300, 101});
    run(9000);                                           // past the wait for the other pad
    assert(new_reports() == 1 && count_of(last()) == 4); // the sixth stays ignored
    for (int i = 0; i < 4; i++)
        assert(y_of(last(), i) == 100);
    FEED(0, {1, 2, 200, 100}, {2, 3, 300, 100}, {3, 4, 400, 100}); // a new contact
    run(9000);                                                     // past the wait for the other pad
    assert(new_reports() == 1 && count_of(last()) == 5 && slot_at(last(), 400, 100) >= 0);
    puts("  five contacts: a sixth is ignored for its whole lifetime, a later new one is admitted");
}

static void test_short_tap(void) {
    start("short-tap");
    FEED(1, {0, 1, 300, 300});
    up(1); // appeared and vanished before any frame was built
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 3);
    uint8_t a = id_of(last(), 0);
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 1 && id_of(last(), 0) == a);
    run(60000);
    assert(new_reports() == 0);
    puts("  tap shorter than a frame: one Tip=1 frame, then one Tip=0 frame");
}

static void test_keepalive(void) {
    start("keepalive");
    FEED(0, {0, 1, 700, 700});
    run(5000);
    uint16_t s = scan_of(last());
    new_reports();
    run(20000);
    assert(new_reports() == 0);
    run(31000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 3 && x_of(last(), 0) == 700);
    // The first frame is stamped when its sample was read, 5 ms before it
    // went out; the keepalive, with no new sample, when it is sent: 56 ms.
    assert((uint16_t)(scan_of(last()) - s) == 560);
    puts("  keepalive: a stationary contact repeats every 50 ms; scan time in 100 us units");
}

static void test_scan_time(void) {
    start("scan-time");
    FEED(0, {0, 1, 1, 1});
    run(5000);
    uint16_t prev = scan_of(last());
    uint32_t t0 = ptp_test_us - 5000, total = 0; // the first sample's read time
    for (int i = 0; i < 70; i++) {               // about 7 s: the 16-bit counter wraps once
        uint32_t step = 100000 + 37;             // shorter than the stale time: the pad is alive
        FEED(0, {0, 1, (uint16_t)(2 + i), 1});
        total = ptp_test_us - t0; // stamped with this sample's read time
        run(step);
        uint16_t s = scan_of(last());
        assert(s != prev);
        prev = s;
    }
    assert(new_reports() == 71);
    uint16_t first = scan_of(ptp_sent[ptp_sent_count - 71]);
    assert((uint16_t)(prev - first) == (uint16_t)(total / 100));
    assert(total / 100 > 65535);
    puts("  scan time: the samples' read times, wraps at 16 bits, remainder carried, never repeats");
}

static void test_palm(void) {
    start("palm");
    FEED(0, {0, 1, 100, 100});
    FEED(1, {0, 1, 100, 100});
    run(5000);
    new_reports();
    flagged(0, 0x01); // chip palm flag: no positions in this frame
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 2);
    int p = slot_at(last(), 100, 100);
    assert(p >= 0 && flags_of(last(), p) == 2); // Confidence 0, Tip 1
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), slot_at(last(), 100, 100)) == 0);
    FEED(0, {0, 2, 150, 150});
    FEED(1, {0, 1, 100, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1); // left pad must arm again
    up(0);
    FEED(0, {0, 3, 150, 150});
    run(9000); // the right pad is down too and not fed: past the wait for it
    assert(new_reports() == 1 && count_of(last()) == 2);
    puts("  palm flag: one Confidence=0 frame, lift, the pad arms again on all-up");
}

static void test_palm_area(void) {
    start("palm-area");
    FEED(0, {0, 1, 100, 100, 250});
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 2);
    FEED(0, {0, 1, 105, 100, 20});
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 2); // cleared Confidence stays cleared
    up(0);
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 0);
    puts("  palm area: Confidence cleared for good, contact still lifts normally");
}

static void test_builtin_gestures(void) {
    start("builtin-gestures");
    // Automatic: mode 3 reports contacts and the pad mouse is off.
    FEED(0, {0, 1, 100, 100, 20});
    run(5000);
    assert(new_reports() == 1 && sval_ptp_reporting() && sval_ptp_suppress_mouse());
    // Built-in gestures, set while a finger is down: the contact is lifted
    // once, then nothing more goes to the host while the host keeps mode 3.
    test_builtin = true;
    run(5000);
    assert(!sval_ptp_reporting() && !sval_ptp_active());
    unsigned lifted = new_reports();
    assert(lifted <= 1 && (lifted == 0 || (flags_of(last(), 0) & 2) == 0)); // a release: Tip clear
    FEED(0, {0, 1, 300, 100, 20});
    run(5000);
    up(0);
    run(5000);
    assert(new_reports() == 0);
    assert(!sval_ptp_suppress_mouse()); // the chip's mouse, taps and scroll work
    const uint8_t *d = NULL;
    assert(sval_ptp_get_report(3, 5, &d) == 2 && d[1] == 3); // the host reads back the mode it wrote
    // Back to Automatic: contacts reach the host again.
    test_builtin = false;
    run(5000);
    up(0); // the pads arm again on an all-up sample, as after any return to mode 3
    up(1);
    run(5000);
    new_reports();
    FEED(0, {0, 2, 100, 100, 20});
    run(5000);
    assert(new_reports() >= 1 && sval_ptp_reporting() && sval_ptp_suppress_mouse());
    up(0);
    run(5000);
    puts("  built-in gestures: mode 3 kept but no contacts; the pad mouse works; back to Automatic reports again");
}

static void test_palm_setting(void) {
    start("palm-setting");
    // Off (the default): a large contact is a finger, Confidence set.
    test_palm_at = 0;
    FEED(0, {0, 1, 100, 100, 250});
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 3);
    up(0);
    run(5000);
    new_reports();
    // A lower threshold, set while running: the same contact is a palm.
    test_palm_at = 12;
    FEED(0, {0, 2, 100, 100, 13}, {1, 3, 900, 900, 4});
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 2 && flags_of(last(), 1) == 3); // palm clear, finger kept
    FEED(0, {0, 2, 100, 100, 12}, {1, 3, 905, 900, 4});
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 2 && flags_of(last(), 1) == 3); // once a palm, always
    up(0);
    run(5000);
    test_palm_at = 200;
    puts("  palm setting: off keeps every contact; a threshold set at run time clears only the palm's Confidence");
}

static void test_faults(void) {
    start("faults");
    FEED(0, {0, 1, 100, 100});
    FEED(1, {0, 1, 100, 100});
    run(5000);
    new_reports();
    uint16_t rx = 100 + 2048 + SVAL_PTP_GAP;
    flagged(1, 0x08); // right chip reset
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 2);
    assert(flags_of(last(), slot_at(last(), rx, 100)) == 1 && flags_of(last(), slot_at(last(), 100, 100)) == 3);
    FEED(1, {0, 1, 100, 100});
    FEED(0, {0, 1, 101, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && x_of(last(), 0) == 101);
    flagged(0, 0x04); // invalid read: hold
    run(5000);
    run(5000);
    assert(new_reports() == 0);
    FEED(0, {0, 1, 102, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 3);
    up(1);
    FEED(1, {0, 2, 100, 100});
    run(9000); // past the wait for the left pad's sample
    assert(new_reports() == 1 && count_of(last()) == 2);
    sval_ptp_pad_lost(1); // remote snapshot stale
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), slot_at(last(), rx, 100)) == 1);
    assert(flags_of(last(), slot_at(last(), 102, 100)) == 3);
    FEED(0, {0, 1, 103, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1); // the local pad keeps reporting
    puts("  faults: a reset or stale pad lifts only its own contacts; invalid reads hold");
}

static void test_stream_restart(void) {
    start("stream-restart");
    FEED(0, {0, 7, 500, 500});
    run(5000);
    uint8_t a = id_of(last(), 0);
    epochs[0]++;
    touch_t near = {0, 1, 520, 505, 20};
    feedn(0, 0x10, 1, &near); // restart, lifetime IDs from 1 again, same finger
    run(5000);
    assert(count_of(last()) == 1 && id_of(last(), 0) == a && flags_of(last(), 0) == 3);
    epochs[0]++;
    FEED(0, {0, 1, 1500, 1500}); // restart, the slot now holds a far contact
    run(5000);
    assert(count_of(last()) == 2 && flags_of(last(), slot_with_id(last(), a)) == 1);
    puts("  stream restart: a contact keeps its ID when it stays put, else lift and new");
}

static void test_leave_mode3(void) {
    start("leave-mode3");
    FEED(0, {0, 1, 100, 100});
    FEED(1, {0, 1, 100, 100});
    run(5000);
    new_reports();
    set_feature(5, 0);
    assert(sval_ptp_suppress_mouse()); // still suppressed before the task runs
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 2 && last()[34] == 0);
    assert(flags_of(last(), 0) == 1 && flags_of(last(), 1) == 1);
    FEED(0, {0, 1, 110, 100});
    run(60000);
    assert(new_reports() == 0 && sval_ptp_suppress_mouse() && !sval_ptp_active());
    up(0);
    run(5000);
    assert(sval_ptp_suppress_mouse()); // the right pad is still down
    up(1);
    run(5000);
    assert(!sval_ptp_suppress_mouse());
    puts("  leaving mode 3: one all-up frame, then mouse only after every finger is up");
}

static void test_surface_switch(void) {
    start("surface-switch");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    set_feature(6, 2); // surface off, buttons on
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1);
    FEED(0, {0, 1, 110, 100});
    run(5000);
    assert(new_reports() == 0 && sval_ptp_suppress_mouse());
    set_feature(6, 3);
    run(5000);
    FEED(0, {0, 1, 120, 100});
    run(5000);
    assert(new_reports() == 0); // re-arm first
    up(0);
    FEED(0, {0, 2, 130, 100});
    run(5000);
    assert(new_reports() == 1 && x_of(last(), 0) == 130);
    puts("  surface switch: off sends one all-up frame and silence; on again needs all-up");
}

static void test_quick_toggle(void) {
    start("quick-toggle");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    ptp_test_busy = true;
    set_feature(6, 2); // surface off; the release cannot go out yet
    run(5000);
    set_feature(6, 3); // and on again before it did
    ptp_test_busy = false;
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 1);
    FEED(0, {0, 1, 100, 100});
    run(5000);
    assert(new_reports() == 0); // the pad arms again first
    puts("  quick toggle: a pending release still goes out before reporting resumes");
}

static void test_owner_change(void) {
    start("owner-change");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    sval_ptp_set_native(false);
    assert(sval_ptp_suppress_mouse() && !sval_ptp_active() && sval_ptp_wants_frames());
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1);
    up(0);
    run(5000);
    assert(!sval_ptp_suppress_mouse() && !sval_ptp_wants_frames());
    puts("  owner change: the old owner releases, the new one waits for all-up");
}

static void test_usb_reset(void) {
    start("usb-reset");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    const uint8_t *d = NULL;
    assert(sval_ptp_get_report(1, 1, &d) == 35 && d[33] == 1);
    sval_ptp_usb_reset();
    run(5000);
    assert(new_reports() == 0 && sval_ptp_suppress_mouse() && !sval_ptp_active());
    // GET_REPORT(Input) no longer hands out the old contact.
    assert(sval_ptp_get_report(1, 1, &d) == 35 && d[0] == 1 && d[33] == 0 && d[1] == 0 && d[34] == 0);
    up(0);
    up(1);
    run(5000);
    assert(!sval_ptp_suppress_mouse());
    puts("  USB reset: back to mouse mode with no stray frame, pad mouse after all-up");
}

static void test_busy_endpoint(void) {
    start("busy");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    ptp_test_busy = true;
    up(0);
    run(5000);
    run(5000);
    assert(new_reports() == 0 && send_busy == 2);
    ptp_test_busy = false;
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1); // the lift is not lost
    puts("  busy endpoint: never blocks, retries, the lift still arrives");
}

static void test_contact_ids(void) {
    start("contact-ids");
    uint8_t seen[40];
    for (int i = 0; i < 40; i++) {
        FEED(i & 1, {0, (uint8_t)(i + 1), 10, 10});
        run(5000);
        seen[i] = id_of(last(), 0);
        up(i & 1);
        run(5000);
        assert(flags_of(last(), 0) == 1);
    }
    for (int i = 1; i < 40; i++)
        assert(seen[i] == ((seen[i - 1] + 1) & 15));
    puts("  contact IDs: one pool of 0..15 for both pads, round robin");
}

static void test_key_buttons(void) {
    start("key-buttons");
    assert(sval_ptp_key_button(0, true));
    run(5000);
    // MS_BTN1 is the primary external button: PTP Button 2 (bit 1).
    assert(new_reports() == 1 && count_of(last()) == 0 && last()[34] == 2);
    assert(sval_ptp_key_button(1, true)); // MS_BTN2: Button 3 (bit 2)
    run(5000);
    assert(new_reports() == 1 && last()[34] == 6);
    set_feature(6, 1); // button switch off: held buttons are released at once
    run(5000);
    assert(new_reports() == 1 && last()[34] == 0);
    assert(sval_ptp_key_button(0, false) && sval_ptp_key_button(1, false));
    run(5000);
    new_reports();
    // With the button switch off, a press is not taken: it stays on the
    // mouse report, so the click is not lost.
    assert(!sval_ptp_key_button(1, true) && !sval_ptp_key_button(1, false));
    set_feature(6, 3);
    run(5000);
    assert(sval_ptp_key_button(0, true) && sval_ptp_key_button(1, true));
    run(5000);
    assert(new_reports() == 1 && last()[34] == 6);
    // MS_BTN3 (middle) has no PTP usage: it stays on the mouse report.
    assert(!sval_ptp_key_button(2, true) && !sval_ptp_key_button(2, false));
    assert(sval_ptp_key_button(0, false) && sval_ptp_key_button(1, false));
    run(5000);
    assert(new_reports() == 1 && last()[34] == 0);
    set_feature(5, 0);
    run(5000);
    assert(!sval_ptp_key_button(1, true) && !sval_ptp_key_button(1, false)); // mouse report owns it
    // Surface switch off (no reports): the press stays on the mouse report.
    set_feature(5, 3);
    set_feature(6, 2);
    run(5000);
    assert(!sval_ptp_key_button(0, true) && !sval_ptp_key_button(0, false));
    puts("  key buttons (build option): taken only while reporting with the button switch on; release follows press");
}

static void test_pads(void) {
    // No pad on either hand: nothing is ever sent or suppressed, even in mode 3.
    fresh("no-pads");
    sval_ptp_set_pads(0);
    sval_ptp_set_native(true);
    set_feature(5, 3);
    run(5000);
    up(0);
    up(1);
    FEED(0, {0, 1, 100, 100});
    run(5000);
    assert(new_reports() == 0 && !sval_ptp_suppress_mouse() && !sval_ptp_active() && !sval_ptp_wants_frames());
    // A pad on the right hand only: the left hand's frames are ignored.
    fresh("right-pad-only");
    sval_ptp_set_pads(2);
    sval_ptp_set_native(true);
    set_feature(5, 3);
    run(5000);
    up(0);
    up(1);
    run(5000);
    FEED(0, {0, 1, 100, 100});
    FEED(1, {0, 1, 300, 400});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 1 && x_of(last(), 0) == 300 + 2048 + SVAL_PTP_GAP);
    assert(sval_ptp_suppress_mouse() && sval_ptp_active());
    // The pad goes away (its half came back with another module): its
    // contact lifts.
    sval_ptp_set_pads(0);
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1 && !sval_ptp_suppress_mouse());
    puts("  pads: no pad, nothing sent or suppressed; one pad reports alone; a pad that goes away lifts");
}

static void test_dead_pad(void) {
    start("dead-pad");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    // The local pad stops answering mid-touch (every read invalid): its
    // contact lifts after SVAL_PTP_STALE_MS, not kept alive forever.
    unsigned frames = 0;
    uint32_t died   = ptp_test_us;
    for (int ms = 0; ms < 2 * SVAL_PTP_STALE_MS; ms += 8) {
        flagged(0, 0x04);
        run(8000);
        frames += new_reports();
        if (frames && flags_of(last(), 0) == 1) break;
    }
    assert(frames >= 1 && flags_of(last(), 0) == 1);
    assert(ptp_test_us - died <= SVAL_PTP_STALE_MS * 1000u + 16000u);
    uint8_t old_id = id_of(last(), 0);
    run(100000);
    assert(new_reports() == 0); // no keepalive for it afterwards
    // It answers again with the finger still down: the finger is reported
    // again at once, as a new contact (not unseen until it lifts).
    FEED(0, {0, 1, 120, 100});
    run(8000);
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 3 && id_of(last(), 0) != old_id && x_of(last(), 0) == 120);
    up(0);
    run(8000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1);
    // A pad lifted for a chip reset still waits for an all-up sample.
    FEED(0, {0, 1, 100, 100});
    run(8000);
    new_reports();
    flagged(0, 0x08);
    run(8000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1);
    FEED(0, {0, 1, 140, 100});
    run(8000);
    assert(new_reports() == 0);
    up(0);
    FEED(0, {0, 2, 150, 100});
    run(8000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 3);
    up(0);
    run(8000);
    new_reports();
    puts("  dead pad: a contact on a pad that stops answering lifts after the stale time, and a finger still down comes back when it answers");
}

static void test_sample_time(void) {
    start("sample-time");
    FEED(0, {0, 1, 100, 100});
    FEED(1, {0, 1, 100, 100});
    run(5000);
    uint16_t s0 = scan_of(last());
    new_reports();
    // Left read now, right read 3 ms later, the frame sent 2 ms after that:
    // it is stamped with the newest sample's read time.
    FEED(0, {0, 1, 101, 100});
    ptp_test_us += 3000;
    FEED(1, {0, 1, 101, 100});
    run(2000);
    assert(new_reports() == 1);
    assert((uint16_t)(scan_of(last()) - s0) == 80); // 5 ms + 3 ms after the first frame's samples
    puts("  sample time: a frame carries the read time of its newest pad sample");
}

static void test_host_silent(void) {
    start("host-silent");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    ptp_test_busy = true; // the host stopped reading the endpoint
    for (int i = 0; i < 20; i++) {
        FEED(0, {0, 1, (uint16_t)(101 + i), 100});
        run(8000);
    }
    assert(host_silent == 1);
    const uint8_t *d = NULL;
    assert(sval_ptp_get_report(1, 1, &d) == 35 && d[33] == 0);
    run(500000); // however long it stays away
    assert(host_silent == 1 && new_reports() == 0);
    ptp_test_busy = false;
    FEED(0, {0, 1, 200, 100});
    run(8000);
    // It reads again: first the release of the contact it saw down.
    assert(new_reports() == 1 && count_of(last()) == 1 && flags_of(last(), 0) == 1 && x_of(last(), 0) == 100);
    FEED(0, {0, 1, 200, 100});
    run(8000);
    assert(new_reports() == 0); // the pad arms again first: no old contact
    up(0);
    FEED(0, {0, 2, 210, 100});
    run(8000);
    assert(new_reports() == 1 && x_of(last(), 0) == 210);
    puts("  host not reading: after 100 ms frames stop; the release goes out when it reads again");
}

static void test_paused(void) {
    start("paused");
    FEED(0, {0, 1, 100, 100});
    run(5000);
    new_reports();
    sval_ptp_set_paused(true); // an update starts
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1 && sval_ptp_suppress_mouse());
    FEED(0, {0, 1, 110, 100});
    run(50000);
    assert(new_reports() == 0);
    sval_ptp_set_paused(false);
    run(5000);
    up(0);
    up(1);
    run(5000);
    FEED(0, {0, 2, 120, 100});
    run(5000);
    assert(new_reports() == 1 && x_of(last(), 0) == 120);
    puts("  paused (update running): contacts lift, no reports, back after all-up");
}

// Velocity Scroll's grab: the OS sees the finger lift, gets nothing while
// grabbed, and one finger per pad is followed by the firmware.
static void test_grab(void) {
    start("grab");
    uint16_t x, y;
    FEED(0, {0, 1, 100, 200});
    run(5000);
    new_reports();
    assert(sval_ptp_grab_ready(0) && sval_ptp_grab_ready(1));
    assert(sval_ptp_grab_finger(0, &x, &y) == 0); // not grabbed
    sval_ptp_set_grab(true);
    run(5000);
    assert(new_reports() == 1 && flags_of(last(), 0) == 1 && sval_ptp_suppress_mouse()); // the OS sees a lift
    FEED(0, {0, 1, 110, 205});                                                           // the finger already down is followed at once
    run(5000);
    uint16_t a = sval_ptp_grab_finger(0, &x, &y);
    assert(a && x == 110 && y == 205);
    FEED(1, {0, 1, 300, 300}); // the other pad follows its own finger
    FEED(0, {0, 1, 150, 260}, {1, 2, 900, 900});
    run(50000);
    assert(new_reports() == 0); // nothing reaches the OS while grabbed
    uint16_t b = sval_ptp_grab_finger(1, &x, &y);
    assert(b && b != a && x == 300);
    assert(sval_ptp_grab_finger(0, &x, &y) == a && x == 150 && y == 260); // a second finger never takes over
    FEED(0, {1, 2, 910, 900});                                            // the followed finger lifts
    assert(sval_ptp_grab_finger(0, &x, &y) == 0);                         // the one still down stays unused
    up(0);
    FEED(0, {2, 3, 700, 700}); // a finger that lands later is taken
    uint16_t c = sval_ptp_grab_finger(0, &x, &y);
    assert(c && c != a && c != b && x == 700);
    flagged(0, 0x02); // palm: that finger is done
    assert(sval_ptp_grab_finger(0, &x, &y) == 0);
    sval_ptp_set_grab(false);
    FEED(0, {2, 3, 720, 700});
    run(5000);
    assert(new_reports() == 0 && sval_ptp_grab_finger(0, &x, &y) == 0); // a finger down at release never moves the pointer
    up(0);
    up(1);
    run(5000);
    FEED(0, {0, 4, 50, 50});
    run(5000);
    assert(new_reports() == 1 && x_of(last(), 0) == 50); // reports again after all-up
    puts("  grab: OS sees the lift, then nothing; one finger per pad followed; resting fingers never take over");
}

// Sval POINTING_CONTACTS (0x2F): every contact the computer has, back on its
// own pad in that pad's counts.
typedef struct {
    uint8_t  id, right, tip, conf, extra; // extra: bit 7, the computer does not have it
    uint16_t x, y;
} host_contact_t;
static uint8_t read_contacts(uint8_t *flags, host_contact_t *c) {
    uint8_t o[32];
    memset(o, 0xAA, sizeof(o));
    assert(sval_ptp_contacts(o, 10) == 0); // too short: nothing written
    assert(sval_ptp_contacts(o, sizeof(o)) == SVAL_PTP_CONTACTS_LEN);
    assert(o[0] == 1 && o[3] <= SVAL_CONTACTS_REPORT_MAX);
    *flags = o[1];
    for (uint8_t i = 0; i < o[3]; i++) {
        const uint8_t *p = o + 4 + 4 * i;
        uint32_t       w = p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16;
        c[i]             = (host_contact_t){p[0] & 15, (p[0] >> 6) & 1, (p[0] >> 4) & 1, (p[0] >> 5) & 1, p[0] >> 7, w & 0xFFF, (w >> 12) & 0xFFF};
    }
    for (uint8_t i = 4 + 4 * o[3]; i < SVAL_PTP_CONTACTS_LEN; i++)
        assert(o[i] == 0);
    return o[3];
}
static const host_contact_t *find_host(const host_contact_t *c, uint8_t n, uint8_t right, uint16_t x, uint16_t y) {
    for (uint8_t i = 0; i < n; i++)
        if (c[i].right == right && c[i].x == x && c[i].y == y) return &c[i];
    return NULL;
}

static void test_host_contacts(void) {
    host_contact_t c[SVAL_CONTACTS_REPORT_MAX];
    uint8_t        flags;
    fresh("host-contacts-off");
    assert(read_contacts(&flags, c) == 0 && flags == 6); // both pads, not reporting
    start("host-contacts");
    assert(read_contacts(&flags, c) == 0 && (flags & 1));
    // Two fingers on the left pad, one in the right pad's top-right corner.
    FEED(0, {0, 1, 100, 200}, {1, 2, 1900, 1700});
    FEED(1, {3, 1, 2047, 0});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 3);
    assert(read_contacts(&flags, c) == 3 && flags == 7);
    const host_contact_t *a = find_host(c, 3, 0, 100, 200), *b = find_host(c, 3, 0, 1900, 1700), *r = find_host(c, 3, 1, 2047, 0);
    assert(a && b && r && a->tip && b->tip && r->tip && a->conf && r->conf);
    assert(a->id != b->id && a->id != r->id && b->id != r->id);
    // The far corners of each pad come back as the pad's own extremes.
    FEED(0, {0, 1, 0, 1791}, {1, 2, 2047, 0});
    run(SVAL_PTP_MERGE_US + 1000);
    FEED(1, {3, 1, 0, 1791});
    run(SVAL_PTP_MERGE_US + 1000);
    assert(read_contacts(&flags, c) == 3);
    assert(find_host(c, 3, 0, 0, 1791) && find_host(c, 3, 0, 2047, 0) && find_host(c, 3, 1, 0, 1791));
    // One finger lifts: its last report carries it with tip off.
    FEED(0, {0, 1, 0, 1791});
    run(SVAL_PTP_MERGE_US + 1000);
    uint8_t n = read_contacts(&flags, c), down = 0;
    for (uint8_t i = 0; i < n; i++)
        down += c[i].tip;
    assert(down == 2);
    // The host stops reading: no contacts once the stream is stale.
    ptp_test_busy = true;
    for (int i = 0; i < 40; i++) {
        FEED(0, {0, 1, 10, 1791});
        FEED(1, {3, 1, 10, 1791});
        run(8000);
    }
    assert(read_contacts(&flags, c) == 0);
    ptp_test_busy = false;
    puts("  host contacts: each on its own pad in pad counts, lifts carry tip off, a stale stream reads as none");
}

// Three fingers on each pad: the touchpad reports five (Microsoft's limit),
// and 0x2F shows all six, the sixth marked as one the computer does not have.
static void test_host_contacts_six(void) {
    host_contact_t c[SVAL_CONTACTS_REPORT_MAX];
    uint8_t        flags, n;
    start("host-contacts-six");
    FEED(0, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 300, 100});
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100});
    run(5000);
    assert(new_reports() == 1 && count_of(last()) == 5);
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 300, 100}); // the sixth finger
    run(9000);
    assert(new_reports() == 1 && count_of(last()) == 5);
    assert(read_contacts(&flags, c) == 6 && flags == 7);
    uint8_t left = 0, right = 0, extra = 0;
    for (uint8_t i = 0; i < 6; i++) {
        assert(c[i].tip && c[i].conf);
        left += !c[i].right;
        right += c[i].right;
        extra += c[i].extra;
        for (uint8_t j = 0; j < i; j++)
            assert(c[i].id != c[j].id || c[i].extra != c[j].extra); // bit 7 has its own IDs
    }
    assert(left == 3 && right == 3 && extra == 1);
    const host_contact_t *sixth = find_host(c, 6, 1, 300, 100);
    assert(sixth && sixth->extra);
    // It follows the finger, though the touchpad never reports it.
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 350, 120});
    run(9000);
    assert(count_of(last()) == 5 && slot_at(last(), 350 + 2048 + SVAL_PTP_GAP, 120) < 0);
    assert(read_contacts(&flags, c) == 6 && find_host(c, 6, 1, 350, 120)->extra);
    // A reported finger lifts: the sixth stays out of the touchpad (ignored for
    // its lifetime) but stays in 0x2F; after it lifts, it is gone.
    FEED(0, {1, 2, 200, 100}, {2, 3, 300, 100});
    run(9000);
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100}, {2, 3, 360, 120});
    run(9000);
    assert(count_of(last()) == 4 && slot_at(last(), 360 + 2048 + SVAL_PTP_GAP, 120) < 0);
    n = read_contacts(&flags, c);
    assert(n == 5 && find_host(c, n, 1, 360, 120) && find_host(c, n, 1, 360, 120)->extra);
    FEED(1, {0, 1, 100, 100}, {1, 2, 200, 100});
    run(9000);
    n = read_contacts(&flags, c);
    assert(n == 4 && !find_host(c, n, 1, 360, 120));
    puts("  host contacts: three on each pad, the touchpad reports five, 0x2F all six (the sixth marked, live position, gone after it lifts)");
}

int main(int argc, char **argv) {
    if (argc > 1) ptp_trace = fopen(argv[1], "w");
    puts("native PTP host tests:");
    test_features();
    test_negotiation();
    test_one_finger();
    test_two_pads();
    test_frame_cap();
    test_cross_gap();
    test_five_contacts();
    test_short_tap();
    test_keepalive();
    test_scan_time();
    test_palm();
    test_palm_area();
    test_palm_setting();
    test_builtin_gestures();
    test_faults();
    test_stream_restart();
    test_leave_mode3();
    test_surface_switch();
    test_quick_toggle();
    test_owner_change();
    test_usb_reset();
    test_busy_endpoint();
    test_contact_ids();
    test_key_buttons();
    test_pads();
    test_dead_pad();
    test_sample_time();
    test_host_silent();
    test_paused();
    test_grab();
    test_host_contacts();
    test_host_contacts_six();
    fresh("end");
    if (ptp_trace) fclose(ptp_trace);
    printf("native PTP: %u reports checked, all passed\n", ptp_sent_count);
    return 0;
}
