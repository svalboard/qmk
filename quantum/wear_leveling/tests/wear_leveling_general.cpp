// Copyright 2022 Nick Brassel (@tzarc)
// SPDX-License-Identifier: GPL-2.0-or-later
#include <numeric>
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "backing_mocks.hpp"

class WearLevelingGeneral : public ::testing::Test {
   protected:
    void SetUp() override {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
    }
};

/**
 * This test verifies that consolidated data whose checksum never matches is reported as lost, and the store reset,
 * rather than the write log being replayed over an empty cache.
 */
TEST_F(WearLevelingGeneral, InvalidChecksum_StoreReset) {
    auto& inst     = MockBackingStore::Instance();
    auto  logstart = inst.storage_begin() + (WEAR_LEVELING_LOGICAL_SIZE / sizeof(backing_store_int_t));

    // Generate a test block of data
    std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> testvalue;
    std::iota(testvalue.begin(), testvalue.end(), 0x20);

    // Write the data
    EXPECT_EQ(wear_leveling_write(0, testvalue.data(), testvalue.size()), WEAR_LEVELING_CONSOLIDATED) << "Write returned incorrect status";

    // Invalidate the checksum
    (logstart + 0)->erase();
    (logstart + 1)->erase();
    (logstart + 2)->erase();
    (logstart + 3)->erase();

    // Set up a 1-byte logical write of [0x11] at logical offset 0x01
    auto entry0 = LOG_ENTRY_MAKE_OPTIMIZED_64(0x01, 0x11);
    (logstart + 4)->set(~entry0.raw16[0]);

    // Re-init
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_CONSOLIDATED) << "Init returned incorrect status";
    EXPECT_TRUE(wear_leveling_data_lost()) << "Lost data should have been reported";
    EXPECT_EQ(wear_leveling_read(0, testvalue.data(), testvalue.size()), WEAR_LEVELING_SUCCESS) << "Failed to read";
    for (int i = 0; i < WEAR_LEVELING_LOGICAL_SIZE; ++i) {
        EXPECT_EQ(testvalue[i], 0x00) << "Store should have been reset";
    }
}

/**
 * This test verifies that a bad read during initialization is retried, keeping the stored data.
 */
TEST_F(WearLevelingGeneral, TransientBadRead_DataKept) {
    auto& inst = MockBackingStore::Instance();

    std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> testvalue;
    std::iota(testvalue.begin(), testvalue.end(), 0x20);
    EXPECT_EQ(wear_leveling_write(0, testvalue.data(), testvalue.size()), WEAR_LEVELING_CONSOLIDATED) << "Write returned incorrect status";
    EXPECT_EQ(wear_leveling_write(1, "\x11", 1), WEAR_LEVELING_SUCCESS) << "Write returned incorrect status";

    // Corrupt the first read of the consolidated data only
    bool corrupted = false;
    inst.set_read_callback([&](std::uint32_t address, backing_store_int_t& value) {
        if (address == 0 && !corrupted) {
            corrupted = true;
            value ^= 1;
        }
    });
    uint64_t erase_count = inst.erase_invoke_count();

    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_SUCCESS) << "Init returned incorrect status";
    EXPECT_TRUE(corrupted) << "Test did not corrupt a read";
    EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
    EXPECT_EQ(inst.erase_invoke_count(), erase_count) << "Nothing should have been erased";
    testvalue[1] = 0x11;
    std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE> readback;
    EXPECT_EQ(wear_leveling_read(0, readback.data(), readback.size()), WEAR_LEVELING_SUCCESS) << "Failed to read";
    EXPECT_EQ(readback, testvalue) << "Stored data should have been kept";
}

/**
 * This test verifies that a store which has never been consolidated keeps its write log, and is not reported as lost.
 */
