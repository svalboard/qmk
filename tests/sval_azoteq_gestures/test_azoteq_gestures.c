// Host tests for the touchpad tap setting write in
// keyboards/svalboard/azoteq/azoteq.c (the mouse path, no native touchpad).
// Run by util/updater_test/run.sh.
//
// A changed tap setting, or a DPI change that rescales the dead zone and
// scroll distance, is written to the IQS5xx in a window of its own. When
// that write fails twice, it stays pending and is tried again after a
// growing wait, until it is written or a bounded number of tries is spent.
#define SVAL_POINTING_AUTO // the search's second look is not under test
#include "../../keyboards/svalboard/azoteq/azoteq.c"

#include <assert.h>

// ------------------------------------------------------------ host stand-ins

static uint32_t now_ms;
static bool     fail_gestures;   // writes of the gesture registers fail
static unsigned gestures_writes; // successful writes of the gesture registers
static unsigned gestures_tries;  // every write of the gesture registers
static uint8_t  setting;

void         i2c_init(void) {}
i2c_status_t i2c_ping_address(uint8_t address, uint16_t timeout) {
    (void)address;
    (void)timeout;
    return I2C_STATUS_SUCCESS;
}
i2c_status_t i2c_read_register16(uint8_t devaddr, uint16_t regaddr, uint8_t *data, uint16_t length, uint16_t timeout) {
    (void)devaddr;
    (void)regaddr;
    (void)timeout;
    memset(data, 0, length);
    return I2C_STATUS_SUCCESS;
}
i2c_status_t i2c_write_register16(uint8_t devaddr, uint16_t regaddr, const uint8_t *data, uint16_t length, uint16_t timeout) {
    (void)devaddr;
    (void)data;
    (void)length;
    (void)timeout;
    if (regaddr != SVAL_PAD_REG_GESTURES) return I2C_STATUS_SUCCESS;
    gestures_tries++;
    if (fail_gestures) return I2C_STATUS_TIMEOUT;
    gestures_writes++;
    return I2C_STATUS_SUCCESS;
}
void wait_us(uint32_t us) {
    (void)us;
}
uint16_t timer_read(void) {
    return (uint16_t)now_ms;
}
uint32_t timer_read32(void) {
    return now_ms;
}
uint16_t timer_elapsed(uint16_t last) {
    return (uint16_t)(now_ms - last);
}
bool gpio_read_pin(int pin) {
    (void)pin;
    return true;
}
bool is_keyboard_left(void) {
    return true;
}
void pointing_device_set_status(pointing_device_status_t status) {
    (void)status;
}
bool azoteq_iqs5xx_init(void) {
    return true;
}
uint16_t azoteq_iqs5xx_get_product(void) {
    return AZOTEQ_IQS5XX_IQS550;
}
void azoteq_iqs5xx_set_cpi(uint16_t cpi) {
    (void)cpi;
}
i2c_status_t azoteq_iqs5xx_end_session(void) {
    return I2C_STATUS_SUCCESS;
}
report_mouse_t azoteq_iqs5xx_get_report(report_mouse_t mouse_report) {
    return mouse_report;
}
uint8_t sval_pad_gestures(void) {
    return setting;
}
uint16_t sval_pad_tuning(void) {
    return 0;
}

// One pass of the pointing task, ms later.
static void poll(uint32_t ms) {
    now_ms += ms;
    report_mouse_t r = {0};
    (void)pointing_device_driver_get_report(r);
}

static void start(void) {
    fail_gestures = false;
    now_ms        = 1000;
    assert(pointing_device_driver_init());
    poll(1);
    assert(!azo_gestures_pending);
    gestures_writes = gestures_tries = 0;
}

// Two failures in a row leave the setting pending; it goes out on a later
// pass once the pad answers again.
static void test_retried(void) {
    start();
    setting = 1;
    sval_pad_gestures_changed();
    fail_gestures = true;
    poll(1);
    assert(gestures_tries == 2 && gestures_writes == 0);
    fail_gestures = false;
    poll(1); // inside the wait: not tried yet
    assert(gestures_tries == 2);
    for (int i = 0; i < 20 && !gestures_writes; i++)
        poll(10);
    assert(gestures_writes == 1);
    assert(!azo_gestures_pending);
    poll(1000);
    assert(gestures_writes == 1); // written once, not again
    puts("  a failed tap setting write is tried again and written");
}

// A DPI change rescales the dead zone and scroll distance: same path.
static void test_dpi_retried(void) {
    start();
    fail_gestures = true;
    pointing_device_driver_set_cpi(800);
    poll(1);
    assert(gestures_tries == 2 && gestures_writes == 0);
    fail_gestures = false;
    for (int i = 0; i < 20 && !gestures_writes; i++)
        poll(10);
    assert(gestures_writes == 1);
    puts("  a failed write after a DPI change is tried again and written");
}

// The waits double, and a pad that never answers is left alone after the
// longest wait, so it costs the loop a bounded number of tries.
static void test_bounded(void) {
    start();
    setting = 2;
    sval_pad_gestures_changed();
    fail_gestures   = true;
    unsigned passes = 0;
    while (azo_gestures_pending && passes < 100000) {
        poll(1);
        passes++;
    }
    assert(!azo_gestures_pending);
    assert(gestures_tries == 2 * 9); // the first try and eight retries, two writes each
    assert(passes >= 25500 && passes < 26000);
    unsigned tries = gestures_tries;
    poll(60000);
    assert(gestures_tries == tries);
    // A new change is written at once, whatever happened before.
    fail_gestures = false;
    setting       = 3;
    sval_pad_gestures_changed();
    poll(1);
    assert(gestures_writes == 1);
    puts("  retries back off and stop after about 25 s; a new change goes out at once");
}

// A new change while a retry waits goes out on the next pass.
static void test_change_during_wait(void) {
    start();
    sval_pad_gestures_changed();
    fail_gestures = true;
    poll(1);
    poll(30);
    poll(1); // the 100 ms wait is not over
    assert(gestures_tries == 2);
    fail_gestures = false;
    sval_pad_gestures_changed();
    poll(1);
    assert(gestures_writes == 1);
    puts("  a new change during the wait goes out at once");
}

int main(void) {
    puts("IQS5xx tap setting write:");
    test_retried();
    test_dpi_retried();
    test_bounded();
    test_change_during_wait();
    puts("IQS5xx tap setting write: all passed");
    return 0;
}
