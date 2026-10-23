# One image for both halves (svalboard/either, svalboard/trackball/pmw3389/either).
# The half learns its side at boot: thumb strap, then the stored side record,
# else unknown (side.h). Included by those targets' rules.mk only, so no other
# build changes.
OPT_DEFS += -DSVAL_EITHER_HAND
SRC += side.c
