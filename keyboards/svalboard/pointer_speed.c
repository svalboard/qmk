// SPDX-License-Identifier: GPL-2.0-or-later
#include "pointer_speed.h"
#include "scroll_speed.h"

uint16_t sval_dpi_module_max(uint8_t module) {
    switch (module) {
        case SVAL_DPI_MODULE_PMW3360: return SVAL_DPI_MAX_PMW3360;
        case SVAL_DPI_MODULE_PMW3389: return SVAL_DPI_MAX_PMW3389;
        case SVAL_DPI_MODULE_PAD: return SVAL_PAD_CAP_DPI_X;
        case SVAL_DPI_MODULE_TRACKPOINT: return SVAL_DPI_MAX_TRACKPOINT;
        default: return 0xFFFF;
    }
}

uint8_t sval_dpi_index_limit(const uint16_t *choices, uint8_t n, uint8_t module) {
    return sval_dpi_index_at_most(choices, n, sval_dpi_module_max(module));
}

uint8_t sval_dpi_index_clamp(const uint16_t *choices, uint8_t n, uint8_t module, uint8_t index) {
    uint8_t max = sval_dpi_index_limit(choices, n, module);
    return index > max ? max : index;
}

uint8_t sval_trackpoint_base_index(const uint16_t *choices, uint8_t n) {
    return sval_dpi_index_at_most(choices, n, SVAL_TRACKPOINT_BASE_DPI);
}

void sval_tp_scale(sval_tp_scale_t *s, uint16_t cpi, int32_t x, int32_t y, int32_t *out_x, int32_t *out_y) {
    if (cpi == 0 || cpi == SVAL_TRACKPOINT_BASE_DPI) {
        s->rem_x = s->rem_y = 0;
        s->cpi              = cpi;
        *out_x              = x;
        *out_y              = y;
        return;
    }
    if (s->cpi != cpi) { // a part of a count belongs to the speed it came from
        s->cpi   = cpi;
        s->rem_x = s->rem_y = 0;
    }
    int32_t tx = x * (int32_t)cpi + s->rem_x, ty = y * (int32_t)cpi + s->rem_y;
    *out_x   = tx / SVAL_TRACKPOINT_BASE_DPI; // C division truncates toward zero
    *out_y   = ty / SVAL_TRACKPOINT_BASE_DPI;
    s->rem_x = tx - *out_x * SVAL_TRACKPOINT_BASE_DPI;
    s->rem_y = ty - *out_y * SVAL_TRACKPOINT_BASE_DPI;
}
