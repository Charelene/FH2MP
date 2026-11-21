#!/usr/bin/env bash
# The developer badge's row mark in the HUD's player list (src/dev_badge_hud.inc) with a stand-in formatter, row and change notice: only a
# developer's row formatted inside party_row's call gets the mark and loses its shield (club role 0). Runs natively, without Wine.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
typedef unsigned short WCHAR;
typedef unsigned long DWORD;
static DWORD tid = 1;
static DWORD GetCurrentThreadId(void) { return tid; }
static unsigned char *find_fh2_image(void) { return NULL; }
static int code_is(const char *a, const char *b, const unsigned char *c, unsigned d, const void *e, unsigned f) { (void)a, (void)b, (void)c, (void)d, (void)e, (void)f; return 1; }
static int plat_hook(const char *a, void *b, void *c, void **d) { (void)a, (void)b, (void)c, (void)d; return 1; }
static void log_begin(void) {}
static void log_raw(const char *s) { (void)s; }
#include "dev_badge.inc"
#include "dev_badge_hud.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static int seen_role;
static int notices;
static char desc;
static void *fake_fmt(void *out, const WCHAR *name, unsigned char marks, int role, void *club, int layout)
{
    (void)name, (void)club;
    CHECK(marks == 3 && layout == 2);            /* passed through as they came */
    seen_role = role;
    return out;
}
static void *fake_desc(void) { return &desc; }
static void fake_notify(void *row, void *d) { (void)row; CHECK(d == &desc); notices++; }
static void wide(WCHAR *w, const char *s) { while ((*w++ = (unsigned char)*s++)) {} }
/* what party_row does around one call to the row writer, which formats `name` and sets TeamID to 0 */
static void fill(unsigned char *row, const char *name, int role)
{
    void *slot = row;
    WCHAR w[32];
    wide(w, name);
    devhud_row_begin();
    devhud_fmt(NULL, w, 3, role, NULL, 2);
    *(int *)(row + 0x54) = 0;
    devhud_row_end(&slot);
}
int main(void)
{
    unsigned char row[0x60] = {0};
    WCHAR w[32];

    g_devhud_fmt_orig = fake_fmt;
    g_devhud_team_desc = fake_desc;
    g_devhud_notify = fake_notify;
    fill(row, "AVALON", 1);                      /* off until the hook is made: nothing happens */
    CHECK(*(int *)(row + 0x54) == 0 && seen_role == 1 && notices == 0);
    g_devhud_on = 1;
    fill(row, "AVALON", 1);
    CHECK(*(int *)(row + 0x54) == DEV_BADGE_HUD_TEAM && seen_role == 0 && notices == 1);   /* marked, shield dropped */
    fill(row, "Charelene", 2);
    CHECK(*(int *)(row + 0x54) == DEV_BADGE_HUD_TEAM && seen_role == 0 && notices == 2);
    fill(row, "Velox", 2);                       /* the same row filled for someone else: the writer's 0 stays, the shield too */
    CHECK(*(int *)(row + 0x54) == 0 && seen_role == 2 && notices == 2);
    fill(row, "AVALON2", 0);                     /* whole names only, and case counts as on the pages */
    CHECK(*(int *)(row + 0x54) == 0);
    fill(row, "avalon", 0);
    CHECK(*(int *)(row + 0x54) == 0);
    fill(row, "AVALO", 0);
    CHECK(*(int *)(row + 0x54) == 0);
    wide(w, "AVALON");                           /* the formatter called from elsewhere, or on another thread: untouched */
    devhud_fmt(NULL, w, 3, 1, NULL, 2);
    CHECK(seen_role == 1 && g_devhud_hit == 0);
    devhud_row_begin();
    tid = 2;
    devhud_fmt(NULL, w, 3, 1, NULL, 2);
    CHECK(seen_role == 1 && g_devhud_hit == 0);
    tid = 1;
    devhud_row_end(NULL);
    CHECK(g_devhud_tid == 0);
    if (!failed) printf("dev badge HUD: all checks pass\n");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
