// SPDX-License-Identifier: GPL-2.0-or-later
// The Azoteq pads' contact path for the native touchpad (SVAL_NATIVE_PTP; see
// keyboards/svalboard/docs/native-touchpad.md).
//
// Both halves turn each pad read into a 56-byte contact frame (stable
// lifetime IDs per slot, flags for faults). The USB half feeds its own frames
// to ptp.c at once. The other half keeps its latest frame as a snapshot and
// marks its split pointing report with a counter that moves only when its pad
// has news; the USB half fetches the snapshot over the split link when the
// counter moves, and on slow backup and heartbeat timers otherwise.
//
// The fetch rides on the Scan Lab RPC (KEYBOARD_SYNC_B) with its own request
// op, so the split message table is the one every other build has: a half
// with this firmware pairs with a half without it (keys work; the other pad
// stays a mouse), and an older half answers the op with zeros.
#ifdef SVAL_MT_HOST_TEST
#    include "mt_test_platform.h"
#else
#    include "quantum.h"
#    include "hal.h"
#    include "drivers/sensors/azoteq_iqs5xx.h"
#    include "transactions.h"
#    include "transport.h"
#    include "split_util.h"
#    include "svalboard.h"
#    include <string.h>
#    ifdef SVAL_UPDATER
#        include "updater/updater.h"
#        define mt_updater_busy() updater_active()
#    else
#        define mt_updater_busy() false
#    endif
static inline uint32_t mt_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}
// quantum/keyboard.c; not in its header.
void last_pointing_device_activity_trigger(void);
// azoteq.c: the chip's full start, with the settings written after it.
void sval_azoteq_restart(void);
#endif
#include "multitouch.h"
#include "ptp.h"
#include "../pad_gestures.h"
#ifdef SVAL_PERF_STATS
#    include "../perf.h"
#endif

#define FRAME_SIZE 56
// A pad that reported a chip reset is set up again, at most this often.
#define MT_REINIT_MIN_MS 5000
// The other half keeps the news counter in its pointing report this long
// after the USB half last asked for it ("hints ok"). An older USB half never
// asks, so it never sees a nonzero boot_x.
#define MT_HINTS_MS 1000
// How often the USB half checks which hands have a pad.
#define MT_PADS_CHECK_MS 100
// Answers with no sample, in a row, before the USB half takes a half that has
// been sending samples as having none. One such answer is far more likely a
// reply garbled or left over on the split link (QMK's RPC response carries no
// checksum) than a pad that went away, and taking it at its word lifts that
// pad's fingers.
#define MT_EMPTY_REPLIES 3

static uint8_t  ids[5], next_id, last_mask;
// The palm rejection on the mouse path: a contact above the threshold was
// seen since the pad last had no finger (sval_mt_palm()).
static bool palm_down;
static uint16_t sequence;
static uint32_t epoch, samples, errors;
static uint32_t read_timeouts, read_errors, end_timeouts, end_errors, max_read_ms;
static uint32_t retry_attempts, retry_recovered, reinits;
static bool     reinit_pending, reinit_done;
static uint32_t reinit_at;
static uint8_t  touch_owner = SVAL_TOUCH_OWNER_DEFAULT; // RAM only: the build default returns at power-up
// Hands with a pad (bit 0 left, bit 1 right), from both halves' identity, and
// the pads the touchpad has: this half's, and the other half's once it has
// answered with a snapshot. An other half that cannot send one (older
// firmware) keeps its pad as a mouse: it is never suppressed.
static uint8_t  pad_hands, pad_mask;
static uint32_t pads_checked;

// The half without USB: its snapshot and news counter.
static uint8_t  snapshot[FRAME_SIZE];
static bool     snapshot_valid;
static uint32_t snapshot_at, hints_until;
static bool     hints_asked;
static uint8_t  split_hint = 0x80; // bit 7 set; the low bits move only on news

