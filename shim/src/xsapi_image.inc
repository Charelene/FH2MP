/* Candidate mappings can disappear after VirtualQuery/IsBadReadPtr (#155).
 * Inspect local header copies only; read_game_memory safely rejects a failed or
 * partial copy, including an unmap between its page query and actual read.
 */
static int read_xsapi_headers(const unsigned char *img)
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    UINT_PTR base = (UINT_PTR)img;
    if (!img || !read_game_memory(img, &dos, sizeof dos) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew >= 0x1000 ||
        (UINT_PTR)dos.e_lfanew > (UINT_PTR)-1 - base ||
        !read_game_memory((const void *)(base + (UINT_PTR)dos.e_lfanew), &nt, sizeof nt)) {
        return 0;
    }
    return nt.Signature == IMAGE_NT_SIGNATURE &&
           nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
           nt.FileHeader.TimeDateStamp == XS_TIMESTAMP &&
           nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt.OptionalHeader.SizeOfImage == XS_SIZE_OF_IMAGE;
}

/* 1 when p, read from an import slot of img, is a bound function. The loader maps a module before it binds the module's
 * imports; until then a slot still holds its hint/name RVA, a number below SizeOfImage (#305: the token patch called
 * 0x4223d0 when it ran in that gap). */
static int xs_bound(const unsigned char *img, const void *p)
{
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + ((const IMAGE_DOS_HEADER *)img)->e_lfanew);
    MEMORY_BASIC_INFORMATION m;
    if (!p || (ULONG_PTR)p < nt->OptionalHeader.SizeOfImage) {
        return 0;
    }
    return VirtualQuery(p, &m, sizeof m) == sizeof m && m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
           (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
