#!/usr/bin/env bash
# Test platform registration and actual process detection, isolated from the user's game/Wine prefix.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
export WINEPREFIX="$tmp/prefix" WINEDEBUG=-all
. ./wine_prefix.sh; wine_prefix "$WINEPREFIX"
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
static int nitro;
static const char *version(const char *name) { return nitro && !strcmp(name,"NitroSystem") ? "1.2.1" : NULL; }
static struct { const char *(*mod_version)(const char *); } api = {version}, *g_api = &api;
#include "restricted_tools.inc"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main(int argc, char **argv) {
    WCHAR path[MAX_PATH], command[MAX_PATH+30]; STARTUPINFOW si={0}; PROCESS_INFORMATION pi={0};
    if (argc > 1 && !strcmp(argv[1],"--helper")) { Sleep(30000); return 0; }
    CHECK(scan_restricted_tools() == 0);
    nitro=1; CHECK(scan_restricted_tools() == 1); nitro=0;
    GetModuleFileNameW(NULL,path,MAX_PATH);
    WCHAR *slash=wcsrchr(path,L'\\'); CHECK(slash); lstrcpyW(slash+1,L"Redline.exe");
    wsprintfW(command,L"\"%s\" --helper",path); si.cb=sizeof si;
    CHECK(CreateProcessW(path,command,NULL,NULL,FALSE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));
    LONG result=scan_restricted_tools();
    TerminateProcess(pi.hProcess,0); WaitForSingleObject(pi.hProcess,5000); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    // Only the helper has the restricted executable name.
    CHECK(result & 2);
    puts("restricted tools: NitroSystem registration and Redline.exe process detected");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -Wno-unused-function -I src "$tmp/test.c" -o "$tmp/test.exe"
cp "$tmp/test.exe" "$tmp/Redline.exe"
wine "$tmp/test.exe"
wineserver -w
