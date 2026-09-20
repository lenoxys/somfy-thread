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
 * One shade in a fixed-capacity slot table. `addr == 0` marks an empty slot;
 * slots are never compacted, so a slot's index stays stable for the life of the
 * shade. `ep_id` is the Matter endpoint id assigned when the shade is first
 * added (0 = none yet) and is persisted so the same identity is resumed across
 * reboots. `enabled` is the "On" switch: whether the shade is exposed over
 * Thread. `remote` records provenance: true when the shade was cloned from a
 * physical Somfy remote (discovery), false when it was added via PROG with no
 * remote — the motor then obeys us only after PROG pairing. The carrier
 * frequency is a single device-wide radio setting (all EU Somfy RTS motors
 * share the band), not per-shade — see blind_store_freq().
 */
typedef struct {
    uint32_t addr;
    uint16_t rolling;
    uint16_t ep_id;
    char     name[16];
    bool     enabled;
    bool     remote;
} shade_t;

/**
 * Initialise NVS and load the slot table. Starts empty (0 shades) on first boot
 * or whenever the persisted blob does not match the current layout.
 */
void     blind_store_init(void);

/**
 * @return The slot-table capacity (BLIND_MAX_COUNT), not the number in use.
 */
int      blind_store_count(void);

/**
 * @return true if slot `idx` holds a shade (addr != 0).
 */
bool     blind_store_used(int idx);

/**
 * Add a shade in the first free slot.
 * @return The slot index, or -1 if the table is full.
 */
int      blind_store_add(uint32_t addr, uint16_t rolling, const char *name);

/**
 * Clear slot `idx` (frees it for reuse) and persist.
 */
void     blind_store_remove(int idx);

/**
 * @return A fresh MAC-derived 24-bit address not already used by any slot, for
 *         the PROG "add a motor without a remote" path.
 */
uint32_t blind_store_gen_addr(void);

/**
 * @return A pointer to slot `idx`, or NULL if out of range. The slot may be
 *         empty — check blind_store_used().
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