TEST_F(WearLevelingGeneral, LogOnlyStore_Kept) {
    auto& inst = MockBackingStore::Instance();
    EXPECT_EQ(wear_leveling_write(3, "\x42", 1), WEAR_LEVELING_SUCCESS) << "Write returned incorrect status";
    uint64_t erase_count = inst.erase_invoke_count();

    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_SUCCESS) << "Init returned incorrect status";
    EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
    EXPECT_EQ(inst.erase_invoke_count(), erase_count) << "Nothing should have been erased";
    uint8_t tmp;
    wear_leveling_read(3, &tmp, sizeof(tmp));
    EXPECT_EQ(tmp, 0x42) << "Stored data should have been kept";
}

/**
 * This test verifies that an erased store initializes empty, without reporting lost data.
 */
TEST_F(WearLevelingGeneral, ErasedStore_NotLost) {
    auto& inst = MockBackingStore::Instance();
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_SUCCESS) << "Init returned incorrect status";
    EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Nothing should have been erased";
}

/**
 * This test verifies that writing the same data multiple times does not result in subsequent writes to the backing store.
 */
TEST_F(WearLevelingGeneral, SameValue_SingleBackingWrite) {
    auto& inst = MockBackingStore::Instance();

    uint8_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(0x02, &test_val, sizeof(test_val)), WEAR_LEVELING_SUCCESS) << "First overall write operation should have succeeded";

    uint64_t invoke_count = inst.unlock_invoke_count();
    uint64_t erase_count  = inst.erase_invoke_count();
    uint64_t write_count  = inst.write_invoke_count();
    uint64_t lock_count   = inst.lock_invoke_count();

    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(wear_leveling_write(0x02, &test_val, sizeof(test_val)), WEAR_LEVELING_SUCCESS) << "Subsequent overall write operation should have succeeded";

        EXPECT_EQ(inst.unlock_invoke_count(), invoke_count) << "Unlock count should match";
        EXPECT_EQ(inst.erase_invoke_count(), erase_count) << "Erase count should match";
        EXPECT_EQ(inst.write_invoke_count(), write_count) << "Write count should match";
        EXPECT_EQ(inst.lock_invoke_count(), lock_count) << "Lock count should match";
    }
}

/**
 * This test verifies that no other invocations occur if `backing_store_init()` fails.
 */
TEST_F(WearLevelingGeneral, InitFailure) {
    auto& inst = MockBackingStore::Instance();
    inst.reset_instance(); // make sure the counters are all zero
    inst.set_init_callback([](std::uint64_t count) { return false; });

    EXPECT_EQ(inst.erasure_count(), 0) << "Invalid initial erase count";
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init should have failed";
    EXPECT_EQ(inst.erasure_count(), 0) << "Invalid final erase count";

    EXPECT_EQ(inst.init_invoke_count(), 1) << "Init should have been invoked once";
    EXPECT_EQ(inst.unlock_invoke_count(), 0) << "Unlock should not have been invoked";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";
}

/**
 * This test verifies that no invocations occur if the supplied address is out of range while writing.
 */
TEST_F(WearLevelingGeneral, WriteFailure_OOB) {
    auto& inst = MockBackingStore::Instance();

    uint8_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(0x21349830, &test_val, sizeof(test_val)), WEAR_LEVELING_FAILED) << "Overall write operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 0) << "Unlock should not have been invoked";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";
}

/**
 * This test verifies that a single write occurs if the supplied address and data length hits the edge of the logical area.
 */
TEST_F(WearLevelingGeneral, WriteSuccess_BoundaryOK) {
    auto& inst = MockBackingStore::Instance();

    uint16_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(WEAR_LEVELING_LOGICAL_SIZE - sizeof(test_val), &test_val, sizeof(test_val)), WEAR_LEVELING_SUCCESS) << "Overall write operation should have succeeded";

    EXPECT_EQ(inst.unlock_invoke_count(), 1) << "Unlock should have been invoked once";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 2) << "Write should have been invoked twice";
    EXPECT_EQ(inst.lock_invoke_count(), 1) << "Lock should have been invoked once";
}

/**
 * This test verifies that no invocations occur if the supplied address and length would generate writes outside the logical range.
 */
