// SPDX-License-Identifier: Unlicense
#include "somfy_frame.h"

/**
 * Assemble the RTS frame. Byte 0 is the fixed key nibble 0xA plus an arbitrary
 * low nibble. Byte 1 carries the command in its high nibble and the checksum in
 * its low nibble, where the checksum is the XOR of every nibble of bytes 0..6
 * so the finished frame's nibble-XOR is zero. Bytes 2..3 hold the rolling code
 * big-endian, bytes 4..6 the address little-endian. Finally each byte is XORed
 * with its predecessor (ascending) to obfuscate the frame.
 */
void somfy_build_frame(uint8_t f[SOMFY_FRAME_BYTES],
                       uint32_t addr, uint16_t code, uint8_t cmd)
{
    f[0] = 0xA7;
    f[1] = (uint8_t)((cmd & 0x0F) << 4);
    f[2] = (uint8_t)(code >> 8);
    f[3] = (uint8_t)(code & 0xFF);
    f[4] = (uint8_t)(addr & 0xFF);
    f[5] = (uint8_t)((addr >> 8) & 0xFF);
    f[6] = (uint8_t)((addr >> 16) & 0xFF);

    uint8_t cksum = 0;
    for (int i = 0; i < SOMFY_FRAME_BYTES; i++)
        cksum ^= f[i] ^ (f[i] >> 4);
    f[1] |= (cksum & 0x0F);

    for (int i = 1; i < SOMFY_FRAME_BYTES; i++)
        f[i] ^= f[i - 1];
}
