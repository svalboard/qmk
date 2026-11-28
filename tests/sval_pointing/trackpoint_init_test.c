// Host test for the TrackPoint's start (keyboards/svalboard/trackpoint/trackpoint.c,
// pointing_device_init_kb()), against a simulated Sprintek stick on the RP2040 PS/2 driver's
// one receive queue (ACKs, replies and stream bytes all land in it, in order):
// - the Sprintek setup (E2 47 toggle, E2 81 write) waits for no reply the stick never sends,
//   so the start takes a few ms, not ~300 ms of receive timeouts (S21);
// - a stick that streamed motion before the setup stopped the stream (moved or drifting at
//   boot) is still found by Get Device ID, whatever was queued, so the build's search never
//   runs for a stick that is there (S24);
// - a stick that answers Get Device ID with a byte other than the ACK is asked again, and a
//   connector with nothing on it is asked once, as before.
#include <stdio.h>
#include <string.h>
#include "../../keyboards/svalboard/trackpoint/trackpoint.c"

static int failures = 0;
#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); \
            failures++;                                           \
        }                                                         \
    } while (0)

/* ---- the simulated stick ---- */
#define STICK_ID 0x01 // what it answers to Get Device ID (any value; checked as sent)

static const uint8_t packet[3] = {0x08, 0x03, 0xFB}; // no buttons, x +3, y -5

static uint8_t  queue[512];
static int      q_head, q_len;
static uint32_t now_ms;
static bool     present;   // a stick on the connector
static bool     streaming; // data reporting on
static bool     moving;    // the stick is pushed (or drifting): a streaming stick sends packets
static uint8_t  ram[256];  // the stick's RAM, as E2 47 / 80 / 81 see it
static uint8_t  e2_sub;    // 0: not in an E2 command, else its sub-command
static uint8_t  e2_args[2], e2_nargs;
static int      timeouts, f2_sent;
static uint8_t  resend_f2; // Get Device IDs the stick answers with RESEND (0xFE)
uint8_t         ps2_error;

static void push(uint8_t b) {
    if (q_len < (int)sizeof queue) queue[(q_head + q_len++) % sizeof queue] = b;
}
static bool pop(uint8_t *b) {
    if (q_len == 0) return false;
    *b     = queue[q_head];
    q_head = (q_head + 1) % sizeof queue;
    q_len--;
    return true;
}

// Time passes; a streaming stick that is moving sends a packet every 10 ms.
static void advance(uint32_t ms) {
    while (ms--) {
        now_ms++;
        if (present && streaming && moving && now_ms % 10 == 0) {
            for (int i = 0; i < 3; i++)
                push(packet[i]);
        }
    }
}

void wait_ms(uint32_t ms) {
    advance(ms);
}

// The IBM TrackPoint command set as the Sprintek has it: E2 then a sub-command and its
// arguments; each byte is ACKed. 47 <addr> <mask> toggles bits, 81 <addr> <value> writes,
// 80 <addr> reads (the value follows the last ACK), 51 recalibrates.
static uint8_t e2_arg_count(uint8_t sub) {
    switch (sub) {
        case 0x47:
        case 0x81:
            return 2;
        case 0x80:
            return 1;
        default:
            return 0;
    }
}

static void stick_receive(uint8_t data) {
    if (e2_sub == 0xFF) { // the byte after E2: the sub-command
        e2_sub   = data;
        e2_nargs = 0;
        push(PS2_ACK);
        if (e2_arg_count(e2_sub) == 0) e2_sub = 0;
        return;
    }
    if (e2_sub) {
        e2_args[e2_nargs++] = data;
        push(PS2_ACK);
        if (e2_nargs < e2_arg_count(e2_sub)) return;
        if (e2_sub == 0x47) ram[e2_args[0]] ^= e2_args[1];
        if (e2_sub == 0x81) ram[e2_args[0]] = e2_args[1];
        if (e2_sub == 0x80) push(ram[e2_args[0]]);
        e2_sub = 0;
        return;
    }
    switch (data) {
        case 0xE2:
            push(PS2_ACK);
            e2_sub = 0xFF;
            return;
        case 0xF2:
            f2_sent++;
            if (resend_f2) {
                resend_f2--;
                push(PS2_RESEND);
                return;
            }
            push(PS2_ACK);
            push(STICK_ID);
            return;
        case PS2_MOUSE_CMD_DISABLE_DATA_REPORTING:
            push(PS2_ACK);
            streaming = false;
            return;
        case PS2_MOUSE_CMD_ENABLE_DATA_REPORTING:
            push(PS2_ACK);
            streaming = true;
            return;
        default:
            push(PS2_ACK);
            return;
    }
}

// A host-to-device byte takes about 1 ms; the stick answers after whatever it already queued.
uint8_t ps2_host_send(uint8_t data) {
    advance(1);
    if (data == 0xF2 && !present) f2_sent++;
    if (present) stick_receive(data);
    return ps2_host_recv_response();
}

// Takes the next queued byte as the answer; waits up to 100 ms for one (ps2_vendor.c).
uint8_t ps2_host_recv_response(void) {
    uint8_t b;
    for (int waited = 0; waited <= 100; waited++) {
        if (pop(&b)) return b;
        advance(1);
    }
    timeouts++;
    ps2_error = PS2_ERR_NODATA;
    return 0;
}

bool pbuf_has_data(void) {
    return q_len > 0;
}

uint8_t ps2_host_recv(void) {
    uint8_t b = 0;
    pop(&b);
    return b;
}

