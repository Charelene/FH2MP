#!/usr/bin/env bash
# The car list room guard (src/carlist_room.inc, issue #486): the growth rule of the game's car list (game+0x305a20, src/carlist_room_rule.inc), natively.
# Expected values from the game's code and the dump: a list filled one car at a time moves every time it is full (14 cars in a capacity of 19 in the dump of
# #486 means it had moved 8 times); with the guard's room made at the start it never moves until CRG_ROOM cars.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "carlist_room_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

/* push_back's path: when size == capacity, reserve one more (game+0x308e20 -> game+0x305a20(list, 1)). -> how often the list moved. */
static unsigned fill(unsigned long long cars, unsigned long long room, unsigned long long *cap)
{
    unsigned long long size = 0, c = 0, after;
    unsigned moves = 0;

    if (room) {
        after = crg_reserve(size, c, room);
        moves += after != c;            /* the guard's own move happens before any car is in the list */
        c = after;
    }
    for (; size < cars; size++) {
        if (size == c) {
            c = crg_reserve(size, c, 1);
            moves++;
        }
    }
    *cap = c;
    return moves;
}

int main(void)
{
    unsigned long long cap = 0;

    /* the game's own growth: 1, 2, 3, 4, 6, 9, 13, 19 */
    CHECK(fill(14, 0, &cap) == 8 && cap == 19);         /* the dump of #486: 14 cars, capacity 19 */
    CHECK(crg_reserve(4, 4, 1) == 6 && crg_reserve(13, 13, 1) == 19 && crg_reserve(3, 6, 1) == 6);
    /* with the room made at the start: one move while the list is still empty, none while cars arrive */
    CHECK(fill(14, CRG_ROOM, &cap) == 1 && cap == CRG_ROOM);
    CHECK(fill(CRG_ROOM, CRG_ROOM, &cap) == 1);
    CHECK(fill(CRG_ROOM + 1, CRG_ROOM, &cap) == 2);      /* past the room the game's own growth takes over */
    CHECK(crg_reserve(0, CRG_ROOM, CRG_ROOM) == CRG_ROOM);   /* asking again changes nothing */

    if (!failed) puts("car list room guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
