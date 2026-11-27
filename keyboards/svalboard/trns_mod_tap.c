// SPDX-License-Identifier: GPL-2.0-or-later
#include "trns_mod_tap.h"

static bool   in_mod_tap       = false;
static int8_t in_mod_tap_layer = -1;
static keypos_t in_mod_tap_key; // the mod-tap's key, while in_mod_tap

int8_t sval_trns_mod_tap_layer(uint16_t keycode, const keyrecord_t *record) {
    if (!IS_QK_MOD_TAP(keycode) || (keycode & 0xFF) != KC_TRANSPARENT) return -1;
    if (record->tap.count == 0 || in_mod_tap || in_mod_tap_layer != -1 || !record->event.pressed) return -1;
    // The layer the key resolves on, which is not always the highest one: a
    // higher layer where the key is transparent (the mouse layer while Auto
    // Mouse has it on) would leave the press on the same mod-tap, whose tap
    // then types nothing.
    return (int8_t)layer_switch_get_layer(record->event.key);
}

void sval_trns_mod_tap_begin(uint8_t layer, keypos_t key) {
    in_mod_tap_layer = (int8_t)layer;
    in_mod_tap_key   = key;
    layer_state      = layer_state & ~((layer_state_t)1 << layer);
    in_mod_tap       = true;
}

bool sval_trns_mod_tap_end(const keyrecord_t *record) {
    // The mod-tap's own release, not that of another key let go meanwhile
    // (nor a combo's, which has no key position).
    if (record->event.pressed || !in_mod_tap) return false;
    if (IS_COMBOEVENT(record->event) || !KEYEQ(record->event.key, in_mod_tap_key)) return false;
    in_mod_tap       = false;
    layer_state      = layer_state | ((layer_state_t)1 << in_mod_tap_layer);
    in_mod_tap_layer = -1;
    return true;
}

int8_t sval_trns_mod_tap_lifted_layer(void) {
    return in_mod_tap ? in_mod_tap_layer : -1;
}