// The PS/2 module (pointing_device_ps2.c) as trackpoint.c uses it.
void ps2_mouse_disable_data_reporting(void) {
    ps2_host_send(PS2_MOUSE_CMD_DISABLE_DATA_REPORTING);
}
void ps2_mouse_enable_data_reporting(void) {
    ps2_host_send(PS2_MOUSE_CMD_ENABLE_DATA_REPORTING);
}
report_mouse_t ps2_mouse_get_report(report_mouse_t r) {
    return r;
}
void sval_tp_scale(sval_tp_scale_t *s, uint16_t cpi, int32_t x, int32_t y, int32_t *ox, int32_t *oy) {
    (void)s, (void)cpi;
    *ox = x, *oy = y;
}
void pointing_device_init_user(void) {}

// The PS/2 module's start leaves the stream on (ps2_mouse_init_core); queued bytes are a
// packet's first ones, as the queue has them when the stick moved just before.
static void reset_stick(bool on, bool move, int queued, uint32_t start_ms) {
    q_head = q_len = 0;
    now_ms         = start_ms;
    present        = on;
    streaming      = true;
    moving         = move;
    memset(ram, 0, sizeof ram);
    e2_sub    = 0;
    timeouts  = 0;
    f2_sent   = 0;
    resend_f2 = 0;
    ps2_error = 0;
    for (int i = 0; i < queued; i++)
        push(packet[i % 3]);
}

// The settings the setup makes: drift correction off (bit 0x10 of 0xFA), the deadzone, no
// double Z tap, the Z-tap location untouched (its toggle mask is 0), and the stream on.
static void check_setup_applied(void) {
    CHECK(ram[0xFA] & 0x10);
    CHECK(ram[0x92] == 0x15);
    CHECK(ram[0x5E] == 0x00);
    CHECK(ram[0x2C] == 0x00);
    CHECK(streaming);
}

static void check_found(void) {
    sval_pointer_identity_t id = {0};
    sval_pointer_identity(&id);
    CHECK(id.detected == SVAL_SENSOR_TRACKPOINT);
    CHECK(id.flags & SVAL_POINTER_INIT_OK);
    CHECK(id.product_id == STICK_ID);
    CHECK(sval_pointer_retry_own() == SVAL_RETRY_OK);
}

// S21: a still stick starts in a few ms, with no receive waiting out its 100 ms.
static void test_start_waits_for_no_missing_reply(void) {
    reset_stick(true, false, 0, 1000);
    pointing_device_init_kb();
    printf("start, still stick: %u ms, %d timeouts\n", (unsigned)(now_ms - 1000), timeouts);
    CHECK(timeouts == 0);
    CHECK(now_ms - 1000 < 50);
    check_found();
    check_setup_applied();
    CHECK(q_len == 0); // nothing left over for the stream reader
}

// S21: with drift correction already off, the setup leaves bit 0x10 set (no toggle).
static void test_drift_bit_already_set(void) {
    reset_stick(true, false, 0, 1000);
    ram[0xFA] = 0x10;
    pointing_device_init_kb();
    CHECK(timeouts == 0);
    CHECK(ram[0xFA] == 0x10);
    check_found();
}

// S24: the stick moved (or drifted) at boot: motion bytes are queued, or arrive, before the
// setup stops the stream. Get Device ID still finds it, at every phase of the packet period.
static void test_found_while_streaming(void) {
    int missed = 0;
    for (int queued = 0; queued <= 3; queued++) {
        for (uint32_t phase = 0; phase < 10; phase++) {
            reset_stick(true, true, queued, 1000 + phase);
            int before = failures;
            pointing_device_init_kb();
            check_found();
            check_setup_applied();
            if (failures != before) missed++;
        }
    }
    if (missed) printf("  stick moving at start: %d of 40 cases wrong\n", missed);
}

// A stick that answers Get Device ID with RESEND first is asked again.
static void test_resend_asked_again(void) {
    reset_stick(true, false, 0, 1000);
    resend_f2 = 1;
    pointing_device_init_kb();
    CHECK(f2_sent == 2);
    check_found();
}

// Nothing on the connector: Get Device ID is sent once, as before, and nothing is found.
static void test_nothing_attached(void) {
    reset_stick(false, false, 0, 1000);
    pointing_device_init_kb();
    CHECK(f2_sent == 1);
    sval_pointer_identity_t id = {0};
    sval_pointer_identity(&id);
    CHECK(id.detected == SVAL_SENSOR_NO_RESPONSE);
    CHECK(!(id.flags & SVAL_POINTER_INIT_OK));
    CHECK(sval_pointer_retry_own() == SVAL_RETRY_STOP);
}

// A second start (the auto build can start the driver again) asks again: a stick found the
// first time and gone the second isn't reported as found.
static void test_second_start_asks_again(void) {
    reset_stick(true, false, 0, 1000);
    pointing_device_init_kb();
    check_found();
    reset_stick(false, false, 0, 5000);
    pointing_device_init_kb();
    sval_pointer_identity_t id = {0};
    sval_pointer_identity(&id);
    CHECK(id.detected == SVAL_SENSOR_NO_RESPONSE);
}

int main(void) {
    test_start_waits_for_no_missing_reply();
    test_drift_bit_already_set();
    test_found_while_streaming();
    test_resend_asked_again();
    test_nothing_attached();
    test_second_start_asks_again();
    printf(failures ? "%d FAILED\n" : "OK\n", failures);
    return failures != 0;
}