// The USB half: the other half's pad.
static uint8_t  remote_hint, fetched_hint, remote_seq, remote_epoch;
static uint32_t poll_at, received_at;
static bool     remote_seen, remote_lost, remote_hints, remote_empty, last_ok;
static uint8_t  empty_replies; // answers with no sample in a row (MT_EMPTY_REPLIES)

static void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
static uint16_t get16(const uint8_t *p) {
    return p[0] | (uint16_t)p[1] << 8;
}
static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

// ------------------------------------------------------------ pad reads

void sval_mt_read_status(i2c_status_t read_status, i2c_status_t end_status, uint32_t elapsed_ms) {
    if (read_status == I2C_STATUS_TIMEOUT)
        read_timeouts++;
    else if (read_status != I2C_STATUS_SUCCESS)
        read_errors++;
    if (end_status == I2C_STATUS_TIMEOUT)
        end_timeouts++;
    else if (end_status != I2C_STATUS_SUCCESS)
        end_errors++;
    if (elapsed_ms > max_read_ms) max_read_ms = elapsed_ms;
}

// The error bits of the last failed transfer. The RP2040 driver reports an
// address or data NACK as a failure with no bits set; arbitration loss and
// overrun set bits, and a timeout has its own status. i2cStop() keeps them.
uint32_t sval_mt_i2c_error_flags(void) {
    return i2cGetErrors(&I2C_DRIVER);
}

void sval_mt_acquisition_fault(uint32_t flags, bool retried, bool recovered) {
    (void)flags;
    if (retried) retry_attempts++;
    if (recovered) retry_recovered++;
}

static void reset_stream(void) {
    last_mask = next_id = 0;
    memset(ids, 0, sizeof(ids));
    epoch++;
}

// One pad read (drivers/sensors/azoteq_iqs5xx.c): raw is the 45 bytes from
// register 0x000C, valid when the read and its end of session succeeded.
void sval_mt_sample(const uint8_t *raw, bool valid) {
#ifndef SVAL_MT_HOST_TEST
    sval_touch_sample(raw, valid); // the touch page (touch_stream.c); only while a host reads it
#endif
    samples++;
    uint8_t       flags = 0, mask = 0;
    const uint8_t prev_mask = last_mask;
    if (!valid) {
        flags |= 4;
        errors++;
    } else {
        // Palm rejection (pad_gestures.h): latched from the first contact
        // above the threshold until every finger lifts, as the chip latches
        // its own palm detect.
        if (!raw[5]) {
            palm_down = false;
        } else if (raw[5] <= 5) {
            const uint8_t palm_at = sval_palm_area();
            for (uint8_t i = 0; i < 5; i++) {
                const uint8_t *p = raw + 10 + 7 * i;
                if ((p[4] || p[5]) && sval_pad_is_palm(palm_at, p[6])) palm_down = true;
            }
        }
        if (raw[4] & 2) flags |= 1;    // palm
        if (raw[4] & 4) flags |= 2;    // too many fingers
        if (raw[3] & 0x98) flags |= 8; // reset, re-ATI, ATI error
        if (raw[5] > 5) flags |= 4;
        // SHOW_RESET, acknowledged at init: the chip reset since and lost its
        // settings (report period, contacts, resolution). Set it up again.
        if (raw[3] & 0x80) reinit_pending = true;
        if (!flags && raw[5]) {
            for (uint8_t i = 0; i < 5; i++) {
                const uint8_t *p = raw + 10 + 7 * i;
                if (p[4] || p[5]) mask |= 1 << i;
            }
        }
    }
    const bool master = is_keyboard_master();
    if (master && !sval_ptp_wants_frames()) return;
    if (flags) reset_stream();
    // Restart before an 8-bit lifetime ID can wrap ambiguously.
    if (next_id > 249 && (mask & ~last_mask)) {
        reset_stream();
        flags |= 0x10;
    }
    uint8_t frame[FRAME_SIZE];
    memset(frame, 0, FRAME_SIZE);
    frame[0] = 1;
    frame[1] = is_keyboard_left() ? 0 : 1;
    frame[2] = flags;
    frame[3] = mask;
    put16(frame + 4, sequence++);
    put16(frame + 6, FRAME_SIZE);
    put32(frame + 8, timer_read32() * 1000u);
    put32(frame + 12, epoch);
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t *p   = raw + 10 + 7 * i;
        uint8_t       *out = frame + 16 + 8 * i;
        if ((mask & (1 << i)) && !(last_mask & (1 << i))) ids[i] = ++next_id;
        out[0] = ids[i];
        out[1] = p[1];
        out[2] = p[0];
        out[3] = p[3];
        out[4] = p[2];
        out[5] = p[5];
        out[6] = p[4];
        out[7] = p[6];
    }
    last_mask = mask;
    if (!master) {
        // The RPC callback only copies this complete frame; no I2C in the
        // split transport's callback. This short critical section is bounded.
        osalSysLock();
        memcpy(snapshot, frame, FRAME_SIZE);
        snapshot_at    = timer_read32();
        snapshot_valid = true;
        osalSysUnlock();
        // News is a contact, a lift or any flag; a repeat of a valid all-up
        // sample is not, so an idle pad never moves the counter.
        if (flags || mask || prev_mask) split_hint = 0x80 | ((split_hint + 1) & 0x7F);
        return;
    }
