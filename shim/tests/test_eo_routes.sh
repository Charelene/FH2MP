#!/usr/bin/env bash
# Passing the online route feature state on to whatever FH2 Extra Options put on the offline call sites (src/eo_routes.inc): the pure parts
# (which feature a game string names, where a call rel32 goes, when to pass on), natively without Wine.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "eo_routes.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

static struct eor_str heap(const char *text)
{
    struct eor_str s;
    memset(&s, 0, sizeof s);
    s.u.ptr = text;
    s.length = strlen(text);
    s.capacity = s.length < 16 ? 16 : s.length;
    return s;
}

static struct eor_str inline_str(const char *text)
{
    struct eor_str s;
    memset(&s, 0, sizeof s);
    strcpy(s.u.text, text);
    s.length = strlen(text);
    s.capacity = 15;
    return s;
}

static void call(unsigned char code[5], unsigned long long site, unsigned long long to)
{
    int rel = (int)(long long)(to - (site + 5));
    code[0] = 0xE8;
    memcpy(code + 1, &rel, 4);
}

int main(void)
{
    struct eor_str s, b;
    unsigned char code[5];
    const unsigned long long img = 0x140000000ull, set = img + EOR_SET_FEATURE_RVA, site = img + EOR_MAINLAND_CALL_RVA;
    char longer[] = "online_route_activationsX";

    /* the game's strings: both names are longer than 15, so they are on the heap; another feature, a prefix, a longer name, an inline one */
    s = heap("online_route_activations");
    CHECK(eor_route_group(&s) == 0);
    s = heap("online_island_route_activations");
    CHECK(eor_route_group(&s) == 1);
    s = heap("online_route_activations_entities");
    CHECK(eor_route_group(&s) == -1);
    s = heap(longer);
    s.length = 24;                                    /* the length decides, not the terminator */
    CHECK(eor_route_group(&s) == 0);
    s = heap("career_events");
    CHECK(eor_route_group(&s) == -1);
    s = inline_str("tombola");
    CHECK(eor_route_group(&s) == -1);
    s = heap("online_route_activations");
    s.length = 1000;                                  /* a broken string is never read past 64 */
    CHECK(eor_route_group(&s) == -1);
    CHECK(eor_route_group(NULL) == -1);

    /* the borrowed name reads back as the same feature, with the text out of line */
    eor_borrow(&b, 0);
    CHECK(eor_route_group(&b) == 0 && b.capacity >= 16 && eor_str_text(&b) == EOR_MAINLAND);
    eor_borrow(&b, 1);
    CHECK(eor_route_group(&b) == 1 && b.capacity >= 16 && eor_str_text(&b) == EOR_ISLAND);

    /* call rel32 targets, forward and backward, and bytes that are not a call */
    call(code, site, set);
    CHECK(eor_call_target(code, site) == set);
    call(code, site, img + 0x816f04);
    CHECK(eor_call_target(code, site) == img + 0x816f04);
    call(code, site, img + 0x2c00000);
    CHECK(eor_call_target(code, site) == img + 0x2c00000);
    memcpy(code, "\xff\x15\0\0\0", 5);
    CHECK(eor_call_target(code, site) == 0);

    /* pass on only when someone redirected the offline call: vanilla (SetFeature) and an unknown change get nothing */
    call(code, site, set);
    CHECK(eor_forward_target(code, site, set) == 0);
    call(code, site, img + 0x816f04);                 /* Extra Options 0.6.0's thunk */
    CHECK(eor_forward_target(code, site, set) == img + 0x816f04);
    code[0] = 0xE9;
    CHECK(eor_forward_target(code, site, set) == 0);

    if (!failed) puts("eo routes: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
