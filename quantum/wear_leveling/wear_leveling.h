// Copyright 2022 Nick Brassel (@tzarc)
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * @typedef Status returned from any wear-leveling API.
 */
typedef enum wear_leveling_status_t {
    WEAR_LEVELING_FAILED,      //< Invocation failed
    WEAR_LEVELING_SUCCESS,     //< Invocation succeeded
    WEAR_LEVELING_CONSOLIDATED //< Invocation succeeded, consolidation occurred
} wear_leveling_status_t;

/**
 * Wear-leveling initialization
 *
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_init(void);

/**
 * Wear-leveling erasure.
 *
 * Clears the wear-leveling area, with the definition that the "reset state" of all data is zero.
 *
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_erase(void);

/**
 * Writes logical data into the backing store.
 *
 * Skips writes if there are no changes to written values. The entire written block is considered when attempting to
 * determine if an overwrite should occur -- if there is any data mismatch the entire block will be written to the log,
 * not just the changed bytes.
 *
 * @param address[in] the logical address to write data
 * @param value[in] pointer to the source buffer
 * @param length[in] length of the data
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_write(uint32_t address, const void* value, size_t length);

/**
 * Reads logical data from the cache.
 *
 * @param address[in] the logical address to read data
 * @param value[out] pointer to the destination buffer
 * @param length[in] length of the data
 * @return Status of the request
 */
wear_leveling_status_t wear_leveling_read(uint32_t address, void* value, size_t length);

/**
 * Whether the last initialization found stored data that could not be read on any attempt, and erased it.
 *
 * @return true if stored data was lost
 */
bool wear_leveling_data_lost(void);

/**
 * Whether the backing store could not be opened at the last initialization, or a write or erase has failed since.
 *
 * Reads are served from the cache, so a failed write still reads back as written: anything changed while this is set
 * may be lost on the next restart.
 *
 * @return true if a change may not have been saved
 */
bool wear_leveling_write_failed(void);
