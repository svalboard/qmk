// Copyright 2026 Morgan Venable
// SPDX-License-Identifier: GPL-2.0-or-later

// Host tests for the updater's session state machine (updater.c) and the
// confirmation chord (update_gesture.c), with simulated time. Included by
// test_updater.c after its flash mock, whose ROM operations advance sim_us.
//
// Each updater_task() call is one main-loop pass; the test adds 1 ms of other
// work between passes. "Every op answers within 50 ms" is checked as: the
// longest single pass plus the longest op handler, in simulated flash time,
// stays at or under 50 ms.

#include "updater.h"
#include "updater_port.h"
#include "update_gesture.h"
#include "update_led.h"
#include "update_commit.h"
#include "update_split.h"

void updater_host_reset(void);
void update_split_host_slave_reset(void);

// ---- port and mocks ----------------------------------------------------------------------

static uint32_t        port_client; // 0: the packet came without the client wrapper
static bool            port_failing;
static bool            port_scan_override; // a Scan Lab sweep is running
static uint32_t        port_rand = 0x2545F491u;
static update_device_t self_dev;

uint32_t updater_port_now_ms(void) {
    return (uint32_t)(sim_us / 1000);
}
bool updater_port_client(uint32_t *id) {
    if (!port_client) return false;
    *id = port_client;
    return true;
}
bool updater_port_settings_failing(void) {
    return port_failing;
}
uint32_t updater_port_random32(void) {
    port_rand = port_rand * 1664525u + 1013904223u;
    return port_rand ? port_rand : 1;
}
// The split simulation (M2, below) runs both halves in this one process:
// sim_slave says whose code is running.
static bool            sim_slave;
static update_device_t slave_dev;
void                   update_device_self(update_device_t *dev) {
    *dev = sim_slave ? slave_dev : self_dev;
}
bool updater_port_scan_override(void) {
    return port_scan_override;
}
static uint8_t port_split_presence; // update_presence_status_t
static bool    presence_sim;        // presence from the simulated pings (update_split.c) instead
uint8_t        updater_port_split_presence(void) {
    return presence_sim ? update_split_presence() : port_split_presence;
}
#define HOST_STACK_SIZE 0xAE0
#define HOST_STACK_FREE 0x2A4
uint16_t updater_port_stack_free(uint16_t *size) {
    *size = HOST_STACK_SIZE;
    return HOST_STACK_FREE;
}

static update_led_mode_t led_now;
static int               led_seen; // bit per mode shown
void                     update_led_show(update_led_mode_t mode) {
    led_now = mode;
    led_seen |= 1 << mode;
}

// A mock commit, so the COMMITTING path can be tested on its own. The real
// update_commit.c is tested by test_commit.c, in the build of this harness
// with -DSVAL_TEST_REAL_COMMIT.
static bool            commit_avail;
static int             commit_runs;
static uint32_t        commit_crc, commit_len;
static update_status_t commit_result;
// The other half's commit, in the split simulation: either it refuses with
// slave_commit_result, or (slave_commit_resets) it "resets into the new image":
// the simulation reboots that half with the manifest's fw_version. The same
// for this half's commit with master_commit_resets (sval_update.py pair).
static int             slave_commit_runs;
static uint32_t        slave_commit_crc, slave_commit_len;
static update_status_t slave_commit_result = UPDATE_UNSUPPORTED;
static bool            slave_commit_resets, slave_reset_pending, master_commit_resets, master_reset_pending;
static uint32_t        slave_next_fw, master_next_fw;
#ifndef SVAL_TEST_REAL_COMMIT
bool update_commit_available(void) {
    return sim_slave ? true : commit_avail;
}
update_status_t update_commit_run(const sval_update_manifest_t *m, uint32_t crc_body) {
    if (sim_slave) {
        slave_commit_runs++;
        slave_commit_crc = crc_body;
        slave_commit_len = m->image_len;
        if (slave_commit_resets) {
            slave_reset_pending = true;
            slave_next_fw       = m->fw_version;
        }
        return slave_commit_resets ? UPDATE_UNSUPPORTED : slave_commit_result;
    }
    commit_runs++;
    commit_crc = crc_body;
    commit_len = m->image_len;
    if (master_commit_resets) {
        master_reset_pending = true;
        master_next_fw       = m->fw_version;
    }
    return commit_result;
}
#endif

// The commit marker (updater_port.h): each half's own, which survives the
// simulated resets as watchdog scratch 0 does.
static bool master_marked, slave_marked;
static int  mark_sets;
void        updater_port_commit_mark(bool on) {
    if (on) mark_sets++;
    // A mock commit that "reset" returns, which the real one never does.
    if (!on && (sim_slave ? slave_reset_pending : master_reset_pending)) return;
    if (sim_slave) {
        slave_marked = on;
    } else {
        master_marked = on;
    }
}
bool updater_port_commit_mark_take(void) {
    bool *m   = sim_slave ? &slave_marked : &master_marked;
    bool  was = *m;
    *m        = false;
    return was;
}

// ---- the split link (M2): update_split.c's port and a simulated other half ----------------
//
// Both halves' code runs in this process. The other half's KEYBOARD_UPDATE
// callback runs inside update_split_port_rpc() (as the SlaveThread would,
// while the master waits), and its housekeeping (update_split_slave_task) in
// pass(), after this half's. Both use the one mock die: in the relay tests
// this half's slot is master_slot (a RAM copy), and the die's slot is the
// other half's. Faults can be injected into every RPC.

static update_build_id_t master_id, slave_id;
static bool              link_up = true, link_paused, link_dead;
static int               pause_calls, lock_depth, lock_errors, unlocks;
static void (*unlock_hook)(int n); // after each unlock by the other half's housekeeping (a preemption point)
static void (*lock_hook)(void);    // before each lock by the other half's housekeeping (a preemption point)
// The other half with its interrupts off: while it erases a sector or runs its
// commit (slave_irq_sim), RPCs fail as on the real link (the master's serial
// transactions time out), and its housekeeping does not run until it is done.
static bool     slave_irq_sim;
static uint64_t slave_busy_until_us;
static uint32_t slave_busy_rpcs; // RPCs that failed for it
#define SIM_ERASE_US 50000       // M1 #13: a sector erase 50-53 ms
#define SIM_SLAVE_COMMIT_US 1500000
// DMA busy when the other half programs a page (update_split_port_program_page).
static uint32_t       dma_busy_pct, dma_refusals;
static bool           slave_hk_frozen;  // the other half's housekeeping does not run (it is stuck)
static int            drop_rsp_op = -1; // every answer to requests with this op is lost
static bool           old_slave;        // the other half on protocol 3: KEYBOARD_UPDATE op 0x07 is unknown there
static const uint8_t *master_slot;      // this half's slot for the relay; NULL: the die's
static uint8_t        shadow[SVAL_UPDATE_MAX_IMAGE];
static bool           auto_shadow; // the relay's pause takes a copy of the die's slot as this half's (tool tests)
static struct {
    uint32_t n;                                                          // RPCs so far
    uint32_t drop_req, drop_rsp, flip_req, flip_rsp, dup, replay, stale; // percent chance per RPC
    uint32_t faults;                                                     // faults injected
} lk;
static uint8_t  hist[8][USPLIT_MSG_MAX], hist_len[8]; // the last requests, for replays
static uint32_t hist_n;
static uint64_t last_ping_us;
static uint64_t last_slave_rx_us; // when the other half last got a request
#define SIM_RPC_US 600            // four transactions at 1 Mbaud, with the handshakes
#define SIM_RPC_FAIL_US 20000     // a transaction that times out (SERIAL_USART_TIMEOUT)

uint32_t update_split_port_now_ms(void) {
    return (uint32_t)(sim_us / 1000);
}
void update_split_port_build_id(update_build_id_t *id) {
    *id = sim_slave ? slave_id : master_id;
}
uint8_t update_split_port_info_flags(void) {
    return 0x01 | (port_failing ? 0x02 : 0);
}
bool update_split_port_updating(void) {
    return sim_slave ? update_split_slave_active() : updater_active();
}
void update_split_port_lock(void) {
    if (lock_hook) lock_hook();
    if (!sim_slave || lock_depth) lock_errors++; // only the other half's housekeeping takes it, never nested
    lock_depth++;
}
void update_split_port_unlock(void) {
    if (lock_depth != 1) lock_errors++;
    lock_depth--;
    unlocks++;
    if (unlock_hook) unlock_hook(unlocks);
}
bool update_split_port_pause(bool on) {
    if (sim_slave) return false;
    if (on) {
        if (!link_up || link_dead) return false; // sval_split_pause refuses with the link down
        if (auto_shadow && !master_slot) {
            // The relay pauses first: the image this half staged and verified
            // becomes "its slot", and the die's slot the other half's.
            memcpy(shadow, update_host_flash + SVAL_UPDATE_BASE, sizeof(shadow));
            master_slot = shadow;
        }
        if (!link_paused) pause_calls++;
        link_paused = true;
    } else {
        link_paused = false;
    }
    return true;
}
// This half's own image (the boot relay's source): its own copy, apart from
// the die, which the simulation shares with the other half.
static uint8_t master_self[SVAL_UPDATE_MAX_IMAGE];
static bool    port_master = true; // this half has USB
bool           updater_port_is_master(void) {
    return !sim_slave && port_master;
}
bool update_split_port_self_read(uint32_t off, uint8_t *dst, uint32_t n) {
    if (off % 4 || n % 4 || off > SVAL_UPDATE_MAX_IMAGE || n > SVAL_UPDATE_MAX_IMAGE - off) return false;
    memcpy(dst, master_self + off, n);
    return true;
}
bool update_split_port_slot_read(uint32_t off, uint8_t *dst, uint32_t n) {
    if (off % 4 || n % 4 || off > SVAL_UPDATE_SIZE || n > SVAL_UPDATE_SIZE - off) return false;
    memcpy(dst, master_slot ? master_slot + off : update_host_flash + SVAL_UPDATE_BASE + off, n);
    return true;
}

