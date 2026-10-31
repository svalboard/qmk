// Copyright 2026 QMK
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gtest/gtest.h"

extern "C" {
#include "azoteq_iqs5xx.h"

void azoteq_iqs5xx_mock_reset(void);
void azoteq_iqs5xx_mock_set_base_data(azoteq_iqs5xx_base_data_t base_data);
}

class AzoteqIqs5xx : public testing::Test {
   protected:
    void SetUp() override {
        azoteq_iqs5xx_mock_reset();
    }
};

TEST_F(AzoteqIqs5xx, PreservesIncomingButtonsWhenReportingMovement) {
    azoteq_iqs5xx_base_data_t base_data = {0};
    base_data.number_of_fingers         = 1;
    base_data.x.l                       = 12;
    base_data.y.l                       = 34;
    azoteq_iqs5xx_mock_set_base_data(base_data);

    report_mouse_t input_report = {0};
    input_report.buttons        = (1 << POINTING_DEVICE_BUTTON1) | (1 << POINTING_DEVICE_BUTTON2);

    report_mouse_t report = azoteq_iqs5xx_get_report(input_report);

    EXPECT_EQ(report.x, 12);
    EXPECT_EQ(report.y, 34);
    EXPECT_EQ(report.h, 0);
    EXPECT_EQ(report.v, 0);
    EXPECT_EQ(report.buttons, input_report.buttons);
}

TEST_F(AzoteqIqs5xx, CombinesGestureButtonWithIncomingButtons) {
    azoteq_iqs5xx_base_data_t base_data       = {0};
    base_data.gesture_events_1.two_finger_tap = true;
    azoteq_iqs5xx_mock_set_base_data(base_data);

    report_mouse_t input_report = {0};
    input_report.buttons        = 1 << POINTING_DEVICE_BUTTON1;

    report_mouse_t report = azoteq_iqs5xx_get_report(input_report);

    EXPECT_EQ(report.x, 0);
    EXPECT_EQ(report.y, 0);
    EXPECT_EQ(report.h, 0);
    EXPECT_EQ(report.v, 0);
    EXPECT_EQ(report.buttons, input_report.buttons | (1 << POINTING_DEVICE_BUTTON2));
}

// Buttons from the chip's gestures, polled the way the Svalboard runs the
// driver. On the USB half (SPLIT_POINTING_ENABLE + POINTING_DEVICE_COMBINED),
// pointing_device_task passes the driver's previous buttons back in, and the
// Svalboard wrapper (keyboards/svalboard/azoteq/azoteq.c) keeps them between
// reads. On the other half, the split code passes an empty report every time.
// A press the chip reports must be released once the chip stops reporting it.
extern "C" void azoteq_iqs5xx_mock_set_read_status(i2c_status_t status);

namespace {
const uint8_t B1 = 1 << POINTING_DEVICE_BUTTON1;
const uint8_t B2 = 1 << POINTING_DEVICE_BUTTON2;

struct Frame {
    bool    tap, hold, two_finger_tap, swipe_x_neg, scroll;
    uint8_t fingers;
};

azoteq_iqs5xx_base_data_t base_of(Frame f) {
    azoteq_iqs5xx_base_data_t b       = {0};
    b.gesture_events_0.single_tap     = f.tap;
    b.gesture_events_0.press_and_hold = f.hold;
    b.gesture_events_0.swipe_x_neg    = f.swipe_x_neg;
    b.gesture_events_1.two_finger_tap = f.two_finger_tap;
    b.gesture_events_1.scroll         = f.scroll;
    b.number_of_fingers               = f.fingers;
    return b;
}

// The USB half: what went out last time comes back in, plus `outside`
// (buttons another source put in the report).
class UsbHalf {
   public:
    uint8_t held = 0;
    uint8_t poll(Frame f, uint8_t outside = 0) {
        azoteq_iqs5xx_mock_set_base_data(base_of(f));
        report_mouse_t in = {0};
        in.buttons        = held | outside;
        held              = azoteq_iqs5xx_get_report(in).buttons;
        return held;
    }
};

// The half behind the split cable: an empty report in every time.
uint8_t poll_other_half(Frame f) {
    azoteq_iqs5xx_mock_set_base_data(base_of(f));
    return azoteq_iqs5xx_get_report(report_mouse_t{}).buttons;
}

const Frame IDLE       = {};
const Frame TOUCH      = {.fingers = 1};
const Frame TAP        = {.tap = true};
const Frame HOLD       = {.hold = true, .fingers = 1};
const Frame TWO_TAP    = {.two_finger_tap = true};
const Frame SWIPE_LEFT = {.swipe_x_neg = true, .fingers = 1};
} // namespace

