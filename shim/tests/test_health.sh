#!/usr/bin/env bash
# The watcher's decisions and the run id text (src/health.inc, the part without Windows calls): runs natively. The run ids it writes must pass the server's own
# check (native/internal/diag RunID), or the server would drop them from its lines without a word.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define HEALTH_LOGIC_ONLY
#include "health.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static health_state h;
static unsigned long long ms, pause_ms;
static int step(unsigned long long now, unsigned long long last) { ms = pause_ms = 0; return health_step(&h, now, last, &ms, &pause_ms); }
int main(void)
{
    unsigned long long t = 1000000;
    char text[20];
    int i;

    CHECK(step(t, 0) == HEALTH_NOTHING);                         /* no prediction yet: the menus before the first session say nothing */
    for (i = 0; i < 10; i++) {                                   /* never flowed: never "stopped" */
        CHECK(step(t += 1000, 0) == HEALTH_NOTHING);
    }
    t += 1000;
    CHECK(step(t, t - 60000) == HEALTH_NOTHING);                 /* a stale time seen first (the watcher started late): nothing was seen stopping */
    t += 1000;
    CHECK(step(t, t - 61000) == HEALTH_NOTHING);
    for (i = 0; i < 5; i++) {                                    /* flowing */
        t += 1000;
        CHECK(step(t, t - 16) == HEALTH_NOTHING);
    }
    unsigned long long stop = t - 16;
    CHECK(step(t += 1000, stop) == HEALTH_NOTHING);              /* a gap below the limit is not a stop */
    CHECK(step(t += 1000, stop) == HEALTH_NOTHING);
    CHECK(step(t += 1000, stop) == HEALTH_NOTHING);
    CHECK(step(t += 1000, stop) == HEALTH_NOTHING);
    CHECK(step(t += 1000, stop) == HEALTH_PREDICTIONS_STOPPED);  /* once */
    CHECK(ms >= HEALTH_SILENT_MS);
    for (i = 0; i < 60; i++) {
        CHECK(step(t += 1000, stop) == HEALTH_NOTHING);          /* and only once */
    }
    t += 1000;
    CHECK(step(t, t - 10) == HEALTH_PREDICTIONS_RESUMED);
    CHECK(ms == t - 10 - stop);                                  /* the length of the silence */
    t += 1000;
    CHECK(step(t, t - 10) == HEALTH_NOTHING);
    t += 9000;                                                   /* the whole process stood still: this is not a game stop on its own */
    CHECK(step(t, t - 9000) == (HEALTH_PROCESS_PAUSED | HEALTH_PREDICTIONS_STOPPED));
    CHECK(pause_ms == 9000);
    t += 1000;
    CHECK(step(t, t - 5) == HEALTH_PREDICTIONS_RESUMED);
    t += 2999;
    CHECK(!(step(t, t - 5) & HEALTH_PROCESS_PAUSED));            /* just under the limit */

    CHECK(run_id_ok("ab12cd34"));
    CHECK(!run_id_ok("AB12CD34"));
    CHECK(!run_id_ok("ab12cd3"));
    CHECK(!run_id_ok("ab12cd345"));
    CHECK(!run_id_ok("ab12cd3g"));
    CHECK(!run_id_ok(""));
    run_request_text(text, "ab12cd34", 57);
    CHECK(!strcmp(text, "ab12cd34.57"));
    puts(text);
    run_request_text(text, "ab12cd34", 0);
    CHECK(!strcmp(text, "ab12cd34.0"));
    run_request_text(text, "ab12cd34", 4294967295UL);
    CHECK(!strcmp(text, "ab12cd34.4294967295") && strlen(text) < sizeof text);
    puts(text);
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t" > "$tmp/ids"
# the server's rule for a run id (native/internal/diag RunID, formerly server/diag.py run_id): 8 hex digits, optionally .<request number>
while IFS= read -r line; do
    line=${line%$'\r'}
    [[ "$line" =~ ^[0-9a-f]{8}(\.[0-9]{1,10})?$ ]] || { echo "run id the server would not accept: $line"; exit 1; }
done < "$tmp/ids"
echo "health: all checks pass"
