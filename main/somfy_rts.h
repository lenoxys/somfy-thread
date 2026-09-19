// SPDX-License-Identifier: Unlicense
// Somfy RTS transmit over CC1101 (OOK, RMT-driven Manchester). Timing follows
// the standard European RTS protocol (Nickduino/Somfy_Remote, ESPSomfy-RTS).
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "cc1101.h"
#include "driver/rmt_tx.h"
#include "somfy_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Standard RTS physical-layer timings, in microseconds. SOMFY_SYMBOL_US is the
 * Manchester half-bit. SOMFY_HWSYNC_US is one hardware-sync half (four
 * symbols). SOMFY_HWSYNC_FIRST/REPEAT are the hardware-sync cycle counts for
 * the wake-up frame and for each repeat frame. These are the last values to
 * adjust if a motor stays silent; the frame content is fixed by the protocol.
 */
#define SOMFY_SYMBOL_US     640
#define SOMFY_HWSYNC_US     2560
#define SOMFY_SWSYNC_HI_US  4550
#define SOMFY_SWSYNC_LO_US  640
#define SOMFY_WAKE_HI_US    9415
#define SOMFY_WAKE_LO_US    89565
#define SOMFY_INTERFRAME_US 30415
#define SOMFY_HWSYNC_FIRST  2
#define SOMFY_HWSYNC_REPEAT 7

typedef struct {
    cc1101_t             *cc;
    rmt_channel_handle_t  chan;
    rmt_encoder_handle_t  enc;
} somfy_rts_t;

/**
 * Set up the RMT TX channel on the CC1101 GDO0 pin.
 * @return false if the RMT channel or encoder cannot be created.
 */
bool somfy_rts_init(somfy_rts_t *ctx, cc1101_t *cc);

/**
 * Transmit one RTS command burst: a wake-up frame followed by `repeats` frames,
 * all carrying the same rolling code (a single button press). The caller owns
 * rolling-code increment and persistence.
 * @param addr     24-bit remote address.
 * @param rolling  16-bit rolling code.
 * @param freq_mhz Carrier frequency.
 * @param cmd      One of enum somfy_cmd.
 * @param repeats  Repeat-frame count; values below 1 are clamped to 2.
 */
void somfy_rts_send(somfy_rts_t *ctx, uint32_t addr, uint16_t rolling,
                    float freq_mhz, uint8_t cmd, int repeats);

#ifdef __cplusplus
}
#endif
