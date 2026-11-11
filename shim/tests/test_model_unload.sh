#!/usr/bin/env bash
# The car model unload guard (src/model_unload_guard.inc, issue #449): when the je at game+0xf6d8ff is turned into "not this car's event"
# (src/model_unload_rule.inc), natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "model_unload_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main(void)
{
    const unsigned long long zf = MU_ZF, other_flags = 0x202;   /* IF and bit 1, as a real rflags carries */

    CHECK(mu_skip(other_flags, 0, 0));              /* this car's event, no loader, immediate swap: the crash case */
    CHECK(!mu_skip(other_flags | zf, 0, 0));        /* not this car's event: the game takes the je anyway */
    CHECK(!mu_skip(other_flags, 0x26b48c90000ull, 0));   /* a loader: the game's own path */
    CHECK(!mu_skip(other_flags, 0, 1));             /* deferred swap: +0xf7c880, no loader call here */

    if (!failed) puts("model unload guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
