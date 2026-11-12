# The module's tap dances (sval_tap_dance.c), with the board's limit on dances
# held at once, read from keyboards/svalboard/config.h.
TAP_DANCE_ENABLE = yes
SRC += modules/svalboard/core/sval_tap_dance.c
TD_MAX_SIMULTANEOUS := $(shell sed -n 's/^\#define TAP_DANCE_MAX_SIMULTANEOUS \([0-9]*\).*/\1/p' keyboards/svalboard/config.h)
ifneq ($(TD_MAX_SIMULTANEOUS),)
    OPT_DEFS += -DTAP_DANCE_MAX_SIMULTANEOUS=$(TD_MAX_SIMULTANEOUS)
endif
