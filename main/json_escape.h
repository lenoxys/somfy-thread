// SPDX-License-Identifier: Unlicense
#pragma once
#include <stddef.h>
#include <stdio.h>

/**
 * Escape `in` into `out` so it is safe inside a JSON string literal: backslash,
 * double-quote, and control chars (< 0x20) are escaped; everything else copies
 * through. Stops before overflow; `out` should hold up to 6 bytes per input char
 * plus a NUL. Always NUL-terminates.
 */
static inline void json_escape(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 7 < outsz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, outsz - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}
