#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include "fh2mod.h"
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#include "src/title_scope.inc"

static unsigned registrations;
static FH2Mod *registered(const FH2ModInfo *info)
{
    (void)info;
    ++registrations;
    return NULL;
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t temp[MAX_PATH], root[MAX_PATH], data[MAX_PATH], config[MAX_PATH], exe[MAX_PATH];
    assert(argc == 2);
    assert(GetTempPathW(MAX_PATH, temp));
    assert(GetTempFileNameW(temp, L"fh2", 0, root));
    assert(DeleteFileW(root) && CreateDirectoryW(root, NULL));
    lstrcpyW(data, root); lstrcatW(data, L"\\data");
    assert(CreateDirectoryW(data, NULL));
    lstrcpyW(config, data); lstrcatW(config, L"\\azimuth.ini");
    assert(!fh2_launch_selected(root, L"azimuth.exe"));
    assert(WritePrivateProfileStringW(L"launcher", L"game", L"deadbeef", config));
    assert(!fh2_launch_selected(root, L"azimuth.exe --skip-launcher"));
    assert(fh2_launch_selected(root, L"azimuth.exe --game=1A96545B"));
    assert(WritePrivateProfileStringW(L"launcher", L"game", L"1A96545B", config));
    assert(fh2_launch_selected(root, L"azimuth.exe"));
    assert(fh2_launch_selected(root, L"\"C:\\Games with spaces\\azimuth.exe\" \"--game=1a96545b\""));
    assert(!fh2_launch_selected(root, L"azimuth.exe --game=deadbeef"));
    assert(!fh2_launch_selected(root, L"azimuth.exe --game="));
    assert(!fh2_launch_selected(root, L"azimuth.exe --game=deadbeef --game=1a96545b"));
    assert(!fh2_launch_executable(root));

    lstrcpyW(exe, root); lstrcatW(exe, L"\\forza_durango_release_final.exe");
    HANDLE file = CreateFileW(exe, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    IMAGE_DOS_HEADER dos = {0}; IMAGE_NT_HEADERS64 nt = {0};
    dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128;
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.TimeDateStamp = FH2_TIMESTAMP;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = FH2_SIZE_OF_IMAGE;
    DWORD got; LARGE_INTEGER position;
    assert(WriteFile(file, &dos, sizeof dos, &got, NULL) && got == sizeof dos);
    position.QuadPart = 128; assert(SetFilePointerEx(file, position, NULL, FILE_BEGIN));
    assert(WriteFile(file, &nt, sizeof nt, &got, NULL) && got == sizeof nt);
    assert(!fh2_launch_executable(root)); /* Short file with plausible headers. */
    position.QuadPart = 58204160; assert(SetFilePointerEx(file, position, NULL, FILE_BEGIN));
    assert(SetEndOfFile(file)); CloseHandle(file);
    assert(fh2_launch_executable(root));
    nt.FileHeader.TimeDateStamp ^= 1;
    file = CreateFileW(exe, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    position.QuadPart = 128; assert(SetFilePointerEx(file, position, NULL, FILE_BEGIN));
    assert(WriteFile(file, &nt, sizeof nt, &got, NULL)); CloseHandle(file);
    assert(!fh2_launch_executable(root));

    /* Exercise the real DLL entry point: another title must not even register
     * the mod, in online or offline-account mode. No game is running here. */
    HMODULE dll = LoadLibraryW(argv[1]); assert(dll);
    int (*initialize)(const FH2ModApi *) = (void *)GetProcAddress(dll, "fh2mod_init");
    assert(initialize);
    FH2ModApi api = {0};
    api.size = sizeof api; api.api_version = 8;
    api.azimuth_dir = root; api.game_dir = root; api.register_mod = registered;
    assert(WritePrivateProfileStringW(L"launcher", L"game", L"deadbeef", config));
    assert(initialize(&api) == 0 && registrations == 0);
    assert(SetEnvironmentVariableW(L"FH2MP_OFFLINE", L"1"));
    assert(initialize(&api) == 0 && registrations == 0);
    assert(SetEnvironmentVariableW(L"FH2MP_OFFLINE", NULL));
    FreeLibrary(dll);
    assert(DeleteFileW(exe) && DeleteFileW(config));
    assert(RemoveDirectoryW(data) && RemoveDirectoryW(root));
    puts("PASS: title selection, explicit overrides, executable identity, and real DLL inactivity for other titles");
    return 0;
}
