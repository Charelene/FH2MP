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
    /* the lists: the player list's and the leaderboards' gamertag line, each turned into the same Control with name-only rules */
    {
        const char *news[2] = {DEV_BADGE_LIST_NEW, DEV_BADGE_BOARD_NEW}, *olds[2] = {DEV_BADGE_LIST_OLD, DEV_BADGE_BOARD_OLD};
        const char *styles[2] = {"Style=\"{StaticResource A_Style_FZTextBlock_PlayerList}\"", "Style=\"{StaticResource A_Style_FZTextBlock_Primary}\""};
        int p;
        for (p = 0; p < 2; p++) {
            size_t len = strlen(news[p]), k;
            CHECK(count(olds[p], "Text=\"{Binding UserProfile.FormattedGamertag}\"") == 1 && count(olds[p], styles[p]) == 1 && count(olds[p], "\r\n") == 1);
            CHECK(count(news[p], "Text=\"{Binding UserProfile.FormattedGamertag}\"") == 1 && count(news[p], styles[p]) == 1);
            CHECK(count(news[p], "<Control ") == 1 && !strcmp(news[p] + len - 12, "</Control>\r\n"));
            CHECK(count(news[p], "<Grid x:Name=\"FH2MP_DevBadge\" Width=\"17\" Height=\"17\"") == 1 && count(news[p], "Visibility=\"Collapsed\"") == 1);
            CHECK(count(news[p], "Viewbox") == 0);                         /* a Viewbox around the shapes drew nothing in the game */
            CHECK(count(news[p], "x:Name=\"PlayerName\"") == 0);
            for (k = 0; k < len; k++) CHECK(news[p][k] != '\n' || (k && news[p][k - 1] == '\r'));
            for (j = 0; names[j]; j++) {
                snprintf(value, sizeof value, "Value=\"%s\"", names[j]);
                CHECK(count(news[p], value) == 3);                         /* the plain name's rule, and the text the two rules with a mark set */
                snprintf(value, sizeof value, "Value=\"%s&#x222b;\">", names[j]);
                CHECK(count(news[p], value) == 1);
                snprintf(value, sizeof value, "Value=\"%s&#x222c;\">", names[j]);
                CHECK(count(news[p], value) == 1);
            }
            CHECK(count(news[p], "<DataTrigger ") == 3 * (int)i && count(news[p], "IsDrivatar") == 0);
        }
    }
    /* the HUD's player list: one rule on the row's TeamID, the value dev_badge_hud.inc writes; the template's other rules kept */
    snprintf(value, sizeof value, "<DataTrigger Binding=\"{Binding TeamID}\" Value=\"%d\">", DEV_BADGE_HUD_TEAM);
    CHECK(DEV_BADGE_HUD_TEAM != 0 && DEV_BADGE_HUD_TEAM != 1 && count(DEV_BADGE_HUD_NEW, value) == 1 && count(DEV_BADGE_HUD_NEW, "TeamID") == 1);
    CHECK(count(DEV_BADGE_HUD_OLD, "IsInPartyWithLocalPlayer") == 1 && count(DEV_BADGE_HUD_NEW, "IsInPartyWithLocalPlayer") == 1);
    CHECK(count(DEV_BADGE_HUD_NEW, "Text=\"{Binding Gamertag}\"") == 1 && count(DEV_BADGE_HUD_NEW, "<Grid x:Name=\"FH2MP_DevBadge\" Width=\"20\" Height=\"20\"") == 1);
    CHECK(count(DEV_BADGE_HUD_NEW, "Visibility=\"Collapsed\"") == 1 && count(DEV_BADGE_HUD_NEW, "Viewbox") == 0);
    for (j = 0; j < sizeof DEV_BADGE_HUD_NEW - 1; j++) CHECK(DEV_BADGE_HUD_NEW[j] != '\n' || (j && DEV_BADGE_HUD_NEW[j - 1] == '\r'));
    CHECK(count(DEV_BADGE_NEW, "<MultiDataTrigger>") == 3 * (int)i);
    CHECK(count(DEV_BADGE_NEW, "Property=\"Text\"\r\n") == 2 * (int)i);
    CHECK(count(DEV_BADGE_NEW, "Binding=\"{Binding IsDrivatar}\"\r\n") == 3 * (int)i && count(DEV_BADGE_NEW, "Value=\"False\" />") == 3 * (int)i);   /* never on a Drivatar */
    if (!failed) printf("dev badge: all checks pass (%d names)\n", (int)i);
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
