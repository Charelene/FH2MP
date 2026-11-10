#!/usr/bin/env bash
# Azimuth's XboxUserId follows the game Azimuth really starts (src/identity_scope.inc): the decision and the replacement load, natively without
# Wine. The load bytes are checked against the assembler when one is there.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "identity_scope.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
#define ACCOUNT 0x00090000abd7df93ull
#define AZIMUTH 0x0009000000000000ull

static int answers[8], asked;
static int started(void *ctx)
{
    (void)ctx;
    return answers[asked++];
}

static void fresh(struct id_scope *s, unsigned long long *slot, int a0, int a1, int a2)
{
    memset(s, 0, sizeof *s);
    s->slot = slot;
    s->account = ACCOUNT;
    s->azimuth = AZIMUTH;
    *slot = AZIMUTH;
    asked = 0;
    answers[0] = a0;
    answers[1] = a1;
    answers[2] = a2;
}

int main(void)
{
    struct id_scope s;
    unsigned long long slot;
    unsigned char load[10];
    int game;

    /* FH2 started: the account id from the first request on, decided once */
    fresh(&s, &slot, 1, 0, 0);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 1 && game == 1 && slot == ACCOUNT);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 0 && slot == ACCOUNT && asked == 1);

    /* another game (FH3, FM5) started: Azimuth's own id, for good, even if asked again */
    fresh(&s, &slot, 0, 1, 1);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 1 && game == 0 && slot == AZIMUTH);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 0 && slot == AZIMUTH && asked == 1);

    /* asked before any game is mapped: Azimuth's id for that request, the next one decides */
    fresh(&s, &slot, -1, -1, 1);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 1 && game < 0 && slot == AZIMUTH && s.state == ID_OPEN);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 1 && game < 0 && slot == AZIMUTH);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 1 && game == 1 && slot == ACCOUNT && s.state == ID_DECIDED);
    CHECK(id_scope_settle(&s, started, NULL, &game) == 0 && asked == 3);

    /* the load: mov r8, [rip+disp32] + nop dword [rax], disp from the end of the 7-byte mov */
    CHECK(id_scope_load(load, 0x7ff600042185ull, 0x7ff600030000ull));
    CHECK(load[0] == 0x4C && load[1] == 0x8B && load[2] == 0x05 && load[7] == 0x0F && load[8] == 0x1F && load[9] == 0x00);
    {
        int disp;
        memcpy(&disp, load + 3, 4);
        CHECK(0x7ff600042185ll + 7 + disp == 0x7ff600030000ll);
    }
    CHECK(id_scope_load(load, 0x7ff600042185ull, 0x7ff600042185ull + 7 + 0x7FFFFFFFull));
    CHECK(!id_scope_load(load, 0x7ff600042185ull, 0x7ff600042185ull + 7 + 0x80000000ull));
    CHECK(id_scope_load(load, 0x7ff600042185ull, 0x7ff600042185ull + 7 - 0x80000000ull));
    CHECK(!id_scope_load(load, 0x7ff600042185ull, 0x7ff600042185ull + 7 - 0x80000001ull));

    /* what an assembler makes of the same instruction (printed for the shell part below) */
    id_scope_load(load, 0x1000, 0x2000);
    for (game = 0; game < 10; game++) printf("%02x", load[game]);
    printf("\n");
    if (!failed) puts("identity scope: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t" > "$tmp/out"
cat "$tmp/out"
# 0x2000 - (0x1000 + 7) = 0xff9: the same bytes as `mov r8, [rip+0xff9]; nop dword [rax]`
want=$(head -1 "$tmp/out")
[ "$want" = 4c8b05f90f00000f1f00 ] || { echo "FAIL: load bytes $want"; exit 1; }
grep -q "all checks pass" "$tmp/out"
