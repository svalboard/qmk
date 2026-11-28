// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in timing counters (SVAL_PERF_STATS=yes, dev builds only; release
// builds refuse them). Kept on each half and read over raw HID 0xFC op 0x43,
// the other half's through the split link. Host side:
// keyboards/svalboard/tools/multitouch/perf.py. Times are in microseconds
// from the 1 MHz system timer. The sums are 32-bit: a window must be shorter
// than 71 minutes (perf.py refuses longer ones).
//
// They work with and without the native touchpad, so a build without it can
// be measured the same way. Without it, the pad read is timed around the
// whole driver call (read and end of session) instead of the read alone.
#include "quantum.h"
#include "svalboard.h"
#include "perf.h"
#include "transactions.h"
#include "transport.h"
#include "split_util.h"
#include <string.h>
#ifdef SVAL_NATIVE_PTP
#    include "azoteq/ptp.h"
// The pad read schedule (azoteq/azoteq.c): its lead, bare transfer and last stretch.
void sval_pad_sched_stats(uint32_t *lead_us, uint32_t *xfer_us, uint32_t *wait_us);
#endif
#include "updater/updater_api.h"
#ifdef SVAL_UPDATE_RELEASE
#    error "release builds cannot have SVAL_PERF_STATS"
#endif

typedef struct {
    uint32_t     since_ms, last_loop, last_scan;
    uint32_t     loops, loop_sum, loop_max, loop_hist[SVAL_PERF_BUCKETS];
    uint32_t     scans, scan_sum, scan_max, scan_hist[SVAL_PERF_BUCKETS];
    uint32_t     i2c_n, i2c_sum, i2c_max, i2c_fail, i2c_hist[SVAL_PERF_BUCKETS];
    uint32_t     rpc_n, rpc_fail, rpc_sum, rpc_max, rpc_new, rpc_hint;
    uint32_t     raw_down, raw_changes; // raw matrix frames with a key down; raw changes
    uint32_t     sec_sum[SVAL_PERF_SECTIONS], sec_max[SVAL_PERF_SECTIONS];
    // The scan gap now running, by part, and the slowest one so far.
    uint32_t     gap_at, gap_part[SVAL_PERF_GAP_PARTS], gap_passes, gap_pass_max, gap_reads, gap_fetches;
    uint32_t     worst_gap, worst_part[SVAL_PERF_GAP_PARTS], worst_passes, worst_pass_max, worst_at_ms, worst_reads, worst_fetches;
    bool         raw_seen;
    matrix_row_t raw_last[SVAL_PERF_MATRIX_ROWS];
} perf_t;

static perf_t s;
// The part running now, since when, and the part a matrix scan interrupted.
// Not reset with the counters: a reset can come in the middle of a part.
static uint8_t  phase = SVAL_PERF_GAP_OTHER, scan_prev = SVAL_PERF_GAP_OTHER;
static uint32_t phase_at;

// Kept in the ELF so check_release_elf.py finds it.
__attribute__((used)) const uint8_t sval_perf_stats_build = 1;

uint32_t sval_perf_now_us(void) {
    return (uint32_t)chVTGetSystemTimeX();
}

// <1, 1-2, 2-4, 4-8, 8-16, >=16 ms
static uint8_t bucket(uint32_t us) {
    uint8_t b = 0;
    for (uint32_t edge = 1000; b < SVAL_PERF_BUCKETS - 1 && us >= edge; edge <<= 1)
        b++;
    return b;
}

static void add(uint32_t us, uint32_t *n, uint32_t *sum, uint32_t *max, uint32_t *hist) {
    (*n)++;
    *sum += us;
    if (us > *max) *max = us;
    if (hist) hist[bucket(us)]++;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = v;
    p[1] = v >> 8;
    p[2] = v >> 16;
    p[3] = v >> 24;
}
static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void sval_perf_reset(void) {
    memset(&s, 0, sizeof(s));
    s.since_ms = timer_read32();
    phase_at   = sval_perf_now_us();
}

// Charges the time since the last change to the part running.
static void charge(uint32_t now) {
    s.gap_part[phase] += now - phase_at;
    phase_at = now;
}

uint8_t sval_perf_enter(uint8_t part) {
    uint8_t prev = phase;
    charge(sval_perf_now_us());
    phase = part < SVAL_PERF_GAP_PARTS ? part : SVAL_PERF_GAP_OTHER;
    if (part == SVAL_PERF_GAP_PAD) s.gap_reads++;
    if (part == SVAL_PERF_GAP_FETCH) s.gap_fetches++;
    return prev;
}

void sval_perf_leave(uint8_t prev) {
    charge(sval_perf_now_us());
    phase = prev;
}

void sval_perf_loop(void) {
    uint32_t now = sval_perf_now_us();
    if (s.last_loop) {
        add(now - s.last_loop, &s.loops, &s.loop_sum, &s.loop_max, s.loop_hist);
        if (now - s.last_loop > s.gap_pass_max) s.gap_pass_max = now - s.last_loop;
    }
    s.gap_passes++;
    s.last_loop = now;
}

