# MCU name
MCU = RP2040
BOOTLOADER = rp2040
BOARD = GENERIC_RP_RP2040

# we want some pretty lights (RGBLIGHT_ENABLE is in info.json)
RGBLIGHT_SPLIT = yes
RGBLIGHT_DRIVER = ws2812
WS2812_DRIVER   = vendor

CUSTOM_MATRIX = lite

# Key names for the debug console's key log (keycode_names.c).
KEYCODE_STRING_ENABLE = yes
SRC += keycode_names.c
SRC += key_log.c debug_console.c axis_scale.c sval_timing.c matrix.c mainboard.c scanlab.c power.c identity.c pointing_detect.c pointer_reprobe.c build_reset.c velocity_scroll.c pad_follow.c typing_pause.c flicker_latch.c squal.c
# Every firmware restart is a full chip reset (full_reset.c).
SRC += full_reset.c

# Settings start at 0x160000; keep firmware below them.
LDFLAGS += -Wl,-T,keyboards/svalboard/flash_reservation.ld

SERIAL_DRIVER = vendor

POINTING_DEVICE_ENABLE = yes
POINTING_DEVICE_DRIVER = custom

# Use shared endpoint for proper hires scroll feature report handling
MOUSE_SHARED_EP = yes

LAYER_LOCK_ENABLE = yes
AUTO_SHIFT_ENABLE = yes

# this turns on Manna-Harbour's automousekeys:
MH_AUTO_BUTTONS = yes

OS_DETECTION_ENABLE = yes
NO_USB_STARTUP_CHECK = yes

ifeq ($(strip $(MH_AUTO_BUTTONS)), yes)
  OPT_DEFS += -DMH_AUTO_BUTTONS
endif

# Enable VIA (we'll use VIA3 custom values instead of Vial)
VIA_ENABLE = yes

# Allow VIA to read matrix state for Matrix Tester (note: enables keylogger attack vector)
VIA_INSECURE = yes

# Note: Sval module is enabled via keymap.json with "modules": ["svalboard/core"]

# Debounce (debounce_sval.c): the optical keys follow a saved mode, on (the
# default: QMK's sym_defer_g, 5 ms, as release firmware types) or off (straight
# through); the thumb Double Down tact switch (thumb row, COL6, each half) is
# debounced in both, 5 ms per key, deferred.
DEBOUNCE_TYPE = custom
SRC += debounce_sval.c
SRC += link_light.c

# Link light tests (link_light.c): a dev-only raw HID protocol (0xFA) that pauses
# the split link, restarts the other half, or restarts this half as the half
# without USB. Opt in with -e SVAL_LINK_TEST=yes; needs SVAL_UPDATER (the pause);
# release builds refuse it.
ifeq ($(strip $(SVAL_LINK_TEST)), yes)
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    $(error SVAL_LINK_TEST cannot be part of a release build)
  endif
  ifneq ($(strip $(SVAL_UPDATER)), yes)
    $(error SVAL_LINK_TEST needs SVAL_UPDATER=yes)
  endif
  OPT_DEFS += -DSVAL_LINK_TEST
endif
SRC += pad_gestures.c

# On-device action-path tests; opt in with -e SVAL_KEYTEST=yes.
ifeq ($(strip $(SVAL_KEYTEST)), yes)
  SRC += keytest.c
  OPT_DEFS += -DSVAL_KEYTEST
endif

# Timing counters (perf.c: main loop, matrix frames, pad reads, split fetches,
# touchpad frames), read with tools/multitouch/perf.py over raw HID 0xFC. Dev
# builds only: opt in with -e SVAL_PERF_STATS=yes; release builds refuse it.
ifeq ($(strip $(SVAL_PERF_STATS)), yes)
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    $(error SVAL_PERF_STATS cannot be part of a release build)
  endif
  OPT_DEFS += -DSVAL_PERF_STATS
  SRC += perf.c
endif

# Bench builds that reset stored settings when flashed over other firmware; opt in with -e SVAL_TEST_WIPE=yes.
ifeq ($(strip $(SVAL_TEST_WIPE)), yes)
  OPT_DEFS += -DSVAL_TEST_WIPE
endif

# In-firmware updater (docs/updater.md); opt in with -e SVAL_UPDATER=yes.
include keyboards/svalboard/updater/updater.mk

# Dev builds that keep stored settings across flashes (build_reset.c, D37):
# -e SVAL_DEV_KEEP_SETTINGS=yes skips the reset on the first boot of a new build
# and leaves the record alone, so util/sval_dev_flash.sh keep can iterate on a
# board's own settings. Never in a release.
ifeq ($(strip $(SVAL_DEV_KEEP_SETTINGS)), yes)
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    $(error SVAL_DEV_KEEP_SETTINGS cannot be part of a release build)
  endif
  OPT_DEFS += -DSVAL_DEV_KEEP_SETTINGS
endif
