#!/usr/bin/env bash
# Another player's car follows its owner's path (src/copy_path.inc): the blend fraction and the branch test, natively. With FH2_EXE set
# it also checks the exe (1.0.0.64): the hooked functions' first bytes and the blend call in helper_9effa0 whose arguments the hook
# reproduces (body, out, A at +0xa0, B at +0x1d0, seconds at [rsp+0x20], fraction at [rsp+0x28], vslot 0xb90).
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "copy_path.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
#define NEAR(a, b) ((a) - (b) < 1e-6f && (b) - (a) < 1e-6f)

static long file_off(const unsigned char *pe, unsigned rva)
{
    unsigned lfanew = *(const uint32_t *)(pe + 0x3c), nsec = *(const uint16_t *)(pe + lfanew + 6), opt = *(const uint16_t *)(pe + lfanew + 0x14), i;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = pe + lfanew + 0x18 + opt + i * 40;
        uint32_t va = *(const uint32_t *)(s + 12), vs = *(const uint32_t *)(s + 8), raw = *(const uint32_t *)(s + 20);
        if (rva >= va && rva < va + vs) return (long)(raw + (rva - va));
    }
    return -1;
}

static int at(const unsigned char *pe, unsigned rva, const unsigned char *want, size_t n)
{
    long o = file_off(pe, rva);
    return o > 0 && memcmp(pe + o, want, n) == 0;
}

int main(int argc, char **argv)
{
    /* the fraction helper_9effa0 used for the last frame */
    CHECK(NEAR(cp_frac(1000, 1120, 1060), 0.5f));
    CHECK(NEAR(cp_frac(1000, 1120, 1000), 0.0f));
    CHECK(NEAR(cp_frac(1000, 1120, 990), 0.0f));       /* a last frame before the blend began starts it at A */
    CHECK(NEAR(cp_frac(1000, 1120, 1200), 1.0f));      /* past the end: B */
    CHECK(cp_frac(1000, 1001, 1000) < 0.0f);           /* under 2 ms: the game applied no blend */
    CHECK(cp_frac(1000, 3001, 2000) < 0.0f);           /* over 2 s: neither */
    CHECK(cp_frac(1000, 3000, 2000) > 0.0f);           /* exactly 2 s still blends */
    CHECK(cp_frac(0, 0, 0) < 0.0f);                    /* a copy that never had a sample */
    CHECK(NEAR(cp_frac(-100, 20, -40), 0.5f));         /* the clock is a signed 32-bit count; negative values work the same */

    /* only the usual branch of ApplyUnpackedState is changed */
    CHECK(cp_restarted_from_body(0, 0, 5000, 5000));
    CHECK(!cp_restarted_from_body(1, 0, 5000, 5000));  /* coming back from stale data: the body is all there is */
    CHECK(!cp_restarted_from_body(2, 0, 5000, 5000));
    CHECK(!cp_restarted_from_body(0, 1, 5000, 5000));  /* snapped to the sample: A is B */
    CHECK(!cp_restarted_from_body(0, 0, 4990, 5000));  /* t0 from another source */

    if (argc > 1) {
        static unsigned char pe[64 << 20];
        static const unsigned char apply_head[12] = {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};
        static const unsigned char offset_body[21] = {0xC5, 0xF8, 0x28, 0x81, 0x20, 0x03, 0x00, 0x00, 0xC5, 0xF8, 0x58,
                                                      0x0A, 0xC5, 0xF8, 0x29, 0x89, 0x20, 0x03, 0x00, 0x00, 0xC3};
        static const unsigned char b_arg[7] = {0x4C, 0x8D, 0x8B, 0xD0, 0x01, 0x00, 0x00};   /* lea r9,[rbx+0x1d0] */
        static const unsigned char frac_arg[6] = {0xC5, 0xFA, 0x11, 0x74, 0x24, 0x28};       /* vmovss [rsp+0x28],xmm6 */
        static const unsigned char a_arg[7] = {0x4C, 0x8D, 0x83, 0xA0, 0x00, 0x00, 0x00};   /* lea r8,[rbx+0xa0] */
        static const unsigned char secs_arg[6] = {0xC5, 0xFA, 0x11, 0x44, 0x24, 0x20};       /* vmovss [rsp+0x20],xmm0 */
        static const unsigned char call[6] = {0xFF, 0x90, 0x90, 0x0B, 0x00, 0x00};           /* call [rax+0xb90] */
        FILE *f = fopen(argv[1], "rb");
        long n = f ? (long)fread(pe, 1, sizeof pe, f) : 0;
        if (f) fclose(f);
        CHECK(n > 0x40);
        CHECK(at(pe, 0x9ebc20, apply_head, sizeof apply_head));
        CHECK(at(pe, 0x9ecf80, offset_body, sizeof offset_body));
        CHECK(at(pe, 0x9f00bc, b_arg, sizeof b_arg));
        CHECK(at(pe, 0x9f00cb, frac_arg, sizeof frac_arg));
        CHECK(at(pe, 0x9f00d4, a_arg, sizeof a_arg));
        CHECK(at(pe, 0x9f00df, secs_arg, sizeof secs_arg));
        CHECK(at(pe, 0x9f00e5, call, sizeof call));
        if (!failed) puts("copy path: hooked functions and the blend call found in the exe");
    }
    if (!failed) puts("copy path: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
if [ -n "${FH2_EXE:-}" ]; then "$tmp/t" "$FH2_EXE"; else "$tmp/t"; fi