static bool chance(uint32_t pct) {
    return pct && rnd() % 100 < pct;
}

update_status_t update_split_port_program_page(uint32_t off, const uint8_t *page) {
    if (chance(dma_busy_pct)) {
        dma_refusals++;
        return UPDATE_BUSY;
    }
    return update_flash_program_page(off, page);
}

// SETTINGS_RESET (D42): each half's build reset record, as invalidated, and
// its reset, which the simulation runs as a reboot into the same image.
static int  master_invalidations, slave_invalidations, master_soft_resets, slave_soft_resets;
static bool master_invalidate_fails, slave_invalidate_fails;
static int  master_reset_mode = -1, slave_reset_mode = -1; // the last mode each half took (D44)
static bool slave_pre_blank;                               // the other half is a protocol 4 build from before D44: no mode byte
bool        updater_port_settings_invalidate(uint8_t mode) {
    if (master_invalidate_fails) return false;
    master_invalidations++;
    master_reset_mode = mode;
    return true;
}
void updater_port_reboot(void) {
    master_soft_resets++;
    master_reset_pending = true;
    master_next_fw       = master_id.fw_version;
}
update_status_t update_split_port_settings_invalidate(uint8_t mode) {
    if (chance(dma_busy_pct)) {
        dma_refusals++;
        return UPDATE_BUSY;
    }
    if (slave_invalidate_fails) return UPDATE_FLASH_ERR;
    slave_invalidations++;
    slave_reset_mode = mode;
    return UPDATE_OK;
}
void update_split_port_reboot(void) {
    slave_soft_resets++;
    slave_reset_pending = true;
    slave_next_fw       = slave_id.fw_version;
}

// The other half's callback, as the SlaveThread runs it.
static void slave_rpc(const uint8_t *in, uint8_t len, uint8_t *out, uint8_t out_len) {
    bool was  = sim_slave;
    sim_slave = true;
    update_split_slave_rpc(in, len, out, out_len);
    sim_slave = was;
}

bool update_split_port_rpc(const uint8_t *req, uint8_t req_len, uint8_t *rsp, uint8_t rsp_len) {
    if (sim_slave || req_len > USPLIT_MSG_MAX || rsp_len > USPLIT_MSG_MAX) {
        lock_errors++;
        return false;
    }
    lk.n++;
    sim_us += SIM_RPC_US;
    if (!link_up || link_dead) {
        sim_us += SIM_RPC_FAIL_US;
        return false;
    }
    if (sim_us < slave_busy_until_us) { // its interrupts are off: no answer
        slave_busy_rpcs++;
        sim_us += SIM_RPC_FAIL_US;
        return false;
    }
    if (old_slave && req[0] == USPLIT_OP_SETTINGS_RESET) {
        memset(rsp, 0, rsp_len); // an unknown op builds no answer (update_split_slave_rpc)
        return true;
    }
    if (slave_pre_blank && req[0] == USPLIT_OP_SETTINGS_RESET && req_len > USPLIT_REQ_HDR + 4 + USPLIT_CRC_BYTES) {
        // a protocol 4 half from before D44: its payload rule takes 4 bytes only
        usplit_rsp_build(rsp, rsp_len, UPDATE_INVALID, USPLIT_OP_SETTINGS_RESET, req[1], UPDATE_STATE_IDLE, NULL);
        return true;
    }
    uint8_t in[USPLIT_MSG_MAX], out[USPLIT_MSG_MAX];
    memcpy(in, req, req_len);
    if (chance(lk.drop_req)) { // lost on the way: the other half never runs it
        lk.faults++;
        sim_us += SIM_RPC_FAIL_US;
        return false;
    }
    if (chance(lk.replay) && hist_n) { // an older request arrives first (reordered)
        lk.faults++;
        uint32_t k = rnd() % (hist_n < 8 ? hist_n : 8);
        slave_rpc(hist[k], hist_len[k], out, USPLIT_MSG_MAX);
    }
    bool stale = chance(lk.stale) && hist_n; // the buffer still held the last request (a short write)
    if (stale) {
        lk.faults++;
        uint32_t k = (hist_n - 1) % 8;
        memcpy(in, hist[k], hist_len[k]);
        req_len = hist_len[k];
    }
    if (chance(lk.flip_req)) {
        lk.faults++;
        in[rnd() % req_len] ^= (uint8_t)(1u << (rnd() % 8));
    }
    last_slave_rx_us = sim_us;
    slave_rpc(in, req_len, out, rsp_len);
    if (chance(lk.dup)) { // delivered twice
        lk.faults++;
        slave_rpc(in, req_len, out, rsp_len);
    }
    if (!stale) {
        memcpy(hist[hist_n % 8], req, req_len);
        hist_len[hist_n % 8] = req_len;
        hist_n++;
    }
    if (chance(lk.drop_rsp) || (drop_rsp_op >= 0 && req[0] == drop_rsp_op)) { // the answer is lost
        lk.faults++;
        sim_us += SIM_RPC_FAIL_US;
        return false;
    }
    if (chance(lk.flip_rsp) && rsp_len) {
        lk.faults++;
        out[rnd() % rsp_len] ^= (uint8_t)(1u << (rnd() % 8));
    }
    memcpy(rsp, out, rsp_len);
    return true;
}

// The other half reboots: its RAM state is gone (a power cut, or its commit).
static int  slave_reboots, master_reboots;
static void slave_reboot(void) {
    update_split_host_slave_reset();
    slave_reboots++;
}

// The other half's housekeeping pass.
static void slave_hk(void) {
    if (sim_us < slave_busy_until_us || slave_hk_frozen) return; // still erasing, or committing
    int erases = erase_ops, commits = slave_commit_runs;
    sim_slave = true;
    update_split_slave_task();
    sim_slave = false;
    if (slave_irq_sim && erase_ops != erases) slave_busy_until_us = sim_us + SIM_ERASE_US;
    if (slave_irq_sim && slave_commit_runs != commits) slave_busy_until_us = sim_us + SIM_SLAVE_COMMIT_US;
    if (slave_reset_pending) {
        slave_reset_pending = false;
        slave_reboot();
        slave_id.fw_version = slave_next_fw;
    }
}

// The 500 ms presence ping (svalboard.c), skipped while paused, with the
// other half's real presence answer.
static void presence_ping(void) {
    uint8_t out[UPDATE_PRESENCE_BYTES];
    bool    answered = link_up && !link_dead;
    if (answered) {
        sim_slave = true;
        update_split_presence_fill(out, sizeof(out));
        sim_slave = false;
    }
    update_split_presence_result(answered, out, sizeof(out));
}

// ---- driving it -------------------------------------------------------------------------

#define CLIENT_A 0x00A1A1A1u
#define CLIENT_B 0x00B2B2B2u
#define CLIENT_C 0x00C3C3C3u

// The packet layout for RAW_EPSIZE (keyboards/svalboard/config.h, included
// first): as updater.c derives it, written out again here so a change to one
// is caught by the other.
#define T_REPORT RAW_EPSIZE        // 64
#define T_VALUE (T_REPORT - 6 - 3) // value bytes after the wrapper and VIA's header: 55
#define T_CHUNK (T_VALUE - 5)      // CHUNK data after [hand][off u24][n]: 50
#define T_MPIECE (T_VALUE - 3)     // MANIFEST data after [hand][off][n]: 52
_Static_assert(T_REPORT == 64, "64-byte raw HID reports");

static uint8_t  rsp[T_VALUE];
static uint8_t  req_hand = UPDATE_HAND_RIGHT;
static uint8_t  req_cmd  = 0x07; // id_custom_set_value
static uint64_t max_op_us, max_pass_us, sig_pass_us;

static uint32_t get24le(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}
static uint32_t get32le(const uint8_t *p) {
    return get24le(p) | ((uint32_t)p[3] << 24);
}
static uint16_t get16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static update_status_t send(uint32_t client, uint8_t opcode, const void *args, size_t n) {
    // A wrapped report as VIA sees it: the request in the first 3 + T_VALUE
    // bytes, and the wrapper's 6 at the end, whose reply bytes never reach the host.
    uint8_t pkt[T_REPORT];
    CHECK(n <= T_VALUE - 1);
    memset(pkt, 0xEE, sizeof(pkt)); // stale bytes past the request
    pkt[0] = req_cmd;
    pkt[1] = UPDATE_CHANNEL;
    pkt[2] = opcode;
    pkt[3] = req_hand;
    memset(pkt + 4, 0, T_VALUE - 1);
    if (n) memcpy(pkt + 4, args, n);
    port_client = client;
    uint64_t t0 = sim_us;
    updater_via_command(pkt, sizeof(pkt));
    if (sim_us - t0 > max_op_us) max_op_us = sim_us - t0;
    port_client = 0;
    CHECK(pkt[0] == req_cmd && pkt[1] == UPDATE_CHANNEL && pkt[2] == opcode); // header untouched
    for (int i = 3 + T_VALUE; i < T_REPORT; i++)
        CHECK(pkt[i] == 0xEE); // nothing past the value bytes
    memcpy(rsp, pkt + 3, sizeof(rsp));
    return (update_status_t)rsp[0];
}

