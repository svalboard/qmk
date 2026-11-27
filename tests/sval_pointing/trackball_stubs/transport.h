// Host-test stand-in for QMK's transport.h, only what squal.c uses.
#pragma once
#include <stdbool.h>
static inline bool is_transport_connected(void) {
    return false;
}
