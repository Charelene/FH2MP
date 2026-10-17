/*
 * FH2MP shim: a winmm.dll proxy loaded by azimuth.exe (0.9.10+) from its own folder.
 *
 * Azimuth 0.9.10 verifies azimuth-tls.dll against its own SHA-256, so the shim can no longer replace
 * that helper. azimuth.exe imports WINMM.dll (timeBeginPeriod), so a winmm.dll placed next to it is
 * loaded in-process before the game starts. This is the same mechanism FH2 Extra Options 0.2.88 uses.
 *
 * This DLL:
 *   1. forwards every winmm export to the system winmm.dll (winmm_forward.inc, generated)
 *   2. chain-loads the FH2 Extra Options winmm proxy if it is present as fh2extra\winmm.dll
 *   3. hooks azimuth.exe's Platform::Details::GetCmdArguments so the game receives launch
 *      arguments (stock Azimuth always returns argv == [""]).
 *   4. once Azimuth has loaded its own azimuth-tls.dll (the game image is mapped by then), applies
 *      the game patches (Winsock, matchmaking groups, traffic, STS auth) and starts the monitor.
 *
 * Argument sources, in order: env FH2MP_ARGS (space separated), then fh2mp-args.txt in data\bin next
 * to azimuth-tls.dll (one argument per line, '#' starts a comment). Logs and probe files live there too.
 *
 * Only KERNEL32 is used (no CRT).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define MAX_ARGS 32
#define MAX_ARG_CHARS 256

/* Platform::Details::GetCmdArguments(int*): same 301-byte function in each supported azimuth.exe */
#define HOOK_SIZE 301u
typedef struct {
    unsigned rva;
    unsigned long long fnv1a64;
} hook_build;
static const hook_build g_hook_builds[] = {
    {0xc0850u, 0xb2987ae53c46b28cull}, /* azimuth.exe 0.9.10 (sha256 78a5f635...3739) */
};

/* forza_durango_release_final.exe 1.0.0.64 (image identity, as checked by the Extra Options mod) */
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
/* `lea r8,[rip+disp32]` in the matchmaking-group table that picks the FreeRoam entity group */
#define FH2_FREEROAM_LEA_RVA 0x80e868u
#define FH2_FREEROAM_STR_RVA 0x1e8aba8u
static const char FH2_FREEROAM_OLD[] = "EntityModel\\Network\\network_matchmaking_freeroam.xml";
static const char FH2_FREEROAM_NEW[] =
    "EntityModel\\Network\\network_matchmaking_freeroam_directip.xml";

/* the RoadTrip entry of the same table (FUN_14080e7d0, session type 1) */
#define FH2_ROADTRIP_LEA_RVA 0x80e801u
#define FH2_ROADTRIP_STR_RVA 0x1e8ab70u
static const char FH2_ROADTRIP_OLD[] = "EntityModel\\Network\\network_matchmaking_roadtrip.xml";
static const char FH2_ROADTRIP_NEW[] =
    "EntityModel\\Network\\network_matchmaking_roadtrip_directip.xml";

/* FH2 option singleton (FUN_1402959a0 returns it): static object, lazily initialised */
#define FH2_OPTIONS_RVA 0x34af7d0u
#define FH2_OPTIONS_INIT_FLAG_RVA 0x34b3390u /* bit 0 set once constructed */
#define FH2_OPT_BYPASS_MATCHMAKING 0x2ff1u
#define FH2_OPT_SINGLE_PLAYER_EVENTS 0x30f4u /* bool: NetSessionEnableSinglePlayerFreeRoamEvents */
#define FH2_OPT_DIRECTIP_ROADTRIP 0x3013u /* char[0x20] */
#define FH2_OPT_DIRECTIP_FREEROAM 0x3033u /* char[0x20] */
#define FH2_OPT_LOCAL_LOBBY_HOPPER 0x321au
#define FH2_OPT_ENABLE_DEBUG_IP_LIST 0x321bu
#define FH2_OPT_IP_SLOT_SIZE 0x20u
/* connect probe: the two calls NetworkRequestDirectIpMatch::OnEnter makes (FUN_1408d0670) */
#define FH2_RVA_BUILD_REQUEST 0x1763490u /* FUN_141763490(req, std::string *ip, std::string *port) */
#define FH2_RVA_BEGIN_JOIN 0x17a99e0u    /* FUN_1417a99e0(manager, req) */
#define FH2_RVA_NET_MANAGER_PTR 0x36e2960u /* DAT_1436e2960: pointer to the network manager */

static unsigned char *find_fh2_image(void);
static void trace_phase_reset(const char *why);
static int trace_count(void);
static void arm_trace_file(void);

static wchar_t g_dir[MAX_PATH];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_args[MAX_ARGS][MAX_ARG_CHARS];
static wchar_t *g_argv[MAX_ARGS + 1];
static int g_argc;

static void mem_copy(void *dst, const void *src, SIZE_T n)
{
    volatile unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
}

static volatile LONG g_log_lock;
static volatile DWORD g_log_owner;

static void log_lock(void)
{
    DWORD me = GetCurrentThreadId();

    if (g_log_owner == me) {
        return; /* re-entrant on the same thread */
    }
    while (InterlockedCompareExchange(&g_log_lock, 1, 0) != 0) {
        SwitchToThread();
    }
    g_log_owner = me;
}

static void log_unlock(void)
{
    if (g_log_owner == GetCurrentThreadId()) {
        g_log_owner = 0;
        InterlockedExchange(&g_log_lock, 0);
    }
}

/* log_begin() takes the lock; a string ending in '\n' releases it, so each line is atomic */
static void log_raw(const char *s)
{
    DWORD written;
    DWORD n = 0;

    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    while (s[n]) {
        n++;
    }
    WriteFile(g_log, s, n, &written, NULL);
    if (n > 0 && s[n - 1] == '\n') {
        log_unlock();
    }
}

static void log_hex(unsigned long long v, int digits)
{
    char buf[17];
    int i;

    for (i = digits - 1; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    buf[digits] = '\0';
    log_raw(buf);
}

static void log_dec(unsigned long v)
{
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    log_raw(buf + i);
}

static void log_wide(const wchar_t *w)
{
    char buf[3 * MAX_ARG_CHARS + 1];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL);

    log_raw(n > 0 ? buf : "?");
}

static void log_begin(void)
{
    log_lock();
    log_raw("[");
    log_dec(GetTickCount());
    log_raw("] ");
}

static void add_arg(const wchar_t *s, int len)
{
    int i;

    if (g_argc >= MAX_ARGS || len <= 0 || len >= MAX_ARG_CHARS) {
        return;
    }
    for (i = 0; i < len; i++) {
        g_args[g_argc][i] = s[i];
    }
    g_args[g_argc][len] = L'\0';
    g_argv[g_argc] = g_args[g_argc];
    g_argc++;
}

static void load_args(void)
{
    static wchar_t env[2048];
    wchar_t path[MAX_PATH];
    DWORD n;
    HANDLE f;

    /* argv[0] stays the empty string, exactly as stock Azimuth reports it */
    g_args[0][0] = L'\0';
    g_argv[0] = g_args[0];
    g_argc = 1;

    n = GetEnvironmentVariableW(L"FH2MP_ARGS", env, 2048);
    if (n > 0 && n < 2048) {
        wchar_t *p = env;
        while (*p) {
            wchar_t *start;
            while (*p == L' ' || *p == L'\t') {
                p++;
            }
            start = p;
            while (*p && *p != L' ' && *p != L'\t') {
                p++;
            }
            add_arg(start, (int)(p - start));
        }
    }

    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-args.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        static char raw[4096];
        DWORD got = 0;
        if (ReadFile(f, raw, sizeof raw - 1, &got, NULL)) {
            char *line = raw;
            raw[got] = '\0';
            while (*line) {
                char *eol = line;
                int len;
                wchar_t wide[MAX_ARG_CHARS];
                while (*eol && *eol != '\n') {
                    eol++;
                }
                len = (int)(eol - line);
                while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ')) {
                    len--;
                }
                if (len > 0 && line[0] != '#' && len < MAX_ARG_CHARS) {
                    int w = MultiByteToWideChar(CP_UTF8, 0, line, len, wide, MAX_ARG_CHARS - 1);
                    if (w > 0) {
                        add_arg(wide, w);
                    }
                }
                line = *eol ? eol + 1 : eol;
            }
        }
        CloseHandle(f);
    }
    g_argv[g_argc] = NULL;
}