static update_status_t send_nonce(uint32_t client, uint8_t opcode, uint32_t nonce) {
    uint8_t a[4];
    put32(a, nonce);
    return send(client, opcode, a, 4);
}

static void pass(void) {
    update_state_t before = updater_state();
    uint64_t       t0     = sim_us;
    updater_task();
    uint64_t dt = sim_us - t0;
    if (master_reset_pending) { // this half "reset into its new image" (pair)
        master_reset_pending = false;
        updater_host_reset();
        update_split_host_master_reset();
        master_id.fw_version = master_next_fw;
        master_reboots++;
    }
    slave_hk();
    if (presence_sim && !link_paused && sim_us - last_ping_us >= 500000) {
        last_ping_us = sim_us;
        presence_ping();
    }
    if (before == UPDATE_STATE_VERIFYING_MANIFEST) {
        if (dt > sig_pass_us) sig_pass_us = dt;
    } else if (dt > max_pass_us) {
        max_pass_us = dt;
    }
    sim_us += 1000;
}

static void wait_ms(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++)
        pass();
}

static void passes_until(update_state_t want, int max) {
    for (int i = 0; i < max && updater_state() != want && updater_state() != UPDATE_STATE_ERROR; i++)
        pass();
}

// ---- the chord ---------------------------------------------------------------------------

static matrix_row_t rows[5];

// One matrix scan with the chord keys at (a, b) and an unrelated key held;
// returns what QMK would see in rows 1 and 2, column 0.
static uint8_t scan(bool a, bool b) {
    rows[0]                       = 0x04;
    rows[SVAL_UPDATE_CHORD_ROW_A] = (a ? 1 : 0) << SVAL_UPDATE_CHORD_COL | 0x08;
    rows[SVAL_UPDATE_CHORD_ROW_B] = (b ? 1 : 0) << SVAL_UPDATE_CHORD_COL;
    update_gesture_scan(rows);
    CHECK(rows[0] == 0x04 && (rows[SVAL_UPDATE_CHORD_ROW_A] & 0x08)); // other keys pass through
    return (rows[SVAL_UPDATE_CHORD_ROW_A] & 1) | (rows[SVAL_UPDATE_CHORD_ROW_B] & 1) << 1;
}

// Hold the chord for ms, scanning every 10 ms, with main-loop passes.
static bool hold(uint32_t ms) {
    bool leaked = false;
    for (uint32_t t = 0; t < ms; t += 10) {
        leaked |= scan(true, true) != 0;
        for (int i = 0; i < 10; i++)
            pass();
    }
    return leaked;
}

static void chord(void) {
    scan(false, false);
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS + 50));
    scan(false, false);
}

// ---- an update ------------------------------------------------------------------------------

#define IMG_MAX SVAL_UPDATE_MAX_IMAGE
static uint8_t  img[IMG_MAX];
static uint8_t  blob[UPDATE_MANIFEST_BYTES]; // the manifest, as the MANIFEST op sends it
static uint32_t img_len;

static sval_update_manifest_t *blob_manifest(void) {
    return (sval_update_manifest_t *)blob;
}

static void make_update(uint32_t len) {
    img_len = len;
    for (uint32_t i = 0; i < len; i++)
        img[i] = (uint8_t)(i * 13 + 5 + (i >> 9));
    memcpy(img, boot2_page0, 256);
    put32(img + 0x100, 0x20041000);
    put32(img + 0x104, 0x100001F7);
    sval_update_manifest_t m = good_manifest();
    m.image_len              = len;
    m.security_epoch         = 0;
    crypto_sha512(m.sha512, img, len);
    memcpy(blob, &m, sizeof(m));
}

static uint32_t body_crc(void) {
    return update_crc32_mpeg2(img + 0x100, img_len - 0x100);
}

static update_status_t load_manifest(uint32_t client) {
    for (uint32_t off = 0; off < UPDATE_MANIFEST_BYTES; off += T_MPIECE) {
        uint8_t n = UPDATE_MANIFEST_BYTES - off < T_MPIECE ? (uint8_t)(UPDATE_MANIFEST_BYTES - off) : T_MPIECE;
        uint8_t a[2 + T_MPIECE];
        a[0] = (uint8_t)off;
        a[1] = n;
        memcpy(a + 2, blob + off, n);
        update_status_t st = send(client, UPDATE_OP_MANIFEST, a, 2 + n);
        if (st != UPDATE_OK) return st;
        CHECK_EQ(rsp[1], off + n);
    }
    return UPDATE_OK;
}

static update_status_t send_chunk(uint32_t client, uint32_t off, uint8_t n, const uint8_t *data) {
    uint8_t a[4 + T_CHUNK] = {0};
    a[0]                   = off & 0xFF;
    a[1]                   = (off >> 8) & 0xFF;
    a[2]                   = (off >> 16) & 0xFF;
    a[3]                   = n;
    memcpy(a + 4, data, n > T_CHUNK ? T_CHUNK : n);
    return send(client, UPDATE_OP_CHUNK, a, sizeof(a));
}

static update_status_t send_image(uint32_t client, uint32_t from, uint32_t to) {
    for (uint32_t off = from; off < to;) {
        uint8_t         n  = to - off < T_CHUNK ? (uint8_t)(to - off) : T_CHUNK;
        update_status_t st = send_chunk(client, off, n, img + off);
        if (st != UPDATE_OK) return st;
        CHECK_EQ(get24le(rsp + 1), off + n);
        off += n;
    }
    return UPDATE_OK;
}

static uint32_t session_nonce;

static bool to_confirm_wait(uint32_t client) {
    if (load_manifest(client) != UPDATE_OK) return false;
    if (send(client, UPDATE_OP_ARM, NULL, 0) != UPDATE_OK) return false;
    session_nonce = get32le(rsp + 1);
    return updater_state() == UPDATE_STATE_CONFIRM_WAIT;
}

