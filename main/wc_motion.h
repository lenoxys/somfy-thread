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
 * Total time (us) to move a shade from `from` to `to`: a fixed startup dead-time
 * `lag_ms` (the motor takes up load before the shade visibly moves — pronounced
 * lifting against gravity, near-zero dropping) plus the moving time, which scales
 * `travel_ms` (full-range duration) by the fraction of range covered. Returns 0
 * when there is nothing to do or the direction has no configured travel time (so
 * the caller snaps instead of ramping).
 */
static inline int64_t wc_motion_dur_us(uint16_t from, uint16_t to, uint16_t travel_ms, uint16_t lag_ms)
{
    int32_t dist = abs((int)to - (int)from);
    if (dist == 0 || travel_ms == 0) return 0;
    return (int64_t)lag_ms * 1000 + (int64_t)dist * travel_ms * 1000 / 10000;
}

/**
 * Position after `el_us` of a total `dur_us` move: held at `from` through the
 * `lag_us` startup dead-time, then a linear ramp toward `to` over the remaining
 * moving window. Clamps to the endpoints outside [0, dur_us].
 */
static inline uint16_t wc_motion_lerp(uint16_t from, uint16_t to, int64_t el_us, int64_t dur_us, int64_t lag_us)
{
    if (dur_us <= 0 || el_us >= dur_us) return to;
    if (el_us <= lag_us) return from;
    int64_t move_us = dur_us - lag_us;
    int32_t span = (int32_t)to - (int32_t)from;
    return (uint16_t)((int32_t)from + (int64_t)span * (el_us - lag_us) / move_us);
}
