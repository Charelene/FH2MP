#!/usr/bin/env bash
# Exercise real Windows memory reads against a stale cached game pointer in an isolated Wine prefix.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#include "game_image.inc"
int main(void)
{
    unsigned char *p = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    IMAGE_DOS_HEADER *dos = (void *)p;
    IMAGE_NT_HEADERS64 *nt = (void *)(p + 128), copy;
    IMAGE_SECTION_HEADER sections[96];
    DWORD old;
    assert(p);
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = FH2_TIMESTAMP;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof nt->OptionalHeader;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = FH2_SIZE_OF_IMAGE;
    assert(read_game_headers(p, &copy, sections));
    nt->FileHeader.TimeDateStamp++;
    assert(!read_game_headers(p, &copy, NULL));
    nt->FileHeader.TimeDateStamp--;
    nt->FileHeader.NumberOfSections = 97;
    assert(!read_game_headers(p, &copy, sections));
    nt->FileHeader.NumberOfSections = 1;
    dos->e_lfanew = -1;
    assert(!read_game_headers(p, &copy, NULL));
    dos->e_lfanew = 128;
    nt->FileHeader.SizeOfOptionalHeader = 4096;
    assert(VirtualFree(p + 4096, 4096, MEM_DECOMMIT));
    assert(!read_game_headers(p, &copy, sections));
    nt->FileHeader.SizeOfOptionalHeader = sizeof nt->OptionalHeader;
    assert(VirtualProtect(p, 4096, PAGE_NOACCESS, &old));
    assert(!read_game_headers(p, &copy, NULL));
    assert(VirtualProtect(p, 4096, PAGE_READWRITE, &old));
    assert(read_game_headers(p, &copy, sections));
    assert(VirtualFree(p, 0, MEM_RELEASE));
    assert(!read_game_headers(p, &copy, NULL));
    assert(!read_game_headers(NULL, &copy, NULL));
    puts("game_image: valid, invalid, inaccessible and unmapped headers checked");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -I src "$tmp/test.c" -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
