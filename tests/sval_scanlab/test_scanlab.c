// Host test for the Scan Lab settle probe across the split link
// (keyboards/svalboard/scanlab.c). On the half without USB a probe request
// arrives in the split link's slave thread, which can preempt the main loop
// mid-scan; the probe drives the rows, so it must run from housekeeping, between
// scans, and the half with USB waits for its result.
//
// Built and run by util/updater_test/run.sh, from a copy of scanlab.c in its own
// directory, so its #include "svalboard.h" finds the mock in stub/.
#include <stdio.h>
#include "quantum.h"
#include "scanlab.c"
#ifndef SCANLAB_REMOTE_PROBE_WAIT_MS
#    define SCANLAB_REMOTE_PROBE_WAIT_MS 100 // as scanlab.c, for a build from before it waited
#endif

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

// ------------------------------------------------------------------ mocks ---
int             scanlab_test_lock_depth;
saved_values_t  global_saved_values;
const pin_t     col_pins[MATRIX_COLS] = {1, 2, 3, 4, 5, 6};
static bool     master = true, left = true;
static int      row_writes; // rows driven or released by the probe
static uint32_t us_now, ms_now;

bool is_keyboard_master(void) {
    return master;
}
bool is_keyboard_left(void) {
    return left;
}
bool gpio_read_pin(pin_t pin) {
    (void)pin;
    return false;
}
uint32_t chVTGetSystemTimeX(void) {
    return ++us_now;
}
void wait_us(uint32_t us) {
    us_now += us;
}
void wait_ms(uint32_t ms) {
    ms_now += ms;
}
uint32_t timer_read32(void) {
    return ms_now;
}
uint32_t timer_elapsed32(uint32_t since) {
    return ms_now - since;
}
void     reset_keyboard(void) {}
uint32_t last_input_activity_elapsed(void) {
    return 0;
}
uint32_t last_matrix_activity_elapsed(void) {
    return 0;
}
uint32_t last_pointing_device_activity_elapsed(void) {
    return 0;
}
void sval_idle_status(sval_idle_status_t *st) {
    memset(st, 0, sizeof(*st));
}
uint8_t sval_hw_rev(void) {
    return 0;
}
bool sval_other_half_connected(void) {
    return true;
}
void sval_scan_timing(uint16_t *pre, uint16_t *post) {
    *pre = *post = 0;
}
uint8_t sval_pushed_mask(bool thumbs) {
    (void)thumbs;
    return 0;
}
void sval_row_drive_raw(uint8_t row, bool on) {
    (void)row;
    (void)on;
    row_writes++;
}
void unselect_row(uint8_t row) {
    (void)row;
    row_writes++;
}
uint32_t sval_scan_period_now(uint8_t *stage) {
    *stage = 0;
    return 0;
}
void sval_scan_stats(uint32_t *frame_us, uint16_t *led_us, uint8_t *stage) {
    *frame_us = 0;
    *led_us   = 0;
    *stage    = 0;
}
uint8_t sval_clock_mhz(void) {
    return 125;
}
uint8_t sval_deep_clock_mhz(void) {
    return 48;
}
uint8_t sval_timing_preset(uint16_t pre, uint16_t post) {
    (void)pre;
    (void)post;
    return 0xFF;
}
bool sval_optical_debounce_on(void) {
    return false;
}
void sval_link_stopped(void) {}

// The other half: its split RPC handler (svalboard.c scanlab_rpc_listener) runs
// scanlab_handle() in the slave thread; its main loop runs housekeeping between
// the master's requests, `sub_loops` times before each.
static int sub_loops = 1, sub_rpcs, sub_row_writes_in_rpc;
bool       transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    (void)id;
    bool was = master;
    master   = false;
    for (int i = 0; i < sub_loops; i++)
        scanlab_housekeeping();
    uint8_t req[SCANLAB_REQ_LEN] = {0}, rsp[SCANLAB_RSP_LEN];
    memcpy(req, in, in_len < SCANLAB_REQ_LEN ? in_len : SCANLAB_REQ_LEN);
    int before = row_writes;
    scanlab_handle(req, rsp);
    sub_row_writes_in_rpc += row_writes - before;
    sub_rpcs++;
    memcpy(out, rsp, out_len < SCANLAB_RSP_LEN ? out_len : SCANLAB_RSP_LEN);
    master = was;
    return true;
}