#ifdef SVAL_PERF_STATS
    uint32_t feed_started = mt_now_us();
    sval_ptp_feed(frame, feed_started);
    sval_perf_section(SVAL_PERF_SEC_SAMPLE, mt_now_us() - feed_started);
#else
    sval_ptp_feed(frame, mt_now_us());
#endif
}

// ---------------------------------------------------- the split link

// Half without USB: the counter for its split pointing report, or 0 while
// no USB half has asked for it.
uint8_t sval_mt_split_hint(void) {
    if (!hints_asked || (int32_t)(timer_read32() - hints_until) >= 0) return 0;
    return split_hint;
}

// USB half: the other half's counter, taken off its pointing report
// (svalboard.c). Bit 7 marks a counter; 0 means none.
//
// The split link sends the pointing report only when its checksum changes,
// so a counter change with an unchanged crc8 (1 in 256) is seen late; the
// backup fetch while contacts are down bounds that to SVAL_PTP_REMOTE_BACKUP_MS.
void sval_mt_remote_hint(uint8_t hint) {
    if (hint & 0x80) remote_hint = hint;
}

// The snapshot reply, SVAL_MT_RPC_REPLY bytes: op, status (bit 0 a sample is
// held, bit 1 hints on), age in ms (saturating), flags, mask, sequence and
// epoch (low bytes), then per slot the lifetime ID and a 32-bit word
// x | y << 11 | area << 22 (x 0..2047, y 0..1791).
static void pack_snapshot(uint8_t *out) {
    memset(out, 0, SVAL_MT_RPC_REPLY);
    out[0] = SVAL_MT_RPC_SNAPSHOT;
    osalSysLock();
    out[1]       = (snapshot_valid ? 1 : 0) | 2;
    uint32_t age = timer_elapsed32(snapshot_at);
    out[2]       = age > 255 ? 255 : (uint8_t)age;
    out[3]       = snapshot[2] & 0x7F;
    out[4]       = snapshot[3] & 0x1F;
    out[5]       = snapshot[4];
    out[6]       = snapshot[12];
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t *s = snapshot + 16 + 8 * i;
        uint8_t       *o = out + 7 + 5 * i;
        o[0]             = s[0];
        put32(o + 1, (get16(s + 1) & 0x7FFu) | (uint32_t)(get16(s + 3) & 0x7FFu) << 11 | (uint32_t)s[7] << 22);
    }
    osalSysUnlock();
}

