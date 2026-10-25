/*
 * FH2MP shim: fh2-mods\FH2MP.dll, a mod for the FH2 Mod Platform (Trinculo54, MIT; third_party/fh2-mod-platform).
 *
 * The platform is the d3dcompiler_47.dll next to azimuth.exe. It loads every DLL in fh2-mods and calls its fh2mod_init, so FH2MP
 * runs next to the other FH2 mods (FH2 Extra Options keeps winmm.dll, which is its own) and changes the game through the platform's
 * shared hooks: each change is checked against the exe on disk, Extra Options' patches and every other mod's hooks, and is listed in
 * the platform's crash reports.
 *
 * This DLL:
 *   1. hooks azimuth.exe's Platform::Details::GetCmdArguments so the game receives launch
 *      arguments (stock Azimuth always returns argv == [""]), and patches Azimuth's fixed Xbox user id.
 *   2. once the platform says the game's code is ready (mapped, and Extra Options done with it), applies
 *      the game patches (Winsock, matchmaking groups, traffic, STS auth) and starts the monitor.
 *   3. once every mod has started, puts the car database the launcher synced in the game's mediaoverride folder, keeping the one
 *      another mod had there, which it puts back when the game starts without the server's database (offline).
 *
 * Argument sources, in order: env FH2MP_ARGS (space separated), then fh2mp-args.txt in the Azimuth folder's
 * data\bin (one argument per line, '#' starts a comment). Logs and probe files live there too.
 *
 * Only KERNEL32 is used (no CRT).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "fh2mod.h"

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
#ifdef FH2MP_DEBUG_BUILD
static void trace_phase_reset(const char *why);
static int trace_count(void);
static void arm_trace_file(void);
#endif

static wchar_t g_dir[MAX_PATH];

#ifndef FH2MP_SHIM_VERSION
#define FH2MP_SHIM_VERSION "0.0.0-dev"
#endif
#define FH2MP_WSTR2(x) L##x
#define FH2MP_WSTR(x) FH2MP_WSTR2(x)
/* The launcher finds out which shim is installed by reading this marker out of the DLL (it cannot run it). */
const char fh2mp_shim_marker[] __attribute__((used)) = "FH2MP-SHIM-VERSION:" FH2MP_SHIM_VERSION ":END";
/*
 * Import slots Azimuth fills at run time (patch_game_winsock, xhr2.inc) are still written directly, and their pages are left writable:
 * restoring the old protection raced with Azimuth, which binds the game's imports on other threads (0xc0000005 inside memcpy).
 */
#define patch_done(addr, len, old) ((void)(addr), (void)(len), (void)(old))

/*
 * Code changes go through the FH2 Mod Platform. It refuses a change that overlaps another mod's hook or game code someone already
 * changed outside the platform, and says why in fh2-mods\platform.log; we then log "not patched" and carry on without that change.
 * Each change is switched on as soon as it is made: they are independent, and most are made once at start-up.
 */
static const FH2ModApi *g_api;
static FH2Mod *g_mod;

static int plat_patch(const char *name, void *at, const void *expect, const void *bytes, unsigned n)
{
    FH2Hook *h = g_api->patch_bytes(g_mod, name, at, expect, bytes, n);
    return h && g_api->enable_hooks(g_mod, h);
}

/* Replaces the function at `target` with `detour`; `original` (may be NULL) gets what to call for the game's own code. */
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    void *unused;
    FH2Hook *h = g_api->hook_function(g_mod, name, target, detour, original ? original : &unused);
    return h && g_api->enable_hooks(g_mod, h);
}

/* Points one `call rel32` at `detour`; `original` gets the function it called. */
static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    FH2Hook *h = g_api->hook_call(g_mod, name, site, detour, original);
    return h && g_api->enable_hooks(g_mod, h);
}

/* Commits one page of executable memory within +-2 GB of `anchor` (so a rip-relative operand reaches it), nearest free region first,
 * upward and downward alternately. A fixed window after the module is not enough: on some Windows address-space layouts every 64 KB
 * slot there is taken (reported 2026-10-19). For the data our patches point game code at (a call slot, a longer path string). */
static unsigned char *alloc_near(unsigned char *anchor)
{
    SYSTEM_INFO si;
    ULONG_PTR lo, hi, up, down;
    int step;

    GetSystemInfo(&si);
    lo = (ULONG_PTR)anchor > 0x7FF00000u ? (ULONG_PTR)anchor - 0x7FF00000u : 0;
    if (lo < (ULONG_PTR)si.lpMinimumApplicationAddress) {
        lo = (ULONG_PTR)si.lpMinimumApplicationAddress;
    }
    hi = (ULONG_PTR)anchor + 0x7FF00000u;
    if (hi > (ULONG_PTR)si.lpMaximumApplicationAddress) {
        hi = (ULONG_PTR)si.lpMaximumApplicationAddress;
    }
    up = down = (ULONG_PTR)anchor;
    for (step = 0; step < 200000 && (up < hi || down > lo); step++) {
        MEMORY_BASIC_INFORMATION mbi;
        unsigned char *p;

        if (up < hi) {
            if (!VirtualQuery((void *)up, &mbi, sizeof mbi)) {
                up = hi;
            } else {
                ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
                ULONG_PTR cand = (base + 0xFFFFu) & ~(ULONG_PTR)0xFFFFu;
                if (mbi.State == MEM_FREE && cand + 0x1000 <= end && cand + 0x1000 <= hi && cand >= lo &&
                    (p = (unsigned char *)VirtualAlloc((void *)cand, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
                    return p;
                }
                up = end > up ? end : up + 0x1000;
            }
        }
        if (down > lo) {
            if (!VirtualQuery((void *)(down - 1), &mbi, sizeof mbi)) {
                down = lo;
            } else {
                ULONG_PTR base = (ULONG_PTR)mbi.BaseAddress, end = base + mbi.RegionSize;
                ULONG_PTR cand = (end - 0x1000) & ~(ULONG_PTR)0xFFFFu;
                if (mbi.State == MEM_FREE && cand >= base && cand >= lo && cand + 0x1000 <= hi &&
                    (p = (unsigned char *)VirtualAlloc((void *)cand, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))) {
                    return p;
                }
                down = base < down ? base : down - 0x1000;
            }
        }
    }
    return NULL;
}

/* Session token of the player signed in through the launcher (see session_load); sent to the stand-in services as X-FH2MP-Token. */
static char g_token[96];
static HANDLE g_log = INVALID_HANDLE_VALUE;
static wchar_t g_args[MAX_ARGS][MAX_ARG_CHARS];
static wchar_t *g_argv[MAX_ARGS + 1];
static int g_argc;

static int mem_equal(const unsigned char *a, const unsigned char *b, SIZE_T n)
{
    while (n--) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return 1;
}

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
#ifndef FH2MP_DEBUG_BUILD
static char g_line[600];
static int g_ll;

static int line_has(const char *l, int n, const char *pat)
{
    int i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; pat[k] && i + k < n && l[i + k] == pat[k]; k++) {
        }
        if (!pat[k]) {
            return 1;
        }
    }
    return 0;
}

static int line_wanted(const char *l, int n)
{
    static const char *const keep[] = {"attached", "session:", "identity:", "late:", "not patched", "NOT ", "unexpected", "fail", "error", "could not",
                                       "refus", "crash", "xsbody", "bindfix", "gamedb:", "rivalsleave", "probe:", 0};
    int i;
    for (i = 0; keep[i]; i++) {
        if (line_has(l, n, keep[i])) {
            return 1;
        }
    }
    return 0;
}
#endif

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
#ifndef FH2MP_DEBUG_BUILD
    /* Release builds keep the log short: lines are assembled here and only the ones that matter to a player are written
     * (start-up summary, anything that failed). The debug build logs everything. */
    {
        DWORD i;
        for (i = 0; i < n; i++) {
            if (g_ll < (int)sizeof g_line - 1) {
                g_line[g_ll++] = s[i];
            }
        }
        if (n > 0 && s[n - 1] == '\n') {
            g_line[g_ll] = 0;
            if (line_wanted(g_line, g_ll)) {
                WriteFile(g_log, g_line, (DWORD)g_ll, &written, NULL);
            }
            g_ll = 0;
            log_unlock();
        }
        return;
    }
#endif
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

/*
 * Implicit bind. The game never calls bind(): its UDP socket (AF_INET6, IPV6_V6ONLY cleared by the game) gets a WSARecvFrom posted at
 * once and relies on that call binding the socket to an ephemeral port, which Linux (and so Wine) does. Windows rejects a receive on an
 * unbound socket with WSAEINVAL (10022); the game then drops the socket, the later send fails with WSAENOTSOCK and no packet ever leaves
 * ("Unable to join session", found with the debug tracer on 2026-10-19). So before a receive, a socket that has no local address yet is
 * bound to the wildcard address and port 0, which is what the implicit bind would have done. The game's WSARecvFrom and recvfrom slots
 * point at the wrappers below in every build.
 */
typedef UINT_PTR fh_sock;
typedef int(WINAPI *fh_f_wsarecvfrom)(fh_sock, void *, DWORD, DWORD *, DWORD *, void *, int *, void *, void *);
typedef int(WINAPI *fh_f_recvfrom)(fh_sock, char *, int, int, void *, int *);
typedef int(WINAPI *fh_f_bind)(fh_sock, const void *, int);
typedef int(WINAPI *fh_f_getsockname)(fh_sock, void *, int *);
static fh_f_wsarecvfrom g_r_wsarecvfrom;
static fh_f_recvfrom g_r_recvfrom;
static fh_f_bind g_r_bind;
static fh_f_getsockname g_r_getsockname;

static void fh_ensure_bound(fh_sock s)
{
    unsigned char name[28];
    int nl = 28, i;
    DWORD err;

    if (s == (fh_sock)~(UINT_PTR)0) {
        return;
    }
    if (!g_r_bind || !g_r_getsockname) {
        HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");

        if (!ws2) {
            return;
        }
        g_r_getsockname = (fh_f_getsockname)(void *)GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)6);
        g_r_bind = (fh_f_bind)(void *)GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)2);
        if (!g_r_bind || !g_r_getsockname) {
            return;
        }
    }
    if (g_r_getsockname(s, name, &nl) == 0) {
        return; /* already has a local address */
    }
    err = GetLastError();
    if (err != 10022) { /* WSAEINVAL = not bound; anything else (not a socket, ...) is the game's business */
        SetLastError(err);
        return;
    }
#ifdef FH2MP_DEBUG_BUILD
    {
        wchar_t p[MAX_PATH];

        lstrcpyW(p, g_dir);
        lstrcatW(p, L"\\fh2mp-nobindfix.txt"); /* A/B switch for the tracer: the file turns the fix off */
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
            SetLastError(err);
            return;
        }
    }
#endif
    for (i = 0; i < 28; i++) {
        name[i] = 0;
    }
    name[0] = 23; /* AF_INET6, :: port 0 (the game's socket is dual-stack) */
    if (g_r_bind(s, name, 28) != 0) {
        unsigned char sa4[16] = {2, 0}; /* not an IPv6 socket after all: 0.0.0.0:0 */

        g_r_bind(s, sa4, 16);
    }
    nl = 28;
    log_begin();
    log_raw("bindfix: receive on an unbound socket 0x");
    log_hex(s, 4);
    if (g_r_getsockname(s, name, &nl) == 0) {
        log_raw(": bound to port ");
        log_dec(((unsigned)name[2] << 8) | name[3]);
    } else {
        log_raw(": bind failed, error ");
        log_dec(GetLastError());
    }
    log_raw("\n");
    SetLastError(err);
}

#ifdef FH2MP_DEBUG_BUILD
/*
 * Winsock tracer (debug builds only). The game's WS2_32 slots are pointed at these wrappers, which log the calls that decide whether the
 * UDP connection can work on this platform and then call the real function. Logged: WSASocketW (family, type, and the socket's IPV6_V6ONLY
 * value right after creation), setsockopt, getaddrinfo, WSAStartup, and the first calls plus every failure of the send and receive
 * functions, with the destination address. (It showed that the game sets IPV6_V6ONLY=0 itself, and that its first WSARecvFrom fails with
 * 10022 on Windows: see fh_ensure_bound above.)
 */
typedef int(WINAPI *fh_f_startup)(WORD, void *);
typedef fh_sock(WINAPI *fh_f_socketw)(int, int, int, void *, unsigned, DWORD);
typedef int(WINAPI *fh_f_setsockopt)(fh_sock, int, int, const char *, int);
typedef int(WINAPI *fh_f_getsockopt)(fh_sock, int, int, char *, int *);
typedef int(WINAPI *fh_f_wsasendto)(fh_sock, void *, DWORD, DWORD *, DWORD, const void *, int, void *, void *);
typedef int(WINAPI *fh_f_sendto)(fh_sock, const char *, int, int, const void *, int);
typedef int(WINAPI *fh_f_getaddrinfo)(const char *, const char *, const void *, void **);

static fh_f_startup g_r_startup;
static fh_f_socketw g_r_socketw;
static fh_f_setsockopt g_r_setsockopt;
static fh_f_wsasendto g_r_wsasendto;
static fh_f_sendto g_r_sendto;
static fh_f_getaddrinfo g_r_getaddrinfo;
static volatile LONG g_n_sock, g_n_sockopt, g_n_gai, g_n_send, g_n_sendfail, g_n_recv, g_n_recvfail, g_n_start;

static void fh_log_sa(const void *p, int len)
{
    const unsigned char *a = (const unsigned char *)p;
    unsigned fam;
    int i;

    if (!a || len < 4) {
        log_raw("(none)");
        return;
    }
    fam = (unsigned)a[0] | ((unsigned)a[1] << 8);
    if (fam == 2 && len >= 8) {
        log_raw("inet ");
        for (i = 0; i < 4; i++) {
            log_dec(a[4 + i]);
            if (i < 3) {
                log_raw(".");
            }
        }
    } else if (fam == 23 && len >= 24) {
        int mapped = 1;
        for (i = 8; i < 18; i++) {
            if (a[i]) {
                mapped = 0;
            }
        }
        mapped = mapped && a[18] == 0xff && a[19] == 0xff;
        log_raw("inet6 ");
        if (mapped) {
            log_raw("::ffff:");
            for (i = 0; i < 4; i++) {
                log_dec(a[20 + i]);
                if (i < 3) {
                    log_raw(".");
                }
            }
        } else {
            for (i = 0; i < 16; i++) {
                log_hex(a[8 + i], 2);
                if (i & 1 && i < 15) {
                    log_raw(":");
                }
            }
        }
    } else {
        log_raw("family ");
        log_dec(fam);
        return;
    }
    log_raw(" port ");
    log_dec(((unsigned)a[2] << 8) | a[3]);
}

static void fh_log_int(int v)
{
    if (v < 0) {
        log_raw("-");
        log_dec((unsigned long)(-(long)v));
    } else {
        log_dec((unsigned long)v);
    }
}

static void fh_log_str(const char *s)
{
    char buf[72];
    int i;

    if (!s) {
        log_raw("(null)");
        return;
    }
    for (i = 0; i < 70 && s[i]; i++) {
        buf[i] = s[i];
    }
    buf[i] = 0;
    log_raw(buf);
}

static int fh_v6only_of(fh_sock s)
{
    HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");
    fh_f_getsockopt gso = ws2 ? (fh_f_getsockopt)GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)7) : NULL;
    int v = 0, len = 4;

    if (gso && gso(s, 41 /* IPPROTO_IPV6 */, 27 /* IPV6_V6ONLY */, (char *)&v, &len) != 0) {
        v = -2;
    }
    return v;
}

