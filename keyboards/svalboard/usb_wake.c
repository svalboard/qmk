// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_wake.h"
#include "suspend.h"

bool sval_usb_wake_key_down(void) {
    return suspend_wakeup_condition();
}
