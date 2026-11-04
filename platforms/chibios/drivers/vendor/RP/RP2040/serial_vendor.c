// Copyright 2022 Stefan Kerkmann
// SPDX-License-Identifier: GPL-2.0-or-later

#include "serial_usart.h"
#include "serial_protocol.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "wait.h"
#include "debug.h"

#if !defined(MCU_RP)
#    error PIO Driver is only available for Raspberry Pi 2040 MCUs!
#endif

static inline bool receive_impl(uint8_t* destination, const size_t size, sysinterval_t timeout);
static inline bool send_impl(const uint8_t* source, const size_t size);
static inline void pio_serve_interrupt(void);

#define MSG_PIO_ERROR ((msg_t)(-3))

#if defined(SERIAL_PIO_USE_PIO1)
static const PIO pio = pio1;

OSAL_IRQ_HANDLER(RP_PIO1_IRQ_0_HANDLER) {
    OSAL_IRQ_PROLOGUE();
    pio_serve_interrupt();
    OSAL_IRQ_EPILOGUE();
}
#else
static const PIO pio = pio0;

OSAL_IRQ_HANDLER(RP_PIO0_IRQ_0_HANDLER) {
    OSAL_IRQ_PROLOGUE();
    pio_serve_interrupt();
    OSAL_IRQ_EPILOGUE();
}
#endif

#define UART_TX_WRAP_TARGET 0
#define UART_TX_WRAP 3

// clang-format off
#if defined(SERIAL_USART_FULL_DUPLEX)
static const uint16_t uart_tx_program_instructions[] = {
            //     .wrap_target
    0x9fa0, //  0: pull   block           side 1 [7]
    0xf727, //  1: set    x, 7            side 0 [7]
    0x6001, //  2: out    pins, 1
    0x0642, //  3: jmp    x--, 2                 [6]
            //     .wrap
};
#else
static const uint16_t uart_tx_program_instructions[] = {
            //     .wrap_target
    0x9fa0, //  0: pull   block           side 1 [7]
    0xf727, //  1: set    x, 7            side 0 [7]
    0x6081, //  2: out    pindirs, 1
    0x0642, //  3: jmp    x--, 2                 [6]
            //     .wrap
};
#endif
// clang-format on

static const pio_program_t uart_tx_program = {
    .instructions = uart_tx_program_instructions,
    .length       = 4,
    .origin       = -1,
};

#define UART_RX_WRAP_TARGET 0
#define UART_RX_WRAP 8

// clang-format off
static const uint16_t uart_rx_program_instructions[] = {
            //     .wrap_target
    0x2020, //  0: wait   0 pin, 0
    0xea27, //  1: set    x, 7                   [10]
    0x4001, //  2: in     pins, 1
    0x0642, //  3: jmp    x--, 2                 [6]
    0x00c8, //  4: jmp    pin, 8
    0xc020, //  5: irq    wait 0
    0x20a0, //  6: wait   1 pin, 0
    0x0000, //  7: jmp    0
    0x8020, //  8: push   block
            //     .wrap
};
// clang-format on

static const pio_program_t uart_rx_program = {
    .instructions = uart_rx_program_instructions,
    .length       = 9,
    .origin       = -1,
};

thread_reference_t rx_thread        = NULL;

#if defined(SERIAL_PIO_RX_DMA)
// SERIAL_PIO_RX_DMA: the master's receive by DMA. The RX state machine's FIFO is
// drained by a DMA channel into a 256-byte ring as each byte arrives, so no
// interrupt or thread wake is needed per byte, and an interrupt or thread
// that is late no longer overflows the 8-byte FIFO (which lost bits and
// failed the transaction). The receive waits for its bytes with its own
// deadline. The slave keeps the interrupt receive: its high-priority thread
// drains in time, and it waits for a transaction with no deadline.
#    include "hardware/structs/timer.h"
static bool                    rx_dma_on = false;
static const rp_dma_channel_t* rx_dma;
static volatile uint8_t        rx_ring[256] __attribute__((aligned(256))); // written by the DMA only
static uint32_t                rx_ring_read; // bytes taken since the channel started
#    define RX_DMA_COUNT 0xFFFFFFFFu
// The shared DMA interrupt's priority, as the WS2812 driver passes it. This
// channel never raises it (no completion interrupt).
#    if !defined(RP_DMA_PRIORITY_SERIAL_RX)
#        define RP_DMA_PRIORITY_SERIAL_RX 3
#    endif
static volatile bool rx_dma_error = false; // a framing or break error since the receive started
#endif
// The receive deadline in force (Svalboard: per transaction, serial_protocol.c).
static uint32_t rx_timeout_ms = SERIAL_USART_TIMEOUT;
void            serial_transport_set_timeout_ms(uint32_t ms) {
    rx_timeout_ms = ms ? ms : SERIAL_USART_TIMEOUT;
}
static int         rx_state_machine = -1;

