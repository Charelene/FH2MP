/* Azimuth remaps the game during startup. Copy headers through ReadProcessMemory so an unmap during a probe fails safely (#32).
 * VirtualQuery followed by a direct pointer read cannot provide that guarantee. Windows types/functions come from windows.h.
 */
static int read_game_headers(const unsigned char *img, IMAGE_NT_HEADERS64 *nt, IMAGE_SECTION_HEADER *sections)
{
    IMAGE_DOS_HEADER dos;
    SIZE_T got;
    if (!img || !ReadProcessMemory(GetCurrentProcess(), img, &dos, sizeof dos, &got) || got != sizeof dos ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < (LONG)sizeof dos || dos.e_lfanew > 0x100000 ||
        !ReadProcessMemory(GetCurrentProcess(), img + dos.e_lfanew, nt, sizeof *nt, &got) || got != sizeof *nt ||
        nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != FH2_TIMESTAMP || nt->OptionalHeader.SizeOfImage != FH2_SIZE_OF_IMAGE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96 ||
        nt->FileHeader.SizeOfOptionalHeader < sizeof nt->OptionalHeader) {
        return 0;
    }
    return !sections ||
        (ReadProcessMemory(GetCurrentProcess(), img + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                           nt->FileHeader.SizeOfOptionalHeader, sections, nt->FileHeader.NumberOfSections * sizeof sections[0], &got) &&
         got == nt->FileHeader.NumberOfSections * sizeof sections[0]);
}
