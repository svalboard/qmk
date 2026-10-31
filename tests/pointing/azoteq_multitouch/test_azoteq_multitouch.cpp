// SPDX-License-Identifier: GPL-2.0-or-later
// The Azoteq driver's contact read for the Svalboard native touchpad
// (SVAL_AZOTEQ_MULTITOUCH): one read of the base bytes and the contact slots
// the last read needed, a second only when the count says more are down, one
// end of session, and one retry only for a NACK.
#include "gtest/gtest.h"
#include <cstring>
extern "C" {
#include "azoteq_iqs5xx.h"
}

static uint8_t      source[45], captured[45];
static uint16_t     addresses[4], lengths[4], end_address, read_timeout;
static uint8_t      sched_slots;
static int          reads, ends;
static bool         valid;
static i2c_status_t read_status, end_status;
static i2c_status_t observed_read, observed_end;
static bool         recover_second, retried, recovered;
static uint32_t     error_flags, now_us;
static int          fault_calls;

extern "C" {
void     i2c_init(void) {}
uint32_t sval_mt_i2c_error_flags(void) {
    return error_flags;
}
uint32_t sval_mt_now_us(void) {
    return now_us += 100;
}
void sval_mt_acquisition_fault(uint32_t, bool retry, bool ok) {
    fault_calls++;
    retried   = retry;
    recovered = ok;
}
i2c_status_t i2c_ping_address(uint8_t, uint16_t) {
    return I2C_STATUS_SUCCESS;
}
i2c_status_t i2c_read_register16(uint8_t, uint16_t reg, uint8_t *data, uint16_t len, uint16_t timeout) {
    if (reads < 4) {
        addresses[reads] = reg;
        lengths[reads]   = len;
    }
    reads++;
    if (recover_second && reads == 2) read_status = I2C_STATUS_SUCCESS;
    read_timeout = timeout;
    if (read_status == I2C_STATUS_SUCCESS) memcpy(data, source + (reg - 0x000C), len);
    return read_status;
}
i2c_status_t i2c_write_register16(uint8_t, uint16_t reg, const uint8_t *, uint16_t, uint16_t) {
    ends++;
    end_address = reg;
    return end_status;
}
void sval_mt_sample(const uint8_t *raw, bool ok) {
    memcpy(captured, raw, 45);
    valid = ok;
}
void sval_mt_read_status(i2c_status_t read, i2c_status_t end, uint32_t) {
    observed_read = read;
    observed_end  = end;
}
uint8_t pointing_device_handle_buttons(uint8_t buttons, bool pressed, pointing_device_buttons_t button) {
    return pressed ? buttons | (1 << button) : buttons & ~(1 << button);
}
}

extern "C" i2c_status_t azoteq_iqs5xx_last_read(uint32_t *start_us, uint32_t *end_us, bool *retried, uint8_t *slots);

// Contact n (0-4) down in slot `slot` of the chip's registers.
static void put_contact(uint8_t slot, uint8_t n) {
    uint8_t *p = source + 10 + 7 * slot;
    p[0]       = 0x01;
    p[1]       = 0x10 + n; // X
    p[2]       = 0x02;
    p[3]       = 0x20 + n; // Y
    p[4]       = 0x00;
    p[5]       = 0x40 + n; // touch strength
    p[6]       = 3 + n;    // area
}
static void lift(uint8_t slot) {
    memset(source + 10 + 7 * slot, 0, 7);
}
static void set_count(uint8_t n) {
    source[5] = n;
}
// One read, with the counters cleared first.
static void read_once() {
    reads = ends = 0;
    memset(captured, 0xEE, sizeof(captured));
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    uint32_t start, end;
    bool     retried;
    azoteq_iqs5xx_last_read(&start, &end, &retried, &sched_slots);
    EXPECT_EQ(ends, 1);
    EXPECT_TRUE(valid);
    EXPECT_EQ(memcmp(captured, source, 10), 0);
}
static bool slot_is(uint8_t slot, bool down) {
    const uint8_t       *c = captured + 10 + 7 * slot, *src = source + 10 + 7 * slot;
    static const uint8_t zero[7] = {0};
    return down ? memcmp(c, src, 7) == 0 : memcmp(c, zero, 7) == 0;
}