thread_reference_t tx_thread        = NULL;
static int         tx_state_machine = -1;

void pio_serve_interrupt(void) {
    uint32_t irqs = pio->ints0;

    // The RX FIFO is not empty any more, therefore wake any sleeping rx thread
    if (irqs & (PIO_IRQ0_INTF_SM0_RXNEMPTY_BITS << rx_state_machine)) {
        // Disable rx not empty interrupt
        pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty + rx_state_machine, false);

        osalSysLockFromISR();
        osalThreadResumeI(&rx_thread, MSG_OK);
        osalSysUnlockFromISR();
    }

    // The TX FIFO is not full any more, therefore wake any sleeping tx thread
    if (irqs & (PIO_IRQ0_INTF_SM0_TXNFULL_BITS << tx_state_machine)) {
        // Disable tx not full interrupt
        pio_set_irq0_source_enabled(pio, pis_sm0_tx_fifo_not_full + tx_state_machine, false);
        osalSysLockFromISR();
        osalThreadResumeI(&tx_thread, MSG_OK);
        osalSysUnlockFromISR();
    }

    // IRQ 0 is set on framing or break errors by the rx state machine
    if (pio_interrupt_get(pio, 0UL)) {
        pio_interrupt_clear(pio, 0UL);
#ifdef SVAL_TOUCH_DEV
        extern volatile uint8_t sval_serial_dbg[8];
        sval_serial_dbg[5]++;
#endif
#if defined(SERIAL_PIO_RX_DMA)
        rx_dma_error = true;
#endif

        osalSysLockFromISR();
        osalThreadResumeI(&rx_thread, MSG_PIO_ERROR);
        osalSysUnlockFromISR();
    }
}

#if !defined(SERIAL_USART_FULL_DUPLEX)
// The internal pull-ups of the RP2040 are rather weakish with a range of 50k to
// 80k, which in turn do not provide enough current to guarantee fast signal rise
// times with a parasitic capacitance of greater than 100pf. In real world
// applications, like split keyboards which might have vias in the signal path
// or long PCB traces, this prevents a successful communication. The solution
// is to temporarily augment the weak pull ups from the receiving side by
// driving the tx pin high. On the receiving side the lowest possible drive
// strength is chosen because the transmitting side must still be able to drive
// the signal low. With this configuration the rise times are fast enough and
// the generated low level with 360mV will generate a logical zero.
static void __no_inline_not_in_flash_func(enter_rx_state)(void) {
    osalSysLock();
    // Wait for the transmitting state machines FIFO to run empty. At this point
    // the last byte has been pulled from the transmitting state machines FIFO
    // into the output shift register. We have to wait a tiny bit more until
    // this byte is transmitted, before we can turn on the receiving state
    // machine again.
    while (!pio_sm_is_tx_fifo_empty(pio, tx_state_machine)) {
    }
    // Wait for ~11 bits, 1 start bit + 8 data bits + 1 stop bit + 1 bit
    // headroom.
    wait_us(1000000U * 11U / SERIAL_USART_SPEED);
    // Disable tx state machine to not interfere with our tx pin manipulation
    pio_sm_set_enabled(pio, tx_state_machine, false);
    gpio_set_drive_strength(SERIAL_USART_TX_PIN, GPIO_DRIVE_STRENGTH_2MA);
    pio_sm_set_pins_with_mask(pio, tx_state_machine, 1U << SERIAL_USART_TX_PIN, 1U << SERIAL_USART_TX_PIN);
    pio_sm_set_consecutive_pindirs(pio, tx_state_machine, SERIAL_USART_TX_PIN, 1U, false);
    pio_sm_set_enabled(pio, rx_state_machine, true);
    osalSysUnlock();
}

