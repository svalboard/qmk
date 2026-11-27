// Host test for keyboards/svalboard/power_policy.h: the idle scan period while a
// key is held (matrix.c), the PIO dividers across a deep-idle clock switch, and
// LED writes at a clock too low for the WS2812 timing (power.c). Built and run
// by util/updater_test/run.sh.
#include <stdio.h>
#include "power_policy.h"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

#define FULL 125000000u
#define DIV(x) ((uint32_t)((x) * 256.0 + 0.5) << 8) // a 16.8 divider in the clkdiv register

// 1 ms active, light idle after 500 ms (20 ms frames), deep after 5 s (1 s frames).
static uint32_t period(uint32_t quiet_ms, bool key_down, uint8_t *stage) {
    return sval_idle_period(1000, 20000, 500, 1000000, 5, quiet_ms, key_down, stage);
}

static void test_idle_stages(void) {
    uint8_t s;
    CHECK(period(100, false, &s) == 1000 && s == 0);
    CHECK(period(600, false, &s) == 20000 && s == 1);
    CHECK(period(6000, false, &s) == 1000000 && s == 2);
    // Unpaced, or stages off.
    CHECK(sval_idle_period(0, 20000, 500, 1000000, 5, 60000, false, &s) == 0 && s == 0);
    CHECK(sval_idle_period(1000, 20000, 0, 1000000, 0, 60000, false, &s) == 1000 && s == 0);
    // A stage no slower than active is no stage.
    CHECK(sval_idle_period(1000, 1000, 500, 1000, 5, 60000, false, &s) == 1000 && s == 0);
}

// F7: a key held past both timeouts (nothing changes while it is held) keeps
// the active period, so its release is seen within a frame.
static void test_key_held(void) {
    uint8_t s = 9;
    CHECK(period(600, true, &s) == 1000 && s == 0);
    CHECK(period(6000, true, &s) == 1000 && s == 0);
    CHECK(period(600000, true, &s) == 1000 && s == 0);
    CHECK(period(600000, true, NULL) == 1000);
}

static void test_div_scale(void) {
    bool c = false;
    // WS2812 at 125 MHz: 6.25 (20 MHz). 48 and 24 MHz keep it, 12 MHz cannot (0.6).
    CHECK(sval_pio_div_scale(DIV(6.25), FULL, 48000000u, &c) == DIV(2.4) && !c);
    CHECK(sval_pio_div_scale(DIV(6.25), FULL, 24000000u, &c) == DIV(1.2) && !c);
    CHECK(sval_pio_div_scale(DIV(6.25), FULL, 12000000u, &c) == DIV(1.0) && c);
    c = false;
    // Split serial (8 MHz) and PS/2 (200 kHz) fit at 12 MHz.
    CHECK(sval_pio_div_scale(DIV(15.625), FULL, 12000000u, &c) == DIV(1.5) && !c);
    CHECK(sval_pio_div_scale(DIV(625), FULL, 12000000u, &c) == DIV(60) && !c);
    // And back up from 12 MHz.
    CHECK(sval_pio_div_scale(DIV(60), 12000000u, FULL, &c) == DIV(625) && !c);
}

// Down and back: every state machine running at the switch gets its exact
// divider back.
static void test_round_trip(void) {
    sval_pio_reclock_t r               = {0};
    uint32_t           d[SVAL_PIO_SMS] = {DIV(15.625), DIV(6.25), 0, 0, DIV(625), 0, 0, 0};
    uint8_t            en              = 0x13;
    sval_pio_reclock_down(&r, d, en, FULL, 48000000u);
    CHECK(d[0] == DIV(6.0) && d[1] == DIV(2.4) && d[4] == DIV(240));
    CHECK(d[2] == 0); // not running: untouched
    CHECK(r.clamped_mask == 0);
    sval_pio_reclock_up(&r, d, en, 48000000u, FULL);
    CHECK(d[0] == DIV(15.625) && d[1] == DIV(6.25) && d[4] == DIV(625));
}

// F38: the pointer search starts the PS/2 state machine while the clock is low;
// its driver sets the divider for that clock (12 MHz / 200 kHz = 60). Back at
// full clock it must be 625, never a divider nothing saved (0 = divide by 65536).
static void test_started_while_low(void) {
    const uint32_t lows[] = {48000000u, 24000000u, 12000000u};
    for (unsigned k = 0; k < 3; k++) {
        uint32_t           low             = lows[k];
        sval_pio_reclock_t r               = {0};
        uint32_t           d[SVAL_PIO_SMS] = {DIV(15.625), DIV(6.25)};
        sval_pio_reclock_down(&r, d, 0x03, FULL, low);
        d[4] = DIV(low / 200000.0); // pio1 sm0 started at the low clock
        sval_pio_reclock_up(&r, d, 0x13, low, FULL);
        CHECK(d[4] == DIV(625));
        CHECK(d[0] == DIV(15.625) && d[1] == DIV(6.25));
        // The next trip down saves it like the others.
        sval_pio_reclock_down(&r, d, 0x13, FULL, low);
        sval_pio_reclock_up(&r, d, 0x13, low, FULL);
        CHECK(d[4] == DIV(625));
    }
}

// F46: at 12 MHz the WS2812 divider is clamped, so an LED write first needs the
// full clock, and the clock goes low again only once the frame is out.
static void test_led_write_at_low_clock(void) {
    sval_pio_reclock_t r               = {0};
    uint32_t           d[SVAL_PIO_SMS] = {DIV(15.625), DIV(6.25)};
    CHECK(!sval_pio_would_clamp(d, 0x03, FULL, 48000000u));
    CHECK(!sval_pio_would_clamp(d, 0x03, FULL, 24000000u));
    CHECK(sval_pio_would_clamp(d, 0x03, FULL, 12000000u));
    CHECK(!sval_pio_would_clamp(d, 0x01, FULL, 12000000u)); // no WS2812 running

    sval_pio_reclock_down(&r, d, 0x03, FULL, 12000000u);
    CHECK(r.clamped_mask == 0x02);
    CHECK(sval_led_write_needs_full(true, r.clamped_mask));
    CHECK(!sval_led_write_needs_full(false, r.clamped_mask));
    sval_pio_reclock_up(&r, d, 0x03, 12000000u, FULL);
    CHECK(r.clamped_mask == 0);

    // 48 and 24 MHz keep the LED timing: writes stay at the low clock, as before.
    sval_pio_reclock_down(&r, d, 0x03, FULL, 24000000u);
    CHECK(!sval_led_write_needs_full(true, r.clamped_mask));
    sval_pio_reclock_up(&r, d, 0x03, 24000000u, FULL);

    // Down to 12 MHz waits out a write in progress; other clocks never wait.
    CHECK(sval_clock_low_allowed(true, false, 0));
    CHECK(!sval_clock_low_allowed(true, true, 0));
    CHECK(!sval_clock_low_allowed(true, true, SVAL_LED_WRITE_SETTLE_MS));
    CHECK(sval_clock_low_allowed(true, true, SVAL_LED_WRITE_SETTLE_MS + 1));
    CHECK(sval_clock_low_allowed(false, true, 0));
}

int main(void) {
    test_idle_stages();
    test_key_held();
    test_div_scale();
    test_round_trip();
    test_started_while_low();
    test_led_write_at_low_clock();
    printf("power policy: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
