/* Which lines of a zipmanifest.xml name a zip the game can't open (crash #99, "0xc0000005 at game+0xfd1927 reading 0x0", 2026-11-15).
 * Plain C without library calls: built into the shim (zipmanifest_guard.inc) and natively into tests/test_zipmanifest.sh.
 *
 * The game reads game:\media\zipmanifest.xml early in its start (FUN_140fd1600, from +0xf64c4e on its main thread) and mounts every
 * <Zip>/<MiniZip> line's zip: the archive open at +0xfd191d (the file system's vtable +0x210, which Extra Options and the platform chain)
 * leaves its result NULL for a zip that isn't there, and +0xfd1927 reads through it [confirmed: dump 20261115T175053-029-8700, the path
 * "game:\media\cars\maz_rx7_85.zip" in the std::string at rbp+0x20; disassembly of build 1.0.0.64]. A car mod's manifest left behind
 * when its zip was removed does that at every start [inferred from the reports: alf_gtv6_86, maz_rx7_85, nis_gtrfast7_12, none of them
 * stock FH2 cars].
 *
 * A line is judged only when it is plainly one zip under game:\media: anything else (another drive, an XML escape, "..", a second element
 * on the line) is kept as it is. */

#define ZM_REL_MAX 240

static int zm_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

/* `n` bytes at `s` begin with `lit` (lit's case for letters ignored when `fold`). */
static int zm_starts(const char *s, unsigned long n, const char *lit, int fold)
{
    unsigned long i;
    for (i = 0; lit[i]; i++) {
        if (i >= n || (fold ? zm_lower((unsigned char)s[i]) != zm_lower((unsigned char)lit[i]) : s[i] != lit[i])) {
            return 0;
        }
    }
    return 1;
}

/* A <Zip> or <MiniZip> line's zip under game:\media, into `rel` as written ("cars\maz_rx7_85.zip", NUL-terminated): 1, or 0 for any
 * other line and for one that can't be judged. `line` is the line without its line break. */
static int zm_line_zip(const char *line, unsigned long n, char *rel, unsigned long cap)
{
    unsigned long i = 0, v, e, k;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    /* the element names the game's loop compares (strcmp with "Zip" and "MiniZip" at +0xfd17ee / +0xfd1803) */
    if (zm_starts(line + i, n - i, "<Zip ", 0)) {
        i += 5;
    } else if (zm_starts(line + i, n - i, "<MiniZip ", 0)) {
        i += 9;
    } else {
        return 0;
    }
    for (k = i; k < n; k++) {
        if (line[k] == '<') {
            return 0;                     /* a second element on the line */
        }
    }
    for (v = i; v + 6 <= n; v++) {
        if ((line[v - 1] == ' ' || line[v - 1] == '\t') && zm_starts(line + v, n - v, "path=\"", 0)) {
            break;
        }
    }
    if (v + 6 > n) {
        return 0;
    }
    v += 6;
    for (e = v; e < n && line[e] != '"'; e++) {
    }
    if (e >= n || !zm_starts(line + v, e - v, "game:\\media\\", 1)) {
        return 0;
    }
    v += 12;
    if (e == v || e - v >= cap || e - v >= ZM_REL_MAX) {
        return 0;
    }
    for (k = v; k < e; k++) {
        const char c = line[k];
        if (c == '&' || c == ':' || c == '/' || c == '<' || c == '>' || (unsigned char)c < 0x20 ||
            (c == '.' && k + 1 < e && line[k + 1] == '.') || (c == '\\' && k + 1 < e && line[k + 1] == '\\')) {
            return 0;
        }
        rel[k - v] = c;
    }
    rel[e - v] = 0;
    return rel[0] != '\\' && rel[e - v - 1] != '\\';
}

typedef int (*zm_present_fn)(const char *rel, void *user);      /* 1 when the game can find game:\media\<rel> */
typedef void (*zm_dropped_fn)(const char *rel, void *user);     /* one line left out */

/* `text` without the lines whose zip `present` says is nowhere, into `out` (room for `len` bytes), every other byte as it was (the BOM,
 * CRLF). -> how many lines were left out; `*zips` counts the lines judged. A text without "</ZipFiles>" (not a whole manifest, or not
 * one at all) is copied as it is. */
static unsigned long zm_filter(const char *text, unsigned long len, char *out, unsigned long *out_len, unsigned long *zips,
                               zm_present_fn present, zm_dropped_fn dropped, void *user)
{
    unsigned long pos = 0, o = 0, left = 0, i;
    char rel[ZM_REL_MAX + 1];
    int whole = 0;

    *zips = 0;
    for (i = 0; i + 11 <= len && !whole; i++) {
        whole = zm_starts(text + i, len - i, "</ZipFiles>", 0);
    }
    while (pos < len) {
        unsigned long end = pos, body;
        while (end < len && text[end] != '\n') {
            end++;
        }
        if (end < len) {
            end++;                                        /* the line with its '\n' */
        }
        body = end - pos;
        while (body && (text[pos + body - 1] == '\n' || text[pos + body - 1] == '\r')) {
            body--;
        }
        if (whole && zm_line_zip(text + pos, body, rel, sizeof rel)) {
            ++*zips;
            if (!present(rel, user)) {
                left++;
                if (dropped) {
                    dropped(rel, user);
                }
                pos = end;
                continue;
            }
        }
        for (i = pos; i < end; i++) {
            out[o++] = text[i];
        }
        pos = end;
    }
    *out_len = o;
    return left;
}

/* Whether a manifest with `left` of its `zips` lines naming zips that are nowhere is changed: a few missing cars are a mod's leftovers;
 * many at once mean the folders looked in aren't the game's (a wrong game folder, a drive not there yet), and the file is left alone. */
static int zm_should_write(unsigned long left, unsigned long zips)
{
    return left > 0 && left <= 16 && left * 10 <= zips;
}