static void __no_inline_not_in_flash_func(leave_rx_state)(void) {
    osalSysLock();
    // In Half-duplex operation the tx pin dual-functions as sender and
    // receiver. To not receive the data we will send, we disable the receiving
    // state machine.
    pio_sm_set_enabled(pio, rx_state_machine, false);
    pio_sm_set_consecutive_pindirs(pio, tx_state_machine, SERIAL_USART_TX_PIN, 1U, true);
    pio_sm_set_pins_with_mask(pio, tx_state_machine, 0U, 1U << SERIAL_USART_TX_PIN);
    gpio_set_drive_strength(SERIAL_USART_TX_PIN, GPIO_DRIVE_STRENGTH_12MA);
    pio_sm_restart(pio, tx_state_machine);
    pio_sm_set_enabled(pio, tx_state_machine, true);
    osalSysUnlock();
}
#else
// All this trickery is gladly not necessary for full-duplex.
static inline void enter_rx_state(void) {}
static inline void leave_rx_state(void) {}
#endif

/**
 * @brief Clear the FIFO of the RX state machine.
 */
#if defined(SERIAL_PIO_RX_DMA)
static void rx_dma_start(void) {
    dmaChannelDisableX(rx_dma);
    while (!pio_sm_is_rx_fifo_empty(pio, rx_state_machine)) {
        pio_sm_clear_fifos(pio, rx_state_machine);
    }
    dmaChannelSetSourceX(rx_dma, (uint32_t)&pio->rxf[rx_state_machine] + 3U); // the byte in bits 31:24
    dmaChannelSetDestinationX(rx_dma, (uint32_t)rx_ring);
    dmaChannelSetCounterX(rx_dma, RX_DMA_COUNT);
    uint32_t dreq = (pio == pio0 ? 4U : 12U) + (uint32_t)rx_state_machine; // DREQ_PIOx_RXn
    dmaChannelSetModeX(rx_dma, DMA_CTRL_TRIG_DATA_SIZE_BYTE | DMA_CTRL_TRIG_INCR_WRITE | DMA_CTRL_TRIG_RING_SEL | DMA_CTRL_TRIG_RING_SIZE(8U) | DMA_CTRL_TRIG_TREQ_SEL(dreq) | DMA_CTRL_TRIG_PRIORITY(1U));
    rx_ring_read = 0;
    rx_dma_error = false;
    dmaChannelEnableX(rx_dma);
}
#endif

// Between transactions the channel is stopped: the updater refuses to
// program or commit flash while any DMA channel is busy (update_split.c,
// update_commit.c), so a channel left running would block every update of
// this half. A transaction starts it (serial_transport_driver_clear) and
// stops it when done (serial_transport_rx_idle, serial_protocol.c).
void serial_transport_rx_idle(void) {
#if defined(SERIAL_PIO_RX_DMA)
    if (rx_dma_on) dmaChannelDisableX(rx_dma);
#endif
}

inline void serial_transport_driver_clear(void) {
#if defined(SERIAL_PIO_RX_DMA)
    if (rx_dma_on) {
        rx_dma_start();
        return;
    }
#endif
    osalSysLock();
    while (!pio_sm_is_rx_fifo_empty(pio, rx_state_machine)) {
        pio_sm_clear_fifos(pio, rx_state_machine);
    }
    osalSysUnlock();
}

static inline msg_t sync_tx(sysinterval_t timeout) {
    msg_t msg = MSG_OK;
    osalSysLock();
    while (pio_sm_is_tx_fifo_full(pio, tx_state_machine)) {
        pio_set_irq0_source_enabled(pio, pis_sm0_tx_fifo_not_full + tx_state_machine, true);
        msg = osalThreadSuspendTimeoutS(&tx_thread, timeout);
        if (msg < MSG_OK) {
            pio_set_irq0_source_enabled(pio, pis_sm0_tx_fifo_not_full + tx_state_machine, false);
            break;
        }
    }
    osalSysUnlock();
    return msg;
}