static bool to_receiving(uint32_t client) {
    if (!to_confirm_wait(client)) return false;
    chord();
    if (send_nonce(client, UPDATE_OP_BEGIN, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    return updater_state() == UPDATE_STATE_RECEIVING;
}

static bool to_verified(uint32_t client) {
    if (!to_receiving(client)) return false;
    if (send_image(client, 0, img_len) != UPDATE_OK) return false;
    if (send_nonce(client, UPDATE_OP_END, session_nonce) != UPDATE_ACCEPTED) return false;
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    return updater_state() == UPDATE_STATE_VERIFIED;
}

static void fresh(uint32_t len) {
    updater_host_reset();
    die_reset();
    scan(false, false);
    scan(false, false);
    port_failing        = false;
    port_scan_override  = false;
    port_split_presence = UPDATE_PRESENCE_NONE;
    // the split simulation: a left half with a TrackPoint, the same release
    update_split_host_reset();
    sim_slave    = false;
    presence_sim = false;
    link_up      = true;
    link_paused = link_dead = false;
    pause_calls = lock_depth = lock_errors = unlocks = 0;
    unlock_hook                                      = NULL;
    lock_hook                                        = NULL;
    slave_irq_sim                                    = false;
    slave_busy_until_us                              = 0;
    slave_busy_rpcs                                  = 0;
    dma_busy_pct = dma_refusals = 0;
    slave_hk_frozen             = false;
    drop_rsp_op                 = -1;
    old_slave                   = false;
    master_slot                 = NULL;
    auto_shadow                 = false;
    memset(&lk, 0, sizeof(lk));
    hist_n                   = 0;
    last_ping_us             = 0;
    master_id                = (update_build_id_t){.fw_version = 2001, .git_hash = 0x3b2ca7e1, .updater_proto = UPDATE_PROTOCOL_VERSION, .hand = UPDATE_HAND_RIGHT, .pointing_id = UPDATE_POINTING_PMW3389};
    slave_id                 = master_id;
    slave_id.hand            = UPDATE_HAND_LEFT;
    slave_id.pointing_id     = UPDATE_POINTING_TRACKPOINT;
    slave_dev                = test_dev;
    slave_dev.hand           = UPDATE_HAND_LEFT;
    slave_dev.pointing_id    = UPDATE_POINTING_TRACKPOINT;
    slave_dev.security_epoch = 0;
    slave_commit_runs        = 0;
    slave_commit_result      = UPDATE_UNSUPPORTED;
    slave_commit_resets = slave_reset_pending = master_commit_resets = master_reset_pending = false;
    slave_reboots = master_reboots = 0;
    master_invalidations = slave_invalidations = master_soft_resets = slave_soft_resets = 0;
    master_invalidate_fails = slave_invalidate_fails = false;
    master_reset_mode = slave_reset_mode = -1;
    slave_pre_blank                      = false;
    commit_avail                         = false;
    master_marked                        = false;
    slave_marked                         = false;
    commit_runs                          = 0;
    commit_result                        = UPDATE_UNSUPPORTED;
    req_hand                             = UPDATE_HAND_RIGHT;
    req_cmd                              = 0x07;
    self_dev                             = test_dev;
    self_dev.security_epoch              = 0;
    make_update(len);
}

// ---- tests ---------------------------------------------------------------------------------

static void test_session_info_and_wrapper(void) {
    fresh(0x3000);
    // INFO needs no wrapper, and takes 0xFF for "this half"
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_IDLE);
    CHECK_EQ(rsp[2], UPDATE_PROTOCOL_VERSION);
    CHECK_EQ(get16le(rsp + 3), SVAL_UPDATE_BASE / 4096);
    CHECK_EQ(get16le(rsp + 5), SVAL_UPDATE_SIZE / 4096);
    CHECK_EQ(get16le(rsp + 7), SVAL_UPDATE_MAX_IMAGE / 4096);
    CHECK(rsp[9] == 0xEF && rsp[10] == 0x40 && rsp[11] == 0x18);
    CHECK_EQ(rsp[12], UPDATE_POINTING_PMW3389);
    CHECK_EQ(rsp[13], UPDATE_HAND_RIGHT);
    CHECK_EQ(rsp[18], SVAL_UPDATE_STORAGE_FORMAT);
    CHECK_EQ(rsp[21] & 0x07, 0x01);
    CHECK_EQ(rsp[21] & (0x10 | 0x40), 0); // reserved (protocol 1's key flags)
    // protocol 3: the largest CHUNK and MANIFEST pieces and the report size,
    // which a host sizes its requests by; nothing after them
    CHECK_EQ(rsp[2], 4); // protocol 4 (D41-D43)
    CHECK_EQ(rsp[23], T_CHUNK);
    CHECK_EQ(rsp[24], T_MPIECE);
    CHECK_EQ(rsp[25], T_REPORT);
    for (int i = 26; i < T_VALUE; i++)
        CHECK_EQ(rsp[i], 0);
    // INFO page 1 (M3): the version from the build-info record; no wrapper needed
    const uint8_t page1 = 1, page2 = 2;
    CHECK_EQ(send(0, UPDATE_OP_INFO, &page1, 1), UPDATE_OK);
    CHECK_EQ(rsp[1], 1);
    CHECK_EQ(get32le(rsp + 2), sval_update_build_info.fw_version);
    CHECK(memcmp(rsp + 6, sval_update_build_info.version, UPDATE_VERSION_CHARS) == 0);
    CHECK_EQ(send(0, UPDATE_OP_INFO, &page2, 1), UPDATE_INVALID);
    req_hand = UPDATE_HAND_LEFT; // the other half: page 1 is this half's only
    CHECK_EQ(send(0, UPDATE_OP_INFO, &page1, 1), UPDATE_INVALID);
    req_hand = 0xFF;
    CHECK_EQ(send(0, UPDATE_OP_INFO, &page1, 1), UPDATE_OK);
    CHECK_EQ(rsp[1], 1);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[13], UPDATE_HAND_RIGHT);
    // 0xFF for anything but INFO, or a hand that is neither half: UNSUPPORTED
    // (TEST_HALT is an unknown op, INVALID, in a build without the hooks)
#ifdef SVAL_UPDATE_TEST_HOOKS
    const update_status_t test_halt_unknown = UPDATE_UNSUPPORTED;
#else
    const update_status_t test_halt_unknown = UPDATE_INVALID;
#endif
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_UNSUPPORTED);
    for (int h = 2; h < 0xFF; h += 37) {
        req_hand = (uint8_t)h;
        CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNSUPPORTED);
        for (uint8_t op = UPDATE_OP_MANIFEST; op <= UPDATE_OP_RELAY; op++)
            CHECK_EQ(send(CLIENT_A, op, NULL, 0), op == UPDATE_OP_TEST_HALT ? test_halt_unknown : UPDATE_UNSUPPORTED);
    }
    // the other half (M2): TEST_HALT and DIAG are this half's only; a session
    // for it needs its presence answer (UNAVAILABLE, nothing changes)
    req_hand = UPDATE_HAND_LEFT;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_TEST_HALT, NULL, 0), test_halt_unknown);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_DIAG, NULL, 0), UPDATE_UNSUPPORTED);
    uint8_t m0[22] = {0, 20};
    memcpy(m0 + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, m0, sizeof(m0)), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    req_hand = UPDATE_HAND_RIGHT;

    // every op but INFO refused without the client wrapper, with no state change
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    for (uint8_t op = UPDATE_OP_MANIFEST; op <= UPDATE_OP_REBIND; op++) {
        CHECK_EQ(send(0, op, a, sizeof(a)), UPDATE_INVALID);
        CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    }
    // DIAG needs the wrapper too
    CHECK_EQ(send(0, UPDATE_OP_DIAG, NULL, 0), UPDATE_INVALID);
    // unknown ops, wrapped or not (TEST_HALT outside a session, or in a build without hooks)
    CHECK_EQ(send(CLIENT_A, 0x0A, NULL, 0), UPDATE_INVALID);
    CHECK_EQ(send(CLIENT_A, 0x0D, NULL, 0), UPDATE_INVALID);
    CHECK_EQ(send(CLIENT_A, 0xFF, NULL, 0), UPDATE_INVALID);
    // VIA's save command on this channel is left alone (value byte 0 is still the request's hand)
    req_cmd = 0x09;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_HAND_RIGHT);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    req_cmd = 0x08; // get_value works like set_value
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    req_cmd = 0x07;
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // mid-session, unwrapped packets still change nothing
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_chunk(0, 0, 18, img), UPDATE_INVALID);
    CHECK_EQ(send_nonce(0, UPDATE_OP_ABORT, session_nonce), UPDATE_INVALID);
    CHECK_EQ(send_nonce(0, UPDATE_OP_REBIND, session_nonce), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 2), 0);
    // a short report is refused, not read past its end
    uint8_t pkt[T_REPORT] = {0x07, UPDATE_CHANNEL, UPDATE_OP_INFO, 0xFF};
    port_client           = CLIENT_A;
    updater_via_command(pkt, 20);
    port_client = 0;
    CHECK_EQ(pkt[3], UPDATE_INVALID);
    // so is a 32-byte report (protocol 2's), and one a byte short of the value
    // bytes; only the status byte is written
    const uint8_t lens[] = {32, 3 + T_VALUE - 1};
    for (size_t i = 0; i < sizeof(lens); i++) {
        memset(pkt, 0xEE, sizeof(pkt));
        pkt[0] = 0x07, pkt[1] = UPDATE_CHANNEL, pkt[2] = UPDATE_OP_INFO, pkt[3] = 0xFF;
        port_client = CLIENT_A;
        updater_via_command(pkt, lens[i]);
        port_client = 0;
        CHECK_EQ(pkt[3], UPDATE_INVALID);
        for (int k = 4; k < T_REPORT; k++)
            CHECK_EQ(pkt[k], 0xEE);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
}

static void test_session_happy_path(void) {
    fresh(0x30000);                                                       // 48 sectors, across three 64 KiB blocks
    memset(update_host_flash + SVAL_UPDATE_BASE, 0x00, SVAL_UPDATE_SIZE); // every sector needs erasing
    snap();
    max_op_us = max_pass_us = sig_pass_us = 0;
    led_seen                              = 0;
    const uint8_t clear                   = 1;
    CHECK_EQ(send(CLIENT_C, UPDATE_OP_DIAG, &clear, 1), UPDATE_OK); // restart the longest-pass record

    // record each distinct state, in order
    update_state_t seen[16];
    int            nseen = 0;
#define NOTE()                                                                                 \
    do {                                                                                       \
        if (nseen == 0 || seen[nseen - 1] != updater_state()) seen[nseen++] = updater_state(); \
    } while (0)
    NOTE();
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    NOTE();
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    NOTE();
    session_nonce = get32le(rsp + 1);
    CHECK(session_nonce != 0);
    uint8_t h[64];
    crypto_sha512(h, blob, UPDATE_MANIFEST_BYTES);
    CHECK(memcmp(rsp + 5, h, 4) == 0); // the hash prefix the host shows (D5)
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_NOT_CONFIRMED); // no chord yet: refused, still waiting
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    chord();
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CHORD);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[21] & 0x03, 0x03);
    wait_ms(400);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS); // approved: updating from here on (no solid blue)
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    NOTE();
    // CHUNK and END wait for the erase
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_BUSY);
    for (int i = 0; i < 2000 && updater_state() != UPDATE_STATE_RECEIVING; i++) {
        pass();
        NOTE();
        CHECK(updater_state() != UPDATE_STATE_ERROR);
    }
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(sector_erases, 0x30000 / 4096);
    CHECK_EQ(block_erases, 0);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(get16le(rsp + 8), 48);
    CHECK_EQ(get16le(rsp + 10), 48);
    CHECK_EQ(get16le(rsp + 17), SIM_SECTOR_ERASE_US / 1000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    NOTE();
    for (int i = 0; i < 5000 && updater_state() == UPDATE_STATE_VERIFYING_IMAGE; i++)
        pass();
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, img_len) == 0);
    CHECK(unchanged_outside(SVAL_UPDATE_BASE, SVAL_UPDATE_BASE + SVAL_UPDATE_SIZE)); // invariant (a)
    CHECK_EQ(outside_ops, 0);
    CHECK_EQ(sequence_errors, 0);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_VERIFIED);
    CHECK_EQ(get24le(rsp + 2), img_len);
    CHECK_EQ(get24le(rsp + 5), img_len);
    CHECK_EQ(get16le(rsp + 19), body_crc() & 0xFFFF);
    CHECK_EQ(rsp[12], UPDATE_OK);
    // DIAG (M1 #13): main's stack, and the longest pass, here a sector erase,
    // as the device measures it (any client may ask; the session is unaffected)
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_DIAG, NULL, 0), UPDATE_OK);
    CHECK_EQ(get16le(rsp + 1), HOST_STACK_FREE);
    CHECK_EQ(get16le(rsp + 3), HOST_STACK_SIZE);
    CHECK_EQ(get16le(rsp + 5), SIM_SECTOR_ERASE_US / 1000);
    CHECK_EQ(rsp[7], UPDATE_STATE_ERASING);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_DIAG, &clear, 1), UPDATE_OK);
    CHECK_EQ(get16le(rsp + 5), SIM_SECTOR_ERASE_US / 1000); // the reply is from before the clear
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_DIAG, NULL, 0), UPDATE_OK);
    CHECK_EQ(get16le(rsp + 5), 0);

    // COMMIT: the stub refuses and nothing happens
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_UNSUPPORTED);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // with a commit routine present: acknowledged, run 100 ms later from a pass
    commit_avail  = true;
    commit_result = UPDATE_BUSY; // e.g. a DMA channel busy at step 1: refused with no erase
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_COMMITTING);
    // nothing but INFO and STATUS while committing
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, session_nonce), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    wait_ms(90);
    CHECK_EQ(commit_runs, 0);
    CHECK_EQ(led_now, UPDATE_LED_WRITING);
    wait_ms(15);
    CHECK_EQ(commit_runs, 1);
    CHECK_EQ(commit_crc, body_crc());
    CHECK_EQ(commit_len, img_len);
    NOTE();
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_BUSY);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
#undef NOTE
    int led_shown = led_seen;

    const update_state_t want[] = {
        UPDATE_STATE_IDLE, UPDATE_STATE_MANIFEST_LOADING, UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_VERIFYING_MANIFEST, UPDATE_STATE_ERASING, UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFYING_IMAGE, UPDATE_STATE_VERIFIED, UPDATE_STATE_COMMITTING, UPDATE_STATE_ERROR,
    };
    CHECK_EQ(nseen, (int)(sizeof(want) / sizeof(want[0])));
    for (int i = 0; i < nseen && i < (int)(sizeof(want) / sizeof(want[0])); i++)
        CHECK_EQ(seen[i], want[i]);
    CHECK_EQ(led_shown, (1 << UPDATE_LED_CONFIRM) | (1 << UPDATE_LED_CHORD) | (1 << UPDATE_LED_PROGRESS) | (1 << UPDATE_LED_WRITING) | (1 << UPDATE_LED_ERROR));

    // the 50 ms budget, in simulated flash time
    printf("longest op %llu us, longest pass %llu us\n", (unsigned long long)max_op_us, (unsigned long long)max_pass_us);
    CHECK(max_op_us + max_pass_us <= 50000);
    CHECK(max_op_us <= SIM_PAGE_PROGRAM_US); // at most one page per CHUNK

    // the commit refused and returned: no commit marker is left for the next boot
    CHECK(!master_marked);
    // the red stops after SVAL_UPDATE_RED_MS; the error stays for STATUS and ABORT
    wait_ms(SVAL_UPDATE_RED_MS - 200);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    wait_ms(400);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    // ABORT from anyone clears the error, and the LEDs go back
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_NONE);
}

