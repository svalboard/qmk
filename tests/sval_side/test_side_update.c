// Host test for Sval 0x33 (side) writes during a firmware update
// (keyboards/svalboard/side.c, its firmware part, against the mocks of
// stub/side_stub.h). While an update runs on either half, or the split link is
// paused for one, a side SET or CLEAR is refused (BUSY) for both targets: none
// crosses the paused link, none writes flash or restarts a half in the middle
// of its session. On the half without USB, a relayed write is refused while
// its session runs, and one taken just before waits, with its restart, until
// the session is over. Built and run by util/updater_test/run.sh.
#include <stdio.h>
#include "side_stub.h"
#include "side.h"
#include "identity.h"
#include "link_light.h"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

uint8_t         sim_top[0x10000];
static sio_hw_t sio    = {3};
sio_hw_t       *sio_hw = &sio;

static uint32_t now_ms;
static bool     master = true, connected = true, updating, paused;
static int      rpcs, record_writes, resets;

uint32_t timer_read32(void) {
    return now_ms;
}
void wait_us(uint32_t us) {
    (void)us;
}
void gpio_set_pin_input_high(pin_t pin) {
    (void)pin;
}
void serial_transport_repin(pin_t tx_pin, pin_t rx_pin) {
    (void)tx_pin;
    (void)rx_pin;
}
void soft_reset_keyboard(void) {
    resets++;
}
void clear_keyboard(void) {}
bool is_keyboard_master(void) {
    return master;
}
bool is_transport_connected(void) {
    return connected;
}
bool pre_process_record_user(uint16_t keycode, keyrecord_t *record) {
    (void)keycode;
    (void)record;
    return true;
}
bool sval_typing_pause_pre_process(keyrecord_t *record) {
    (void)record;
    return true;
}
uint16_t eeprom_read_word(const uint16_t *addr) {
    (void)addr;
    return 0xFFFF;
}
uint8_t eeprom_read_byte(const uint8_t *addr) {
    (void)addr;
    return 0xFF;
}
uint8_t sval_thumb_hand(void) {
    return 0; // no strap
}
uint8_t sval_thumb_hand_raw(void) {
    return 0;
}
bool sval_other_half_connected(void) {
    return connected;
}
bool sval_split_paused(void) {
    return paused;
}
bool updater_active(void) {
    return updating;
}
void sval_link_stopped(void) {}
bool identity_available(void) {
    return true;
}
bool identity_side_record_write(uint32_t sector, const void *data, uint32_t len) {
    memset(sim_top + (sector - 0xFF0000u), 0xFF, 4096);
    memcpy(sim_top + (sector - 0xFF0000u), data, len);
    record_writes++;
    return true;
}
// The other half's end of the relayed write: its sval_side_rpc() answer.
static uint8_t other_status = 7; // SIDE_ACCEPTED
bool           transaction_rpc_exec(int id, uint8_t in_len, const void *in, uint8_t out_len, void *out) {
    (void)id;
    (void)in_len;
    (void)in;
    rpcs++;
    uint8_t rsp[2] = {SVAL_SIDE_RPC_OP, other_status};
    memcpy(out, rsp, out_len < 2 ? out_len : 2);
    return true;
}

// Sval 0x33 SET: [op 1][target][side][source][flags], 16 bytes or more.
static uint8_t side_set(uint8_t target, uint8_t s, uint8_t flags) {
    uint8_t d[16] = {1, target, s, SVAL_SIDE_SRC_HOST, flags};
    sval_side_command(d, sizeof(d));
    return d[1];
}
static uint8_t side_clear(uint8_t target) {
    uint8_t d[16] = {2, target, 0};
    sval_side_command(d, sizeof(d));
    return d[1];
}
// The other half's callback for a relayed SET with the restart flag.
static uint8_t relayed_set(uint8_t s) {
    uint8_t in[5] = {SVAL_SIDE_RPC_OP, 1, s, SVAL_SIDE_SRC_HOST, 1}, out[2] = {0, 0};
    CHECK(sval_side_rpc(sizeof(in), in, sizeof(out), out));
    CHECK(out[0] == SVAL_SIDE_RPC_OP);
    return out[1];
}
static void tasks(int ms) {
    for (int i = 0; i < ms; i++) {
        sval_side_task();
        now_ms++;
    }
}

// The half with USB.
static void test_master(void) {
    master = true;
    // No update: the other half takes it, this half writes its own record.
    rpcs = record_writes = resets = 0;
    CHECK(side_set(1, SVAL_SIDE_LEFT, 0) == 7);
    CHECK(rpcs == 1);
    CHECK(side_set(0, SVAL_SIDE_RIGHT, 0) == SVAL_SIDE_OK);
    CHECK(record_writes == 1);
    // An update running (a relay, this half's session or the other half's):
    // both targets refused, nothing sent over the link, nothing written.
    updating = true;
    rpcs = record_writes = 0;
    CHECK(side_set(1, SVAL_SIDE_LEFT, 1) == SVAL_SIDE_BUSY);
    CHECK(side_clear(1) == SVAL_SIDE_BUSY);
    CHECK(side_set(0, SVAL_SIDE_RIGHT, 1) == SVAL_SIDE_BUSY);
    CHECK(side_clear(0) == SVAL_SIDE_BUSY);
    CHECK(rpcs == 0);
    CHECK(record_writes == 0);
    tasks(200);
    CHECK(resets == 0);
    // The link paused for one: the same.
    updating = false;
    paused   = true;
    CHECK(side_set(1, SVAL_SIDE_LEFT, 1) == SVAL_SIDE_BUSY);
    CHECK(side_set(0, SVAL_SIDE_RIGHT, 0) == SVAL_SIDE_BUSY);
    CHECK(rpcs == 0);
    CHECK(record_writes == 0);
    paused = false;
    // A bad request is still INVALID, update or not.
    updating = true;
    CHECK(side_set(2, SVAL_SIDE_LEFT, 0) == SVAL_SIDE_INVALID);
    CHECK(side_set(0, SVAL_SIDE_UNKNOWN, 0) == SVAL_SIDE_INVALID);
    updating = false;
    // A restart taken just before an update starts waits for its end.
    CHECK(side_set(0, SVAL_SIDE_RIGHT, 1) == SVAL_SIDE_OK);
    updating = true;
    tasks(500);
    CHECK(resets == 0);
    updating = false;
    tasks(2);
    CHECK(resets == 1);
}

// The half without USB, the relayed write's other end.
static void test_other_half(void) {
    master = false;
    // Its session running: refused, nothing pending, nothing written later.
    updating      = true;
    record_writes = resets = 0;
    CHECK(relayed_set(SVAL_SIDE_LEFT) == SVAL_SIDE_BUSY);
    tasks(300);
    updating = false;
    tasks(300);
    CHECK(record_writes == 0);
    CHECK(resets == 0);
    // Taken with no session; the session starts before the main loop writes
    // it: the write and the restart wait until the session is over.
    CHECK(relayed_set(SVAL_SIDE_LEFT) == 7);
    updating = true;
    tasks(500);
    CHECK(record_writes == 0);
    CHECK(resets == 0);
    updating = false;
    tasks(1);
    CHECK(record_writes == 1);
    CHECK(resets == 0);
    tasks(200);
    CHECK(resets == 1);
}

int main(void) {
    memset(sim_top, 0xFF, sizeof(sim_top));
    sval_side_init();
    test_master();
    test_other_half();
    printf("side writes during an update: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