// A matrix scan starts: the last scan gap ends here. Its parts are summed on
// the same clock as the phases, so they add up to it exactly.
void sval_perf_scan(uint32_t now) {
    if (s.last_scan) add(now - s.last_scan, &s.scans, &s.scan_sum, &s.scan_max, s.scan_hist);
    s.last_scan = now;
    uint32_t t = sval_perf_now_us();
    charge(t);
    if (s.gap_at && t - s.gap_at > s.worst_gap) {
        s.worst_gap = t - s.gap_at;
        memcpy(s.worst_part, s.gap_part, sizeof(s.gap_part));
        s.worst_passes   = s.gap_passes;
        s.worst_pass_max = s.gap_pass_max;
        s.worst_at_ms    = timer_elapsed32(s.since_ms);
        s.worst_reads    = s.gap_reads;
        s.worst_fetches  = s.gap_fetches;
    }
    memset(s.gap_part, 0, sizeof(s.gap_part));
    s.gap_at     = t;
    s.gap_passes = s.gap_pass_max = s.gap_reads = s.gap_fetches = 0;
    scan_prev    = phase;
    phase        = SVAL_PERF_GAP_MATRIX;
}

// The scan's row reads are done (matrix.c): back to what the scan interrupted.
void sval_perf_matrix(const matrix_row_t *rows, uint8_t count) {
    sval_perf_leave(scan_prev);
    if (count > SVAL_PERF_MATRIX_ROWS) count = SVAL_PERF_MATRIX_ROWS;
    bool down = false;
    for (uint8_t i = 0; i < count; i++)
        down |= rows[i] != 0;
    if (down) s.raw_down++;
    if (s.raw_seen && memcmp(rows, s.raw_last, count * sizeof(matrix_row_t))) s.raw_changes++;
    memcpy(s.raw_last, rows, count * sizeof(matrix_row_t));
    s.raw_seen = true;
}

void sval_perf_section(uint8_t section, uint32_t us) {
    if (section >= SVAL_PERF_SECTIONS) return;
    s.sec_sum[section] += us;
    if (us > s.sec_max[section]) s.sec_max[section] = us;
}

void sval_perf_i2c(uint32_t us, bool ok) {
    add(us, &s.i2c_n, &s.i2c_sum, &s.i2c_max, s.i2c_hist);
    if (!ok) s.i2c_fail++;
}

void sval_perf_rpc(uint32_t us, bool ok, bool fresh) {
    add(us, &s.rpc_n, &s.rpc_sum, &s.rpc_max, NULL);
    if (!ok) s.rpc_fail++;
    if (fresh) s.rpc_new++;
}

void sval_perf_rpc_hint(void) {
    s.rpc_hint++;
}

// Six pages of 13 counters; layout in tools/multitouch/perf.py.
bool sval_perf_page(uint8_t page, uint32_t out[SVAL_PERF_PAGE_WORDS]) {
    memset(out, 0, SVAL_PERF_PAGE_WORDS * sizeof(uint32_t));
    uint32_t frames = 0, busy = 0, lead = 0, xfer = 0, wait = 0, pads = 0, merge_waits = 0, host_silent = 0;
#ifdef SVAL_NATIVE_PTP
    uint8_t st[SVAL_PTP_STATUS_SIZE];
    sval_ptp_status(st);
    frames      = get32(st + 6);
    busy        = get32(st + 10);
    pads        = st[16];
    host_silent = st[18] | (uint32_t)st[19] << 8;
    merge_waits = st[20] | (uint32_t)st[21] << 8;
    sval_pad_sched_stats(&lead, &xfer, &wait);
#endif
    switch (page) {
        case 0: {
            const uint32_t v[] = {timer_elapsed32(s.since_ms), s.loops, s.loop_sum, s.loop_max, s.scans, s.scan_sum, s.scan_max, s.i2c_n, s.i2c_sum, s.i2c_max, s.i2c_fail, s.rpc_n, s.rpc_fail};
            memcpy(out, v, sizeof(v));
            return true;
        }
        case 1: {
            const uint32_t v[] = {s.rpc_sum, s.rpc_max, s.rpc_new, frames, busy};
            memcpy(out, v, sizeof(v));
            memcpy(out + 5, s.loop_hist, sizeof(s.loop_hist));
            out[11] = lead;
            out[12] = xfer;
            return true;
        }
        case 2:
            memcpy(out, s.i2c_hist, sizeof(s.i2c_hist));
            memcpy(out + SVAL_PERF_BUCKETS, s.scan_hist, sizeof(s.scan_hist));
            out[12] = s.rpc_hint;
            return true;
        case 3: {
            // The scan settings in effect, and what the raw matrix saw.
            uint16_t pre, post;
            uint8_t  stage;
            sval_scan_timing(&pre, &post);
            uint32_t period = sval_scan_period_now(&stage);
#ifdef SVAL_NATIVE_PTP
            const uint32_t build = 1;
#else
            const uint32_t build = 0;
#endif
            const uint32_t v[] = {global_saved_values.scan_period_us, period, pre, post, sval_timing_preset(pre, post), stage, sval_hw_rev_is_flipfet(), s.raw_down, s.raw_changes, wait, pads | build << 8, merge_waits, host_silent};
            memcpy(out, v, sizeof(v));
            return true;
        }
        case 4: // the last raw matrix frame, one row a word; then time per section
            for (uint8_t i = 0; i < SVAL_PERF_MATRIX_ROWS; i++)
                out[i] = s.raw_last[i];
            for (uint8_t i = 0; i < SVAL_PERF_SECTIONS; i++) {
                out[5 + 2 * i] = s.sec_sum[i];
                out[6 + 2 * i] = s.sec_max[i];
            }
            return true;
        case 5: // the slowest scan gap: its length, its parts, where it fell
            _Static_assert(SVAL_PERF_GAP_PARTS == 7, "page 5 layout (perf.py)");
            out[0] = s.worst_gap;
            memcpy(out + 1, s.worst_part, sizeof(s.worst_part));
            out[8]  = s.worst_passes;
            out[9]  = s.worst_pass_max;
            out[10] = s.worst_at_ms;
            out[11] = s.worst_reads;
            out[12] = s.worst_fetches;
            return true;
    }
    return false;
}

