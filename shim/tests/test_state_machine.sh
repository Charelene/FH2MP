#!/usr/bin/env bash
# The animation state machine guard (src/state_machine_guard.inc, issue #502): when the jle at game+0x1710ebb is turned into "no nodes"
# (src/state_machine_rule.inc), natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "state_machine_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main(void)
{
    /* the flags of "cmp count, 0" with IF and bit 1 set, as a real rflags carries */
    const unsigned long long positive = 0x202, zero = 0x202 | SM_ZF, negative = 0x202 | SM_SF, list = 0x6029764f10a4ull;

    CHECK(sm_skip(positive, 0));                    /* nodes counted, no list: the crash case */
    CHECK(!sm_skip(positive, list));                /* nodes counted, a list: the game's own walk */
    CHECK(!sm_skip(zero, 0));                       /* no nodes: the game takes the jle anyway */
    CHECK(!sm_skip(negative, 0));                   /* a negative count: jle taken too */
    CHECK(!sm_skip(0x202 | SM_SF | SM_OF, list));   /* SF equal to OF: greater, the walk runs */
    CHECK(sm_skip(0x202 | SM_SF | SM_OF, 0));       /* ... and without a list it is skipped */
    CHECK(!sm_skip(0x202 | SM_OF, 0));              /* SF not equal to OF: less, jle taken */

    if (!failed) puts("state machine guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
