// SPDX-License-Identifier: Unlicense
// Somfy RTS frame construction. Pure logic, no ESP dependencies (host-testable).
// Reference: Nickduino/Somfy_Remote, ESPSomfy-RTS.
#pragma once
#include <stdint.h>

/**
 * Somfy RTS command codes. The low 4 bits are the wire control field.
 */
enum somfy_cmd {
    SOMFY_MY      = 0x1,
    SOMFY_UP      = 0x2,
    SOMFY_MY_UP   = 0x3,
    SOMFY_DOWN    = 0x4,
    SOMFY_MY_DOWN = 0x5,
    SOMFY_UP_DOWN = 0x6,
    SOMFY_PROG    = 0x8,
    SOMFY_SUN     = 0x9,
    SOMFY_FLAG    = 0xA,
};

/**
 * Standard European RTS frame length: 56 bits, 7 bytes.
 */
#define SOMFY_FRAME_BYTES 7

/**
 * Build a 7-byte RTS frame: checksummed and obfuscated per the Somfy RTS spec.
 * @param frame Output buffer of SOMFY_FRAME_BYTES.
 * @param addr  24-bit remote address.
 * @param code  16-bit rolling code.
 * @param cmd   One of enum somfy_cmd (low 4 bits used).
 */
void somfy_build_frame(uint8_t frame[SOMFY_FRAME_BYTES],
                       uint32_t addr, uint16_t code, uint8_t cmd);
