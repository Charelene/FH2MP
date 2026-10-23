#!/usr/bin/env bash
# Checks src/inflate.inc (the shim's zip reader) against Python's zlib: stored, fixed and dynamic blocks at every level, random and text
# data. With a path to the game's media/stateflow.zip it also inflates global.xml from it. Usage: shim/test_inflate.sh [stateflow.zip]
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void mem_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
#define INFLATE_ONLY
#include "inflate.inc"
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    size_t want = (size_t)atol(argv[2]), n;
    static unsigned char in[1 << 24];
    unsigned char *out = malloc(want + 1);
    n = fread(in, 1, sizeof in, f);
    if (!inflate_raw(in, n, out, want)) { puts("FAIL"); return 1; }
    fwrite(out, 1, want, stdout);
    return 0;
}
C
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
python3 -I - "$tmp" "${1:-}" <<'PY'
import os, random, subprocess, sys, zipfile, zlib
tmp, zp = sys.argv[1], sys.argv[2]
random.seed(7)
words = [b"<state id=", b"node", b"exit", b"target=", b"\r\n    ", b"global.", b"network.", b"behavior"]
cases = [b"", b"a", os.urandom(70000), b"".join(random.choice(words) for _ in range(40000))]
n = 0
for data in cases:
    for level in range(10):
        for strategy in (zlib.Z_DEFAULT_STRATEGY, zlib.Z_FIXED, zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE):
            c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
            raw = c.compress(data) + c.flush()
            open(os.path.join(tmp, "in"), "wb").write(raw)
            got = subprocess.run([os.path.join(tmp, "t"), os.path.join(tmp, "in"), str(len(data))], capture_output=True).stdout
            if got != data:
                sys.exit(f"inflate mismatch: {len(data)} bytes, level {level}, strategy {strategy}")
            n += 1
if zp:
    z = zipfile.ZipFile(zp)
    info = z.getinfo("global.xml")
    with open(zp, "rb") as f:
        f.seek(info.header_offset)
        h = f.read(30)
        f.seek(info.header_offset + 30 + int.from_bytes(h[26:28], "little") + int.from_bytes(h[28:30], "little"))
        open(os.path.join(tmp, "in"), "wb").write(f.read(info.compress_size))
    got = subprocess.run([os.path.join(tmp, "t"), os.path.join(tmp, "in"), str(info.file_size)], capture_output=True).stdout
    if got != z.read("global.xml"):
        sys.exit("inflate mismatch on global.xml")
    n += 1
print(f"inflate: ok ({n} cases)")
PY
