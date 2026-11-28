// Host test for the RP2040 PS/2 receiver (platforms/chibios/drivers/vendor/RP/
// RP2040/ps2_vendor.c), built against stand-ins for the PIO, the ChibiOS queue
// and the clock line (vendor_stubs/):
// - a full receive queue must not leave the frame in the PIO FIFO, or the
//   level-triggered interrupt re-enters forever and the half hangs (F1);
// - a dropped frame, an RX FIFO stall (interrupts off during a flash erase while
//   the device clocks) or a malformed frame restarts the state machine at the
//   next idle gap and drops the queued bytes, so the bit framing and the packet
//   grouping recover without a reboot (F11).
#include <stdio.h>
#include <string.h>
#include "hardware/structs/timer.h"
#include "ps2_vendor.c"

// The driver's defaults, so this test also builds against a driver without them.
#ifndef PS2_RESYNC_IDLE_US
#    define PS2_RESYNC_IDLE_US 2000
#endif
#ifndef PS2_RESYNC_TIMEOUT_US
#    define PS2_RESYNC_TIMEOUT_US 30000
#endif

pio_hw_t   sim_pio;
timer_hw_t sim_timer;

static int failures = 0;
#define CHECK(cond)                                               \
    do {                                                          \
        if (!(cond)) {                                            \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); \
            failures++;                                           \
        }                                                         \
    } while (0)

/* ---- the clock line ---- */
// Until busy_until_us the device is sending (the clock toggles every 40 us);
// after that the line is idle (high). Each read of the line takes 10 us.
static uint32_t busy_until_us;
bool            sim_clock_high(void) {
    sim_timer.timerawl += 10;
    uint32_t t = sim_timer.timerawl;
    return t >= busy_until_us || (t / 40) % 2;
}

/* ---- frames, as the state machine pushes them (11 bits, shifted right) ---- */
static uint32_t frame_of(uint8_t data) {
    uint32_t parity = __builtin_parity(data) ? 0 : 1; // odd parity
    return (1u << 31) | (parity << 30) | ((uint32_t)data << 22);
}

static void fifo_push(uint32_t frame) {
    if (sim_pio.rx_count < 4) sim_pio.rx[sim_pio.rx_count++] = frame;
}

// FDEBUG is write-1-to-clear. The driver sees fdebug_hw with a marker bit the
// driver never tests; a write replaces the marker, and its 1 bits clear.
static uint32_t fdebug_hw;
#define FDEBUG_MARK 0x80000000u
static void fdebug_before(void) {
    sim_pio.fdebug = fdebug_hw | FDEBUG_MARK;
}
static void fdebug_after(void) {
    if (!(sim_pio.fdebug & FDEBUG_MARK)) fdebug_hw &= ~sim_pio.fdebug;
}

// The driver's poll, with FDEBUG as the hardware has it.
static bool poll(void) {
    fdebug_before();
    bool r = pbuf_has_data();
    fdebug_after();
    return r;
}

// The NVIC: calls the handler while its source is pending, as the level-
// triggered RXNEMPTY interrupt does. Returns how many times it ran.
static int run_irq(int limit) {
    int n = 0;
    while (sim_pio.rx_irq_enabled && sim_pio.rx_count > 0 && n < limit) {
        sim_pio.ints0 = PIO_IRQ0_INTF_SM0_RXNEMPTY_BITS;
        fdebug_before();
        RP_PIO1_IRQ_0_HANDLER();
        fdebug_after();
        n++;
    }
    sim_pio.ints0 = 0;
    return n;
}

static void deliver(uint8_t data) {
    fifo_push(frame_of(data));
    run_irq(100);
}

static void setup(void) {
    memset(&sim_pio, 0, sizeof sim_pio);
    fdebug_hw          = 0;
    sim_timer.timerawl = 1000000;
    busy_until_us      = 0;
    ps2_host_init();
    // Settle any state left by an earlier test.
    while (poll())
        ps2_host_recv();
    sim_pio.restarts = 0;
}

static void test_good_frames_pass_through(void) {
    setup();
    deliver(0x08);
    deliver(0x05);
    deliver(0xFB);
    CHECK(poll());
    CHECK(ps2_host_recv() == 0x08);
    CHECK(ps2_host_recv_response() == 0x05);
    CHECK(ps2_host_recv_response() == 0xFB);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 0);
}

// F1: the queue is full and the main loop isn't reading it (for example during
// a wear-leveling erase). The next frame must still leave the FIFO.
static void test_full_queue_does_not_storm(void) {
    setup();
    for (int i = 0; i < BUFFER_SIZE; i++)
        deliver(0x08);
    fifo_push(frame_of(0x08));
    int calls = run_irq(1000);
    CHECK(calls == 1);
    CHECK(sim_pio.rx_count == 0);
}

// F1: after a dropped frame the packet grouping is lost, so the queued bytes go
// and the next byte read is the first one sent after the resync.
static void test_dropped_frame_resyncs(void) {
    setup();
    for (int i = 0; i < BUFFER_SIZE; i++)
        deliver(0x08);
    deliver(0x01); // dropped
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);
    deliver(0x09);
    deliver(0x02);
    deliver(0x03);
    CHECK(poll());
    CHECK(ps2_host_recv() == 0x09);
    CHECK(ps2_host_recv_response() == 0x02);
    CHECK(ps2_host_recv_response() == 0x03);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);
}

// F11: interrupts were off long enough for the 4-frame FIFO to fill, and the
// state machine stalled while the device kept clocking. It restarts once the
// line has been idle, not while a byte is on the wire.
static void test_rx_stall_resyncs_at_idle(void) {
    setup();
    for (int i = 0; i < 4; i++)
        fifo_push(frame_of(0x08));
    fdebug_hw     = 1u << PIO_FDEBUG_RXSTALL_LSB;
    busy_until_us = sim_timer.timerawl + 5000;
    run_irq(100);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);
    // Idle counts from the last low clock seen (reads are 10 us apart).
    CHECK(sim_timer.timerawl + 20 >= busy_until_us + PS2_RESYNC_IDLE_US);
    CHECK((fdebug_hw & 1u) == 0);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);
}

// F11: a frame with a bad start or stop bit means the receiver counts edges
// from the middle of a byte; it never corrects itself without a restart.
static void test_malformed_frame_resyncs(void) {
    setup();
    fifo_push(frame_of(0x08) | (1u << 21)); // start bit 1
    run_irq(100);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);

    fifo_push(frame_of(0x08) & ~(1u << 31)); // stop bit 0
    run_irq(100);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 2);

    fifo_push(frame_of(0x08) ^ (1u << 30)); // parity
    run_irq(100);
    CHECK(!poll());
    CHECK(sim_pio.restarts == 3);
}

// A line that never goes idle: give up after the timeout, keep the request,
// and resync on a later poll once it does.
static void test_busy_line_retries_later(void) {
    setup();
    fifo_push(frame_of(0x08) | (1u << 21));
    run_irq(100);
    busy_until_us  = 0xFFFFFFFFu;
    uint32_t start = sim_timer.timerawl;
    CHECK(!poll());
    CHECK(sim_pio.restarts == 0);
    CHECK(sim_timer.timerawl - start <= PS2_RESYNC_TIMEOUT_US + 100);
    busy_until_us = 0;
    CHECK(!poll());
    CHECK(sim_pio.restarts == 1);
}

int main(void) {
    test_good_frames_pass_through();
    test_full_queue_does_not_storm();
    test_dropped_frame_resyncs();
    test_rx_stall_resyncs_at_idle();
    test_malformed_frame_resyncs();
    test_busy_line_retries_later();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
