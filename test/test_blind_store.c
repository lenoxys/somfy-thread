// SPDX-License-Identifier: Unlicense
// Host self-check for the blind_store slot table (the dynamic-shade bookkeeping
// the runtime endpoint lifecycle relies on). Build & run:
//   cc -I../main -Istubs test_blind_store.c ../main/blind_store.c -o /tmp/tb && /tmp/tb
// Asserts: starts empty, add fills the first free slot, remove clears a slot
// without shifting the others, a freed slot is reused by the next add (so an
// index stays bound to its shade for life), the table caps at BLIND_MAX_COUNT,
// and gen_addr returns a non-zero address that collides with no used slot.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "blind_store.h"

int main(void)
{
    blind_store_init();
    for (int i = 0; i < BLIND_MAX_COUNT; i++) assert(!blind_store_used(i));

    int a = blind_store_add(0x0000AA, 5, "kitchen");
    int b = blind_store_add(0x0000BB, 9, "living");
    assert(a == 0 && b == 1);
    assert(blind_store_used(0) && blind_store_used(1));

    shade_t *s = blind_store_get(0);
    assert(s->addr == 0x0000AA && s->rolling == 5 && s->enabled && s->ep_id == 0);
    assert(!s->remote);  // provenance defaults to PROG until cmd_add flags a clone
    // Position-estimate params default to unknown: no travel time (snap), no
    // favourite, not inverted, no startup lag, position 0 (open).
    assert(s->up_ms == 0 && s->down_ms == 0 && s->my_pct == SHADE_MY_UNSET && !s->invert
           && s->up_lag_ms == 0 && s->down_lag_ms == 0 && s->pos == 0);
    assert(strcmp(s->name, "kitchen") == 0);

    // Linked remote: none by default; set/seen advances the code only when newer;
    // removing the shade clears its link.
    assert(blind_store_link_addr(0) == 0);
    blind_store_set_link(0, 0x6702167, 2020);
    assert(blind_store_link_addr(0) == (0x6702167 & 0xFFFFFF));
    blind_store_link_seen(0, 1000);  // older — ignored
    blind_store_link_seen(0, 3000);  // newer — kept (no getter for roll; just must not crash)

    // Remove slot 0; slot 1 must stay put (indices never shift). The link clears.
    blind_store_remove(0);
    assert(!blind_store_used(0));
    assert(blind_store_link_addr(0) == 0);
    assert(blind_store_used(1) && blind_store_get(1)->addr == 0x0000BB);

    // The next add reuses the freed first slot rather than growing.
    int c = blind_store_add(0x0000CC, 1, "office");
    assert(c == 0 && blind_store_get(0)->addr == 0x0000CC);

    // Fill to capacity, then adds fail.
    while (blind_store_add(0x0000DD, 1, "x") >= 0) { /* keep filling */ }
    for (int i = 0; i < BLIND_MAX_COUNT; i++) assert(blind_store_used(i));
    assert(blind_store_add(0x0000EE, 1, "overflow") == -1);

    // gen_addr avoids every used address and never returns 0.
    uint32_t g = blind_store_gen_addr();
    assert(g != 0);
    for (int i = 0; i < BLIND_MAX_COUNT; i++)
        assert(blind_store_get(i)->addr != g);

    printf("test_blind_store: all checks passed\n");
    return 0;
}