TEST_F(WearLevelingGeneral, WriteFailure_BoundaryOverflow) {
    auto& inst = MockBackingStore::Instance();

    uint16_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(WEAR_LEVELING_LOGICAL_SIZE - sizeof(test_val) + 1, &test_val, sizeof(test_val)), WEAR_LEVELING_FAILED) << "Overall write operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 0) << "Unlock should not have been invoked";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";
}

/**
 * This test verifies that no invocations occur if the supplied address is out of range while reading.
 */
TEST_F(WearLevelingGeneral, ReadFailure_OOB) {
    auto& inst = MockBackingStore::Instance();

    uint8_t test_val = 0;
    EXPECT_EQ(wear_leveling_read(0x21349830, &test_val, sizeof(test_val)), WEAR_LEVELING_FAILED) << "Overall read operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 0) << "Unlock should not have been invoked";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";
}

/**
 * This test verifies that no write invocations occur if `backing_store_unlock()` fails.
 */
TEST_F(WearLevelingGeneral, UnlockFailure_NoWrite) {
    auto& inst = MockBackingStore::Instance();
    inst.set_unlock_callback([](std::uint64_t count) { return false; });

    uint8_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(0x04, &test_val, sizeof(test_val)), WEAR_LEVELING_FAILED) << "Overall write operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 1) << "Unlock should have been invoked once";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";

    test_val = 0;
    wear_leveling_read(0x04, &test_val, sizeof(test_val));
    EXPECT_EQ(test_val, 0x14) << "Readback should come from cache regardless of unlock failure";
}

/**
 * This test verifies that no erase invocations occur if `backing_store_unlock()` fails.
 */
TEST_F(WearLevelingGeneral, UnlockFailure_NoErase) {
    auto& inst = MockBackingStore::Instance();
    inst.set_unlock_callback([](std::uint64_t count) { return false; });

    EXPECT_EQ(wear_leveling_erase(), WEAR_LEVELING_FAILED) << "Overall erase operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 1) << "Unlock should have been invoked once";
    EXPECT_EQ(inst.erase_invoke_count(), 0) << "Erase should not have been invoked";
    EXPECT_EQ(inst.write_invoke_count(), 0) << "Write should not have been invoked";
    EXPECT_EQ(inst.lock_invoke_count(), 0) << "Lock should not have been invoked";
}

/**
 * This test verifies that a log entry which fails to program is not programmed again: the store consolidates
 * instead, and when that fails too, the next write retries the consolidation rather than appending.
 * (Svalboard: upstream left write_address on the failed slot, and the next write programmed it again.)
 */
TEST_F(WearLevelingGeneral, WriteFailure_ConsolidatesInstead) {
    auto& inst = MockBackingStore::Instance();
    inst.set_write_callback([](std::uint64_t count, std::uint32_t address) { return false; });

    uint8_t test_val = 0x14;
    EXPECT_EQ(wear_leveling_write(0x04, &test_val, sizeof(test_val)), WEAR_LEVELING_FAILED) << "Overall write operation should have failed";

    EXPECT_EQ(inst.unlock_invoke_count(), 1) << "Unlock should have been invoked once";
    EXPECT_EQ(inst.erase_invoke_count(), 1) << "Erase should have been invoked once, to consolidate";
    EXPECT_EQ(inst.write_invoke_count(), 2) << "The entry, then the consolidated data, should each have been tried once";
    EXPECT_EQ(inst.lock_invoke_count(), 1) << "Lock should have been invoked once";

    inst.set_write_callback(nullptr);
    uint8_t other = 0x22;
    EXPECT_EQ(wear_leveling_write(0x05, &other, sizeof(other)), WEAR_LEVELING_CONSOLIDATED) << "The next write should consolidate";
    EXPECT_EQ(inst.erase_invoke_count(), 2) << "Erase should have been invoked again";

    test_val = 0;
    wear_leveling_read(0x04, &test_val, sizeof(test_val));
    EXPECT_EQ(test_val, 0x14) << "Readback should come from cache regardless of unlock failure";
}