// The reply back into a 56-byte frame for ptp.c.
static void unpack_snapshot(const uint8_t *r, uint8_t pad, uint8_t *frame) {
    memset(frame, 0, FRAME_SIZE);
    frame[0] = 1;
    frame[1] = pad;
    frame[2] = r[3] & 0x7F;
    frame[3] = r[4] & 0x1F;
    put16(frame + 4, r[5]);
    put16(frame + 6, FRAME_SIZE);
    put32(frame + 12, r[6]);
    for (uint8_t i = 0; i < 5; i++) {
        const uint8_t *o = r + 7 + 5 * i;
        uint8_t       *s = frame + 16 + 8 * i;
        uint32_t       w = get32(o + 1);
        s[0]             = o[0];
        put16(s + 1, w & 0x7FF);
        put16(s + 3, (w >> 11) & 0x7FF);
        s[7] = (w >> 22) & 0xFF;
    }
}

// Half without USB: a request from the USB half (svalboard.c routes the ops
// here from the Scan Lab RPC). Runs in the split transport's callback: no I2C,
// no waiting. True when the op was one of these.
bool sval_mt_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 2 || out_len < SVAL_MT_RPC_REPLY) return false;
    if (in[0] == SVAL_MT_RPC_SNAPSHOT) {
        if (in[1] & 1) { // the USB half reads the news counter
            hints_until = timer_read32() + MT_HINTS_MS;
            hints_asked = true;
        }
        pack_snapshot(out);
        return true;
    }
    return false;
}

static bool remote_rpc(const uint8_t *req, uint8_t req_len, uint8_t *reply) {
    if (mt_updater_busy() || !is_transport_connected()) return false;
#ifdef SVAL_PERF_STATS
    uint8_t perf_prev = sval_perf_enter(SVAL_PERF_GAP_FETCH);
    bool    ok        = transaction_rpc_exec(KEYBOARD_SYNC_B, req_len, req, SVAL_MT_RPC_REPLY, reply);
    sval_perf_leave(perf_prev);
    return ok;
#else
    return transaction_rpc_exec(KEYBOARD_SYNC_B, req_len, req, SVAL_MT_RPC_REPLY, reply);
#endif
}

static void remote_gone(uint8_t pad) {
    if (remote_seen && !remote_lost) {
        sval_ptp_pad_lost(pad);
        remote_lost = true;
    }
}

static void update_pads(void) {
    const uint8_t local = is_keyboard_left() ? 1 : 2;
    // The other half's pad rides the split link: hold its contacts across
    // failed transactions (ptp.h, SVAL_PTP_REMOTE_STALE_MS).
    sval_ptp_set_stale_ms(local == 1 ? 1 : 0, SVAL_PTP_REMOTE_STALE_MS);
    uint8_t mask = pad_hands & (local | ((remote_seen && !remote_empty) ? 3 - local : 0));
    if (mask != pad_mask) {
        pad_mask = mask;
        sval_ptp_set_pads(mask);
    }
}

