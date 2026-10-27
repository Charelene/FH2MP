#!/usr/bin/env bash
# Exercise game and XSAPI header reads, including revoked mappings, in an isolated Wine prefix.
set -euo pipefail
cd "$(dirname "$0")"
tmp=$(mktemp -d)
cleanup() { WINEPREFIX="$tmp/prefix" wineserver -w || true; rm -rf "$tmp"; }
trap cleanup EXIT
cat > "$tmp/test.c" <<'C'
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#define XS_TIMESTAMP 0x5431cfcdu
#define XS_SIZE_OF_IMAGE 0x579000u
static int reserved_reads_succeed;
static const void *revoke_at;
static int revoked, short_read, revoked_reads_succeed;
static BOOL WINAPI compatibility_read(HANDLE process, LPCVOID address, LPVOID out, SIZE_T size, SIZE_T *got)
{
    MEMORY_BASIC_INFORMATION info;
    if (address == revoke_at) {
        DWORD old;
        if (revoked_reads_succeed) {
            assert(ReadProcessMemory(process, address, out, size, got) && *got == size);
        }
        assert(VirtualProtect((void *)address, size, PAGE_NOACCESS, &old));
        revoke_at = NULL;
        revoked++;
        if (revoked_reads_succeed) return TRUE;
    }
    if (reserved_reads_succeed && VirtualQuery(address, &info, sizeof info) && info.State == MEM_RESERVE) {
        ZeroMemory(out, size); *got = size; return TRUE;
    }
    BOOL ok = ReadProcessMemory(process, address, out, size, got);
    if (ok && short_read && *got) --*got;
    return ok;
}
#define ReadProcessMemory compatibility_read
#include "game_image.inc"
#include "xsapi_image.inc"
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
    reserved_reads_succeed = 1;  /* Wine 6 reports success and zero-filled bytes for this reserved section table. */
    assert(!read_game_headers(p, &copy, sections));
    reserved_reads_succeed = 0;
    nt->FileHeader.SizeOfOptionalHeader = sizeof nt->OptionalHeader;
    assert(VirtualProtect(p, 4096, PAGE_NOACCESS, &old));
    assert(!read_game_headers(p, &copy, NULL));
    assert(VirtualProtect(p, 4096, PAGE_READWRITE, &old));
    assert(read_game_headers(p, &copy, sections));
    assert(VirtualFree(p, 0, MEM_RELEASE));
    assert(!read_game_headers(p, &copy, NULL));
    assert(!read_game_headers(NULL, &copy, NULL));
    p = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(p);
    dos = (void *)p;
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0xff0;
    /* An accepted NT offset whose copy spans both pages. */
    nt = (void *)(p + dos->e_lfanew);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = XS_TIMESTAMP;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = XS_SIZE_OF_IMAGE;
    assert(read_xsapi_headers(p));
    nt->FileHeader.TimeDateStamp++;
    assert(!read_xsapi_headers(p));
    nt->FileHeader.TimeDateStamp--;
    nt->OptionalHeader.SizeOfImage++;
    assert(!read_xsapi_headers(p));
    nt->OptionalHeader.SizeOfImage--;
    dos->e_lfanew = 0x1000;
    assert(!read_xsapi_headers(p));
    dos->e_lfanew = -1;
    assert(!read_xsapi_headers(p));
    dos->e_lfanew = 0xff0;
    short_read = 1;
    assert(!read_xsapi_headers(p));
    short_read = 0;
    /* Revoke only after the production reader has queried the mapping. */
    revoke_at = nt;
    assert(!read_xsapi_headers(p));
    assert(revoked == 1 && !revoke_at);
    assert(VirtualProtect(p, 8192, PAGE_READWRITE, &old));
    assert(read_xsapi_headers(p));
    revoke_at = p;
    assert(!read_xsapi_headers(p));
    assert(revoked == 2 && !revoke_at);
    assert(VirtualProtect(p, 8192, PAGE_READWRITE, &old));
    assert(read_xsapi_headers(p));
    /* Wine 6 can return the original bytes even after revocation. Model its
     * successful-copy result on every host so the post-copy check is covered.
     */
    revoked_reads_succeed = 1;
    revoke_at = nt;
    assert(!read_xsapi_headers(p));
    assert(revoked == 3 && !revoke_at);
    assert(VirtualProtect(p, 8192, PAGE_READWRITE, &old));
    revoke_at = p;
    assert(!read_xsapi_headers(p));
    assert(revoked == 4 && !revoke_at);
    assert(VirtualProtect(p, 8192, PAGE_READWRITE, &old));
    assert(read_xsapi_headers(p));
    assert(VirtualFree(p, 0, MEM_RELEASE));
    assert(!read_xsapi_headers(p));
    assert(!read_xsapi_headers(NULL));
    puts("game_image: valid, invalid, inaccessible and unmapped headers checked");
    puts("xsapi_image: valid identity, mismatches, bounds, partial copies and revoked DOS/NT mappings checked");
    return 0;
}
C
${CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -Wextra -I src "$tmp/test.c" -o "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" WINEDEBUG=-all wine "$tmp/test.exe"
WINEPREFIX="$tmp/prefix" wineserver -w