static int WINAPI fh_startup(WORD ver, void *data)
{
    int r = g_r_startup(ver, data);
    DWORD err = GetLastError();

    if (InterlockedIncrement(&g_n_start) <= 10) {
        log_begin();
        log_raw("ws: WSAStartup version 0x");
        log_hex(ver, 4);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static fh_sock WINAPI fh_socketw(int af, int type, int proto, void *info, unsigned group, DWORD flags)
{
    fh_sock s = g_r_socketw(af, type, proto, info, group, flags);
    DWORD err = GetLastError();

    if (InterlockedIncrement(&g_n_sock) <= 40) {
        int v6 = -3;

        if (s != (fh_sock)~(UINT_PTR)0 && af == 23) {
            v6 = fh_v6only_of(s);
        }
        log_begin();
        log_raw("ws: WSASocketW af=");
        log_dec((unsigned long)af);
        log_raw(" type=");
        log_dec((unsigned long)type);
        log_raw(" proto=");
        log_dec((unsigned long)proto);
        log_raw(" flags=0x");
        log_hex(flags, 4);
        if (s == (fh_sock)~(UINT_PTR)0) {
            log_raw(" -> INVALID_SOCKET err ");
            log_dec(err);
        } else {
            log_raw(" -> socket 0x");
            log_hex(s, 4);
            if (af == 23) {
                log_raw(" IPV6_V6ONLY=");
                fh_log_int(v6); /* 0 or 1; -2 = could not be read */
            }
        }
        log_raw("\n");
    }
    SetLastError(err);
    return s;
}

static int WINAPI fh_setsockopt(fh_sock s, int level, int opt, const char *val, int len)
{
    int r = g_r_setsockopt(s, level, opt, val, len);
    DWORD err = GetLastError();

    if (InterlockedIncrement(&g_n_sockopt) <= 60) {
        log_begin();
        log_raw("ws: setsockopt s=0x");
        log_hex(s, 4);
        log_raw(" level=");
        log_dec((unsigned long)level);
        log_raw(" opt=");
        log_dec((unsigned long)opt);
        log_raw(" val=");
        log_dec(val && len >= 4 ? *(const unsigned *)val : 0xffffffffu);
        log_raw(" len=");
        log_dec((unsigned long)len);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r) {
            log_raw(" err ");
            log_dec(err);
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static int WINAPI fh_getaddrinfo(const char *node, const char *service, const void *hints, void **res)
{
    int r = g_r_getaddrinfo(node, service, hints, res);
    DWORD err = GetLastError();

    if (InterlockedIncrement(&g_n_gai) <= 40) {
        log_begin();
        log_raw("ws: getaddrinfo node=");
        fh_log_str(node);
        log_raw(" service=");
        fh_log_str(service);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r == 0 && res && *res) {
            const unsigned char *ai = (const unsigned char *)*res;

            log_raw(" first: family ");
            log_dec(*(const unsigned *)(ai + 4));
            log_raw(" ");
            fh_log_sa(*(void *const *)(ai + 32), (int)*(const SIZE_T *)(ai + 16));
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static int WINAPI fh_wsasendto(fh_sock s, void *bufs, DWORD cnt, DWORD *sent, DWORD flags, const void *to, int tolen, void *ov, void *cr)
{
    int r = g_r_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cr);
    DWORD err = GetLastError();
    LONG n = InterlockedIncrement(&g_n_send);

    if (n <= 25 || (r && InterlockedIncrement(&g_n_sendfail) <= 80)) {
        log_begin();
        log_raw("ws: WSASendTo s=0x");
        log_hex(s, 4);
        log_raw(" bufs=");
        log_dec(cnt);
        log_raw(" len=");
        log_dec(bufs && cnt ? *(const ULONG *)bufs : 0);
        log_raw(" to ");
        fh_log_sa(to, tolen);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r) {
            log_raw(" err ");
            log_dec(err);
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static int WINAPI fh_sendto(fh_sock s, const char *buf, int len, int flags, const void *to, int tolen)
{
    int r = g_r_sendto(s, buf, len, flags, to, tolen);
    DWORD err = GetLastError();
    LONG n = InterlockedIncrement(&g_n_send);

    if (n <= 25 || (r < 0 && InterlockedIncrement(&g_n_sendfail) <= 80)) {
        log_begin();
        log_raw("ws: sendto s=0x");
        log_hex(s, 4);
        log_raw(" len=");
        log_dec((unsigned long)len);
        log_raw(" to ");
        fh_log_sa(to, tolen);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r < 0) {
            log_raw(" err ");
            log_dec(err);
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static int WINAPI fh_wsarecvfrom(fh_sock s, void *bufs, DWORD cnt, DWORD *got, DWORD *flags, void *from, int *fromlen, void *ov, void *cr)
{
    int r;
    DWORD err;
    LONG n = InterlockedIncrement(&g_n_recv);

    fh_ensure_bound(s);
    r = g_r_wsarecvfrom(s, bufs, cnt, got, flags, from, fromlen, ov, cr);
    err = GetLastError();

    if (n <= 12 || (r && err != 997 && InterlockedIncrement(&g_n_recvfail) <= 80)) {
        log_begin();
        log_raw("ws: WSARecvFrom s=0x");
        log_hex(s, 4);
        log_raw(" overlapped=");
        log_dec(ov ? 1 : 0);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r) {
            log_raw(" err ");
            log_dec(err);
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

static int WINAPI fh_recvfrom(fh_sock s, char *buf, int len, int flags, void *from, int *fromlen)
{
    int r;
    DWORD err;
    LONG n = InterlockedIncrement(&g_n_recv);

    fh_ensure_bound(s);
    r = g_r_recvfrom(s, buf, len, flags, from, fromlen);
    err = GetLastError();

    if (n <= 12 || (r < 0 && err != 10035 && InterlockedIncrement(&g_n_recvfail) <= 80)) {
        log_begin();
        log_raw("ws: recvfrom s=0x");
        log_hex(s, 4);
        log_raw(" -> ");
        log_dec((unsigned long)r);
        if (r < 0) {
            log_raw(" err ");
            log_dec(err);
        }
        log_raw("\n");
    }
    SetLastError(err);
    return r;
}

/* which wrapper stands in for IAT slot i (g_ws2_slots order); the real function is remembered for the wrapper to call */
static void *ws2_target(unsigned i, void *real)
{
    switch (i) {
    case 0: g_r_recvfrom = (fh_f_recvfrom)real; return (void *)fh_recvfrom;
    case 2: g_r_sendto = (fh_f_sendto)real; return (void *)fh_sendto;
    case 3: g_r_setsockopt = (fh_f_setsockopt)real; return (void *)fh_setsockopt;
    case 5: g_r_startup = (fh_f_startup)real; return (void *)fh_startup;
    case 10: g_r_wsarecvfrom = (fh_f_wsarecvfrom)real; return (void *)fh_wsarecvfrom;
    case 12: g_r_wsasendto = (fh_f_wsasendto)real; return (void *)fh_wsasendto;
    case 14: g_r_socketw = (fh_f_socketw)real; return (void *)fh_socketw;
    case 15: g_r_getaddrinfo = (fh_f_getaddrinfo)real; return (void *)fh_getaddrinfo;
    default: return real;
    }
}
#else
/* release build: only the two receive slots are wrapped (implicit bind, see fh_ensure_bound) */
static int WINAPI fh_wsarecvfrom(fh_sock s, void *bufs, DWORD cnt, DWORD *got, DWORD *flags, void *from, int *fromlen, void *ov, void *cr)
{
    fh_ensure_bound(s);
    return g_r_wsarecvfrom(s, bufs, cnt, got, flags, from, fromlen, ov, cr);
}

static int WINAPI fh_recvfrom(fh_sock s, char *buf, int len, int flags, void *from, int *fromlen)
{
    fh_ensure_bound(s);
    return g_r_recvfrom(s, buf, len, flags, from, fromlen);
}

static void *ws2_target(unsigned i, void *real)
{
    switch (i) {
    case 0: g_r_recvfrom = (fh_f_recvfrom)real; return (void *)fh_recvfrom;
    case 10: g_r_wsarecvfrom = (fh_f_wsarecvfrom)real; return (void *)fh_wsarecvfrom;
    default: return real;
    }
}
#endif

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
        void *want = (void *)real;

        if (!real) {
            continue;
        }
#ifdef FH2MP_DEBUG_BUILD
#endif
        want = ws2_target(i, (void *)real);
        module_name_of(*slot, oldmod, sizeof oldmod);
        if (*slot == want) {
            continue; /* already the real function (or its tracer) */
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
        *slot = want;
        patch_done(slot, 8, oldprot);
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
    static const unsigned char zero = 0x00, one = 0x01;
    unsigned char *img = find_fh2_image();
    unsigned char *fn;

    if (!img) {
        return 0;
    }
    fn = img + FH2_TRAFFIC_NETMODE_RVA;
    if (fn[0] != 0xC6 || fn[1] != 0x41 || fn[2] != 0x20 || fn[3] != 0x00 || fn[4] != 0xC3 ||
        !plat_patch("traffic: networked mode stays local", fn + 3, &zero, &one, 1)) {
        log_begin();
        log_raw("traffic: unexpected bytes at set-networked function, not patched\n");
        return 0;
    }
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
    unsigned char jmp[5];
    int i, rel32;

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
    rel32 = (int)(tgt - (fn + 5));
    jmp[0] = 0xE9;
    mem_copy(jmp + 1, &rel32, 4);
    if (!plat_patch("traffic: SyncTrafficModule go-online -> go-offline", fn, on_sig, jmp, 5)) {
        log_begin();
        log_raw("traffic: module online/offline switch not patched\n");
        return 0;
    }
    log_begin();
    log_raw("traffic: module go-online redirected to go-offline; single-player traffic stays active\n");
    return 1;
}

/*
 * Stateflow `network.is_match_making_allowed` (lambda thunk 0x7fff20 -> FUN_14080f6b0) returns 0, and the
 * game answers "matchmaking failed", whenever the CLobby singleton's byte at +0xf0 is 0. Its constructor sets
 * that byte to 1, but under Azimuth's offline platform layer it ends up 0, so the cloud matchmaking flow
 * (NetworkInitializeMatchRequester, cloud config, hopper) never starts. The test is
 *   mov rax,[CLobby] ; cmp [rax+0xf0],bl(=0) ; je -> return 0
 * and the je is turned into a 6-byte nop. The user, sign-in-state and privilege checks that follow stay.
 * Disable with env FH2MP_NO_MMALLOW. See docs/matchmaking.md.
 */
#define FH2_MM_ALLOWED_JE_RVA 0x80f6d3u
static int patch_matchmaking_allowed(void)
{
    /* cmp byte [rax+0xf0],bl  ;  je +0xbd */
    static const unsigned char want[] = {0x38, 0x98, 0xF0, 0x00, 0x00, 0x00, 0x0F, 0x84, 0xBD, 0x00, 0x00, 0x00};
    static const unsigned char nop6[] = {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    int i;

    if (!img) {
        return 0;
    }
    at = img + FH2_MM_ALLOWED_JE_RVA - 6;
    for (i = 0; i < (int)sizeof want; i++) {
        if (at[i] != want[i]) {
            log_begin();
            log_raw("mmallow: unexpected bytes in IsMatchMakingAllowed, not patched\n");
            return 0;
        }
    }
    if (!plat_patch("mmallow: IsMatchMakingAllowed without the CLobby flag", at + 6, want + 6, nop6, 6)) {
        log_begin();
        log_raw("mmallow: IsMatchMakingAllowed not patched\n");
        return 0;
    }
    log_begin();
    log_raw("mmallow: IsMatchMakingAllowed no longer requires the CLobby flag at +0xf0\n");
    return 1;
}

/*
 * Match-request QoS step. When the QoS ticket session comes up, the game measures the QoS servers through the
 * lambda FUN_1417940f0: it asks the platform for a secure-device-association template and calls CreateAssociationAsync;
 * under Azimuth that task throws ("offline secure association creation"), the continuation FUN_14178f140 catches
 * it and records failure ([manager+0x278] = 0), so the match request ends with result 2 and the lobby falls back to
 * Drivatars ("Unable to join session"). This patches the lambda so it always takes its existing "no interface" branch
 * (which only queues the callback and sets the flag) and makes that branch report success: the
 *   test rdi,rdi ; jne <do the association>      -> jne becomes a 2-byte nop        (RVA 0x17941db)
 *   mov byte [rax+0x278], 0                       -> immediate becomes 1            (RVA 0x1794216)
 * The matchmaker then continues to the real ticket request. Disable with env FH2MP_NO_QOSOK. See docs/matchmaking.md.
 */
#define FH2_QOS_JNE_RVA 0x17941dbu
#define FH2_QOS_FLAG_RVA 0x1794216u
static int patch_qos_assume_success(void)
{
    static const unsigned char want_jne[] = {0x48, 0x85, 0xFF, 0x75, 0x56, 0x48, 0x8B, 0x1E};
    static const unsigned char want_flag[] = {0xC6, 0x80, 0x78, 0x02, 0x00, 0x00, 0x00};
    static const unsigned char nop2[] = {0x90, 0x90}, one = 0x01;
    unsigned char *img = find_fh2_image();
    unsigned char *a, *b;
    int i;

    if (!img) {
        return 0;
    }
    a = img + FH2_QOS_JNE_RVA - 3;
    b = img + FH2_QOS_FLAG_RVA - 6;
    for (i = 0; i < (int)sizeof want_jne; i++) {
        if (a[i] != want_jne[i]) {
            log_begin();
            log_raw("qosok: unexpected bytes in the match-request QoS lambda, not patched\n");
            return 0;
        }
    }
    for (i = 0; i < (int)sizeof want_flag; i++) {
        if (b[i] != want_flag[i]) {
            log_begin();
            log_raw("qosok: unexpected flag store in the match-request QoS lambda, not patched\n");
            return 0;
        }
    }
    {
        /* both or neither: half of it would skip the association and still report failure */
        FH2Hook *ha = g_api->patch_bytes(g_mod, "qosok: skip the secure association", a + 3, want_jne + 3, nop2, 2);
        FH2Hook *hb = ha ? g_api->patch_bytes(g_mod, "qosok: report success", b + 6, want_flag + 6, &one, 1) : NULL;
        if (!hb || !g_api->enable_hooks(g_mod, ha) || !g_api->enable_hooks(g_mod, hb)) {
            if (ha) {
                g_api->disable_hooks(g_mod, ha);
            }
            log_begin();
            log_raw("qosok: match-request QoS lambda not patched\n");
            return 0;
        }
    }
    log_begin();
    log_raw("qosok: match-request QoS step reports success without creating a secure association\n");
    return 1;
}

/*
 * Secure device association. Once the lobby session lists a game server, Nerd::MatchRequester (FUN_14179f640) creates a
 * Nerd::SecureIPResolver for it (FUN_1417a0b90 -> FUN_1417b2340 ProcessSecureData -> FUN_1417a0ff0), which asks the
 * platform to CreateAssociationAsync for the server's secure device address; Azimuth's offline layer ends the process
 * there ("STOPPED offline secure association creation"). When the association completes the resolver signals
 * SecureIPResolvedArgs to MatchRequester::OnSecureIPResolved (FUN_1417aec90). With hr == 0 and no endpoint list in the
 * args (+0xd8), that handler builds the endpoint from the ServerInfo host name (the session's
 * servers.gameServerPlatform...hostName) and the literal service "5000", then continues (FUN_14179f440). This replaces
 * FUN_1417a0ff0 with a stub that calls the handler right away with such a result. Disable with env FH2MP_NO_SECUREIP.
 * See docs/matchmaking.md.
 */
#define FH2_SECURE_RESOLVE_RVA 0x17a0ff0u
#define FH2_RESOLVED_HANDLER_RVA 0x17aec90u
#define FH2_MATCHREQUESTER_RVA 0x36e2960u
static unsigned char *g_secure_img;

static void secure_resolve_stub(void *resolver)
{
    void *mr = *(void **)(g_secure_img + FH2_MATCHREQUESTER_RVA);
    unsigned char *args;
    (void)resolver;
    if (!mr) {
        return;
    }
    args = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100); /* hr = 0, endpoints = NULL */
    if (!args) {
        return;
    }
    ((void (*)(void *, void *))(g_secure_img + FH2_RESOLVED_HANDLER_RVA))(mr, args);
}

static int patch_secure_association(void)
{
    static const unsigned char want_fn[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0x50};
    static const unsigned char want_handler[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56};
    unsigned char *img = find_fh2_image();
    int i;

    if (!img) {
        return 0;
    }
    for (i = 0; i < (int)sizeof want_fn; i++) {
        if (img[FH2_SECURE_RESOLVE_RVA + i] != want_fn[i]) {
            log_begin();
            log_raw("secureip: unexpected bytes in SecureIPResolver, not patched\n");
            return 0;
        }
    }
    for (i = 0; i < (int)sizeof want_handler; i++) {
        if (img[FH2_RESOLVED_HANDLER_RVA + i] != want_handler[i]) {
            log_begin();
            log_raw("secureip: unexpected bytes in the resolved handler, not patched\n");
            return 0;
        }
    }
    g_secure_img = img;
    if (!plat_hook("secureip: SecureIPResolver", img + FH2_SECURE_RESOLVE_RVA, (void *)secure_resolve_stub, NULL)) {
        log_begin();
        log_raw("secureip: SecureIPResolver not patched\n");
        return 0;
    }
    log_begin();
    log_raw("secureip: SecureIPResolver reports success without a secure association (host from the session)\n");
    return 1;
}

/*
 * Joining a Car Meet game-mode invite. The Car Meet component (FUN_1407d4790) joins the Free Roam / Road Trip session the
 * invite names with FUN_1417b8c50 (MatchRequester::SetSession: stores the session at +0x118, state 8). The lobby type that
 * the connect request then carries (OpenConnection field a) is MatchRequester+0x160, which only CreatePrivateGame
 * (FUN_1417b6de0) and a match set; a joiner would still send the Car Meet type (3) and land in a Car Meet world. So set it
 * from the synced invite type the way the creator does (FUN_1407d00e0): Road Trip = 1, Free Roam = 2. Takes over the whole
 * function (it is small). Disable with env FH2MP_NO_INVITETYPE.
 */
#define FH2_SETSESSION_RVA 0x17b8c50u
#define FH2_CARMEET_SYNCED_GLOBAL_RVA 0x34d5c68u
#define FH2_WEAKGET_RVA 0x4021b0u
static unsigned char *g_invite_img;

static void set_session_stub(void *mr, void **refp)
{
    unsigned char *img = g_invite_img;
    void *ref = *refp;
    void *old;
    void *synced = *(void **)(img + FH2_CARMEET_SYNCED_GLOBAL_RVA);
    if (synced) {
        void **holder = ((void **(*)(void *))(img + FH2_WEAKGET_RVA))(synced);
        void *obj = holder ? *holder : NULL;
        if (obj) {
            unsigned char invite = ((unsigned char (*)(void *))(*(void ***)obj)[0xa0 / 8])(obj);
            if (invite == 1 || invite == 2) {
                *(int *)((unsigned char *)mr + 0x160) = invite == 2 ? 1 : 2;
                log_begin();
                log_raw(invite == 2 ? "invitetype: joining a Road Trip invite, lobby type 1\n"
                                    : "invitetype: joining a Free Roam invite, lobby type 2\n");
            }
        }
    }
    if (ref) {
        ((void (*)(void *))(*(void ***)ref)[0x10 / 8])(ref);
    }
    old = *(void **)((unsigned char *)mr + 0x118);
    *(void **)((unsigned char *)mr + 0x118) = ref;
    if (old) {
        ((void (*)(void *))(*(void ***)old)[0x18 / 8])(old);
    }
    *(int *)((unsigned char *)mr + 0xb0) = 8;
    if (*refp) {
        ((void (*)(void *))(*(void ***)*refp)[0x18 / 8])(*refp);
    }
}

static int patch_invite_type(void)
{
    static const unsigned char want[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20};
    unsigned char *img = find_fh2_image();
    int i;
    if (!img) {
        return 0;
    }
    for (i = 0; i < (int)sizeof want; i++) {
        if (img[FH2_SETSESSION_RVA + i] != want[i]) {
            log_begin();
            log_raw("invitetype: unexpected bytes in MatchRequester::SetSession, not patched\n");
            return 0;
        }
    }
    g_invite_img = img;
    if (!plat_hook("invitetype: MatchRequester::SetSession", img + FH2_SETSESSION_RVA, (void *)set_session_stub, NULL)) {
        log_begin();
        log_raw("invitetype: MatchRequester::SetSession not patched\n");
        return 0;
    }
    log_begin();
    log_raw("invitetype: joining an invite session sets the lobby type from the invite\n");
    return 1;
}

/*
 * Server port. The resolved-address handler (FUN_1417aec90) builds the endpoint from the ServerInfo host and the literal
 * service string "5000" (a 4-character constant in the game's .rdata, RVA 0x1e9b0f4). Env FH2MP_SERVER_PORT=<4 digits>
 * replaces it, which lets a test client use a different server instance (e.g. one per lobby type).
 */
#define FH2_PORT_LITERAL_RVA 0x1e9b0f4u
static int patch_server_port(void)
{
    WCHAR env[8];
    DWORD n = GetEnvironmentVariableW(L"FH2MP_SERVER_PORT", env, 8);
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    unsigned char port[4];
    int i;

    if (n != 4 || !img) {
        return 0;
    }
    at = img + FH2_PORT_LITERAL_RVA;
    if (at[0] != '5' || at[1] != '0' || at[2] != '0' || at[3] != '0' || at[4] != 0) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (env[i] < L'0' || env[i] > L'9') {
            return 0;
        }
        port[i] = (unsigned char)env[i];
    }
    if (!plat_patch("serverport: game server port", at, "5000", port, 4)) {
        log_begin();
        log_raw("serverport: port literal not patched\n");
        return 0;
    }
    log_begin();
    log_raw("serverport: the game connects to the port from FH2MP_SERVER_PORT\n");
    return 1;
}

/*
 * Online credit difficulty bonus. The post-race "Difficulty and Assists" bonus is base credits * a percentage that
 * FUN_140f58920 sums from the assist settings (registry values UI/Assists/..., set in code). Online (second argument != 0)
 * the AI-difficulty term is the Medium entry (0 %) and there is no player-chosen AI difficulty, so Free Roam never earns
 * the bonus. The AI table is Easy -10 %, Medium 0, Hard +20 %, Pro +30 %, Insane +40 % (entries 0x28 apart from RVA
 * 0x354f9a0). The instruction at RVA 0xf58a5e is `vaddss xmm0,xmm1,[rip+disp32]` reading the Medium entry (RVA 0x354f9c8);
 * its displacement is pointed at the Insane entry (RVA 0x354fa40) so online always counts as maximum difficulty.
 * Disable with env FH2MP_NO_MPDIFF.
 */
#define FH2_MPDIFF_INSN_RVA 0xf58a5eu
static int patch_mp_difficulty(void)
{
    static const unsigned char want[] = {0xC5, 0xF2, 0x58, 0x05, 0x62, 0x6F, 0x5F, 0x02};
    static const unsigned char now[] = {0xC5, 0xF2, 0x58, 0x05, 0xDA, 0x6F, 0x5F, 0x02};
    unsigned char *img = find_fh2_image();
    unsigned char *at;

    if (!img) {
        return 0;
    }
    at = img + FH2_MPDIFF_INSN_RVA;
    if (memcmp(at, now, sizeof now) == 0) {
        return 1;
    }
    if (memcmp(at, want, sizeof want) != 0 || !plat_patch("mpdiff: online AI difficulty bonus", at, want, now, sizeof now)) {
        log_begin();
        log_raw("mpdiff: unexpected bytes in the assists bonus code, not patched\n");
        return 0;
    }
    log_begin();
    log_raw("mpdiff: online races use the maximum (Insane, +40%) AI difficulty bonus\n");
    return 1;
}

/*
 * Traffic ownership on collisions. A client sends RequestOwnership for a traffic car it hit (FUN_140717410, called from the collision
 * handler FUN_140408af0) only if the car's "can take" flag (+0xaa) is set, and that flag is set only by FUN_140723b40 (the
 * CanTakeOwnership sender), which rarely runs, so most collisions asked for nothing and the car stayed with its owner: the hitter saw
 * it react locally, then snap back. The `cmp byte [rcx+0xaa],0 ; je` at RVA 0x717445 (the je is 74 7d) is turned into two nops, so
 * every collision with a car the local client does not own requests it (the 2 s limit between requests stays). Disable with
 * env FH2MP_NO_TRAFFICOWN.
 */
#define FH2_TRAFFIC_REQOWN_RVA 0x717445u
static int patch_traffic_request_ownership(void)
{
    static const unsigned char cmp_ctx[] = {0x80, 0xB9, 0xAA, 0x00, 0x00, 0x00, 0x00};
    static const unsigned char je[] = {0x74, 0x7D}, nop2[] = {0x90, 0x90};
    unsigned char *img = find_fh2_image();
    unsigned char *at;

    if (!img) {
        return 0;
    }
    at = img + FH2_TRAFFIC_REQOWN_RVA;
    if (at[0] == 0x90 && at[1] == 0x90) {
        return 1;
    }
    if (memcmp(at - sizeof cmp_ctx, cmp_ctx, sizeof cmp_ctx) != 0 ||
        !plat_patch("trafficown: RequestOwnership on every collision", at, je, nop2, 2)) {
        log_begin();
        log_raw("trafficown: unexpected bytes in the RequestOwnership trigger, not patched\n");
        return 0;
    }
    log_begin();
    log_raw("trafficown: a collision with a traffic car always requests its ownership\n");
    return 1;
}

/*
 * "Connected to Xbox Live" menu gates. Live-only menu actions (Show Gamercard in a Car Meet's player actions, and the other
 * Live-gated entries) test the byte at CLobby+0xf0 (CLobby = TBasicSingleton at RVA 0x34d6348):
 *     cmp byte [rax+0xf0], 0 ; jne <allowed>      else: popup "NO NETWORK ACCESS ... not connected to Xbox Live"
 * in FUN_140c86a70, FUN_140e68760 and FUN_140e0d6b0. The constructor sets the byte to 1; under Azimuth's offline platform layer
 * it ends up 0. Forcing the byte itself crashed the game (it also turns on Live presence code that is not ready), so
 * the three branches are patched instead: jne (0F 85 rel32) becomes jmp (E9 rel32+1) + nop, i.e. always allowed.
 * Disable with env FH2MP_NO_LIVEGATE. See docs/matchmaking.md.
 */
static int patch_live_gates(void)
{
    static const unsigned cmp_rva[3] = {0xc86a9bu, 0xe6879du, 0xe0d756u};
    static const unsigned char cmp_bytes[7] = {0x80, 0xB8, 0xF0, 0x00, 0x00, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    int i, j, done = 0;

    if (!img) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        unsigned char *c = img + cmp_rva[i];
        unsigned char *jne = c + 7;
        unsigned char jmp[6];
        int rel;
        for (j = 0; j < 7; j++) {
            if (c[j] != cmp_bytes[j]) {
                break;
            }
        }
        if (j != 7 || jne[0] != 0x0F || jne[1] != 0x85) {
            if (jne[0] == 0xE9) {
                done++;           /* already patched */
                continue;
            }
            log_begin();
            log_raw("livegate: unexpected bytes at a Live gate, skipped\n");
            continue;
        }
        mem_copy(&rel, jne + 2, 4);
        rel += 1;
        jmp[0] = 0xE9;
        mem_copy(jmp + 1, &rel, 4);
        jmp[5] = 0x90;
        if (!plat_patch("livegate: Live-only menu entry", jne, jne, jmp, 6)) {
            log_begin();
            log_raw("livegate: a Live gate was not patched\n");
            continue;
        }
        done++;
    }
    if (done) {
        log_begin();
        log_raw("livegate: Live-only menu entries no longer check the connected-to-Live byte\n");
    }
    return done;
}

/*
 * Home page "Online Road Trip" tile (PART_Button_HorizonOnline). The pause-menu tile setup (FUN_140ddac80) copies the
 * connected-to-Live byte (CLobby+0xf0, 0 under Azimuth) into page+0x537 and, when it is 0, forces the Home tile (and Game Party /
 * Join Friend) to state 0, which shows it greyed with a padlock:
 *     140ddb494  cmp [rbx+0x537], r13b      44 38 AB 37 05 00 00
 *     140ddb49b  jnz 140ddb4c9              75 2C
 * The jnz becomes jmp, i.e. the tile gets the "connected" handling. The byte itself stays 0: setting page+0x537 for the whole
 * function also changed the Social tab and broke LB/RB tab navigation. Disable with env FH2MP_NO_HOMETILE.
 */
static int patch_home_tile(void)
{
    static const unsigned char want[9] = {0x44, 0x38, 0xAB, 0x37, 0x05, 0x00, 0x00, 0x75, 0x2C};
    static const unsigned char jmp = 0xEB;
    unsigned char *img = find_fh2_image();
    unsigned char *p;
    int i;

    if (!img) {
        return 0;
    }
    p = img + 0xddb494u;
    if (p[7] == 0xEB && p[8] == 0x2C) {
        return 1;                 /* already patched */
    }
    for (i = 0; i < 9; i++) {
        if (p[i] != want[i]) {
            log_begin();
            log_raw("hometile: unexpected bytes at the Home tile test, not patched\n");
            return 0;
        }
    }
    if (!plat_patch("hometile: Online Road Trip tile", p + 7, want + 7, &jmp, 1)) {
        log_begin();
        log_raw("hometile: the Home tile test was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("hometile: the Home page Online Road Trip tile is no longer forced off without the connected-to-Live byte\n");
    return 1;
}

/*
 * "Finding session" that never ends after a disconnect. The matchmaker's session-validation handler (FUN_1417b0720) runs when a ticket
 * is Found (matchmaking state 2 or 6) and, before it joins the session, compares the offered session's name with the client's
 * current session (DAT_1436e2958+0x30) and ignores the offer when they are equal:
 *     1417b07be  test eax, eax        85 C0      (eax = _wcsicmp result)
 *     1417b07c0  jnz  1417b07c6       75 04      different -> bl = 1 (go on and join)
 *     1417b07c2  xor  bl, bl          32 DB      same -> bl = 0 (return, no join)
 * After a drop the client still remembers the old session, and the server hands the same lobby out again (a restart or a quick
 * rejoin gives the same session name), so the offer was always dropped: the ticket was Found, nothing else happened (no ticket
 * delete, no join call FUN_141799f90). The jnz becomes jmp, so the offer is always followed. The handler only runs while a
 * match is being looked for, never for an established session. Found on dev 2026-10-20 (breakpoints + live patch, both rigs then
 * joined). Disable with env FH2MP_NO_SAMESESSION.
 */
static int patch_same_session(void)
{
    static const unsigned char want[10] = {0x85, 0xC0, 0x75, 0x04, 0x32, 0xDB, 0xEB, 0x02, 0xB3, 0x01};
    static const unsigned char jmp = 0xEB;
    unsigned char *img = find_fh2_image();
    unsigned char *p;
    int i;

    if (!img) {
        return 0;
    }
    p = img + 0x17b07beu;
    if (p[2] == 0xEB && p[3] == 0x04) {
        return 1;                 /* already patched */
    }
    for (i = 0; i < 10; i++) {
        if (p[i] != want[i]) {
            log_begin();
            log_raw("samesession: unexpected bytes in the session validation handler, not patched\n");
            return 0;
        }
    }
    if (!plat_patch("samesession: join a found session with the remembered name", p + 2, want + 2, &jmp, 1)) {
        log_begin();
        log_raw("samesession: the session validation handler was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("samesession: a found session with the same name as the remembered one is joined instead of ignored\n");
    return 1;
}

/*
 * "Azimuth stopped: exception 0xc0000005 at game+0x1064408 reading 0x0" when a car appears. FUN_1410643b0 reads an engine's torque curve
 * (TorqueScale, ZeroThrottleTorqueScale, NumTorqueValues, then one value per RPM entry) from the result of
 * "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=<id>"; the callers (FUN_14105d690 for engines, FUN_14105e700 for the electric motor)
 * pass that result in. With an id that has no row (a part id the game's database does not know) the result is null and the first read,
 *     141064405  mov rcx,[rsi]      (rsi = pointer to the result)
 *     141064408  mov rax,[rcx]      <- rcx = 0
 * crashes the client of everybody who loads that car: seen on prod on 2026-10-20 whenever a player joined one particular lobby, the same
 * call chain every time (call chain and registers read from three minidumps). An engine the local game does not simulate (another
 * player's car is driven by prediction) does not need its curve, so the function now returns when the result is null, leaving the output as
 * it was. The function is hooked through the platform with a small stub as the detour: it does the check, then jumps on to the platform's
 * `original` (the game's first instructions, 48 8B C4 mov rax,rsp / 48 89 50 10 mov [rax+0x10],rdx, and the rest of the function).
 * Disable with env FH2MP_NO_NULLTORQUE.
 */
static volatile unsigned *g_nulltorque_hits;      /* counts the engines the guard skipped (read by the monitor thread, which logs changes) */

static int patch_null_torque_curve(void)
{
    static const unsigned char want[10] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x41, 0x56};
    /* test rdx,rdx ; jz hit ; mov rax,[rdx] ; test rax,rax ; jz hit ; jmp [rip+original] ; hit: inc dword [rip+counter] ; ret */
    static const unsigned char code[26] = {0x48, 0x85, 0xd2, 0x74, 0x0e, 0x48, 0x8b, 0x02, 0x48, 0x85, 0xc0, 0x74, 0x06,
                                           0xff, 0x25, 56 - 19, 0, 0, 0,          /* the original lives at stub+56, the jmp ends at stub+19 */
                                           0xff, 0x05, 64 - 25, 0, 0, 0,          /* the counter lives at stub+64, the inc ends at stub+25 */
                                           0xc3};
    unsigned char *img = find_fh2_image();
    unsigned char *site, *stub;
    void *original = NULL;
    FH2Hook *h;
    int i;

    if (!img) {
        return 0;
    }
    site = img + 0x10643b0u;
    for (i = 0; i < 10; i++) {
        if (site[i] != want[i]) {
            log_begin();
            log_raw("nulltorque: unexpected bytes at the torque curve reader, not patched\n");
            return 0;
        }
    }
    stub = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub) {
        log_begin();
        log_raw("nulltorque: could not allocate the stub, not patched\n");
        return 0;
    }
    mem_copy(stub, code, sizeof code);
    h = g_api->hook_function(g_mod, "nulltorque: torque curve reader", site, stub, &original);
    if (!h) {
        log_begin();
        log_raw("nulltorque: the torque curve reader was not patched\n");
        return 0;
    }
    *(void **)(stub + 56) = original;
    *(unsigned *)(stub + 64) = 0;
    FlushInstructionCache(GetCurrentProcess(), stub, 68);
    g_nulltorque_hits = (volatile unsigned *)(stub + 64);
    if (!g_api->enable_hooks(g_mod, h)) {
        log_begin();
        log_raw("nulltorque: the torque curve reader was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("nulltorque: an engine whose torque curve is missing from the game database is skipped instead of crashing the game\n");
    return 1;
}

/*
 * "Azimuth stopped: CreatePlacementTexture2D requires GPU-write/CPU-map/unknown placement semantics" (game+0x10c897b): Azimuth ends the whole
 * game when the game places a texture whose description its CreatePlacementTexture2D does not support (azimuth.exe 0.9.10, FUN_14012f990).
 * Supported: a GPU-written texture (usage default, no CPU access, misc 0/0x20000/0x40000, 1 mip, 1 slice, bind 0x28, 0x48 or 0xa8), or a
 * shader-resource-only texture (bind 8, no CPU access, 1 sample, 1-15 mips, up to 0x800 slices, usage default or immutable). Anything else stops
 * the game. The game reaches that function from one call site in its wrapper FUN_1410c88e0:
 *     1410c8974  call qword ptr [r10+0x200]        41 FF 92 00 02 00 00   (followed by a nop)
 * We replace it by "call [rip+slot]" (FF 15 disp32) into placement_hook, which has the same arguments: a supported description goes to Azimuth
 * unchanged, an unsupported one is logged and created as an ordinary texture (vtable slot 5, CreateTexture2D) instead of ending the game. The
 * ordinary texture does not alias the game's memory at the placement address, so such a texture may look wrong, but the game keeps running.
 * Env FH2MP_NO_PLACEMENTFIX disables it; FH2MP_PLACEMENT_FORCE=1 treats every depth-stencil placement texture as unsupported (to test the path).
 */
typedef long (*placement_fn)(void *dev, const unsigned *desc, unsigned tile, unsigned pitch, void *va, void **out);
typedef long (*create2d_fn)(void *dev, const unsigned *desc, const void *init, void **out);
static int g_place_force;
static volatile LONG g_place_logged;

static int placement_supported(const unsigned *d)
{
    /* d: width, height, mips, slices, format, samples, quality, usage, bind, cpu access, misc */
    if (d[7] == 0 && d[9] == 0 && (d[10] == 0 || ((d[10] - 0x20000u) & 0xfffdffffu) == 0) && d[2] == 1 && d[3] == 1 &&
        (d[8] == 0x28 || d[8] == 0x48 || d[8] == 0xa8)) {
        return 1;
    }
    return !(d[8] != 8 || d[9] != 0 || d[5] != 1 || d[2] == 0 || d[2] > 15 || d[3] > 0x800 || d[3] == 0 || d[7] > 1);
}

static long placement_hook(void *dev, const unsigned *desc, unsigned tile, unsigned pitch, void *va, void **out)
{
    void **vt = *(void ***)dev;
    unsigned copy[11];
    long hr;
    LONG n;
    int i;

    if (!(g_place_force && (desc[8] & 0x40)) && placement_supported(desc)) {
        return ((placement_fn)vt[0x200 / 8])(dev, desc, tile, pitch, va, out);
    }
    mem_copy(copy, desc, sizeof copy);
    copy[10] &= 0xffffu;                 /* keep the standard D3D11 misc flags; the Xbox ones are what Azimuth's CreateTexture2D refuses */
    hr = ((create2d_fn)vt[5])(dev, copy, 0, out);
    n = InterlockedIncrement(&g_place_logged);
    if (n <= 40) {
        log_begin();
        log_raw("placement: Azimuth would stop the game for this texture, created it as an ordinary texture instead:");
        for (i = 0; i < 11; i++) {
            log_raw(" ");
            log_hex(desc[i], 1 + (desc[i] > 0xf) + (desc[i] > 0xff) + (desc[i] > 0xfff) + (desc[i] > 0xffff) + (desc[i] > 0xfffff) +
                             (desc[i] > 0xffffff) + (desc[i] > 0xfffffff));
        }
        log_raw(" (w h mips slices format samples quality usage bind cpu misc), hr=0x");
        log_hex((unsigned long long)(unsigned long)hr, 8);
        log_raw(" #");
        log_dec((unsigned long)n);
        log_raw("\n");
    }
    return hr;
}

static int patch_placement_fallback(void)
{
    static const unsigned char want[11] = {0x49, 0x8b, 0xd6, 0x41, 0xff, 0x92, 0x00, 0x02, 0x00, 0x00, 0x90};
    unsigned char *img = find_fh2_image();
    unsigned char *site, *slot;
    unsigned char call[8] = {0xff, 0x15, 0, 0, 0, 0, 0x90, 0x90};     /* call qword ptr [rip+disp32] ; the original call was 7 bytes + nop */
    long long disp;
    wchar_t env[8];
    int i;

    if (!img) {
        return 0;
    }
    site = img + 0x10c8971u;              /* mov rdx,r14 ; call [r10+0x200] ; nop */
    for (i = 0; i < 11; i++) {
        if (site[i] != want[i]) {
            if (site[3] == 0xff && site[4] == 0x15) {
                return 1;                 /* already patched */
            }
            log_begin();
            log_raw("placement: unexpected bytes at the CreatePlacementTexture2D call, not patched\n");
            return 0;
        }
    }
    g_place_force = GetEnvironmentVariableW(L"FH2MP_PLACEMENT_FORCE", env, 8) != 0;
    slot = alloc_near(site);
    if (!slot) {
        log_begin();
        log_raw("placement: could not allocate a page near the game image, not patched\n");
        return 0;
    }
    *(void **)slot = (void *)placement_hook;
    disp = (long long)(slot - (site + 3 + 6));
    if (disp > 0x7fffffffll || disp < -0x80000000ll) {
        log_begin();
        log_raw("placement: slot out of rel32 range, not patched\n");
        return 0;
    }
    {
        int d32 = (int)disp;
        mem_copy(call + 2, &d32, 4);
    }
    if (!plat_patch("placement: CreatePlacementTexture2D fallback", site + 3, want + 3, call, sizeof call)) {
        log_begin();
        log_raw("placement: the CreatePlacementTexture2D call was not patched\n");
        return 0;
    }
    log_begin();
    log_raw(g_place_force ? "placement: textures Azimuth cannot place are created as ordinary textures (FORCED for depth-stencil, test mode)\n"
                          : "placement: textures Azimuth cannot place are created as ordinary textures instead of ending the game\n");
    return 1;
}

/*
 * Road Trip: a fresh lobby's first drive starts at the festival. After the destination vote and car select the client always fast-travels to
 * the "end event grid" (stateflow network_roadtrip_states.xml: free_roam_ui -> fast_travel_to_destination_drive_grid): BuildEndEventGrid
 * (FUN_140813080) gives the local car a grid slot (race data +0x2a8), places the grid with FUN_140627c20 (position +0x2b0, forward vector +0x2c0,
 * from the "start_location_NNN" locators of the race in RoadTripSynced.EventRaceId) and SetFastTravelToEndEventGrid (FUN_140815360) teleports
 * the car there. No online event starts or ends at the festival, so the server sends EventRaceId 0 for a lobby that has raced nothing yet and
 * this patch puts that grid at the festival hub instead (the client alone would use a default locator at the Saint-Martin aerodrome).
 * The one call at 1408131a0 (E8 rel32 to FUN_140627c20) is pointed at festival_grid_hook through the platform (hook_call).
 * Disable with env FH2MP_NO_FESTIVALGRID.
 */
typedef void (*grid_place_fn)(void *arg);
typedef void **(*rt_get_fn)(void *mgr);
typedef int (*vint_fn)(void *self);
typedef void *(*vptr_fn)(void *self);
typedef void (*vout_fn)(void *self, void **out);
typedef unsigned char *(*race_data_fn)(void *car);
static unsigned char *g_fg_img;
static grid_place_fn g_fg_place;          /* FUN_140627c20, or the next mod's hook on that call */
static volatile LONG g_fg_logged;

/* the hub (f10 parked there on dev, 2026-10-22) and the direction the grid faces, x/z unit vector; slots stand in rows of two */
static const float FG_X = 6154.6f, FG_Y = 118.6f, FG_Z = -5935.6f;
static const float FG_FX = -0.9996f, FG_FZ = -0.0266f;      /* f10 drove off the spot along this line (to 6068,-5938) */
static const float FG_COLUMN_M = 4.5f, FG_ROW_M = 9.0f;

static void festival_grid_hook(void *arg)
{
    unsigned char *img = g_fg_img;
    void *mgr, **rtp, *rt, *pl, *car = 0;
    unsigned char *rd;
    unsigned slot;
    float side, back;

    g_fg_place(arg);
    mgr = *(void **)(img + 0x34d6ac8u);                          /* DAT_1434d6ac8: the Road Trip manager */
    if (!mgr) {
        return;
    }
    rtp = ((rt_get_fn)(img + 0x14a8620u))(mgr);                   /* FUN_1414a8620: RoadTripSynced (as BuildRouteForDrive reads it) */
    rt = rtp ? *rtp : 0;
    if (!rt || ((vint_fn)(*(void ***)rt)[0xc0 / 8])(rt) != 0) {   /* vtable +0xc0: EventRaceId; 0 = nothing raced yet */
        return;
    }
    pl = ((vptr_fn)(img + 0x327270u))(0);                         /* the local player -> its car, as FUN_140815360 finds it */
    pl = pl ? ((vptr_fn)(*(void ***)pl)[0x48 / 8])(pl) : 0;
    if (!pl) {
        return;
    }
    ((vout_fn)(*(void ***)pl)[0x90 / 8])(pl, &car);
    rd = car ? ((race_data_fn)(img + 0x1762870u))(car) : 0;
    if (!rd) {
        return;
    }
    slot = rd[0x2a8];
    if (slot > 11) {
        slot = 0;
    }
    side = (slot & 1) ? FG_COLUMN_M : -FG_COLUMN_M;
    back = (float)(slot >> 1) * FG_ROW_M;
    ((float *)(rd + 0x2b0))[0] = FG_X - FG_FZ * side - FG_FX * back;     /* side vector = (-fz, 0, fx) */
    ((float *)(rd + 0x2b0))[1] = FG_Y + 0.5f;
    ((float *)(rd + 0x2b0))[2] = FG_Z + FG_FX * side - FG_FZ * back;
    ((float *)(rd + 0x2c0))[0] = FG_FX;
    ((float *)(rd + 0x2c0))[1] = 0.0f;
    ((float *)(rd + 0x2c0))[2] = FG_FZ;
    if (InterlockedIncrement(&g_fg_logged) <= 20) {
        log_begin();
        log_raw("festivalgrid: fresh Road Trip lobby, drive grid slot ");
        log_dec(slot);
        log_raw(" placed at the festival\n");
    }
}

static int patch_festival_grid(void)
{
    unsigned char *img = find_fh2_image();
    unsigned char *site;
    int d32;

    if (!img) {
        return 0;
    }
    site = img + 0x8131a0u;
    mem_copy(&d32, site + 1, 4);
    if (site[0] != 0xe8 || site + 5 + d32 != img + 0x627c20u) {
        log_begin();
        log_raw("festivalgrid: unexpected bytes at the end-event-grid call, not patched\n");
        return 0;
    }
    g_fg_img = img;
    if (!plat_hook_call("festivalgrid: end-event grid placement", site, (void *)festival_grid_hook, (void **)&g_fg_place)) {
        log_begin();
        log_raw("festivalgrid: the end-event-grid call was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("festivalgrid: a fresh Road Trip lobby's first drive starts at the festival\n");
    return 1;
}

/*
 * Rivals "race this event" gate. Pressing A on a Rivals event (Monthly, Festival Routes, ...) calls FUN_140d7b680, which only
 * starts the race when FUN_140666f40 (RVA 0x666f40) returns true, else it raises FUN_140e68760's "Feature unavailable: cannot connect
 * to Xbox Live" popup. That function begins with the same CLobby+0xf0 test as the Live gates above:
 *     cmp byte [rax+0xf0], 0 ; jne +7 ; xor al,al ; ret      (80 B8 F0 00 00 00 00 75 07 32 C0 ...)
 * then checks the Live sign-in state (FUN_140a42090) and the permissions 0x56..0x58 and the user flag mask 0x80. Those pass once the
 * Logon response carries the permission table (server/fh2mp_lsp.py), so only the byte test is removed: jne (75 07) becomes jmp (EB 07).
 * Setting the byte to 1 at runtime made "Setting Up Rivals Event" start (2026-10-18). Disable with env FH2MP_NO_RIVALGATE.
 */
static int patch_rival_gate(void)
{
    static const unsigned char want[9] = {0x80, 0xB8, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x75, 0x07};
    static const unsigned char jmp = 0xEB;
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    int i;

    if (!img) {
        return 0;
    }
    at = img + 0x666f4b;
    if (at[7] == 0xEB) {
        return 1;                 /* already patched */
    }
    for (i = 0; i < 9; i++) {
        if (at[i] != want[i]) {
            log_begin();
            log_raw("rivalgate: unexpected bytes in the Rivals race gate, not patched\n");
            return 0;
        }
    }
    if (!plat_patch("rivalgate: Rivals race gate", at + 7, want + 7, &jmp, 1)) {
        log_begin();
        log_raw("rivalgate: the Rivals race gate was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("rivalgate: Rivals events no longer require the connected-to-Live byte\n");
    return 1;
}

/*
 * Car Meet "is this an online meet" test. The Car Meet view model's tick (FUN_140830ad0) sets IsOnlineCarMeet (which gates
 * "Start Online Session" / "Start Showdown Race" and their actions, FUN_140ca0d50) from a chain of checks: the CarMeetSynced object
 * exists, the local player's lobby entry is valid, the network connection is up (+0x10 == 1) and, last,
 *     car = FUN_1407cfdc0(...);  car->vtable[0xb0]()      -- the local player's ForzaCarNetworked is "ready"
 * That last one is false under this setup, so the meet looks offline and the online entries stay hidden. Forcing the debug option
 * byte at options+0x2dfc shows the entries but disables their actions, so instead the failing `je` after that call is
 * turned into a 2-byte nop. Disable with env FH2MP_NO_ONLINEMEET. See docs/matchmaking.md.
 */
#define FH2_ONLINEMEET_TEST_RVA 0x830b8cu
static int patch_online_meet(void)
{
    static const unsigned char want[] = {0x84, 0xC0, 0x74, 0x05, 0x40, 0xB7, 0x01, 0xEB, 0x03, 0x40, 0x32, 0xFF};
    static const unsigned char nop2[] = {0x90, 0x90};
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    int i;

    if (!img) {
        return 0;
    }
    at = img + FH2_ONLINEMEET_TEST_RVA;
    for (i = 0; i < (int)sizeof want; i++) {
        if (at[i] != want[i]) {
            if (at[2] == 0x90 && at[3] == 0x90) {
                return 1;       /* already patched */
            }
            log_begin();
            log_raw("onlinemeet: unexpected bytes in the Car Meet view model, not patched\n");
            return 0;
        }
    }
    if (!plat_patch("onlinemeet: Car Meet counts as online", at + 2, want + 2, nop2, 2)) {
        log_begin();
        log_raw("onlinemeet: the Car Meet view model was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("onlinemeet: the Car Meet counts as online once connected (online entries available)\n");
    return 1;
}

/*
 * XSAPI token stub. Azimuth loads Microsoft's genuine Microsoft.Xbox.Services.dll (Xbox One build, PE timestamp
 * 0x5431cfcd) from the game folder. Its HTTP layer asks Authentication::GetTokenAndSignature (module +0x309a0, or
 * the *ForAllUsers variant +0x30d90) for a token before every request, and that goes to
 * Windows.Xbox.System.User.GetTokenAndSignatureAsync, which Azimuth implements as Azimuth.OfflineTokenRequest:
 * an async object already completed with 0x800704cf (network unreachable). So every XSAPI REST call (QoS servers,
 * sessions, tickets) fails before any HTTP request is sent, and the game reports "Unable to connect to the Live
 * configuration servers". Both functions return a Microsoft::Xbox::Services::Authentication object (0x48 bytes; the
 * HTTP layer reads the Token HSTRING at +0x20 and the Signature HSTRING at +0x28). This replaces them with a stub
 * that returns one such object carrying placeholder strings, built with the module's own allocator, constructor
 * (+0x30860) and Azimuth's WindowsCreateString (found through the module's import table).
 * Disable with env FH2MP_NO_XSTOKEN. See docs/matchmaking.md.
 */
#define XS_TIMESTAMP 0x5431cfcdu
#define XS_SIZE_OF_IMAGE 0x579000u
#define XS_GETTOKEN_RVA 0x309a0u
#define XS_GETTOKEN_ALL_RVA 0x30d90u
#define XS_AUTH_CTOR_RVA 0x30860u
#define XS_ALLOC_CALL_OFF 0xdcu /* mov ecx,0x48 ; call [rip+disp] inside GetTokenAndSignature */

static unsigned char *find_xsapi_image(void)
{
    SYSTEM_INFO si;
    unsigned char *addr;
    MEMORY_BASIC_INFORMATION mbi;

    GetSystemInfo(&si);
    addr = (unsigned char *)si.lpMinimumApplicationAddress;
    while (addr < (unsigned char *)si.lpMaximumApplicationAddress &&
           VirtualQuery(addr, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT && mbi.BaseAddress == mbi.AllocationBase && mbi.RegionSize >= 0x1000) {
            const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)mbi.AllocationBase;
            if (!IsBadReadPtr(dos, sizeof *dos) && dos->e_magic == IMAGE_DOS_SIGNATURE &&
                dos->e_lfanew > 0 && dos->e_lfanew < 0x1000) {
                const IMAGE_NT_HEADERS64 *nt =
                    (const IMAGE_NT_HEADERS64 *)((unsigned char *)dos + dos->e_lfanew);
                if (!IsBadReadPtr(nt, sizeof *nt) && nt->Signature == IMAGE_NT_SIGNATURE &&
                    nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                    nt->FileHeader.TimeDateStamp == XS_TIMESTAMP &&
                    nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                    nt->OptionalHeader.SizeOfImage == XS_SIZE_OF_IMAGE) {
                    return (unsigned char *)mbi.AllocationBase;
                }
            }
        }
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
    }
    return NULL;
}

/* IAT slot of a by-name import (the loader has already filled it) */
static void **xs_find_import_slot(unsigned char *img, const char *want)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)img;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(img + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_IMPORT_DESCRIPTOR *d;

    if (!dd->VirtualAddress) {
        return NULL;
    }
    for (d = (const IMAGE_IMPORT_DESCRIPTOR *)(img + dd->VirtualAddress); d->Name; d++) {
        const ULONGLONG *oft = (const ULONGLONG *)(img + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        void **iat = (void **)(img + d->FirstThunk);
        unsigned i;
        for (i = 0; oft[i]; i++) {
            const char *n;
            unsigned k = 0;
            if (oft[i] & 0x8000000000000000ull) {
                continue; /* by ordinal */
            }
            n = (const char *)(img + (oft[i] & 0x7fffffffu) + 2);
            while (want[k] && n[k] == want[k]) {
                k++;
            }
            if (!want[k] && !n[k]) {
                return &iat[i];
            }
        }
    }
    return NULL;
}

static void *xs_find_import(unsigned char *img, const char *want)
{
    void **slot = xs_find_import_slot(img, want);
    return slot ? *slot : NULL;
}

static void *g_xs_auth; /* the shared Authentication object returned by the stub */

static void *xs_token_stub(void *a, void *b, void *c, void *d)
{
    void **vt;
    (void)a; (void)b; (void)c; (void)d;
    vt = *(void ***)g_xs_auth;
    ((ULONG(*)(void *))vt[1])(g_xs_auth); /* AddRef, as a returned reference */
    return g_xs_auth;
}

/* 0: module not there yet (retry), 1: patched, 2: unusable (do not retry) */
/* Identity. Azimuth gives every client the same Xbox user id, 0x0009000000000000 (a movabs immediate in the code that
 * formats User.XboxUserId). Two clients (or two players on different machines) would be one person to every service, so
 * the shim derives a stable per-install id: 0x0009000000000000 | a random 32-bit number kept in fh2mp-identity.txt next to the
 * log (override with env FH2MP_XUID=<hex>), patches the immediate, and carries the id in the XSAPI token ("XBL3.0
 * x=fh2mp;<decimal xuid>") so the stand-in services can tell the callers apart. Disable with env FH2MP_NO_IDENTITY. */
static unsigned long long g_xuid = 0x0009000000000000ULL;

static void xuid_decimal(WCHAR *out)
{
    WCHAR t[24];
    int n = 0, i;
    unsigned long long v = g_xuid;
    do {
        t[n++] = (WCHAR)(L'0' + (int)(v % 10));
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++) {
        out[i] = t[n - 1 - i];
    }
    out[n] = 0;
}

static int patch_xsapi_token(void)
{
    static const unsigned char sig_get[12] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
    static const unsigned char sig_all[12] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48};
    static const unsigned char alloc_sig[7] = {0xB9, 0x48, 0x00, 0x00, 0x00, 0xFF, 0x15};
    static const WCHAR tok_head[] = L"XBL3.0 x=fh2mp;";
    static const WCHAR sgn[] = L"fh2mp";
    WCHAR tok[64];
    unsigned tok_len;
    unsigned char *img = find_xsapi_image();
    unsigned char *f1, *f2, *call;
    void *(*alloc)(ULONGLONG);
    void *(*ctor)(void *);
    LONG (*mkstr)(const WCHAR *, unsigned, void **);
    void *mem, *obj, *h_tok = NULL, *h_sig = NULL;
    unsigned i;
    int rel;

    if (!img) {
        return 0;
    }
    f1 = img + XS_GETTOKEN_RVA;
    f2 = img + XS_GETTOKEN_ALL_RVA;
    for (i = 0; i < 12; i++) {
        if (f1[i] != sig_get[i] || f2[i] != sig_all[i]) {
            log_begin();
            log_raw("xstoken: unexpected bytes in the XSAPI token functions, not patched\n");
            return 2;
        }
    }
    call = f1 + XS_ALLOC_CALL_OFF;
    for (i = 0; i < 7; i++) {
        if (call[i] != alloc_sig[i]) {
            log_begin();
            log_raw("xstoken: allocator call not where expected, not patched\n");
            return 2;
        }
    }
    mem_copy(&rel, call + 7, 4);
    alloc = *(void *(**)(ULONGLONG))(call + 11 + rel);
    mkstr = (LONG(*)(const WCHAR *, unsigned, void **))xs_find_import(img, "WindowsCreateString");
    ctor = (void *(*)(void *))(img + XS_AUTH_CTOR_RVA);
    if (!alloc || !mkstr) {
        log_begin();
        log_raw("xstoken: could not resolve the allocator or WindowsCreateString, not patched\n");
        return 2;
    }
    mem = alloc(0x48);
    if (!mem) {
        return 2;
    }
    obj = ctor(mem);
    lstrcpyW(tok, tok_head);
    xuid_decimal(tok + (sizeof tok_head / sizeof tok_head[0] - 1));
    tok_len = (unsigned)lstrlenW(tok);
    if (!obj || mkstr(tok, tok_len, &h_tok) < 0 ||
        mkstr(sgn, (unsigned)(sizeof sgn / sizeof sgn[0] - 1), &h_sig) < 0) {
        log_begin();
        log_raw("xstoken: could not build the Authentication object, not patched\n");
        return 2;
    }
    *(void **)((unsigned char *)obj + 0x20) = h_tok;
    *(void **)((unsigned char *)obj + 0x28) = h_sig;
    {
        void **vt = *(void ***)obj;
        for (i = 0; i < 1000000; i++) { /* keep it alive however many references the callers drop */
            ((ULONG(*)(void *))vt[1])(obj);
        }
    }
    g_xs_auth = obj;
    if (!plat_hook("xstoken: XSAPI GetTokenAndSignature", f1, (void *)xs_token_stub, NULL) ||
        !plat_hook("xstoken: XSAPI GetTokenAndSignatureForAllUsers", f2, (void *)xs_token_stub, NULL)) {
        log_begin();
        log_raw("xstoken: the XSAPI token functions were not patched\n");
        return 2;
    }
    log_begin();
    log_raw("xstoken: XSAPI GetTokenAndSignature(+ForAllUsers) now return a placeholder token\n");
    return 1;
}

/*
 * Local secure device address string. When the game creates or updates its multiplayer session it takes the local
 * Windows.Xbox.Networking.SecureDeviceAddress (FUN_1417a5f50 = SecureDeviceAddress::GetLocal) and asks it for its
 * Base64 string (FUN_1417a5b00, vtable slot 0x50). Azimuth's offline address returns a null string, the session
 * member property "secureDeviceAddress" is then empty, and the embedded XSAPI refuses to serialize the session
 * (E_INVALIDARG in SerializerBase::SerializeStringToJsonObject, serialization.cpp:0x2af), which the game reports
 * as "Unable to join session". This replaces FUN_1417a5b00 with a stub that returns a placeholder Base64 string.
 * Disable with env FH2MP_NO_SDASTR. See docs/matchmaking.md.
 */
#define FH2_SDA_STR_RVA 0x17a5b00u
static LONG (*g_sda_mkstr)(const WCHAR *, unsigned, void **);

static void *sda_string_stub(void *self)
{
    static const WCHAR s[] = L"RkgyTVAtU0RBLTAwMDAwMDA=";
    void *h = NULL;
    (void)self;
    if (g_sda_mkstr && g_sda_mkstr(s, (unsigned)(sizeof s / sizeof s[0] - 1), &h) >= 0) {
        return h; /* the caller owns it and releases it with WindowsDeleteString */
    }
    return NULL;
}

static int patch_sda_string(void)
{
    static const unsigned char sig[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE,
                                        0xFF, 0xFF, 0xFF, 0x33, 0xDB, 0x48, 0x89, 0x5C, 0x24, 0x40};
    unsigned char *img = find_fh2_image();
    unsigned char *xs = find_xsapi_image();
    unsigned char *fn;
    void *mk = NULL;
    unsigned i;

    if (!img || !xs) {
        return 0; /* both must be mapped (XSAPI supplies WindowsCreateString) */
    }
    fn = img + FH2_SDA_STR_RVA;
    for (i = 0; i < sizeof sig; i++) {
        if (fn[i] != sig[i]) {
            log_begin();
            log_raw("sdastr: unexpected bytes at the secure device address string getter, not patched\n");
            return 2;
        }
    }
    mk = xs_find_import(img, "WindowsCreateString");
    if (!mk) {
        mk = xs_find_import(xs, "WindowsCreateString");
    }
    if (!mk) {
        log_begin();
        log_raw("sdastr: WindowsCreateString not found, not patched\n");
        return 2;
    }
    g_sda_mkstr = (LONG(*)(const WCHAR *, unsigned, void **))mk;
    if (!plat_hook("sdastr: SecureDeviceAddress string", fn, (void *)sda_string_stub, NULL)) {
        log_begin();
        log_raw("sdastr: the secure device address string getter was not patched\n");
        return 2;
    }
    log_begin();
    log_raw("sdastr: local secure device address string now returns a placeholder\n");
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
    if (!plat_hook("sts: StsAuthTokenManager::GetAuthToken", fn, (void *)sts_auth_stub, NULL)) {
        log_begin();
        log_raw("sts: GetAuthToken was not patched\n");
        return 0;
    }
    log_begin();
    log_raw("sts: GetAuthToken replaced; LSP requests no longer wait for an Xbox token\n");
    return 1;
}

static int install_cmdline_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    unsigned char *target = NULL;
    unsigned long long h = 0;
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
    if (!plat_hook("hook: Azimuth Platform::Details::GetCmdArguments", target, (void *)hooked_get_cmd_arguments, NULL)) {
        log_begin();
        log_raw("hook: GetCmdArguments was not patched; launch arguments will not be injected\n");
        return 0;
    }
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

/* The mapped game (the platform recognises it by FH2_TIMESTAMP and FH2_SIZE_OF_IMAGE, as Extra Options does), or NULL. */
static unsigned char *find_fh2_image(void)
{
    return g_api ? (unsigned char *)g_api->game_base() : NULL;
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
    unsigned char *newstr;
    int oldDisp;
    long long newDisp;
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

    newstr = alloc_near(lea);
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
        if (!plat_patch("redirect: matchmaking group path", lea + 3, &oldDisp, &d32, 4)) {
            log_begin();
            log_raw("redirect: the matchmaking group entry was not patched\n");
            return 0;
        }
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

#ifdef FH2MP_DEBUG_BUILD
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
#endif

#include "sha256.inc"
#include "inflate.inc"

/*
 * #64: a Monthly Rival started from the pause menu inside an online session loaded for ever. The menu's launch (RivalsModule
 * RequestLaunchRivalsEventFromMenu, then the global/rivals_launcher entity and its offline race_trigger_activation flow) only works
 * offline, and nothing on the way leaves the session. global.xml's request_launch_rivals_event_from_menu, the pause menu's last step
 * before the launch, now first leaves the session the way "return to single player" does (broadcast NetworkSessionRequestDisconnect,
 * then wait for the disconnect to complete). The game's own global.xml is read from media\stateflow.zip, changed in that one state and
 * handed to the platform (set_file: mediaoverride\stateflow\global.xml, removed again when FH2MP is). pause.xml cannot carry it: the
 * platform owns that file for its MODS page. Disable with env FH2MP_NO_RIVALSLEAVE.
 */
static const char RIVALS_LAUNCH_OLD[] =
    "  <state id=\"request_launch_rivals_event_from_menu\">\r\n"
    "    <behavior id=\"ModuleMethodCall\" module_id=\"RivalsModule\" method=\"RequestLaunchRivalsEventFromMenu\" />\r\n"
    "  </state>";
static const char RIVALS_LAUNCH_NEW[] =
    "  <state id=\"request_launch_rivals_event_from_menu\">\r\n"
    "    <node state=\"network.is_in_network_session\">\r\n"
    "      <exit id=\"in_network_session\" target=\"_fh2mp_leave_session\" />\r\n"
    "      <exit id=\"not_in_network_session\" target=\"_fh2mp_launch\" />\r\n"
    "    </node>\r\n"
    "    <node id=\"_fh2mp_leave_session\" state=\"global.broadcast_event\" event=\"NetworkSessionRequestDisconnect\" />\r\n"
    "    <node state=\"network.wait_for_network_disconnection_complete\" />\r\n"
    "    <node id=\"_fh2mp_launch\" state=\"request_launch_rivals_event_from_menu_now\" />\r\n"
    "  </state>\r\n"
    "\r\n"
    "  <state id=\"request_launch_rivals_event_from_menu_now\">\r\n"
    "    <behavior id=\"ModuleMethodCall\" module_id=\"RivalsModule\" method=\"RequestLaunchRivalsEventFromMenu\" />\r\n"
    "  </state>";

static int stateflow_rivals_leave_session(void)
{
    WCHAR zip[MAX_PATH];
    unsigned char *orig, *out;
    size_t size = 0, at, olen = sizeof RIVALS_LAUNCH_OLD - 1, nlen = sizeof RIVALS_LAUNCH_NEW - 1, i, hits = 0;
    int ok;

    if (!g_api->game_dir[0] || lstrlenW(g_api->game_dir) + 32 >= MAX_PATH) {
        return 0;
    }
    lstrcpyW(zip, g_api->game_dir);
    lstrcatW(zip, L"\\media\\stateflow.zip");
    orig = zip_read_file(zip, "global.xml", &size);
    if (!orig) {
        log_begin();
        log_raw("rivalsleave: could not read global.xml from the game's stateflow.zip, not changed\n");
        return 0;
    }
    for (i = 0, at = 0; i + olen <= size; i++) {
        if (orig[i] == RIVALS_LAUNCH_OLD[0] && mem_equal(orig + i, (const unsigned char *)RIVALS_LAUNCH_OLD, olen)) {
            at = i;
            hits++;
        }
    }
    if (hits != 1 || !(out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size - olen + nlen))) {
        HeapFree(GetProcessHeap(), 0, orig);
        log_begin();
        log_raw("rivalsleave: the Rivals launch state in global.xml is not as expected, not changed\n");
        return 0;
    }
    mem_copy(out, orig, at);
    mem_copy(out + at, RIVALS_LAUNCH_NEW, nlen);
    mem_copy(out + at + nlen, orig + at + olen, size - at - olen);
    ok = g_api->set_file(g_mod, "stateflow/global.xml", out, size - olen + nlen);
    HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, orig);
    log_begin();
    log_raw(ok ? "rivalsleave: a Rival started inside an online session leaves the session first\n"
               : "rivalsleave: global.xml was not changed (another mod replaces it, see fh2-mods\\platform.log)\n");
    return ok;
}

/*
 * The car database the game runs with (the mediaoverride copy gamedb_place keeps equal to the launcher's, session line gamedb=). Its SHA-256 goes
 * with every request as X-FH2MP-GameDb, so the server can refuse a game that runs with another database (a car pack the others do not
 * have crashes them). Hashed again only when the file's size or write time changes. "missing" when it cannot be read.
 */
static WCHAR g_gamedb_path[MAX_PATH];
static WCHAR g_gamedb_src[MAX_PATH];      /* the launcher's copy, put at g_gamedb_path by gamedb_place */
static volatile LONG g_gamedb_placed;
static volatile LONG g_gamedb_started;    /* every mod has started (the platform's own database step is done) */
static int gamedb_place(void);
static SRWLOCK g_gamedb_lock = SRWLOCK_INIT;
static ULONGLONG g_gamedb_size, g_gamedb_time;
static char g_gamedb_hex[65];

static void gamedb_hash(char out[65])
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    ULONGLONG size, wtime;
    int i;

    AcquireSRWLockExclusive(&g_gamedb_lock);
    if (!GetFileAttributesExW(g_gamedb_path, GetFileExInfoStandard, &fa)) {
        lstrcpyA(g_gamedb_hex, "missing");
        g_gamedb_size = g_gamedb_time = 0;
    } else {
        size = (ULONGLONG)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        wtime = (ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
        if (!g_gamedb_hex[0] || g_gamedb_hex[0] == 'm' || size != g_gamedb_size || wtime != g_gamedb_time) {
            HANDLE f = CreateFileW(g_gamedb_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            static unsigned char buf[1 << 16];
            sha256_ctx c;
            DWORD got;
            int ok = f != INVALID_HANDLE_VALUE;

            sha256_init(&c);
            while (ok && (ok = ReadFile(f, buf, sizeof buf, &got, NULL)) && got) {
                sha256_update(&c, buf, got);
            }
            if (f != INVALID_HANDLE_VALUE) {
                CloseHandle(f);
            }
            if (ok) {
                sha256_final_hex(&c, g_gamedb_hex);
                g_gamedb_size = size, g_gamedb_time = wtime;
            } else {
                lstrcpyA(g_gamedb_hex, "missing");
            }
            log_begin();
            log_raw("gamedb: ");
            log_raw(g_gamedb_hex);
            log_raw("\n");
        }
    }
    for (i = 0; i < 65; i++) {
        out[i] = g_gamedb_hex[i];
    }
    ReleaseSRWLockExclusive(&g_gamedb_lock);
}

#include "xhr2.inc"
#ifdef FH2MP_DEBUG_BUILD
#include "dbg.inc"
#endif

/*
 * Watches the game's option singleton. Logs the FreeRoam direct-IP slot and related flags as they
 * change; if the singleton is up and the slot is still empty, fills it from our argument.
 */
/*
 * Weather/time-of-day sampler (env FH2MP_WEATHER_DUMP=1), for recovering the game's weather pattern from a solo session.
 * The world object is *(*(img+0x34aba60 + 0x48) + 0x98); +0x480 is the time-of-day manager (float seconds at +0x10, time
 * scale float at +0x18) and +0x488 the weather state that the game's own server-side code (FUN_1406f5520/5710/5910) packs into
 * FullWeather / NewAtmosphericData / NewPrecipitationData. A line "weather: tod=<s> scale=<f> | off=word ..." is logged when
 * any sampled word changed, and every 15 s regardless. Words are hex; read the floats offline.
 */
#define FH2_WORLD_ROOT_RVA 0x34aba60u
static int rd_ok(const void *p, size_t n)
{
    return p && (ULONG_PTR)p > 0x10000 && !IsBadReadPtr(p, n);
}

static void weather_sample(unsigned char *img)
{
    static const struct { unsigned lo, hi; } rng[] = {
        {0x08, 0x30}, {0x58, 0x68}, {0xa0, 0xc0}, {0x110, 0x120}, {0x1a0, 0x1d0}};
    static unsigned prev[256];
    static unsigned long last_beat;
    unsigned char *root, *mgr, *world, *tod, *w;
    unsigned cur[256];
    unsigned n = 0, i, k, changed = 0;
    float f_tod = 0.0f, f_scale = 0.0f;
    unsigned u_tod, u_scale;

    if (!rd_ok(img + FH2_WORLD_ROOT_RVA, 8)) {
        return;
    }
    root = *(unsigned char **)(img + FH2_WORLD_ROOT_RVA);
    if (!rd_ok(root + 0x48, 8) || !(mgr = *(unsigned char **)(root + 0x48)) || !rd_ok(mgr + 0x98, 8)) {
        return;
    }
    world = *(unsigned char **)(mgr + 0x98);
    if (!rd_ok(world + 0x488, 8)) {
        return;
    }
    tod = *(unsigned char **)(world + 0x480);
    w = *(unsigned char **)(world + 0x488);
    if (!rd_ok(w, 0x1d0)) {
        return;
    }
    if (rd_ok(tod, 0x20)) {
        f_tod = *(float *)(tod + 0x10);
        f_scale = *(float *)(tod + 0x18);
    }
    for (k = 0; k < sizeof rng / sizeof rng[0]; k++) {
        for (i = rng[k].lo; i < rng[k].hi; i += 4) {
            cur[n++] = *(unsigned *)(w + i);
        }
    }
    for (i = 0; i < n; i++) {
        if (cur[i] != prev[i]) {
            changed = 1;
        }
    }
    if (!changed && GetTickCount() - last_beat < 15000) {
        return;
    }
    last_beat = GetTickCount();
    log_begin();
    memcpy(&u_tod, &f_tod, 4);
    memcpy(&u_scale, &f_scale, 4);
    log_raw("weather: tod=");
    log_hex(u_tod, 8);
    log_raw(" scale=");
    log_hex(u_scale, 8);
    log_raw(" |");
    n = 0;
    for (k = 0; k < sizeof rng / sizeof rng[0]; k++) {
        for (i = rng[k].lo; i < rng[k].hi; i += 4) {
            log_raw(" ");
            log_hex(i, 3);
            log_raw("=");
            log_hex(cur[n], 8);
            prev[n] = cur[n];
            n++;
        }
    }
    log_raw("\n");
}

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
    int weather_dump;
    WCHAR wd_env[4];

    (void)unused;
#ifdef FH2MP_DEBUG_BUILD
    weather_dump = GetEnvironmentVariableW(L"FH2MP_WEATHER_DUMP", wd_env, 4) > 0;
#else
    (void)wd_env;
    weather_dump = 0;
#endif
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
        patch_xsapi_cocreate();
        {
            static int xb_state;
            wchar_t xb_env[8];
            if (xb_state == 0 && GetEnvironmentVariableW(L"FH2MP_NO_XSBODY", xb_env, 8) == 0) {
                xb_state = patch_xsapi_body();
            }
        }
        {
            /* XSAPI is mapped a few seconds after the game image; retry until it is there */
            static int xs_state;
            wchar_t xs_env[8];
            if (xs_state == 0 && GetEnvironmentVariableW(L"FH2MP_NO_XSTOKEN", xs_env, 8) == 0) {
                xs_state = patch_xsapi_token();
            }
        }
        {
            static int sda_state;
            wchar_t sda_env[8];
            if (sda_state == 0 && GetEnvironmentVariableW(L"FH2MP_NO_SDASTR", sda_env, 8) == 0) {
                sda_state = patch_sda_string();
            }
        }
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
#ifdef FH2MP_DEBUG_BUILD
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
#endif
        if (!(img[FH2_OPTIONS_INIT_FLAG_RVA] & 1)) {
            Sleep(500);
            continue;
        }
#ifdef FH2MP_DEBUG_BUILD
        if (tick >= 30) {
            maybe_run_connect_probe(img);
        }
#endif
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
        if (weather_dump) {
            weather_sample(img);
        }
        if (g_gamedb_started && tick % 10 == 0) {
            gamedb_place();               /* a copy that failed, or a mod's apply_database putting the platform's copy back */
        }
        if (g_nulltorque_hits) {
            static unsigned nt_seen;
            unsigned nt_now = *g_nulltorque_hits;
            if (nt_now != nt_seen) {
                log_begin();
                log_raw("nulltorque: skipped ");
                log_dec(nt_now - nt_seen);
                log_raw(" engine(s) with no torque curve (");
                log_dec(nt_now);
                log_raw(" in all)\n");
                nt_seen = nt_now;
            }
        }
        Sleep(1000);
    }
    return 0;
}

#ifdef FH2MP_DEBUG_BUILD
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

    if (n > 3000) { /* was 200, which hid everything after the first minute of a session */
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
#endif

static wchar_t g_exe_dir[MAX_PATH];     /* the Azimuth folder (the platform's azimuth_dir) */

/*
 * The car database. The launcher keeps the server's copy in <Azimuth>\fh2mp\gamedbRC.slt and names it in the session (gamedb=, relative
 * to the Azimuth folder; g_gamedb_src). The game reads <Mount>\mediaoverride\Stripped\gamedbRC.slt (g_gamedb_path), which is also where
 * the platform writes the database it builds from other mods' SQL, and which it deletes once no mod has any. So when every mod has
 * started (the platform has written or removed its copy by then), ours is put there: online play needs exactly the server's database.
 * Changes other mods made to it are named in the log and on the title screen, and the platform's record of its copy is deleted, so that
 * it builds it again next time (a game started without the FH2MP launcher then gets those changes). Another mod's database there (traffic
 * cars, a car pack's) is kept as fh2mp\gamedb-kept.slt first, and put back when the game starts without a server database in the session
 * (gamedb_keep.inc).
 */
static ULONGLONG g_gamedb_placed_size, g_gamedb_placed_time;

static int file_stamp(const WCHAR *path, ULONGLONG *size, ULONGLONG *wtime)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
        return 0;
    }
    *size = (ULONGLONG)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
    *wtime = (ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
    return 1;
}

/* A file another program has open for a moment (a virus scanner, say), or is still writing (FH2MP in another process putting the
 * server's database in place): tried again for up to a second. */
static int file_busy(DWORD e)
{
    return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
}

static int file_missing(DWORD e)
{
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

/* Opens a file to read, sharing it with everyone. INVALID_HANDLE_VALUE, with *err, when it can't be. */
static HANDLE file_open_read(const WCHAR *path, DWORD flags, DWORD *err)
{
    int i;

    for (i = 0;; i++) {
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, flags,
                               NULL);
        if (f != INVALID_HANDLE_VALUE) {
            *err = 0;
            return f;
        }
        *err = GetLastError();
        if (!file_busy(*err) || i == 9) {
            return INVALID_HANDLE_VALUE;
        }
        Sleep(100);
    }
}

static DWORD read_error(void)
{
    DWORD e = GetLastError();
    return e ? e : ERROR_READ_FAULT;
}

/* Whether two files have the same bytes: 1, 0 (also when one isn't there), or -1 when one can't be read (*err). A file that can't be
 * read is never taken for a different one: FH2MP's copy, or the one kept, would then look like another mod's database. */
static int files_compare(const WCHAR *a, const WCHAR *b, DWORD *err)
{
    static unsigned char ba[1 << 16], bb[1 << 16];
    HANDLE fa, fb = INVALID_HANDLE_VALUE;
    DWORD na, nb;
    int same = -1;

    fa = file_open_read(a, FILE_FLAG_SEQUENTIAL_SCAN, err);
    if (fa != INVALID_HANDLE_VALUE) {
        fb = file_open_read(b, FILE_FLAG_SEQUENTIAL_SCAN, err);
    }
    if (fb == INVALID_HANDLE_VALUE) {
        same = file_missing(*err) ? 0 : -1;
    }
    while (same < 0 && fb != INVALID_HANDLE_VALUE) {
        if (!ReadFile(fa, ba, sizeof ba, &na, NULL) || !ReadFile(fb, bb, sizeof bb, &nb, NULL)) {
            *err = read_error();
            break;
        }
        if (na != nb || !mem_equal(ba, bb, na)) {
            same = 0;
        } else if (!na) {
            same = 1;
        }
    }
    if (fa != INVALID_HANDLE_VALUE) {
        CloseHandle(fa);
    }
    if (fb != INVALID_HANDLE_VALUE) {
        CloseHandle(fb);
    }
    return same;
}

/* Up to n bytes from the start of a file. -> how many were read, or -1 when it can't be read (*err; file_missing when it isn't there). */
static int file_head(const WCHAR *path, void *buf, DWORD n, DWORD *err)
{
    HANDLE f = file_open_read(path, FILE_ATTRIBUTE_NORMAL, err);
    DWORD got = 0;
    int ok;

    if (f == INVALID_HANDLE_VALUE) {
        return -1;
    }
    ok = ReadFile(f, buf, n, &got, NULL);
    if (!ok) {
        *err = read_error();
    }
    CloseHandle(f);
    return ok ? (int)got : -1;
}

#include "gamedb_keep.inc"

/* <Azimuth>\fh2mp\<name>: next to the launcher's copy, also when the game starts without a session. gamedb_dir makes the folder, before a
 * file is written there. */
static void gamedb_file(WCHAR *out, const WCHAR *name)
{
    lstrcpyW(out, g_exe_dir);
    lstrcatW(out, L"\\fh2mp\\");
    lstrcatW(out, name);
}

static void gamedb_dir(void)
{
    WCHAR dir[MAX_PATH];

    lstrcpyW(dir, g_exe_dir);
    lstrcatW(dir, L"\\fh2mp");
    CreateDirectoryW(dir, NULL);
}

/* <Mount>\mediaoverride\Stripped\<name> (24 characters at most). -> 0 when the platform found no game folder. gamedb_game_dirs makes
 * the folders, which a mod's uninstall steps may have removed. */
static int gamedb_game_file(WCHAR *out, const WCHAR *name)
{
    out[0] = 0;
    if (!g_api->game_dir[0] || lstrlenW(g_api->game_dir) + 50 >= MAX_PATH) {
        return 0;
    }
    lstrcpyW(out, g_api->game_dir);
    lstrcatW(out, L"\\mediaoverride\\Stripped\\");
    lstrcatW(out, name);
    return 1;
}

static void gamedb_game_dirs(void)
{
    WCHAR dir[MAX_PATH];

    lstrcpyW(dir, g_api->game_dir);
    lstrcatW(dir, L"\\mediaoverride");
    CreateDirectoryW(dir, NULL);
    lstrcatW(dir, L"\\Stripped");
    CreateDirectoryW(dir, NULL);
}

/* Copies `from` over `to` with its date, by way of <to>.fh2mp-tmp, so `to` is never half written: the game, the platform and FH2MP in
 * another Azimuth folder never see a copy cut short. -> 0, or the Windows error. */
static DWORD gamedb_copy_over(const WCHAR *from, const WCHAR *to)
{
    WCHAR tmp[MAX_PATH];
    DWORD err;

    if (lstrlenW(to) + 11 > MAX_PATH) {
        return ERROR_FILENAME_EXCED_RANGE;
    }
    lstrcpyW(tmp, to);
    lstrcatW(tmp, L".fh2mp-tmp");
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(from, tmp, FALSE)) {
        return GetLastError();
    }
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(to, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp, to, MOVEFILE_REPLACE_EXISTING)) {
        err = GetLastError();
        DeleteFileW(tmp);
        return err;
    }
    return 0;
}

/* R's lists (gamedb_keep.inc): 0 FH2MP's own, 1 the one next to T for every Azimuth folder, 2 the platform's own (only read). -> 0 when
 * there is none (no game folder). */
#define GAMEDB_LISTS 3

static int gamedb_list(WCHAR *out, int which)
{
    if (which == 1) {
        return gamedb_game_file(out, L"platform-db-fh2mp.txt");
    }
    lstrcpyW(out, g_exe_dir);
    lstrcatW(out, which ? L"\\fh2-mods\\platform-db-fh2mp.txt" : L"\\fh2mp\\gamedb-placed.txt");
    return 1;
}

/* The "size date" lines of such a file, oldest first, GK_PLACED_MAX at most. -> how many (0 when it isn't there), or -1 when it can't be
 * read (*err). */
static int gamedb_read_stamps(const WCHAR *path, gk_stamp *list, DWORD *err)
{
    char buf[512];
    int n = file_head(path, buf, sizeof buf, err);

    if (n < 0) {
        return file_missing(*err) ? 0 : -1;
    }
    return gk_placed_parse(buf, n, list);
}

/* Writes `n` stamps as such a file. -> 0, or the Windows error. */
static DWORD gamedb_write_stamps(const WCHAR *path, const gk_stamp *list, int n)
{
    char text[200];
    DWORD len = (DWORD)gk_placed_format(list, n, text), put = 0, err = 0;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (f == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }
    if (!WriteFile(f, text, len, &put, NULL)) {
        err = GetLastError();
    } else if (put != len) {
        err = ERROR_WRITE_FAULT;
    }
    CloseHandle(f);
    return err;
}

/* T is the server's copy now: its stamp goes last in FH2MP's list and in the one next to T, so it is still known as FH2MP's once the
 * server's database changes, also by FH2MP in another Azimuth folder using this game folder. */
static void gamedb_note_placed(void)
{
    gk_stamp list[GK_PLACED_MAX], t;
    WCHAR path[MAX_PATH];
    DWORD err;
    int which, n;

    if (!file_stamp(g_gamedb_path, &t.size, &t.time)) {
        return;
    }
    gamedb_dir();
    for (which = 0; which < 2; which++) {
        if (!gamedb_list(path, which)) {
            continue;
        }
        n = gamedb_read_stamps(path, list, &err);
        if (n > 0 && list[n - 1].size == t.size && list[n - 1].time == t.time) {
            continue;
        }
        if (n >= 0) {
            err = gamedb_write_stamps(path, list, gk_placed_add(list, n, &t));
        }
        if (err) {
            log_begin();
            log_raw(which ? "gamedb: could not write mediaoverride\\Stripped\\platform-db-fh2mp.txt, error "
                          : "gamedb: could not write fh2mp\\gamedb-placed.txt, error ");
            log_dec(err);
            log_raw("\n");
        }
    }
}

/* What the database at `target` (stamp t) is (gk_classify). `same`: it has the bytes of the launcher's copy. -> 0, or the error when a
 * file it is judged by can't be read (*what is then not set). */
static DWORD gamedb_what(const WCHAR *target, const gk_stamp *t, int same, int *what)
{
    gk_stamp placed[GAMEDB_LISTS * GK_PLACED_MAX];
    unsigned char head[GK_HEAD];
    WCHAR path[MAX_PATH];
    ULONGLONG state_size;
    DWORD err;
    gk_facts f;
    int which, n;

    f.t = *t;
    f.same_as_server = same;
    f.nplaced = 0;
    for (which = 0; which < GAMEDB_LISTS; which++) {
        if (gamedb_list(path, which)) {
            if ((n = gamedb_read_stamps(path, placed + f.nplaced, &err)) < 0) {
                return err;
            }
            f.nplaced += n;
        }
    }
    f.placed = placed;
    if ((f.nhead = file_head(target, head, sizeof head, &err)) < 0) {
        return err;
    }
    f.head = head;
    lstrcpyW(path, g_exe_dir);
    lstrcatW(path, L"\\fh2-mods\\platform-db.txt");
    f.have_state = file_stamp(path, &state_size, &f.state_time) && state_size > 0;
    *what = gk_classify(&f);
    return 0;
}

/* S is there and not empty: the platform has built its copy at T since FH2MP last removed S */
static int gamedb_platform_built(void)
{
    WCHAR path[MAX_PATH];
    ULONGLONG size, wtime;

    lstrcpyW(path, g_exe_dir);
    lstrcatW(path, L"\\fh2-mods\\platform-db.txt");
    return file_stamp(path, &size, &wtime) && size > 0;
}

/* B (gamedb_keep.inc): 1 when K, stamp k, is back at T, 0 when not, -1 when B can't be read (*err). B names K by its stamp, so one left
 * from an earlier K counts for nothing. */
static int gamedb_back(const gk_stamp *k, DWORD *err)
{
    gk_stamp s[GK_PLACED_MAX];
    WCHAR path[MAX_PATH];
    int n;

    gamedb_file(path, L"gamedb-put-back.txt");
    if ((n = gamedb_read_stamps(path, s, err)) < 0) {
        return -1;
    }
    return n > 0 && s[n - 1].size == k->size && s[n - 1].time == k->time;
}

/* K (stamp k) is back at T: B says so (unless `back` already does), and S goes, as FH2MP's online copy removes it, so that an older
 * platform doesn't take K for its own copy (and remove it, or keep it without its mods' changes) but builds its copy again. */
static void gamedb_is_back(const gk_stamp *k, int back)
{
    WCHAR path[MAX_PATH];
    DWORD err;

    gamedb_file(path, L"gamedb-put-back.txt");
    if (!back && (err = gamedb_write_stamps(path, k, 1)) != 0) {
        log_begin();
        log_raw("gamedb: could not write fh2mp\\gamedb-put-back.txt, error ");
        log_dec(err);
        log_raw("\n");
    }
    lstrcpyW(path, g_exe_dir);
    lstrcatW(path, L"\\fh2-mods\\platform-db.txt");
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path)) {
        log_begin();
        log_raw("gamedb: removed fh2-mods\\platform-db.txt, so the platform doesn't take the car database put back for its own copy\n");
    } else if (!file_missing(err = GetLastError())) {
        log_begin();
        log_raw("gamedb: could not remove fh2-mods\\platform-db.txt, error ");
        log_dec(err);
        log_raw("\n");
    }
}

/* K isn't back at T (any more): B goes */
static void gamedb_not_back(void)
{
    WCHAR path[MAX_PATH];
    DWORD err;

    gamedb_file(path, L"gamedb-put-back.txt");
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path) && !file_missing(err = GetLastError())) {
        log_begin();
        log_raw("gamedb: could not remove fh2mp\\gamedb-put-back.txt, error ");
        log_dec(err);
        log_raw("\n");
    }
}

/* K becomes K', replacing the one there. -> 0, or the Windows error. */
static DWORD gamedb_retire_kept(void)
{
    WCHAR kept[MAX_PATH], prev[MAX_PATH];

    gamedb_file(kept, L"gamedb-kept.slt");
    gamedb_file(prev, L"gamedb-kept.previous.slt");
    SetFileAttributesW(prev, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(kept, prev, MOVEFILE_REPLACE_EXISTING)) {
        return GetLastError();
    }
    gamedb_not_back();
    return 0;
}

static void gamedb_log(const char *line)
{
    log_begin();
    log_raw("gamedb: ");
    log_raw(line);
    log_raw("\n");
    g_api->log(g_mod, line);
}

/* Online, before the server's copy goes over the database in mediaoverride (it isn't that copy): another mod's database there is kept as
 * K first, by way of K.tmp, with its date, and only then does the one kept before become K'. With T gone after K was put back, that mod
 * is gone: K becomes K' (gk_removed). -> 0, or the Windows error that stopped it (the server's copy is then not put there, and nothing is
 * lost); *unread when a file couldn't be read, and nothing was done. */
static DWORD gamedb_keep_other(int *unread)
{
    WCHAR kept[MAX_PATH], tmp[MAX_PATH];
    gk_stamp t, k;
    DWORD err;
    int have_kept, back, act, what = GK_OTHER, same = 0;

    *unread = 1;
    gamedb_file(kept, L"gamedb-kept.slt");
    have_kept = file_stamp(kept, &k.size, &k.time);
    if (!file_stamp(g_gamedb_path, &t.size, &t.time)) {
        if (!have_kept) {
            return 0;
        }
        if ((back = gamedb_back(&k, &err)) < 0) {
            return err;
        }
        *unread = 0;
        if (!gk_removed(back, gamedb_platform_built(), gk_platform_keeps(g_api->platform_version))) {
            return 0;
        }
        if ((err = gamedb_retire_kept()) == 0) {
            gamedb_log("the car database another mod had in mediaoverride was removed after it was put back, so it isn't put back "
                       "again (it's kept as fh2mp\\gamedb-kept.previous.slt)");
        }
        return err;
    }
    if ((err = gamedb_what(g_gamedb_path, &t, 0, &what)) != 0 ||
        (what == GK_OTHER && have_kept && (same = files_compare(kept, g_gamedb_path, &err)) < 0)) {
        return err;
    }
    *unread = 0;
    if (what == GK_BROKEN) {
        gamedb_log("mediaoverride's gamedbRC.slt isn't a whole database (a copy cut short?), so it isn't kept");
        return 0;
    }
    if ((act = gk_online(what, have_kept, same)) == GK_NOTHING) {
        return 0;
    }
    gamedb_dir();
    gamedb_file(tmp, L"gamedb-kept.slt.tmp");
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileW(g_gamedb_path, tmp, FALSE)) {
        return GetLastError();
    }
    /* FH2MP in another process may have put the server's copy there meanwhile */
    if ((same = files_compare(tmp, g_gamedb_src, &err)) != 0) {
        DeleteFileW(tmp);
        *unread = same < 0;
        return same < 0 ? err : 0;
    }
    if (act == GK_KEEP_REPLACING && (err = gamedb_retire_kept()) != 0) {
        DeleteFileW(tmp);
        return err;
    }
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    if (!MoveFileExW(tmp, kept, MOVEFILE_REPLACE_EXISTING)) {
        err = GetLastError();
        DeleteFileW(tmp);
        return err;
    }
    gamedb_not_back();
    gamedb_log("kept the car database another mod had in mediaoverride as fh2mp\\gamedb-kept.slt; it's put back when you play offline");
    return 0;
}

/* The facts gk_offline needs about T (there, stamp t): what it is, and whether it has K's bytes. -> 0, or the error when a file couldn't be
 * read. */
static DWORD gamedb_offline_facts(const WCHAR *target, const WCHAR *kept, const gk_stamp *t, int *what, int *same_as_kept)
{
    WCHAR server[MAX_PATH];
    DWORD err;
    int same;

    gamedb_file(server, L"gamedbRC.slt");
    *same_as_kept = 0;
    if ((same = files_compare(target, server, &err)) < 0 || (err = gamedb_what(target, t, same, what)) != 0) {
        return err;
    }
    if ((*what == GK_PLATFORM || *what == GK_OTHER) && (*same_as_kept = files_compare(kept, target, &err)) < 0) {
        return err;
    }
    return 0;
}

/* Offline (no server database in the session), from fh2mod_init, so before the platform's database step, which then sees what is put
 * back here: K goes back to mediaoverride where FH2MP's copy sat (gk_offline). Nothing is done while a file it is judged by can't be read,
 * a failure is logged, and nothing is deleted after one. */
static void gamedb_put_back(void)
{
    WCHAR kept[MAX_PATH], target[MAX_PATH];
    gk_stamp t, k;
    int have_t, back, what = GK_OTHER, same_as_kept = 0;
    const char *step = NULL;
    DWORD err = 0;

    gamedb_file(kept, L"gamedb-kept.slt");
    if (!file_stamp(kept, &k.size, &k.time) || !gamedb_game_file(target, L"gamedbRC.slt")) {
        return;
    }
    have_t = file_stamp(target, &t.size, &t.time);
    if ((back = gamedb_back(&k, &err)) < 0 || (have_t && (err = gamedb_offline_facts(target, kept, &t, &what, &same_as_kept)) != 0)) {
        log_begin();
        log_raw("gamedb: could not read the car database in mediaoverride, or the one kept (fh2mp\\gamedb-kept.slt), error ");
        log_dec(err);
        log_raw(", so both are left as they are this time\n");
        g_api->log(g_mod, "could not read the car database in mediaoverride, or the one kept (in use?), so both are left as they are "
                          "this time");
        return;
    }
    switch (gk_offline(have_t, what, same_as_kept, back, gamedb_platform_built(), gk_platform_keeps(g_api->platform_version))) {
    case GK_PUT_BACK:
        step = "put back the car database another mod had in mediaoverride (fh2mp\\gamedb-kept.slt)";
        gamedb_game_dirs();
        if ((err = gamedb_copy_over(kept, target)) != 0) {
            break;
        }
        gamedb_log("put back the car database another mod had in mediaoverride (playing offline)");
        gamedb_is_back(&k, back);
        break;
    case GK_IS_BACK:
        gamedb_is_back(&k, back);
        break;
    case GK_RETIRE_KEPT:
        step = "move fh2mp\\gamedb-kept.slt to fh2mp\\gamedb-kept.previous.slt";
        if ((err = gamedb_retire_kept()) != 0) {
            break;
        }
        gamedb_log(have_t ? "another car database is in mediaoverride now, so the one kept before is now fh2mp\\gamedb-kept.previous.slt"
                          : "the car database another mod had in mediaoverride was removed after it was put back, so it isn't put "
                            "back again (it's kept as fh2mp\\gamedb-kept.previous.slt)");
        break;
    case GK_NOT_BACK:
        if (back) {
            gamedb_not_back();
        }
        break;
    default:
        break;
    }
    if (err) {
        char line[160];
        lstrcpyA(line, "could not ");
        lstrcatA(line, step);
        log_begin();
        log_raw("gamedb: ");
        log_raw(line);
        log_raw(", error ");
        log_dec(err);
        log_raw("\n");
        g_api->log(g_mod, line);
    }
}

/* The mods (other than FH2MP) whose database changes the platform built into its copy, from fh2-mods\platform-db.txt (line 1: its
 * hash, then "mod<TAB>result" per mod), as "A, B"; the file is deleted so the platform builds its copy again next launch. */
static void gamedb_drop_platform_copy(char *names, int cap)
{
    WCHAR path[MAX_PATH];
    static char buf[4096];
    DWORD got = 0, i;
    HANDLE f;
    int n = 0;

    names[0] = 0;
    lstrcpyW(path, g_exe_dir);
    lstrcatW(path, L"\\fh2-mods\\platform-db.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!ReadFile(f, buf, sizeof buf - 1, &got, NULL)) {
        got = 0;
    }
    CloseHandle(f);
    buf[got] = 0;
    for (i = 0; i < got && buf[i] != '\n'; i++) {
    }
    while (i < got) {
        char *line = buf + ++i;
        DWORD k = 0;
        while (i < got && buf[i] != '\n' && buf[i] != '\t') {
            i++;
            k++;
        }
        if (k && buf[i] == '\t' && !(k == 5 && line[0] == 'F' && line[1] == 'H' && line[2] == '2' && line[3] == 'M' && line[4] == 'P')) {
            if (n && n + 2 < cap) {
                names[n++] = ',';
                names[n++] = ' ';
            }
            while (k-- && n + 1 < cap) {
                names[n++] = *line++;
            }
            names[n] = 0;
        }
        while (i < got && buf[i] != '\n') {
            i++;
        }
    }
    DeleteFileW(path);
}

/* A step of gamedb_place that failed: logged once per error code (`last`), as the caller tries again and online play is refused until
 * the copy is in place. */
static void gamedb_failed(DWORD *last, DWORD err, const char *what)
{
    char line[300];

    if (err == *last) {
        return;
    }
    *last = err;
    log_begin();
    log_raw("gamedb: ");
    log_raw(what);
    log_raw(", error ");
    log_dec(err);
    log_raw(" (trying again)\n");
    lstrcpyA(line, what);
    lstrcatA(line, "; trying again (until it is there, online play is refused)");
    g_api->log(g_mod, line);
}

/* gamedb_thread and the monitor both call gamedb_place; this keeps them one at a time. */
static SRWLOCK g_gamedb_place_lock = SRWLOCK_INIT;

/* Puts the launcher's copy in place when the file there is not that copy, keeping another mod's database first. Cheap when nothing changed
 * since the last call. -> 1 when the copy is in place. A failure is logged once per error code and tried again by the caller. */
static int gamedb_place_locked(void)
{
    static DWORD last_read, last_keep, last_err;
    ULONGLONG size, wtime;
    char others[256];
    DWORD err;
    int same, unread;

    if (!g_gamedb_src[0] || !g_gamedb_path[0]) {
        return 0;
    }
    if (g_gamedb_placed && file_stamp(g_gamedb_path, &size, &wtime) && size == g_gamedb_placed_size && wtime == g_gamedb_placed_time) {
        return 1;
    }
    if ((same = files_compare(g_gamedb_src, g_gamedb_path, &err)) < 0) {
        InterlockedExchange(&g_gamedb_placed, 0);
        gamedb_failed(&last_read, err, "could not read the car database in mediaoverride or the launcher's copy (in use?), so the "
                                       "server's is not put there yet");
        return 0;
    }
    if (!same) {
        if ((err = gamedb_keep_other(&unread)) != 0) {
            InterlockedExchange(&g_gamedb_placed, 0);
            gamedb_failed(&last_keep, err, unread ? "could not read the car database in mediaoverride, or the one kept (in use?), so "
                                                    "the server's is not put there yet"
                                                  : "could not keep the car database another mod had in mediaoverride "
                                                    "(fh2mp\\gamedb-kept.slt), so the server's is not put there yet");
            return 0;
        }
        gamedb_game_dirs();
        if ((err = gamedb_copy_over(g_gamedb_src, g_gamedb_path)) != 0) {
            InterlockedExchange(&g_gamedb_placed, 0);
            gamedb_failed(&last_err, err, "could not put the server's car database in mediaoverride");
            return 0;
        }
        log_begin();
        log_raw("gamedb: the server's car database is in mediaoverride\n");
    }
    last_read = last_keep = last_err = 0;
    gamedb_note_placed();
    gamedb_not_back();
    gamedb_drop_platform_copy(others, sizeof others);
    if (others[0]) {
        char line[400];
        lstrcpyA(line, "online play uses the server's car database, so the database changes of these mods are off this time: ");
        lstrcpynA(line + lstrlenA(line), others, (int)sizeof line - lstrlenA(line));
        log_begin();
        log_raw("gamedb: ");
        log_raw(line);
        log_raw("\n");
        g_api->log(g_mod, line);
        g_api->set_notice(g_mod, "Online: other mods' car data is off");
        g_api->apply_strings(g_mod);
    }
    if (file_stamp(g_gamedb_path, &size, &wtime)) {
        g_gamedb_placed_size = size, g_gamedb_placed_time = wtime;
        InterlockedExchange(&g_gamedb_placed, 1);
    }
    return 1;
}

static int gamedb_place(void)
{
    int ok;

    AcquireSRWLockExclusive(&g_gamedb_place_lock);
    ok = gamedb_place_locked();
    ReleaseSRWLockExclusive(&g_gamedb_place_lock);
    return ok;
}

static DWORD WINAPI gamedb_thread(LPVOID unused)
{
    int i;
    (void)unused;
    g_api->wait_for_mods(0xFFFFFFFFu);
    InterlockedExchange(&g_gamedb_started, 1);
    for (i = 0; i < 60 && !gamedb_place(); i++) {      /* the game reads the database early: retry quickly at first, the monitor then every 10 s */
        Sleep(1000);
    }
    return 0;
}

/*
 * Probe: what the game was told to build when it died. Crash reports of players (the platform's, Azimuth's) say where the game stopped, never
 * which message it was working on, and a malformed player object of one client can take a whole lobby down (prod 2026-10-23: 11 clients at once,
 * the server cannot see why from its side). The game's ObjectCreate receive handler (FUN_1417ab160(manager, id out, BitReader), the function that
 * builds a joiner's player object, the weather, the console ...) is hooked through the platform; before it runs, the bytes it is about to parse
 * (the datagram from the reader's position on, up to PROBE_BYTES) go into a ring of the last PROBE_ENTRIES calls. The ring lives in a file
 * mapping, data\bin\fh2mp-creates.ring, so it survives a crash; the one of the session before is kept as fh2mp-creates.prev (attach it to a
 * crash report; tools/decode_creates.py reads it). Nothing is sent anywhere. BitReader fields (FUN_141767100 / FUN_141767940): +4 size of the
 * buffer in bits, +8 byte index, +0xc bit position, +0x18 -> buffer object whose data pointer is at +0x10. Disable with env FH2MP_NO_PROBE.
 */
#define FH2_RVA_OBJECT_CREATE_RECV 0x17ab160u
#define PROBE_MAGIC 0x32434846u                     /* "FHC2" */
#define PROBE_ENTRIES 64
#define PROBE_BYTES 640

typedef struct {
    unsigned tick, seq, tid;
    int buf_bits;                                   /* size of the datagram being parsed, in bits */
    unsigned byte_idx, bit_pos, nbytes, reserved;
    unsigned char data[PROBE_BYTES];                /* from byte_idx on */
} probe_entry;

typedef struct {
    unsigned magic, version;
    volatile LONG next;                             /* number of calls recorded so far */
    unsigned entries, entry_size, start_tick, ft_low, ft_high;
    probe_entry e[PROBE_ENTRIES];
} probe_ring;

typedef unsigned long long (*object_create_fn)(void *mgr, void *out, void *reader);
static object_create_fn g_create_orig;
static probe_ring *g_probe;

static unsigned long long probe_object_create(void *mgr, void *out, void *reader)
{
    probe_ring *r = g_probe;
    unsigned char *rd = (unsigned char *)reader, *buf, *data;

    if (r && rd && !IsBadReadPtr(rd, 0x20) && (buf = *(unsigned char **)(rd + 0x18)) != NULL && !IsBadReadPtr(buf, 0x18) &&
        (data = *(unsigned char **)(buf + 0x10)) != NULL) {
        LONG n = InterlockedIncrement(&r->next) - 1;
        probe_entry *e = &r->e[(unsigned)n % PROBE_ENTRIES];
        int bits = *(int *)(rd + 4);
        unsigned idx = *(unsigned *)(rd + 8), room = bits > 0 && (unsigned)bits / 8 > idx ? (unsigned)bits / 8 - idx : 0;
        unsigned take = room < PROBE_BYTES ? room : PROBE_BYTES;

        e->nbytes = 0;                              /* a reader of the ring that sees a half-written entry sees nbytes 0 */
        e->tick = GetTickCount();
        e->seq = (unsigned)n;
        e->tid = GetCurrentThreadId();
        e->buf_bits = bits;
        e->byte_idx = idx;
        e->bit_pos = *(unsigned *)(rd + 0xc);
        if (take && !IsBadReadPtr(data + idx, take)) {
            mem_copy(e->data, data + idx, take);
            e->nbytes = take;
        }
    }
    return g_create_orig(mgr, out, reader);
}

static int install_create_probe(void)
{
    WCHAR path[MAX_PATH], prev[MAX_PATH], prev2[MAX_PATH], prev3[MAX_PATH];
    HANDLE f, m;
    FILETIME ft;
    unsigned char *img = find_fh2_image();

    if (!img || lstrlenW(g_dir) + 32 >= MAX_PATH) {
        return 0;
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-creates.ring");
    lstrcpyW(prev, g_dir);
    lstrcatW(prev, L"\\fh2mp-creates.prev");
    lstrcpyW(prev2, g_dir);
    lstrcatW(prev2, L"\\fh2mp-creates.prev2");
    lstrcpyW(prev3, g_dir);
    lstrcatW(prev3, L"\\fh2mp-creates.prev3");
    MoveFileExW(prev2, prev3, MOVEFILE_REPLACE_EXISTING);      /* the last three sessions' rings, newest first: .prev, .prev2, .prev3 */
    MoveFileExW(prev, prev2, MOVEFILE_REPLACE_EXISTING);
    MoveFileExW(path, prev, MOVEFILE_REPLACE_EXISTING);        /* the last session's ring, for a crash report */
    f = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return 0;
    }
    m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, sizeof(probe_ring), NULL);
    CloseHandle(f);
    if (!m) {
        return 0;
    }
    g_probe = (probe_ring *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(probe_ring));
    CloseHandle(m);
    if (!g_probe) {
        return 0;
    }
    GetSystemTimeAsFileTime(&ft);
    g_probe->magic = PROBE_MAGIC;
    g_probe->version = 1;
    g_probe->entries = PROBE_ENTRIES;
    g_probe->entry_size = sizeof(probe_entry);
    g_probe->start_tick = GetTickCount();
    g_probe->ft_low = ft.dwLowDateTime;
    g_probe->ft_high = ft.dwHighDateTime;
    if (!plat_hook("probe: ObjectCreate receive", img + FH2_RVA_OBJECT_CREATE_RECV, (void *)probe_object_create, (void **)&g_create_orig)) {
        log_begin();
        log_raw("probe: the ObjectCreate receive handler was not hooked\n");
        g_probe = NULL;
        return 0;
    }
    log_begin();
    log_raw("probe: the last 64 ObjectCreate messages this client receives are kept in fh2mp-creates.ring\n");
    return 1;
}

/*
 * wait_for_code gives up waiting for the game's code to be marked executable after 10 seconds and goes ahead; when Azimuth is held up
 * (a message box while it maps the game) our patches would then read pages that are not there yet (seen 2026-10-23: 0xc0000005 at
 * game+0x3b94f0). So also wait, without a limit, until every executable section of the mapped game is executable.
 */
static int page_executable(const void *p)
{
    MEMORY_BASIC_INFORMATION m;
    return VirtualQuery(p, &m, sizeof m) == sizeof m && m.State == MEM_COMMIT &&
           (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static void wait_for_executable_game(void)
{
    unsigned char *img;
    int logged = 0;

    for (;;) {
        const IMAGE_NT_HEADERS64 *nt;
        const IMAGE_SECTION_HEADER *sec;
        unsigned i, ok = 1;
        img = find_fh2_image();
        if (img) {
            nt = (const IMAGE_NT_HEADERS64 *)(img + ((const IMAGE_DOS_HEADER *)img)->e_lfanew);
            sec = IMAGE_FIRST_SECTION(nt);
            for (i = 0; i < nt->FileHeader.NumberOfSections && ok; i++) {
                if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && sec[i].Misc.VirtualSize &&
                    (!page_executable(img + sec[i].VirtualAddress) ||
                     !page_executable(img + sec[i].VirtualAddress + sec[i].Misc.VirtualSize - 1))) {
                    ok = 0;
                }
            }
            if (ok) {
                break;
            }
        }
        if (!logged++) {
            log_begin();
            log_raw("late: waiting for the game's code to be mapped and executable\n");
        }
        Sleep(50);
    }
}

/*
 * Everything that needs the game image runs here, once the platform says the game's code is ready.
 */
static DWORD WINAPI late_init(LPVOID unused)
{
    (void)unused;
    /* Azimuth shows its launcher first: the game is mapped only after Play is clicked, which can take arbitrarily long, so wait for it
     * without a limit. wait_for_code also waits for FH2 Extra Options to finish: it checks every place it patches before it changes any,
     * so a change of ours made first would switch all of it off. */
    g_api->wait_for_code(0xFFFFFFFFu);
    wait_for_executable_game();
    {
        wchar_t pr_env[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_PROBE", pr_env, 8) == 0) {
            install_create_probe();
        }
    }
    log_begin();
    log_raw(find_fh2_image() ? "late: game image mapped, applying patches\n"
                             : "late: game image not found, applying what is possible\n");
    if (has_arg_prefix(L"NetSessionDirectIpRoadTrip=")) {
        redirect_roadtrip_group();
    }
    if (has_arg_prefix(L"NetSessionDirectIpFreeRoam=")) {
        redirect_freeroam_group();
    }
    {
        /* every online mode (direct IP and the cloud matchmaking / Car Meet invite flow): each client simulates its own traffic */
        wchar_t tmp2[8];
        if (GetEnvironmentVariableW(L"FH2MP_NET_TRAFFIC", tmp2, 8) == 0) {
            patch_traffic_local_mode();
            patch_traffic_module_offline();
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
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_MMALLOW", tmp, 8) == 0) {
            patch_matchmaking_allowed();
        }
    }
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_QOSOK", tmp, 8) == 0) {
            patch_qos_assume_success();
        }
    }
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_SECUREIP", tmp, 8) == 0) {
            patch_secure_association();
        }
        patch_server_port();
        {
            wchar_t it_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_INVITETYPE", it_env, 8) == 0) {
                patch_invite_type();
            }
        }
        {
            wchar_t om_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_ONLINEMEET", om_env, 8) == 0) {
                patch_online_meet();
            }
        }
        {
            wchar_t lg_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_LIVEGATE", lg_env, 8) == 0) {
                patch_live_gates();
            }
        }
        {
            wchar_t ht_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_HOMETILE", ht_env, 8) == 0) {
                patch_home_tile();
            }
        }
        {
            wchar_t nt_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_NULLTORQUE", nt_env, 8) == 0) {
                patch_null_torque_curve();
            }
        }
        {
            wchar_t ss_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_SAMESESSION", ss_env, 8) == 0) {
                patch_same_session();
            }
        }
        {
            wchar_t pf_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_PLACEMENTFIX", pf_env, 8) == 0) {
                patch_placement_fallback();
            }
        }
        {
            wchar_t fg_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_FESTIVALGRID", fg_env, 8) == 0) {
                patch_festival_grid();
            }
        }
        {
            wchar_t rg_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_RIVALGATE", rg_env, 8) == 0) {
                patch_rival_gate();
            }
        }
        {
            wchar_t md_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_MPDIFF", md_env, 8) == 0) {
                patch_mp_difficulty();
            }
        }
        {
            wchar_t to_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_TRAFFICOWN", to_env, 8) == 0) {
                patch_traffic_request_ownership();
            }
        }
    }