static inline bool send_impl(const uint8_t* source, const size_t size) {
    size_t send = 0;
    msg_t  msg;
    while (send < size) {
        msg = sync_tx(TIME_MS2I(SERIAL_USART_TIMEOUT));
        if (msg < MSG_OK) {
            return false;
        }

        osalSysLock();
        while (send < size) {
            if (pio_sm_is_tx_fifo_full(pio, tx_state_machine)) {
                break;
            }
            if (send >= size) {
                break;
            }
            pio_sm_put(pio, tx_state_machine, (uint32_t)(*source));
            source++;
            send++;
        }
        osalSysUnlock();
    }

    return send == size;
}

/**
 * @brief Blocking send of buffer with timeout.
 *
 * @return true Send success.
 * @return false Send failed.
 */
inline bool serial_transport_send(const uint8_t* source, const size_t size) {
    leave_rx_state();
    bool result = send_impl(source, size);
    enter_rx_state();

    return result;
}

static inline msg_t sync_rx(sysinterval_t timeout) {
    msg_t msg = MSG_OK;
    osalSysLock();
    while (pio_sm_is_rx_fifo_empty(pio, rx_state_machine)) {
        pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty + rx_state_machine, true);
        msg = osalThreadSuspendTimeoutS(&rx_thread, timeout);
        if (msg < MSG_OK) {
            pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty + rx_state_machine, false);
            break;
        }
    }
    osalSysUnlock();
    return msg;
}

#ifdef SVAL_TOUCH_DEV
volatile uint16_t sval_rx_full; // the receive loop woke to a full FIFO (8 bytes waiting)
// The receive state machine stalled on a full FIFO since the last call (sticky RXSTALL).
bool sval_serial_rx_stalled(void) {
    uint32_t bit = 1u << (PIO_FDEBUG_RXSTALL_LSB + rx_state_machine);
    bool     s   = pio->fdebug & bit;
    if (s) pio->fdebug = bit; // write 1 to clear
    return s;
}
#endif
#if defined(SERIAL_PIO_RX_DMA)
// The wait for bytes still to come spins by default (0). Above 0, the thread
// sleeps for the time they take on the wire (at least this long, us) between
// looks, so the core can wait in WFI while the DMA channel keeps receiving.
// Measured on a trackball pair, 60 s idle with the default power saving: the
// spinning wait keeps the core awake 5.3 % of the time, sleeping 0.5 %, but
// sleeping costs about 4 % of the scan rate (391 to 375 scans/s), from
// the wake-up latency at the end of each receive.
#    ifndef SERIAL_PIO_RX_DMA_POLL_US
#        define SERIAL_PIO_RX_DMA_POLL_US 0
#    endif
#    define RX_DMA_BYTE_US ((10U * 1000000U + SERIAL_USART_SPEED - 1U) / SERIAL_USART_SPEED)
#    ifdef SVAL_TOUCH_DEV
volatile uint32_t sval_rx_awake_us; // dev builds: time awake in receive_dma (wall time less sleeps)
#    endif
// Waits for the bytes the DMA channel has put in the ring, until the
// transaction's deadline (forever: no deadline). A framing or break error
// means a byte was lost: fail now, not at the deadline.
static bool receive_dma(uint8_t* destination, const size_t size, bool forever) {
    size_t   read  = 0U;
    uint32_t start = timer_hw->timerawl, slept = 0U;
    bool     ok;
    while (true) {
        uint32_t written = RX_DMA_COUNT - rx_dma->channel->TRANS_COUNT;
        if (written - rx_ring_read > sizeof(rx_ring)) { // the ring overran: bytes lost
            ok = false;
            break;
        }
        while (rx_ring_read < written && read < size) {
            *destination++ = rx_ring[rx_ring_read++ & 0xFFU];
            read++;
        }
        if (read == size) {
            ok = true;
            break;
        }
        if (rx_dma_error || (!forever && timer_hw->timerawl - start > rx_timeout_ms * 1000U)) {
            ok = false;
            break;
        }
#    if SERIAL_PIO_RX_DMA_POLL_US > 0
        uint32_t us = (uint32_t)(size - read) * RX_DMA_BYTE_US;
        if (us < SERIAL_PIO_RX_DMA_POLL_US) us = SERIAL_PIO_RX_DMA_POLL_US;
        uint32_t t = timer_hw->timerawl;
        chThdSleepMicroseconds(us);
        slept += timer_hw->timerawl - t;
#    endif
    }
#    ifdef SVAL_TOUCH_DEV
    sval_rx_awake_us += timer_hw->timerawl - start - slept;
#    else
    (void)slept;
#    endif
    return ok;
}
#endif