static void test_session_gesture(void) {
    fresh(0x3000);
    // not watching: keys pass through
    CHECK_EQ(scan(true, true), 3);
    CHECK_EQ(scan(false, false), 0);

    // keys held when ARM arrives are swallowed (QMK sees them released) and do
    // not confirm until let go and pressed again
    scan(true, true);
    CHECK(to_confirm_wait(CLIENT_A));
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS + 200));
    CHECK(!update_gesture_done(NULL));
    // one key alone, or a hold that is too short, does nothing; all swallowed
    scan(false, false);
    for (int i = 0; i < 150; i++) {
        CHECK_EQ(scan(true, false), 0);
        wait_ms(10);
    }
    scan(false, false);
    CHECK(!hold(SVAL_UPDATE_CHORD_HOLD_MS - 30));
    CHECK(!update_gesture_done(NULL));
    // a bounce restarts the hold
    scan(false, false);
    CHECK(!hold(700));
    CHECK_EQ(scan(true, false), 0);
    CHECK(!hold(700));
    CHECK(!update_gesture_done(NULL));
    CHECK(!hold(400));
    CHECK(update_gesture_done(NULL));
    // still held after BEGIN: swallowed until each key is released
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    CHECK_EQ(scan(true, true), 0);
    CHECK_EQ(scan(false, true), 0);
    CHECK_EQ(scan(true, true), 1); // A was released, so it is a new press
    CHECK_EQ(scan(true, false), 1);
    CHECK_EQ(scan(true, true), 3);

    // the chord window: 30 s from ARM, then NOT_CONFIRMED (latched)
    fresh(0x3000);
    CHECK(to_confirm_wait(CLIENT_A));
    for (int i = 0; i < 29; i++) {
        wait_ms(1000);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // keeps the session alive
    }
    wait_ms(990);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    CHECK_EQ(scan(true, true), 0);
    wait_ms(20);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_NOT_CONFIRMED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_INVALID);
    CHECK_EQ(scan(true, true), 0); // still held: still swallowed
    CHECK_EQ(scan(false, false), 0);
    CHECK_EQ(scan(true, true), 3);
    CHECK_EQ(erase_ops + program_ops, 0);
}

// The chord is read only from frames scanned with a safe timing: while it is
// awaited, a host-chosen scan timing (saved values or a Scan Lab sweep, which
// can misread keys) is raised to Scan Lab's safe reference; and ARM waits
// while a sweep runs (scanlab.c, in turn, refuses a sweep or a probe while the
// updater is active).
static void test_session_scan_timing(void) {
    fresh(0x3000);
    uint16_t pre = 0, post = 3;
    update_gesture_timing(&pre, &post); // not waiting for the chord: untouched
    CHECK(pre == 0 && post == 3);
    // a sweep running: ARM refused (BUSY), nothing changes, no nonce
    port_scan_override = true;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_BUSY);
    CHECK_EQ(get32le(rsp + 1), 0);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    port_scan_override = false;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    session_nonce = get32le(rsp + 1);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    // waiting: short waits are raised, longer ones kept
    const uint16_t cases[][2] = {{0, 0}, {1, 1}, {SVAL_UPDATE_CHORD_PREWAIT_US - 1, 90}, {SVAL_UPDATE_CHORD_PREWAIT_US, SVAL_UPDATE_CHORD_POSTWAIT_US}, {2000, 65535}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pre  = cases[i][0];
        post = cases[i][1];
        update_gesture_timing(&pre, &post);
        CHECK_EQ(pre, cases[i][0] > SVAL_UPDATE_CHORD_PREWAIT_US ? cases[i][0] : SVAL_UPDATE_CHORD_PREWAIT_US);
        CHECK_EQ(post, cases[i][1] > SVAL_UPDATE_CHORD_POSTWAIT_US ? cases[i][1] : SVAL_UPDATE_CHORD_POSTWAIT_US);
    }
    CHECK(SVAL_UPDATE_CHORD_PREWAIT_US >= 500 && SVAL_UPDATE_CHORD_POSTWAIT_US >= 500); // Scan Lab's safe reference
    // once the chord is made, or the wait ends, the host's timing applies again
    chord();
    CHECK(update_gesture_done(NULL));
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    pre  = 0;
    post = 0;
    update_gesture_timing(&pre, &post);
    CHECK(pre == 0 && post == 0);
}

static void test_session_rebind(void) {
    fresh(0x3000);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, 1), UPDATE_INVALID); // no session
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, 0), UPDATE_INVALID); // not bound until ARM
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, 0x1000), UPDATE_OK);
    // wrong nonce: OTHER_CLIENT, nothing moves
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, session_nonce ^ 1), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_REBIND, session_nonce + 1), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_image(CLIENT_A, 0x1000, 0x1012), UPDATE_OK);
    CHECK_EQ(send_image(CLIENT_B, 0x1012, 0x1024), UPDATE_OTHER_CLIENT);
    // right nonce: the session moves to B; A is refused from now on
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(rsp[1], UPDATE_STATE_RECEIVING);
    CHECK_EQ(send_image(CLIENT_A, 0x1012, 0x1024), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_OTHER_CLIENT); // even a repeat
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_image(CLIENT_B, 0x1012, img_len), UPDATE_OK);
    // and again, B to C, mid-way through the session
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // REBIND to the same client is harmless
    CHECK_EQ(send_nonce(CLIENT_C, UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    // REBIND keeps a session alive across a 50 s renewal cycle (D3)
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    uint32_t clients[] = {CLIENT_A, CLIENT_B, CLIENT_C, CLIENT_A + 7};
    uint32_t off       = 0;
    for (int round = 1; round < 4; round++) {
        for (int i = 0; i < 20; i++) {
            wait_ms(2500); // 50 s of slow transfer per round
            uint8_t n = 18;
            CHECK_EQ(send_chunk(clients[round - 1], off, n, img + off), UPDATE_OK);
            off += n;
        }
        CHECK_EQ(send_nonce(clients[round], UPDATE_OP_REBIND, session_nonce), UPDATE_OK);
        CHECK_EQ(send_chunk(clients[round - 1], off, 18, img + off), UPDATE_OTHER_CLIENT);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
}

static void test_session_clients_and_abort(void) {
    // a second client while one loads the manifest
    fresh(0x3000);
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OTHER_CLIENT);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_ARM, NULL, 0), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OTHER_CLIENT);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    session_nonce = get32le(rsp + 1);
    // then while A's session runs: B is refused everywhere, with no effect
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_BUSY);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_ARM, NULL, 0), UPDATE_BUSY);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_BEGIN, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce ^ 0x100), UPDATE_OTHER_CLIENT); // the session, wrong nonce
    chord();
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_BEGIN, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_chunk(CLIENT_B, 0, 18, img), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_END, session_nonce), UPDATE_OTHER_CLIENT);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // anyone may read STATUS
    CHECK_EQ(rsp[1], UPDATE_STATE_RECEIVING);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    uint8_t c[6];
    put32(c, session_nonce);
    c[4]         = body_crc() & 0xFF;
    c[5]         = (body_crc() >> 8) & 0xFF;
    commit_avail = true;
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_COMMIT, c, 6), UPDATE_OTHER_CLIENT);
    // a COMMIT for another image (wrong CRC) is refused and the session stays verified
    c[4] ^= 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_BAD_HASH);
    put32(c, session_nonce + 1);
    c[4] ^= 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_OTHER_CLIENT);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK_EQ(commit_runs, 0);

    // ABORT: from the session (client and nonce), in each state; never a flash op
    const update_state_t stops[] = {UPDATE_STATE_MANIFEST_LOADING, UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_ERASING, UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFIED};
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        fresh(0x3000);
        memset(update_host_flash + SVAL_UPDATE_BASE, 0x00, 0x3000);
        switch (stops[i]) {
            case UPDATE_STATE_MANIFEST_LOADING:
                CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
                break;
            case UPDATE_STATE_CONFIRM_WAIT:
                CHECK(to_confirm_wait(CLIENT_A));
                break;
            case UPDATE_STATE_ERASING:
                CHECK(to_confirm_wait(CLIENT_A));
                chord();
                CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
                pass();
                pass(); // the manifest check, then the first sector
                break;
            case UPDATE_STATE_RECEIVING:
                CHECK(to_receiving(CLIENT_A));
                CHECK_EQ(send_image(CLIENT_A, 0, 0x180), UPDATE_OK);
                break;
            default:
                CHECK(to_verified(CLIENT_A));
                break;
        }
        CHECK_EQ(updater_state(), stops[i]);
        int ops = erase_ops + program_ops;
        snap();
        CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, session_nonce), UPDATE_OTHER_CLIENT);
        CHECK_EQ(updater_state(), stops[i]);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
        CHECK_EQ(rsp[1], UPDATE_STATE_IDLE);
        CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
        wait_ms(5);
        CHECK_EQ(erase_ops + program_ops, ops); // ABORT does not erase
        CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
        CHECK_EQ(led_now, UPDATE_LED_NONE);
    }
    // in IDLE: nothing to do
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    // ERROR: anyone wrapped may clear it, but not without the wrapper
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, img_len, 1, img), UPDATE_OVERRUN);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send_nonce(0, UPDATE_OP_ABORT, 0), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_BUSY); // ABORT first
    CHECK_EQ(send_nonce(CLIENT_B, UPDATE_OP_ABORT, 0), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    // and a new session can start straight away, from anyone
    CHECK(to_confirm_wait(CLIENT_B));
}

