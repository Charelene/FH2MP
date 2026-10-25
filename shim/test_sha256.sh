#!/usr/bin/env bash
# Checks src/sha256.inc (the shim's car database hash) against sha256sum on the padding edge cases and a larger random file, and its
# HMAC (the UDP ticket MAC) against Python's hmac with short and long keys.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "sha256.inc"
int main(int argc, char **argv)
{
    static unsigned char b[997];
    if (argc == 3) { /* hmac <key> <message>: hex digest */
        unsigned char d[32];
        int i;
        hmac_sha256((const unsigned char *)argv[1], strlen(argv[1]), (const unsigned char *)argv[2], strlen(argv[2]), d);
        for (i = 0; i < 32; i++) {
            printf("%02x", d[i]);
        }
        puts("");
        return 0;
    }
    sha256_ctx c;
    char hex[65];
    size_t n;
    FILE *f = fopen(argv[argc - 1], "rb");
    sha256_init(&c);
    while ((n = fread(b, 1, sizeof b, f)) > 0) {
        sha256_update(&c, b, n);
    }
    sha256_final_hex(&c, hex);
    puts(hex);
    return 0;
}
EOF
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
for sz in 0 1 55 56 63 64 65 119 120 1000 1048577; do
  head -c "$sz" /dev/urandom > "$tmp/r"
  [ "$("$tmp/t" "$tmp/r")" = "$(sha256sum "$tmp/r" | cut -d' ' -f1)" ] || { echo "sha256 mismatch at $sz bytes"; exit 1; }
done
for key in k fh2.0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJ; do
  want=$(python3 -I -c 'import hmac,sys; print(hmac.new(sys.argv[1].encode(), sys.argv[2].encode(), "sha256").hexdigest())' "$key" "fh2mp udp ticket v1")
  [ "$("$tmp/t" "$key" "fh2mp udp ticket v1")" = "$want" ] || { echo "hmac mismatch for a ${#key}-byte key"; exit 1; }
done
echo "sha256: ok"