/* Replacement for Platform::Details::GetCmdArguments(int* count) -> wchar_t** */
static wchar_t **hooked_get_cmd_arguments(int *count)
{
    if (count) {
        *count = g_argc;
    }
    return g_argv;
}

static unsigned long long fnv1a64(const unsigned char *p, SIZE_T n)
{
    unsigned long long h = 0xcbf29ce484222325ull;
    while (n--) {
        h = (h ^ *p++) * 0x100000001b3ull;
    }
    return h;
}

/*
 * Azimuth binds the game's WS2_32 imports to its own "offline" implementations (WSASocketW always
 * fails with WSENETDOWN, getaddrinfo never reaches ws2_32...). The game's WS2_32 import block is
 * 23 contiguous slots; repoint every slot that is not already inside the real ws2_32 module.
 * Entries: name, or ordinal when name is NULL.
 */
#define FH2_WS2_IAT_RVA 0x1dd0d80u
typedef struct {
    const char *name;
    unsigned short ordinal;
} ws2_slot;
static const ws2_slot g_ws2_slots[] = {
    {NULL, 17}, {NULL, 18}, {NULL, 20}, {NULL, 21}, {NULL, 22}, {NULL, 115}, {NULL, 116},
    {NULL, 111}, {"WSACloseEvent", 0}, {"WSACreateEvent", 0}, {"WSARecvFrom", 0},
    {"WSAResetEvent", 0}, {"WSASendTo", 0}, {"WSASetEvent", 0}, {"WSASocketW", 0},
    {"getaddrinfo", 0}, {"freeaddrinfo", 0}, {"getnameinfo", 0}, {NULL, 8}, {NULL, 9}, {NULL, 14},
    {NULL, 15}, {NULL, 16},
};
static int g_ws2_patched;

static void module_name_of(void *addr, char *out, int cap)
{
    HMODULE m = NULL;
    out[0] = '?';
    out[1] = '\0';
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)addr, &m) && m) {
        wchar_t w[MAX_PATH];
        DWORD n = GetModuleFileNameW(m, w, MAX_PATH);
        if (n) {
            DWORD k = n;
            int j = 0;
            while (k > 0 && w[k - 1] != L'\\' && w[k - 1] != L'/') {
                k--;
            }
            for (; k < n && j < cap - 1; k++) {
                out[j++] = (char)w[k];
            }
            out[j] = '\0';
        }
    }
}

static void patch_game_winsock(unsigned char *img)
{
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    unsigned i;
    unsigned changed = 0;

    if (!img) {
        return;
    }
    if (!ws2) {
        ws2 = LoadLibraryW(L"ws2_32.dll");
    }
    if (!ws2) {
        log_begin();
        log_raw("winsock: ws2_32.dll not available\n");
        return;
    }
    for (i = 0; i < sizeof g_ws2_slots / sizeof g_ws2_slots[0]; i++) {
        void **slot = (void **)(img + FH2_WS2_IAT_RVA + i * 8u);
        FARPROC real = g_ws2_slots[i].name
                           ? GetProcAddress(ws2, g_ws2_slots[i].name)
                           : GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)g_ws2_slots[i].ordinal);
        char oldmod[40];
        DWORD oldprot;

        if (!real) {
            continue;
        }
        module_name_of(*slot, oldmod, sizeof oldmod);
        if (*slot == (void *)real) {
            continue; /* already the real function */
        }
        if (!VirtualProtect(slot, 8, PAGE_READWRITE, &oldprot)) {
            continue;
        }
        log_begin();
        log_raw("winsock: slot ");
        log_dec(i);
        log_raw(g_ws2_slots[i].name ? " " : " ordinal ");
        if (g_ws2_slots[i].name) {
            log_raw(g_ws2_slots[i].name);
        } else {
            log_dec(g_ws2_slots[i].ordinal);
        }
        log_raw(": ");
        log_raw(oldmod);
        log_raw(" -> ws2_32\n");
        *slot = (void *)real;
        VirtualProtect(slot, 8, oldprot, &oldprot);
        changed++;
    }
    g_ws2_patched = 1;
    if (changed) {
        log_begin();
        log_raw("winsock: import block checked, slots repointed=");
        log_dec(changed);
        log_raw("\n");
    }
}

/*
 * The online traffic switch calls FUN_1403b94f0 (`mov byte [rcx+0x20],0 ; ret`) to put the
 * TrafficMessageCoordinator into networked mode, where traffic comes from a dedicated server that
 * no longer exists. Turning that 0 into a 1 keeps the in-process OfflineTrafficManager in charge
 * during online sessions (every client simulates its own AI traffic).
 */
