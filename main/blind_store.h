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
 * share the band), not per-shade — see blind_store_freq(). `up_ms`/`down_ms` are
 * the measured full-open and full-close travel times (0 = unknown → the position
 * estimate snaps to target instead of ramping). `my_pct` is the motor's favourite
 * ("my") position as a 0..100 percent-closed value, or SHADE_MY_UNSET when
 * unknown; it is set physically by holding the remote's My button and cannot be
 * read over RF, so this is only our copy of it. `invert` flips open/close for
 * reversed installs. `up_lag_ms`/`down_lag_ms` are the per-direction startup
 * dead-times (how long after the command the shade stays still before it visibly
 * moves — pronounced lifting against gravity, near-zero dropping); they add a
 * fixed offset before the timed ramp so partial moves and mid-travel stops track.
 * `pos` is the last settled position estimate (Percent100ths, 0 = open) persisted
 * so it survives a reboot — stale if a wall remote moved the shade while powered
 * off, re-zeroed by the next full open/close.
 */
typedef struct {
    uint32_t addr;
    uint16_t rolling;
    uint16_t ep_id;
    char     name[16];
    bool     enabled;
    bool     remote;
    uint16_t up_ms;
    uint16_t down_ms;
    uint8_t  my_pct;
    bool     invert;
    uint16_t up_lag_ms;
    uint16_t down_lag_ms;
    uint16_t pos;
} shade_t;

#define SHADE_MY_UNSET 0xFF

/**
 * Initialise NVS and load the slot table. Starts empty (0 shades) on first boot
 * or whenever the persisted blob does not match the current layout.
 */
void     blind_store_init(void);

/**
 * @return true if slot `idx` holds a shade (addr != 0).
 */
bool     blind_store_used(int idx);

/**
 * Add a shade in the first free slot. Does not persist; the caller commits with
 * blind_store_save() once the endpoint is live, or drops it with
 * blind_store_remove() on failure.
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
 * Persist the shade table, links, and device-wide settings. Call after editing
 * name, address, rolling, or any other shade field.
 */
void     blind_store_save(void);

/**
 * Erase the whole Somfy NVS namespace (shades, links, radio settings) for a full
 * factory reset. Matter/Thread state lives in separate CHIP namespaces and is
 * cleared by esp_matter::factory_reset(), not here.
 */
void     blind_store_factory_erase(void);

/**
 * Increment shade `idx`'s rolling code, persist immediately, and return it.
 * @return The new rolling code, or 0 if `idx` is out of range.
 */
uint16_t blind_store_next_rolling(int idx);

/**
 * @return Slot `idx`'s monitored linked-remote address (0 = none). This is a
 *         physical Somfy remote the firmware only listens for (never transmits
 *         as): hearing it mirrors the shade's position when someone uses the
 *         wall remote, without contending on the shade's own rolling code. It
 *         lives in an NVS blob separate from the shade table, independent of the
 *         shade layout and its Matter endpoint ids.
 */
uint32_t blind_store_link_addr(int idx);

/**
 * Set (addr != 0) or clear (addr == 0) slot `idx`'s monitored linked remote and
 * persist.
 */
void     blind_store_set_link(int idx, uint32_t addr);

/**
 * @return The device-wide carrier frequency in MHz (default BOARD_DEFAULT_FREQ_MHZ).
 */
float    blind_store_freq(void);

/**
 * Set and persist the device-wide carrier frequency (MHz). Applies to every
 * shade's next transmit.
 */
void     blind_store_set_freq(float mhz);

/**
 * @return The device-wide TX-power index (into cc1101_tx_power_dbm[]; default is
 *         the top +10 dBm level).
 */
uint8_t  blind_store_tx_power(void);

/** Set and persist the device-wide TX-power index. */
void     blind_store_set_tx_power(uint8_t idx);

/**
 * @return The device-wide RX-bandwidth index (into cc1101_rxbw_khz[]; default is
 *         the ~203 kHz preset matching the init register set).
 */
uint8_t  blind_store_rxbw(void);

/** Set and persist the device-wide RX-bandwidth index. */
void     blind_store_set_rxbw(uint8_t idx);

/**
 * @return The Matter endpoint id of the Aggregator that makes this node a bridge
 *         (0 = not created yet). Persisted in its own NVS key, independent of the
 *         shade blob, so it is resumed to the same id every boot — Home Assistant
 *         only treats the node as a bridge (and shows per-cover names) when the
 *         Aggregator sits on endpoint 1.
 */
uint16_t blind_store_agg_ep(void);

/** Persist the Aggregator's Matter endpoint id. */
void     blind_store_set_agg_ep(uint16_t ep_id);

#ifdef __cplusplus
}
#endif