// Fetch the other half's snapshot only when it can hold something new: at
// once on news, every BACKUP ms while its pad has contacts or is not yet armed
// (covers a lost counter change and a dead link), and every IDLE ms while it
// is quiet or has no snapshot. Until the first answer says the counter is on,
// every POLL ms.
static void poll_remote(void) {
    const uint8_t pad = is_keyboard_left() ? 1 : 0;
    if (!(pad_hands & (1u << pad))) { // no pad there: nothing to fetch
        remote_seen = remote_lost = remote_hints = remote_empty = false;
        return;
    }
    if (mt_updater_busy()) { // the update owns the split link: lift, fetch nothing
        remote_gone(pad);
        return;
    }
    uint32_t since = timer_elapsed32(poll_at);
    bool     quiet = remote_empty || (remote_hints && remote_seen && !remote_lost && sval_ptp_pad_quiet(pad));
    bool     news  = remote_hints && remote_hint != fetched_hint;
    bool     due;
    if (news)
        due = last_ok || since >= SVAL_PTP_REMOTE_POLL_MS; // no retry storm on a dead link
    else if (!remote_hints && !remote_empty)
        due = since >= SVAL_PTP_REMOTE_POLL_MS;
    else
        due = since >= (quiet ? SVAL_PTP_REMOTE_IDLE_MS : SVAL_PTP_REMOTE_BACKUP_MS);
    if (!due) return;
    poll_at              = timer_read32();
    const uint8_t want   = remote_hint;
    uint8_t       req[2] = {SVAL_MT_RPC_SNAPSHOT, 1}, reply[SVAL_MT_RPC_REPLY] = {0};
#ifdef SVAL_PERF_STATS
    uint32_t rpc_started = sval_perf_now_us();
#endif
    bool ok    = remote_rpc(req, sizeof(req), reply);
    bool fresh = false;
    last_ok    = ok;
    if (ok && reply[0] == SVAL_MT_RPC_SNAPSHOT && (reply[1] & 1) && reply[2] <= SVAL_PTP_STALE_MS) {
        empty_replies = 0;
        remote_hints  = reply[1] & 2;
        remote_empty  = false;
        fetched_hint  = want;
        received_at   = timer_read32();
        if (!remote_seen || remote_lost || reply[5] != remote_seq || reply[6] != remote_epoch) {
            uint8_t frame[FRAME_SIZE];
            unpack_snapshot(reply, pad, frame);
            sval_ptp_feed(frame, mt_now_us() - reply[2] * 1000u);
            fresh = true;
        }
        remote_seq   = reply[5];
        remote_epoch = reply[6];
        remote_seen  = true;
        remote_lost  = false;
        update_pads();
#ifdef SVAL_PERF_STATS
        sval_perf_rpc(sval_perf_now_us() - rpc_started, true, fresh);
        if (news) sval_perf_rpc_hint();
#endif
        return;
    }
#ifdef SVAL_PERF_STATS
    sval_perf_rpc(sval_perf_now_us() - rpc_started, ok, false);
#endif
    bool empty = ok && (reply[0] != SVAL_MT_RPC_SNAPSHOT || !(reply[1] & 1));
    if (empty && remote_seen && !remote_lost && !remote_empty && ++empty_replies < MT_EMPTY_REPLIES) {
        empty = false; // a half that was sending samples: a failed fetch until it repeats
        ok    = false;
    }
    if (empty) {
        // An answer with no sample: firmware without this op (all zero), or a
        // pad that has not been read yet. Treat it as no pad: heartbeat only.
        empty_replies = 0;
        remote_empty  = true;
        remote_hints  = reply[0] == SVAL_MT_RPC_SNAPSHOT && (reply[1] & 2);
        fetched_hint  = want;
        remote_gone(pad);
        update_pads();
        return;
    }
    (void)fresh;
    // A failed fetch, or a snapshot too old to use (the other pad stopped
    // answering). A quiet pad's last good snapshot is legitimately a
    // heartbeat old, so it gets the heartbeat on top.
    uint32_t stale = quiet ? SVAL_PTP_REMOTE_IDLE_MS + SVAL_PTP_REMOTE_STALE_MS : SVAL_PTP_REMOTE_STALE_MS;
    if (remote_seen && timer_elapsed32(received_at) > stale) remote_gone(pad);
}

// --------------------------------------------------------- dev command

