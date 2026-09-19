// SPDX-License-Identifier: Unlicense
// Host self-check for the Somfy RTS frame build + decode. Build & run:
//   cc -I../main test_somfy_frame.c ../main/somfy_frame.c -o /tmp/t && /tmp/t
// Asserts, against the working ESPSomfy-RTS/Nickduino reference (Somfy.cpp
// 336-342, 128-169): the on-air byte layout (rolling BE [2..3], 24-bit address
// BE / MSB-first [4..6], command in [1] high nibble, zero nibble-XOR checksum),
// that build->decode round-trips the fields, that a corrupted frame is
// rejected, and that the RX pulse state machine recovers a synthesized burst.
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "somfy_frame.h"

/**
 * De-obfuscate a built frame, assert its on-air layout, and assert
 * somfy_decode_frame round-trips the fields.
 */
static void check_valid(uint32_t addr, uint16_t code, uint8_t cmd)
{
    uint8_t f[SOMFY_FRAME_BYTES];
    somfy_build_frame(f, addr, code, cmd);

    uint8_t clear[SOMFY_FRAME_BYTES];
    clear[0] = f[0];
    for (int i = 1; i < SOMFY_FRAME_BYTES; i++)
        clear[i] = f[i] ^ f[i - 1];

    assert(((clear[1] >> 4) & 0x0F) == (cmd & 0x0F));
    assert(clear[2] == (uint8_t)(code >> 8));
    assert(clear[3] == (uint8_t)(code & 0xFF));
    assert(clear[4] == (uint8_t)(addr >> 16));  // MSB first — big-endian, not LE
    assert(clear[5] == (uint8_t)((addr >> 8) & 0xFF));
    assert(clear[6] == (uint8_t)(addr & 0xFF));

    uint8_t x = 0;
    for (int i = 0; i < SOMFY_FRAME_BYTES; i++)
        x ^= clear[i] ^ (clear[i] >> 4);
    assert((x & 0x0F) == 0);

    uint32_t a; uint16_t c; uint8_t m;
    assert(somfy_decode_frame(f, &a, &c, &m));
    assert(a == addr && c == code && m == (cmd & 0x0F));
}

/**
 * Synthesize the pulse stream the decoder expects for `f` (four hardware-sync
 * pulses, one software-sync pulse, then Manchester: a differing bit is one full
 * symbol, an equal bit two half symbols — decoder prev_bit starts at 0).
 * @return the number of durations written.
 */
static int synth_pulses(const uint8_t *f, uint16_t *out)
{
    int n = 0, prev = 0;
    for (int i = 0; i < 4; i++) out[n++] = 2560;
    out[n++] = 4550;
    for (int byte = 0; byte < SOMFY_FRAME_BYTES; byte++)
        for (int bit = 7; bit >= 0; bit--) {
            int v = (f[byte] >> bit) & 1;
            if (v != prev) { out[n++] = 1280; prev = v; }
            else           { out[n++] = 640; out[n++] = 640; }
        }
    return n;
}

int main(void)
{
    check_valid(0x0EB995, 1, SOMFY_UP);
    check_valid(0x0EB995, 0xFFFF, SOMFY_DOWN);
    check_valid(0x121300, 42, SOMFY_MY);
    check_valid(0x000000, 0, SOMFY_PROG);
    check_valid(0xFFFFFF, 0x1234, SOMFY_FLAG);

    // Corrupt the last byte (maps to a single decoded byte) so the checksum shifts.
    uint8_t f[SOMFY_FRAME_BYTES];
    somfy_build_frame(f, 0x0EB995, 0x0393, SOMFY_UP);
    uint8_t bad[SOMFY_FRAME_BYTES];
    memcpy(bad, f, SOMFY_FRAME_BYTES);
    bad[6] ^= 0x10;
    uint32_t a; uint16_t c; uint8_t m;
    assert(!somfy_decode_frame(bad, &a, &c, &m));

    // Full RX path: synthesized pulse burst -> decode.
    uint16_t pulses[512];
    int pn = synth_pulses(f, pulses);
    assert(somfy_decode_pulses(pulses, pn, &a, &c, &m));
    assert(a == 0x0EB995 && c == 0x0393 && m == SOMFY_UP);

    printf("test_somfy_frame: all checks passed (%d pulses)\n", pn);
    return 0;
}
