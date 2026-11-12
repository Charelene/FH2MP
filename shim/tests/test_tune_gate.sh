#!/usr/bin/env bash
# The ANNA Tune Car gate (src/tune_gate.inc): which presses count as a mod's, and when one is refused online (src/tune_gate_rule.inc), natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "tune_gate_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main(void)
{
    const unsigned long long img = 0x140000000ull, size = 0x3200000ull;

    /* the game's own presses come from inside its image; no caller noted counts as the game's */
    CHECK(!tg_mod_press(img + 0xdc6f00, img, size));
    CHECK(!tg_mod_press(img, img, size));
    CHECK(!tg_mod_press(img + size - 1, img, size));
    CHECK(!tg_mod_press(0, img, size));
    /* a mod DLL above or below the image */
    CHECK(tg_mod_press(img + size, img, size));
    CHECK(tg_mod_press(0x7ffb12340000ull, img, size));
    CHECK(tg_mod_press(0x180001000ull - 0x100000000ull, img, size));
    /* no image found: nothing is gated */
    CHECK(!tg_mod_press(0x7ffb12340000ull, 0, size));

    /* online, the press opens only when the Tune Car tile and the Cars tab are both Enabled (Road Trip locks the Cars tab) */
    CHECK(!tg_refuses(1, 1));
    CHECK(tg_refuses(1, 0));
    CHECK(tg_refuses(0, 1));
    CHECK(tg_refuses(0, 0));

    if (!failed) puts("tune gate: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
