#!/usr/bin/env bash
# Parking EO Weather Control's time lock during a session (src/eo_timelock.inc): a pure step, so it runs natively without Wine. Every case runs
# with each known plugin build's layout (1.0.4 in Extra Options 0.4.4, 1.1.0 in 0.6.0).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "eo_timelock.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static _Alignas(8) unsigned char wc[0xb000], clock_a[0x400], clock_b[0x400];
static const struct eo_wc_layout *at;
#define ENABLED (*(int *)(wc + at->enabled))
#define WEATHER (*(int *)(wc + at->weather))
#define GENERATION (*(int *)(wc + at->generation))
#define HELD (*(unsigned char **)(wc + at->held_clock))
static void reset(int enabled, int weather, unsigned char *held)
{
    memset(wc, 0, sizeof wc);
    memset(clock_a, 0, sizeof clock_a);
    memset(clock_b, 0, sizeof clock_b);
    ENABLED = enabled;
    WEATHER = weather;
    HELD = held;
    clock_a[0x24] = held == clock_a;
}
static void cases(void)
{
    struct eo_lock s = {0};

    reset(1, 3, clock_a);                                             /* solo: the player's lock stands */
    CHECK(eo_lock_step(&s, wc, at, 0, clock_a) == EO_LOCK_NONE);
    CHECK(ENABLED == 1 && WEATHER == 3 && GENERATION == 0 && clock_a[0x24] == 1 && clock_a[0x361] == 0);

    CHECK(eo_lock_step(&s, wc, at, 1, NULL) == EO_LOCK_PARKED);           /* join, world not there yet */
    CHECK(ENABLED == 0 && WEATHER == 0 && GENERATION == 1 && clock_a[0x24] == 1);
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_CLOCK_RELEASED);
    CHECK(clock_a[0x24] == 0 && clock_a[0x361] == 1 && HELD == NULL);
    clock_a[0x361] = 0;                                               /* an event's fixed time: not ours to undo */
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_NONE);
    CHECK(clock_a[0x361] == 0 && GENERATION == 1);

    ENABLED = 1;                                                      /* the settings page during the session */
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_PARKED);
    CHECK(ENABLED == 0 && GENERATION == 2);

    CHECK(eo_lock_step(&s, wc, at, 0, clock_a) == EO_LOCK_RESTORED);      /* leave: both come back, and the plugin notices */
    CHECK(ENABLED == 1 && WEATHER == 3 && GENERATION == 3);
    CHECK(eo_lock_step(&s, wc, at, 0, clock_a) == EO_LOCK_NONE);
    CHECK(GENERATION == 3);

    reset(1, 0, clock_a);                                             /* the game has this clock fixed: wait for it */
    clock_a[0x25] = 1;
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_PARKED);
    CHECK(clock_a[0x24] == 1 && clock_a[0x361] == 0 && HELD == clock_a);
    clock_a[0x25] = 0;
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_CLOCK_RELEASED);
    CHECK(eo_lock_step(&s, wc, at, 0, NULL) == EO_LOCK_RESTORED);
    CHECK(ENABLED == 1 && WEATHER == 0);

    reset(1, 0, clock_a);                                             /* a new world: the old clock is gone, this one is untouched */
    CHECK(eo_lock_step(&s, wc, at, 1, clock_b) == EO_LOCK_PARKED);
    CHECK(clock_b[0x361] == 0 && clock_a[0x361] == 0 && HELD == clock_a);
    eo_lock_step(&s, wc, at, 0, NULL);

    reset(0, 0, NULL);                                                /* lock off: a session changes nothing */
    CHECK(eo_lock_step(&s, wc, at, 1, clock_a) == EO_LOCK_NONE);
    CHECK(eo_lock_step(&s, wc, at, 0, clock_a) == EO_LOCK_NONE);
    CHECK(GENERATION == 0 && clock_a[0x361] == 0);
}
int main(void)
{
    const struct eo_wc_layout *builds[] = {&EO_WC_1_0_4, &EO_WC_1_1_0};
    unsigned i;

    /* the four globals of a build never overlap (each is 4 bytes, the held clock 8, 8-aligned) */
    for (i = 0; i < 2; i++) {
        const struct eo_wc_layout *l = builds[i];
        CHECK(l->held_clock % 8 == 0);
        CHECK(l->enabled != l->weather && l->enabled != l->generation && l->weather != l->generation);
        CHECK(l->held_clock + 8 <= sizeof wc);
    }
    for (i = 0; i < 2; i++) {
        at = builds[i];
        cases();
    }
    if (!failed) puts("eo timelock: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
