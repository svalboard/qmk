// Copyright 2022 Stefan Kerkmann
// SPDX-License-Identifier: GPL-2.0-or-later

#include <ch.h>

#include "serial.h"
#include "serial_protocol.h"
#include "synchronization_util.h"

static inline bool initiate_transaction(uint8_t transaction_id);

#if defined(SERIAL_PIO_RX_DMA)
// With SERIAL_PIO_RX_DMA, QMK's own per-scan transactions (matrix, pointing, layers and
// the rest: their slave side copies a buffer) get a short deadline, so a
// failure costs about 3 ms instead of 20; the RPC transactions keep the full
// one, because their slave callbacks can take time (the updater's relay,
// settings writes). A caller may ask for the short one for an RPC it knows is
// quick (the touchpad's snapshot fetch, serial_transport_short_rpc). The
// half without USB waits for the master's buffer, which follows its
// handshake at once, for the short deadline too: a master that gave up never
// sends it, and the next transaction's bytes must not be taken for it.
#    include "transaction_id_define.h"
void          serial_transport_set_timeout_ms(uint32_t ms); // serial_vendor.c
void          serial_transport_rx_idle(void);               // serial_vendor.c: stop the receive DMA
volatile bool serial_transport_short_rpc;
#    ifndef SERIAL_USART_TIMEOUT
#        define SERIAL_USART_TIMEOUT 20 // as serial_usart.h
#    endif
#    ifndef SERIAL_USART_SHORT_TIMEOUT
#        define SERIAL_USART_SHORT_TIMEOUT 3
#    endif
#endif

#ifdef SVAL_TOUCH_DEV
#    include "transaction_id_define.h"
// Svalboard dev builds: where master transactions fail (wrapping counters),
// read by the touchpad dev status (keyboards/svalboard/azoteq/multitouch.c):
// 0 handshake not sent, 1 no handshake back, 2 a wrong handshake, 3 buffer
// not sent, 4 buffer not received, 5 PIO framing/break errors, 6 failed
// transactions that are RPC ones, 7 the last failed transaction ID.
volatile uint8_t sval_serial_dbg[8];
// The time master transactions take, from the 1 MHz timer: failed ones (count,
// sum, and a histogram <0.5, <2, <8, <16, <24, >=24 ms), and the longest good one.
#    include "hardware/structs/timer.h"
volatile uint16_t sval_serial_fail_n;
volatile uint32_t sval_serial_fail_us, sval_serial_ok_max_us;
volatile uint8_t  sval_serial_fail_hist[6];
// The slave's side: 0 bad transaction ID, 1 handshake not sent, 2 buffer not
// received, 3 buffer not sent (wrapping).
volatile uint8_t sval_serial_slave_dbg[4];
// Transactions after which the receive FIFO had overflowed (RXSTALL), ok or failed.
volatile uint16_t sval_rx_stall_ok, sval_rx_stall_fail;
bool              sval_serial_rx_stalled(void); // serial_vendor.c
static void sval_serial_fail(uint8_t stage, uint8_t id) {
    sval_serial_dbg[stage]++;
    if (id >= PUT_RPC_INFO && id <= GET_RPC_RESP_DATA) sval_serial_dbg[6]++;
    sval_serial_dbg[7] = id;
}
#    define SVAL_SERIAL_FAIL(stage, id) sval_serial_fail((stage), (id))
#else
#    define SVAL_SERIAL_FAIL(stage, id)
#endif
static inline bool react_to_transaction(void);

/**
 * @brief This thread runs on the slave and responds to transactions initiated
 * by the master.
 */
static THD_WORKING_AREA(waSlaveThread, 1024);
static THD_FUNCTION(SlaveThread, arg) {
    (void)arg;
    chRegSetThreadName("split_protocol_tx_rx");

    while (true) {
        if (unlikely(!react_to_transaction())) {
            /* Clear the receive queue, to start with a clean slate.
             * Parts of failed transactions or spurious bytes could still be in it. */
            serial_transport_driver_clear();
        }
    }
}

/**
 * @brief Slave specific initializations.
 */
void soft_serial_target_init(void) {
    serial_transport_driver_slave_init();

    /* Start transport thread. */
    chThdCreateStatic(waSlaveThread, sizeof(waSlaveThread), HIGHPRIO, SlaveThread, NULL);
}

/**
 * @brief Master specific initializations.
 */
void soft_serial_initiator_init(void) {
    serial_transport_driver_master_init();
}

/**
 * @brief React to transactions started by the master.
 */
