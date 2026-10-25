#!/usr/bin/env bash
# Checks src/gamedb_keep.inc (keeping another mod's car database while the server's is in mediaoverride, and putting it back offline):
# what mediaoverride's database is taken for, on SQLite files Python makes with and without the platform's mark and on copies cut short,
# what is kept online and put back offline (also once it was removed after being put back), and the list of FH2MP's copies
# (gamedb-placed.txt) written to a file and read back.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
python3 -I - "$tmp" <<'PY'
import os, sqlite3, sys
d = sys.argv[1]
for name, mark in (("plain.slt", 0), ("marked.slt", 0x46483244), ("other-id.slt", 0x46483245)):
    c = sqlite3.connect(os.path.join(d, name))
    c.execute("CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, Name TEXT)")
    c.executemany("INSERT INTO Data_Car VALUES (?, ?)", [(i, "car %d" % i * 20) for i in range(400)])
    c.execute(f"PRAGMA application_id = {mark}")
    c.commit()
    c.close()
with open(os.path.join(d, "plain.slt"), "rb") as f:
    whole = f.read()
assert len(whole) >= 8 * 4096
with open(os.path.join(d, "cut.slt"), "wb") as f:              # a copy stopped half way
    f.write(whole[:len(whole) // 2])
with open(os.path.join(d, "cut-old.slt"), "wb") as f:          # the same, with the page count not kept (SQLite before 3.7.0)
    f.write(whole[:92] + bytes(4) + whole[96:len(whole) // 2])
with open(os.path.join(d, "longer.slt"), "wb") as f:           # more bytes than its pages: still whole
    f.write(whole + bytes(4096))
with open(os.path.join(d, "not-sqlite.slt"), "wb") as f:
    f.write(b"\0" * 68 + b"FH2D" + b"\0" * 100)
with open(os.path.join(d, "short.slt"), "wb") as f:
    f.write(whole[:80])
PY
cat > "$tmp/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "gamedb_keep.inc"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
#define S 10000000ULL  /* 1 s in 100 ns ticks */
#define T0 133950000000000000ULL  /* a date in 2025, whole 10 ms */

static const char *dir;

/* A file's first GK_HEAD bytes, and its size */
static int head(const char *name, unsigned char *h, unsigned long long *size)
{
    char path[1024];
    FILE *f;
    int n;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    n = (int)fread(h, 1, GK_HEAD, f);
    fseek(f, 0, SEEK_END);
    *size = (unsigned long long)ftell(f);
    fclose(f);
    return n;
}

static int whole(const char *name)
{
    unsigned char h[GK_HEAD];
    unsigned long long size;
    int n = head(name, h, &size);
    return n >= 0 && gk_whole(h, n, size);
}

static int classify(gk_stamp t, int same, const gk_stamp *placed, int nplaced, const unsigned char *h, int nh, int have_state,
                    unsigned long long state_time)
{
    gk_facts f;
    f.t = t;
    f.same_as_server = same;
    f.placed = placed;
    f.nplaced = nplaced;
    f.head = h;
    f.nhead = nh;
    f.have_state = have_state;
    f.state_time = state_time;
    return gk_classify(&f);
}

int main(int argc, char **argv)
{
    unsigned char plain[GK_HEAD], marked[GK_HEAD], other_id[GK_HEAD], junk[GK_HEAD], cut[GK_HEAD];
    unsigned long long plain_size, marked_size, other_size, junk_size, cut_size;
    int np, nm, no, nj, nc;
    gk_stamp a, b, list[GK_PLACED_MAX + 1], back[GK_PLACED_MAX];
    char text[200], path[1024], buf[512];
    FILE *f;
    int i, n;

    dir = argv[argc - 1];
    np = head("plain.slt", plain, &plain_size);
    nm = head("marked.slt", marked, &marked_size);
    no = head("other-id.slt", other_id, &other_size);
    nj = head("not-sqlite.slt", junk, &junk_size);
    nc = head("cut.slt", cut, &cut_size);
    a.size = plain_size, a.time = T0;
    b.size = plain_size, b.time = T0 + 3 * S;

    /* the platform's mark, read from real SQLite files */
    CHECK(np == GK_HEAD && nm == GK_HEAD && no == GK_HEAD && nj == GK_HEAD && nc == GK_HEAD);
    CHECK(!gk_marked(plain, np));
    CHECK(gk_marked(marked, nm));
    CHECK(!gk_marked(other_id, no));
    CHECK(!gk_marked(junk, nj));
    CHECK(!gk_marked(marked, 71));

    /* a whole database: its header's page count fits its size, when the header keeps one */
    CHECK(whole("plain.slt") && whole("marked.slt") && whole("longer.slt"));
    CHECK(!whole("cut.slt"));
    CHECK(whole("cut-old.slt"));                  /* no page count to go by */
    CHECK(!whole("not-sqlite.slt") && !whole("short.slt"));
    CHECK(!gk_whole(plain, GK_HEAD - 1, plain_size));
    CHECK(!gk_whole(plain, np, plain_size - 1));
    {
        unsigned char odd[GK_HEAD];
        memcpy(odd, plain, sizeof odd);
        odd[16] = 0x03, odd[17] = 0x00;           /* a page size that isn't a power of 2 */
        CHECK(!gk_whole(odd, np, plain_size));
        odd[16] = 0x00, odd[17] = 0x01;           /* 1 is 65536: the same page count needs more bytes */
        CHECK(gk_whole(odd, np, 65536ULL * gk_be32(odd + 28)));
        CHECK(!gk_whole(odd, np, 65536ULL * gk_be32(odd + 28) - 1));
    }

    /* a stamp R lists: the same size, and the same date, or 2 s off at most when T's is whole 10 ms */
    {
        gk_stamp s = {100, T0}, t = s;
        CHECK(gk_nearly(&s, &t));
        t.size = 101;
        CHECK(!gk_nearly(&s, &t));
        t.size = 100, t.time = T0 + 2 * S;
        CHECK(gk_nearly(&s, &t));
        t.time = T0 - 2 * S;
        CHECK(gk_nearly(&s, &t));
        t.time = T0 + 2 * S + GK_COARSE;
        CHECK(!gk_nearly(&s, &t));
        t.time = T0 + 15000000ULL + 1;         /* 1.5 s off, a finer date (NTFS): not the same */
        CHECK(!gk_nearly(&s, &t));
        s.time = T0 + 1, t.time = T0 + 1;      /* a finer date that is the same */
        CHECK(gk_nearly(&s, &t));
    }

    /* 1. FH2MP's copy, before anything else */
    CHECK(classify(a, 1, NULL, 0, marked, nm, 1, a.time) == GK_FH2MP);
    CHECK(classify(a, 0, &a, 1, plain, np, 0, 0) == GK_FH2MP);
    CHECK(classify(a, 1, NULL, 0, cut, nc, 0, 0) == GK_FH2MP);
    {
        gk_stamp coarse = {a.size, a.time + S}, fine = {a.size, a.time + S + 7}, lists[3] = {{1, 1}, {2, 2}, {0, 0}};
        CHECK(classify(coarse, 0, &a, 1, plain, np, 0, 0) == GK_FH2MP);
        CHECK(classify(fine, 0, &a, 1, plain, np, 0, 0) == GK_OTHER);
        CHECK(classify(a, 0, &b, 1, plain, np, 0, 0) == GK_OTHER);
        lists[2] = a;                          /* listed in the shared list or the platform's, after FH2MP's own */
        CHECK(classify(a, 0, lists, 3, plain, np, 0, 0) == GK_FH2MP);
    }
    /* 2. not a whole database: a copy cut short, or not SQLite at all */
    {
        gk_stamp c = {cut_size, T0};
        CHECK(classify(c, 0, NULL, 0, cut, nc, 0, 0) == GK_BROKEN);
        CHECK(classify(c, 0, &c, 1, cut, nc, 0, 0) == GK_FH2MP);
        CHECK(classify(a, 0, NULL, 0, cut, nc, 0, 0) == GK_OTHER);    /* the header alone, with the size it says: whole */
    }
    CHECK(classify(a, 0, NULL, 0, junk, nj, 0, 0) == GK_BROKEN);
    CHECK(classify(a, 0, NULL, 0, plain, 0, 0, 0) == GK_BROKEN);
    {
        gk_stamp short_t = {plain_size / 2, T0};
        CHECK(classify(short_t, 0, NULL, 0, marked, nm, 0, 0) == GK_BROKEN);
    }
    /* 3. a platform build: marked (2.7.2 on, FH2ML), or dated from 10 s before an older platform's platform-db.txt to 2 s after */
    CHECK(classify(a, 0, &b, 1, marked, nm, 0, 0) == GK_PLATFORM);
    CHECK(classify(a, 0, NULL, 0, plain, np, 1, a.time + 7 * 10000ULL) == GK_PLATFORM);
    CHECK(classify(a, 0, NULL, 0, plain, np, 1, a.time + 10 * S) == GK_PLATFORM);
    CHECK(classify(a, 0, NULL, 0, plain, np, 1, a.time + 10 * S + 1) == GK_OTHER);
    CHECK(classify(a, 0, NULL, 0, plain, np, 1, a.time - 2 * S) == GK_PLATFORM);
    CHECK(classify(a, 0, NULL, 0, plain, np, 1, a.time - 2 * S - 1) == GK_OTHER);
    CHECK(classify(a, 0, NULL, 0, plain, np, 0, a.time) == GK_OTHER);
    /* 4. another mod's database */
    CHECK(classify(a, 0, NULL, 0, plain, np, 0, 0) == GK_OTHER);
    CHECK(classify(a, 0, NULL, 0, other_id, no, 0, 0) == GK_OTHER);

    /* online: only another mod's database is kept, once */
    CHECK(gk_online(GK_FH2MP, 0, 0) == GK_NOTHING);
    CHECK(gk_online(GK_BROKEN, 0, 0) == GK_NOTHING);
    CHECK(gk_online(GK_BROKEN, 1, 0) == GK_NOTHING);
    CHECK(gk_online(GK_PLATFORM, 1, 0) == GK_NOTHING);
    CHECK(gk_online(GK_OTHER, 0, 0) == GK_KEEP);
    CHECK(gk_online(GK_OTHER, 1, 1) == GK_NOTHING);
    CHECK(gk_online(GK_OTHER, 1, 0) == GK_KEEP_REPLACING);

    /* the platform keeps what it builds on from 2.7.2 on */
    CHECK(gk_platform_keeps("2.7.2") && gk_platform_keeps("2.7.10") && gk_platform_keeps("2.8.0") && gk_platform_keeps("3.0.0"));
    CHECK(gk_platform_keeps("2.7.2-beta") && gk_platform_keeps("10"));
    CHECK(!gk_platform_keeps("2.7.1") && !gk_platform_keeps("2.6.9") && !gk_platform_keeps("1.9.9") && !gk_platform_keeps("2.7"));
    CHECK(!gk_platform_keeps("") && !gk_platform_keeps(0) && !gk_platform_keeps("x2.7.2") && !gk_platform_keeps("2x7.2"));

    /* T missing: removed after K was put back, with no older platform's copy built over it since, means that mod is gone */
    CHECK(gk_removed(1, 0, 0) && gk_removed(1, 0, 1));
    CHECK(!gk_removed(1, 1, 0));
    CHECK(gk_removed(1, 1, 1));                 /* 2.7.2 on: the platform keeps what it built on K, and decides */
    CHECK(!gk_removed(0, 0, 0) && !gk_removed(0, 1, 0) && !gk_removed(0, 0, 1) && !gk_removed(0, 1, 1));

    /* offline, with K there; up to 2.7.1, then from 2.7.2 on where that differs */
    CHECK(gk_offline(0, GK_OTHER, 0, 0, 0, 0) == GK_PUT_BACK);      /* FH2MP's copy was last there, and was removed */
    CHECK(gk_offline(0, GK_OTHER, 0, 0, 1, 0) == GK_PUT_BACK);
    CHECK(gk_offline(0, GK_OTHER, 0, 0, 1, 1) == GK_PUT_BACK);
    CHECK(gk_offline(0, GK_OTHER, 0, 1, 1, 0) == GK_PUT_BACK);      /* 2.7.1's copy built over K was removed */
    CHECK(gk_offline(0, GK_OTHER, 0, 1, 1, 1) == GK_RETIRE_KEPT);   /* 2.7.2's, built on K: the platform puts it back, or not */
    CHECK(gk_offline(0, GK_OTHER, 0, 1, 0, 0) == GK_RETIRE_KEPT);   /* K itself was removed: that mod is gone */
    CHECK(gk_offline(0, GK_OTHER, 0, 1, 0, 1) == GK_RETIRE_KEPT);
    CHECK(gk_offline(1, GK_FH2MP, 0, 0, 0, 0) == GK_PUT_BACK);
    CHECK(gk_offline(1, GK_FH2MP, 0, 1, 0, 1) == GK_PUT_BACK);
    CHECK(gk_offline(1, GK_BROKEN, 0, 0, 0, 0) == GK_PUT_BACK);
    CHECK(gk_offline(1, GK_OTHER, 1, 0, 0, 0) == GK_IS_BACK);       /* K is kept: an older platform may still build over T */
    CHECK(gk_offline(1, GK_OTHER, 1, 1, 1, 1) == GK_IS_BACK);
    CHECK(gk_offline(1, GK_PLATFORM, 1, 0, 1, 0) == GK_IS_BACK);    /* K's bytes count before a platform-db.txt date */
    CHECK(gk_offline(1, GK_PLATFORM, 0, 1, 1, 0) == GK_NOT_BACK);   /* 2.7.1 built over K from the game's own database */
    CHECK(gk_offline(1, GK_PLATFORM, 0, 0, 1, 0) == GK_NOT_BACK);
    CHECK(gk_offline(1, GK_PLATFORM, 0, 1, 1, 1) == GK_LEAVE);      /* 2.7.2 built on K, and keeps it */
    CHECK(gk_offline(1, GK_OTHER, 0, 0, 0, 0) == GK_RETIRE_KEPT);
    CHECK(gk_offline(1, GK_OTHER, 0, 1, 0, 1) == GK_RETIRE_KEPT);

    /* R: newest last, a repeat moves to the end, the last 4 are kept; written to a file and read back */
    n = 0;
    for (i = 0; i < 6; i++) {
        gk_stamp s = {1000 + (unsigned)i, T0 + (unsigned long long)i * S};
        n = gk_placed_add(list, n, &s);
    }
    CHECK(n == 4 && list[0].size == 1002 && list[3].size == 1005);
    {
        gk_stamp again = list[1];
        n = gk_placed_add(list, n, &again);
        CHECK(n == 4 && list[0].size == 1002 && list[1].size == 1004 && list[2].size == 1005 && list[3].size == 1003);
    }
    gk_placed_format(list, n, text);
    snprintf(path, sizeof path, "%s/gamedb-placed.txt", dir);
    f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
    f = fopen(path, "rb");
    i = (int)fread(buf, 1, sizeof buf, f);
    fclose(f);
    CHECK(gk_placed_parse(buf, i, back) == 4);
    for (i = 0; i < 4; i++) {
        CHECK(back[i].size == list[i].size && back[i].time == list[i].time);
    }
    CHECK(strncmp(text, "1002 133950000020000000\n", 24) == 0);
    {
        static const char *bad = "12 34\r\nx 1\n\n5 6 7\n 8 9\n10 \n18446744073709551615 1\n11 12";
        n = gk_placed_parse(bad, (int)strlen(bad), back);
        CHECK(n == 2 && back[0].size == 12 && back[0].time == 34 && back[1].size == 11 && back[1].time == 12);
        n = gk_placed_parse("1 2\n3 4\n5 6\n7 8\n9 10\n", 21, back);
        CHECK(n == 4 && back[0].size == 3 && back[3].size == 9);
        CHECK(gk_placed_parse("", 0, back) == 0);
    }
    /* B, one stamp: what gk_placed_format writes for it reads back */
    n = gk_placed_format(&a, 1, text);
    CHECK(gk_placed_parse(text, n, back) == 1 && back[0].size == a.size && back[0].time == a.time);
    if (fails) {
        return 1;
    }
    puts("gamedb_keep: ok");
    return 0;
}
C
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t" "$tmp"
