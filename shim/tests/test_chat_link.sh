#!/usr/bin/env bash
# Chat link v1 (src/chat_link_wire.h): every encoder writes exactly the frame in tests/vectors/chat_link.txt, and every vector decodes back to
# the same values; framing of partial and impossible lengths. Natively, no Wine.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "chat_link_wire.h"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

static uint8_t vec[16][256];
static size_t vec_len[16];
static char vec_name[16][32];
static int nvec;

static void load(const char *path)
{
    char line[1024];
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL: cannot open %s\n", path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        char name[32], hex[600];
        size_t i;
        if (line[0] == '#' || sscanf(line, "%31s %599s", name, hex) != 2) continue;
        strcpy(vec_name[nvec], name);
        for (i = 0; hex[2 * i] && hex[2 * i + 1]; i++) {
            unsigned b;
            sscanf(hex + 2 * i, "%2x", &b);
            vec[nvec][i] = (uint8_t)b;
        }
        vec_len[nvec++] = i;
    }
    fclose(f);
}

static int find(const char *name)
{
    int i;
    for (i = 0; i < nvec; i++) if (!strcmp(vec_name[i], name)) return i;
    printf("FAIL: no vector %s\n", name);
    exit(1);
}

static void same(const char *name, const uint8_t *got, size_t n)
{
    int i = find(name);
    if (n != vec_len[i] || memcmp(got, vec[i], n)) { printf("FAIL: %s encodes differently\n", name); failed++; }
}

/* the vector's payload after framing it */
static const uint8_t *payload_of(const char *name, uint16_t want, size_t *plen)
{
    int i = find(name);
    uint16_t type; const uint8_t *p; size_t used;
    CHECK(cl_frame(vec[i], vec_len[i], &type, &p, plen, &used) == 1 && used == vec_len[i] && type == want);
    return p;
}

int main(int argc, char **argv)
{
    uint8_t out[700];
    size_t n, plen;
    const uint8_t *p;
    struct cl_hello h, h2;
    struct cl_pad pad = {0x1401, 0, 255, -32768, 32767, 100, -100}, pad2;
    struct cl_window win = {-1920, 0, 1920, 1080, 1}, win2;
    struct cl_game_state gs = {1, 1, 11}, gs2;
    struct cl_open_page op;
    uint16_t ver; uint8_t flags, b;
    int i;
    load(argc > 1 ? argv[1] : "tests/vectors/chat_link.txt");

    h.version = 1;
    for (i = 0; i < 32; i++) h.secret[i] = (uint8_t)i;
    h.pid = 4242; h.hwnd = 0x1a0b2c; h.xuid = 0x0009000027d5236bull;
    n = cl_put_hello(out, sizeof out, &h); same("hello", out, n);
    p = payload_of("hello", CL_HELLO, &plen);
    CHECK(cl_get_hello(p, plen, &h2) && h2.version == 1 && h2.pid == 4242 && h2.hwnd == 0x1a0b2c && h2.xuid == h.xuid && !memcmp(h2.secret, h.secret, 32));

    n = cl_put_welcome(out, sizeof out, 1, 1); same("welcome", out, n);
    p = payload_of("welcome", CL_WELCOME, &plen);
    CHECK(cl_get_welcome(p, plen, &ver, &flags) && ver == 1 && flags == 1);

    n = cl_put_pad(out, sizeof out, &pad); same("pad", out, n);
    p = payload_of("pad", CL_PAD, &plen);
    CHECK(cl_get_pad(p, plen, &pad2) && !memcmp(&pad, &pad2, sizeof pad));

    n = cl_put_byte(out, sizeof out, CL_GUIDE, 1); same("guide", out, n);
    n = cl_put_byte(out, sizeof out, CL_GUIDE_STATE, 1); same("guide_state", out, n);
    n = cl_put_byte(out, sizeof out, CL_BYE, 2); same("bye", out, n);
    p = payload_of("bye", CL_BYE, &plen);
    CHECK(cl_get_byte(p, plen, &b) && b == 2);

    n = cl_put_window(out, sizeof out, &win); same("window", out, n);
    p = payload_of("window", CL_WINDOW, &plen);
    CHECK(cl_get_window(p, plen, &win2) && win2.x == -1920 && win2.y == 0 && win2.w == 1920 && win2.h == 1080 && win2.flags == 1);

    n = cl_put_game_state(out, sizeof out, &gs); same("game_state", out, n);
    p = payload_of("game_state", CL_GAME_STATE, &plen);
    CHECK(cl_get_game_state(p, plen, &gs2) && gs2.online == 1 && gs2.mode == 1 && gs2.players == 11);

    n = cl_put_open_page(out, sizeof out, 1, "freeroam:7741dc1d", 17); same("open_page", out, n);
    p = payload_of("open_page", CL_OPEN_PAGE, &plen);
    CHECK(cl_get_open_page(p, plen, &op) && op.page == 1 && op.len == 17 && !strcmp(op.arg, "freeroam:7741dc1d"));

    /* limits: no room, an argument too long, wrong payload sizes */
    CHECK(cl_put_hello(out, 59, &h) == 0);
    CHECK(cl_put_open_page(out, sizeof out, 1, (const char *)out, CL_MAX_ARG + 1) == 0);
    CHECK(!cl_get_pad(p, 11, &pad2));
    CHECK(!cl_get_open_page((const uint8_t *)"\x01\x05\x00ab", 5, &op));

    /* framing: partial, two frames back to back, impossible lengths */
    {
        uint16_t type; size_t used;
        int g = find("guide"), w = find("welcome");
        uint8_t two[64];
        memcpy(two, vec[g], vec_len[g]);
        memcpy(two + vec_len[g], vec[w], vec_len[w]);
        CHECK(cl_frame(two, 3, &type, &p, &plen, &used) == 0);
        CHECK(cl_frame(two, vec_len[g] - 1, &type, &p, &plen, &used) == 0);
        CHECK(cl_frame(two, sizeof two, &type, &p, &plen, &used) == 1 && type == CL_GUIDE && used == vec_len[g]);
        CHECK(cl_frame(two + used, vec_len[w], &type, &p, &plen, &used) == 1 && type == CL_WELCOME);
        CHECK(cl_frame((const uint8_t *)"\x01\x00\x00\x00\x00\x00", 6, &type, &p, &plen, &used) == -1);
        CHECK(cl_frame((const uint8_t *)"\x01\x00\x01\x00\x00\x00", 6, &type, &p, &plen, &used) == -1);
    }

    if (!failed) puts("chat link: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t" tests/vectors/chat_link.txt
