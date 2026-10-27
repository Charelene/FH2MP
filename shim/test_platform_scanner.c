/* Standalone memory-reader diagnosis. Loads the verified upstream platform in
 * Wine under a host name that DOES NOT start its mod loader. No game, FH2MP,
 * fake game headers, sockets, input, or clients. The only modified address is
 * this test process's copy of the platform's VirtualQuery import slot.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char *module, *candidate;
static SIZE_T (WINAPI *real_query)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
static unsigned queries;
static int revoke;

static SIZE_T WINAPI query(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T size)
{
    SIZE_T result = real_query(address, info, size);
    if (result == sizeof *info && address == candidate && info->BaseAddress == candidate) {
        ++queries;
        if (revoke && queries == 2) {
            DWORD old;
            assert(VirtualProtect(candidate, 4096, PAGE_NOACCESS, &old));
        }
    }
    return result;
}

static LONG CALLBACK fault(EXCEPTION_POINTERS *p)
{
    ULONG_PTR rva = (ULONG_PTR)p->ExceptionRecord->ExceptionAddress - (ULONG_PTR)module;
    if (p->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rva == 0x18fa &&
        p->ExceptionRecord->NumberParameters == 2 && p->ExceptionRecord->ExceptionInformation[0] == 0 &&
        p->ExceptionRecord->ExceptionInformation[1] == (ULONG_PTR)candidate && queries == 2 &&
        !GetModuleHandleW(L"FH2MP.dll")) {
        printf("confirmed: original platform+0x18fa reads revoked candidate; queries=%u; FH2MP absent\n", queries);
        fflush(stdout);
        ExitProcess(42); /* Expected diagnostic result, not a recovered scanner. */
    }
    fprintf(stderr, "unexpected exception code=%lx rva=%llx queries=%u\n",
            p->ExceptionRecord->ExceptionCode, (unsigned long long)rva, queries);
    fflush(stderr);
    ExitProcess(99);
}

int main(int argc, char **argv)
{
    static const unsigned char entry[] = {0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x56,0x57,0x55,0x53};
    static const unsigned char mz[] = {0x66,0x41,0x81,0x3f,0x4d,0x5a};
    assert(argc == 3);
    revoke = !strcmp(argv[2], "revoke");
    assert(revoke || !strcmp(argv[2], "control"));
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    AddVectoredExceptionHandler(1, fault);
    assert(!GetModuleHandleW(L"FH2MP.dll"));
    module = (unsigned char *)LoadLibraryA(argv[1]);
    if (!module) { fprintf(stderr, "LoadLibrary failed %lu\n", GetLastError()); return 2; }
    assert(!memcmp(module + 0x1780, entry, sizeof entry));
    assert(!memcmp(module + 0x18fa, mz, sizeof mz));
    IMAGE_DOS_HEADER *dos = (void *)module;
    IMAGE_NT_HEADERS64 *nt = (void *)(module + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR *desc = (void *)(module + nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    void **slot = NULL;
    for (; desc->Name; ++desc) {
        IMAGE_THUNK_DATA64 *names = (void *)(module + desc->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (void *)(module + desc->FirstThunk);
        for (unsigned i = 0; names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *name = (void *)(module + names[i].u1.AddressOfData);
            if (!strcmp((const char *)name->Name, "VirtualQuery")) slot = (void **)&iat[i].u1.Function;
        }
    }
    assert(slot);
    real_query = (void *)*slot;
    candidate = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(candidate && *(unsigned short *)candidate == 0); /* Not a fabricated game image. */
    DWORD old;
    assert(VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old));
    *slot = (void *)query;
    assert(VirtualProtect(slot, sizeof *slot, old, &old));
    void *found = ((void *(*)(void))(module + 0x1780))();
    assert(!found && queries == 2 && !revoke && !GetModuleHandleW(L"FH2MP.dll"));
    puts("control: original platform scanned committed candidate without a fault; FH2MP absent");
    assert(VirtualFree(candidate, 0, MEM_RELEASE));
    return 0;
}
