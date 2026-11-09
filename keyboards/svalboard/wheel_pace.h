// Wheel pacing: at most one detent per axis in each USB mouse report.
//
// Windows Terminal turns each wheel message into one wheel event for the
// program it hosts, however large its delta (TerminalInput::HandleMouse), so a
// report of 420 counts scrolls a psmux or tmux pane as far as one of 120.
// Every scroll source here (Drag Scroll, Scroll Mode, a touchpad's two-finger
// scroll, Velocity Scroll) can put several detents in one report, and more
// with a higher Scroll speed. This caps each report at one detent per axis and
// carries the rest into the following reports, so the total is unchanged and
// only spread over a few milliseconds (one report per 1 ms USB poll).
//
// Windows Terminal also drops what a report carries past the 120 that
// completes a detent (it clears its running total on each event it sends).
// So a report never crosses a detent boundary: after a partial 60, the next
// report carries 60, then whole detents. The boundary is counted from the
// last change of direction, as the terminal counts it.
//
// A backlog is bounded to SVAL_WHEEL_PACE_BACKLOG counts (about 100 ms of
// reports) so a fast spin never builds lag; anything over it is dropped.
// Reversing direction drops the backlog of the old direction at once.
// macOS reads each count as a line rather than 1/120 of a detent, so on a Mac
// reports pass through unchanged.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SVAL_WHEEL_PACE_MAX 120
#ifndef SVAL_WHEEL_PACE_BACKLOG
#    define SVAL_WHEEL_PACE_BACKLOG (SVAL_WHEEL_PACE_MAX * 100)
#endif

typedef struct {
    int32_t h, v;         // counts not yet sent
    int16_t phase_h;      // counts sent since the last detent boundary,
    int16_t phase_v;      // signed with the direction
} sval_wheel_pace_t;

// One report's h and v in, the capped h and v out. With pass true (a Mac) the
// report is unchanged and any backlog is cleared.
void sval_wheel_pace(sval_wheel_pace_t *p, int16_t *h, int16_t *v, bool pass);
