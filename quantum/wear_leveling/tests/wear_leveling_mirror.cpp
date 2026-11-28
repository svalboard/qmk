// Copyright 2026 Svalboard
// SPDX-License-Identifier: GPL-2.0-or-later
#include <numeric>
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "backing_mocks.hpp"

using Data = std::array<std::uint8_t, WEAR_LEVELING_LOGICAL_SIZE>;

static constexpr std::size_t COPY_ELEMENTS  = WEAR_LEVELING_BACKING_SIZE / sizeof(backing_store_int_t);
static constexpr std::size_t IMAGE_ELEMENTS = WEAR_LEVELING_LOGICAL_SIZE / sizeof(backing_store_int_t);

class WearLevelingMirror : public ::testing::Test {
   protected:
    void SetUp() override {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
    }

    static MockBackingStoreElement& element(std::size_t copy, std::size_t index) {
        return *(MockBackingStore::Instance().storage_begin() + copy * COPY_ELEMENTS + index);
    }

    static void damage(std::size_t copy, std::size_t index) {
        backing_store_int_t value = ~element(copy, index).get();
        element(copy, index).erase();
        element(copy, index).set(~(value ^ 0x5A));
    }

    static bool copies_equal() {
        for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
            if (element(0, i).get() != element(1, i).get()) {
                return false;
            }
        }
        return true;
    }

    // Consolidated data plus one write log entry.
    static Data seed() {
        Data data;
        std::iota(data.begin(), data.end(), 0x20);
        EXPECT_EQ(wear_leveling_write(0, data.data(), data.size()), WEAR_LEVELING_CONSOLIDATED) << "Write returned incorrect status";
        data[3] = 0x77;
        EXPECT_EQ(wear_leveling_write(3, &data[3], 1), WEAR_LEVELING_SUCCESS) << "Write returned incorrect status";
        return data;
    }

    static Data stored() {
        Data data;
        EXPECT_EQ(wear_leveling_read(0, data.data(), data.size()), WEAR_LEVELING_SUCCESS) << "Failed to read";
        return data;
    }

    static void expect_recovered(const Data& expected) {
        EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
        EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
        EXPECT_EQ(stored(), expected) << "Stored data should have been kept";
        EXPECT_TRUE(copies_equal()) << "Copies should match after init";
    }
};

/**
 * This test verifies that every write reaches both copies.
 */
TEST_F(WearLevelingMirror, WritesReachEveryCopy) {
    seed();
    EXPECT_TRUE(copies_equal()) << "Copies should match";
}

/**
 * This test verifies that a healthy store is loaded without rewriting anything.
 */
TEST_F(WearLevelingMirror, HealthyStore_NoRewrite) {
    auto     data        = seed();
    uint64_t erase_count = MockBackingStore::Instance().erase_invoke_count();
    expect_recovered(data);
    EXPECT_EQ(MockBackingStore::Instance().erase_invoke_count(), erase_count) << "Nothing should have been erased";
}

/**
 * This test verifies that damage to the first copy is recovered from the second, and the first repaired.
 */
TEST_F(WearLevelingMirror, DamagedFirstCopy_LoadsSecond) {
    auto data = seed();
    damage(0, 2);
    expect_recovered(data);
}

/**
 * This test verifies that damage to the second copy is repaired from the first.
 */
TEST_F(WearLevelingMirror, DamagedSecondCopy_Repaired) {
    auto data = seed();
    damage(1, IMAGE_ELEMENTS); // its checksum
    expect_recovered(data);
}

/**
 * This test verifies that a first copy left erased by an interrupted consolidation does not hide the second.
 */