#ifdef FH2MP_DEBUG_BUILD
    install_trace();
    dbg_start();
#endif
    /* Always run: it re-applies the Winsock patch and serves on-demand trace arming even when
     * no NetSessionDirectIp argument is given (its option patching is gated on the argument). */
    return option_monitor(NULL);
}

/*
 * Session handed over by the launcher: fh2mp-session.txt next to the log, "key=value" lines:
 *   xuid=<hex>      the Xbox user id the server assigned to the account (replaces the random per-install id)
 *   token=fh2.xxxx  the session token (the server derives the player's identity from it)
 *   gamedb=<path>   the car database the launcher synced (UTF-8, relative to the Azimuth folder or a Windows path), put in mediaoverride
 *                   (gamedb_place) and hashed there for every request (gamedb_hash)
 * Env FH2MP_TOKEN / FH2MP_XUID override the file (tests, rigs). The file is not deleted here (Azimuth may start the game in a second
 * process that loads this DLL again); the launcher removes it when the game exits. Returns 1 when an xuid was given.
 */
static int session_load(void)
{
    WCHAR path[MAX_PATH], env[100];
    char buf[1536];
    DWORD got = 0, i, n;
    HANDLE f;
    int have_xuid = 0;

    if (GetEnvironmentVariableW(L"FH2MP_TOKEN", env, 100) && env[0]) {
        for (n = 0; env[n] && n < sizeof g_token - 1; n++) {
            g_token[n] = (char)env[n];
        }
        g_token[n] = 0;
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-session.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, buf, sizeof buf - 1, &got, NULL)) {
            buf[got] = 0;
            for (i = 0; i < got;) {
                char *line = buf + i;
                char *eq = NULL;
                char *p = line;
                while (*p && *p != '\n') {
                    if (*p == '=' && !eq) {
                        eq = p;
                    }
                    p++;
                }
                i = (DWORD)(p - buf) + (*p ? 1 : 0);
                if (!*p) {
                    i = got;
                }
                *p = 0;
                if (p > line && p[-1] == '\r') {
                    p[-1] = 0;
                }
                if (!eq) {
                    continue;
                }
                *eq++ = 0;
                if (line[0] == 'x' && line[1] == 'u' && line[2] == 'i' && line[3] == 'd' && line[4] == 0) {
                    unsigned long long v = 0;
                    while (*eq) {
                        char c = *eq++;
                        if (c >= '0' && c <= '9') {
                            v = v * 16 + (unsigned)(c - '0');
                        } else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
                            v = v * 16 + (unsigned)((c | 0x20) - 'a' + 10);
                        }
                    }
                    if (v) {
                        g_xuid = v;
                        have_xuid = 1;
                    }
                } else if (line[0] == 't' && line[1] == 'o' && line[2] == 'k' && line[3] == 'e' && line[4] == 'n' && line[5] == 0 &&
                           !g_token[0]) {
                    for (n = 0; eq[n] && n < sizeof g_token - 1; n++) {
                        g_token[n] = eq[n];
                    }
                    g_token[n] = 0;
                } else if (lstrcmpA(line, "gamedb") == 0 && eq[0]) {
                    WCHAR rel[MAX_PATH];
                    int absolute = eq[0] == '\\' || (eq[0] && eq[1] == ':');
                    if (!g_api->game_dir[0] || lstrlenW(g_api->game_dir) + 40 >= MAX_PATH) {
                        /* the platform found no game folder (Azimuth's settings name none it can read): nothing can be put in place, so
                         * say so, and send "missing" so the server refuses the game openly instead of it running another database */
                        lstrcpyW(g_gamedb_path, L"\\\\?\\fh2mp\\no-game-folder\\gamedbRC.slt");
                        g_api->log(g_mod, "the game folder was not found (Azimuth's game folder setting), so the server's car database "
                                          "cannot be put in place: online play will be refused");
                        g_api->set_notice(g_mod, "FH2MP: game files not found\nSet the game folder in Azimuth");
                    } else if (MultiByteToWideChar(CP_UTF8, 0, eq, -1, rel, MAX_PATH) &&
                               lstrlenW(g_exe_dir) + lstrlenW(rel) + 2 < MAX_PATH) {
                        lstrcpyW(g_gamedb_src, absolute ? L"" : g_exe_dir);
                        lstrcatW(g_gamedb_src, absolute ? L"" : L"\\");
                        lstrcatW(g_gamedb_src, rel);
                        lstrcpyW(g_gamedb_path, g_api->game_dir);
                        lstrcatW(g_gamedb_path, L"\\mediaoverride\\Stripped\\gamedbRC.slt");
                    }
                }
            }
        }
        CloseHandle(f);
    }
    log_begin();
    log_raw("session: ");
    log_raw(g_token[0] ? "token present" : "no token");
    log_raw(g_gamedb_path[0] ? ", car database set" : "");
    log_raw(have_xuid ? ", xuid from the launcher\n" : "\n");
    return have_xuid;
}