#ifdef SVAL_TOUCH_DEV
#    define SVAL_SLAVE_FAIL(i) (sval_serial_slave_dbg[i]++)
#else
#    define SVAL_SLAVE_FAIL(i)
#endif
static inline bool react_to_transaction(void) {
    uint8_t transaction_id = 0;
    /* Wait until there is a transaction for us. */
    if (unlikely(!serial_transport_receive_blocking(&transaction_id, sizeof(transaction_id)))) {
        return false;
    }

    /* Sanity check that we are actually responding to a valid transaction. */
    if (unlikely(transaction_id >= NUM_TOTAL_TRANSACTIONS)) {
        SVAL_SLAVE_FAIL(0);
        return false;
    }

    split_shared_memory_lock_autounlock();

    split_transaction_desc_t* transaction = &split_transaction_table[transaction_id];

    /* Send back the handshake which is XORed as a simple checksum,
     to signal that the slave is ready to receive possible transaction buffers  */
    transaction_id ^= NUM_TOTAL_TRANSACTIONS;
    if (unlikely(!serial_transport_send(&transaction_id, sizeof(transaction_id)))) {
        SVAL_SLAVE_FAIL(1);
        return false;
    }

    /* Receive transaction buffer from the master. If this transaction requires it.*/
    if (transaction->initiator2target_buffer_size) {
#if defined(SERIAL_PIO_RX_DMA)
        serial_transport_set_timeout_ms(SERIAL_USART_SHORT_TIMEOUT);
#endif
        if (unlikely(!serial_transport_receive(split_trans_initiator2target_buffer(transaction), transaction->initiator2target_buffer_size))) {
            SVAL_SLAVE_FAIL(2);
            return false;
        }
    }

    /* Allow any slave processing to occur. */
    if (transaction->slave_callback) {
        transaction->slave_callback(transaction->initiator2target_buffer_size, split_trans_initiator2target_buffer(transaction), transaction->initiator2target_buffer_size, split_trans_target2initiator_buffer(transaction));
    }

    /* Send transaction buffer to the master. If this transaction requires it. */
    if (transaction->target2initiator_buffer_size) {
        if (unlikely(!serial_transport_send(split_trans_target2initiator_buffer(transaction), transaction->target2initiator_buffer_size))) {
            SVAL_SLAVE_FAIL(3);
            return false;
        }
    }

    return true;
}

/**
 * @brief Start transaction from the master half to the slave half.
 *
 * @param index Transaction Table index of the transaction to start.
 * @return bool Indicates success of transaction.
 */
bool soft_serial_transaction(int index) {
#if defined(SERIAL_PIO_RX_DMA)
    bool rpc = index >= PUT_RPC_INFO && index <= GET_RPC_RESP_DATA;
    serial_transport_set_timeout_ms(!rpc || serial_transport_short_rpc ? SERIAL_USART_SHORT_TIMEOUT : SERIAL_USART_TIMEOUT);
#endif
    /* Clear the receive queue, to start with a clean slate.
     * Parts of failed transactions or spurious bytes could still be in it. */
    serial_transport_driver_clear();

#ifdef SVAL_TOUCH_DEV
    uint32_t t0 = timer_hw->timerawl;
    bool     ok = initiate_transaction((uint8_t)index);
#    if defined(SERIAL_PIO_RX_DMA)
    serial_transport_rx_idle();
#    endif
    uint32_t us = timer_hw->timerawl - t0;
    if (sval_serial_rx_stalled()) {
        if (ok)
            sval_rx_stall_ok++;
        else
            sval_rx_stall_fail++;
    }
    if (ok) {
        if (us > sval_serial_ok_max_us) sval_serial_ok_max_us = us;
    } else {
        sval_serial_fail_n++;
        sval_serial_fail_us += us;
        uint8_t b = us < 500 ? 0 : us < 2000 ? 1 : us < 8000 ? 2 : us < 16000 ? 3 : us < 24000 ? 4 : 5;
        sval_serial_fail_hist[b]++;
    }
    return ok;
#else
#    if defined(SERIAL_PIO_RX_DMA)
    bool ok = initiate_transaction((uint8_t)index);
    serial_transport_rx_idle();
    return ok;
#    else
    return initiate_transaction((uint8_t)index);
#    endif
#endif
}

/**
 * @brief Initiate transaction to slave half.
 */
static inline bool initiate_transaction(uint8_t transaction_id) {
    /* Sanity check that we are actually starting a valid transaction. */
    if (unlikely(transaction_id >= NUM_TOTAL_TRANSACTIONS)) {
        serial_dprintf("SPLIT: illegal transaction id\n");
        return false;
    }

    split_shared_memory_lock_autounlock();

    split_transaction_desc_t* transaction = &split_transaction_table[transaction_id];

    /* Send transaction table index to the slave, which doubles as basic handshake token. */
    if (unlikely(!serial_transport_send(&transaction_id, sizeof(transaction_id)))) {
        serial_dprintf("SPLIT: sending handshake failed\n");
        SVAL_SERIAL_FAIL(0, transaction_id);
        return false;
    }

    uint8_t transaction_id_shake = 0xFF;

    /* Which we always read back first so that we can error out correctly.
     *   - due to the half duplex limitations on return codes, we always have to read *something*.
     *   - without the read, write only transactions *always* succeed, even during the boot process where the slave is not ready.
     */
    if (unlikely(!serial_transport_receive(&transaction_id_shake, sizeof(transaction_id_shake)) || (transaction_id_shake != (transaction_id ^ NUM_TOTAL_TRANSACTIONS)))) {
        serial_dprintf("SPLIT: receiving handshake failed\n");
        SVAL_SERIAL_FAIL(transaction_id_shake == 0xFF ? 1 : 2, transaction_id);
        return false;
    }

    /* Send transaction buffer to the slave. If this transaction requires it. */
    if (transaction->initiator2target_buffer_size) {
        if (unlikely(!serial_transport_send(split_trans_initiator2target_buffer(transaction), transaction->initiator2target_buffer_size))) {
            serial_dprintf("SPLIT: sending buffer failed\n");
            SVAL_SERIAL_FAIL(3, transaction_id);
            return false;
        }
    }

    /* Receive transaction buffer from the slave. If this transaction requires it. */
    if (transaction->target2initiator_buffer_size) {
        if (unlikely(!serial_transport_receive(split_trans_target2initiator_buffer(transaction), transaction->target2initiator_buffer_size))) {
            serial_dprintf("SPLIT: receiving buffer failed\n");
            SVAL_SERIAL_FAIL(4, transaction_id);
            return false;
        }
    }

    return true;
}