static inline bool receive_impl(uint8_t* destination, const size_t size, sysinterval_t timeout) {
#if defined(SERIAL_PIO_RX_DMA)
    if (rx_dma_on) return receive_dma(destination, size, timeout == TIME_INFINITE);
#endif
    size_t read = 0U;

    while (read < size) {
        msg_t msg = sync_rx(timeout);
        if (msg < MSG_OK) {
            return false;
        }
#ifdef SVAL_TOUCH_DEV
        if (pio_sm_get_rx_fifo_level(pio, rx_state_machine) >= 8) sval_rx_full++;
#endif
        osalSysLock();
        while (true) {
            if (pio_sm_is_rx_fifo_empty(pio, rx_state_machine)) {
                break;
            }
            if (read >= size) {
                break;
            }
            *destination++ = *((uint8_t*)&pio->rxf[rx_state_machine] + 3U);
            read++;
        }
        osalSysUnlock();
    }

    return read == size;
}

/**
 * @brief  Blocking receive of size * bytes with timeout.
 *
 * @return true Receive success.
 * @return false Receive failed, e.g. by timeout.
 */
inline bool serial_transport_receive(uint8_t* destination, const size_t size) {
    return receive_impl(destination, size, TIME_MS2I(rx_timeout_ms));
}

/**
 * @brief  Blocking receive of size * bytes.
 *
 * @return true Receive success.
 * @return false Receive failed.
 */
inline bool serial_transport_receive_blocking(uint8_t* destination, const size_t size) {
    return receive_impl(destination, size, TIME_INFINITE);
}

static inline void pio_tx_init(pin_t tx_pin) {
    uint pio_idx = pio_get_index(pio);
#if defined(SERIAL_USART_PINS_RUNTIME)
    // Loaded once: serial_transport_repin() configures the machine again.
    static int tx_offset = -1;
    if (tx_offset < 0) tx_offset = pio_add_program(pio, &uart_tx_program);
    uint offset = tx_offset;
#else
    uint offset  = pio_add_program(pio, &uart_tx_program);
#endif

#if defined(SERIAL_USART_FULL_DUPLEX)
    // clang-format off
    iomode_t tx_pin_mode = PAL_RP_GPIO_OE |
                           PAL_RP_PAD_SLEWFAST |
                           PAL_RP_PAD_DRIVE4 |
                           (pio_idx == 0 ? PAL_MODE_ALTERNATE_PIO0 : PAL_MODE_ALTERNATE_PIO1);
    // clang-format on
    pio_sm_set_pins_with_mask(pio, tx_state_machine, 1U << tx_pin, 1U << tx_pin);
    pio_sm_set_consecutive_pindirs(pio, tx_state_machine, tx_pin, 1U, true);
#else
    // clang-format off
    iomode_t tx_pin_mode = PAL_RP_PAD_IE |
                           PAL_RP_GPIO_OE |
                           PAL_RP_PAD_SCHMITT |
                           PAL_RP_PAD_PUE |
                           PAL_RP_PAD_SLEWFAST |
                           PAL_RP_PAD_DRIVE12 |
                           PAL_RP_IOCTRL_OEOVER_DRVINVPERI |
                           (pio_idx == 0 ? PAL_MODE_ALTERNATE_PIO0 : PAL_MODE_ALTERNATE_PIO1);
    // clang-format on
    pio_sm_set_pins_with_mask(pio, tx_state_machine, 0U << tx_pin, 1U << tx_pin);
    pio_sm_set_consecutive_pindirs(pio, tx_state_machine, tx_pin, 1U, true);
#endif

    palSetLineMode(tx_pin, tx_pin_mode);

    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset + UART_TX_WRAP_TARGET, offset + UART_TX_WRAP);
