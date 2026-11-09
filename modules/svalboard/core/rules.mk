# Sval - VIA3-based dynamic features module
# Provides: tap dance, combo, key override, leader, keyboard definition

OPT_DEFS += -DSVAL_ENABLE

# Set USB serial number for GUI detection (unless keyboard overrides it)
SERIAL_NUMBER ?= sval:12345-00
OPT_DEFS += -DSERIAL_NUMBER=\"$(SERIAL_NUMBER)\"

# Use different raw HID usage page/id than Vial (0xFF60/0x61) to avoid enumeration conflicts
OPT_DEFS += -DRAW_USAGE_PAGE=0xFF61
OPT_DEFS += -DRAW_USAGE_ID=0x62

# Module path for source files
SVAL_MODULE_PATH := $(dir $(lastword $(MAKEFILE_LIST)))

# Search for sval.json in keymap directories (same search order as QMK)
SVAL_JSON_PATH := $(firstword \
    $(wildcard $(MAIN_KEYMAP_PATH_1)/sval.json) \
    $(wildcard $(MAIN_KEYMAP_PATH_2)/sval.json) \
    $(wildcard $(MAIN_KEYMAP_PATH_3)/sval.json) \
    $(wildcard $(MAIN_KEYMAP_PATH_4)/sval.json) \
    $(wildcard $(MAIN_KEYMAP_PATH_5)/sval.json))

ifeq ($(SVAL_JSON_PATH),)
    $(error Sval module requires sval.json in your keymap directory)
endif

# Generated file paths
SVAL_CONFIG_HEADER := $(INTERMEDIATE_OUTPUT)/src/sval_config.h
SVAL_CONFIG_MK := $(INTERMEDIATE_OUTPUT)/src/sval_config.mk
SVAL_DEFINITION_HEADER := $(INTERMEDIATE_OUTPUT)/src/sval_definition_data.h

# Generate config header and make fragment from sval.json
# This determines which features are enabled based on the 'sval' object in JSON
$(shell mkdir -p "$(INTERMEDIATE_OUTPUT)/src" && \
    python3 "$(SVAL_MODULE_PATH)sval_config.py" \
        "$(SVAL_JSON_PATH)" \
        "$(SVAL_CONFIG_HEADER)" \
        "$(SVAL_CONFIG_MK)" >/dev/null 2>&1)

# Include generated make fragment (sets TAP_DANCE_ENABLE, COMBO_ENABLE, etc.)
-include $(SVAL_CONFIG_MK)

# Explicitly add OPT_DEFS for features (in case module is processed after generic_features.mk)
ifeq ($(LEADER_ENABLE),yes)
    OPT_DEFS += -DLEADER_ENABLE
endif

# Add include path for generated headers
VPATH += $(INTERMEDIATE_OUTPUT)/src

# Generate compressed keyboard definition header
$(shell python3 "$(SVAL_MODULE_PATH)sval_compress.py" \
    "$(SVAL_JSON_PATH)" \
    "$(SVAL_DEFINITION_HEADER)" >/dev/null 2>&1)

# Core source files (always included)
SRC += $(SVAL_MODULE_PATH)sval.c
SRC += $(SVAL_MODULE_PATH)sval_context_layer.c
SRC += $(SVAL_MODULE_PATH)sval_definition.c
SRC += $(SVAL_MODULE_PATH)sval_qmk_settings.c
SRC += $(SVAL_MODULE_PATH)sval_auto_shift.c
SRC += $(SVAL_MODULE_PATH)sval_fragments.c
SRC += $(SVAL_MODULE_PATH)client_wrapper.c

# Conditionally include feature source files based on sval.json config
ifeq ($(TAP_DANCE_ENABLE),yes)
    SRC += $(SVAL_MODULE_PATH)sval_tap_dance.c
endif

ifeq ($(COMBO_ENABLE),yes)
    SRC += $(SVAL_MODULE_PATH)sval_combo.c
endif

ifeq ($(KEY_OVERRIDE_ENABLE),yes)
    SRC += $(SVAL_MODULE_PATH)sval_key_override.c
endif

ifeq ($(LEADER_ENABLE),yes)
    SRC += $(SVAL_MODULE_PATH)sval_leader.c
endif

# Alt repeat key doesn't have a QMK enable flag, check for entries directly
ifneq ($(wildcard $(SVAL_CONFIG_HEADER)),)
    ifneq ($(shell grep -c SVAL_ALT_REPEAT_KEY_ENABLE $(SVAL_CONFIG_HEADER)),0)
        SRC += $(SVAL_MODULE_PATH)sval_alt_repeat_key.c
    endif
endif
