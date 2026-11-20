#!/usr/bin/env bash
# The travel XP guard (src/travelxp_guard.inc, issue #428): the divisor it hands the game's rounding at game+0x66a492 (src/travelxp_rule.inc), natively.
# Expected values from the game's own code: XP rounded up to a multiple of RoundXPTo; with RoundXPTo 0 the game divided by zero, with the guard the XP
# stays as it is.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "travelxp_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main(void)
{
    CHECK(txg_divisor(0) == 1);                                       /* RoundXPTo 0: the crash case, no rounding instead */
    CHECK(txg_divisor(0x1234567800000000ull) == 0x1234567800000001ull);   /* only ecx is the divisor; rcx's upper half is kept */
    CHECK(txg_divisor(50) == 50);                                     /* a real RoundXPTo: the game's own path */
    CHECK(txg_divisor(0xffffffff00000032ull) == 0xffffffff00000032ull);
    /* the game's rounding: up to a multiple of RoundXPTo */
    CHECK(txg_round(123, 50) == 150);
    CHECK(txg_round(150, 50) == 150);
    CHECK(txg_round(1, 50) == 50);
    CHECK(txg_round(0, 50) == 0);
    /* with the guard's divisor of 1 the XP is the route's own */
    CHECK(txg_round(123, (int)txg_divisor(0)) == 123);
    CHECK(txg_round(0, (int)txg_divisor(0)) == 0);

    if (!failed) puts("travel xp guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
