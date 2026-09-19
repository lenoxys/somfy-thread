// SPDX-License-Identifier: Unlicense
#include "somfy_frame.h"

/**
 * Assemble the RTS frame. Byte 0 is the fixed key nibble 0xA plus an arbitrary
 * low nibble. Byte 1 carries the command in its high nibble and the checksum in
 * its low nibble, where the checksum is the XOR of every nibble of bytes 0..6
 * so the finished frame's nibble-XOR is zero. Bytes 2..3 hold the rolling code
 * big-endian, bytes 4..6 the 24-bit address big-endian (MSB first), matching
 * ESPSomfy-RTS/Nickduino so motors paired by those stacks respond. Finally each
 * byte is XORed with its predecessor (ascending) to obfuscate the frame.
 */
void somfy_build_frame(uint8_t f[SOMFY_FRAME_BYTES],
                       uint32_t addr, uint16_t code, uint8_t cmd)
{
    f[0] = 0xA7;
    f[1] = (uint8_t)((cmd & 0x0F) << 4);
    f[2] = (uint8_t)(code >> 8);
    f[3] = (uint8_t)(code & 0xFF);
    f[4] = (uint8_t)((addr >> 16) & 0xFF);
    f[5] = (uint8_t)((addr >> 8) & 0xFF);
    f[6] = (uint8_t)(addr & 0xFF);

    uint8_t cksum = 0;
    for (int i = 0; i < SOMFY_FRAME_BYTES; i++)
        cksum ^= f[i] ^ (f[i] >> 4);
    f[1] |= (cksum & 0x0F);

    for (int i = 1; i < SOMFY_FRAME_BYTES; i++)
        f[i] ^= f[i - 1];
}

bool somfy_decode_frame(const uint8_t f[SOMFY_FRAME_BYTES],
                        uint32_t *addr, uint16_t *code, uint8_t *cmd)
{
    uint8_t d[SOMFY_FRAME_BYTES];
    d[0] = f[0];
    for (int i = 1; i < SOMFY_FRAME_BYTES; i++)
        d[i] = f[i] ^ f[i - 1];

    uint8_t x = 0;
    for (int i = 0; i < SOMFY_FRAME_BYTES; i++)
        x ^= d[i] ^ (d[i] >> 4);
    if (x & 0x0F)
        return false;

    *cmd  = d[1] >> 4;
    *code = (uint16_t)((d[2] << 8) | d[3]);
    *addr = ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 8) | d[6];
    return true;
}

/*
 * RX pulse windows (microseconds), from ESPSomfy-RTS (Somfy.cpp:4219-4239):
 * SYMBOL=640 half-bit, ±30% tolerance. A pulse below the glitch floor is
 * ignored; a hardware-sync pulse is ~4*SYMBOL; the software sync ~4850us; a
 * full symbol ~2*SYMBOL toggles the bit, two half symbols emit the current bit.
 */
#define RX_GLITCH_US    448
#define RX_HWSYNC_MIN   1792
#define RX_HWSYNC_MAX   3328
#define RX_SWSYNC_MIN   3395
#define RX_SWSYNC_MAX   6305
#define RX_HALF_MIN     448
#define RX_HALF_MAX     832
#define RX_FULL_MIN     896
#define RX_FULL_MAX     1664
#define RX_HWSYNC_COUNT 4

bool somfy_decode_pulses(const uint16_t *durations, int n,
                         uint32_t *addr, uint16_t *code, uint8_t *cmd)
{
    const int nbits = SOMFY_FRAME_BYTES * 8;
    int cpt_hw = 0, cpt_bits = 0, prev_bit = 0;
    bool receiving = false, waiting_half = false;
    uint8_t payload[SOMFY_FRAME_BYTES];

    for (int i = 0; i < n; i++) {
        uint16_t dur = durations[i];
        if (dur < RX_GLITCH_US)
            continue;

        if (!receiving) {
            if (dur > RX_HWSYNC_MIN && dur < RX_HWSYNC_MAX) {
                cpt_hw++;
            } else if (dur > RX_SWSYNC_MIN && dur < RX_SWSYNC_MAX && cpt_hw >= RX_HWSYNC_COUNT) {
                receiving = true;
                waiting_half = false;
                cpt_bits = 0;
                prev_bit = 0;
                for (int b = 0; b < SOMFY_FRAME_BYTES; b++) payload[b] = 0;
            } else {
                cpt_hw = 0;
            }
            continue;
        }

        if (dur > RX_FULL_MIN && dur < RX_FULL_MAX && !waiting_half) {
            prev_bit = 1 - prev_bit;
            payload[cpt_bits / 8] |= prev_bit << (7 - cpt_bits % 8);
            cpt_bits++;
        } else if (dur > RX_HALF_MIN && dur < RX_HALF_MAX) {
            if (waiting_half) {
                waiting_half = false;
                payload[cpt_bits / 8] |= prev_bit << (7 - cpt_bits % 8);
                cpt_bits++;
            } else {
                waiting_half = true;
            }
        } else {
            receiving = false;
            cpt_hw = 0;
            continue;
        }

        if (cpt_bits >= nbits)
            return somfy_decode_frame(payload, addr, code, cmd);
    }
    return false;
}
