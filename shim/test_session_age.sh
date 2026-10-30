#!/usr/bin/env bash
# The session-file age rule (src/session_age.inc): a pure function, so it runs natively without Wine.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include "session_age.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
int main(void)
{
    const unsigned long long S = 10000000ULL, now = 133000000000000000ULL, max = SESSION_MAX_AGE_TICKS;
    CHECK(SESSION_MAX_AGE_TICKS == 1800 * S);
    CHECK(!session_is_stale(now - 5 * S, now, max));                  /* Play just now */
    CHECK(!session_is_stale(now - 1800 * S, now, max));               /* exactly at the limit still counts */
    CHECK(session_is_stale(now - 1800 * S - 1, now, max));            /* one tick over */
    CHECK(session_is_stale(now - 3600 * S * 5, now, max));            /* left behind hours ago */
    CHECK(session_is_stale(now - 86400 * S, now, max));
    CHECK(!session_is_stale(now + 60 * S, now, max));                 /* a clock a minute off is not a reason */
    CHECK(session_is_stale(now + 7200 * S, now, max));                /* two hours in the future is */
    CHECK(!session_is_stale(0, now, max));                            /* undatable file: left to the launcher */
    CHECK(!session_is_stale(now - 86400 * S, now, 0));                /* FH2MP_SESSION_MAX_AGE_S=0: no limit (rigs) */
    if (!failed) puts("session age: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