class AzoteqGestureButtons : public testing::Test {
   protected:
    UsbHalf usb;
    void    SetUp() override {
        azoteq_iqs5xx_mock_reset();
        // Whatever an earlier test left the driver holding, an idle read with
        // nothing coming in leaves nothing held.
        azoteq_iqs5xx_get_report(report_mouse_t{});
    }
};

TEST_F(AzoteqGestureButtons, SingleTapClicksOnceAndReleases) {
    EXPECT_EQ(usb.poll(TOUCH), 0);
    EXPECT_EQ(usb.poll(TAP), B1);
    EXPECT_EQ(usb.poll(IDLE), 0) << "the tap's left button stays down";
    EXPECT_EQ(usb.poll(IDLE), 0);
}

TEST_F(AzoteqGestureButtons, PressAndHoldDragsUntilTheFingerLifts) {
    EXPECT_EQ(usb.poll(TOUCH), 0);
    for (int i = 0; i < 20; i++)
        EXPECT_EQ(usb.poll(HOLD), B1);
    EXPECT_EQ(usb.poll(TOUCH), 0) << "the hold is over but the left button stays down";
    EXPECT_EQ(usb.poll(IDLE), 0);
}

TEST_F(AzoteqGestureButtons, TwoFingerTapRightClicksOnceAndReleases) {
    EXPECT_EQ(usb.poll(TWO_TAP), B2);
    EXPECT_EQ(usb.poll(IDLE), 0) << "the two-finger tap's right button stays down";
}

TEST_F(AzoteqGestureButtons, SwipeButtonReleases) {
    EXPECT_EQ(usb.poll(SWIPE_LEFT), 1 << POINTING_DEVICE_BUTTON4);
    EXPECT_EQ(usb.poll(IDLE), 0);
}

TEST_F(AzoteqGestureButtons, RepeatedFirmPressesEachReleaseThenATapStillClicks) {
    for (int press = 0; press < 5; press++) {
        usb.poll(TOUCH);
        for (int i = 0; i < 5; i++)
            EXPECT_EQ(usb.poll(HOLD), B1);
        EXPECT_EQ(usb.poll(IDLE), 0) << "press " << press;
    }
    EXPECT_EQ(usb.poll(TWO_TAP), B2);
    EXPECT_EQ(usb.poll(IDLE), 0);
    EXPECT_EQ(usb.poll(TAP), B1);
    EXPECT_EQ(usb.poll(IDLE), 0);
}

TEST_F(AzoteqGestureButtons, OtherHalfReleasesToo) {
    EXPECT_EQ(poll_other_half(HOLD), B1);
    EXPECT_EQ(poll_other_half(IDLE), 0);
    EXPECT_EQ(poll_other_half(TWO_TAP), B2);
    EXPECT_EQ(poll_other_half(IDLE), 0);
}

TEST_F(AzoteqGestureButtons, ButtonsFromElsewhereStayThroughAGesture) {
    // Upstream #26168: a button held by a key (or another device) must not be
    // dropped by the pad's report, and the pad must release only its own.
    EXPECT_EQ(usb.poll(TOUCH, B2), B2);
    EXPECT_EQ(usb.poll(TAP, B2), B1 | B2);
    EXPECT_EQ(usb.poll(IDLE, B2), B2);
    usb.held = 0;
    EXPECT_EQ(usb.poll(IDLE), 0) << "the key's button let go, nothing held";
}

TEST_F(AzoteqGestureButtons, ScrollAndMovementCarryNoButton) {
    EXPECT_EQ(usb.poll(Frame{.scroll = true}), 0);
    EXPECT_EQ(usb.poll(TOUCH), 0);
}

TEST_F(AzoteqGestureButtons, AFailedReadReleasesThePadsButtons) {
    // A pad that stops answering mid-drag must not leave the button down.
    EXPECT_EQ(usb.poll(HOLD), B1);
    azoteq_iqs5xx_mock_set_read_status(I2C_STATUS_TIMEOUT);
    report_mouse_t in = {0};
    in.buttons        = usb.held;
    usb.held          = azoteq_iqs5xx_get_report(in).buttons;
    EXPECT_EQ(usb.held, 0);
}