static void test_session_chunks(void) {
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    int ops = program_ops;
    // lengths
    CHECK_EQ(send_chunk(CLIENT_A, 0, 0, img), UPDATE_INVALID);
    CHECK_EQ(send_chunk(CLIENT_A, 0, T_CHUNK + 1, img), UPDATE_INVALID); // one more than the value bytes hold
    CHECK_EQ(send_chunk(CLIENT_A, 0, 0xFF, img), UPDATE_INVALID);
    CHECK_EQ(get24le(rsp + 1), 0);
    CHECK_EQ(program_ops, ops);
    // out of order: ahead of the next offset, refused with the offset expected
    CHECK_EQ(send_chunk(CLIENT_A, 18, 18, img + 18), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(get24le(rsp + 1), 0);
    CHECK_EQ(send_chunk(CLIENT_A, 0xFFFFFF, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // in order across a page boundary: one page programmed when it fills
    CHECK_EQ(send_image(CLIENT_A, 0, 252), UPDATE_OK);
    CHECK_EQ(program_ops, ops);
    CHECK_EQ(send_chunk(CLIENT_A, 252, 18, img + 252), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 270);
    CHECK_EQ(program_ops, ops + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // a repeat of an earlier chunk is acknowledged, not rewritten, even with other bytes
    uint8_t junk[18];
    memset(junk, 0x00, sizeof(junk));
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, junk), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 270);
    CHECK_EQ(send_chunk(CLIENT_A, 252, 18, img + 252), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, 260, 10, img + 260), UPDATE_OK);
    CHECK_EQ(program_ops, ops + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // overlapping the next offset is not a repeat
    CHECK_EQ(send_chunk(CLIENT_A, 260, 18, img + 260), UPDATE_OUT_OF_ORDER);
    // END too early
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // the rest; the last chunk ends exactly at image_len
    CHECK_EQ(send_image(CLIENT_A, 270, img_len), UPDATE_OK);
    CHECK_EQ(program_ops, ops + (int)(img_len / 256));
    // overlapping the end of what was staged is not a repeat either
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 4, 18, img), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // a chunk past the end: OVERRUN, latched
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, img_len - 10), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 18, img + img_len - 10), UPDATE_OVERRUN);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
    CHECK_EQ(rsp[12], UPDATE_OVERRUN);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    CHECK_EQ(send_chunk(CLIENT_A, img_len - 10, 10, img + img_len - 10), UPDATE_INVALID); // nothing more after ERROR
    // CHUNK and END out of state
    fresh(0x3000);
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_INVALID);
    CHECK(to_confirm_wait(CLIENT_A));
    CHECK_EQ(send_chunk(CLIENT_A, 0, 18, img), UPDATE_INVALID);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_INVALID);
    // a flash fault while programming: FLASH_ERR, latched
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    tear_next = 1;
    CHECK_EQ(send_image(CLIENT_A, 0, 256), UPDATE_FLASH_ERR);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

static void test_session_timeouts(void) {
    // no op from the session for 30 s: TIMEOUT, latched, in every waiting state
    const update_state_t stops[] = {UPDATE_STATE_CONFIRM_WAIT, UPDATE_STATE_RECEIVING, UPDATE_STATE_VERIFIED};
    for (size_t i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        fresh(0x3000);
        if (stops[i] == UPDATE_STATE_CONFIRM_WAIT) {
            CHECK(to_confirm_wait(CLIENT_A));
            chord(); // chord made, but no BEGIN
        } else if (stops[i] == UPDATE_STATE_RECEIVING) {
            CHECK(to_receiving(CLIENT_A));
        } else {
            CHECK(to_verified(CLIENT_A));
        }
        // another client's STATUS does not keep it alive
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK); // the session's does
        for (int k = 0; k < 29; k++) {
            wait_ms(1000);
            CHECK_EQ(send(CLIENT_B, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        }
        CHECK_EQ(updater_state(), stops[i]);
        wait_ms(1100);
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        CHECK_EQ(rsp[12], UPDATE_TIMEOUT);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    }
    // the session's own ops keep it alive: a slow transfer, 25 s between chunks
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    for (uint32_t off = 0; off < 18 * 4; off += 18) {
        wait_ms(25000);
        CHECK_EQ(send_chunk(CLIENT_A, off, 18, img + off), UPDATE_OK);
    }
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);
    // a half-sent manifest is dropped quietly after 30 s (it was never shown)
    fresh(0x3000);
    uint8_t a[22] = {0, 20};
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    wait_ms(SVAL_UPDATE_SESSION_TIMEOUT_MS + 10);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK_EQ(send(CLIENT_B, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    // COMMITTING: no session timeout and no ABORT (D23); the commit runs 100 ms
    // after the reply, and a refusal it returns is latched
    fresh(0x3000);
    CHECK(to_verified(CLIENT_A));
    commit_avail  = true;
    commit_result = UPDATE_FLASH_ERR;
    uint8_t c[6];
    put32(c, session_nonce);
    c[4] = body_crc() & 0xFF;
    c[5] = (body_crc() >> 8) & 0xFF;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);
    pass();
    CHECK_EQ(updater_state(), UPDATE_STATE_COMMITTING);
    wait_ms(200);
    CHECK_EQ(commit_runs, 1);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

static update_status_t refusal_after_begin(void) {
    if (!to_confirm_wait(CLIENT_A)) return (update_status_t)rsp[0];
    chord();
    if (send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce) != UPDATE_ACCEPTED) return (update_status_t)rsp[0];
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0);
    return updater_state() == UPDATE_STATE_ERROR ? (update_status_t)rsp[12] : UPDATE_OK;
}

static void mutate(void (*f)(sval_update_manifest_t *)) {
    f(blob_manifest());
}

static void m_hand(sval_update_manifest_t *m) {
    m->hand = UPDATE_HAND_LEFT;
}
static void m_pointing(sval_update_manifest_t *m) {
    m->pointing_id = UPDATE_POINTING_PMW3360;
}
static void m_auto(sval_update_manifest_t *m) {
    m->pointing_id = UPDATE_POINTING_AUTO;
}
static void m_hand_pointing(sval_update_manifest_t *m) {
    m->hand        = UPDATE_HAND_LEFT;
    m->pointing_id = UPDATE_POINTING_TRACKPOINT;
}
static void m_large(sval_update_manifest_t *m) {
    m->image_len = SVAL_UPDATE_MAX_IMAGE + 0x100;
}
static void m_unaligned(sval_update_manifest_t *m) {
    m->image_len = 0x3010;
}
static void m_storage(sval_update_manifest_t *m) {
    m->storage_format = SVAL_UPDATE_STORAGE_FORMAT - 1;
}
static void m_magic(sval_update_manifest_t *m) {
    m->magic ^= 0x100;
}
static void m_v1(sval_update_manifest_t *m) {
    m->manifest_ver = 1;
}
static void m_reserved0(sval_update_manifest_t *m) {
    m->reserved0 = 1;
} // version 1's key_id
static void m_diagnostic(sval_update_manifest_t *m) {
    m->flags = UPDATE_FLAG_DIAGNOSTIC;
}
static void m_release(sval_update_manifest_t *m) {
    m->flags = UPDATE_FLAG_RELEASE;
}
static void m_hash(sval_update_manifest_t *m) {
    m->sha512[17] ^= 0x20;
}

