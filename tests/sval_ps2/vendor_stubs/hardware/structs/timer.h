// The 1 MHz timer; the test's simulated clock line advances it.
#pragma once
#include <stdint.h>
typedef struct {
    uint32_t timerawl;
} timer_hw_t;
extern timer_hw_t sim_timer;
#define timer_hw (&sim_timer)