#if defined(SERIAL_USART_FULL_DUPLEX)
    sm_config_set_sideset(&config, 2, true, false);
#else
    sm_config_set_sideset(&config, 2, true, true);
#endif
    // OUT shifts to right, no autopull
    sm_config_set_out_shift(&config, true, false, 32);
    // We are mapping both OUT and side-set to the same pin, because sometimes
    // we need to assert user data onto the pin (with OUT) and sometimes
    // assert constant values (start/stop bit)
    sm_config_set_out_pins(&config, tx_pin, 1);
    sm_config_set_sideset_pins(&config, tx_pin);
    // We only need TX, so get an 8-deep FIFO!
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    // SM transmits 1 bit per 8 execution cycles.
    float div = (float)clock_get_hz(clk_sys) / (8 * SERIAL_USART_SPEED);
    sm_config_set_clkdiv(&config, div);
    pio_sm_init(pio, tx_state_machine, offset, &config);
    pio_sm_set_enabled(pio, tx_state_machine, true);
}

static inline void pio_rx_init(pin_t rx_pin) {
#if defined(SERIAL_USART_PINS_RUNTIME)
    static int rx_offset = -1;
    if (rx_offset < 0) rx_offset = pio_add_program(pio, &uart_rx_program);
    uint offset = rx_offset;
#else
    uint offset = pio_add_program(pio, &uart_rx_program);
#endif

#if defined(SERIAL_USART_FULL_DUPLEX)
    uint pio_idx = pio_get_index(pio);
    pio_sm_set_consecutive_pindirs(pio, rx_state_machine, rx_pin, 1, false);
    // clang-format off
    iomode_t rx_pin_mode = PAL_RP_PAD_IE |
                           PAL_RP_PAD_SCHMITT |
                           PAL_RP_PAD_PUE |
                           (pio_idx == 0 ? PAL_MODE_ALTERNATE_PIO0 : PAL_MODE_ALTERNATE_PIO1);
    // clang-format on
    palSetLineMode(rx_pin, rx_pin_mode);
#endif

    pio_sm_config config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, offset + UART_RX_WRAP_TARGET, offset + UART_RX_WRAP);
    sm_config_set_in_pins(&config, rx_pin); // for WAIT, IN
    sm_config_set_jmp_pin(&config, rx_pin); // for JMP
    // Shift to right, autopush disabled
    sm_config_set_in_shift(&config, true, false, 32);
    // Deeper FIFO as we're not doing any TX
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_RX);
    // SM transmits 1 bit per 8 execution cycles.
    float div = (float)clock_get_hz(clk_sys) / (8 * SERIAL_USART_SPEED);
    sm_config_set_clkdiv(&config, div);
    pio_sm_init(pio, rx_state_machine, offset, &config);
    pio_sm_set_enabled(pio, rx_state_machine, true);
}

static inline void pio_init(pin_t tx_pin, pin_t rx_pin) {
    uint pio_idx = pio_get_index(pio);

    /* Get PIOx peripheral out of reset state. */
    hal_lld_peripheral_unreset(pio_idx == 0 ? RESETS_ALLREG_PIO0 : RESETS_ALLREG_PIO1);

    tx_state_machine = pio_claim_unused_sm(pio, true);
    if (tx_state_machine < 0) {
        dprintln("ERROR: Failed to acquire state machine for serial transmission!");
        return;
    }
    pio_tx_init(tx_pin);

    rx_state_machine = pio_claim_unused_sm(pio, true);
    if (rx_state_machine < 0) {
        dprintln("ERROR: Failed to acquire state machine for serial reception!");
        return;
    }
    pio_rx_init(rx_pin);

    // Enable error flag IRQ source for rx state machine
    pio_set_irq0_source_enabled(pio, pis_sm0_rx_fifo_not_empty + rx_state_machine, true);
    pio_set_irq0_source_enabled(pio, pis_sm0_tx_fifo_not_full + tx_state_machine, true);
    pio_set_irq0_source_enabled(pio, pis_interrupt0, true);

    // Enable PIO specific interrupt vector, as the pio implementation is timing
    // critical we use the highest possible priority.
#if defined(SERIAL_PIO_USE_PIO1)
    nvicEnableVector(RP_PIO1_IRQ_0_NUMBER, CORTEX_MAX_KERNEL_PRIORITY);
#else
    nvicEnableVector(RP_PIO0_IRQ_0_NUMBER, CORTEX_MAX_KERNEL_PRIORITY);
#endif

    enter_rx_state();
}