static int identity_load(void)
{
    WCHAR path[MAX_PATH], env[32];
    char buf[24];
    DWORD got = 0, i;
    unsigned long long v = 0;
    HANDLE f;

    if (GetEnvironmentVariableW(L"FH2MP_XUID", env, 32)) {
        for (i = 0; env[i]; i++) {
            WCHAR c = env[i];
            v = v * 16 + (unsigned)(c >= L'0' && c <= L'9' ? c - L'0' : (c | 0x20) - L'a' + 10);
        }
        if (v) {
            g_xuid = v;
            return 1;
        }
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-identity.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, buf, 16, &got, NULL)) {
            for (i = 0; i < got; i++) {
                char c = buf[i];
                if (c >= '0' && c <= '9') {
                    v = v * 16 + (unsigned)(c - '0');
                } else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') {
                    v = v * 16 + (unsigned)((c | 0x20) - 'a' + 10);
                } else {
                    break;
                }
            }
        }
        CloseHandle(f);
    }
    if (v == 0) {
        unsigned long r = 0;
        BOOLEAN (WINAPI *rnd)(PVOID, ULONG) = NULL;
        HMODULE adv = LoadLibraryW(L"advapi32.dll");
        if (adv) {
            rnd = (BOOLEAN(WINAPI *)(PVOID, ULONG))GetProcAddress(adv, "SystemFunction036");
        }
        if (!rnd || !rnd(&r, sizeof r)) {
            r = GetTickCount() * 2654435761u ^ GetCurrentProcessId();
        }
        v = (unsigned long long)(r | 1u);
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            static const char hx[] = "0123456789abcdef";
            for (i = 0; i < 8; i++) {
                buf[i] = hx[(v >> (28 - 4 * i)) & 15];
            }
            buf[8] = '\n';
            WriteFile(f, buf, 9, &got, NULL);
            CloseHandle(f);
        }
    }
    g_xuid = 0x0009000000000000ULL | (v & 0xFFFFFFFFULL);
    return 1;
}

