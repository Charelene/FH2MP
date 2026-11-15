#!/usr/bin/env bash
# The zip manifest check (src/zipmanifest_rule.inc, crash #99 at game+0xfd1927), natively: which lines name a zip under game:\media, which
# are taken out when their zip is nowhere, that every other byte (BOM, CRLF, a last line without a break) stays, what is kept because it
# can't be judged, and when a file is left alone because too many of its zips are missing.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "zipmanifest_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

/* Present: every zip but the ones named "gone" */
static int present(const char *rel, void *user)
{
    (void)user;
    return strstr(rel, "gone") == NULL && strcmp(rel, "cars\\maz_rx7_85.zip") != 0;
}

static char seen[8][ZM_REL_MAX + 1];
static int seen_n;
static void dropped(const char *rel, void *user)
{
    (void)user;
    if (seen_n < 8) strcpy(seen[seen_n++], rel);
}

static int line_zip(const char *line, char *rel)
{
    return zm_line_zip(line, (unsigned long)strlen(line), rel, ZM_REL_MAX + 1);
}

int main(void)
{
    char rel[ZM_REL_MAX + 1], out[4096];
    unsigned long out_len = 0, zips = 0, left;
    static const char head[] = "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<ZipFiles version=\"3\">\r\n";
    static const char stock1[] = "  <Zip version=\"1\" path=\"game:\\media\\cars\\maz_rx7_97.zip\" priority=\"80\" dirstart=\"20287363\" dirsize=\"36960\" direntries=\"354\" />\r\n";
    static const char dead[] = "  <Zip version=\"1\" path=\"game:\\media\\cars\\maz_rx7_85.zip\" priority=\"80\" dirstart=\"1\" dirsize=\"2\" direntries=\"3\" />\r\n";
    static const char stock2[] = "  <Zip version=\"1\" path=\"game:\\media\\ui\\textures\\data_bound\\cu2.zip\" priority=\"80\" dirstart=\"10837\" dirsize=\"325\" direntries=\"4\" />\r\n";
    static const char tail[] = "</ZipFiles>";      /* no line break after it, as some editors save it */
    char text[4096], want[4096];

    /* the line of the crash dump: its zip, as written */
    CHECK(line_zip("  <Zip version=\"1\" path=\"game:\\media\\cars\\maz_rx7_85.zip\" priority=\"80\" />", rel) && !strcmp(rel, "cars\\maz_rx7_85.zip"));
    CHECK(line_zip("\t<MiniZip path=\"game:\\media\\stripped\\rc3.zip\" />", rel) && !strcmp(rel, "stripped\\rc3.zip"));
    CHECK(line_zip("<Zip path=\"GAME:\\Media\\cars\\x.zip\"/>", rel) && !strcmp(rel, "cars\\x.zip"));     /* the drive's case */
    /* not judged: other elements, other drives, escapes, "..", a second element, no closing quote, nothing after media\ */
    CHECK(!line_zip("<ZipFiles version=\"3\">", rel));
    CHECK(!line_zip("  <zip version=\"1\" path=\"game:\\media\\cars\\gone.zip\" />", rel));       /* the game compares "Zip" exactly */
    CHECK(!line_zip("  <Zip path=\"T:\\fh2-platform\\packs\\a\\cars\\gone.zip\" />", rel));
    CHECK(!line_zip("  <Zip path=\"game:\\media\\cars\\a&amp;b_gone.zip\" />", rel));
    CHECK(!line_zip("  <Zip path=\"game:\\media\\..\\gone.zip\" />", rel));
    CHECK(!line_zip("  <Zip path=\"game:\\media\\cars\\gone.zip\" /><Zip path=\"game:\\media\\cars\\b.zip\" />", rel));
    CHECK(!line_zip("  <Zip path=\"game:\\media\\cars\\gone.zip />", rel));
    CHECK(!line_zip("  <Zip path=\"game:\\media\\\" />", rel));
    CHECK(!line_zip("  <Zip xpath=\"game:\\media\\cars\\gone.zip\" />", rel));                 /* another attribute's name */
    CHECK(!line_zip("  <Zip path=\"game:\\media\\cars/gone.zip\" />", rel));

    /* one dead line among stock ones: only it goes, every other byte stays */
    snprintf(text, sizeof text, "%s%s%s%s%s", head, stock1, dead, stock2, tail);
    snprintf(want, sizeof want, "%s%s%s%s", head, stock1, stock2, tail);
    left = zm_filter(text, (unsigned long)strlen(text), out, &out_len, &zips, present, dropped, NULL);
    CHECK(left == 1 && zips == 3 && seen_n == 1 && !strcmp(seen[0], "cars\\maz_rx7_85.zip"));
    CHECK(out_len == strlen(want) && !memcmp(out, want, out_len));

    /* all present: a copy of the text */
    seen_n = 0;
    snprintf(text, sizeof text, "%s%s%s%s", head, stock1, stock2, tail);
    left = zm_filter(text, (unsigned long)strlen(text), out, &out_len, &zips, present, dropped, NULL);
    CHECK(left == 0 && zips == 2 && seen_n == 0 && out_len == strlen(text) && !memcmp(out, text, out_len));

    /* LF only, and the dead line last, without a break */
    snprintf(text, sizeof text, "<ZipFiles>\n<Zip path=\"game:\\media\\cars\\a.zip\" />\n</ZipFiles>\n<Zip path=\"game:\\media\\cars\\gone.zip\" />");
    left = zm_filter(text, (unsigned long)strlen(text), out, &out_len, &zips, present, NULL, NULL);
    CHECK(left == 1 && out_len == strlen("<ZipFiles>\n<Zip path=\"game:\\media\\cars\\a.zip\" />\n</ZipFiles>\n") &&
          !memcmp(out, "<ZipFiles>\n<Zip path=\"game:\\media\\cars\\a.zip\" />\n</ZipFiles>\n", out_len));

    /* not a whole manifest (cut short): copied as it is, nothing judged */
    snprintf(text, sizeof text, "%s%s%s", head, stock1, dead);
    left = zm_filter(text, (unsigned long)strlen(text), out, &out_len, &zips, present, NULL, NULL);
    CHECK(left == 0 && zips == 0 && out_len == strlen(text) && !memcmp(out, text, out_len));

    /* lines that can't be judged stay even with a zip that is "gone" */
    snprintf(text, sizeof text, "%s  <Zip path=\"T:\\fh2-platform\\gone.zip\" />\r\n  <Zip path=\"game:\\media\\a&amp;gone.zip\" />\r\n%s", head, tail);
    left = zm_filter(text, (unsigned long)strlen(text), out, &out_len, &zips, present, NULL, NULL);
    CHECK(left == 0 && zips == 0 && out_len == strlen(text));

    /* when a file is written: a few missing of many, never more than 16 or a tenth */
    CHECK(zm_should_write(1, 609));
    CHECK(zm_should_write(16, 609));
    CHECK(!zm_should_write(17, 609));
    CHECK(!zm_should_write(0, 609));
    CHECK(zm_should_write(1, 10));
    CHECK(!zm_should_write(2, 10));
    CHECK(!zm_should_write(1, 3));       /* a manifest of 3 zips with one missing: the folders, more likely */
    CHECK(!zm_should_write(608, 608));   /* a wrong game folder: everything "missing" */

    if (!failed) puts("zip manifest check: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