#if defined(SERIAL_USART_PINS_RUNTIME)
#    if !defined(SERIAL_USART_FULL_DUPLEX)
#        error "SERIAL_USART_PINS_RUNTIME needs SERIAL_USART_FULL_DUPLEX"
#    endif
// The keyboard picks the TX and RX pins at run time (for example from a side
// it learns at boot), once for each role, before the PIO takes them.
void serial_usart_pins_kb(bool master, pin_t *tx_pin, pin_t *rx_pin);
void serial_transport_repin(pin_t tx_pin, pin_t rx_pin);
#endif

#if defined(SERIAL_USART_PINS_RUNTIME)
/**
 * @brief Moves a running link to other pins (the master's search for a half
 * that transmits on the line this one does). Call between transactions.
 */
void serial_transport_repin(pin_t tx_pin, pin_t rx_pin) {
    pio_sm_set_enabled(pio, tx_state_machine, false);
    pio_sm_set_enabled(pio, rx_state_machine, false);
    pio_sm_clear_fifos(pio, tx_state_machine);
    pio_sm_clear_fifos(pio, rx_state_machine);
    // The pins it leaves: back to plain inputs with pull-ups (the PIO no longer drives them).
    palSetLineMode(rx_pin, PAL_MODE_INPUT_PULLUP);
    palSetLineMode(tx_pin, PAL_MODE_INPUT_PULLUP);
    pio_tx_init(tx_pin);
    pio_rx_init(rx_pin);
#    if defined(SERIAL_PIO_RX_DMA)
    if (rx_dma_on) {
        rx_dma_start();
        serial_transport_rx_idle();
    }
#    endif
}
#endif

/**
 * @brief PIO driver specific initialization function for the master side.
 */
void serial_transport_driver_master_init(void) {
#if defined(SERIAL_USART_PINS_RUNTIME)
    pin_t tx_pin, rx_pin;
    serial_usart_pins_kb(true, &tx_pin, &rx_pin);
#elif defined(SERIAL_USART_FULL_DUPLEX)
    pin_t tx_pin = SERIAL_USART_TX_PIN;
    pin_t rx_pin = SERIAL_USART_RX_PIN;
#else
    pin_t tx_pin = SERIAL_USART_TX_PIN;
    pin_t rx_pin = SERIAL_USART_TX_PIN;
#endif

#if defined(SERIAL_USART_PIN_SWAP)
    pio_init(rx_pin, tx_pin);
#else
    pio_init(tx_pin, rx_pin);
#endif
#if defined(SERIAL_PIO_RX_DMA)
    if (!rx_dma) rx_dma = dmaChannelAlloc(RP_DMA_CHANNEL_ID_ANY, RP_DMA_PRIORITY_SERIAL_RX, NULL, NULL);
    if (rx_dma) {
        rx_dma_on = true;
        rx_dma_start();
        serial_transport_rx_idle(); // started by each transaction
    }
#endif
}

/**
 * @brief PIO driver specific initialization function for the slave side.
 */
void serial_transport_driver_slave_init(void) {
#if defined(SERIAL_USART_PINS_RUNTIME)
    pin_t tx_pin, rx_pin;
    serial_usart_pins_kb(false, &tx_pin, &rx_pin);
#elif defined(SERIAL_USART_FULL_DUPLEX)
    pin_t tx_pin = SERIAL_USART_TX_PIN;
    pin_t rx_pin = SERIAL_USART_RX_PIN;
#else
    pin_t tx_pin = SERIAL_USART_TX_PIN;
    pin_t rx_pin = SERIAL_USART_TX_PIN;
#endif

    pio_init(tx_pin, rx_pin);
}
