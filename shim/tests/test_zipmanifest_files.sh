#!/usr/bin/env bash
# The zip manifest check's file side (src/zipmanifest_guard.inc) on a game folder of its own: a mediaoverride manifest with a line whose zip
# is nowhere loses that line and keeps a .bak; zips in mediaoverride, an Extra Options mod (both layouts), a car pack and an in-zip override
# folder count as there; a mediapc manifest with too many missing zips and the stock one are left alone; a second start changes nothing.
# Native on Windows, under Wine elsewhere.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>
typedef struct FH2Mod FH2Mod;
static FH2Mod *g_mod;
static char g_platform_log[8192], g_shim_log[16384];
static void plat_log(FH2Mod *m, const char *s) { (void)m; strncat(g_platform_log, s, sizeof g_platform_log - strlen(g_platform_log) - 2); strcat(g_platform_log, "\n"); }
static struct { WCHAR game_dir[MAX_PATH]; WCHAR azimuth_dir[MAX_PATH]; void (*log)(FH2Mod *, const char *); } api;
static struct { const WCHAR *game_dir; const WCHAR *azimuth_dir; void (*log)(FH2Mod *, const char *); } view;
static __typeof__(view) *g_api = &view;
static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_shim_log, s, sizeof g_shim_log - strlen(g_shim_log) - 1); }
static void log_dec(unsigned long n) { char b[16]; sprintf(b, "%lu", n); log_raw(b); }
static void log_wide(const wchar_t *w) { char b[1024]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, NULL, NULL); log_raw(b); }
#include "zipmanifest_guard.inc"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s (Win32 %lu)\nshim log:\n%s\nplatform log:\n%s\n", __LINE__, #c, GetLastError(), g_shim_log, g_platform_log); return 1; } } while (0)

static WCHAR root[MAX_PATH];

static void path(WCHAR *out, const WCHAR *rel) { lstrcpyW(out, root); lstrcatW(out, L"\\"); lstrcatW(out, rel); }

static void mkdirs(const WCHAR *rel)
{
    WCHAR p[MAX_PATH];
    int i;
    path(p, rel);
    for (i = lstrlenW(root) + 1; p[i]; i++) {
        if (p[i] == L'\\') { p[i] = 0; CreateDirectoryW(p, NULL); p[i] = L'\\'; }
    }
    CreateDirectoryW(p, NULL);
}

static int put(const WCHAR *rel, const char *data)
{
    WCHAR p[MAX_PATH];
    DWORD n;
    HANDLE h;
    path(p, rel);
    h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, data, (DWORD)strlen(data), &n, NULL);
    CloseHandle(h);
    return 1;
}

