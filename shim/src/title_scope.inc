/* Azimuth hosts several titles. Do not touch its identity, argv, files or
 * network hooks unless this launch selects FH2 and the platform found FH2's
 * supported executable. The mapped image is checked again before game hooks.
 */
static int fh2_launch_selected(const wchar_t *directory, const wchar_t *command)
{
    typedef wchar_t **(WINAPI *split_command_fn)(const wchar_t *, int *);
    wchar_t config[MAX_PATH], selected[32];
    HMODULE shell;
    split_command_fn split;
    wchar_t **args;
    int count = 0, explicit_title = 0, i;
    if (!directory || lstrlenW(directory) >= MAX_PATH - 24) return 0;
    shell = LoadLibraryExW(L"shell32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    split = shell ? (split_command_fn)GetProcAddress(shell, "CommandLineToArgvW") : NULL;
    args = split && command ? split(command, &count) : NULL;
    if (!args) {
        if (shell) FreeLibrary(shell);
        return 0;
    }
    for (i = 1; i < count; ++i) {
        const wchar_t *arg = args[i];
        if (lstrlenW(arg) >= 7 && arg[0] == L'-' && arg[1] == L'-' &&
            arg[2] == L'g' && arg[3] == L'a' && arg[4] == L'm' &&
            arg[5] == L'e' && arg[6] == L'=') {
            if (lstrcmpiW(arg + 7, L"1a96545b") != 0) {
                LocalFree(args);
                FreeLibrary(shell);
                return 0;
            }
            explicit_title = 1;
        }
    }
    LocalFree(args);
    FreeLibrary(shell);
    if (explicit_title) return 1;
    lstrcpyW(config, directory);
    lstrcatW(config, L"\\data\\azimuth.ini");
    return GetPrivateProfileStringW(L"launcher", L"game", L"", selected, 32, config) == 8 &&
           lstrcmpiW(selected, L"1a96545b") == 0;
}

static int fh2_launch_executable(const wchar_t *mount)
{
    wchar_t path[MAX_PATH];
    HANDLE file;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    LARGE_INTEGER size, offset;
    DWORD got;
    int valid;
    if (!mount || !mount[0] || lstrlenW(mount) >= MAX_PATH - 36) return 0;
    lstrcpyW(path, mount);
    lstrcatW(path, L"\\forza_durango_release_final.exe");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    valid = GetFileSizeEx(file, &size) && size.QuadPart == 58204160 &&
            ReadFile(file, &dos, sizeof dos, &got, NULL) && got == sizeof dos &&
            dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew >= (LONG)sizeof dos &&
            dos.e_lfanew <= 0x100000;
    if (valid) {
        offset.QuadPart = dos.e_lfanew;
        valid = SetFilePointerEx(file, offset, NULL, FILE_BEGIN) &&
                ReadFile(file, &nt, sizeof nt, &got, NULL) && got == sizeof nt &&
                nt.Signature == IMAGE_NT_SIGNATURE &&
                nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                nt.FileHeader.TimeDateStamp == FH2_TIMESTAMP &&
                nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                nt.OptionalHeader.SizeOfImage == FH2_SIZE_OF_IMAGE;
    }
    CloseHandle(file);
    return valid;
}
