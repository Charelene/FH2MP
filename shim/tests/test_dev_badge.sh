#!/usr/bin/env bash
# The developer badge's page text (src/dev_badge.inc): only strings, so it runs natively without Wine. The names go into the game's name tag page
# as written, and a page the game can't read stops it while it starts, for every player: so the names are checked here.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "dev_badge.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
#define NAME_ROW(name) name,
static const char *const names[] = { DEV_BADGE_NAMES(NAME_ROW) NULL };
static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (hay = strstr(hay, needle); hay; hay = strstr(hay + 1, needle)) n++;
    return n;
}
int main(void)
{
    size_t i, j;
    char value[64];
    CHECK(names[0] != NULL);                                               /* a badge nobody wears is a page change for nothing */
    CHECK(count(DEV_BADGE_OLD, "Text=\"{Binding UserProfile.FormattedGamertag}\" />\r\n") == 1);
    CHECK(count(DEV_BADGE_NEW, "Style=\"{StaticResource A_Style_OpponentTag}\"\r\n") == 1 &&
          count(DEV_BADGE_NEW, "Text=\"{Binding UserProfile.FormattedGamertag}\" />\r\n") == 1);   /* the tag still shows the game's gamertag, once */
    CHECK(count(DEV_BADGE_NEW, "<Control ") == 1 && count(DEV_BADGE_NEW, "</Control>\r\n") == 1);
    CHECK(!strcmp(DEV_BADGE_NEW + sizeof DEV_BADGE_NEW - 1 - 12, "</Control>\r\n"));
    for (i = 0; i + 1 < sizeof DEV_BADGE_NEW; i++) CHECK(DEV_BADGE_NEW[i] != '\n' || (i && DEV_BADGE_NEW[i - 1] == '\r'));   /* the page's line ends */
    for (i = 0; names[i]; i++) {
        size_t n = strlen(names[i]);
        CHECK(n >= 1 && n <= 15);                                          /* a gamertag */
        CHECK(names[i][0] != ' ' && names[i][n - 1] != ' ');
        for (j = 0; j < n; j++) {
            char c = names[i][j];
            CHECK((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == ' ' && names[i][j + 1] != ' '));
        }
        /* three rules each: the plain name, and the name with a club leader's or a club mod's mark (U+222B, U+222C), which put the plain name back */
        snprintf(value, sizeof value, "Value=\"%s\" />", names[i]);
        CHECK(count(DEV_BADGE_NEW, value) == 3);                           /* one condition, and the text of the two rules with a mark */
        snprintf(value, sizeof value, "Value=\"%s&#x222b;\" />", names[i]);
        CHECK(count(DEV_BADGE_NEW, value) == 1);
        snprintf(value, sizeof value, "Value=\"%s&#x222c;\" />", names[i]);
        CHECK(count(DEV_BADGE_NEW, value) == 1);
    }
    CHECK(count(DEV_BADGE_NEW, "<MultiDataTrigger>") == 3 * (int)i);
    CHECK(count(DEV_BADGE_NEW, "Property=\"Text\"\r\n") == 2 * (int)i);
    CHECK(count(DEV_BADGE_NEW, "Binding=\"{Binding IsDrivatar}\"\r\n") == 3 * (int)i && count(DEV_BADGE_NEW, "Value=\"False\" />") == 3 * (int)i);   /* never on a Drivatar */
    if (!failed) printf("dev badge: all checks pass (%d names)\n", (int)i);
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
