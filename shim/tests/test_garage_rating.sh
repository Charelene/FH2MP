#!/usr/bin/env bash
# The garage rating guard (src/garage_rating_guard.inc, crash family heap-corruption-game-12e0de0): what a garage car it does not rate is
# left with (src/garage_rating_rule.inc), natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "garage_rating_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

static unsigned u32(const unsigned char *p, unsigned off) { unsigned v; memcpy(&v, p + off, 4); return v; }

int main(void)
{
    static unsigned char cig[0xab8];                     /* the game allocates 0xab8 bytes (game+0x2dac40) */
    unsigned off;
    memset(cig, 0xcd, sizeof cig);                       /* heap memory, not zeroed */
    gr_clear(cig);
    for (off = 0x260; off < 0x2bc; off += 4) {
        if (off == 0x288) {
            CHECK(u32(cig, off) == 0xcdcdcdcdu);         /* +0x2f5120's field stays */
        } else if (u32(cig, off) != 0) {
            printf("FAIL: field +0x%x not cleared\n", off);
            failed++;
        }
    }
    CHECK(u32(cig, 0xaa4) == 0);                         /* no torque samples */
    CHECK(u32(cig, 0x2bc) == 0xcdcdcdcdu);               /* the save row id */
    CHECK(u32(cig, 0x2cc) == 0xcdcdcdcdu);               /* ClassID from the save row */
    CHECK(u32(cig, 0x40) == 0xcdcdcdcdu && u32(cig, 0x48) == 0xcdcdcdcdu);   /* the car and its parts */
    CHECK(u32(cig, 0x25c) == 0xcdcdcdcdu && u32(cig, 0xaa8) == 0xcdcdcdcdu);
    if (!failed) puts("garage rating guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
