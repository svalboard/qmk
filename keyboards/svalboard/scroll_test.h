// Scroll test motion (SVAL_SCROLL_TEST, dev builds only; release builds refuse
// it). Raw HID 0xFC op 0x44 runs a segment of synthetic sensor motion into one
// hand's report, upstream of Scroll Mode, Scroll speed and report generation,
// with Scroll Mode forced on for that hand while it runs, so wheel behavior
// can be measured with no hands (tools/scroll_test.py).
//
// Request: data[7] action (0 stop, 1 run), data[8] hand (0 left, 1 right),
// data[9..12] int32 y counts per second (negative is up: scroll up),
// data[13..14] uint16 duration ms. Reply: 0xC4, status, then from data[8]
// int32 counts injected since the last run started.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "report.h"

void sval_scroll_test_command(uint8_t *data, uint8_t length);
// Adds this task's motion to the hand's report (svalboard.c, combined_kb).
void sval_scroll_test_motion(report_mouse_t *left, report_mouse_t *right);
// The hand scrolls whatever its settings say while a segment runs.
bool sval_scroll_test_forces(uint8_t hand);
