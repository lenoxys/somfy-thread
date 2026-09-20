// SPDX-License-Identifier: Unlicense
// Somfy RTS receiver: captures the demodulated OOK pulse train on the CC1101
// GDO2 pin with a GPIO any-edge interrupt and decodes RTS frames, so manually-
// operated remotes are seen (Somfy is otherwise one-way). Decode logic lives in
// somfy_frame.c; this file is the edge-timing ISR and the listening task.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cc1101.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Called from the receive task for every valid decoded frame.
 * @param addr 24-bit remote address, code 16-bit rolling code, cmd command nibble.
 */
typedef void (*somfy_rx_cb_t)(uint32_t addr, uint16_t code, uint8_t cmd);

/**
 * Attach the any-edge interrupt on CC1101_PIN_GD2, start the listening task, and
 * put the radio in RX. `cb` fires for each valid frame.
 * @return false if the GPIO interrupt, queue, or task cannot be created.
 */
bool somfy_rx_init(cc1101_t *cc, somfy_rx_cb_t cb);

/**
 * Re-enter RX after a transmit (the TX path leaves the radio idle). No-op if the
 * receiver was never initialised.
 */
void somfy_rx_resume(void);

#ifdef __cplusplus
}
#endif