class AzoteqMultitouch : public testing::Test {
    void SetUp() override {
        memset(source, 0, sizeof(source));
        memset(captured, 0, sizeof(captured));
        read_status = end_status = I2C_STATUS_SUCCESS;
        // An all-up read: the next read is the base bytes alone.
        azoteq_iqs5xx_base_data_t idle = {};
        azoteq_iqs5xx_get_base_data(&idle);
        recover_second = retried = recovered = false;
        error_flags                          = 0; // a NACK: the RP2040 driver sets no error bits
        fault_calls                          = 0;
        reads = ends = 0;
        read_status = end_status = I2C_STATUS_SUCCESS;
        valid                    = false;
    }
};

TEST_F(AzoteqMultitouch, FingerDownReadsTheSlotsInTheSameWindowAndClosesOnce) {
    source[5]             = 1;
    source[7]             = 12;
    source[9]             = 34;
    source[38]            = 0x06;
    source[39]            = 0x23;
    source[42]            = 0x01;
    source[43]            = 0x80;
    source[44]            = 7;
    report_mouse_t report = azoteq_iqs5xx_get_report((report_mouse_t){0});
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(addresses[0], 0x000C);
    EXPECT_EQ(lengths[0], 10);
    EXPECT_EQ(addresses[1], 0x0016);
    EXPECT_EQ(lengths[1], 35);
    EXPECT_EQ(read_timeout, 30);
    EXPECT_EQ(ends, 1);
    EXPECT_EQ(end_address, 0xEEEE);
    EXPECT_TRUE(valid);
    EXPECT_EQ(memcmp(captured, source, 45), 0); // the fifth slot included
    EXPECT_EQ(report.x, 12);
    EXPECT_EQ(report.y, 34);
}

// From all up, 0 to 5 contacts land: the first read finds the count and a
// second fetches the slots; from then on one read carries just their slots.
TEST_F(AzoteqMultitouch, ZeroToFiveContactsSettleToOneReadSizedToThem) {
    for (uint8_t n = 0; n <= 5; n++) {
        memset(source, 0, sizeof(source)); // all up first
        read_once();
        for (uint8_t i = 0; i < n; i++)
            put_contact(i, i);
        set_count(n);
        read_once();
        EXPECT_EQ(reads, n ? 2 : 1) << int(n);
        EXPECT_EQ(lengths[0], 10);
        if (n) {
            EXPECT_EQ(addresses[1], 0x0016);
            EXPECT_EQ(lengths[1], 35);
        }
        EXPECT_EQ(memcmp(captured, source, 45), 0) << int(n);
        for (int frame = 0; frame < 3; frame++) {
            read_once();
            EXPECT_EQ(reads, 1) << int(n);
            EXPECT_EQ(addresses[0], 0x000C);
            EXPECT_EQ(lengths[0], 10 + 7 * n) << int(n);
            EXPECT_EQ(sched_slots, n);
            EXPECT_EQ(memcmp(captured, source, 45), 0) << int(n);
        }
    }
}

TEST_F(AzoteqMultitouch, AGrowingCountFetchesOnlyTheSlotsNotYetRead) {
    put_contact(0, 0);
    set_count(1);
    read_once();
    read_once();
    EXPECT_EQ(lengths[0], 17);
    put_contact(1, 1); // a second finger lands
    set_count(2);
    read_once();
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(lengths[0], 17);
    EXPECT_EQ(addresses[1], 0x000C + 17);
    EXPECT_EQ(lengths[1], 28);
    EXPECT_TRUE(slot_is(0, true) && slot_is(1, true) && slot_is(2, false));
    read_once();
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(lengths[0], 24);
    put_contact(2, 2); // and a third, with the count jumping straight to 4
    put_contact(3, 3);
    set_count(4);
    read_once();
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(lengths[1], 21);
    EXPECT_EQ(memcmp(captured, source, 45), 0);
    read_once();
    EXPECT_EQ(lengths[0], 38);
}

