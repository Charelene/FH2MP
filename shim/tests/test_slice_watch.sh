#!/usr/bin/env bash
# The slice watch's bookkeeping (src/slice_watch.inc, the part without Windows calls): runs natively. Expected values come from what the watch is for (the buffer
# whose retire queue only grows is named first, with the game call that discards it) and from MSVC's std::deque layout (blocks of 2 pointers for 8-byte elements).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define SLICE_WATCH_LOGIC_ONLY
#include "slice_watch.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static slice_watch_table t;
int main(void)
{
    unsigned top[SLICE_WATCH_TOP], n, i;
    unsigned long long block = 99;
    unsigned place = 9;

    /* a healthy buffer: discarded every frame, its queue stays at 2-3 slices behind the GPU */
    for (i = 0; i < 300; i++) {
        slice_watch_note(&t, 0x1000, 0x5b0b70, 65536, 2 + i % 2, 4, 1, 1000 + i, 999 + i, 0);
    }
    /* a stuck one: its front slice never got a fence, the queue grows with every discard */
    for (i = 0; i < 300; i++) {
        slice_watch_note(&t, 0x2000, 0x10c8974, 4194304, 1 + i, 2 + i, 1, ~0ull, 999 + i, 0);
    }
    /* a buffer from deferred command lists, seen once, with no game frame on the stack */
    slice_watch_note(&t, 0x3000, SLICE_WATCH_UNKNOWN, 256, 1, 1, 0, 0, 1300, 1);
    CHECK(t.discards == 601);
    CHECK(t.passed == 1300);
    n = slice_watch_top(&t, SLICE_WATCH_TOP, top);
    CHECK(n == 3);
    CHECK(t.r[top[0]].res == 0x2000);                 /* the longest queue first */
    CHECK(t.r[top[0]].queue_max == 300);
    CHECK(t.r[top[0]].list_max == 301);
    CHECK(t.r[top[0]].discards == 300);
    CHECK(t.r[top[0]].caller == 0x10c8974);
    CHECK(t.r[top[0]].front_ok && t.r[top[0]].front_fence == ~0ull);
    CHECK(t.r[top[0]].size == 4194304);
    CHECK(t.r[top[1]].res == 0x1000 && t.r[top[1]].queue_max == 3);
    CHECK(t.r[top[2]].res == 0x3000 && t.r[top[2]].from_list && !t.r[top[2]].front_ok);
    /* the first game frame is kept; a later sample does not replace it, but fills one that was missing */
    slice_watch_note(&t, 0x2000, 0x123, 4194304, 301, 302, 1, ~0ull, 1301, 0);
    CHECK(t.r[top[0]].caller == 0x10c8974);
    slice_watch_note(&t, 0x3000, 0x456, 256, 1, 1, 0, 0, 1302, 1);
    CHECK(t.r[top[2]].caller == 0x456);
    /* only n asked for */
    CHECK(slice_watch_top(&t, 1, top) == 1 && t.r[top[0]].res == 0x2000);
    /* a full table drops new buffers and says how many */
    memset(&t, 0, sizeof t);
    for (i = 0; i < SLICE_WATCH_SLOTS + 7; i++) {
        slice_watch_note(&t, 0x10000 + 0x40ull * i, 1, 16, 0, 1, 0, 0, 5, 0);
    }
    CHECK(t.dropped == 7);
    CHECK(t.discards == SLICE_WATCH_SLOTS + 7);
    /* std::deque<T*> front: offset 5 in a map of 8 blocks is block 2, second place; offsets wrap around the map */
    CHECK(slice_watch_front_slot(8, 5, &block, &place) && block == 2 && place == 1);
    CHECK(slice_watch_front_slot(8, 16, &block, &place) && block == 0 && place == 0);
    CHECK(slice_watch_front_slot(1, 3, &block, &place) && block == 0 && place == 1);
    CHECK(!slice_watch_front_slot(6, 5, &block, &place));     /* not a power of two: not a deque map, nothing read */
    CHECK(!slice_watch_front_slot(0, 0, &block, &place));
    if (failed) {
        return 1;
    }
    puts("slice watch: ok");
    return 0;
}
EOT
${CC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
