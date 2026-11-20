#!/usr/bin/env bash
# The guard counts' header text and change detection (src/guard_counts.inc, the part without Windows calls). Runs natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define GUARD_COUNTS_LOGIC_ONLY
#include "guard_counts.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
int main(void)
{
    guard_count c[3] = {{"camanim", 0}, {"dtconv", 0}, {"travelxp", 0}};
    long sent[3] = {0, 0, 0};
    char text[64];

    CHECK(!guard_counts_changed(c, 3, sent));                       /* a game whose guards never fired sends nothing */
    CHECK(guard_counts_text(text, sizeof text, c, 3) == 0 && text[0] == 0);
    c[1].count = 1;
    CHECK(guard_counts_changed(c, 3, sent) && sent[1] == 1);
    CHECK(!guard_counts_changed(c, 3, sent));                       /* once per change */
    guard_counts_text(text, sizeof text, c, 3);
    CHECK(!strcmp(text, "v1 dtconv=1"));
    c[0].count = 3, c[2].count = 1234567;
    CHECK(guard_counts_changed(c, 3, sent));
    guard_counts_text(text, sizeof text, c, 3);
    CHECK(!strcmp(text, "v1 camanim=3 dtconv=1 travelxp=1234567"));
    puts(text);
    CHECK(guard_counts_text(text, 12, c, 3) == 11 && !strcmp(text, "v1 camanim="));   /* cut, still terminated */
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
echo "guard counts: all checks pass"