// A contact keeps its slot until it lifts, so lifts can leave the count below
// the highest slot in use: every contact still down is read, and IDs (slots)
// do not move.
TEST_F(AzoteqMultitouch, LiftsKeepTheRemainingContactsInTheirSlots) {
    for (uint8_t i = 0; i < 3; i++)
        put_contact(i, i);
    set_count(3);
    read_once();
    read_once();
    EXPECT_EQ(lengths[0], 31);
    lift(0); // the first finger lifts: slots 1 and 2 stay
    set_count(2);
    read_once();
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(slot_is(0, false) && slot_is(1, true) && slot_is(2, true));
    read_once();
    EXPECT_EQ(lengths[0], 31); // slot 2 is still the highest
    lift(2);
    set_count(1);
    read_once();
    EXPECT_TRUE(slot_is(1, true) && slot_is(2, false));
    read_once();
    EXPECT_EQ(lengths[0], 24);
    lift(1); // all up
    set_count(0);
    read_once();
    EXPECT_EQ(reads, 1);
    for (int i = 10; i < 45; i++)
        EXPECT_EQ(captured[i], 0);
    read_once();
    EXPECT_EQ(lengths[0], 10);
}

TEST_F(AzoteqMultitouch, AContactInAHighSlotIsFoundAndKeptInTheRead) {
    put_contact(3, 0);
    set_count(1);
    read_once();
    EXPECT_EQ(reads, 2);
    EXPECT_TRUE(slot_is(3, true) && slot_is(0, false));
    read_once();
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(lengths[0], 38);
    EXPECT_TRUE(slot_is(3, true));
    // It lifts as a new finger lands in slot 0: still one read.
    lift(3);
    put_contact(0, 1);
    read_once();
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(slot_is(0, true) && slot_is(3, false));
    read_once();
    EXPECT_EQ(lengths[0], 17);
}

TEST_F(AzoteqMultitouch, AllUpClearsWhateverTheChipLeftInTheSlots) {
    put_contact(0, 0);
    put_contact(1, 1);
    set_count(2);
    read_once();
    read_once();
    set_count(0); // the count drops before the slots are cleared
    read_once();
    EXPECT_EQ(lengths[0], 24);
    for (int i = 10; i < 45; i++)
        EXPECT_EQ(captured[i], 0);
    read_once();
    EXPECT_EQ(lengths[0], 10);
}

// Palm and too-many-fingers are flags in the base bytes: passed through
// untouched (multitouch.c drops the contacts on either), with every slot the
// count asks for read.
TEST_F(AzoteqMultitouch, PalmAndTooManyFingersPassThrough) {
    put_contact(0, 0);
    put_contact(1, 1);
    set_count(2);
    read_once();
    source[4] = 2; // palm
    read_once();
    EXPECT_EQ(captured[4], 2);
    EXPECT_EQ(reads, 1);
    source[4] = 4; // too many fingers
    set_count(6);
    read_once();
    EXPECT_EQ(captured[4], 4);
    EXPECT_EQ(captured[5], 6);
    EXPECT_EQ(reads, 2); // the count is past the slots read: the rest are fetched
    EXPECT_EQ(memcmp(captured, source, 45), 0);
}

TEST_F(AzoteqMultitouch, AFailedReadKeepsTheNextReadSize) {
    put_contact(0, 0);
    put_contact(1, 1);
    set_count(2);
    read_once();
    read_once();
    error_flags                    = 1; // not a NACK
    read_status                    = I2C_STATUS_TIMEOUT;
    azoteq_iqs5xx_base_data_t base = {};
    reads                          = 0;
    EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_EQ(lengths[0], 24);
    EXPECT_FALSE(valid);
    read_status = I2C_STATUS_SUCCESS;
    read_once();
    EXPECT_EQ(lengths[0], 24);
    EXPECT_TRUE(slot_is(0, true) && slot_is(1, true));
}

TEST_F(AzoteqMultitouch, NoFingerReadsOnlyTheBaseBytes) {
    memset(source + 10, 0xAA, 35); // stale slot bytes in the chip
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(lengths[0], 10);
    EXPECT_EQ(ends, 1);
    EXPECT_TRUE(valid);
    for (int i = 10; i < 45; i++)
        EXPECT_EQ(captured[i], 0);
}

TEST_F(AzoteqMultitouch, ReadFailureCannotPublishStaleContacts) {
    read_status = -1;
    error_flags = 1; // not a NACK: no retry
    memset(source, 0xff, sizeof(source));
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_FALSE(valid);
    EXPECT_EQ(ends, 0);
    EXPECT_EQ(observed_read, read_status);
    EXPECT_EQ(observed_end, I2C_STATUS_SUCCESS);
    for (auto value : captured)
        EXPECT_EQ(value, 0);
}

