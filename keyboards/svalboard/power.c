/*
Copyright 2026 Morgan Venable @_claussen

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
// Deep-idle power control for the RP2040: run the core from the USB PLL at
// 48 MHz with the system PLL off, and stop clocking blocks nobody uses while
// the core sits in WFI. USB keeps working because it runs from the USB PLL
// regardless; the ChibiOS system timer and wait_us run from the 1 MHz TIMER
// tick (clk_ref), so scan timing is unaffected. PIO state machines (split
// serial, WS2812) derive their rate from clk_sys, so their dividers are
// rescaled on every switch to keep the baud and LED timing identical.

#include "quantum.h"
#include "svalboard.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/pio.h"
#include "hardware/structs/clocks.h"
#include "power_policy.h"

#define SVAL_CLK_FULL_HZ 125000000u
#define SVAL_CLK_USB_HZ   48000000u
#define SVAL_CLK_XOSC_HZ  12000000u

static bool               clock_low = false;
static uint32_t           low_hz    = SVAL_CLK_USB_HZ;
static sval_pio_reclock_t pio_reclock_state;
static bool               led_written;
static uint32_t           led_write_ms;

uint8_t sval_deep_clock_mhz(void) {
    switch (global_saved_values.scan_deep_clock_idx) {
        case 1:  return 24;
        case 2:  return 12;
        default: return 48;
    }
}

// The 8 state machines' dividers and which are running (power_policy.h).
static uint8_t pio_read(uint32_t clkdiv[SVAL_PIO_SMS]) {
    PIO     pios[2] = {pio0, pio1};
    uint8_t enabled = 0;
    for (int p = 0; p < 2; p++) {
        for (int sm = 0; sm < 4; sm++) {
            clkdiv[p * 4 + sm] = pios[p]->sm[sm].clkdiv;
            if (pios[p]->ctrl & (1u << (PIO_CTRL_SM_ENABLE_LSB + sm))) enabled |= (uint8_t)(1u << (p * 4 + sm));
        }
    }
    return enabled;
}

static void pio_write(const uint32_t clkdiv[SVAL_PIO_SMS], uint8_t enabled) {
    PIO pios[2] = {pio0, pio1};
    for (int i = 0; i < SVAL_PIO_SMS; i++) {
        if (enabled & (1u << i)) pios[i / 4]->sm[i % 4].clkdiv = clkdiv[i];
    }
}

// Scale the divider of every enabled state machine so (clk_sys / div) stays
// constant across a clock change; restore the exact saved values on the way back.
// A state machine started while the clock was low (a pointer driver the search
// started) has nothing saved and is scaled up instead.
static void pio_reclock(uint32_t from_hz, uint32_t to_hz, bool restore) {
    uint32_t clkdiv[SVAL_PIO_SMS];
    uint8_t  enabled = pio_read(clkdiv);
    if (restore) {
        sval_pio_reclock_up(&pio_reclock_state, clkdiv, enabled, from_hz, to_hz);
    } else {
        sval_pio_reclock_down(&pio_reclock_state, clkdiv, enabled, from_hz, to_hz);
    }
    pio_write(clkdiv, enabled);
}

bool    sval_clock_is_low(void) { return clock_low; }
uint8_t sval_clock_mhz(void) { return (clock_low ? low_hz : SVAL_CLK_FULL_HZ) / 1000000u; }

void sval_clock_low(void) {
    if (clock_low) return;
    uint32_t hz = (uint32_t)sval_deep_clock_mhz() * 1000000u;
    // A clock too low for a state machine's rate (the WS2812 one at 12 MHz) waits
    // until the last LED write is out at the full clock.
    uint32_t clkdiv[SVAL_PIO_SMS];
    uint8_t  enabled = pio_read(clkdiv);
    if (!sval_clock_low_allowed(sval_pio_would_clamp(clkdiv, enabled, SVAL_CLK_FULL_HZ, hz), led_written, timer_elapsed32(led_write_ms))) return;
    // 48 and 24 MHz come from the USB PLL (the pico-sdk's set_sys_clock_48mhz sequence,
    // with the clk_sys divider for 24); 12 MHz is the crystal directly. The USB PLL stays
    // up in every case because USB needs it.
    if (hz == SVAL_CLK_XOSC_HZ) {
        clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_XOSC_CLKSRC, SVAL_CLK_XOSC_HZ, SVAL_CLK_XOSC_HZ);
    } else {
        clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB, SVAL_CLK_USB_HZ, hz);
    }
    pll_deinit(pll_sys);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, hz, hz);
    pio_reclock(SVAL_CLK_FULL_HZ, hz, false);
    low_hz    = hz;
    clock_low = true;
}

void sval_clock_full(void) {
    if (!clock_low) return;
    // Same PLL settings as the pico-sdk's clocks_init(): 1500 MHz VCO / 6 / 2 = 125 MHz.
    pll_init(pll_sys, 1, 1500000000u, 6, 2);
    clock_configure(clk_sys, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX, CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS, SVAL_CLK_FULL_HZ, SVAL_CLK_FULL_HZ);
    clock_configure(clk_peri, 0, CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS, SVAL_CLK_FULL_HZ, SVAL_CLK_FULL_HZ);
    pio_reclock(low_hz, SVAL_CLK_FULL_HZ, true);
    clock_low = false;
}

// Every WS2812 write goes through here (LDFLAGS --wrap=ws2812_flush, rules.mk):
// rgblight's, the split sync's on the other half, and the updater's. When the
// low clock had to clamp a divider, the LED timing would be off, so the clock
// goes back to full first; it goes low again once the frame is out.
void __real_ws2812_flush(void) __attribute__((weak));
void __wrap_ws2812_flush(void) {
    if (sval_led_write_needs_full(clock_low, pio_reclock_state.clamped_mask)) sval_clock_full();
    led_written  = true;
    led_write_ms = timer_read32();
    if (__real_ws2812_flush) __real_ws2812_flush();
}

uint32_t sval_deep_nap_us(void) {
    return is_keyboard_master() ? SVAL_DEEP_NAP_MASTER_US : SVAL_DEEP_NAP_SLAVE_US;
}

// SLEEP_EN only matters while the core is in WFI. Everything a sleeping board
// still needs (USB, TIMER, the PIOs, DMA, the SPI the sensor is on, SRAM, the
// bus fabric) stays on; blocks this build never uses are gated.
void sval_sleep_gating_init(void) {
    uint32_t en0 = CLOCKS_SLEEP_EN0_CLK_SYS_JTAG_BITS;
    uint32_t en1 = CLOCKS_SLEEP_EN1_CLK_SYS_TBMAN_BITS;
#if !defined(HAL_USE_ADC) || !HAL_USE_ADC
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_ADC_BITS | CLOCKS_SLEEP_EN0_CLK_ADC_ADC_BITS;
#endif
#if !defined(HAL_USE_PWM) || !HAL_USE_PWM
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_PWM_BITS;
#endif
#if !defined(HAL_USE_I2C) || !HAL_USE_I2C
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_I2C0_BITS | CLOCKS_SLEEP_EN0_CLK_SYS_I2C1_BITS;
#endif
#if !defined(HAL_USE_RTC) || !HAL_USE_RTC
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_RTC_BITS | CLOCKS_SLEEP_EN0_CLK_RTC_RTC_BITS;
#endif
#if !defined(RP_SPI_USE_SPI0) || !RP_SPI_USE_SPI0
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_SPI0_BITS | CLOCKS_SLEEP_EN0_CLK_PERI_SPI0_BITS;
#endif
#if !defined(RP_SPI_USE_SPI1) || !RP_SPI_USE_SPI1
    en0 |= CLOCKS_SLEEP_EN0_CLK_SYS_SPI1_BITS | CLOCKS_SLEEP_EN0_CLK_PERI_SPI1_BITS;
#endif
#if (!defined(HAL_USE_SERIAL) || !HAL_USE_SERIAL) && (!defined(HAL_USE_SIO) || !HAL_USE_SIO) && (!defined(HAL_USE_UART) || !HAL_USE_UART)
    en1 |= CLOCKS_SLEEP_EN1_CLK_SYS_UART0_BITS | CLOCKS_SLEEP_EN1_CLK_PERI_UART0_BITS | CLOCKS_SLEEP_EN1_CLK_SYS_UART1_BITS | CLOCKS_SLEEP_EN1_CLK_PERI_UART1_BITS;
#endif
    hw_clear_bits(&clocks_hw->sleep_en0, en0);
    hw_clear_bits(&clocks_hw->sleep_en1, en1);
}