TEST_F(WearLevelingMirror, ErasedFirstCopy_LoadsSecond) {
    auto data = seed();
    for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
        element(0, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that a second copy missing the last log entry, from power loss between the two writes, is
 * repaired with it.
 */
TEST_F(WearLevelingMirror, LaggingSecondCopy_Repaired) {
    auto data = seed();
    for (std::size_t i = IMAGE_ELEMENTS + 4; i < COPY_ELEMENTS; ++i) {
        element(1, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that when neither copy can be read, the store is reset and the loss reported.
 */
TEST_F(WearLevelingMirror, BothCopiesDamaged_Reset) {
    seed();
    damage(0, 2);
    damage(1, 2);
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_CONSOLIDATED) << "Init returned incorrect status";
    EXPECT_TRUE(wear_leveling_data_lost()) << "Lost data should have been reported";
    EXPECT_EQ(stored(), Data{}) << "Store should have been reset";
    EXPECT_TRUE(copies_equal()) << "Copies should match after reset";
}

/**
 * This test verifies that a bad read of the first copy is retried rather than rewriting both copies.
 */
TEST_F(WearLevelingMirror, TransientBadRead_NoRewrite) {
    auto& inst      = MockBackingStore::Instance();
    auto  data      = seed();
    bool  corrupted = false;
    inst.set_read_callback([&](std::uint32_t address, backing_store_int_t& value) {
        if (address == 0 && !corrupted) {
            corrupted = true;
            value ^= 1;
        }
    });
    uint64_t erase_count = inst.erase_invoke_count();
    expect_recovered(data);
    EXPECT_TRUE(corrupted) << "Test did not corrupt a read";
    EXPECT_EQ(inst.erase_invoke_count(), erase_count) << "Nothing should have been erased";
}

/**
 * This test verifies that normal writes and restarts report no write failure.
 */
TEST_F(WearLevelingMirror, HealthyStore_NoWriteFailure) {
    auto data = seed();
    EXPECT_FALSE(wear_leveling_write_failed()) << "No write should have failed";
    expect_recovered(data);
    EXPECT_FALSE(wear_leveling_write_failed()) << "No write should have failed";
}

/**
 * This test verifies that a write that doesn't reach the backing store is reported until the next init, even though
 * the cache, and so every read, already holds the new value.
 */
TEST_F(WearLevelingMirror, FailedWrite_Reported) {
    auto& inst = MockBackingStore::Instance();
    auto  data = seed();
    inst.set_write_callback([](std::uint64_t, std::uint32_t) { return false; });
    std::uint8_t value = data[7] ^ 0xFF;
    EXPECT_EQ(wear_leveling_write(7, &value, 1), WEAR_LEVELING_FAILED) << "Write should have failed";
    EXPECT_TRUE(wear_leveling_write_failed()) << "Failed write should have been reported";
    inst.set_write_callback(nullptr);

    // The failed write's entry is never programmed again: the next write consolidates, which saves both.
    std::uint8_t other = data[9] ^ 0xFF;
    EXPECT_EQ(wear_leveling_write(9, &other, 1), WEAR_LEVELING_CONSOLIDATED) << "Later write should succeed, by consolidating";
    EXPECT_TRUE(wear_leveling_write_failed()) << "Failure should stay reported until the next init";

    data[7] = value;
    data[9] = other;
    expect_recovered(data);
    EXPECT_FALSE(wear_leveling_write_failed()) << "Init should clear the report";
}

/**
 * This test verifies that a backing store that can't be opened is reported, so that nothing written afterwards is
 * mistaken for saved.
 */
TEST_F(WearLevelingMirror, FailedInit_Reported) {
    auto& inst = MockBackingStore::Instance();
    seed();
    inst.set_init_callback([](std::uint64_t) { return false; });
    EXPECT_EQ(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init should have failed";
    EXPECT_TRUE(wear_leveling_write_failed()) << "Failed init should have been reported";
    EXPECT_FALSE(wear_leveling_data_lost()) << "Nothing was read, so nothing is known to be lost";
}

/**
 * This test verifies that a failed erase is reported.
 */
TEST_F(WearLevelingMirror, FailedErase_Reported) {
    auto& inst = MockBackingStore::Instance();
    seed();
    inst.set_erase_callback([](std::uint64_t) { return false; });
    EXPECT_EQ(wear_leveling_erase(), WEAR_LEVELING_FAILED) << "Erase should have failed";
    EXPECT_TRUE(wear_leveling_write_failed()) << "Failed erase should have been reported";
}

/**
 * This test verifies that power loss at any write or erase of a consolidating write leaves every byte either old or
 * new, never reports lost data, and leaves a store that keeps working.
 */
TEST_F(WearLevelingMirror, PowerLossDuringConsolidation) {
    for (int failing_erase = 0; failing_erase <= 1; ++failing_erase) {
        for (std::uint64_t step = 1;; ++step) {
            auto& inst = MockBackingStore::Instance();
            inst.reset_instance();
            wear_leveling_init();
            auto before = seed();
            Data after;
            std::iota(after.begin(), after.end(), 0x80);

            std::uint64_t writes = inst.write_invoke_count(), erases = inst.erase_invoke_count();
            bool          cut = false;
            if (failing_erase) {
                inst.set_erase_callback([&](std::uint64_t count) { return !(cut = cut || count - erases == step); });
            } else {
                inst.set_write_callback([&](std::uint64_t count, std::uint32_t) { return !(cut = cut || count - writes == step); });
            }
            wear_leveling_write(0, after.data(), after.size());
            inst.set_erase_callback(nullptr);
            inst.set_write_callback(nullptr);
            if (!cut) {
                break; // every step has been interrupted once
            }

            EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed at step " << step;
            EXPECT_FALSE(wear_leveling_data_lost()) << "Data lost at step " << step;
            auto data = stored();
            for (std::size_t i = 0; i < data.size(); ++i) {
                EXPECT_TRUE(data[i] == before[i] || data[i] == after[i]) << "Byte " << i << " corrupted at step " << step;
            }
            EXPECT_TRUE(copies_equal()) << "Copies differ at step " << step;
            expect_recovered(data);
            EXPECT_EQ(wear_leveling_write(0, after.data(), after.size()) == WEAR_LEVELING_FAILED, false) << "Store stopped working at step " << step;
            expect_recovered(after);
        }
    }
}

static constexpr std::size_t LOG_START = IMAGE_ELEMENTS + 8 / sizeof(backing_store_int_t);

// First erased log slot of a copy.
static std::size_t log_end(std::size_t copy) {
    std::size_t i = LOG_START;
    while (i < COPY_ELEMENTS && !(MockBackingStore::Instance().storage_begin() + copy * COPY_ELEMENTS + i)->is_erased()) {
        ++i;
    }
    return i;
}

// Power is cut after `ops` more program or erase operations (each erased element counts as one): from then on every
// write and erase fails, as on a board that lost power. reboot() restores power and runs init.
// An erase stops only between erase units, as the RP2040 driver erases one 64 KiB block at a time: here a copy's
// consolidated image is one unit, and its checksum with the write log another (on Svalboard the checksum and the start
// of the log share a block).
static std::uint64_t power_ops;
static std::uint64_t erase_call;
static std::size_t   erase_element;
static void          power_cut_after(std::uint64_t ops) {
    auto& inst    = MockBackingStore::Instance();
    power_ops     = ops;
    erase_call    = 0;
    erase_element = 0;
    inst.set_write_callback([](std::uint64_t, std::uint32_t) { return power_ops > 0 && power_ops-- > 0; });
    inst.set_erase_callback([](std::uint64_t call) {
        if (call != erase_call) {
            erase_call    = call;
            erase_element = 0;
        }
        std::size_t element = erase_element++ % COPY_ELEMENTS;
        if (element != 0 && element != IMAGE_ELEMENTS) {
            return power_ops > 0; // inside a unit: it completes once started
        }
        return power_ops > 0 && power_ops-- > 0;
    });
}
static bool power_was_cut() {
    return power_ops == 0;
}
static void power_restore() {
    auto& inst = MockBackingStore::Instance();
    inst.set_write_callback(nullptr);
    inst.set_erase_callback(nullptr);
}

/**
 * This test verifies that power lost at any step of the repair a boot makes, when the copies differ, never loses the
 * copy the data was loaded from: the other copies are rewritten first. (S5)
 */
TEST_F(WearLevelingMirror, PowerLossDuringBootRepair_KeepsSource) {
    for (std::uint64_t ops = 0;; ++ops) {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
        auto data = seed();
        damage(1, IMAGE_ELEMENTS); // copy 1's checksum: copy 0 loads, and the boot repairs copy 1
        power_cut_after(ops);
        wear_leveling_init();
        bool cut = power_was_cut();
        power_restore();
        expect_recovered(data);
        if (!cut) {
            break; // the repair finished: every step has been interrupted once
        }
    }
}

/**
 * This test verifies that power lost at any step of a small write leaves the old or the new value, never anything
 * else, and a store that keeps working: no slot is programmed twice afterwards. (S1)
 */
TEST_F(WearLevelingMirror, PowerLossDuringWrite_OldOrNew) {
    // Each write is one log entry: a single byte below 64, else one multi-byte entry of 2 to 5 bytes.
    struct {
        std::uint32_t address;
        std::size_t   length;
    } writes[] = {{3, 1}, {WEAR_LEVELING_LOGICAL_SIZE - 1, 1}, {WEAR_LEVELING_LOGICAL_SIZE - 2, 2}, {WEAR_LEVELING_LOGICAL_SIZE - 5, 5}};
    for (auto w : writes) {
        std::uint32_t address = w.address;
        std::size_t   length  = w.length;
        if (address < 64 && length > 1) {
            continue; // more than one entry
        }
        for (std::uint64_t ops = 0;; ++ops) {
            MockBackingStore::Instance().reset_instance();
            wear_leveling_init();
            auto before = seed();
            auto after  = before;
            for (std::size_t i = 0; i < length; ++i) {
                after[address + i] = 0xC0 + i;
            }
            power_cut_after(ops);
            wear_leveling_write(address, &after[address], length);
            bool cut = power_was_cut();
            power_restore();

            EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed at step " << ops;
            EXPECT_FALSE(wear_leveling_data_lost()) << "Data lost at step " << ops;
            auto data = stored();
            EXPECT_TRUE(data == before || data == after) << "Write to " << address << " cut at step " << ops << " left neither the old nor the new value";
            EXPECT_TRUE(copies_equal()) << "Copies differ at step " << ops;

            // The store keeps working: a later write lands, and reads back after a restart.
            std::uint8_t v = data[1] ^ 0xFF;
            data[1]        = v;
            EXPECT_NE(wear_leveling_write(1, &v, 1), WEAR_LEVELING_FAILED) << "Store stopped working at step " << ops;
            expect_recovered(data);
            if (!cut) {
                break;
            }
        }
    }
}

/**
 * This test verifies that a log entry whose program fails, in either copy, is never programmed again: the store
 * consolidates instead, and every value written reads back after a restart. (S6)
 */
TEST_F(WearLevelingMirror, FailedEntryProgram_NotReprogrammed) {
    for (std::uint64_t failing = 1; failing <= 2; ++failing) {
        auto& inst = MockBackingStore::Instance();
        inst.reset_instance();
        wear_leveling_init();
        auto          data   = seed();
        std::uint64_t writes = inst.write_invoke_count();
        inst.set_write_callback([&](std::uint64_t count, std::uint32_t) { return count - writes != failing; });
        data[5] ^= 0xFF;
        EXPECT_NE(wear_leveling_write(5, &data[5], 1), WEAR_LEVELING_FAILED) << "The retry by consolidation should have saved the write";
        data[6] ^= 0xFF;
        EXPECT_NE(wear_leveling_write(6, &data[6], 1), WEAR_LEVELING_FAILED) << "Later write failed";
        inst.set_write_callback(nullptr);
        EXPECT_FALSE(wear_leveling_write_failed()) << "Nothing was lost, so nothing should be reported";
        expect_recovered(data);
    }
}

// Writes single bytes until one log slot is left: the next single-byte write fills it and consolidates.
static void fill_log(Data& data) {
    while (log_end(0) < COPY_ELEMENTS - 1) {
        data[0] ^= 0x01;
        EXPECT_EQ(wear_leveling_write(0, &data[0], 1), WEAR_LEVELING_SUCCESS) << "Filling write failed";
    }
}

/**
 * This test verifies that after a consolidation's erase fails, later writes never append over the other copy's data
 * or past the store, and nothing is reported lost at the next boot. (S4)
 */
TEST_F(WearLevelingMirror, FailedConsolidationErase_NoAppend) {
    for (int persistent = 0; persistent <= 1; ++persistent) {
        auto& inst = MockBackingStore::Instance();
        inst.reset_instance();
        wear_leveling_init();
        auto data = seed();
        fill_log(data);
        std::uint64_t erased = 0;
        // Copy 0's erase fails part way through
        inst.set_erase_callback([&](std::uint64_t) { return persistent ? erased++ < 3 : erased++ != 3; });
        data[1] ^= 0xFF;
        wear_leveling_write(1, &data[1], 1); // fills the last slot of each copy, then consolidates
        auto saved = data;
        EXPECT_TRUE(persistent == 0 || wear_leveling_write_failed()) << "The failed consolidation should be reported";
        data[2] ^= 0xFF;
        wear_leveling_write(2, &data[2], 1);
        data[3] ^= 0xFF;
        wear_leveling_write(3, &data[3], 1);
        inst.set_erase_callback(nullptr);
        if (persistent) {
            // Nothing after the failure was saved: the boot loads copy 1, as it was before.
            EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
            EXPECT_FALSE(wear_leveling_data_lost()) << "No data should have been lost";
            EXPECT_EQ(stored(), saved) << "Copy 1 should have been kept";
            EXPECT_TRUE(copies_equal()) << "Copies should match after init";
        } else {
            expect_recovered(data); // the retried consolidation saved everything
        }
    }
}

/**
 * This test verifies that after a consolidation fails to write copy 0's image, later writes don't append to copy 1,
 * which was never erased. (S4)
 */
TEST_F(WearLevelingMirror, FailedConsolidationWrite_NoAppend) {
    auto& inst = MockBackingStore::Instance();
    auto  data = seed();
    fill_log(data);
    // The first write of copy 0's image fails, once the consolidation has erased it
    bool          failed = false;
    std::uint64_t erases = inst.erase_invoke_count();
    inst.set_write_callback([&](std::uint64_t, std::uint32_t address) { return failed || inst.erase_invoke_count() == erases || address >= WEAR_LEVELING_BACKING_SIZE || !(failed = true); });
    data[1] ^= 0xFF;
    EXPECT_EQ(wear_leveling_write(1, &data[1], 1), WEAR_LEVELING_FAILED) << "The consolidation should have failed";
    EXPECT_TRUE(failed) << "Test did not fail a write";
    data[2] ^= 0xFF;
    EXPECT_EQ(wear_leveling_write(2, &data[2], 1), WEAR_LEVELING_CONSOLIDATED) << "The next write should retry the consolidation";
    inst.set_write_callback(nullptr);
    expect_recovered(data);
}

/**
 * This test verifies that retrying a consolidation that failed on copy 1 rewrites copy 1 first: copy 0, already
 * rewritten, survives power lost at any step of the retry. (S4, S5)
 */
TEST_F(WearLevelingMirror, RetriedConsolidation_KeepsRewrittenCopy) {
    for (std::uint64_t ops = 0;; ++ops) {
        auto& inst = MockBackingStore::Instance();
        inst.reset_instance();
        wear_leveling_init();
        auto data = seed();
        fill_log(data);
        // The first write of copy 1's image fails
        bool          failed = false;
        std::uint64_t erases = inst.erase_invoke_count();
        inst.set_write_callback([&](std::uint64_t, std::uint32_t) { return failed || inst.erase_invoke_count() < erases + 2 || !(failed = true); });
        data[1] ^= 0xFF;
        EXPECT_EQ(wear_leveling_write(1, &data[1], 1), WEAR_LEVELING_FAILED) << "The consolidation should have failed";
        EXPECT_TRUE(failed) << "Test did not fail a write";
        auto saved = data;
        power_cut_after(ops);
        data[2] ^= 0xFF;
        wear_leveling_write(2, &data[2], 1);
        bool cut = power_was_cut();
        power_restore();
        EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed at step " << ops;
        EXPECT_FALSE(wear_leveling_data_lost()) << "Data lost at step " << ops;
        auto got = stored();
        EXPECT_TRUE(got == saved || got == data) << "Retry cut at step " << ops << " lost data";
        expect_recovered(got);
        if (!cut) {
            break;
        }
    }
}

/**
 * This test verifies the early consolidation a keyboard can run while idle: the free log space shrinks with each
 * write, a consolidation empties the log and keeps the data, and a failed one leaves 0 free until it is retried. (S2)
 */
TEST_F(WearLevelingMirror, EarlyConsolidation) {
    auto&    inst  = MockBackingStore::Instance();
    auto     data  = seed();
    uint32_t empty = WEAR_LEVELING_BACKING_SIZE - WEAR_LEVELING_LOGICAL_SIZE - 8;
    EXPECT_LT(wear_leveling_log_free(), empty) << "The seed's log entry should use space";
    EXPECT_EQ(wear_leveling_consolidate(), WEAR_LEVELING_CONSOLIDATED) << "Consolidation failed";
    EXPECT_EQ(wear_leveling_log_free(), empty) << "The log should be empty";
    expect_recovered(data);

    inst.set_erase_callback([](std::uint64_t) { return false; });
    EXPECT_EQ(wear_leveling_consolidate(), WEAR_LEVELING_FAILED) << "Consolidation should have failed";
    EXPECT_EQ(wear_leveling_log_free(), 0u) << "A failed consolidation leaves no log to append to";
    EXPECT_TRUE(wear_leveling_write_failed()) << "The failure should be reported";
    inst.set_erase_callback(nullptr);
    data[4] ^= 0xFF;
    EXPECT_EQ(wear_leveling_write(4, &data[4], 1), WEAR_LEVELING_CONSOLIDATED) << "The next write should retry";
    EXPECT_EQ(wear_leveling_log_free(), empty) << "The log should be empty";
    expect_recovered(data);
}

#if WEAR_LEVELING_LOGICAL_SIZE > 64

/**
 * This test verifies that a multi-word log entry cut off by the end of a copy, from power loss while it was written,
 * ends the log instead of being completed with whatever follows the copy.
 */
TEST_F(WearLevelingMirror, EntryCutOffAtCopyEnd_Ignored) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    for (std::size_t copy = 0; copy < 2; ++copy) {
        // Fill the log with harmless entries, leaving only the last slot
        for (std::size_t i = log_end(copy); i + 1 < COPY_ELEMENTS; ++i) {
            element(copy, i).set(~LOG_ENTRY_MAKE_OPTIMIZED_64(0, data[0]).raw16[0]);
        }
        // Only the first word of a write to address 100 made it into the last slot
        element(copy, COPY_ELEMENTS - 1).set(~LOG_ENTRY_MAKE_MULTIBYTE(100, 1).raw16[0]);
    }
    // Power was lost after consolidation had erased the first copy
    for (std::size_t i = 0; i < COPY_ELEMENTS; ++i) {
        element(0, i).erase();
    }
    expect_recovered(data);
}

/**
 * This test verifies that a copy which replays completely wins over one whose log stops at an invalid entry.
 */
TEST_F(WearLevelingMirror, TruncatedFirstCopy_LoadsSecond) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    std::size_t slot = log_end(0);
    element(0, slot).set(0x3FFF);                                         // invalid entry type in the first copy
    element(1, slot).set(~LOG_ENTRY_MAKE_OPTIMIZED_64(5, 0x99).raw16[0]); // a real entry in the second
    data[5] = 0x99;
    expect_recovered(data);
}

/**
 * This test verifies that when every copy's log stops at an invalid entry, the entries before it are kept and
 * nothing is reported lost.
 */
TEST_F(WearLevelingMirror, BothCopiesTruncated_PrefixKept) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    std::size_t slot = log_end(0);
    element(0, slot).set(0x3FFF);
    element(1, slot).set(0x3FFF);
    expect_recovered(data);
}

/**
 * This test verifies that a multi-byte entry with a length no writer produces (0, 6 or 7) is rejected rather than
 * replayed, which for 6 and 7 copied bytes from past the entry. (S18)
 */
TEST_F(WearLevelingMirror, InvalidMultibyteLength_Rejected) {
    for (std::uint8_t length : {0, 6, 7}) {
        MockBackingStore::Instance().reset_instance();
        wear_leveling_init();
        auto data = seed();
        EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
        auto entry = LOG_ENTRY_MAKE_MULTIBYTE(64, length);
        for (std::size_t i = 3; i < 8; ++i) {
            entry.raw8[i] = 0xA0 + i;
        }
        std::size_t slot = log_end(0);
        for (std::size_t copy = 0; copy < 2; ++copy) {
            for (std::size_t w = 0; w < 4; ++w) {
                element(copy, slot + w).set(~entry.raw16[w]);
            }
        }
        expect_recovered(data);
    }
}

/**
 * This test verifies that words programmed after the end of the log, the rest of an entry whose first word was never
 * programmed, are not appended over: the boot rewrites the log. (S1)
 */
TEST_F(WearLevelingMirror, CutOffEntryAfterLogEnd_Rewritten) {
    auto data = seed();
    EXPECT_NE(wear_leveling_init(), WEAR_LEVELING_FAILED) << "Init failed";
    std::size_t slot = log_end(0);
    for (std::size_t copy = 0; copy < 2; ++copy) {
        for (std::size_t w = 1; w < 4; ++w) {
            // words that would read as entries of their own once a write filled the slot before them
            element(copy, slot + w).set(~LOG_ENTRY_MAKE_OPTIMIZED_64(10 + w, 0x55).raw16[0]);
        }
    }
    expect_recovered(data);
    data[9] ^= 0xFF;
    EXPECT_EQ(wear_leveling_write(9, &data[9], 1), WEAR_LEVELING_SUCCESS) << "Write failed";
    expect_recovered(data);
}
#endif
