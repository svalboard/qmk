// Host test for the TrackPoint recalibrate command (keyboards/svalboard/
// trackpoint/trackpoint.c, recalibrate_pointer()), against a simulated stick
// that streams 3-byte packets every 10 ms into the same receive queue its
// command ACKs land in, as the RP2040 PS/2 driver has it (F8):
// - E2 51 goes out with the stream stopped, so each command reads its own ACK;
// - the stream is on again afterwards;
// - every byte left for the reader is part of a whole, aligned packet, so no
//   ACK or motion byte is ever read as a packet's button byte.
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
// One packet: no buttons (head 0x08, the always-one bit), x +3, y -5. Its x
// byte has the left and right button bits set, so a packet read one byte out
// of step reports both buttons.
static const uint8_t packet[3] = {0x08, 0x03, 0xFB};

static uint8_t  queue[512];
static int      q_head, q_len;
static uint32_t now_ms;
static bool     streaming;
static bool     recalibrated_while_streaming, recalibrated;
static uint8_t  last_cmd;
static uint8_t  drop_enables; // enable requests the stick ignores (busy recalibrating)
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

// Time passes; a streaming stick sends a packet every 10 ms.
static void advance(uint32_t ms) {
    while (ms--) {
        now_ms++;
        if (streaming && now_ms % 10 == 0) {
            for (int i = 0; i < 3; i++)
                push(packet[i]);
        }
    }
}

void wait_ms(uint32_t ms) {
    advance(ms);
}

// A host-to-device byte takes about 1 ms; the stick ACKs it after whatever it
// already queued, and acts on it.
uint8_t ps2_host_send(uint8_t data) {
    advance(1);
    if (data == PS2_MOUSE_CMD_ENABLE_DATA_REPORTING && drop_enables) {
        drop_enables--;
        return ps2_host_recv_response();
    }
    push(PS2_ACK);
    if (data == PS2_MOUSE_CMD_DISABLE_DATA_REPORTING) streaming = false;
    if (data == PS2_MOUSE_CMD_ENABLE_DATA_REPORTING) streaming = true;
    if (last_cmd == 0xE2 && data == 0x51) {
        recalibrated = true;
        if (streaming) recalibrated_while_streaming = true;
    }
    last_cmd = data;
    return ps2_host_recv_response();
}

// Takes the next queued byte as the answer; waits up to 100 ms for one.
uint8_t ps2_host_recv_response(void) {
    uint8_t b;
    for (int waited = 0; waited <= 100; waited++) {
        if (pop(&b)) return b;
        advance(1);
    }
    ps2_error = 0x20;
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

// Reads what the stream reader would: 3 bytes per packet.
static void check_stream_aligned(const char *when) {
    advance(55);
    CHECK(q_len >= 3);
    CHECK(q_len % 3 == 0);
    int bad = 0;
    while (q_len >= 3) {
        uint8_t p[3];
        for (int i = 0; i < 3; i++)
            pop(&p[i]);
        if (memcmp(p, packet, 3) != 0) bad++;
    }
    if (bad) printf("  %s: %d packet(s) out of step\n", when, bad);
    CHECK(bad == 0);
}

static void reset_stick(int queued, uint32_t start_ms) {
    q_head = q_len = 0;
    now_ms         = start_ms;
    streaming      = true;
    recalibrated = recalibrated_while_streaming = false;
    last_cmd                                    = 0;
    for (int i = 0; i < queued; i++)
        push(packet[i % 3]);
}

// The stick is drifting (streaming) when SV_RECALIBRATE_POINTER is pressed,
// with nothing queued, part of a packet (the rest still on the wire) or a whole
// one, at each phase of the 10 ms packet period.
static void test_recalibrate_while_streaming(void) {
    for (int queued = 0; queued <= 3; queued++) {
        for (uint32_t phase = 0; phase < 10; phase++) {
            reset_stick(queued, 1000 + phase);
            recalibrate_pointer();
            CHECK(recalibrated);
            CHECK(!recalibrated_while_streaming);
            CHECK(streaming);
            check_stream_aligned("after recalibrate");
        }
    }
}

// The stick ignores the first enable while it recalibrates: the stream still
// comes back.
static void test_enable_retried(void) {
    reset_stick(0, 2000);
    drop_enables = 1;
    recalibrate_pointer();
    CHECK(streaming);
    check_stream_aligned("after a missed enable");
    drop_enables = 0;
}

int main(void) {
    test_recalibrate_while_streaming();
    test_enable_retried();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
