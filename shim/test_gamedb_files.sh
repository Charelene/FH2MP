#!/usr/bin/env bash
# Exercise the actual shim backup/restore functions with Win32 file operations in an isolated Wine prefix.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
export WINEPREFIX="$tmp/prefix" WINEDEBUG=-all
. ./wine_prefix.sh; wine_prefix "$WINEPREFIX"
python3 -I - "$tmp" <<'PY'
import pathlib, sqlite3, sys
d = pathlib.Path(sys.argv[1])
for name in ('server', 'pack', 'new-pack'):
    with sqlite3.connect(d / (name + '.slt')) as c:
        c.execute('CREATE TABLE Data_Car (Id INTEGER PRIMARY KEY, Name TEXT)')
        c.execute('INSERT INTO Data_Car VALUES (1, ?)', (name,))
s = pathlib.Path('src/fh2mp_mod.c').read_text()
start = s.index('static int file_stamp(')
end = s.index('/* The mods (other than FH2MP)', start)
(d / 'production.inc').write_text(s[start:end])
PY
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>
static WCHAR g_exe_dir[MAX_PATH], g_gamedb_path[MAX_PATH], g_gamedb_src[MAX_PATH];
static void *g_mod;
static void quiet(void *m, const char *s) { (void)m; (void)s; }
static struct { WCHAR game_dir[MAX_PATH]; const char *platform_version; void (*log)(void *, const char *); } api;
static __typeof__(api) *g_api = &api;
static void log_begin(void) {}
static void log_raw(const char *s) { (void)s; }
static void log_dec(unsigned long long n) { (void)n; }
#define mem_equal(a,b,n) (memcmp(a,b,n) == 0)
#define mem_copy(a,b,n) memcpy(a,b,n)
#include "production.inc"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s (Win32 %lu)\n",__LINE__,#c,GetLastError()); return 1; } } while (0)
static int equal(const WCHAR *a, const WCHAR *b) { DWORD e; return files_compare(a,b,&e) == 1; }
int main(int argc, char **argv) {
    WCHAR fixtures[MAX_PATH], pack[MAX_PATH], newer[MAX_PATH], kept[MAX_PATH], previous[MAX_PATH], mods[MAX_PATH];
    DWORD err; int unread; HANDLE lock;
    CHECK(argc == 2);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, fixtures, MAX_PATH);
    lstrcpyW(g_exe_dir, fixtures); lstrcatW(g_exe_dir,L"\\azimuth"); CHECK(CreateDirectoryW(g_exe_dir,NULL));
    lstrcpyW(api.game_dir,fixtures); lstrcatW(api.game_dir,L"\\Mount"); CHECK(CreateDirectoryW(api.game_dir,NULL));
    api.platform_version="2.7.2"; api.log=quiet;
    gamedb_dir(); gamedb_game_dirs();
    lstrcpyW(mods,g_exe_dir); lstrcatW(mods,L"\\fh2-mods"); CHECK(CreateDirectoryW(mods,NULL));
    gamedb_file(g_gamedb_src,L"gamedbRC.slt"); gamedb_game_file(g_gamedb_path,L"gamedbRC.slt");
    lstrcpyW(pack,fixtures); lstrcatW(pack,L"\\server.slt"); CHECK(CopyFileW(pack,g_gamedb_src,FALSE));
    lstrcpyW(pack,fixtures); lstrcatW(pack,L"\\pack.slt"); CHECK(CopyFileW(pack,g_gamedb_path,FALSE));
    lstrcpyW(newer,fixtures); lstrcatW(newer,L"\\new-pack.slt");
    gamedb_file(kept,L"gamedb-kept.slt"); gamedb_file(previous,L"gamedb-kept.previous.slt");
    CHECK(gamedb_keep_other(&unread) == 0); CHECK(equal(kept,pack));
    CHECK(gamedb_copy_over(g_gamedb_src,g_gamedb_path) == 0); gamedb_note_placed();
    CHECK(equal(g_gamedb_path,g_gamedb_src)); CHECK(equal(kept,pack));
    gamedb_put_back(); CHECK(equal(g_gamedb_path,pack)); CHECK(equal(kept,pack));
    // A later car-pack replacement is preserved as the new backup; the previous backup survives too.
    CHECK(CopyFileW(newer,g_gamedb_path,FALSE));
    CHECK(gamedb_keep_other(&unread) == 0); CHECK(equal(kept,newer)); CHECK(equal(previous,pack));
    CHECK(gamedb_copy_over(g_gamedb_src,g_gamedb_path) == 0); gamedb_note_placed();
    // A locked destination must remain intact, with the backup still recoverable.
    lock=CreateFileW(g_gamedb_path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    CHECK(lock != INVALID_HANDLE_VALUE); err=gamedb_copy_over(kept,g_gamedb_path); CHECK(err != 0);
    CHECK(equal(g_gamedb_path,g_gamedb_src)); CHECK(equal(kept,newer)); CloseHandle(lock);
    gamedb_put_back(); CHECK(equal(g_gamedb_path,newer)); CHECK(equal(previous,pack));
    puts("gamedb files: online backup, offline restoration, changed packs, retained previous backup and locked destination passed");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I src -I "$tmp" "$tmp/test.c" -o "$tmp/test.exe"
fixtures=$(winepath -w "$tmp")
wine "$tmp/test.exe" "$fixtures"
wineserver -w
