// Find which pointing module is attached, whatever the firmware was built for.
//
// The Svalboard's pointing modules share one connector but each uses its own pins and
// protocol: trackball (PMW3360/PMW3389, SPI on GP24-27), Azoteq trackpad (IQS5xx, I2C on
// GP16/17) and TrackPoint (PS/2 on GP23/24). When a build's own module doesn't answer,
// these probes check the others, so a host can name the right .uf2 (Sval 0x2C).
// A TrackPoint build doesn't look for a trackball (sval_probe_skip_buses).
//
// The probes toggle the pins directly instead of using the chip's SPI/I2C/PIO
// peripherals, so they work in any build. They take each pin from whatever peripheral
// had it, so only run them when the build's own module didn't answer.
//
// The trackball goes first and the trackpad straight after, with no wait between: nearly
// every board has one of the two, most a PMW3389. Their pins don't overlap (SPI on
// GP24-27, I2C on GP16/17). A module that answers ends the search. The TrackPoint goes
// last: GP25 is the trackball's chip select and the TrackPoint's reset line, so the SPI
// probe also pulses a TrackPoint's reset, and PS/2 is asked only SVAL_DETECT_TP_SETTLE_MS
// after the SPI probe, once a TrackPoint that it reset has finished its self-test.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Buses a build's own driver already checked; the probes skip them.
#define SVAL_BUS_PS2 0x01
#define SVAL_BUS_I2C 0x02
#define SVAL_BUS_SPI 0x04

// After the SPI probe, how long a TrackPoint gets to finish the self-test the probe's chip
// select may have started, before PS/2 asks for it. The boot search's 1.5 s
// (pointer_reprobe.h) already allows for a TrackPoint's power-on self-test; this is that
// test again after a reset.
#define SVAL_DETECT_TP_SETTLE_MS 1000u

typedef struct {
    uint8_t detected; // enum sval_pointer_sensor: a module, or SVAL_SENSOR_NO_RESPONSE
    uint8_t id0;      // PMW Product_ID, Azoteq product number high byte, or PS/2 device ID
    uint8_t id1;      // PMW Inverse_Product_ID, or Azoteq product number low byte
} sval_probe_result_t;

// The buses a search leaves out, from the ones the build's own driver uses. A build whose own
// bus is PS/2 (a TrackPoint build) leaves out SPI too: the SPI probe drives GP25, the
// TrackPoint's reset, and reads GP24, its clock, taking it from the PS/2 driver. Such a build
// searches when its stick didn't answer the boot Get Device ID, which can happen with the
// stick there; the I2C probe leaves the PS/2 pins alone (pointing_detect.c).
static inline uint8_t sval_probe_skip_buses(uint8_t own_buses) {
    return (own_buses & SVAL_BUS_PS2) ? own_buses | SVAL_BUS_SPI : own_buses;
}

// Probes the buses not in sval_probe_skip_buses(skip_buses), SPI then I2C then PS/2, stopping
// at the first module. Blocking, with the SVAL_DETECT_TP_SETTLE_MS wait before PS/2 after a
// silent SPI probe: the host test uses it; the firmware's searches go one bus per main loop pass
// (pointer_reprobe.c).
// The I2C pins are left alone when I2C is skipped, so a hardware I2C driver keeps them.
sval_probe_result_t sval_probe_pointers(uint8_t skip_buses);

// Probes one bus (SVAL_BUS_*), for the runtime searches that go one bus per main loop pass.
// The boot search is patient (two PS/2 tries, for a TrackPoint still finishing its self-test);
// later ones try PS/2 once, which caps the pass at the device's 10 ms window to answer.
sval_probe_result_t sval_probe_bus(uint8_t bus, bool patient);

// Which trackball sensor a PMW Product_ID / Inverse_Product_ID pair names.
uint8_t sval_pmw33xx_classify(uint8_t product_id, uint8_t inverse_product_id);

// A runtime search's second look at the build's own module (svalboard.c), before the other
// buses: restarts its driver when it now answers. The default does nothing (NONE).
enum sval_retry_own {
    SVAL_RETRY_NONE,   // nothing answered on its own bus: search the other buses
    SVAL_RETRY_OK,     // its module answered and its driver is running
    SVAL_RETRY_FAILED, // its module answered, and its driver didn't start
    SVAL_RETRY_STOP,   // the build can't restart its module: stop searching
};
uint8_t sval_pointer_retry_own(void);

// The search (pointer_reprobe.c): run from the main loop; whether the first search has
// finished, and what the last search of the other buses found (SVAL_SENSOR_NO_RESPONSE:
// nothing).
void                       sval_pointer_probe_task(void);
bool                       sval_pointer_searched(void);
const sval_probe_result_t *sval_pointer_found(void);

// Called with the probe's result; a build with every driver in starts that module's driver
// (pointing_auto/pointing_auto.c). The default does nothing.
void sval_pointer_select(const sval_probe_result_t *found);