static void test_session_refusals(void) {
    // refused at ARM, latched: length, floors, magic, version 1, a nonzero
    // reserved byte (never the hand or pointing device: D33)
    struct {
        void (*mutate)(sval_update_manifest_t *);
        update_status_t want;
    } const arm_cases[] = {
        {m_large, UPDATE_TOO_LARGE}, {m_unaligned, UPDATE_BAD_IMAGE}, {m_storage, UPDATE_STORAGE}, {m_magic, UPDATE_INVALID}, {m_v1, UPDATE_UNSUPPORTED}, {m_reserved0, UPDATE_INVALID},
    };
    for (size_t i = 0; i < sizeof(arm_cases) / sizeof(arm_cases[0]); i++) {
        fresh(0x3000);
        mutate(arm_cases[i].mutate);
        CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), arm_cases[i].want);
        CHECK_EQ(get32le(rsp + 1), 0); // no nonce
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        pass();
        CHECK_EQ(led_now, UPDATE_LED_ERROR);
        CHECK_EQ(erase_ops + program_ops, 0);
    }
    // the security epoch floor
    fresh(0x3000);
    self_dev.security_epoch = 1;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_EPOCH);
    // a release build refuses a DIAGNOSTIC image, and takes a plain or RELEASE one
    fresh(0x3000);
    self_dev.release_build = true;
    mutate(m_diagnostic);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_BAD_IMAGE);
    fresh(0x3000);
    self_dev.release_build = true;
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    fresh(0x3000);
    self_dev.release_build = true;
    mutate(m_release);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);

    // nothing is refused after BEGIN on a manifest ARM took: BEGIN checks it
    // again and erases
    fresh(0x3000);
    CHECK_EQ(refusal_after_begin(), UPDATE_OK);
    CHECK_EQ(updater_state(), UPDATE_STATE_RECEIVING);

    // refused at VERIFYING_IMAGE: the bytes are not the hashed ones, or the
    // hashed image is structurally bad
    struct {
        uint32_t        at;
        uint32_t        value;
        update_status_t want;
    } const image_cases[] = {
        {0x40, 0x12345678, UPDATE_BAD_HASH},   // a byte differs from the manifest's hash (made over the original)
        {0x10, 0, UPDATE_BAD_IMAGE},           // boot2 CRC
        {0x100, 0x1FFFFFF0, UPDATE_BAD_IMAGE}, // initial SP
        {0x104, 0x100001F6, UPDATE_BAD_IMAGE}, // reset vector even
        {0x104, 0x10003001, UPDATE_BAD_IMAGE}, // reset vector past the image
    };
    for (size_t i = 0; i < sizeof(image_cases) / sizeof(image_cases[0]); i++) {
        fresh(0x3000);
        put32(img + image_cases[i].at, image_cases[i].value);
        if (image_cases[i].want == UPDATE_BAD_IMAGE) {
            crypto_sha512(blob_manifest()->sha512, img, img_len); // hashed as it is
        }
        CHECK(to_receiving(CLIENT_A));
        CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
        passes_until(UPDATE_STATE_VERIFIED, 2000);
        CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        CHECK_EQ(rsp[12], image_cases[i].want);
        CHECK_EQ(get16le(rsp + 19), 0); // no CRC offered for COMMIT
    }
    fresh(0x3000);
    mutate(m_hash);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);

    // settings writes failing (D12): ARM, BEGIN and COMMIT refuse, with no state change
    fresh(0x3000);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);
    port_failing = true;
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(rsp[21] & 0x02, 0x02);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    port_failing = false;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    session_nonce = get32le(rsp + 1);
    chord();
    port_failing = true;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_CONFIRM_WAIT);
    port_failing = false;
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_RECEIVING, 2000);
    CHECK_EQ(send_image(CLIENT_A, 0, img_len), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 2000);
    uint8_t c[6];
    put32(c, session_nonce);
    c[4]         = body_crc() & 0xFF;
    c[5]         = (body_crc() >> 8) & 0xFF;
    commit_avail = true;
    port_failing = true;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    port_failing = false;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);

    // a die that is not 16 MiB (D26): unavailable, nothing touched
    fresh(0x3000);
    capacity = 0x15;
    snap();
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_UNAVAILABLE);
    CHECK_EQ(rsp[21] & 0x01, 0);
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_UNAVAILABLE);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    CHECK(memcmp(before, update_host_flash, sizeof(before)) == 0);
    capacity = 0x18;

    // MANIFEST rules
    fresh(0x3000);
    uint8_t a[22] = {20, 20};
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OUT_OF_ORDER); // must start at 0
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    a[0] = 0;
    a[1] = T_MPIECE + 1; // one more than the value bytes hold
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_IDLE);
    a[1] = 0;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID);
    a[0] = 89;
    a[1] = 20;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID); // past 108
    a[0] = 108;
    a[1] = 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID); // where a version-1 signature began
    a[0] = 88;
    a[1] = 20;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OUT_OF_ORDER); // ends at 108: in bounds
    CHECK_EQ(load_manifest(CLIENT_A), UPDATE_OK);                                    // offset 0 restarts
    CHECK_EQ(rsp[1], UPDATE_MANIFEST_BYTES);
    a[0] = 40;
    a[1] = 4;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OUT_OF_ORDER);
    CHECK_EQ(rsp[1], UPDATE_MANIFEST_BYTES);
    // a whole manifest arms: ARM takes exactly 108 bytes
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_OK);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, get32le(rsp + 1)), UPDATE_OK);
    // ARM before the whole manifest
    fresh(0x3000);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_INVALID);
    a[0] = 0;
    a[1] = 20;
    memcpy(a + 2, blob, 20);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    // ARM twice: the second is BUSY and the nonce stays
    CHECK(to_confirm_wait(CLIENT_A));
    uint32_t first = session_nonce;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_ARM, NULL, 0), UPDATE_BUSY);
    chord();
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, first), UPDATE_ACCEPTED);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_BEGIN, first), UPDATE_INVALID); // once
}

// The other half's version by split presence (M2, V): INFO flags bit5 and
// STATUS byte 22. With no update just finished there is no light for it
// (D24): red only after an update, when the halves still differ.
static void test_session_presence(void) {
    static const struct {
        uint8_t presence;
        bool    mismatch;
    } cases[] = {
        {UPDATE_PRESENCE_NONE, false}, {UPDATE_PRESENCE_MATCH, false}, {UPDATE_PRESENCE_VERSION, true}, {UPDATE_PRESENCE_SAME_HAND, true}, {UPDATE_PRESENCE_INVALID, true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fresh(0x3000);
        port_split_presence = cases[i].presence;
        CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
        CHECK_EQ((rsp[21] & 0x20) != 0, cases[i].mismatch);
        CHECK_EQ(send(CLIENT_A, UPDATE_OP_STATUS, NULL, 0), UPDATE_OK);
        CHECK_EQ(rsp[22], cases[i].presence);
        pass();
        CHECK_EQ(led_now, UPDATE_LED_NONE);
        wait_ms(SVAL_UPDATE_AFTER_WAIT_MS + 1000);
        CHECK_EQ(led_now, UPDATE_LED_NONE);
        CHECK_EQ(updater_state(), UPDATE_STATE_IDLE); // a mismatch is not an updater error: nothing latched
        // a session shows its own LEDs over it
        CHECK(to_confirm_wait(CLIENT_A));
        pass();
        CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
        CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
        pass();
        CHECK_EQ(led_now, UPDATE_LED_NONE);
        // fixed: still nothing to show
        port_split_presence = UPDATE_PRESENCE_MATCH;
        pass();
        CHECK_EQ(led_now, UPDATE_LED_NONE);
    }
}

// The first boot after this half's own commit (the commit marker, D24): the
// rainbow until presence reads MATCH, then the green flash and nothing. Halves
// that still differ: the rainbow while idle for SVAL_UPDATE_AFTER_WAIT_MS,
// then red for SVAL_UPDATE_RED_MS, then nothing (INFO flags bit5 stays). A
// half whose other half never answers shows nothing after the wait.
static void after_commit_boot(uint8_t presence) {
    fresh(0x3000);
    master_marked = true; // "the reset came from the commit"
    updater_host_reset();
    port_split_presence = UPDATE_PRESENCE_NONE;
    led_seen            = 0;
    pass();
    CHECK(!master_marked); // taken by the first pass
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_UPDATING);
    wait_ms(800);
    port_split_presence = presence;
    pass();
}

