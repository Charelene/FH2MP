#!/usr/bin/env bash
# The launcher's kill-switch list (src/session_switches.inc): the name parser is pure C, so it runs natively without Wine.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "session_switches.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
/* the list as "name:ok name:ok ..." */
static const char *parse(const char *list)
{
    static char out[512];
    char name[SWITCH_NAME_MAX + 1];
    int ok;
    out[0] = 0;
    while (switch_next(&list, name, &ok)) {
        strcat(out, name);
        strcat(out, ok ? ":1 " : ":0 ");
    }
    return out;
}
int main(void)
{
    CHECK(strcmp(parse("NO_HONK,NO_PARTYHUD"), "NO_HONK:1 NO_PARTYHUD:1 ") == 0);
    CHECK(strcmp(parse(""), "") == 0);
    CHECK(strcmp(parse(",, ,"), "") == 0);                                 /* empty entries are skipped */
    CHECK(strcmp(parse(" NO_HONK , NO_MMTOAST"), "NO_HONK:1 NO_MMTOAST:1 ") == 0);
    CHECK(strcmp(parse("TOKEN"), "TOKEN:0 ") == 0);                       /* only NO_ names: never FH2MP_TOKEN */
    CHECK(strcmp(parse("SERVER_PORT,NO_X"), "SERVER_PORT:0 NO_X:1 ") == 0);
    CHECK(strcmp(parse("NO_"), "NO_:0 ") == 0);                           /* a prefix alone names nothing */
    CHECK(strcmp(parse("no_honk"), "no_honk:0 ") == 0);                   /* capitals only, as the shim's variables are spelt */
    CHECK(strcmp(parse("NO_HONK=1"), "NO_HONK=1:0 ") == 0);               /* no way to smuggle a value */
    CHECK(strcmp(parse("NO_A\\B"), "NO_A\\B:0 ") == 0);
    {
        char longname[80];
        memset(longname, 'A', sizeof longname);
        memcpy(longname, "NO_", 3);
        longname[60] = 0;
        CHECK(strstr(parse(longname), ":0 ") != NULL);                     /* longer than 32 characters: refused, and no overflow */
    }
    if (!failed) puts("session switches: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
