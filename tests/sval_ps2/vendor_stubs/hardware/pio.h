// Host test stand-in for the pico-sdk PIO API: one state machine with a 4-deep
// RX FIFO, the RX interrupt enable, FDEBUG, and counts of reads and restarts.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "gpio.h"

#define PIO_IRQ0_INTF_SM0_RXNEMPTY_BITS 0x001u
#define PIO_IRQ0_INTF_SM0_TXNFULL_BITS 0x010u
#define PIO_FDEBUG_RXSTALL_LSB 0
#define KHZ 1000

typedef struct {
    uint32_t ints0, fdebug;
    uint32_t rx[4];
    int      rx_count;
    bool     rx_irq_enabled, enabled;
    int      restarts, gets;
} pio_hw_t;
typedef pio_hw_t *PIO;
extern pio_hw_t   sim_pio;
#define pio0 (&sim_pio)
#define pio1 (&sim_pio)

enum pio_interrupt_source { pis_sm0_rx_fifo_not_empty = 0, pis_sm0_tx_fifo_not_full = 4 };
struct pio_program {
    const uint16_t *instructions;
    uint8_t         length;
    int8_t          origin;
};
typedef struct {
    int unused;
} pio_sm_config;

static inline uint pio_get_index(PIO p) {
    (void)p;
    return 1;
}
static inline int pio_claim_unused_sm(PIO p, bool required) {
    (void)p;
    (void)required;
    return 0;
}
static inline uint pio_add_program(PIO p, const struct pio_program *prog) {
    (void)p;
    (void)prog;
    return 11;
}
static inline pio_sm_config pio_get_default_sm_config(void) {
    pio_sm_config c = {0};
    return c;
}
#define sm_config_set_wrap(c, a, b) ((void)(c))
#define sm_config_set_clkdiv(c, d) ((void)(c))
#define sm_config_set_set_pins(c, a, b) ((void)(c))
#define sm_config_set_out_pins(c, a, b) ((void)(c))
#define sm_config_set_out_shift(c, a, b, n) ((void)(c))
#define sm_config_set_in_shift(c, a, b, n) ((void)(c))
#define sm_config_set_jmp_pin(c, a) ((void)(c))
#define sm_config_set_in_pins(c, a) ((void)(c))
static inline void pio_sm_set_consecutive_pindirs(PIO p, uint sm, uint base, uint n, bool out) {
    (void)p, (void)sm, (void)base, (void)n, (void)out;
}
static inline void pio_sm_init(PIO p, uint sm, uint offset, const pio_sm_config *c) {
    (void)p, (void)sm, (void)offset, (void)c;
}
static inline void pio_sm_set_enabled(PIO p, uint sm, bool on) {
    (void)sm;
    p->enabled = on;
}
static inline void pio_set_irq0_source_enabled(PIO p, enum pio_interrupt_source s, bool on) {
    if (s == pis_sm0_rx_fifo_not_empty) p->rx_irq_enabled = on;
}
static inline uint32_t pio_sm_get(PIO p, uint sm) {
    (void)sm;
    p->gets++;
    if (p->rx_count == 0) return 0;
    uint32_t v = p->rx[0];
    for (int i = 1; i < p->rx_count; i++)
        p->rx[i - 1] = p->rx[i];
    p->rx_count--;
    return v;
}
static inline void pio_sm_put(PIO p, uint sm, uint32_t v) {
    (void)p, (void)sm, (void)v;
}
static inline bool pio_sm_is_tx_fifo_full(PIO p, uint sm) {
    (void)p, (void)sm;
    return false;
}
static inline void pio_sm_clear_fifos(PIO p, uint sm) {
    (void)sm;
    p->rx_count = 0;
}
static inline void pio_sm_restart(PIO p, uint sm) {
    (void)sm;
    p->restarts++;
}
static inline uint pio_encode_jmp(uint addr) {
    return addr;
}
static inline void pio_sm_exec(PIO p, uint sm, uint instr) {
    (void)p, (void)sm, (void)instr;
}
