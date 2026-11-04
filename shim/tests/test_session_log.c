/* The real DLL's entry point on a fake platform, no game: what fh2mp.log says about the session it was handed. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"

static long long CALLBACK stub(void) { return 0; }
static FH2Mod *registered(const FH2ModInfo *info) { (void)info; return (FH2Mod *)1; }

static char g_log[65536];
static void read_log(const wchar_t *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD got = 0;
    assert(f != INVALID_HANDLE_VALUE);
    ReadFile(f, g_log, sizeof g_log - 1, &got, NULL);
    g_log[got] = 0;
    CloseHandle(f);
}

static void make_game(const wchar_t *root)
{
    wchar_t exe[MAX_PATH];
    IMAGE_DOS_HEADER dos = {0}; IMAGE_NT_HEADERS64 nt = {0};
    DWORD got; LARGE_INTEGER position;
    lstrcpyW(exe, root); lstrcatW(exe, L"\\forza_durango_release_final.exe");
    HANDLE file = CreateFileW(exe, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_NEW, 0, NULL);
    assert(file != INVALID_HANDLE_VALUE);
    dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 128;
    nt.Signature = IMAGE_NT_SIGNATURE;
    nt.FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt.FileHeader.TimeDateStamp = 0x565501edu;
    nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt.OptionalHeader.SizeOfImage = 0x3a18000u;
    WriteFile(file, &dos, sizeof dos, &got, NULL);
    position.QuadPart = 128; SetFilePointerEx(file, position, NULL, FILE_BEGIN);
    WriteFile(file, &nt, sizeof nt, &got, NULL);
    position.QuadPart = 58204160; SetFilePointerEx(file, position, NULL, FILE_BEGIN);
    SetEndOfFile(file); CloseHandle(file);
}

int wmain(int argc, wchar_t **argv)
{
    static const char session[] = "xuid=9000000000abc\ntoken=fh2.notarealtokenvalue\nrun=ab12cd34\n";
    wchar_t temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH];
    DWORD got;
    HANDLE f;
    assert(argc == 2);
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"fh2", 0, root);
    DeleteFileW(root); CreateDirectoryW(root, NULL);
    lstrcpyW(path, root); lstrcatW(path, L"\\data"); CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\bin"); CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\fh2mp-session.txt");
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    WriteFile(f, session, sizeof session - 1, &got, NULL); CloseHandle(f);
    lstrcpyW(path, root); lstrcatW(path, L"\\data\\azimuth.ini");
    WritePrivateProfileStringW(L"launcher", L"game", L"1A96545B", path);
    make_game(root);

    SetEnvironmentVariableW(L"FH2MP_LOG_KEEP", L"nothing-matches,arg[");
    SetEnvironmentVariableW(L"FH2MP_NO_HEALTH", L"1");      /* its own test; here the thread would outlive the temp folder */
    HMODULE dll = LoadLibraryW(argv[1]); assert(dll);
    int (*initialize)(const FH2ModApi *) = (void *)GetProcAddress(dll, "fh2mod_init");
    assert(initialize);
    static FH2ModApi api;
    void **slot = (void **)&api;
    for (size_t i = 0; i < sizeof api / sizeof(void *); i++) slot[i] = (void *)stub;
    api.size = sizeof api; api.api_version = FH2MOD_API_VERSION; api.platform_version = "2.8.0";
    api.azimuth_dir = root; api.game_dir = root; api.register_mod = registered;
    assert(initialize(&api) == 0);

    lstrcpyW(path, root); lstrcatW(path, L"\\data\\bin\\fh2mp.log");
    read_log(path);
    fputs(g_log, stdout);
    assert(strstr(g_log, " attached (FH2 Mod Platform 2.8.0), tid="));
    assert(strstr(g_log, " UTC\n"));                        /* the tick stamps of this log get a wall clock */
    assert(strstr(g_log, "session: run=ab12cd34, token present"));
    assert(strstr(g_log, "arg["));                          /* FH2MP_LOG_KEEP: a line the release log drops */
    assert(!strstr(g_log, "notarealtokenvalue"));
    puts("PASS: the session's run id, the clock and FH2MP_LOG_KEEP in the real DLL's log");
    ExitProcess(0);
}
