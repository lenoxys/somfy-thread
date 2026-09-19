// SPDX-License-Identifier: Unlicense
// Host self-check for the Somfy RTS frame builder. Build & run:
//   cc -I../main test_somfy_frame.c ../main/somfy_frame.c -o /tmp/t && /tmp/t
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "somfy_frame.h"

/**
 * De-obfuscate a built frame and assert every field round-trips and the Somfy
 * checksum invariant (XOR of all nibbles == 0) holds.
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
    assert(clear[4] == (uint8_t)(addr & 0xFF));
    assert(clear[5] == (uint8_t)((addr >> 8) & 0xFF));
    assert(clear[6] == (uint8_t)((addr >> 16) & 0xFF));

    uint8_t x = 0;
    for (int i = 0; i < SOMFY_FRAME_BYTES; i++)
        x ^= clear[i] ^ (clear[i] >> 4);
    assert((x & 0x0F) == 0);
}

int main(void)
{
    check_valid(0x0EB995, 1, SOMFY_UP);
    check_valid(0x0EB995, 0xFFFF, SOMFY_DOWN);
    check_valid(0x121300, 42, SOMFY_MY);
    check_valid(0x000000, 0, SOMFY_PROG);
    check_valid(0xFFFFFF, 0x1234, SOMFY_FLAG);
    printf("test_somfy_frame: all checks passed\n");
    return 0;
}