/* movabs r8, 0x0009000000000000 (49 B8 imm64) in Azimuth's User.XboxUserId formatting code */
static int patch_identity(void)
{
    static const unsigned char pat[10] = {0x49, 0xB8, 0, 0, 0, 0, 0, 0, 0x09, 0};
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt;
    IMAGE_SECTION_HEADER *sec;
    unsigned i, hits = 0;

    if (!base) {
        return 0;
    }
    nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    sec = IMAGE_FIRST_SECTION(nt);
    for (i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        unsigned char *p = base + sec[i].VirtualAddress;
        DWORD n = sec[i].Misc.VirtualSize, j;
        if (!(sec[i].Characteristics & IMAGE_SCN_CNT_CODE) || n < sizeof pat) {
            continue;
        }
        for (j = 0; j + sizeof pat <= n; j++) {
            if (p[j] == 0x49 && p[j + 1] == 0xB8 && mem_equal(p + j, pat, sizeof pat) &&
                plat_patch("identity: Azimuth's XboxUserId", p + j + 2, pat + 2, &g_xuid, 8)) {
                hits++;
            }
        }
    }
    log_begin();
    log_raw("identity: xuid 0x");
    log_hex(g_xuid, 16);
    log_raw(hits ? " (Azimuth's XboxUserId patched)\n" : " (Azimuth's XboxUserId constant NOT found)\n");
    return (int)hits;
}

