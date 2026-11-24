#!/usr/bin/env bash
# Results wait report (src/results_wait.inc, the part without Windows calls): which episode a game is in, when it reports, and the text. Expected values come
# from the game server's log of issue 547 (a client back in Free Roam leaves the results screen about 33 s after the race-over update) and the wording
# the server's client report line carries. Runs natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define RS_LOGIC_ONLY
#include "results_wait.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
int main(void)
{
    rs_state st;
    rs_in in;
    unsigned secs = 0;
    rs_text t;
    static const signed char vars[7] = {1, 1, 1, 1, 0, 1, 1};
    signed char unreadable[7] = {-1, -1, -1, -1, -1, -1, -1};

    /* the race-over update arrived at tick 1000 and the console is still in the event: 45 s later it reports, again at 120 s, then never */
    memset(&st, 0, sizeof st);
    memset(&in, 0, sizeof in);
    in.part = 1; in.in_race = 1; in.hooked = 1; in.host = 0; in.over_at = 1000; in.host_at = 1000; in.seq = 12;
    CHECK(rs_step(&st, &in, 1000 + 44999, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 1000 + 45000, &secs) == RS_OVER_SEEN && secs == 45);
    CHECK(rs_step(&st, &in, 1000 + 46000, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 1000 + 119999, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 1000 + 121000, &secs) == RS_OVER_SEEN && secs == 121);
    CHECK(rs_step(&st, &in, 1000 + 400000, &secs) == RS_NONE);

    /* leaving the event (ParticipatingInRace 0) ends it; the next race over starts a new episode with its own two reports */
    in.part = 0;
    CHECK(rs_step(&st, &in, 1000 + 500000, &secs) == RS_NONE);
    in.part = 1; in.over_at = 700000;
    CHECK(rs_step(&st, &in, 700000 + 45000, &secs) == RS_OVER_SEEN && secs == 45);

    /* a race over that has not been seen: the car sitting still in a race the game believes is on */
    memset(&st, 0, sizeof st);
    memset(&in, 0, sizeof in);
    in.part = 1; in.in_race = 1; in.hooked = 1; in.host = 1; in.host_at = 500; in.over_at = 0; in.idle_known = 1; in.idle_since = 100000;
    CHECK(rs_step(&st, &in, 100000 + 44000, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 100000 + 45000, &secs) == RS_OVER_MISSING && secs == 45);
    in.idle_since = 150000;                                            /* the car moved, then stopped again: a new episode */
    CHECK(rs_step(&st, &in, 150000 + 10000, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 150000 + 45000, &secs) == RS_OVER_MISSING);
    /* the car stood still on the grid before the start: the wait counts from the start (dev 2026-11-23: still since 12 s before it) */
    memset(&st, 0, sizeof st);
    in.idle_since = 200000; in.host_at = 212000;
    CHECK(rs_step(&st, &in, 200000 + 45000, &secs) == RS_NONE);
    CHECK(rs_step(&st, &in, 212000 + 45000, &secs) == RS_OVER_MISSING && secs == 45);
    in.host = -1;                                                      /* no InRace update at all (joined late, or the hook is off) */
    in.hooked = 0;
    in.idle_since = 300000;
    CHECK(rs_step(&st, &in, 300000 + 45000, &secs) == RS_OVER_MISSING);

    /* none of these is an episode: the race flag is 0 without a 1 -> 0 before it (the lobby before the start), RaceInProgress clear (the grid, Free Roam),
     * no car position known */
    memset(&st, 0, sizeof st);
    memset(&in, 0, sizeof in);
    in.part = 1; in.in_race = 1; in.hooked = 1; in.host = 0; in.over_at = 0; in.idle_known = 1; in.idle_since = 1000;
    CHECK(rs_step(&st, &in, 1000 + 900000, &secs) == RS_NONE);
    in.host = 1; in.in_race = 0;
    CHECK(rs_step(&st, &in, 1000 + 900000, &secs) == RS_NONE);
    in.in_race = 1; in.idle_known = 0;
    CHECK(rs_step(&st, &in, 1000 + 900000, &secs) == RS_NONE);
    in.part = -1; in.idle_known = 1;
    CHECK(rs_step(&st, &in, 1000 + 900000, &secs) == RS_NONE);

    /* the text */
    memset(&in, 0, sizeof in);
    in.part = 1; in.in_race = 1; in.hooked = 1; in.host = 0; in.over_at = 1000; in.host_at = 1000; in.seq = 12;
    rs_describe(&t, RS_OVER_SEEN, 47, &in, 1000 + 47000, vars, "RP_ONLINE_RACE");
    CHECK(!strcmp(t.s, "results wait 47 s: race over received 47 s ago (update #12); console R1 S1 P1 I1 Y0 L1 G1; rp RP_ONLINE_RACE"));
    puts(t.s);
    in.host = 1; in.over_at = 0; in.host_at = 5000;
    rs_describe(&t, RS_OVER_MISSING, 47, &in, 305000, vars, "RP_ONLINE_RACE");
    CHECK(!strcmp(t.s, "results wait 47 s: no race over received (InRace last 1, 300 s ago); car still 47 s; console R1 S1 P1 I1 Y0 L1 G1; rp RP_ONLINE_RACE"));
    puts(t.s);
    in.host = -1; in.host_at = 0;
    rs_describe(&t, RS_OVER_MISSING, 60, &in, 305000, unreadable, NULL);
    CHECK(!strcmp(t.s, "results wait 60 s: no race over received (InRace last none); car still 60 s; console R? S? P? I? Y? L? G?"));
    puts(t.s);
    in.hooked = 0;
    rs_describe(&t, RS_OVER_MISSING, 60, &in, 305000, vars, "");
    CHECK(!strcmp(t.s, "results wait 60 s: no race over seen, the message hook is off; car still 60 s; console R1 S1 P1 I1 Y0 L1 G1"));
    CHECK(strlen(t.s) < RS_TEXT);
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
echo "results wait: all checks pass"