// Half without USB: the USB half asks for a page (in two parts) or a reset.
// Runs in the split transport's callback; only copies.
bool sval_perf_split_rpc(uint8_t in_len, const uint8_t *in, uint8_t out_len, uint8_t *out) {
    if (in_len < 3 || in[0] != SVAL_PERF_RPC_OP || out_len < SVAL_PERF_RPC_REPLY) return false;
    uint32_t words[SVAL_PERF_PAGE_WORDS];
    memset(out, 0, SVAL_PERF_RPC_REPLY);
    out[0] = SVAL_PERF_RPC_OP;
    out[2] = in[1];
    out[3] = in[2];
    if (in[1] == 0x80) {
        sval_perf_reset();
        out[1] = 1;
    } else if (in[2] < 2 && sval_perf_page(in[1], words)) {
        out[1]        = 1;
        uint8_t first = in[2] * 7;
        for (uint8_t i = 0; i < 7 && first + i < SVAL_PERF_PAGE_WORDS; i++)
            put32(out + 4 + 4 * i, words[first + i]);
    }
    return true;
}

static bool remote(uint8_t page, uint8_t part, uint8_t *reply) {
    uint8_t req[3] = {SVAL_PERF_RPC_OP, page, part};
    if (updater_busy() || !is_transport_connected()) return false;
    uint8_t prev = sval_perf_enter(SVAL_PERF_GAP_COUNTERS);
    bool    ok   = transaction_rpc_exec(KEYBOARD_SYNC_B, sizeof(req), req, SVAL_PERF_RPC_REPLY, reply) && reply[0] == SVAL_PERF_RPC_OP && reply[1];
    sval_perf_leave(prev);
    return ok;
}

// Raw HID 0xFC op 0x43. data[7] 0 this half, 1 the other half, 2 reset both;
// data[8] page. Reply: 0xC3, status, page, target, native touchpad built,
// 0xA5, then 13 little-endian u32 from data[12].
void sval_perf_command(uint8_t *data, uint8_t length) {
    uint8_t  target = data[7], page = data[8];
    uint32_t words[SVAL_PERF_PAGE_WORDS] = {0};
    memset(data + 6, 0, length - 6);
    data[6] = 0xC3;
    data[8] = page;
    data[9] = target;
#ifdef SVAL_NATIVE_PTP
    data[10] = 1;
#endif
    data[11] = 0xA5; // marks a counters reply
    if (target == 0) {
        data[7] = sval_perf_page(page, words) ? 0 : 2;
    } else if (target == 1) {
        bool ok = true;
        for (uint8_t part = 0; part < 2 && ok; part++) {
            uint8_t reply[SVAL_PERF_RPC_REPLY] = {0};
            ok                                 = remote(page, part, reply);
            for (uint8_t i = 0; ok && i < 7 && part * 7 + i < SVAL_PERF_PAGE_WORDS; i++)
                words[part * 7 + i] = get32(reply + 4 + 4 * i);
        }
        data[7] = ok ? 0 : 1;
    } else if (target == 2) {
        uint8_t reply[SVAL_PERF_RPC_REPLY] = {0};
        data[7]                            = remote(0x80, 0, reply) ? 0 : 1;
        sval_perf_reset();
    } else {
        data[7] = 2;
    }
    for (uint8_t i = 0; i < SVAL_PERF_PAGE_WORDS; i++)
        put32(data + 12 + 4 * i, words[i]);
}
