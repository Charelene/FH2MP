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
