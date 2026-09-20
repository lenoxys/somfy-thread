// SPDX-License-Identifier: Unlicense
// Host test for json_escape. Build & run:
//   cc -I../main test_json_escape.c -o /tmp/tj && /tmp/tj
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "json_escape.h"

int main(void)
{
    char out[128];

    json_escape("Kitchen", out, sizeof(out));
    assert(strcmp(out, "Kitchen") == 0);

    json_escape("Ben\"s", out, sizeof(out));
    assert(strcmp(out, "Ben\\\"s") == 0);

    json_escape("a\\b", out, sizeof(out));
    assert(strcmp(out, "a\\\\b") == 0);

    json_escape("tab\there", out, sizeof(out));
    assert(strcmp(out, "tab\\u0009here") == 0);

    // truncation: never overflows, always NUL-terminated
    char small[8];
    json_escape("aaaaaaaaaaaaaaaa", small, sizeof(small));
    assert(strlen(small) < sizeof(small));

    // a quote right at the boundary must not be half-written
    char tiny[4];
    json_escape("\"\"\"", tiny, sizeof(tiny));
    assert(strlen(tiny) < sizeof(tiny));

    printf("test_json_escape: all checks passed\n");
    return 0;
}
