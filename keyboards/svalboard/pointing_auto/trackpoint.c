// trackpoint/trackpoint.c under its own names (pointing_auto.h). The PS/2 module's
// init runs first, then trackpoint.c's Sprintek setup, as pointing_device_init does.
#define pointing_device_driver_get_report sval_tp_get_report
#define pointing_device_init_kb sval_tp_setup
#define recalibrate_pointer sval_tp_recalibrate
#define sval_pointer_identity sval_tp_identity
#define sval_pointer_own_bus sval_tp_own_bus

// After the renames: headers it pulls in declare these names too.
#include "pointing_auto.h"
#include "../trackpoint/trackpoint.c"

// The PS/2 module's driver functions (ps2_module.c).
bool     sval_ps2_drv_init(void);
uint16_t sval_ps2_drv_get_cpi(void);
void     sval_ps2_drv_set_cpi(uint16_t cpi);

// Records the Pointer speed for sval_tp_get_report, then the PS/2 module's own (a no-op).
static void sval_tp_set_cpi(uint16_t cpi) {
    sval_tp_apply_cpi(cpi);
    sval_ps2_drv_set_cpi(cpi);
}

static bool sval_tp_init(void) {
    bool ok = sval_ps2_drv_init();
    sval_tp_setup();
    return ok;
}

const sval_pointer_driver_t sval_driver_trackpoint = {
    .sensor      = SVAL_SENSOR_TRACKPOINT,
    .init        = sval_tp_init,
    .get_report  = sval_tp_get_report,
    .get_cpi     = sval_ps2_drv_get_cpi,
    .set_cpi     = sval_tp_set_cpi,
    .identity    = sval_tp_identity,
    .recalibrate = sval_tp_recalibrate,
};
