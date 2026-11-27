// Host-test stand-in for QMK's util.h (pmw33xx_burst_test.c).
#pragma once
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define STR(x) #x
