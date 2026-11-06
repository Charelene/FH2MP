/* Azimuth remaps the game during startup. Copy headers through ReadProcessMemory so an unmap during a probe fails safely (#32).
 * VirtualQuery followed by a direct pointer read cannot provide that guarantee. Windows types/functions come from windows.h.
 */
static int readable_game_range(const void *address, SIZE_T size)
{
    /* Older Wine can report a successful zero-filled ReadProcessMemory for reserved
     * pages. Reject uncommitted/unreadable ranges first, then still use the safe
     * copy so an unmap between VirtualQuery and the read cannot fault our probe.
     */
    UINT_PTR start = (UINT_PTR)address, cursor = start;
    if (size > (UINT_PTR)-1 - start) return 0;
    while (cursor < start + size) {
        MEMORY_BASIC_INFORMATION info;
        UINT_PTR end;
        if (!VirtualQuery((const void *)cursor, &info, sizeof info) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) ||
            info.RegionSize > (UINT_PTR)-1 - (UINT_PTR)info.BaseAddress) return 0;
        end = (UINT_PTR)info.BaseAddress + info.RegionSize;
        if (end <= cursor) return 0;
        cursor = end;
    }
    return 1;
}

/* VirtualQuery is slow on Windows inside large committed blocks: it works out RegionSize by walking the page tables from
 * the address to the end of the run of pages with the same state, so in the game's memory under Azimuth (GBs committed)
 * one call can take milliseconds (11 ms for an address in a 1 GB block on a test PC, against 0.5 us for
 * ReadProcessMemory). read_game_memory runs per car and per frame in the lighting sync, so two of those calls per read
 * stalled the game's main thread online (free-roam lag with many players). On Windows, ReadProcessMemory already fails
 * cleanly on pages that are not committed or not readable, so the range checks are only made under Wine.
 */
static int game_reads_need_range_check(void)
{
    static volatile LONG state; /* 0 unknown, 1 Windows, 2 Wine */
    LONG s = state;
    if (!s) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        s = ntdll && GetProcAddress(ntdll, "wine_get_version") ? 2 : 1;
        InterlockedExchange(&state, s);
    }
    return s == 2;
}

static int read_game_memory(const void *address, void *out, SIZE_T size)
{
    SIZE_T got = 0;
    if (!game_reads_need_range_check()) {
        return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
    }
    /* Wine 6 can also copy a page revoked after the initial range check.
     * Inspect the mapping again before trusting the copied header/fields.
     */
    return readable_game_range(address, size) &&
           ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size &&
           readable_game_range(address, size);
}

static int read_game_headers(const unsigned char *img, IMAGE_NT_HEADERS64 *nt, IMAGE_SECTION_HEADER *sections)
{
    IMAGE_DOS_HEADER dos;
    if (!img || !read_game_memory(img, &dos, sizeof dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof dos || dos.e_lfanew > 0x100000 ||
        !read_game_memory(img + dos.e_lfanew, nt, sizeof *nt) ||
        nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != FH2_TIMESTAMP || nt->OptionalHeader.SizeOfImage != FH2_SIZE_OF_IMAGE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof nt->OptionalHeader) {
        return 0;
    }
    return !sections ||
        read_game_memory(img + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                         nt->FileHeader.SizeOfOptionalHeader, sections, nt->FileHeader.NumberOfSections * sizeof sections[0]);
}