#ifdef SVAL_TOUCH_DEV
// Raw HID 0xFC op 0x42 (svalboard.c routes it here; dev builds only, never a
// released protocol): get or set the pads' owner. data[7] 0 get, 1 set; data[8]
// the owner (ptp.h). Refusals keep the old owner. Reply: 0xC2, status, owner,
// built flags, ptp.c status (24 bytes), then this file's counters.
// data[7] 2: hold Velocity Scroll (data[8] 1) or let it go (0) with no key,
// for keyless test boards; a hold ends unless repeated within 500 ms
// (tools/multitouch/velocity_hold.py).
void velocity_scroll_dev_hold(bool on); // keymaps/keymap_support.c
void sval_mt_command(uint8_t *data, uint8_t length) {
    uint8_t action = data[7], wanted = data[8], status = 0;
    if (action == 2) {
        velocity_scroll_dev_hold(wanted != 0);
    } else if (action == 1 && wanted != touch_owner) {
        if (wanted != SVAL_TOUCH_OWNER_MOUSE && wanted != SVAL_TOUCH_OWNER_NATIVE) {
            status = 2; // host injection is retired and the detector is not built
        } else {
            sval_ptp_set_native(wanted == SVAL_TOUCH_OWNER_NATIVE);
            touch_owner = wanted;
        }
    } else if (action > 1) {
        status = 2;
    }
    memset(data + 6, 0, length - 6);
    data[6] = 0xC2;
    data[7] = status;
    data[8] = touch_owner;
    data[9] = 1 | 2 | 4 | 8; // bit 0 native touchpad built, bit 1 status v2 (pads, link), bit 2 v3 (lost fingers), bit 3 action 2
    sval_ptp_status(data + 10);
    uint8_t *o = data + 10 + SVAL_PTP_STATUS_SIZE;
    put32(o, samples);
    put32(o + 4, errors);
    put32(o + 8, read_timeouts + read_errors);
    put16(o + 12, retry_attempts > 0xFFFF ? 0xFFFF : retry_attempts);
    put16(o + 14, retry_recovered > 0xFFFF ? 0xFFFF : retry_recovered);
    put16(o + 16, reinits > 0xFFFF ? 0xFFFF : reinits);
    o[18] = (remote_hints ? 1 : 0) | (remote_empty ? 2 : 0) | (remote_seen ? 4 : 0) | (remote_lost ? 8 : 0);
    o[19] = sval_ptp_lost_down(0); // fingers lifted because their pad's samples stopped
    o[20] = sval_ptp_lost_down(1);
}
#endif

bool sval_mt_palm(void) {
    return palm_down && sval_palm_area();
}

bool sval_pad_palm_supported(void) {
    return true;
}

// ------------------------------------------------------------ the task

void sval_mt_init(void) {
    sval_ptp_set_native(touch_owner == SVAL_TOUCH_OWNER_NATIVE);
}

// Hands with a pad, from both halves' pointing identity (svalboard.c).
static void check_pads(void) {
    if (pads_checked && timer_elapsed32(pads_checked) < MT_PADS_CHECK_MS) return;
    pads_checked = timer_read32() | 1;
    pad_hands    = sval_pad_hands();
    update_pads();
}

void sval_mt_task(void) {
    // A chip that reset is set up again (on either half), never in a loop.
    if (reinit_pending && !mt_updater_busy() && (!reinit_done || timer_elapsed32(reinit_at) >= MT_REINIT_MIN_MS)) {
        reinit_pending = false;
        reinit_done    = true;
        reinit_at      = timer_read32();
        reinits++;
        sval_azoteq_restart();
    }
    if (!is_keyboard_master()) return;
    check_pads();
    sval_ptp_set_paused(mt_updater_busy());
    // The other half's pad is fetched while the touchpad wants frames, and,
    // to find out that it can be part of it, whenever the owner is native.
    const uint8_t remote_hand = is_keyboard_left() ? 2 : 1;
    if (sval_ptp_wants_frames() || (touch_owner == SVAL_TOUCH_OWNER_NATIVE && (pad_hands & remote_hand))) poll_remote();
#ifdef SVAL_PERF_STATS
    uint32_t ptp_started = mt_now_us();
    sval_ptp_task();
    sval_perf_section(SVAL_PERF_SEC_PTP_TASK, mt_now_us() - ptp_started);
#else
    sval_ptp_task();
#endif
    // Touchpad use is input activity: the matrix and pointer idle stages wait.
    if (sval_ptp_reporting() && sval_ptp_contacts_down()) last_pointing_device_activity_trigger();
}

// The pads' mouse output for this hand (0 left, 1 right) must be zero: the
// native touchpad has it. A hand without a pad is never suppressed.
bool sval_mt_suppress_hand(uint8_t pad) {
    return is_keyboard_master() && (pad_mask & (1u << (pad & 1))) && sval_ptp_suppress_mouse();
}

// The OS drives this hand's pad as a touchpad (Sval 0x2C identity flag).
bool sval_mt_os_touchpad(uint8_t pad) {
    return is_keyboard_master() && (pad_mask & (1u << (pad & 1))) && sval_ptp_reporting();
}
