#!/usr/bin/env bash
# The request blob limit (src/blob_limit_guard.inc, src/blob_limit_rule.inc): the bytes the patch expects and writes, natively. A function image
# is laid out from the rule's own tables (the game's bytes are not in the repository) and the change is applied to it as the platform would.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "blob_limit_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

int main(void)
{
    unsigned char fn[0xe8];
    unsigned char before[0xe8];
    unsigned i, changed = 0;

    memset(fn, 0x90, sizeof fn);
    memcpy(fn, g_bl_head, sizeof g_bl_head);
    memcpy(fn + BL_SITE_OFF, g_bl_site, sizeof g_bl_site);
    memcpy(fn + BL_FAIL_OFF, g_bl_fail, sizeof g_bl_fail);
    memcpy(before, fn, sizeof fn);

    /* the layout the guard relies on */
    CHECK(BL_SITE_OFF + sizeof g_bl_site <= BL_FAIL_OFF);
    CHECK(BL_FAIL_OFF + sizeof g_bl_fail <= sizeof fn);
    CHECK(BL_IMM_OFF == BL_SITE_OFF + 3 && sizeof g_bl_old == 4 && sizeof g_bl_new == 4);
    CHECK(g_bl_site[0] == 0x48 && g_bl_site[1] == 0x81 && g_bl_site[2] == 0xfb);   /* cmp rbx,imm32 */
    CHECK(g_bl_site[7] == 0x0f && g_bl_site[8] == 0x86);                           /* jbe rel32: the limit is kept */
    CHECK(g_bl_fail[0] == 0xb8 && g_bl_fail[1] == 0x0c && g_bl_fail[4] == 0x80);   /* mov eax,0x8050000c: XLS_E_DATA_SIZE */

    /* the game's limit is what the rule says it is */
    CHECK(bl_limit_is(fn + BL_SITE_OFF, BL_LIMIT_OLD));
    CHECK(!bl_limit_is(fn + BL_SITE_OFF, BL_LIMIT_NEW));
    CHECK(memcmp(fn + BL_IMM_OFF, g_bl_old, 4) == 0);

    /* the change, as plat_patch makes it: the four bytes at BL_IMM_OFF, nothing else */
    memcpy(fn + BL_IMM_OFF, g_bl_new, 4);
    for (i = 0; i < sizeof fn; i++) {
        changed += fn[i] != before[i];
    }
    CHECK(changed == 1);                                      /* 00 00 08 00 -> 00 00 40 00: one byte differs */
    CHECK(fn[BL_IMM_OFF + 2] == 0x40 && fn[BL_IMM_OFF + 3] == 0x00);
    CHECK(bl_limit_is(fn + BL_SITE_OFF, BL_LIMIT_NEW));
    CHECK(memcmp(fn, before, BL_IMM_OFF) == 0);               /* everything before the immediate is as it was */
    CHECK(memcmp(fn + BL_IMM_OFF + 4, before + BL_IMM_OFF + 4, sizeof fn - BL_IMM_OFF - 4) == 0);

    /* the new limit: more room than the Xbox service's, within what the LSP (16 MiB) and its nginx (20 MiB) take, and a positive imm32 */
    CHECK(BL_LIMIT_OLD == 512u * 1024u);
    CHECK(BL_LIMIT_NEW > BL_LIMIT_OLD && BL_LIMIT_NEW <= 8u << 20);
    CHECK(bl_limit_is(fn + BL_SITE_OFF, BL_LIMIT_NEW) && !bl_limit_is(fn + BL_SITE_OFF, 0x80000000u));
    if (!failed) puts("blob limit: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