static void test_session_after_commit(void) {
    // the halves match: green, then nothing; the ping carries DONE a while
    after_commit_boot(UPDATE_PRESENCE_MATCH);
    CHECK_EQ(led_now, UPDATE_LED_DONE);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_DONE);
    wait_ms(700);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_DONE);
    wait_ms(1500);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_NONE);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS + SVAL_UPDATE_RED_MS);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);
    CHECK_EQ(led_now, UPDATE_LED_NONE);

    // they still differ: rainbow, then red, then nothing
    after_commit_boot(UPDATE_PRESENCE_VERSION);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS - 2000);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0); // never red while it may still come right
    wait_ms(2000);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    CHECK_EQ(updater_split_light(), UPDATE_LIGHT_FAILED);
    wait_ms(SVAL_UPDATE_RED_MS - 1500); // the red began up to 800 ms before the check above
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    wait_ms(2000);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(send(0, UPDATE_OP_INFO, NULL, 0), UPDATE_OK);
    CHECK(rsp[21] & 0x20); // the host still sees it
    // a match after that shows nothing more (a new update opens a new window)
    port_split_presence = UPDATE_PRESENCE_MATCH;
    pass();
    CHECK_EQ(led_now, UPDATE_LED_NONE);

    // a session in the window: its own lights, and the wait starts again after it
    after_commit_boot(UPDATE_PRESENCE_VERSION);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS - 5000);
    CHECK(to_confirm_wait(CLIENT_A));
    pass();
    CHECK_EQ(led_now, UPDATE_LED_CONFIRM);
    wait_ms(10000);
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_ABORT, session_nonce), UPDATE_OK);
    pass();
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS - 2000);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(2500);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);

    // no other half at all: nothing after the wait, never red
    after_commit_boot(UPDATE_PRESENCE_NONE);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS + 1000);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);

    // an ordinary boot: no marker, no light
    fresh(0x3000);
    port_split_presence = UPDATE_PRESENCE_MATCH;
    led_seen            = 0;
    wait_ms(2000);
    CHECK_EQ(led_seen, 1 << UPDATE_LED_NONE);
}

// The half without USB (D24): it cannot see presence, so it follows the
// master's light from the presence ping (updater_split_light_seen).
static void slave_boot(bool marked) {
    fresh(0x3000);
    port_master   = false;
    master_marked = marked; // this half's own marker: updater.c runs as this half here
    updater_host_reset();
    led_seen = 0;
    pass();
}

static void ping_light(uint8_t light, bool has) {
    updater_split_light_seen(has, light);
    pass();
}

static void test_session_slave_lights(void) {
    // after its own commit: rainbow before the first ping, then as the master says
    slave_boot(true);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(1500);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    for (int i = 0; i < 20; i++) {
        ping_light(UPDATE_LIGHT_UPDATING, true);
        CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
        wait_ms(500);
    }
    ping_light(UPDATE_LIGHT_DONE, true);
    CHECK_EQ(led_now, UPDATE_LED_DONE);
    wait_ms(499);
    ping_light(UPDATE_LIGHT_DONE, true); // DONE stays on a few pings: one flash
    wait_ms(700);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    ping_light(UPDATE_LIGHT_DONE, true);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    ping_light(UPDATE_LIGHT_NONE, true);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS + 1000);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);

    // a master that sends no light (a release from before it): nothing to show
    slave_boot(true);
    ping_light(0, false);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS + 1000);
    CHECK_EQ(led_seen & (1 << UPDATE_LED_ERROR), 0);

    // no ping at all for a whole wait after its commit: red, then nothing
    slave_boot(true);
    wait_ms(SVAL_UPDATE_AFTER_WAIT_MS - 1000);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    wait_ms(1500);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    wait_ms(SVAL_UPDATE_RED_MS);
    CHECK_EQ(led_now, UPDATE_LED_NONE);

    // an ordinary boot: the master's own update shows here too; red as the master's
    slave_boot(false);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    ping_light(UPDATE_LIGHT_NONE, true);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    ping_light(UPDATE_LIGHT_UPDATING, true);
    CHECK_EQ(led_now, UPDATE_LED_PROGRESS);
    ping_light(UPDATE_LIGHT_FAILED, true);
    CHECK_EQ(led_now, UPDATE_LED_ERROR);
    ping_light(UPDATE_LIGHT_NONE, true);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    ping_light(UPDATE_LIGHT_FAILED, true);
    wait_ms(SVAL_UPDATE_RED_MS + 500);
    CHECK_EQ(led_now, UPDATE_LED_NONE); // red stops after SVAL_UPDATE_RED_MS whatever the pings say
    ping_light(UPDATE_LIGHT_FAILED, true);
    CHECK_EQ(led_now, UPDATE_LED_NONE);
    // the master's DONE after its own install: the flash here too
    ping_light(UPDATE_LIGHT_UPDATING, true);
    ping_light(UPDATE_LIGHT_DONE, true);
    CHECK_EQ(led_now, UPDATE_LED_DONE);
    port_master = true;
}

// Protocol 3's pieces at their bounds: the largest CHUNK and MANIFEST piece the
// value bytes hold are taken, one more is refused, and no reply reaches past
// the value bytes (send() checks every op).
static void test_session_max_pieces(void) {
    fresh(0x3000);
    // the manifest in two whole pieces and the rest
    CHECK_EQ(UPDATE_MANIFEST_BYTES / T_MPIECE, 2);
    uint8_t a[2 + T_MPIECE];
    a[0] = 0;
    a[1] = T_MPIECE;
    memcpy(a + 2, blob, T_MPIECE);
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_OK);
    CHECK_EQ(rsp[1], T_MPIECE);
    a[0] = T_MPIECE;
    a[1] = T_MPIECE + 1;
    CHECK_EQ(send(CLIENT_A, UPDATE_OP_MANIFEST, a, sizeof(a)), UPDATE_INVALID);
    CHECK_EQ(updater_state(), UPDATE_STATE_MANIFEST_LOADING);
    CHECK_EQ(rsp[1], T_MPIECE); // nothing taken
    updater_host_reset();
    CHECK(to_receiving(CLIENT_A));
    int ops = program_ops;
    // the largest CHUNK, then one across the first page boundary (240 + 50)
    CHECK_EQ(send_chunk(CLIENT_A, 0, T_CHUNK, img), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), T_CHUNK);
    CHECK_EQ(send_chunk(CLIENT_A, T_CHUNK, T_CHUNK + 1, img + T_CHUNK), UPDATE_INVALID);
    CHECK_EQ(get24le(rsp + 1), T_CHUNK); // nothing taken
    CHECK_EQ(send_image(CLIENT_A, T_CHUNK, 240), UPDATE_OK);
    CHECK_EQ(program_ops, ops);
    CHECK_EQ(send_chunk(CLIENT_A, 240, T_CHUNK, img + 240), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 240 + T_CHUNK);
    CHECK_EQ(program_ops, ops + 1);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, 256) == 0);
    // a repeat of the largest chunk is acknowledged
    CHECK_EQ(send_chunk(CLIENT_A, 0, T_CHUNK, img), UPDATE_OK);
    CHECK_EQ(get24le(rsp + 1), 240 + T_CHUNK);
    // the rest in whole chunks: the last ends exactly at image_len, which no
    // chunk size divides here (0x3000 = 245 * 50 + 46)
    CHECK(img_len % T_CHUNK != 0);
    CHECK_EQ(send_image(CLIENT_A, 240 + T_CHUNK, img_len), UPDATE_OK);
    CHECK_EQ(program_ops, ops + (int)(img_len / 256));
    CHECK_EQ(send_nonce(CLIENT_A, UPDATE_OP_END, session_nonce), UPDATE_ACCEPTED);
    passes_until(UPDATE_STATE_VERIFIED, 5000);
    CHECK_EQ(updater_state(), UPDATE_STATE_VERIFIED);
    CHECK(memcmp(update_host_flash + SVAL_UPDATE_BASE, img, img_len) == 0);
    // the largest chunk past the end: OVERRUN
    fresh(0x3000);
    CHECK(to_receiving(CLIENT_A));
    CHECK_EQ(send_image(CLIENT_A, 0, img_len - T_CHUNK + 1), UPDATE_OK);
    CHECK_EQ(send_chunk(CLIENT_A, img_len - T_CHUNK + 1, T_CHUNK, img), UPDATE_OVERRUN);
    CHECK_EQ(updater_state(), UPDATE_STATE_ERROR);
}

// Gentle (D33): an image built for the other hand, another pointing device, or
// auto against a single-module build (and the reverse) goes all the way: ARM,
// erase, verify and COMMIT, on a plain and on a release build.
static void test_session_mismatch_accepted(void) {
    void (*const cases[])(sval_update_manifest_t *) = {m_hand, m_pointing, m_auto, m_hand_pointing};
    for (int release = 0; release < 2; release++) {
        for (size_t i = 0; i <= sizeof(cases) / sizeof(cases[0]); i++) {
            fresh(0x3000);
            self_dev.release_build = release;
            if (i < sizeof(cases) / sizeof(cases[0])) {
                mutate(cases[i]);
            } else {
                self_dev.pointing_id = UPDATE_POINTING_AUTO; // a single-module image on an auto build
            }
            CHECK(to_verified(CLIENT_A));
            commit_avail  = true;
            commit_result = UPDATE_FLASH_ERR; // the mock: any result; it ran
            uint8_t c[6];
            put32(c, session_nonce);
            c[4] = body_crc() & 0xFF;
            c[5] = (body_crc() >> 8) & 0xFF;
            CHECK_EQ(send(CLIENT_A, UPDATE_OP_COMMIT, c, 6), UPDATE_ACCEPTED);
            wait_ms(200);
            CHECK_EQ(commit_runs, 1);
            CHECK_EQ(commit_len, img_len);
        }
    }
}

static void test_session(void) {
    int before_failures = failures;
    test_session_info_and_wrapper();
    test_session_happy_path();
    test_session_gesture();
    test_session_scan_timing();
    test_session_rebind();
    test_session_clients_and_abort();
    test_session_chunks();
    test_session_max_pieces();
    test_session_timeouts();
    test_session_refusals();
    test_session_mismatch_accepted();
    test_session_presence();
    test_session_after_commit();
    test_session_slave_lights();
    printf("session tests: %s\n", failures == before_failures ? "pass" : "FAIL");
}
