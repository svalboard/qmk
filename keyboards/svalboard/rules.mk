# MCU name
MCU = RP2040
BOOTLOADER = rp2040
BOARD = GENERIC_RP_RP2040

# we want some pretty lights (RGBLIGHT_ENABLE is in info.json)
RGBLIGHT_SPLIT = yes
RGBLIGHT_DRIVER = ws2812
WS2812_DRIVER   = vendor

CUSTOM_MATRIX = lite

SRC += axis_scale.c matrix.c scanlab.c power.c identity.c pointing_detect.c pointer_reprobe.c build_reset.c velocity_scroll.c

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
# SVAL_UPDATE_TEST_HOOKS adds the commit halt points for hardware tests.
# SVAL_UPDATE_NO_CHORD makes a dev build: every ARM counts as confirmed, so
# updates need no chord (util/sval_dev_flash.sh). Never in a release.
# SVAL_UPDATE_RELEASE makes a release updater build: images flagged DIAGNOSTIC
# are refused, and test hooks, keytest and the host bootloader are not allowed.
SVAL_UPDATER ?= no
SVAL_UPDATE_TEST_HOOKS ?= no
SVAL_UPDATE_NO_CHORD ?= no
SVAL_UPDATE_RELEASE ?= no
ifeq ($(strip $(SVAL_UPDATER)), yes)
  ifeq ($(strip $(SVAL_KEYTEST)), yes)
    $(error SVAL_UPDATER and SVAL_KEYTEST cannot be combined: keytest injects key events (R11))
  endif
  OPT_DEFS += -DSVAL_UPDATER -DCLIENT_WRAPPER_ID_GETTER
  # Version (D17, proposed D32). SVAL_FW_VERSION, the number, comes from
  # updater/fw_version.txt unless given (make ... SVAL_FW_VERSION=N; the older
  # EXTRAFLAGS=-DSVAL_FW_VERSION=N still works). SVAL_FW_VERSION_STRING, the
  # display string (release CI: the tag name), is empty unless given.
  SVAL_FW_VERSION ?= $(shell sed -n '/^[0-9][0-9]*$$/p' keyboards/svalboard/updater/fw_version.txt)
  ifneq ($(shell echo '$(SVAL_FW_VERSION)' | grep -Ex '[0-9]{1,9}' >/dev/null && echo ok),ok)
    $(error SVAL_FW_VERSION '$(SVAL_FW_VERSION)' is not a number of at most 9 digits (keyboards/svalboard/updater/fw_version.txt))
  endif
  ifeq ($(findstring -DSVAL_FW_VERSION=,$(EXTRAFLAGS)),)
    OPT_DEFS += -DSVAL_FW_VERSION=$(SVAL_FW_VERSION)u
  endif
  SVAL_FW_VERSION_STRING ?=
  ifneq ($(shell echo '$(SVAL_FW_VERSION_STRING)' | grep -Ex '[A-Za-z0-9._+-]{0,16}' >/dev/null && echo ok),ok)
    $(error SVAL_FW_VERSION_STRING '$(SVAL_FW_VERSION_STRING)' must be at most 16 of A-Z a-z 0-9 . _ + -)
  endif
  OPT_DEFS += -DSVAL_FW_VERSION_STRING=\"$(SVAL_FW_VERSION_STRING)\"
  # The keymap in the build-info record (M3), so make_update.py can check the
  # manifest's keymap against the image: 1 sval, 2 blank (D22), 0 other.
  SVAL_UPDATE_KEYMAP_ID := $(if $(filter sval,$(KEYMAP)),1,$(if $(filter blank,$(KEYMAP)),2,0))
  OPT_DEFS += -DSVAL_UPDATE_KEYMAP_ID=$(SVAL_UPDATE_KEYMAP_ID)
  SRC += updater/updater.c updater/update_gesture.c updater/update_led.c updater/update_commit.c
  SRC += updater/update_flash.c updater/update_image.c updater/update_build_info.c
  # M2: the split pause (D15, a matrix_scan() override) and the split relay.
  SRC += split_pause.c updater/update_split.c updater/update_split_wire.c
  # The boot relay of a pair UF2 (docs/pair-uf2.md).
  SRC += updater/update_pair.c
  SRC += updater/vendor/sha512.c
  EXTRAINCDIRS += keyboards/svalboard/updater/vendor
  # The commit's RAM code must not branch into flash (R2): no jump tables, no
  # loops turned into memcpy/memset calls. -fstack-usage writes
  # update_commit.su for tools/check_ram_funcs.py.
  $(INTERMEDIATE_OUTPUT)/updater/update_commit.o: FILE_SPECIFIC_CFLAGS += -fno-jump-tables -fno-tree-loop-distribute-patterns -fstack-usage
  # main()'s stack: 2 KiB by default (platforms/chibios/platform.mk). Updater
  # builds keep the 0xAE0 the M1-M2b hardware tests ran with (DIAG reports the
  # free bytes). SRAM4 (4 KiB) also holds the 1 KiB exception stack and
  # ChibiOS's 0x120-byte ch0, so 0xAE0 is the most that fits. The link fails if
  # anything else lands in SRAM4.
  USE_PROCESS_STACKSIZE = 0xAE0
  # LED takeover (D20): rgblight's driver becomes the gate in
  # updater/update_led.c, which forwards to the WS2812 driver except while the
  # updater shows its own colors. 'override' because svalboard/right/rules.mk
  # sets RGBLIGHT_DRIVER again after this file. Only the ws2812 driver choice
  # pulls in the WS2812 driver (builddefs/common_features.mk), so ask for it
  # here; config.h supplies the LED count that choice would have set.
  override RGBLIGHT_DRIVER = custom
  WS2812_DRIVER_REQUIRED = yes
  ifeq ($(strip $(SVAL_UPDATE_TEST_HOOKS)), yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
      $(error SVAL_UPDATE_TEST_HOOKS cannot be part of a release build)
    endif
    OPT_DEFS += -DSVAL_UPDATE_TEST_HOOKS
  endif
  ifeq ($(strip $(SVAL_UPDATE_NO_CHORD)), yes)
    ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
      $(error SVAL_UPDATE_NO_CHORD cannot be part of a release build)
    endif
    OPT_DEFS += -DSVAL_UPDATE_NO_CHORD
  endif
  ifeq ($(strip $(SVAL_UPDATE_RELEASE)), yes)
    OPT_DEFS += -DSVAL_UPDATE_RELEASE
  endif
else
  ifneq ($(filter yes,$(strip $(SVAL_UPDATE_TEST_HOOKS)) $(strip $(SVAL_UPDATE_RELEASE)) $(strip $(SVAL_UPDATE_NO_CHORD))),)
    $(error SVAL_UPDATE_TEST_HOOKS, SVAL_UPDATE_RELEASE and SVAL_UPDATE_NO_CHORD need SVAL_UPDATER=yes)
  endif
endif

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