TEST_F(AzoteqMultitouch, SessionCloseFailureInvalidatesSample) {
    end_status                     = -1;
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_FALSE(valid);
    EXPECT_EQ(ends, 1);
    EXPECT_EQ(observed_read, I2C_STATUS_SUCCESS);
    EXPECT_EQ(observed_end, end_status);
}

TEST_F(AzoteqMultitouch, CpiCannotChangeAbsoluteCoordinateDomain) {
    azoteq_iqs5xx_set_cpi(400);
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(ends, 0);
}

TEST_F(AzoteqMultitouch, WakeNackRetriesOnceWithoutInvalidatingContacts) {
    read_status                    = I2C_STATUS_ERROR;
    recover_second                 = true;
    source[5]                      = 1;
    source[14]                     = 1;
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
    EXPECT_EQ(reads, 3); // the base read, its retry, the slots
    EXPECT_EQ(ends, 1);
    EXPECT_TRUE(valid);
    EXPECT_TRUE(retried);
    EXPECT_TRUE(recovered);
    EXPECT_EQ(fault_calls, 1);
    EXPECT_EQ(memcmp(captured, source, 45), 0);
}

TEST_F(AzoteqMultitouch, PersistentNackStopsAfterOneRetry) {
    read_status                    = I2C_STATUS_ERROR;
    azoteq_iqs5xx_base_data_t base = {};
    EXPECT_EQ(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_ERROR);
    EXPECT_EQ(reads, 2);
    EXPECT_EQ(ends, 0);
    EXPECT_FALSE(valid);
    EXPECT_TRUE(retried);
    EXPECT_FALSE(recovered);
}

TEST_F(AzoteqMultitouch, TimeoutAndOtherBusFaultsAreNotRetried) {
    for (auto status : {I2C_STATUS_TIMEOUT, I2C_STATUS_ERROR}) {
        reads = ends                   = 0;
        retried                        = false;
        error_flags                    = 1; // arbitration lost
        read_status                    = status;
        azoteq_iqs5xx_base_data_t base = {};
        EXPECT_NE(azoteq_iqs5xx_get_base_data(&base), I2C_STATUS_SUCCESS);
        EXPECT_EQ(reads, 1);
        EXPECT_FALSE(retried);
        EXPECT_FALSE(valid);
    }
}

// The mouse path on a host that never asks for touchpad mode (macOS, a BIOS):
// the chip's gestures through this build's contact read, polled the way the
// USB half runs it (the last buttons come back in). Press and hold (a still
// finger for AZOTEQ_IQS5XX_HOLD_TIME, 20 ms here) and a tap must let go.
TEST_F(AzoteqMultitouch, GestureButtonsReleaseInTheMousePath) {
    uint8_t held = 0;
    auto    poll = [&](uint8_t g0, uint8_t g1, uint8_t fingers) {
        source[1] = g0; // gesture events 0: bit 0 single tap, bit 1 press and hold
        source[2] = g1; // gesture events 1: bit 0 two-finger tap
        set_count(fingers);
        if (fingers)
            put_contact(0, 0);
        else
            lift(0);
        report_mouse_t in = {};
        in.buttons        = held;
        held              = azoteq_iqs5xx_get_report(in).buttons;
        return held;
    };
    const uint8_t b1 = 1 << POINTING_DEVICE_BUTTON1, b2 = 1 << POINTING_DEVICE_BUTTON2;
    EXPECT_EQ(poll(0, 0, 1), 0);
    for (int i = 0; i < 10; i++)
        EXPECT_EQ(poll(0x02, 0, 1), b1); // firm press: press and hold
    EXPECT_EQ(poll(0, 0, 0), 0) << "left button stays down after the finger lifts";
    EXPECT_EQ(poll(0, 0x01, 0), b2); // two-finger tap
    EXPECT_EQ(poll(0, 0, 0), 0) << "right button stays down after a two-finger tap";
    EXPECT_EQ(poll(0x01, 0, 0), b1); // single tap
    EXPECT_EQ(poll(0, 0, 0), 0) << "left button stays down after a tap";
}