#define FH2_TRAFFIC_NETMODE_RVA 0x3b94f0u
static int patch_traffic_local_mode(void)
{
    unsigned char *img = find_fh2_image();
    unsigned char *fn;
    DWORD old;

    if (!img) {
        return 0;
    }
    fn = img + FH2_TRAFFIC_NETMODE_RVA;
    if (fn[0] != 0xC6 || fn[1] != 0x41 || fn[2] != 0x20 || fn[3] != 0x00 || fn[4] != 0xC3) {
        log_begin();
        log_raw("traffic: unexpected bytes at set-networked function, not patched\\n");
        return 0;
    }
    if (!VirtualProtect(fn + 3, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    fn[3] = 0x01;
    VirtualProtect(fn + 3, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    log_begin();
    log_raw("traffic: networked-mode switch patched; traffic stays local\n");
    return 1;
}

/*
 * The stateflow `NetworkSwitchTrafficMode online="true"` state ends in FUN_140728c10 (SyncTrafficModule
 * go-online: sets +0xae and waits for a server to create every vehicle). Keeping only the coordinator
 * local left the module in its online state, so nothing spawned and "Syncing traffic vehicles" never
 * cleared. Jumping to FUN_140728a30 (the go-offline switch) keeps the single-player traffic path.
 */
#define FH2_TRAFFIC_ONLINE_RVA  0x728c10u
#define FH2_TRAFFIC_OFFLINE_RVA 0x728a30u
static int patch_traffic_module_offline(void)
{
    static const unsigned char on_sig[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20};
    static const unsigned char off_sig[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0xC7, 0x44, 0x24, 0x20};
    unsigned char *img = find_fh2_image();
    unsigned char *fn;
    unsigned char *tgt;
    DWORD old;
    int i;

    if (!img) {
        return 0;
    }
    fn = img + FH2_TRAFFIC_ONLINE_RVA;
    tgt = img + FH2_TRAFFIC_OFFLINE_RVA;
    for (i = 0; i < (int)sizeof on_sig; i++) {
        if (fn[i] != on_sig[i] || tgt[i] != off_sig[i]) {
            log_begin();
            log_raw("traffic: unexpected bytes at module online/offline switch, not patched\n");
            return 0;
        }
    }
    if (!VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    {
        long long rel = (long long)(tgt - (fn + 5));
        int rel32 = (int)rel;
        fn[0] = 0xE9;
        mem_copy(fn + 1, &rel32, 4);
    }
    VirtualProtect(fn, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
    log_begin();
    log_raw("traffic: module go-online redirected to go-offline; single-player traffic stays active\n");
    return 1;
}

/*
 * Turn 10 LSP calls (Message Center, profile, ...) first ask StsAuthTokenManager for an Xbox
 * token via Windows.Xbox.System.User.GetTokenAndSignatureAsync. Azimuth never completes that
 * async call, so every web request parks forever and the network is never touched. FUN_140ef88b0
 * (GetAuthToken) is replaced by a stub that completes the game's own IStsAuthReadyCallback
 * (vtable slot 2: hr, token, signature) synchronously with placeholder strings, the same way the
 * game's "No primary Live user" failure branch does. Disable with env FH2MP_NO_STS.
 */
#define FH2_STS_GETAUTH_RVA 0xef88b0u
typedef struct {
    wchar_t buf[8];
    unsigned long long size;
    unsigned long long cap;
} msvc_wstr; /* MSVC x64 std::wstring, small-string form */

static void msvc_wstr_set(msvc_wstr *w, const wchar_t *src)
{
    unsigned long long n = 0;

    while (n < 7 && src[n]) {
        w->buf[n] = src[n];
        n++;
    }
    w->buf[n] = 0;
    w->size = n;
    w->cap = 7;
}

static int g_sts_stub_calls;

static void sts_auth_stub(void *a, void *b, void *c, void *d, void **ref)
{
    void **cb = (void **)ref[1];
    msvc_wstr tok;
    msvc_wstr sig;

    (void)a; (void)b; (void)c; (void)d;
    msvc_wstr_set(&tok, L"fh2mp");
    msvc_wstr_set(&sig, L"fh2mp");
    if (g_sts_stub_calls++ < 8) {
        log_begin();
        log_raw("sts: GetAuthToken completed in-process\n");
    }
    ((void (*)(void *, long, msvc_wstr *, msvc_wstr *))(((void **)*cb)[2]))(cb, 0, &tok, &sig);
}

static int patch_sts_auth(void)
{
    static const unsigned char sig[] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
                                        0x48, 0x81, 0xEC, 0xB0, 0x02, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    unsigned char *fn;
    unsigned long long dest = (unsigned long long)(ULONG_PTR)&sts_auth_stub;
    DWORD old;
    int i;

    if (!img) {
        return 0;
    }
    fn = img + FH2_STS_GETAUTH_RVA;
    for (i = 0; i < (int)sizeof sig; i++) {
        if (fn[i] != sig[i]) {
            log_begin();
            log_raw("sts: unexpected bytes at GetAuthToken, not patched\n");
            return 0;
        }
    }
    if (!VirtualProtect(fn, 14, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    fn[0] = 0xFF; /* jmp [rip+0] ; dq dest */
    fn[1] = 0x25;
    fn[2] = fn[3] = fn[4] = fn[5] = 0;
    mem_copy(fn + 6, &dest, 8);
    VirtualProtect(fn, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 14);
    log_begin();
    log_raw("sts: GetAuthToken replaced; LSP requests no longer wait for an Xbox token\n");
    return 1;
}

static int install_cmdline_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    unsigned char *target = NULL;
    unsigned long long h = 0;
    unsigned long long dest = (unsigned long long)(ULONG_PTR)&hooked_get_cmd_arguments;
    unsigned char stub[12];
    DWORD old;
    size_t i;

    for (i = 0; i < sizeof g_hook_builds / sizeof g_hook_builds[0]; i++) {
        unsigned char *cand = exe + g_hook_builds[i].rva;
        if (!IsBadReadPtr(cand, HOOK_SIZE)) {
            h = fnv1a64(cand, HOOK_SIZE);
            if (h == g_hook_builds[i].fnv1a64) {
                target = cand;
                break;
            }
        }
    }
    if (!target) {
        log_begin();
        log_raw("hook: GetCmdArguments not found in this azimuth.exe (last fnv1a64=");
        log_hex(h, 16);
        log_raw("), skipping; launch arguments will not be injected\n");
        return 0;
    }
    stub[0] = 0x48; /* mov rax, imm64 ; jmp rax */
    stub[1] = 0xB8;
    mem_copy(stub + 2, &dest, 8);
    stub[10] = 0xFF;
    stub[11] = 0xE0;
    if (!VirtualProtect(target, sizeof stub, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("hook: VirtualProtect failed, error ");
        log_dec(GetLastError());
        log_raw("\n");
        return 0;
    }
    mem_copy(target, stub, sizeof stub);
    VirtualProtect(target, sizeof stub, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, sizeof stub);
    log_begin();
    log_raw("hook: GetCmdArguments patched, argc=");
    log_dec((unsigned long)g_argc);
    log_raw("\n");
    return 1;
}

static int has_arg_prefix(const wchar_t *prefix)
{
    int i;

    for (i = 1; i < g_argc; i++) {
        const wchar_t *a = g_argv[i];
        const wchar_t *p = prefix;
        while (*p && *a && ((*a | 0x20) == (*p | 0x20))) {
            a++;
            p++;
        }
        if (!*p) {
            return 1;
        }
    }
    return 0;
}

static unsigned char *find_fh2_image(void)
{
    SYSTEM_INFO si;
    unsigned char *addr;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *found = NULL;

    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;
    while (addr < (unsigned char *)si.lpMaximumApplicationAddress &&
           VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase &&
            (mbi.Type == MEM_IMAGE || mbi.Type == MEM_PRIVATE) && mbi.RegionSize >= 0x1000) {
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mbi.AllocationBase;
            if (!IsBadReadPtr(dos, sizeof *dos) && dos->e_magic == IMAGE_DOS_SIGNATURE &&
                dos->e_lfanew > 0 && dos->e_lfanew < 0x1000) {
                const IMAGE_NT_HEADERS64 *nt =
                    (const IMAGE_NT_HEADERS64 *)((unsigned char *)dos + dos->e_lfanew);
                if (!IsBadReadPtr(nt, sizeof *nt) && nt->Signature == IMAGE_NT_SIGNATURE &&
                    nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                    nt->FileHeader.TimeDateStamp == FH2_TIMESTAMP &&
                    nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                    nt->OptionalHeader.SizeOfImage == FH2_SIZE_OF_IMAGE &&
                    !(nt->FileHeader.Characteristics & IMAGE_FILE_DLL)) {
                    found = (unsigned char *)mbi.AllocationBase;
                    break;
                }
            }
        }
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return found;
}

static int str_equal(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/*
 * Point one entry (FreeRoam or RoadTrip) of the matchmaking group table at its `_directip` group, whose
 * coordinator runs the stateflow `matchmake_*_debug_ip` (NetworkRequestDirectIpMatch).
 * The replacement path is longer than the original string slot, so it lives in a fresh page
 * allocated within +-2GB and the lea's rip-relative displacement is rewritten.
 */
static int redirect_group(const char *name, unsigned lea_rva, unsigned str_rva, const char *oldstr,
                          const char *newstr_src, unsigned newstr_len)
{
    unsigned char *img = find_fh2_image();
    unsigned char *lea;
    unsigned char *newstr = NULL;
    unsigned char *probe;
    unsigned char *lo;
    unsigned char *hi;
    int oldDisp;
    long long newDisp;
    DWORD old;
    SIZE_T i;

    if (!img) {
        log_begin();
        log_raw("redirect: FH2 1.0.0.64 image not found in this process\n");
        return 0;
    }
    lea = img + lea_rva;
    log_begin();
    log_raw("redirect: FH2 image found, patching the ");
    log_raw(name);
    log_raw(" entry\n");
    if (lea[0] != 0x4c || lea[1] != 0x8d || lea[2] != 0x05) {
        log_begin();
        log_raw("redirect: unexpected instruction bytes, skipping\n");
        return 0;
    }
    mem_copy(&oldDisp, lea + 3, 4);
    if ((lea + 7 + oldDisp) != (img + str_rva) || !str_equal((const char *)(img + str_rva), oldstr)) {
        log_begin();
        log_raw("redirect: call site does not reference the expected path string, skipping\n");
        return 0;
    }

    lo = lea > (unsigned char *)0x70000000 ? lea - 0x70000000 : (unsigned char *)0x10000;
    hi = lea + 0x70000000;
    for (probe = (unsigned char *)((ULONG_PTR)(img + FH2_SIZE_OF_IMAGE + 0xffff) & ~(ULONG_PTR)0xffff);
         probe < hi; probe += 0x10000) {
        newstr = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (newstr) {
            break;
        }
    }
    if (!newstr) {
        for (probe = (unsigned char *)((ULONG_PTR)(img - 0x10000) & ~(ULONG_PTR)0xffff);
             probe > lo; probe -= 0x10000) {
            newstr = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (newstr) {
                break;
            }
        }
    }
    if (!newstr) {
        log_begin();
        log_raw("redirect: could not allocate a page near the game image\n");
        return 0;
    }
    for (i = 0; i < newstr_len; i++) {
        newstr[i] = (unsigned char)newstr_src[i];
    }
    newDisp = (long long)(newstr - (lea + 7));
    if (newDisp > 0x7fffffffll || newDisp < -0x80000000ll) {
        log_begin();
        log_raw("redirect: allocated page out of rel32 range\n");
        return 0;
    }
    {
        int d32 = (int)newDisp;
        if (!VirtualProtect(lea, 8, PAGE_EXECUTE_READWRITE, &old)) {
            log_begin();
            log_raw("redirect: VirtualProtect failed\n");
            return 0;
        }
        mem_copy(lea + 3, &d32, 4);
        VirtualProtect(lea, 8, old, &old);
        FlushInstructionCache(GetCurrentProcess(), lea, 8);
    }
    log_begin();
    log_raw("redirect: ");
    log_raw(name);
    log_raw(" matchmaking group -> ");
    log_raw(newstr_src);
    log_raw("\n");
    return 1;
}

static int redirect_freeroam_group(void)
{
    return redirect_group("FreeRoam", FH2_FREEROAM_LEA_RVA, FH2_FREEROAM_STR_RVA, FH2_FREEROAM_OLD,
                          FH2_FREEROAM_NEW, sizeof FH2_FREEROAM_NEW);
}

static int redirect_roadtrip_group(void)
{
    return redirect_group("RoadTrip", FH2_ROADTRIP_LEA_RVA, FH2_ROADTRIP_STR_RVA, FH2_ROADTRIP_OLD,
                          FH2_ROADTRIP_NEW, sizeof FH2_ROADTRIP_NEW);
}

/* value of the first `<prefix><value>` argument as narrow ASCII; returns 0 when absent */
static int arg_value_ascii(const wchar_t *prefix, char *out, int cap)
{
    int i;

    for (i = 1; i < g_argc; i++) {
        const wchar_t *a = g_argv[i];
        const wchar_t *p = prefix;
        while (*p && *a && ((*a | 0x20) == (*p | 0x20))) {
            a++;
            p++;
        }
        if (!*p) {
            int n = 0;
            while (*a && n < cap - 1) {
                out[n++] = (char)*a++;
            }
            out[n] = '\0';
            return n > 0;
        }
    }
    return 0;
}

static void log_slot(const char *label, const unsigned char *p)
{
    char buf[FH2_OPT_IP_SLOT_SIZE + 1];
    unsigned i;

    for (i = 0; i < FH2_OPT_IP_SLOT_SIZE; i++) {
        buf[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : (p[i] ? '?' : '\0');
    }
    buf[FH2_OPT_IP_SLOT_SIZE] = '\0';
    log_begin();
    log_raw(label);
    log_raw("=\"");
    log_raw(buf);
    log_raw("\"\n");
}

typedef void *(*build_req_fn)(void *out, void *ip, void *port, void *unused);
typedef void (*begin_join_fn)(void *manager, void *req);

static void make_std_string(unsigned char *s, const char *text)
{
    unsigned i = 0;
    for (i = 0; i < 32; i++) {
        s[i] = 0;
    }
    for (i = 0; text[i] && i < 15; i++) {
        s[i] = (unsigned char)text[i];
    }
    *(unsigned long long *)(s + 0x10) = i; /* size */
    *(unsigned long long *)(s + 0x18) = 15; /* capacity, SSO */
}

/* Reads "host[:port]" from fh2mp-connect.txt once; renames it to .done after use. */
static void maybe_run_connect_probe(unsigned char *img)
{
    wchar_t path[MAX_PATH];
    wchar_t done[MAX_PATH];
    char raw[128];
    char host[64];
    char port[16];
    DWORD got = 0;
    HANDLE f;
    unsigned i;
    unsigned n = 0;
    void *mgr;
    unsigned char *req;
    unsigned char ipstr[32];
    unsigned char portstr[32];

    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-connect.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!ReadFile(f, raw, sizeof raw - 1, &got, NULL)) {
        got = 0;
    }
    CloseHandle(f);
    raw[got] = '\0';
    mgr = *(void **)(img + FH2_RVA_NET_MANAGER_PTR);
    if (!mgr) {
        return; /* network manager not constructed yet; try again later */
    }
    lstrcpyW(done, g_dir);
    lstrcatW(done, L"\\fh2mp-connect.done");
    MoveFileExW(path, done, MOVEFILE_REPLACE_EXISTING);

    for (i = 0; raw[i] && raw[i] != ':' && raw[i] != '\r' && raw[i] != '\n' && n < sizeof host - 1; i++) {
        host[n++] = raw[i];
    }
    host[n] = '\0';
    n = 0;
    if (raw[i] == ':') {
        for (i++; raw[i] && raw[i] != '\r' && raw[i] != '\n' && n < sizeof port - 1; i++) {
            port[n++] = raw[i];
        }
    }
    if (n == 0) {
        port[n++] = '5';
        port[n++] = '0';
        port[n++] = '0';
        port[n++] = '0';
    }
    port[n] = '\0';

    req = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!req) {
        return;
    }
    make_std_string(ipstr, host);
    make_std_string(portstr, port);
    log_begin();
    log_raw("probe: connecting via game join path host=");
    log_raw(host);
    log_raw(" port=");
    log_raw(port);
    log_raw(" manager=");
    log_hex((unsigned long long)(ULONG_PTR)mgr, 16);
    log_raw("\n");
    arm_trace_file();
    if (trace_count()) {
        trace_phase_reset("connect probe");
    }
    ((build_req_fn)(img + FH2_RVA_BUILD_REQUEST))(req, ipstr, portstr, NULL);
    log_begin();
    log_raw("probe: request built; first bytes:\n");
    {
        unsigned off;
        for (off = 0; off < 0xd0; off += 16) {
            unsigned j;
            log_raw("  ");
            log_hex(off, 4);
            log_raw(": ");
            for (j = 0; j < 16; j++) {
                log_hex(req[off + j], 2);
                log_raw(" ");
            }
            log_raw(" |");
            for (j = 0; j < 16; j++) {
                char c[2];
                c[0] = (req[off + j] >= 32 && req[off + j] < 127) ? (char)req[off + j] : '.';
                c[1] = '\0';
                log_raw(c);
            }
            log_raw("|\n");
        }
    }
    log_begin();
    log_raw("probe: calling begin-join\n");
    ((begin_join_fn)(img + FH2_RVA_BEGIN_JOIN))(mgr, req);
    log_begin();
    log_raw("probe: begin-join returned\n");
}

#include "xhr2.inc"
#include "dbg.inc"

/*
 * Watches the game's option singleton. Logs the FreeRoam direct-IP slot and related flags as they
 * change; if the singleton is up and the slot is still empty, fills it from our argument.
 */
static DWORD WINAPI option_monitor(LPVOID unused)
{
    unsigned char *img = NULL;
    unsigned char *opts;
    char want[FH2_OPT_IP_SLOT_SIZE];
    int have_want;
    char want_rt[FH2_OPT_IP_SLOT_SIZE];
    int have_want_rt;
    int have_single_events;
    int patched = 0;
    int patched_rt = 0;
    int tick;
    unsigned char last[FH2_OPT_IP_SLOT_SIZE + 4];
    int first = 1;

    (void)unused;
    have_want = arg_value_ascii(L"NetSessionDirectIpFreeRoam=", want, sizeof want);
    have_want_rt = arg_value_ascii(L"NetSessionDirectIpRoadTrip=", want_rt, sizeof want_rt);
    have_single_events = has_arg_prefix(L"NetSessionEnableSinglePlayerFreeRoamEvents");
    for (tick = 0; tick < 60 && !img; tick++) {
        img = find_fh2_image();
        if (!img) {
            Sleep(500);
        }
    }
    if (!img) {
        log_begin();
        log_raw("monitor: FH2 image not found\n");
        return 0;
    }
    opts = img + FH2_OPTIONS_RVA;
    for (tick = 0; tick < 24000; tick++) { /* ~3.3 h of 0.5 s ticks */
        /* Azimuth binds the game's imports after the helper loads; keep our pointers on top */
        patch_game_winsock(img);
        patch_game_cocreate(img);
        unsigned char cur[FH2_OPT_IP_SLOT_SIZE + 4];
        unsigned i;
        int changed = 0;

        {
            static wchar_t last_cwd[MAX_PATH];
            wchar_t cwd[MAX_PATH];
            DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
            if (n > 0 && n < MAX_PATH && lstrcmpW(cwd, last_cwd) != 0) {
                lstrcpyW(last_cwd, cwd);
                log_begin();
                log_raw("cwd: ");
                log_wide(cwd);
                log_raw("\n");
            }
        }
        /* on-demand trace arming must work even before the game reads its options */
        {
            wchar_t apath[MAX_PATH];
            wchar_t adone[MAX_PATH];
            lstrcpyW(apath, g_dir);
            lstrcatW(apath, L"\\fh2mp-arm.txt");
            if (GetFileAttributesW(apath) != INVALID_FILE_ATTRIBUTES) {
                lstrcpyW(adone, g_dir);
                lstrcatW(adone, L"\\fh2mp-arm.done");
                MoveFileExW(apath, adone, MOVEFILE_REPLACE_EXISTING);
                arm_trace_file();
            }
        }
        if (!(img[FH2_OPTIONS_INIT_FLAG_RVA] & 1)) {
            Sleep(500);
            continue;
        }
        if (tick >= 30) {
            maybe_run_connect_probe(img);
        }
        for (i = 0; i < FH2_OPT_IP_SLOT_SIZE; i++) {
            cur[i] = opts[FH2_OPT_DIRECTIP_FREEROAM + i];
        }
        cur[FH2_OPT_IP_SLOT_SIZE] = opts[FH2_OPT_BYPASS_MATCHMAKING];
        cur[FH2_OPT_IP_SLOT_SIZE + 1] = opts[FH2_OPT_LOCAL_LOBBY_HOPPER];
        cur[FH2_OPT_IP_SLOT_SIZE + 2] = opts[FH2_OPT_ENABLE_DEBUG_IP_LIST];
        for (i = 0; i < sizeof cur; i++) {
            if (first || cur[i] != last[i]) {
                changed = 1;
            }
            last[i] = cur[i];
        }
        if (changed) {
            log_begin();
            log_raw(first ? "monitor: options singleton initialised\n" : "monitor: options changed\n");
            log_slot("  NetSessionDirectIpFreeRoam", &opts[FH2_OPT_DIRECTIP_FREEROAM]);
            log_begin();
            log_raw("  BypassMatchmaking=");
            log_dec(cur[FH2_OPT_IP_SLOT_SIZE]);
            log_raw(" LocalLobbyHopper=");
            log_dec(cur[FH2_OPT_IP_SLOT_SIZE + 1]);
            log_raw(" EnableDebugIPList=");
            log_dec(cur[FH2_OPT_IP_SLOT_SIZE + 2]);
            log_raw("\n");
            first = 0;
        }
        if (!patched && have_want && opts[FH2_OPT_DIRECTIP_FREEROAM] == 0 && tick >= 20) {
            unsigned n = 0;
            while (want[n] && n < FH2_OPT_IP_SLOT_SIZE - 1) {
                opts[FH2_OPT_DIRECTIP_FREEROAM + n] = (unsigned char)want[n];
                n++;
            }
            opts[FH2_OPT_DIRECTIP_FREEROAM + n] = 0;
            patched = 1;
            log_begin();
            log_raw("monitor: slot was empty after parse, wrote it from argument\n");
        }
        if (!patched_rt && have_want_rt && opts[FH2_OPT_DIRECTIP_ROADTRIP] == 0 && tick >= 20) {
            unsigned n = 0;
            while (want_rt[n] && n < FH2_OPT_IP_SLOT_SIZE - 1) {
                opts[FH2_OPT_DIRECTIP_ROADTRIP + n] = (unsigned char)want_rt[n];
                n++;
            }
            opts[FH2_OPT_DIRECTIP_ROADTRIP + n] = 0;
            patched_rt = 1;
            log_begin();
            log_raw("monitor: RoadTrip slot was empty after parse, wrote it from argument\n");
        }
        if (have_single_events && opts[FH2_OPT_SINGLE_PLAYER_EVENTS] == 0 && tick >= 20) {
            opts[FH2_OPT_SINGLE_PLAYER_EVENTS] = 1;
            log_begin();
            log_raw("monitor: single-player events enabled from argument\n");
        }
        Sleep(1000);
    }
    return 0;
}

/*
 * One-shot trace breakpoints. fh2mp-trace.txt (next to this DLL) lists "<rva-hex> <label>"; each
 * address gets an int3 that logs the first time it executes, then restores the original byte.
 * Gives an ordered "first reached" log of game functions without a disassembler or trampolines.
 */
#define MAX_TRACE 2048
typedef struct {
    unsigned char *addr;
    unsigned char orig;
    char label[80];
    volatile LONG hits;
} trace_bp;
#define TRACE_MULTI_CAP 60 /* max logged hits per address in multi mode */
#define TRACE_LOG_CAP 6000 /* cap for game-logger hooks (label starts with "LOG") */
static int g_trace_multi;
static DWORD g_trace_tls = TLS_OUT_OF_INDEXES;
static trace_bp g_bp[MAX_TRACE];
static int g_nbp;
static unsigned char *g_trace_img;

static int trace_count(void)
{
    return g_nbp;
}

static void trace_phase_reset(const char *why)
{
    int k;
    for (k = 0; k < g_nbp; k++) {
        DWORD old2;
        g_bp[k].hits = 0;
        if (VirtualProtect(g_bp[k].addr, 1, PAGE_EXECUTE_READWRITE, &old2) &&
            *(volatile unsigned char *)g_bp[k].addr != 0xCC) {
            *(volatile unsigned char *)g_bp[k].addr = 0xCC;
        }
    }
    log_begin();
    log_raw("trace: ---- phase reset (");
    log_raw(why);
    log_raw("): all breakpoints re-armed ----\n");
}

static void log_cxx_exception(EXCEPTION_POINTERS *ep)
{
    static volatile LONG count;
    LONG n = InterlockedIncrement(&count);
    void *frames[12];
    USHORT got;
    USHORT i;
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);

    if (n > 200) {
        return;
    }
    got = RtlCaptureStackBackTrace(0, 12, frames, NULL);
    log_begin();
    log_raw("cxx-exception #");
    log_dec((unsigned long)n);
    log_raw(" tid=");
    log_dec(GetCurrentThreadId());
    log_raw(" obj=");
    log_hex(ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0, 16);
    log_raw(" bt:");
    for (i = 0; i < got; i++) {
        unsigned char *f = (unsigned char *)frames[i];
        log_raw(" ");
        if (g_trace_img && f >= g_trace_img && f < g_trace_img + FH2_SIZE_OF_IMAGE) {
            log_raw("fh2+");
            log_hex((unsigned long long)(f - g_trace_img), 8);
        } else if (f >= exe && f < exe + 0x352000) {
            log_raw("azimuth+");
            log_hex((unsigned long long)(f - exe), 8);
        } else {
            log_raw("?");
            log_hex((unsigned long long)(ULONG_PTR)f, 12);
        }
    }
    log_raw("\n");
}

static int contains_str(const char *hay, const char *needle)
{
    for (; *hay; hay++) {
        const char *h = hay;
        const char *n = needle;
        while (*n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return 1;
        }
    }
    return 0;
}

#define VT_MAX 2048
static unsigned long long g_vt_seen[VT_MAX];
static int g_vt_n;

static void log_vtable_once(EXCEPTION_POINTERS *ep, const char *label)
{
    unsigned long long vt = 0;
    const unsigned long long *obj = (const unsigned long long *)(ULONG_PTR)ep->ContextRecord->Rcx;
    int i;

    if (!IsBadReadPtr(obj, 8)) {
        vt = *obj;
    }
    if (!vt || !g_trace_img || vt < (unsigned long long)(ULONG_PTR)g_trace_img ||
        vt > (unsigned long long)(ULONG_PTR)g_trace_img + FH2_SIZE_OF_IMAGE) {
        return;
    }
    for (i = 0; i < g_vt_n; i++) {
        if (g_vt_seen[i] == vt) {
            return;
        }
    }
    if (g_vt_n < VT_MAX) {
        g_vt_seen[g_vt_n++] = vt;
    }
    log_begin();
    log_raw("vtbl: ");
    log_raw(label);
    log_raw(" rva=");
    log_hex(vt - (unsigned long long)(ULONG_PTR)g_trace_img, 8);
    log_raw("\n");
}

static int log_is_noise(const char *s)
{
    static const char *const noisy[] = {
        "PackingVar", "PackObjectId", "PackObjectTypeId", "Send:Packet", "Finalize", "Queued Time",
        "Process Time", "PING_RTT", "RTT", "SetPacketId", "[Conn(%d)][MSG:", "[Conn(%d)][PKT:",
        "Link(", "Recv:PacketHeader", "Send:%s:%d", "[Ping]", "[Pong]", "CarPrediction",
        "ServerTraffic", "RepairDamage", "TimeSync::Update", "ClearRelayObjects",
    };
    unsigned i;

    for (i = 0; i < sizeof noisy / sizeof noisy[0]; i++) {
        if (contains_str(s, noisy[i])) {
            return 1;
        }
    }
    return 0;
}

static void log_ptr_string(const char *label, unsigned long long v)
{
    const char *p = (const char *)(ULONG_PTR)v;
    char buf[48];
    int k = 0;

    if (v < 0x10000 || IsBadReadPtr(p, 4)) {
        return;
    }
    while (k < 40 && !IsBadReadPtr(p + k, 1) && p[k] >= 32 && p[k] < 127) {
        buf[k] = p[k];
        k++;
    }
    if (k >= 3 && (p[k] == '\0')) {
        buf[k] = '\0';
        log_raw(label);
        log_raw("\"");
        log_raw(buf);
        log_raw("\"");
    }
}

static LONG CALLBACK trace_veh(EXCEPTION_POINTERS *ep)
{
    if (dbg_handle(ep)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    int i;
    void *a;

    if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363u) {
        log_cxx_exception(ep);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
        trace_bp *pending = (trace_bp *)TlsGetValue(g_trace_tls);
        if (pending) {
            TlsSetValue(g_trace_tls, NULL);
            *(volatile unsigned char *)pending->addr = 0xCC; /* re-arm */
            FlushInstructionCache(GetCurrentProcess(), pending->addr, 1);
            ep->ContextRecord->EFlags &= ~0x100u;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    a = ep->ExceptionRecord->ExceptionAddress;
    for (i = 0; i < g_nbp; i++) {
        if (g_bp[i].addr == (unsigned char *)a) {
            trace_bp *bp = &g_bp[i];
            unsigned char cur = *(volatile unsigned char *)a;

            if (cur == 0xCC) {
                DWORD old;
                LONG n = InterlockedIncrement(&bp->hits);
                int is_log = bp->label[0] == 'L' && bp->label[1] == 'O' && bp->label[2] == 'G';
                int is_vt = bp->label[0] == 'V' && bp->label[1] == 'T';
                LONG cap = is_vt ? 2000000 : ((is_log || bp->label[0] == 'X' || bp->label[0] == 'A') ? TRACE_LOG_CAP : TRACE_MULTI_CAP);
                if (g_trace_multi ? n <= cap : n == 1) {
                    if (is_vt) {
                        log_vtable_once(ep, bp->label);
                    } else if (bp->label[0] == 'A' && bp->label[1] == 'R' && bp->label[2] == 'G') {
                        /* ARG*: log rcx/rdx/r8/r9 and, when r9 points at readable memory, 32 bytes of it */
                        const unsigned char *o = (const unsigned char *)(ULONG_PTR)ep->ContextRecord->R9;
                        int q;
                        log_begin();
                        log_raw("arg: ");
                        log_raw(bp->label);
                        log_raw(" rcx=");
                        log_hex(ep->ContextRecord->Rcx, 16);
                        log_raw(" rdx=");
                        log_hex(ep->ContextRecord->Rdx, 16);
                        log_raw(" r8=");
                        log_hex(ep->ContextRecord->R8, 16);
                        log_raw(" r9=");
                        log_hex(ep->ContextRecord->R9, 16);
                        log_raw(" rax=");
                        log_hex(ep->ContextRecord->Rax, 16);
                        log_raw(" rbx=");
                        log_hex(ep->ContextRecord->Rbx, 16);
                        if (bp->label[3] == 'V') {
                            /* ARGV*: rdx is a vtable pointer: log [rdx], [rdx+0xe8..0x100] (the slot called) */
                            const unsigned long long *vt = (const unsigned long long *)(ULONG_PTR)ep->ContextRecord->Rdx;
                            const unsigned char *obj = (const unsigned char *)(ULONG_PTR)ep->ContextRecord->Rcx;
                            log_raw(" vt[0]=");
                            if (ep->ContextRecord->Rdx > 0x10000 && !IsBadReadPtr(vt, 8)) {
                                log_hex(vt[0], 16);
                            }
                            log_raw(" vt[30]=");
                            if (ep->ContextRecord->Rdx > 0x10000 && !IsBadReadPtr(vt + 30, 8)) {
                                log_hex(vt[30], 16);
                            }
                            log_raw(" obj[0..32]=");
                            if (ep->ContextRecord->Rcx > 0x10000 && !IsBadReadPtr(obj, 32)) {
                                for (q = 0; q < 32; q++) {
                                    log_hex(obj[q], 2);
                                }
                            }
                            log_raw(" img=");
                            log_hex((unsigned long long)(ULONG_PTR)g_trace_img, 16);
                        } else if (ep->ContextRecord->R9 > 0x10000 && !IsBadReadPtr(o, 32)) {
                            log_raw(" [r9]=");
                            for (q = 0; q < 32; q++) {
                                log_hex(o[q], 2);
                            }
                        }
                        log_raw("\n");
                    } else if (bp->label[0] == 'X' && bp->label[1] == 'L' && bp->label[2] == 'F') {
                        /* Xls logger forwarder: (this, hr rdx, const char *msg r8) */
                        const char *msg = (const char *)(ULONG_PTR)ep->ContextRecord->R8;
                        char mb[200];
                        int q = 0;
                        if (msg && !IsBadReadPtr(msg, 2)) {
                            while (q < (int)sizeof mb - 1 && msg[q] && !IsBadReadPtr(msg + q, 1)) {
                                mb[q] = (msg[q] >= 32 && msg[q] < 127) ? msg[q] : '?';
                                q++;
                            }
                        }
                        mb[q] = '\0';
                        log_begin();
                        log_raw("xls: ");
                        log_raw(bp->label);
                        log_raw(" hr=");
                        log_hex(ep->ContextRecord->Rdx & 0xffffffffu, 8);
                        log_raw(" ");
                        log_raw(mb);
                        log_raw("\n");
                    } else if (bp->label[0] == 'X' && bp->label[1] == 'L' && bp->label[2] == 'S') {
                        /* Xls logger sink: (this, 0, severity r8, hr r9, const char *msg = 5th arg) */
                        const char *msg = NULL;
                        char mb[200];
                        int q = 0;
                        if (!IsBadReadPtr((void *)(ULONG_PTR)(ep->ContextRecord->Rsp + 0x28), 8)) {
                            msg = *(const char **)(ULONG_PTR)(ep->ContextRecord->Rsp + 0x28);
                        }
                        if (msg && !IsBadReadPtr(msg, 2)) {
                            while (q < (int)sizeof mb - 1 && msg[q] && !IsBadReadPtr(msg + q, 1)) {
                                mb[q] = (msg[q] >= 32 && msg[q] < 127) ? msg[q] : '?';
                                q++;
                            }
                        }
                        mb[q] = '\0';
                        log_begin();
                        log_raw("xls: sev=");
                        log_dec((unsigned long)ep->ContextRecord->R8);
                        log_raw(" hr=");
                        log_hex(ep->ContextRecord->R9 & 0xffffffffu, 8);
                        log_raw(" ");
                        log_raw(mb);
                        log_raw("\n");
                    } else if (is_log) {
                        const char *fmt = (const char *)(ULONG_PTR)ep->ContextRecord->Rdx;
                        char buf[140];
                        int k = 0;
                        if (!IsBadReadPtr(fmt, 8)) {
                            while (k < (int)sizeof buf - 1 && fmt[k] && !IsBadReadPtr(fmt + k, 1)) {
                                buf[k] = (fmt[k] >= 32 && fmt[k] < 127) ? fmt[k] : '?';
                                k++;
                            }
                        }
                        buf[k] = '\0';
                        if (log_is_noise(buf)) {
                            InterlockedDecrement(&bp->hits); /* noise does not use the budget */
                        } else {
                            log_begin();
                            log_raw("gamelog: ");
                            log_raw(buf);
                            log_raw(" | r8=");
                            log_hex(ep->ContextRecord->R8, 8);
                            log_raw(" r9=");
                            log_hex(ep->ContextRecord->R9, 8);
                            log_ptr_string(" s8=", ep->ContextRecord->R8);
                            log_ptr_string(" s9=", ep->ContextRecord->R9);
                            log_raw("\n");
                        }
                    } else {
                        unsigned long long ret = 0;
                        if (!IsBadReadPtr((void *)(ULONG_PTR)ep->ContextRecord->Rsp, 8)) {
                            ret = *(unsigned long long *)(ULONG_PTR)ep->ContextRecord->Rsp;
                        }
                        log_begin();
                        log_raw("trace: ");
                        log_raw(bp->label);
                        log_raw(" #");
                        log_dec((unsigned long)n);
                        log_raw(" tid=");
                        log_dec(GetCurrentThreadId());
                        log_raw(" rcx=");
                        log_hex(ep->ContextRecord->Rcx, 16);
                        log_raw(" ret=+");
                        log_hex(ret - (unsigned long long)(ULONG_PTR)g_trace_img, 8);
                        log_raw("\n");
                        if (bp->label[0] == 'R' && bp->label[1] == 'B') {
                            /* RB*: also dump 16 bytes at rcx+0xa0 (object fields) */
                            const unsigned char *o =
                                (const unsigned char *)(ULONG_PTR)(ep->ContextRecord->Rcx + 0xA0);
                            int q;
                            if (!IsBadReadPtr(o, 16)) {
                                log_begin();
                                log_raw("  mem[rcx+0xa0]=");
                                for (q = 0; q < 16; q++) {
                                    log_hex(o[q], 2);
                                    log_raw(" ");
                                }
                                log_raw("\n");
                            }
                        }
                    }
                }
                /* Azimuth may have re-protected the page since we armed it */
                if (!VirtualProtect(a, 1, PAGE_EXECUTE_READWRITE, &old)) {
                    return EXCEPTION_CONTINUE_SEARCH;
                }
                *(volatile unsigned char *)a = bp->orig;
                FlushInstructionCache(GetCurrentProcess(), a, 1);
                if (g_trace_multi && n < cap && g_trace_tls != TLS_OUT_OF_INDEXES) {
                    TlsSetValue(g_trace_tls, bp);
                    ep->ContextRecord->EFlags |= 0x100u; /* single-step, then re-arm */
                }
            }
            if (g_trace_multi && bp->hits == 1 && (contains_str(bp->label, "NetworkFireStartMatchmakingEvent::slot4") ||
                                                   contains_str(bp->label, "NetworkApplyFeatureStateToGame::slot4"))) {
                trace_phase_reset("matchmaking start");
                bp->hits = 1;
                *(volatile unsigned char *)a = bp->orig;
            }
            ep->ContextRecord->Rip = (DWORD64)(ULONG_PTR)a;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int g_trace_veh_ready;
static int g_trace_armed;

static void trace_setup(void)
{
    wchar_t tmp[8];

    if (g_trace_veh_ready) {
        return;
    }
    g_trace_multi = GetEnvironmentVariableW(L"FH2MP_TRACE_MULTI", tmp, 8) > 0 && tmp[0] == L'1';
    if (g_trace_multi) {
        g_trace_tls = TlsAlloc();
    }
    g_trace_img = find_fh2_image();
    AddVectoredExceptionHandler(1, trace_veh);
    g_trace_veh_ready = 1;
}

/* Arms breakpoints listed in fh2mp-trace.txt. Entries: "<rva-hex> <label>" for game code, or
 * "@module.dll!export <label>" for an API export. Called on demand (game already loaded). */
static void arm_trace_file(void)
{
    static char raw[256 * 1024];
    wchar_t path[MAX_PATH];
    HANDLE f;
    DWORD got = 0;
    char *p;
    int i;

    int first_new = g_nbp;
    int k;

    g_trace_armed = 1;
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-trace.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        log_begin();
        log_raw("trace: no fh2mp-trace.txt to arm\n");
        return;
    }
    if (!ReadFile(f, raw, sizeof raw - 1, &got, NULL)) {
        got = 0;
    }
    CloseHandle(f);
    raw[got] = '\0';
    if (!g_trace_img) {
        g_trace_img = find_fh2_image();
    }
    p = raw;
    while (*p && g_nbp < MAX_TRACE) {
        trace_bp *bp = &g_bp[g_nbp];
        unsigned char *addr = NULL;
        int n = 0;
        DWORD old;

        if (*p == '@') {
            char mod[64];
            char fn[64];
            wchar_t wmod[64];
            int m = 0;
            int q = 0;
            HMODULE hm;
            FARPROC fp;
            p++;
            while (*p && *p != '!' && *p != '\n' && m < 63) {
                mod[m++] = *p++;
            }
            mod[m] = '\0';
            if (*p == '!') {
                p++;
            }
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && q < 63) {
                fn[q++] = *p++;
            }
            fn[q] = '\0';
            for (m = 0; mod[m]; m++) {
                wmod[m] = (wchar_t)mod[m];
            }
            wmod[m] = L'\0';
            hm = GetModuleHandleW(wmod);
            fp = hm ? GetProcAddress(hm, fn) : NULL;
            addr = (unsigned char *)fp;
            n = 0;
            bp->label[n++] = '@';
            for (q = 0; fn[q] && n < (int)sizeof bp->label - 1; q++) {
                bp->label[n++] = fn[q];
            }
        } else {
            unsigned long long rva = 0;
            int digits = 0;
            while (hex_digit(*p) >= 0) {
                rva = (rva << 4) | (unsigned)hex_digit(*p);
                p++;
                digits++;
            }
            if (digits && rva && rva < FH2_SIZE_OF_IMAGE && g_trace_img) {
                addr = g_trace_img + rva;
            }
        }
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        while (*p && *p != '\n' && *p != '\r' && n < (int)sizeof bp->label - 1) {
            bp->label[n++] = *p++;
        }
        bp->label[n] = '\0';
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
        if (!addr) {
            continue;
        }
        for (k = 0; k < first_new; k++) {
            if (g_bp[k].addr == addr) {
                addr = NULL; /* already armed by an earlier pass */
            }
        }
        if (!addr || !VirtualProtect(addr, 1, PAGE_EXECUTE_READWRITE, &old)) {
            continue;
        }
        bp->addr = addr;
        bp->orig = *addr;
        bp->hits = 0;
        if (bp->orig == 0xCC) {
            continue;
        }
        g_nbp++;
    }
    for (i = first_new; i < g_nbp; i++) {
        *g_bp[i].addr = 0xCC;
        FlushInstructionCache(GetCurrentProcess(), g_bp[i].addr, 1);
    }
    log_begin();
    log_raw("trace: armed ");
    log_dec((unsigned long)(g_nbp - first_new));
    log_raw(g_trace_multi ? " multi-shot breakpoints\n" : " one-shot breakpoints\n");
}

static void install_trace(void)
{
    trace_setup(); /* VEH only; breakpoints are armed on demand by the connect probe */
    log_begin();
    log_raw("trace: ready (breakpoints arm when fh2mp-connect.txt is used)\n");
}

#include "winmm_forward.inc"

/*
 * Fills the forwarding table. When the Extra Options proxy is present, every export is forwarded to
 * *its* export of the same name (it forwards on to the system winmm itself): the mod does its
 * initialisation inside its exported timeBeginPeriod, which azimuth.exe calls, so the call must reach it.
 */
static void winmm_forward_init(HMODULE mod)
{
    wchar_t path[MAX_PATH];
    HMODULE real;
    int i;

    if (GetSystemDirectoryW(path, MAX_PATH - 16) == 0) {
        return;
    }
    lstrcatW(path, L"\\winmm.dll");
    real = LoadLibraryW(path); /* full path: never resolves back to this proxy */
    if (!real) {
        log_begin();
        log_raw("winmm: cannot load the system winmm.dll, error ");
        log_dec(GetLastError());
        log_raw("\n");
        return;
    }
    for (i = 0; i < WINMM_FWD_COUNT; i++) {
        void *fn = mod ? (void *)GetProcAddress(mod, g_winmm_names[i]) : NULL;
        g_winmm_real[i] = fn ? fn : (void *)GetProcAddress(real, g_winmm_names[i]);
    }
}

static wchar_t g_exe_dir[MAX_PATH];

/* Extra Options 0.2.88 is itself a winmm proxy; it hooks Azimuth's loader import and waits for the
 * stock TLS helper, so it must be loaded before Azimuth gets that far (and before our late init). */
static HMODULE chain_load_mod(void)
{
    wchar_t path[MAX_PATH];
    HMODULE m;

    lstrcpyW(path, g_exe_dir);
    lstrcatW(path, L"\\fh2extra\\winmm.dll");
    log_begin();
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        log_raw("chain: no fh2extra\\winmm.dll present\n");
        return NULL;
    }
    m = LoadLibraryW(path);
    if (m) {
        log_raw("chain: loaded fh2extra\\winmm.dll\n");
    } else {
        log_raw("chain: LoadLibrary fh2extra\\winmm.dll failed, error ");
        log_dec(GetLastError());
        log_raw("\n");
    }
    return m;
}

/*
 * Everything that needs the game image runs here, once Azimuth has loaded its own TLS helper
 * (it maps the game first). Same order as when the shim was the helper itself.
 */
static DWORD WINAPI late_init(LPVOID unused)
{
    int tick;

    (void)unused;
    /* Azimuth shows its launcher first: the helper (and the game) appear only after Play is clicked,
     * which can take arbitrarily long, so wait for it without a limit. */
    while (!GetModuleHandleW(L"azimuth-tls.dll")) {
        Sleep(5);
    }
    for (tick = 0; tick < 12000 && !find_fh2_image(); tick++) { /* up to ~60 s for the game image */
        Sleep(5);
    }
    log_begin();
    log_raw(find_fh2_image() ? "late: game image mapped, applying patches\n"
                             : "late: game image not found, applying what is possible\n");
    if (has_arg_prefix(L"NetSessionDirectIpRoadTrip=")) {
        redirect_roadtrip_group();
    }
    if (has_arg_prefix(L"NetSessionDirectIpFreeRoam=")) {
        redirect_freeroam_group();
        {
            wchar_t tmp2[8];
            if (GetEnvironmentVariableW(L"FH2MP_NET_TRAFFIC", tmp2, 8) == 0) {
                patch_traffic_local_mode();
                patch_traffic_module_offline();
            }
        }
    }
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_WINSOCK", tmp, 8) == 0) {
            patch_game_winsock(find_fh2_image());
        }
    }
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_STS", tmp, 8) == 0) {
            patch_sts_auth();
        }
    }
    install_trace();
    dbg_start();
    /* Always run: it re-applies the Winsock patch and serves on-demand trace arming even when
     * no NetSessionDirectIp argument is given (its option patching is gated on the argument). */
    return option_monitor(NULL);
}

static void init(HINSTANCE self)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, g_exe_dir, MAX_PATH);
    int i;

    if (n == 0 || n >= MAX_PATH) {
        return;
    }
    while (n > 0 && g_exe_dir[n - 1] != L'\\' && g_exe_dir[n - 1] != L'/') {
        n--;
    }
    if (n > 0) {
        g_exe_dir[n - 1] = L'\0';
    }
    /* logs, args and probe files live in data\bin (where the stock helper sits) when it exists */
    lstrcpyW(g_dir, g_exe_dir);
    lstrcatW(g_dir, L"\\data\\bin");
    if (GetFileAttributesW(g_dir) == INVALID_FILE_ATTRIBUTES) {
        lstrcpyW(g_dir, g_exe_dir);
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp.log");
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    log_begin();
    log_raw("--- fh2mp winmm shim attached, tid=");
    log_dec(GetCurrentThreadId());
    log_raw("\n");

    load_args();
    for (i = 0; i < g_argc; i++) {
        log_begin();
        log_raw("arg[");
        log_dec((unsigned long)i);
        log_raw("]=\"");
        log_wide(g_argv[i]);
        log_raw("\"\n");
    }
    install_cmdline_hook();
    winmm_forward_init(chain_load_mod());
    {
        HANDLE t = CreateThread(NULL, 0, late_init, NULL, 0, NULL);
        if (t) {
            CloseHandle(t);
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        init(self);
    }
    return TRUE;
}
