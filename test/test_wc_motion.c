// SPDX-License-Identifier: Unlicense
// Host self-check for the timed position-estimate math (wc_motion.h). Build & run:
//   cc -I../main test_wc_motion.c -o /tmp/tm && /tmp/tm
// Asserts: full/partial travel durations scale with distance, the ramp clamps to
// its endpoints and interpolates linearly in between, direction (open vs close)
// is symmetric, and a zero travel time collapses to the target (snap fallback).
#include <assert.h>
#include <stdio.h>
#include "wc_motion.h"

int main(void)
{
    // Duration scales with the fraction of range covered.
    assert(wc_motion_dur_us(0, 10000, 5000) == 5000000);   // full close, 5 s
    assert(wc_motion_dur_us(0, 5000, 5000)  == 2500000);   // half range
    assert(wc_motion_dur_us(10000, 0, 4000) == 4000000);   // full open
    assert(wc_motion_dur_us(7000, 3000, 5000) == 2000000); // 40% of range
    assert(wc_motion_dur_us(4200, 4200, 5000) == 0);       // nowhere to go

    // Ramp clamps outside [0, dur] and interpolates linearly within.
    assert(wc_motion_lerp(0, 10000, -5, 1000) == 0);       // before start
    assert(wc_motion_lerp(0, 10000, 2000, 1000) == 10000); // past the end
    assert(wc_motion_lerp(0, 10000, 500, 1000) == 5000);   // midpoint, closing
    assert(wc_motion_lerp(10000, 0, 500, 1000) == 5000);   // midpoint, opening
    assert(wc_motion_lerp(2000, 6000, 1000, 4000) == 3000);// quarter of the way

    // Zero (unknown) travel time collapses to the target — the snap fallback.
    assert(wc_motion_lerp(3000, 8000, 0, 0) == 8000);

    printf("test_wc_motion: all checks passed\n");
    return 0;
}
