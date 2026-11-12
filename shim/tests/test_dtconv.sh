#!/usr/bin/env bash
# The drivetrain converter guard (src/dtconv_guard.inc, issue #412): which converter values name a car that is not in the car table
# (src/dtconv_rule.inc), natively, with fakes for the game's functions.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "dtconv_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

static char int_t, int64_t_, other_t;
typedef struct { void *type; char pad[0x20]; unsigned long long v; } boxed;   /* the value sits at +0x28 */
static void *fake_int(void *u) { (void)u; return &int_t; }
static void *fake_int64(void *u) { (void)u; return &int64_t_; }
static unsigned char fake_is_a(void *value, void *type) { return ((boxed *)value)->type == type; }
static void *cars[3];
static int lookups;
static void *fake_car(void *table, unsigned id) { (void)table; lookups++; return id < 3 ? cars[id] : NULL; }   /* game+0x26f900 */
static char table_obj;
static struct { char pad[0x3a0]; void *table; } provider = {{0}, &table_obj};
static void *service = &provider;

int main(void)
{
    dtc_game g = {fake_int, fake_int64, fake_is_a, fake_car, (void *const *)&service};
    boxed unset64 = {&int64_t_, {0}, 0x7FFFFFFFFFFFFFFFull};   /* both #412 dumps: a car card with no car */
    boxed known = {&int_t, {0}, 1}, gap = {&int_t, {0}, 2}, past = {&int_t, {0}, 77}, text = {&other_t, {0}, 0xFFFFFFFFull};
    static char car1;
    cars[1] = &car1;

    CHECK(dtc_car_missing(&g, &unset64));            /* ordinal 0xFFFFFFFF: past the table, the crash case */
    CHECK(!dtc_car_missing(&g, &known));             /* a car the table has: the game's own code */
    CHECK(dtc_car_missing(&g, &gap));                /* an empty entry inside the table reads NULL too */
    CHECK(dtc_car_missing(&g, &past));
    lookups = 0;
    CHECK(!dtc_car_missing(&g, &text));              /* not an int: the converter answers unset itself, no lookup made */
    CHECK(lookups == 0);
    CHECK(!dtc_car_missing(&g, NULL));
    provider.table = NULL;
    CHECK(!dtc_car_missing(&g, &unset64));           /* no car table yet: leave it to the game */
    provider.table = &table_obj;
    service = NULL;
    CHECK(!dtc_car_missing(&g, &unset64));           /* no service provider yet */
    g.service = NULL;
    CHECK(!dtc_car_missing(&g, &unset64));

    if (!failed) puts("dtconv guard: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
