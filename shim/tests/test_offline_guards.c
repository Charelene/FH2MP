/* The real DLL's entry point on a fake platform in an offline run (FH2MP_OFFLINE): the game-bug guards are made once the game's code is
 * ready, as online. The fake game image carries the expected bytes at the two byte-patch guards (placement fallback, Cinematics check)
 * and zeros everywhere else, so the hook guards find other code at their sites and are logged as not made. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"

#define FH2_SIZE 0x3a18000u
static unsigned char *g_img;

static long long CALLBACK stub(void) { return 0; }
static FH2Mod *registered(const FH2ModInfo *info) { (void)info; return (FH2Mod *)1; }
static void *base(void) { return g_img; }
static void *code(uint32_t ms) { (void)ms; return g_img; }
static int enable(FH2Mod *m, FH2Hook *h) { (void)m; return h != NULL; }
static FH2Hook *patch(FH2Mod *m, const char *name, void *at, const void *expect, const void *bytes, uint32_t n)
{
    (void)m; (void)name;
    if (memcmp(at, expect, n)) return NULL;
    memcpy(at, bytes, n);
    return (FH2Hook *)1;
}

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

static void make_game_file(const wchar_t *root)
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
    nt.OptionalHeader.SizeOfImage = FH2_SIZE;
    WriteFile(file, &dos, sizeof dos, &got, NULL);
    position.QuadPart = 128; SetFilePointerEx(file, position, NULL, FILE_BEGIN);
    WriteFile(file, &nt, sizeof nt, &got, NULL);
    position.QuadPart = 58204160; SetFilePointerEx(file, position, NULL, FILE_BEGIN);
    SetEndOfFile(file); CloseHandle(file);
}

/* The mapped game as the shim reads it: FH2 1.0.0.64's headers and one executable section over the whole image. */
static void map_game(void)
{
    static const unsigned char placement[11] = {0x49, 0x8b, 0xd6, 0x41, 0xff, 0x92, 0x00, 0x02, 0x00, 0x00, 0x90};
    static const unsigned char cinematics[9] = {0x48, 0x8b, 0x01, 0xff, 0x90, 0xb0, 0, 0, 0};
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS64 *nt;
    IMAGE_SECTION_HEADER *sec;
    g_img = VirtualAlloc(NULL, FH2_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    assert(g_img);
    dos = (IMAGE_DOS_HEADER *)g_img;
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 128;
    nt = (IMAGE_NT_HEADERS64 *)(g_img + 128);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->FileHeader.TimeDateStamp = 0x565501edu;
    nt->FileHeader.NumberOfSections = 1;
    nt->FileHeader.SizeOfOptionalHeader = sizeof nt->OptionalHeader;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = FH2_SIZE;
    sec = IMAGE_FIRST_SECTION(nt);
    memcpy(sec->Name, ".text", 5);
    sec->VirtualAddress = 0x1000;
    sec->Misc.VirtualSize = FH2_SIZE - 0x1000;
    sec->Characteristics = IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE;
    memcpy(g_img + 0x10c8971u, placement, sizeof placement);
    memcpy(g_img + 0x21f44du, cinematics, sizeof cinematics);
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH];
    DWORD start;
    assert(argc == 2);
    GetTempPathW(MAX_PATH, temp);
    GetTempFileNameW(temp, L"fh2", 0, root);
    DeleteFileW(root); CreateDirectoryW(root, NULL);
    lstrcpyW(path, root); lstrcatW(path, L"\\data"); CreateDirectoryW(path, NULL);
    lstrcatW(path, L"\\bin"); CreateDirectoryW(path, NULL);
    lstrcpyW(path, root); lstrcatW(path, L"\\data\\azimuth.ini");
    WritePrivateProfileStringW(L"launcher", L"game", L"1A96545B", path);
    make_game_file(root);
    map_game();

    SetEnvironmentVariableW(L"FH2MP_OFFLINE", L"1");
    HMODULE dll = LoadLibraryW(argv[1]); assert(dll);
    int (*initialize)(const FH2ModApi *) = (void *)GetProcAddress(dll, "fh2mod_init");
    assert(initialize);
    static FH2ModApi api;
    void **slot = (void **)&api;
    for (size_t i = 0; i < sizeof api / sizeof(void *); i++) slot[i] = (void *)stub;
    api.size = sizeof api; api.api_version = FH2MOD_API_VERSION; api.platform_version = "2.9.0";
    api.azimuth_dir = root; api.game_dir = root; api.register_mod = registered;
    api.game_base = base; api.wait_for_code = code; api.patch_bytes = patch; api.enable_hooks = enable;
    assert(initialize(&api) == 0);

    lstrcpyW(path, root); lstrcatW(path, L"\\data\\bin\\fh2mp.log");
    for (start = GetTickCount();; Sleep(20)) {
        read_log(path);
        if (strstr(g_log, " crash guards made") || GetTickCount() - start > 10000) break;
    }
    fputs(g_log, stdout);
    assert(strstr(g_log, "offline: FH2MP_OFFLINE is set"));
    assert(strstr(g_log, "offline: 2 of 13 crash guards made (the lines above name the ones that were not)\n"));
    assert(g_img[0x10c8974u] == 0xff && g_img[0x10c8975u] == 0x15);    /* call [rip+slot] into the placement fallback */
    assert(g_img[0x21f44du] == 0xff && g_img[0x21f44eu] == 0x15);      /* call [rip+slot] into the Cinematics check */
    assert(strstr(g_log, "dtconv") && strstr(g_log, "; not patched\n")); /* the hook guards checked their sites and said why not */
    assert(!strstr(g_log, "session:"));                                 /* nothing of the online path */
    puts("PASS: an offline run makes the crash guards once the game's code is ready, and only those");
    ExitProcess(0);
}