// ------------------------------------------------------------------ tests ---
static void reset(void) {
    scanlab_init();
    master                = true;
    left                  = true;
    sub_loops             = 1;
    sub_rpcs              = 0;
    sub_row_writes_in_rpc = 0;
}

// On the half without USB, the request only queues the probe: no row is driven
// in the slave thread. Housekeeping runs it.
static void test_split_request_waits_for_housekeeping(void) {
    reset();
    master                       = false;
    uint8_t req[SCANLAB_REQ_LEN] = {SCANLAB_OP_PROBE, 2}, rsp[SCANLAB_RSP_LEN];
    int     before               = row_writes;
    scanlab_handle(req, rsp);
    CHECK(row_writes == before); // nothing driven from the split link's thread
    CHECK(rsp[0] == 0 && rsp[1] == 2);
    uint8_t st_req[SCANLAB_REQ_LEN] = {SCANLAB_OP_STATUS};
    scanlab_handle(st_req, rsp);
    CHECK(rsp[15] == 0);

    scanlab_housekeeping();
    CHECK(row_writes > before);
    CHECK(scanlab_test_lock_depth == 0);
    scanlab_handle(st_req, rsp);
    CHECK(rsp[15] == 1 && rsp[16] == 2);
    before = row_writes;
    scanlab_housekeeping(); // once only
    CHECK(row_writes == before);

    // A row out of range queues nothing, as it probed nothing before.
    uint8_t bad[SCANLAB_REQ_LEN] = {SCANLAB_OP_PROBE, SCANLAB_ROWS};
    scanlab_handle(bad, rsp);
    scanlab_housekeeping();
    CHECK(row_writes == before);
}

// On the half with USB its own probe still runs at once.
static void test_local_probe_runs_at_once(void) {
    reset();
    uint8_t req[SCANLAB_REQ_LEN] = {SCANLAB_OP_PROBE, 1}, rsp[SCANLAB_RSP_LEN];
    int     before               = row_writes;
    scanlab_handle(req, rsp);
    CHECK(row_writes > before);
    CHECK(rsp[0] == 1 && rsp[1] == 1);
}

static uint8_t via[32];
static void    via_probe(uint8_t hand, uint8_t row) {
    memset(via, 0, sizeof(via));
    via[0] = id_custom_set_value;
    via[1] = SCANLAB_CHANNEL;
    via[2] = SCANLAB_OP_PROBE;
    via[3] = hand;
    via[4] = row;
    scanlab_via_command(via, sizeof(via));
}
static void via_get(uint8_t op, uint8_t hand, uint8_t row) {
    memset(via, 0, sizeof(via));
    via[0] = id_custom_get_value;
    via[1] = SCANLAB_CHANNEL;
    via[2] = op | (hand << 3) | row;
    scanlab_via_command(via, sizeof(via));
}

// The host asks the half with USB for a probe of the other half, then reads
// the result at once, as Keybard does: the result is there.
static void test_relayed_probe_is_ready_for_the_next_read(void) {
    reset();
    via_probe(1, 3); // the right half, from the left
    CHECK(sub_row_writes_in_rpc == 0);
    CHECK(via[3] == 1 && via[4] == 3);
    via_get(SCANLAB_OP_PROBE_ON, 1, 3);
    CHECK(via[3 + 20] == 1);
    via_get(SCANLAB_OP_PROBE_OFF, 1, 3);
    CHECK(via[3 + 20] == 1);
}

// The other half's main loop never gets to it: the wait gives up and the host
// reads "no data", without hanging.
static void test_relayed_probe_gives_up(void) {
    reset();
    sub_loops   = 0;
    uint32_t t0 = ms_now;
    via_probe(1, 0);
    CHECK(ms_now - t0 >= SCANLAB_REMOTE_PROBE_WAIT_MS && ms_now - t0 <= SCANLAB_REMOTE_PROBE_WAIT_MS + 2);
    CHECK(via[3] == 0);
}

int main(void) {
    test_split_request_waits_for_housekeeping();
    test_local_probe_runs_at_once();
    test_relayed_probe_is_ready_for_the_next_read();
    test_relayed_probe_gives_up();
    printf("scan lab: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
