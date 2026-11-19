#!/usr/bin/env bash
# The net watch's decisions (src/net_watch.inc, the part without Windows calls): what the game does with a dispatch result, reading a message id,
# the X-FH2MP-Net value. Expected values come from FH2 1.0.0.64's Connection::ProcessReceivedPackets (game+0x1766100, docs/re/netv2.md
# section 3), the LSB-first bit order of NetV2 and recorded datagrams of native/testdata/game_join_relay.json. Runs natively.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#define NET_WATCH_LOGIC_ONLY
#include "net_watch.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static unsigned hexbytes(const char *hex, unsigned char *out)
{
    unsigned n = 0, v;
    while (hex[0] && hex[1] && sscanf(hex, "%2x", &v) == 1) { out[n++] = (unsigned char)v; hex += 2; }
    return n;
}
int main(void)
{
    unsigned char b[64];
    unsigned v = 99, n;
    char text[96];
    net_watch_close c;

    /* ProcessReceivedPackets: >= 0 goes on; 0x8060000e (keyed event, unknown object) goes on in either state; any other negative result drops the
     * rest of the datagram before the connection is up and disconnects once it is */
    CHECK(net_watch_outcome(0, 0) == NW_OK && net_watch_outcome(17, 1) == NW_OK);
    CHECK(net_watch_outcome((int)0x8060000eu, 0) == NW_SKIPPED && net_watch_outcome((int)0x8060000eu, 1) == NW_SKIPPED);
    CHECK(net_watch_outcome((int)0x80600006u, 0) == NW_DISCONNECT);   /* a Receive stub that refused */
    CHECK(net_watch_outcome((int)0x8060000au, 0) == NW_DISCONNECT);   /* no handler for the id */
    CHECK(net_watch_outcome(-1, 0) == NW_DISCONNECT);
    CHECK(net_watch_outcome((int)0x80600006u, 1) == NW_DATAGRAM_DROPPED && net_watch_outcome((int)0x8060000au, 1) == NW_DATAGRAM_DROPPED);

    CHECK(strstr(net_watch_reason(0x80600008u), "time limit") != NULL);
    CHECK(strstr(net_watch_reason(0x80600006u), "refused") != NULL);
    CHECK(strstr(net_watch_reason(0x8060000au), "no handler") != NULL);
    CHECK(strstr(net_watch_reason(0x8060000fu), "game closed") != NULL);
    CHECK(strstr(net_watch_reason(0x80600021u), "do not name") != NULL);

    /* the first message id after the 88-bit header of three recorded client datagrams: OpenConnection (0), OpenConnectionResponse (1), ObjectCreate (7) */
    n = hexbytes("ffff00000000000001330000109ba1a1981a1923221023b6b0b339b43438020000000000120004000000000000000e00000000", b);
    CHECK(net_watch_bits(b + 11, n - 11, 88, 7, &v) && v == 0);
    n = hexbytes("ffff000000000000011500011c0000001000000000", b);
    CHECK(net_watch_bits(b + 11, n - 11, 88, 7, &v) && v == 1);
    n = hexbytes("ffff000000000000011100870100000000", b);
    CHECK(net_watch_bits(b + 11, n - 11, 88, 7, &v) && v == 7);
    /* an id across a byte boundary, least significant bit first: 32 from bit 5 = bits 5..7 of byte 0 clear, bit 2 of byte 1 set */
    b[0] = 0x00; b[1] = 0x04;
    CHECK(net_watch_bits(b, 2, 5, 7, &v) && v == 32);
    CHECK(net_watch_bits(b, 2, 13, 7, &v) && v == 32);        /* the same bits when the buffer starts at byte 13 / 8 */
    CHECK(!net_watch_bits(b, 1, 5, 7, &v));                   /* not all in the buffer */
    CHECK(!net_watch_bits(b, 8, 0, 33, &v));

    c.seq = 3; c.reason = 0x80600006u; c.msg = 32; c.result = 0x80600006u; c.at = 118; c.size = 600;
    n = net_watch_header_text(text, sizeof text, &c);
    CHECK(!strcmp(text, "v1 seq=3 reason=80600006 msg=32 result=80600006 bits=118/600") && n == strlen(text));
    puts(text);
    c.seq = 1; c.reason = 0x80600008u; c.msg = -1;
    net_watch_header_text(text, sizeof text, &c);
    CHECK(!strcmp(text, "v1 seq=1 reason=80600008"));
    puts(text);
    n = net_watch_header_text(text, 10, &c);                  /* cut, still terminated */
    CHECK(n == 9 && !strcmp(text, "v1 seq=1 "));
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
echo "net watch: all checks pass"
