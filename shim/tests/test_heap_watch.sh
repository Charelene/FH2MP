#!/usr/bin/env bash
# The heap watch's bookkeeping (src/heap_watch.inc, the part without Windows calls): runs natively. Expected values from what the watch is for: what is
# still held at the end of the window, by caller, most bytes first; a freed block no longer counts; a full probe run is counted, not lost silently.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define HEAP_WATCH_LOGIC_ONLY
#include "heap_watch.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static heap_watch_table t;
int main(void)
{
    heap_watch_sum top[HEAP_WATCH_TOP];
    unsigned long long held = 0, p;
    unsigned long count = 0;
    unsigned n, i;

    /* caller A keeps 100 blocks of 1 MB; caller B makes 200 blocks of 64 KB and frees all but 10; caller C keeps one 8 MB block */
    for (i = 0; i < 100; i++) heap_watch_add(&t, 0x10000000ull + (unsigned long long)i * 0x100000, 1u << 20, 0xA);
    for (i = 0; i < 200; i++) heap_watch_add(&t, 0x80000000ull + (unsigned long long)i * 0x10000, 64u << 10, 0xB);
    for (i = 10; i < 200; i++) CHECK(heap_watch_remove(&t, 0x80000000ull + (unsigned long long)i * 0x10000));
    heap_watch_add(&t, 0xC0000000ull, 8u << 20, 0xC);
    CHECK(t.allocs == 301 && t.frees == 190);
    CHECK(t.alloc_bytes == 100ull * (1u << 20) + 200ull * (64u << 10) + (8u << 20));
    CHECK(t.freed_bytes == 190ull * (64u << 10));
    CHECK(heap_watch_has(&t, 0x10000000ull) && !heap_watch_has(&t, 0x80000000ull + 50 * 0x10000) && heap_watch_has(&t, 0x80000000ull + 9 * 0x10000));
    CHECK(!heap_watch_remove(&t, 0x12345));                          /* never followed: not a free of ours */
    n = heap_watch_top(&t, top, HEAP_WATCH_TOP, &held, &count);
    CHECK(n == 3);
    CHECK(top[0].caller == 0xA && top[0].bytes == 100ull << 20 && top[0].count == 100);
    CHECK(top[1].caller == 0xC && top[1].bytes == 8ull << 20 && top[1].count == 1);
    CHECK(top[2].caller == 0xB && top[2].bytes == 10ull * (64u << 10) && top[2].count == 10);
    CHECK(held == (100ull << 20) + (8ull << 20) + 10ull * (64u << 10) && count == 111);
    /* two blocks whose addresses hash to the same slot: freeing the first leaves the second findable, and the freed slot is taken again */
    memset(&t, 0, sizeof t);
    p = 0x5000;
    {
        unsigned long long q = p + 0x1000;
        while (heap_watch_hash(q) != heap_watch_hash(p)) q += 0x1000;
        heap_watch_add(&t, p, 4096, 1);
        heap_watch_add(&t, q, 8192, 2);
        CHECK(heap_watch_has(&t, p) && heap_watch_has(&t, q));
        CHECK(heap_watch_remove(&t, p) && !heap_watch_has(&t, p) && heap_watch_has(&t, q));
        heap_watch_add(&t, p, 4096, 1);
        CHECK(t.e[heap_watch_hash(p)].ptr == p && heap_watch_has(&t, q));
    }
    /* a probe run with no free slot in 64 places: the block is counted as dropped, not lost silently */
    memset(&t, 0, sizeof t);
    for (i = 0; i < 64; i++) t.e[(heap_watch_hash(0x7000) + i) & (HEAP_WATCH_SLOTS - 1)].ptr = 0x100000 + i;
    heap_watch_add(&t, 0x7000, 4096, 1);
    CHECK(t.dropped == 1 && t.allocs == 1 && !heap_watch_has(&t, 0x7000));
    if (failed) {
        return 1;
    }
    puts("heap watch: ok");
    return 0;
}
EOT
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
