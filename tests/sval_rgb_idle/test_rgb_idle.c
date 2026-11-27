// Host test for keyboards/svalboard/rgb_idle.c: the idle dimming never reaches
// the saved lighting (a brightness key or a VIA set/save while dimmed works on
// the awake value), and RGBLIGHT_SLEEP turns the LEDs off while the host sleeps
// and back on when it wakes. rgblight, its EEPROM copy, VIA, the clock and USB
// are mocks (stub/quantum.h). Built and run by util/updater_test/run.sh.
#include <stdio.h>
#include "quantum.h"
#include "../../keyboards/svalboard/rgb_idle.c"

static int checks, failures;
#define CHECK(c)                                               \
    do {                                                       \
        checks++;                                              \
        if (!(c)) {                                            \
            failures++;                                        \
            printf("FAIL %s:%d %s\n", __func__, __LINE__, #c); \
        }                                                      \
    } while (0)

/* ---- mocks ---- */
struct saved_values global_saved_values;
usb_driver_t        USB_DRIVER;
static uint32_t     now, last_input;
static struct {
    bool    enable;
    uint8_t h, s, v;
} rgb, eeprom;
static bool suspended, pre_suspend_enabled;
static int  suspends, wakeups, kb_channel;

uint32_t timer_read32(void) {
    return now;
}
uint32_t timer_elapsed32(uint32_t t) {
    return now - t;
}
uint32_t last_input_activity_elapsed(void) {
    return now - last_input;
}
bool is_keyboard_master(void) {
    return true;
}
void sval_clock_full(void) {}

uint8_t rgblight_get_val(void) {
    return rgb.v;
}
uint8_t rgblight_get_hue(void) {
    return rgb.h;
}
uint8_t rgblight_get_sat(void) {
    return rgb.s;
}
bool rgblight_is_enabled(void) {
    return rgb.enable;
}
void rgblight_enable_noeeprom(void) {
    rgb.enable = true;
}
void rgblight_disable_noeeprom(void) {
    rgb.enable = false;
}
void rgblight_sethsv_noeeprom(uint8_t h, uint8_t s, uint8_t v) {
    rgb.h = h, rgb.s = s, rgb.v = v;
}
// As quantum/rgblight/rgblight.c: once per suspend, and the wake restores the
// enable state from before it.
void rgblight_suspend(void) {
    suspends++;
    if (!suspended) {
        suspended           = true;
        pre_suspend_enabled = rgb.enable;
        rgb.enable          = false;
    }
}
void rgblight_wakeup(void) {
    wakeups++;
    suspended = false;
    if (pre_suspend_enabled) rgb.enable = true;
}
// The brightness-up key (RM_VALU): a step from the current value, saved.
static void key_brightness_up(void) {
    rgb.v += 10;
    eeprom = rgb;
}
// quantum/via.c's lighting channel.
void via_qmk_rgblight_command(uint8_t *data, uint8_t length) {
    (void)length;
    if (data[0] == id_custom_set_value && data[2] == id_qmk_rgblight_brightness) rgb.v = data[3];
    if (data[0] == id_custom_set_value && data[2] == id_qmk_rgblight_color) rgb.h = data[3], rgb.s = data[4];
    if (data[0] == id_custom_get_value && data[2] == id_qmk_rgblight_brightness) data[3] = rgb.v;
    if (data[0] == id_custom_save) eeprom = rgb;
}
void via_custom_value_command_kb(uint8_t *data, uint8_t length) {
    (void)data, (void)length;
    kb_channel++;
}

/* ---- helpers ---- */
static void reset(void) {
    memset(&global_saved_values, 0, sizeof(global_saved_values));
    global_saved_values.idle_flags         = SVAL_IDLE_RGB_DIM;
    global_saved_values.scan_idle_after_ms = 1000;
    global_saved_values.scan_deep_after_s  = 60;
    rgb_idle_applied = 0, rgb_woken = false, rgb_host_asleep = false;
    now = 100000, last_input = now;
    rgb.enable = true, rgb.h = 85, rgb.s = 255, rgb.v = 128;
    eeprom    = rgb;
    suspended = false, suspends = wakeups = kb_channel = 0;
    USB_DRIVER.state = USB_ACTIVE;
}
// One main-loop pass with no input, after ms of quiet.
static void idle_for(uint32_t ms) {
    now += ms;
    sval_rgb_idle_task();
}
// A key press: pre_process_record_kb (typing_pause.c / side.c), the keycode,
// then QMK marks the input and housekeeping runs.
static void press(void (*keycode)(void)) {
    sval_rgb_idle_input();
    if (keycode) keycode();
    last_input = now;
    sval_rgb_idle_task();
}
static void via(uint8_t cmd, uint8_t value_id, uint8_t a, uint8_t b) {
    uint8_t data[32] = {cmd, id_qmk_rgblight_channel, value_id, a, b};
    via_custom_value_command(data, sizeof(data));
    if (cmd == id_custom_get_value) kb_channel = data[3]; // hand the read back
}

/* ---- tests ---- */
static void test_dims(void) {
    reset();
    idle_for(2000);
    CHECK(rgb_idle_applied == 1);
    CHECK(rgb.v == 32);
    CHECK(eeprom.v == 128);
    press(NULL);
    CHECK(rgb_idle_applied == 0);
    CHECK(rgb.v == 128);
}

// F45: the first input while dimmed is a brightness key. It steps from the
// awake value (128 -> 138) and saves that, not 32 -> 42.
static void test_brightness_key_while_dimmed(void) {
    reset();
    idle_for(2000);
    CHECK(rgb.v == 32);
    press(key_brightness_up);
    CHECK(eeprom.v == 138);
    CHECK(rgb.v == 138);
    // Dims again from the new value.
    idle_for(2000);
    CHECK(rgb.v == 34);
    CHECK(eeprom.v == 138);
}

// F45: a host app sets the color and saves while the lighting is dimmed. The
// saved brightness is the awake one; the lighting dims again afterwards.
static void test_via_set_and_save_while_dimmed(void) {
    reset();
    idle_for(2000);
    via(id_custom_set_value, id_qmk_rgblight_color, 10, 200);
    CHECK(rgb.h == 10 && rgb.v == 128);
    idle_for(10); // the next pass: still idle, dims again
    CHECK(rgb.v == 32);
    via(id_custom_save, 0, 0, 0);
    CHECK(eeprom.h == 10 && eeprom.s == 200);
    CHECK(eeprom.v == 128);
    // A brightness set while dimmed is kept as the awake brightness.
    idle_for(10);
    via(id_custom_set_value, id_qmk_rgblight_brightness, 200, 0);
    idle_for(10);
    CHECK(rgb.v == 50);
    via(id_custom_save, 0, 0, 0);
    CHECK(eeprom.v == 200);
}

// F45: reading the brightness while dimmed reports the awake one, so a host
// that writes back what it read stores the awake value.
static void test_via_read_while_dimmed(void) {
    reset();
    idle_for(2000);
    via(id_custom_get_value, id_qmk_rgblight_brightness, 0, 0);
    CHECK(kb_channel == 128);
    reset();
    via(id_custom_get_value, id_qmk_rgblight_brightness, 0, 0);
    CHECK(kb_channel == 128);
}

// Other channels still reach the keyboard's handler.
static void test_via_other_channel(void) {
    reset();
    uint8_t data[32] = {id_custom_set_value, id_custom_channel, 1, 2};
    via_custom_value_command(data, sizeof(data));
    CHECK(kb_channel == 1);
}

// F48: the host sleeps: the LEDs go off once, and come back when it wakes.
static void test_host_sleep(void) {
    reset();
    sval_rgb_host_sleep_task();
    CHECK(suspends == 0 && rgb.enable);
    USB_DRIVER.state = USB_SUSPENDED;
    sval_rgb_host_sleep_task();
    CHECK(suspends == 1);
    CHECK(!rgb.enable);
    sval_rgb_host_sleep_task();
    CHECK(suspends == 1);
    // The USB wake event (usb_main.c) wakes rgblight first; the task's own call changes nothing.
    rgblight_wakeup();
    USB_DRIVER.state = USB_ACTIVE;
    sval_rgb_host_sleep_task();
    CHECK(rgb.enable);
    CHECK(!suspended);
    // A suspend left without a wake event (a bus reset) still turns them back on.
    USB_DRIVER.state = USB_SUSPENDED;
    sval_rgb_host_sleep_task();
    CHECK(!rgb.enable);
    USB_DRIVER.state = USB_READY;
    sval_rgb_host_sleep_task();
    CHECK(rgb.enable);
    // Off before the host slept: still off after.
    reset();
    rgb.enable       = false;
    USB_DRIVER.state = USB_SUSPENDED;
    sval_rgb_host_sleep_task();
    USB_DRIVER.state = USB_ACTIVE;
    sval_rgb_host_sleep_task();
    CHECK(!rgb.enable);
}

int main(void) {
    test_dims();
    test_brightness_key_while_dimmed();
    test_via_set_and_save_while_dimmed();
    test_via_read_while_dimmed();
    test_via_other_channel();
    test_host_sleep();
    printf("rgb idle: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