#ifndef FH2MOD_PLATFORM_VERSION
#define FH2MOD_PLATFORM_VERSION "2.7.1"
#endif

/* Called by the FH2 Mod Platform on its own thread (outside the loader lock) right after it loads this DLL from fh2-mods. */
FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api)
{
    FH2ModInfo info;
    wchar_t path[MAX_PATH];
    int i;

    if (!api || !api->register_mod) {
        return 1;
    }
    info.size = sizeof info;
    info.name = "FH2MP";
    info.version = FH2MP_SHIM_VERSION;
    info.github_repo = NULL;                 /* the FH2MP launcher updates this DLL */
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates = 0;
    info.description = "Forza Horizon 2 online on the FH2MP servers: Free Roam, Road Trip, Rivals, Car Clubs and more. "
                       "Start the game from the FH2MP launcher to play online.";
    g_mod = api->register_mod(&info);
    if (!g_mod) {
        return 1;
    }
    if (api->api_version < 7) {              /* byte patches (patch_bytes) came with platform 2.6.0 */
        api->log(g_mod, "needs FH2 Mod Platform 2.6.0 or newer; start the game from the FH2MP launcher once to put it back");
        return 1;
    }
    g_api = api;
    if (lstrlenW(api->azimuth_dir) >= MAX_PATH - 32) {
        return 1;
    }
    lstrcpyW(g_exe_dir, api->azimuth_dir);
    /* logs, args and probe files live in data\bin (where the stock helper sits) when it exists */
    lstrcpyW(g_dir, g_exe_dir);
    lstrcatW(g_dir, L"\\data\\bin");
    if (GetFileAttributesW(g_dir) == INVALID_FILE_ATTRIBUTES) {
        lstrcpyW(g_dir, g_exe_dir);
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp.log");
    {   /* keep the log from growing for ever: past 8 MB the old one becomes fh2mp.log.1 */
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > (8u << 20)) {
            WCHAR old[MAX_PATH];
            lstrcpyW(old, path);
            lstrcatW(old, L".1");
            MoveFileExW(path, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    g_log = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    log_begin();
    log_raw("--- fh2mp shim " FH2MP_SHIM_VERSION " attached (FH2 Mod Platform ");
    log_raw(api->platform_version);
    log_raw("), tid=");
    log_dec(GetCurrentThreadId());
    log_raw("\n");

    /* Solo play keeps the same XboxUserId/save, without session loading, network hooks or worker threads. */
    {
        wchar_t offline[8];
        if (GetEnvironmentVariableW(L"FH2MP_OFFLINE", offline, 8) != 0) {
            identity_load();
            patch_identity();
            log_raw("offline: account save profile active; online hooks skipped\n");
            return 0;
        }
    }
    load_args();
    for (i = 0; i < g_argc; i++) {
        log_begin();
        log_raw("arg[");
        log_dec((unsigned long)i);
        log_raw("]=\"");
        log_wide(g_argv[i]);
        log_raw("\"\n");
    }
    {
        wchar_t tmp[8];
        if (!session_load()) {
            identity_load();
        }
        if (GetEnvironmentVariableW(L"FH2MP_NO_IDENTITY", tmp, 8) == 0) {
            patch_identity();
        }
    }
    if (!g_gamedb_src[0]) {
        gamedb_put_back();    /* offline: now, before the platform's database step, which then sees what was put back */
    }
    install_cmdline_hook();
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_RIVALSLEAVE", tmp, 8) == 0) {
            stateflow_rivals_leave_session();    /* now: the game reads its stateflow once, early */
        }
    }
    {
        HANDLE t = CreateThread(NULL, 0, late_init, NULL, 0, NULL);
        if (t) {
            CloseHandle(t);
        }
        t = g_gamedb_src[0] ? CreateThread(NULL, 0, gamedb_thread, NULL, 0, NULL) : NULL;
        if (t) {
            CloseHandle(t);
        }
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
    }
    return TRUE;
}
