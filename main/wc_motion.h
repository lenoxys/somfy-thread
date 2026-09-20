// SPDX-License-Identifier: Unlicense
// Pure position-estimate math for the timed WindowCovering model. Somfy RTS is
// one-way (no position feedback), so we estimate where a shade is from how long
// it has been travelling. Kept header-only and dependency-free so it can be
// exercised by a host self-check (test/test_wc_motion.c) without the Matter or
// ESP stack. Positions are Percent100ths: 0 = fully open, 10000 = fully closed.
#pragma once
#include <stdint.h>
#include <stdlib.h>

/**
 * Full-travel time (ms) to move a shade from `from` to `to` given the direction's
 * full-range duration `travel_ms` (time for a complete open or close). Returns
 * microseconds, proportional to the fraction of range covered. 0 when nothing to
 * do or the direction has no configured travel time.
 */
static inline int64_t wc_motion_dur_us(uint16_t from, uint16_t to, uint16_t travel_ms)
{
    int32_t dist = abs((int)to - (int)from);
    return (int64_t)dist * travel_ms * 1000 / 10000;
}

/**
 * Position along a linear ramp from `from` to `to` after `el_us` of a total
 * `dur_us` move. Clamps to the endpoints outside [0, dur_us].
 */
static inline uint16_t wc_motion_lerp(uint16_t from, uint16_t to, int64_t el_us, int64_t dur_us)
{
    if (dur_us <= 0 || el_us >= dur_us) return to;
    if (el_us <= 0) return from;
    int32_t span = (int32_t)to - (int32_t)from;
    return (uint16_t)((int32_t)from + (int64_t)span * el_us / dur_us);
}
