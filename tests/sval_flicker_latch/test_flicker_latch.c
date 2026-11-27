// Host test for the flicker latch (keyboards/svalboard/flicker_latch.c, Sval 0x3D):
// a one-scan bounce on the raw rows latches and counts, a read clears it, and
// the half with USB answers for both halves. The other half is a canned split
// RPC reply. Built and run by util/updater_test/run.sh.
#include <stdio.h>
#include "quantum.h"
#include "../../keyboards/svalboard/flicker_latch.c"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

static bool    left = true, connected = true;
static uint8_t other_reply[SVAL_FLICKER_RPC_REPLY];
static int     rpcs;
int            sval_test_lock_depth, sval_test_locks; // stub/atomic_util.h
static int     rpcs_while_locked;
bool           is_keyboard_master(void) {
    return true;
}
bool is_keyboard_left(void) {
    return left;
}
bool is_transport_connected(void) {
    return connected;
}
bool transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    (void)id;
    (void)in_len;
    (void)in;
    rpcs++;
    if (sval_test_lock_depth) rpcs_while_locked++;
    memcpy(out, other_reply, out_len < sizeof(other_reply) ? out_len : sizeof(other_reply));
    return true;
}

static uint8_t buf[57];
static void    read_latch(void) {
    memset(buf, 0, sizeof(buf));
    buf[0] = SVAL_FLICKER_READ;
    sval_flicker_command(buf, sizeof(buf));
}
static void read_counts(uint8_t half) {
    memset(buf, 0, sizeof(buf));
    buf[0] = SVAL_FLICKER_COUNTS;
    buf[1] = half;
    sval_flicker_command(buf, sizeof(buf));
}
static void frame(matrix_row_t r0, matrix_row_t r3) {
    matrix_row_t rows[HALF_ROWS] = {r0, 0, 0, r3, 0};
    sval_flicker_frame(rows, HALF_ROWS);
}
#define RAW(i) buf[4 + (i)]
#define PRESSED(i) buf[4 + MATRIX_ROWS + (i)]
#define RELEASED(i) buf[4 + 2 * MATRIX_ROWS + (i)]

static void test_one_scan_bounce(void) {
    frame(0, 0);
    frame(0, 1 << 4); // one scan down
    frame(0, 0);      // and up again
    read_latch();
    CHECK(buf[0] == SVAL_FLICKER_READ && buf[1] == 0);
    CHECK(buf[3] == MATRIX_ROWS);
    CHECK(RAW(3) == 0);
    CHECK(PRESSED(3) == (1 << 4));
    CHECK(RELEASED(3) == (1 << 4));
    read_latch(); // a read clears it
    CHECK(PRESSED(3) == 0 && RELEASED(3) == 0);
    read_counts(0);
    CHECK(buf[2] == 1 && buf[3] == 0);
    CHECK(buf[4 + 3 * MATRIX_COLS + 4] == 2); // down and up
    read_counts(0);
    CHECK(buf[4 + 3 * MATRIX_COLS + 4] == 0);
}

static void test_held_key_and_counts_saturate(void) {
    frame(1, 0);
    read_latch();
    CHECK(RAW(0) == 1 && PRESSED(0) == 1 && RELEASED(0) == 0);
    for (int i = 0; i < 300; i++)
        frame(i & 1 ? 1 : 0, 0);
    read_counts(0);
    CHECK(buf[4 + 0] == 255);
}

static void test_other_half_merged(void) {
    memset(other_reply, 0, sizeof(other_reply));
    other_reply[0]                     = SVAL_FLICKER_RPC_OP;
    other_reply[1 + 2]                 = 0x08; // raw row 2 of the right half: absolute row 7
    other_reply[1 + HALF_ROWS + 2]     = 0x08;
    other_reply[1 + 2 * HALF_ROWS + 4] = 0x01;
    frame(0, 0);
    read_latch();
    CHECK(buf[2] == 3); // both blocks
    CHECK(RAW(7) == 0x08 && PRESSED(7) == 0x08 && RELEASED(9) == 0x01);
    connected = false;
    read_latch();
    CHECK(buf[2] == 1); // only the left block
    connected = true;
    memset(other_reply, 0, sizeof(other_reply));
    other_reply[0]                       = SVAL_FLICKER_RPC_OP;
    other_reply[1 + 2 * MATRIX_COLS + 3] = 5;
    read_counts(1);
    CHECK(buf[2] == 1 && buf[3] == 1 && buf[4 + 2 * MATRIX_COLS + 3] == 5);
}

static void test_right_half_with_usb(void) {
    left = false;
    frame(0x02, 0);
    read_latch();
    CHECK(RAW(5) == 0x02); // this half's rows are 5-9
    left = true;
}

static void test_split_rpc_side(void) {
    frame(0, 0);
    frame(0x04, 0);
    uint8_t in[2] = {SVAL_FLICKER_RPC_OP, SVAL_FLICKER_READ}, out[SVAL_FLICKER_RPC_REPLY];
    CHECK(sval_flicker_split_rpc(sizeof(in), in, sizeof(out), out));
    CHECK(out[0] == SVAL_FLICKER_RPC_OP && out[1] == 0x04 && out[1 + HALF_ROWS] == 0x04);
    uint8_t other[2] = {0xAE, 0};
    CHECK(!sval_flicker_split_rpc(sizeof(other), other, sizeof(out), out));
}

static void test_short_and_bad(void) {
    uint8_t s[10] = {SVAL_FLICKER_READ};
    sval_flicker_command(s, sizeof(s));
    CHECK(s[1] == 3);
    memset(buf, 0, sizeof(buf));
    buf[0] = 9;
    sval_flicker_command(buf, sizeof(buf));
    CHECK(buf[1] == 1);
}

// On the half without USB a read comes in the split link's slave thread and
// can preempt a scan: the scan's update and the read's copy-and-clear each run
// under the kernel lock, and the lock is never held across a split transaction.
static void test_scan_and_reads_hold_the_lock(void) {
    int locks = sval_test_locks;
    frame(0, 0);
    CHECK(sval_test_locks == locks + 1 && sval_test_lock_depth == 0);
    uint8_t in[2] = {SVAL_FLICKER_RPC_OP, SVAL_FLICKER_READ}, out[SVAL_FLICKER_RPC_REPLY];
    locks         = sval_test_locks;
    CHECK(sval_flicker_split_rpc(sizeof(in), in, sizeof(out), out));
    CHECK(sval_test_locks == locks + 1 && sval_test_lock_depth == 0);
    in[1] = SVAL_FLICKER_COUNTS;
    locks = sval_test_locks;
    CHECK(sval_flicker_split_rpc(sizeof(in), in, sizeof(out), out));
    CHECK(sval_test_locks == locks + 1 && sval_test_lock_depth == 0);
    rpcs_while_locked = 0;
    read_latch();
    read_counts(1);
    CHECK(rpcs_while_locked == 0 && sval_test_lock_depth == 0);
}

int main(void) {
    test_one_scan_bounce();
    test_held_key_and_counts_saturate();
    test_other_half_merged();
    test_right_half_with_usb();
    test_split_rpc_side();
    test_short_and_bad();
    test_scan_and_reads_hold_the_lock();
    printf("flicker latch: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
