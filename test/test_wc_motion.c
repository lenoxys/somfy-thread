// SPDX-License-Identifier: Unlicense
// Host self-check for the timed position-estimate math (wc_motion.h). Build & run:
//   cc -I../main test_wc_motion.c -o /tmp/tm && /tmp/tm
// Asserts: full/partial travel durations scale with distance, a startup lag adds
// dead-time and holds position through it, the ramp clamps to its endpoints and
// interpolates linearly in between, direction (open vs close) is symmetric, and a
// zero travel time collapses to the target (snap fallback).
#include <assert.h>
#include <stdio.h>
#include "wc_motion.h"

int main(void)
{
    // Duration scales with the fraction of range covered (no lag).
    assert(wc_motion_dur_us(0, 10000, 5000, 0) == 5000000);   // full close, 5 s
    assert(wc_motion_dur_us(0, 5000, 5000, 0)  == 2500000);   // half range
    assert(wc_motion_dur_us(10000, 0, 4000, 0) == 4000000);   // full open
    assert(wc_motion_dur_us(7000, 3000, 5000, 0) == 2000000); // 40% of range
    assert(wc_motion_dur_us(4200, 4200, 5000, 0) == 0);       // nowhere to go
    assert(wc_motion_dur_us(0, 10000, 5000, 0) == 5000000 &&  // travel unknown -> snap
           wc_motion_dur_us(0, 10000, 0, 800) == 0);

    // A startup lag adds fixed dead-time on top of the moving window.
    assert(wc_motion_dur_us(0, 5000, 5000, 800) == 2500000 + 800000); // half range + 0.8 s lag

    // Ramp clamps outside [0, dur] and interpolates linearly within (no lag).
    assert(wc_motion_lerp(0, 10000, -5, 1000, 0) == 0);       // before start
    assert(wc_motion_lerp(0, 10000, 2000, 1000, 0) == 10000); // past the end
    assert(wc_motion_lerp(0, 10000, 500, 1000, 0) == 5000);   // midpoint, closing
    assert(wc_motion_lerp(10000, 0, 500, 1000, 0) == 5000);   // midpoint, opening
    assert(wc_motion_lerp(2000, 6000, 1000, 4000, 0) == 3000);// quarter of the way

    // Position holds at `from` through the lag, then ramps over the moving window.
    assert(wc_motion_lerp(0, 10000, 100, 1200, 200) == 0);    // still in the lag
    assert(wc_motion_lerp(0, 10000, 200, 1200, 200) == 0);    // lag just ended
    assert(wc_motion_lerp(0, 10000, 700, 1200, 200) == 5000); // half the moving window

    // Zero (unknown) travel time collapses to the target — the snap fallback.
    assert(wc_motion_lerp(3000, 8000, 0, 0, 0) == 8000);

    printf("test_wc_motion: all checks passed\n");
    return 0;
}
