// SPDX-License-Identifier: Unlicense
// Persistent store for the Somfy shades, backed by NVS. Rolling codes are the
// critical state and are persisted on every transmit so a reboot never rewinds
// them (motors reject stale codes).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * One shade: its 24-bit RTS remote address, last-transmitted 16-bit rolling
 * code, display name, and active flag. The carrier frequency is a single
 * device-wide radio setting (all EU Somfy RTS motors share the band), not
 * per-shade — see blind_store_freq().
 */
typedef struct {
    uint32_t addr;
    uint16_t rolling;
    char     name[16];
    bool     active;
} shade_t;

/**
 * Load shades from NVS. On first boot, seed BLIND_MAX_COUNT defaults with
 * MAC-derived unique addresses. Initialises NVS as a side effect.
 */
void     blind_store_init(void);

/**
 * @return The number of shades, always BLIND_MAX_COUNT.
 */
int      blind_store_count(void);

/**
 * @return A pointer to shade `idx`, or NULL if out of range.
 */
shade_t *blind_store_get(int idx);

/**
 * Persist the whole table. Call after editing name, freq, address, or rolling.
 */
void     blind_store_save(void);

/**
 * Increment shade `idx`'s rolling code, persist immediately, and return it.
 * @return The new rolling code, or 0 if `idx` is out of range.
 */
uint16_t blind_store_next_rolling(int idx);

/**
 * @return The device-wide carrier frequency in MHz (default BOARD_DEFAULT_FREQ_MHZ).
 */
float    blind_store_freq(void);

/**
 * Set and persist the device-wide carrier frequency (MHz). Applies to every
 * shade's next transmit.
 */
void     blind_store_set_freq(float mhz);

#ifdef __cplusplus
}
#endif