static int get(const WCHAR *rel, char *buf, DWORD cap)
{
    WCHAR p[MAX_PATH];
    DWORD n = 0;
    HANDLE h;
    path(p, rel);
    h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    ReadFile(h, buf, cap - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    return (int)n;
}

/* How many zipmanifest.xml.fh2mp-*.bak are in `dir`; the last one's name in `last`. */
static int baks(const WCHAR *dir, WCHAR *last)
{
    WCHAR p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    path(p, dir);
    lstrcatW(p, L"\\zipmanifest.xml.fh2mp-*.bak");
    h = FindFirstFileW(p, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do { n++; if (last) { lstrcpyW(last, dir); lstrcatW(last, L"\\"); lstrcatW(last, fd.cFileName); } } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void manifest(char *out, const char *const *zips)
{
    strcpy(out, "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<ZipFiles version=\"3\">\r\n");
    for (; *zips; zips++) {
        strcat(out, "  <Zip version=\"1\" path=\"game:\\media\\cars\\");
        strcat(out, *zips);
        strcat(out, "\" priority=\"80\" dirstart=\"1\" dirsize=\"2\" direntries=\"3\" />\r\n");
    }
    strcat(out, "</ZipFiles>\r\n");
}

int main(int argc, char **argv)
{
    static const char *const stock[] = {"a.zip", "b.zip", "c.zip", "d.zip", "e.zip", "f.zip", "g.zip", "h.zip", "i.zip", "j.zip", 0};
    static const char *const ovr[] = {"a.zip", "b.zip", "c.zip", "d.zip", "e.zip", "f.zip", "g.zip", "h.zip", "maz_rx7_85.zip",
                                      "eo.zip", "old.zip", "pack.zip", "inzip.zip", "ov.zip", 0};
    static const char *const ovr_after[] = {"a.zip", "b.zip", "c.zip", "d.zip", "e.zip", "f.zip", "g.zip", "h.zip",
                                            "eo.zip", "old.zip", "pack.zip", "inzip.zip", "ov.zip", 0};
    static const char *const pc[] = {"a.zip", "b.zip", "c.zip", "d.zip", "e.zip", "f.zip", "g.zip", "gone1.zip", "gone2.zip", 0};
    static char text[16384], want[16384], got[16384], before[16384];
    WCHAR last[MAX_PATH];
    int i;

    CHECK(argc == 2);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, root, MAX_PATH);
    path(api.game_dir, L"Mount");
    path(api.azimuth_dir, L"azimuth");
    view.game_dir = api.game_dir;
    view.azimuth_dir = api.azimuth_dir;
    view.log = plat_log;

    mkdirs(L"Mount\\media\\cars");
    for (i = 0; stock[i]; i++) {
        WCHAR rel[MAX_PATH];
        wsprintfW(rel, L"Mount\\media\\cars\\%S", stock[i]);
        CHECK(put(rel, "PK"));
    }
    manifest(text, stock);
    CHECK(put(L"Mount\\media\\zipmanifest.xml", text));
    mkdirs(L"Mount\\mediaoverride\\cars");
    CHECK(put(L"Mount\\mediaoverride\\cars\\ov.zip", "PK"));
    mkdirs(L"Mount\\mediapc\\cars\\inzip");                          /* Extra Options' in-zip override folder */
    mkdirs(L"azimuth\\mods\\Forza Horizon 2\\Some Car\\media\\cars");
    CHECK(put(L"azimuth\\mods\\Forza Horizon 2\\Some Car\\media\\cars\\eo.zip", "PK"));
    mkdirs(L"azimuth\\mods\\Old Mod\\media\\cars");                  /* the 0.4 layout */
    CHECK(put(L"azimuth\\mods\\Old Mod\\media\\cars\\old.zip", "PK"));
    mkdirs(L"azimuth\\fh2-cars\\My Pack\\media\\cars");
    CHECK(put(L"azimuth\\fh2-cars\\My Pack\\media\\cars\\pack.zip", "PK"));
    manifest(before, ovr);
    CHECK(put(L"Mount\\mediaoverride\\zipmanifest.xml", before));
    manifest(text, pc);
    CHECK(put(L"Mount\\mediapc\\zipmanifest.xml", text));

    zipmanifest_check();

    /* mediaoverride: only the RX-7 line went, the file before is kept */
    manifest(want, ovr_after);
    CHECK(get(L"Mount\\mediaoverride\\zipmanifest.xml", got, sizeof got) == (int)strlen(want) && !strcmp(got, want));
    CHECK(baks(L"Mount\\mediaoverride", last) == 1);
    CHECK(get(last, got, sizeof got) == (int)strlen(before) && !strcmp(got, before));
    CHECK(strstr(g_platform_log, "zipguard: took cars\\maz_rx7_85.zip out of ") && strstr(g_platform_log, "mediaoverride\\zipmanifest.xml") &&
          strstr(g_platform_log, "(the file before: zipmanifest.xml.fh2mp-"));
    CHECK(strstr(g_shim_log, "lists game:\\media\\cars\\maz_rx7_85.zip, which is in none of the folders"));
    CHECK(strstr(g_shim_log, "took 1 of 14 zip line(s) out of "));
    /* mediapc: 2 of 9 missing is past the cap, left as it is and said so */
    CHECK(get(L"Mount\\mediapc\\zipmanifest.xml", got, sizeof got) == (int)strlen(text) && !strcmp(got, text));
    CHECK(baks(L"Mount\\mediapc", NULL) == 0);
    CHECK(strstr(g_shim_log, "mediapc\\zipmanifest.xml lists 2 of its 9 zips in none of the game's folders"));
    /* the stock manifest: nothing missing, untouched */
    CHECK(baks(L"Mount\\media", NULL) == 0);
    CHECK(!strstr(g_shim_log, "media\\zipmanifest.xml lists game"));
    /* no temporary file left */
    {
        WCHAR p[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        path(p, L"Mount\\mediaoverride\\*.fh2mp-tmp-*");
        h = FindFirstFileW(p, &fd);
        CHECK(h == INVALID_HANDLE_VALUE);
    }

    /* the next start: nothing to take out, no second copy, quiet */
    g_platform_log[0] = g_shim_log[0] = 0;
    zipmanifest_check();
    CHECK(baks(L"Mount\\mediaoverride", NULL) == 1);
    CHECK(get(L"Mount\\mediaoverride\\zipmanifest.xml", got, sizeof got) == (int)strlen(want) && !strcmp(got, want));
    CHECK(!g_platform_log[0]);
    CHECK(!strstr(g_shim_log, "took"));

    /* a game folder without media: nothing read, said once */
    g_platform_log[0] = g_shim_log[0] = 0;
    path(api.game_dir, L"NoSuchMount");
    zipmanifest_check();
    CHECK(strstr(g_shim_log, "has no media folder, so its zip manifests were not checked"));

    puts("zip manifest files: a dead line taken out with a copy kept, zips in every folder found, the cap and a second start passed");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I src "$tmp/test.c" -o "$tmp/test.exe"
case "$(uname -s)" in
MINGW*|MSYS*|CYGWIN*)
    "$tmp/test.exe" "$(cygpath -w "$tmp")"
    ;;
*)
    export WINEPREFIX="$tmp/prefix" WINEDEBUG=-all
    . tests/wine_prefix.sh; wine_prefix "$WINEPREFIX"
    wine "$tmp/test.exe" "$(winepath -w "$tmp")"
    wineserver -w
    ;;
esac
