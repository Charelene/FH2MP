#!/usr/bin/env bash
# Chat link v2 (src/chat_link_wire.h): every encoder writes exactly the frame in tests/vectors/chat_link.txt, and every vector decodes back to
# the same values (those of Encore Launcher's P6 contract, below); framing of partial and impossible lengths; payloads one byte short or
# long, a str over 8192 bytes and a list whose count does not match its entries are refused; encoders refuse what does not fit. Natively, no
# Wine. With a C++ compiler (HOSTCXX, default c++) the header is also compiled as C++, as the Guide includes it.
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

static uint8_t vec[16][512];
static size_t vec_len[16];
static char vec_name[16][32];
static int nvec;

static void load(const char *path)
{
    char line[1200];
    FILE *f = fopen(path, "r");
    if (!f) { printf("FAIL: cannot open %s\n", path); exit(1); }
    while (fgets(line, sizeof line, f)) {
        char name[32], hex[1100];
        size_t i;
        if (line[0] == '#' || sscanf(line, "%31s %1099s", name, hex) != 2) continue;
        if (nvec == 16) { printf("FAIL: more than 16 vectors\n"); exit(1); }
        strcpy(vec_name[nvec], name);
        for (i = 0; hex[2 * i] && hex[2 * i + 1] && i < sizeof vec[0]; i++) {
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

static int is(const struct cl_str *s, const char *want)
{
    return s->len == strlen(want) && !memcmp(s->s, want, s->len) && s->s[s->len] == 0;
}

#define DM "2533274790399235"
#define S(x) x, strlen(x)
static const uint64_t ME = 0x0009000000000d01ull, FOX = 0x0009000000000d03ull;

/* decoded values, static: each holds 8 KB buffers per str */
static struct cl_send send_m;
static struct cl_read read_m;
static struct cl_chat chat;
static struct cl_message msg;
static struct cl_sent sent_m;
static struct cl_notice notice_m;
static struct cl_str str;
static uint8_t big[CL_MAX_FRAME + 64], junk[CL_MAX_STR + 2];

int main(int argc, char **argv)
{
    uint8_t out[700], mod[700];
    size_t n, plen;
    const uint8_t *p;
    struct cl_hello h, h2;
    struct cl_welcome w = {2, 1, 0x0009000000000d01ull}, w2;
    struct cl_list l;
    struct cl_entries e;
    uint8_t b;
    int i;
    load(argc > 1 ? argv[1] : "tests/vectors/chat_link.txt");
    CHECK(nvec == 11);

    h.version = 2;
    for (i = 0; i < 32; i++) h.secret[i] = (uint8_t)i;
    h.pid = 4242, h.xuid = ME;
    n = cl_put_hello(out, sizeof out, &h); same("hello", out, n);
    p = payload_of("hello", CL_HELLO, &plen);
    CHECK(cl_get_hello(p, plen, &h2) && h2.version == 2 && h2.pid == 4242 && h2.xuid == ME && !memcmp(h2.secret, h.secret, 32));
    CHECK(!cl_get_hello(p, plen - 1, &h2) && !cl_get_hello(p, plen + 1, &h2));

    n = cl_put_welcome(out, sizeof out, &w); same("welcome", out, n);
    p = payload_of("welcome", CL_WELCOME, &plen);
    CHECK(cl_get_welcome(p, plen, &w2) && w2.version == 2 && w2.flags == CL_WELCOME_E2E && w2.xuid == ME);
    CHECK(!cl_get_welcome(p, plen - 1, &w2) && !cl_get_welcome(p, plen + 1, &w2));

    n = cl_put_bye(out, sizeof out, CL_BYE_PROTOCOL); same("bye", out, n);
    p = payload_of("bye", CL_BYE, &plen);
    CHECK(cl_get_bye(p, plen, &b) && b == 3);
    CHECK(!cl_get_bye(p, 0, &b) && !cl_get_bye(p, 2, &b));

    n = cl_put_open(out, sizeof out, S("club")); same("open", out, n);
    p = payload_of("open", CL_OPEN, &plen);
    CHECK(cl_get_open(p, plen, &str) && is(&str, "club"));
    CHECK(!cl_get_open(p, plen - 1, &str) && !cl_get_open(p, plen + 1, &str));

    n = cl_put_close(out, sizeof out); same("close", out, n);
    p = payload_of("close", CL_CLOSE, &plen);
    CHECK(cl_get_close(p, plen) && !cl_get_close(p, 1));

    n = cl_put_send(out, sizeof out, 7, S(DM), S("ready? \xF0\x9F\x8F\x81")); same("send", out, n);
    p = payload_of("send", CL_SEND, &plen);
    CHECK(cl_get_send(p, plen, &send_m) && send_m.request == 7 && is(&send_m.chat, DM) && is(&send_m.text, "ready? \xF0\x9F\x8F\x81"));
    CHECK(send_m.text.len == 11);
    CHECK(!cl_get_send(p, plen - 1, &send_m) && !cl_get_send(p, plen + 1, &send_m));

    n = cl_put_read(out, sizeof out, S("club"), 42); same("read", out, n);
    p = payload_of("read", CL_READ, &plen);
    CHECK(cl_get_read(p, plen, &read_m) && is(&read_m.chat, "club") && read_m.max_id == 42);
    CHECK(!cl_get_read(p, plen - 1, &read_m) && !cl_get_read(p, plen + 1, &read_m));

    cl_chats_begin(&l, out, sizeof out);
    CHECK(cl_chats_add(&l, S("club"), S("[FHT] FH2MP Testers"), S("ok, meet at the festival hub"), 3, 41, CL_CHAT_CLUB));
    CHECK(cl_chats_add(&l, S(DM), S("DebugFox"), S("echo: hi"), 1, 9, CL_CHAT_E2E));
    n = cl_list_end(&l); same("chats", out, n);
    p = payload_of("chats", CL_CHATS, &plen);
    CHECK(cl_get_chats(p, plen, &e) && e.left == 2);
    CHECK(cl_next_chat(&e, &chat) && is(&chat.id, "club") && is(&chat.title, "[FHT] FH2MP Testers") && is(&chat.last, "ok, meet at the festival hub") &&
          chat.unread == 3 && chat.last_incoming == 41 && chat.flags == 2);
    CHECK(cl_next_chat(&e, &chat) && is(&chat.id, DM) && is(&chat.title, "DebugFox") && is(&chat.last, "echo: hi") && chat.unread == 1 &&
          chat.last_incoming == 9 && chat.flags == 1);
    CHECK(!cl_next_chat(&e, &chat));
    CHECK(!cl_get_chats(p, plen - 1, &e) && !cl_get_chats(p, plen + 1, &e));
    memcpy(mod, p, plen);
    mod[0] = 3;  /* says three, holds two */
    CHECK(!cl_get_chats(mod, plen, &e));
    mod[0] = 1;  /* says one, holds two */
    CHECK(!cl_get_chats(mod, plen, &e));

    cl_messages_begin(&l, out, sizeof out, S(DM));
    CHECK(cl_messages_add(&l, 8, ME, S("You"), S("hi"), 1763520000, CL_MSG_SENT));
    CHECK(cl_messages_add(&l, 9, FOX, S("DebugFox"), S("echo: hi"), 1763520003, CL_MSG_UNREAD));
    n = cl_list_end(&l); same("messages", out, n);
    p = payload_of("messages", CL_MESSAGES, &plen);
    CHECK(cl_get_messages(p, plen, &str, &e) && is(&str, DM) && e.left == 2);
    CHECK(cl_next_message(&e, &msg) && msg.id == 8 && msg.sender == ME && is(&msg.gamertag, "You") && is(&msg.body, "hi") && msg.created == 1763520000 &&
          msg.state == 3);
    CHECK(cl_next_message(&e, &msg) && msg.id == 9 && msg.sender == FOX && is(&msg.gamertag, "DebugFox") && is(&msg.body, "echo: hi") &&
          msg.created == 1763520003 && msg.state == 0);
    CHECK(!cl_next_message(&e, &msg));
    CHECK(!cl_get_messages(p, plen - 1, &str, &e) && !cl_get_messages(p, plen + 1, &str, &e));
    memcpy(mod, p, plen);
    mod[2 + 16] = 3;  /* the count after the chat id: says three, holds two */
    CHECK(!cl_get_messages(mod, plen, &str, &e));

    n = cl_put_sent(out, sizeof out, 7, 0, S("Not friends.")); same("sent", out, n);
    p = payload_of("sent", CL_SENT, &plen);
    CHECK(cl_get_sent(p, plen, &sent_m) && sent_m.request == 7 && sent_m.ok == 0 && is(&sent_m.why, "Not friends."));
    CHECK(!cl_get_sent(p, plen - 1, &sent_m) && !cl_get_sent(p, plen + 1, &sent_m));

    n = cl_put_notice(out, sizeof out, CL_NOTICE_DM, S(DM), S("DebugFox"), S("echo: hi")); same("notice", out, n);
    p = payload_of("notice", CL_NOTICE, &plen);
    CHECK(cl_get_notice(p, plen, &notice_m) && notice_m.kind == 1 && is(&notice_m.chat, DM) && is(&notice_m.title, "DebugFox") && is(&notice_m.text, "echo: hi"));
    CHECK(!cl_get_notice(p, plen - 1, &notice_m) && !cl_get_notice(p, plen + 1, &notice_m));

    /* a str: 8192 bytes are taken, 8193 refused (decoding and encoding) */
    memset(junk, 'x', sizeof junk);
    junk[0] = 0x00, junk[1] = 0x20;  /* 8192 */
    CHECK(cl_get_open(junk, 2 + CL_MAX_STR, &str) && str.len == CL_MAX_STR && str.s[CL_MAX_STR] == 0);
    junk[0] = 0x01;  /* 8193 */
    CHECK(!cl_get_open(junk, 2 + CL_MAX_STR + 1, &str));
    CHECK(cl_put_open(big, sizeof big, (const char *)junk, CL_MAX_STR) == 6 + 2 + CL_MAX_STR);
    CHECK(cl_put_open(big, sizeof big, (const char *)junk, CL_MAX_STR + 1) == 0);
    CHECK(cl_put_send(big, sizeof big, 1, S(DM), (const char *)junk, CL_MAX_STR + 1) == 0);
    CHECK(cl_put_notice(big, sizeof big, 1, S(DM), (const char *)junk, CL_MAX_STR + 1, S("x")) == 0);

    /* no room: one byte short of the frame writes nothing, the exact size writes it */
    CHECK(cl_put_hello(out, 51, &h) == 0 && cl_put_hello(out, 52, &h) == 52);
    CHECK(cl_put_sent(out, 24, 7, 0, S("Not friends.")) == 0 && cl_put_sent(out, 25, 7, 0, S("Not friends.")) == 25);
    cl_chats_begin(&l, out, 7);
    CHECK(cl_list_end(&l) == 0);
    cl_chats_begin(&l, out, 8);
    CHECK(!cl_chats_add(&l, S("club"), S("t"), S("l"), 0, 0, 0) && cl_list_end(&l) == 8);
    CHECK(cl_get_chats(out + 6, 2, &e) && e.left == 0);

    /* a list stops at one frame: the entry that would pass 65536 is refused and the frame stays whole */
    cl_messages_begin(&l, big, sizeof big, S(DM));
    for (i = 0; i < 20; i++) if (!cl_messages_add(&l, i, FOX, S("DebugFox"), (const char *)junk, 4000, 0, 0)) break;
    n = cl_list_end(&l);
    CHECK(i == 16 && l.count == 16 && n <= CL_MAX_FRAME + 4);
    {
        uint16_t type; size_t used;
        CHECK(cl_frame(big, n, &type, &p, &plen, &used) == 1 && used == n && type == CL_MESSAGES);
        CHECK(cl_get_messages(p, plen, &str, &e) && e.left == 16);
    }

    /* cutting text at a UTF-8 boundary: "ready? " is 7 bytes, the flag 4 more */
    CHECK(cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 11) == 11 && cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 20) == 11);
    CHECK(cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 10) == 7 && cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 8) == 7);
    CHECK(cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 7) == 7 && cl_utf8_cut(S("ready? \xF0\x9F\x8F\x81"), 6) == 6);

    /* framing: partial, two frames back to back, an unknown type skipped by its length, impossible lengths */
    {
        uint16_t type; size_t used;
        int c = find("close"), s = find("sent");
        uint8_t two[128];
        memcpy(two, vec[c], vec_len[c]);
        memcpy(two + vec_len[c], vec[s], vec_len[s]);
        CHECK(cl_frame(two, 3, &type, &p, &plen, &used) == 0);
        CHECK(cl_frame(two, vec_len[c] - 1, &type, &p, &plen, &used) == 0);
        CHECK(cl_frame(two, sizeof two, &type, &p, &plen, &used) == 1 && type == CL_CLOSE && plen == 0 && used == vec_len[c]);
        CHECK(cl_frame(two + used, vec_len[s], &type, &p, &plen, &used) == 1 && type == CL_SENT && used == vec_len[s]);
        CHECK(cl_frame(two + vec_len[c], vec_len[s] - 1, &type, &p, &plen, &used) == 0);
        CHECK(cl_frame((const uint8_t *)"\x03\x00\x00\x00\x4d\x00\x09", 7, &type, &p, &plen, &used) == 1 && type == 77 && plen == 1 && used == 7);
        CHECK(cl_frame((const uint8_t *)"\x01\x00\x00\x00\x00\x00", 6, &type, &p, &plen, &used) == -1);
        CHECK(cl_frame((const uint8_t *)"\x01\x00\x01\x00\x00\x00", 6, &type, &p, &plen, &used) == -1);
        CHECK(cl_frame((const uint8_t *)"\x00\x00\x01\x00", 4, &type, &p, &plen, &used) == 0);  /* 65536 is a length that can come */
    }

    if (!failed) puts("chat link: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t" tests/vectors/chat_link.txt
if command -v "${HOSTCXX:-c++}" >/dev/null 2>&1; then
    printf '#include "chat_link_wire.h"\nint main() { struct cl_str s; return cl_get_open((const uint8_t *)"\\0\\0", 2, &s) ? 0 : 1; }\n' > "$tmp/cxx.cpp"
    "${HOSTCXX:-c++}" -Wall -Wextra -Wno-unused-function -I src -o "$tmp/cxx" "$tmp/cxx.cpp" && "$tmp/cxx" && echo "chat link: the header compiles as C++"
else
    echo "chat link: no C++ compiler (${HOSTCXX:-c++}), the header was not compiled as C++"
fi
