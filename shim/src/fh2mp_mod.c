/*
 * FH2MP shim: fh2-mods\FH2MP.dll, a mod for the FH2 Mod Platform (Trinculo54, MIT; third_party/fh2-mod-platform).
 *
 * The platform is DWrite.dll (and the identical d3dcompiler_47.dll) next to azimuth.exe. It loads every DLL in fh2-mods and calls its fh2mod_init, so FH2MP
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "fh2mod.h"

#define MAX_ARGS 32
#define MAX_ARG_CHARS 256

/* Platform::Details::GetCmdArguments(int*): verify the complete function for each build. */
typedef struct {
    unsigned rva;
    unsigned size;
    unsigned long long fnv1a64;
} hook_build;
static const hook_build g_hook_builds[] = {
    {0x2909b0u, 9u, 0x4288f958fe9625cfull}, /* azimuth.exe 1.0.3 (sha256 ebfd5d43...26821f); resolver +2918d2 */
    {0x17fd80u, 9u, 0x4288f958fe9625cfull}, /* azimuth.exe 1.0.4 (sha256 56d2c9cb...b3b092); resolver +180ca2: the same code, new addresses */
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
static DWORD g_init_tick;     /* GetTickCount() when fh2mod_init started */

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

/* The platform prints one line per change it makes, with the name we give it ("* FH2MP: patch game+0x80C10F (name)") and keeps the pointer.
 * Our descriptive names are for the failure lines below; the platform gets only the part before the colon, as a stable interned string. */
static const char *plat_tag(const char *name)
{
    static char tags[64][24];
    static int used;
    char tag[24];
    int n = 0, i;
    while (name[n] && name[n] != ':' && n < 23) {
        tag[n] = name[n];
        n++;
    }
    tag[n] = 0;
    for (i = 0; i < used; ++i)
        if (!lstrcmpA(tags[i], tag)) return tags[i];
    if (used == 64) return "fh2mp";
    lstrcpyA(tags[used], tag);
    return tags[used++];
}

/* One line per change the platform did not make: which change, where, and at which step (the platform's own reason is in fh2-mods\platform.log). */
static void plat_failed(const char *name, const void *at, const char *step);

static int plat_patch(const char *name, void *at, const void *expect, const void *bytes, unsigned n)
{
    FH2Hook *h = g_api->patch_bytes(g_mod, plat_tag(name), at, expect, bytes, n);
    if (!h) {
        plat_failed(name, at, "refused (overlaps another mod's change, or the bytes differ from the exe on disk)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed(name, at, "created but could not be switched on");
        return 0;
    }
    return 1;
}

/* Replaces the function at `target` with `detour`; `original` (may be NULL) gets what to call for the game's own code. */
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    void *unused;
    FH2Hook *h = g_api->hook_function(g_mod, plat_tag(name), target, detour, original ? original : &unused);
    if (!h) {
        plat_failed(name, target, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed(name, target, "created but could not be switched on");
        return 0;
    }
    return 1;
}

/* Points one `call rel32` at `detour`; `original` gets the function it called. */
static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    FH2Hook *h = g_api->hook_call(g_mod, plat_tag(name), site, detour, original);
    if (!h) {
        plat_failed(name, site, "refused (not a call rel32, or overlaps another hook)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed(name, site, "created but could not be switched on");
        return 0;
    }
    return 1;
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
/* The id of this game start in every log (launcher, shim, server; native/internal/diag), from the session file; made here when a session has none. */
static char g_run[9];
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

#include "log.inc"

/* "48 8b 5c" for n bytes; a game update shows up as a different run of these in a "not patched" line */
static void log_hexbytes(const unsigned char *p, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (i) {
            log_raw(" ");
        }
        log_hex(p[i], 2);
    }
}

/* Index of the first byte that differs, or -1 when the n bytes are equal. */
static int bytes_first_diff(const void *a, const void *b, unsigned n)
{
    const unsigned char *x = a, *y = b;
    unsigned i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)i;
        }
    }
    return -1;
}

/* Game code that is not what a patch was written for (a game update, or another mod's change): which patch, where (RVA in the exe, build
 * 1.0.0.64) and how the bytes differ. `at` is where the n compared bytes start, `rva` that place's RVA. Always returns 0. */
static int log_code_mismatch(const char *tag, const char *what, unsigned rva, const void *actual, const void *expect, unsigned n)
{
    int d = bytes_first_diff(actual, expect, n);
    unsigned from = d < 0 ? 0 : (unsigned)d, show = n - from < 8 ? n - from : 8;

    log_begin();
    log_raw(tag);
    log_raw(": unexpected bytes in ");
    log_raw(what);
    log_raw(" at game+0x");
    log_hex(rva + from, 8);
    log_raw(" (first difference at +");
    log_dec(from);
    log_raw(" of ");
    log_dec(n);
    log_raw("): expected ");
    log_hexbytes((const unsigned char *)expect + from, show);
    log_raw(", found ");
    log_hexbytes((const unsigned char *)actual + from, show);
    log_raw("; not patched\n");
    return 0;
}

/* Compares n bytes of game code (at img+rva) with what the patch expects; 1 when equal, else the mismatch is logged and it is 0. */
/* 1 when `actual` is `expect` with another mod's inline hook at its start: a JMP rel32 (MinHook, as the platform's hook_function writes it, e.g. FH2
 * ANNA 1.1.0 on game+0xf5c900 / +0xf7bcc0) over the first 5 bytes, the rest the game's own. The platform chains hooks on one function, so ours
 * goes behind it. */
static int code_hooked_by_other(const unsigned char *actual, const void *expect, unsigned n)
{
    return n > 5 && actual[0] == 0xe9 && bytes_first_diff(actual + 5, (const unsigned char *)expect + 5, n - 5) < 0;
}

static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    if (bytes_first_diff(img + rva, expect, n) < 0) {
        return 1;
    }
    if (code_hooked_by_other(img + rva, expect, n)) {
        log_begin();
        log_raw(tag);
        log_raw(": ");
        log_raw(what);
        log_raw(" at game+0x");
        log_hex(rva, 8);
        log_raw(" starts with another mod's hook, the rest is the game's code: ours goes behind it through the platform\n");
        return 1;
    }
    return log_code_mismatch(tag, what, rva, img + rva, expect, n);
}

static void plat_failed(const char *name, const void *at, const char *step)
{
    HMODULE m = NULL;
    unsigned char *game = g_api ? (unsigned char *)g_api->game_base() : NULL;

    log_begin();
    log_raw(name ? name : "?");
    log_raw(": not patched, the platform ");
    log_raw(step);
    log_raw(", at ");
    if (game && (const unsigned char *)at >= game && (const unsigned char *)at < game + FH2_SIZE_OF_IMAGE) {
        log_raw("game+0x");
        log_hex((unsigned long long)((const unsigned char *)at - game), 8);
    } else if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &m) && m) {
        char file[MAX_PATH];
        DWORD n = GetModuleFileNameA(m, file, sizeof file), k = n;

        while (k > 0 && file[k - 1] != '\\' && file[k - 1] != '/') {
            k--;
        }
        file[n < sizeof file ? n : sizeof file - 1] = 0;
        log_raw(file + k);
        log_raw("+0x");
        log_hex((unsigned long long)((const unsigned char *)at - (const unsigned char *)m), 8);
    } else {
        log_raw("0x");
        log_hex((unsigned long long)(ULONG_PTR)at, 16);
    }
    log_raw(" (reason: fh2-mods\\platform.log)\n");
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
        static volatile LONG told;
        HMODULE ws2 = GetModuleHandleW(L"ws2_32.dll");

        if (!ws2) {
            return;
        }
        g_r_getsockname = (fh_f_getsockname)(void *)GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)6);
        g_r_bind = (fh_f_bind)(void *)GetProcAddress(ws2, (LPCSTR)(ULONG_PTR)2);
        if (!g_r_bind || !g_r_getsockname) {
            if (!InterlockedExchange(&told, 1)) {
                log_begin();
                log_raw("bindfix: ws2_32 has no bind/getsockname (ordinals 2/6), so a receive on an unbound socket is passed on unchanged and may fail with 10022\n");
            }
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

/* The game sends with WSASendTo; its slot points at fh_wsasendto in every build so that udp_ticket_send can go first (see there). */
typedef int(WINAPI *fh_f_wsasendto)(fh_sock, void *, DWORD, DWORD *, DWORD, const void *, int, void *, void *);
static fh_f_wsasendto g_r_wsasendto;
static void udp_ticket_send(fh_sock s, const void *bufs, DWORD cnt, const void *to, int tolen);
static void lighting_note_ticket(const void *to, int len, const unsigned char *nonce);

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
typedef int(WINAPI *fh_f_sendto)(fh_sock, const char *, int, int, const void *, int);
typedef int(WINAPI *fh_f_getaddrinfo)(const char *, const char *, const void *, void **);

static fh_f_startup g_r_startup;
static fh_f_socketw g_r_socketw;
static fh_f_setsockopt g_r_setsockopt;
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
    int r;
    DWORD err;
    LONG n = InterlockedIncrement(&g_n_send);

    udp_ticket_send(s, bufs, cnt, to, tolen);
    r = g_r_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cr);
    err = GetLastError();

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
/* release build: the two receive slots (implicit bind, see fh_ensure_bound) and WSASendTo (udp_ticket_send) are wrapped */
static int WINAPI fh_wsasendto(fh_sock s, void *bufs, DWORD cnt, DWORD *sent, DWORD flags, const void *to, int tolen, void *ov, void *cr)
{
    udp_ticket_send(s, bufs, cnt, to, tolen);
    return g_r_wsasendto(s, bufs, cnt, sent, flags, to, tolen, ov, cr);
}

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
    case 12: g_r_wsasendto = (fh_f_wsasendto)real; return (void *)fh_wsasendto;
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
            static volatile LONG missing;
            if (InterlockedIncrement(&missing) <= 3) {
                log_begin();
                log_raw("winsock: slot ");
                log_dec(i);
                log_raw(g_ws2_slots[i].name ? " " : " ordinal ");
                if (g_ws2_slots[i].name) {
                    log_raw(g_ws2_slots[i].name);
                } else {
                    log_dec(g_ws2_slots[i].ordinal);
                }
                log_raw(": ws2_32 does not export it (error ");
                log_dec(GetLastError());
                log_raw("), the game keeps Azimuth's offline function in this slot\n");
            }
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
            static volatile LONG refused;
            if (InterlockedIncrement(&refused) <= 3) {
                log_begin();
                log_raw("winsock: slot ");
                log_dec(i);
                log_raw(" (game+0x");
                log_hex(FH2_WS2_IAT_RVA + i * 8u, 8);
                log_raw(") could not be made writable, error ");
                log_dec(GetLastError());
                log_raw(": it keeps ");
                log_raw(oldmod);
                log_raw("\n");
            }
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
    static const unsigned char want[5] = {0xC6, 0x41, 0x20, 0x00, 0xC3};
    fn = img + FH2_TRAFFIC_NETMODE_RVA;
    if (!code_is("traffic", "the set-networked function", img, FH2_TRAFFIC_NETMODE_RVA, want, sizeof want)) {
        return 0;
    }
    return plat_patch("traffic: networked mode stays local", fn + 3, &zero, &one, 1);
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
    (void)i;
    if (!code_is("traffic", "SyncTrafficModule go-online", img, FH2_TRAFFIC_ONLINE_RVA, on_sig, sizeof on_sig) ||
        !code_is("traffic", "SyncTrafficModule go-offline", img, FH2_TRAFFIC_OFFLINE_RVA, off_sig, sizeof off_sig)) {
        return 0;
    }
    rel32 = (int)(tgt - (fn + 5));
    jmp[0] = 0xE9;
    mem_copy(jmp + 1, &rel32, 4);
    return plat_patch("traffic: SyncTrafficModule go-online switch", fn, on_sig, jmp, 5);
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
    (void)i;
    if (!code_is("mmallow", "IsMatchMakingAllowed", img, FH2_MM_ALLOWED_JE_RVA - 6, want, sizeof want)) {
        return 0;
    }
    return plat_patch("mmallow: IsMatchMakingAllowed without the CLobby flag", at + 6, want + 6, nop6, 6);
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
    (void)i;
    if (!code_is("qosok", "the match-request QoS lambda (test rdi / jne)", img, FH2_QOS_JNE_RVA - 3, want_jne, sizeof want_jne) ||
        !code_is("qosok", "the match-request QoS lambda (flag store)", img, FH2_QOS_FLAG_RVA - 6, want_flag, sizeof want_flag)) {
        return 0;
    }
    {
        /* both or neither: half of it would skip the association and still report failure */
        FH2Hook *ha = g_api->patch_bytes(g_mod, plat_tag("qosok: skip the secure association"), a + 3, want_jne + 3, nop2, 2);
        FH2Hook *hb = ha ? g_api->patch_bytes(g_mod, plat_tag("qosok: report success"), b + 6, want_flag + 6, &one, 1) : NULL;
        if (!hb || !g_api->enable_hooks(g_mod, ha) || !g_api->enable_hooks(g_mod, hb)) {
            if (ha) {
                g_api->disable_hooks(g_mod, ha);
            }
            plat_failed("qosok: match-request QoS lambda", ha ? b + 6 : a + 3,
                        !ha ? "refused the skip of the secure association" : !hb ? "refused the success flag (the skip is undone)"
                                                                               : "could not switch both changes on (the skip is undone)");
            return 0;
        }
    }
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
    if (!mr || !(args = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x100))) { /* hr = 0, endpoints = NULL */
        /* the resolved-address handler is never called, so this search stays on "Finding session" until the game gives up */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 5) {
            log_begin();
            log_raw(mr ? "secureip: could not allocate the resolver result, the match request gets no server address\n"
                       : "secureip: the match requester does not exist yet (game+0x36e2960 is null), the match request gets no server address\n");
        }
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
    (void)i;
    if (!code_is("secureip", "SecureIPResolver", img, FH2_SECURE_RESOLVE_RVA, want_fn, sizeof want_fn) ||
        !code_is("secureip", "the resolved-address handler", img, FH2_RESOLVED_HANDLER_RVA, want_handler, sizeof want_handler)) {
        return 0;
    }
    g_secure_img = img;
    return plat_hook("secureip: SecureIPResolver", img + FH2_SECURE_RESOLVE_RVA, (void *)secure_resolve_stub, NULL);
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
    (void)i;
    if (!code_is("invitetype", "MatchRequester::SetSession", img, FH2_SETSESSION_RVA, want, sizeof want)) {
        return 0;
    }
    g_invite_img = img;
    return plat_hook("invitetype: MatchRequester::SetSession", img + FH2_SETSESSION_RVA, (void *)set_session_stub, NULL);
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

    if (n == 0 || !img) {
        return 0;
    }
    if (n != 4) {
        log_begin();
        log_raw("serverport: FH2MP_SERVER_PORT must be 4 digits, it has ");
        log_dec(n);
        log_raw(" characters; the game keeps port 5000\n");
        return 0;
    }
    at = img + FH2_PORT_LITERAL_RVA;
    if (!code_is("serverport", "the \"5000\" service literal", img, FH2_PORT_LITERAL_RVA, "5000", 5)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (env[i] < L'0' || env[i] > L'9') {
            log_begin();
            log_raw("serverport: FH2MP_SERVER_PORT is not 4 digits; the game keeps port 5000\n");
            return 0;
        }
        port[i] = (unsigned char)env[i];
    }
    return plat_patch("serverport: game server port", at, "5000", port, 4);
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
    return code_is("mpdiff", "the assists bonus code", img, FH2_MPDIFF_INSN_RVA, want, sizeof want) &&
           plat_patch("mpdiff: online AI difficulty bonus", at, want, now, sizeof now);
}

/*
 * Bucket-list car manager probe (diagnostics, env FH2MP_CARPROBE=1): the per-frame update of the BucketListCarLoadingManager (RVA 0x3fc0f0)
 * decides per car whether it is near enough to be loaded. Every 3 s the manager's bytes and the first cars of its list are logged, to be
 * compared between an offline and an online run (the parked car shows offline only, 2026-10-29). Reads only.
 */
typedef void (*car_mgr_update_fn)(void *mgr);
static car_mgr_update_fn g_car_mgr_update;
static DWORD g_car_probe_tick;

static void car_probe_dump(const char *label, unsigned char *p, unsigned n)
{
    unsigned i;

    if (!p || IsBadReadPtr(p, n)) {
        return;
    }
    log_begin();
    log_raw("carprobe ");
    log_raw(label);
    log_raw(":");
    for (i = 0; i < n; i += 8) {
        log_raw(" ");
        log_hex(*(unsigned long long *)(p + i), 16);
    }
    log_raw("\n");
}

static void car_mgr_update_stub(void *mgr)
{
    DWORD now = GetTickCount();

    if (now - g_car_probe_tick >= 3000) {
        unsigned char *m = (unsigned char *)mgr, **node;
        int k;

        g_car_probe_tick = now;
        car_probe_dump("manager+0x38", m + 0x38, 0x88);
        node = (m && !IsBadReadPtr(m + 0x50, 8)) ? *(unsigned char ***)(m + 0x50) : NULL;       /* circular list: sentinel at +0x50 */
        for (k = 0; node && node != (unsigned char **)(m + 0x50) && k < 4 && !IsBadReadPtr(node, 0x48); k++) {
            unsigned char *car = (unsigned char *)node[4];
            char label[16];

            label[0] = 'n'; label[1] = (char)('0' + k); label[2] = 0;
            car_probe_dump(label, (unsigned char *)node, 0x48);
            if (car && !IsBadReadPtr(car, 0x1c0)) {
                car_probe_dump("car+0x120", car + 0x120, 0x60);
                car_probe_dump("car+0x160", car + 0x160, 0x10);
                car_probe_dump("car+0x1b0", car + 0x1b0, 0x10);
            }
            node = (unsigned char **)node[0];
        }
    }
    g_car_mgr_update(mgr);
}

static int install_car_probe(void)
{
    unsigned char *img = find_fh2_image();

    if (!img) {
        return 0;
    }
    return plat_hook("carprobe: bucket-list car manager update", img + 0x3fc0f0, (void *)car_mgr_update_stub, (void **)&g_car_mgr_update);
}

/*
 * Stall sampler (diagnostics, env FH2MP_SAMPLE=1): once a second every thread of the process is stopped for a moment and, when its
 * instruction pointer is inside the game's image, "sample tid=<n> game+0x<rva>" is logged. A search that stalls for 30 s with no network traffic
 * (dev 2026-10-29) shows where the game waits. Nothing is written to the game.
 */
#include <tlhelp32.h>

static DWORD WINAPI stall_sampler(LPVOID unused)
{
    unsigned char *img = NULL;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    (void)unused;

    for (;;) {
        HANDLE snap;
        THREADENTRY32 te;
        DWORD tids[96];
        ULONG_PTR rvas[96], rip[96], callers[96][6];
        int ncall[96];
        int n = 0, i, j;

        Sleep(1000);
        if (!img) {
            img = find_fh2_image();
            if (!img) {
                continue;
            }
        }
        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) {
            continue;
        }
        te.dwSize = sizeof te;
        if (Thread32First(snap, &te)) {
            do {
                HANDLE h;
                CONTEXT ctx;

                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
                    continue;
                }
                h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
                if (!h) {
                    continue;
                }
                if (SuspendThread(h) != (DWORD)-1) {
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    if (n < 96 && GetThreadContext(h, &ctx)) {
                        /* the game's code addresses on the top of the stack: who the thread is waiting for */
                        ULONG_PTR *sp = (ULONG_PTR *)ctx.Rsp;
                        int k, nc = 0;

                        for (k = 0; k < 256 && nc < 6; k++) {
                            ULONG_PTR v;
                            SIZE_T got = 0;
                            if (!ReadProcessMemory(GetCurrentProcess(), sp + k, &v, sizeof v, &got) || got != sizeof v) {
                                break;
                            }
                            if (v >= (ULONG_PTR)img && v < (ULONG_PTR)img + 0x3A18000u) {
                                callers[n][nc++] = v - (ULONG_PTR)img;
                            }
                        }
                        if (nc || (ctx.Rip >= (ULONG_PTR)img && ctx.Rip < (ULONG_PTR)img + 0x3A18000u)) {
                            tids[n] = te.th32ThreadID;      /* logged after every thread runs again: a stopped one may hold the log's lock */
                            rip[n] = ctx.Rip;
                            rvas[n] = (ctx.Rip >= (ULONG_PTR)img && ctx.Rip < (ULONG_PTR)img + 0x3A18000u) ? ctx.Rip - (ULONG_PTR)img : 0;
                            ncall[n++] = nc;
                        }
                    }
                    ResumeThread(h);
                }
                CloseHandle(h);
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        for (i = 0; i < n; i++) {
            log_begin();
            log_raw("sample tid=");
            log_dec(tids[i]);
            if (rvas[i]) {
                log_raw(" game+0x");
                log_hex(rvas[i], 8);
            } else {
                HMODULE m = NULL;
                char name[64];
                name[0] = 0;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)rip[i], &m) && m) {
                    GetModuleFileNameA(m, name, sizeof name);
                }
                log_raw(" in ");
                log_raw(name[0] ? name : "?");
                log_raw("+0x");
                log_hex(name[0] ? rip[i] - (ULONG_PTR)m : rip[i], 8);
            }
            log_raw(" via");
            for (j = 0; j < ncall[i]; j++) {
                log_raw(" 0x");
                log_hex(callers[i][j], 7);
            }
            log_raw("\n");
        }
    }
    return 0;
}

/*
 * Match ticket polling. The game asks the matchmaking service for the state of a ticket every 5000 ms (FUN_14183fc20 and the status
 * handler FUN_14183ae80 each write next_poll = network_time + 5000 at ticket + 0xa4), so a search that the service answers at once still
 * waits 5 to 6 s for its first poll and the same again for the next step; the service's waitTime is ignored (dev 2026-10-29: waitTime 0
 * and 1 both gave 5 s; "lobbies were finding for a fucking eternity"). The immediates of the two `add eax, 5000` (RVAs 0x183aff2 and
 * 0x183fc3f) become 500. Disable with env FH2MP_NO_MATCHPOLL.
 */
#define FH2_MATCHPOLL_RVA_A 0x183aff2u
#define FH2_MATCHPOLL_RVA_B 0x183fc3fu
#define FH2_MATCHPOLL_RVA_C 0x183afcdu
static int patch_match_poll(void)
{
    static const unsigned char want[] = {0x05, 0x88, 0x13, 0x00, 0x00};     /* add eax, 5000 */
    static const unsigned char now[] = {0x05, 0xF4, 0x01, 0x00, 0x00};      /* add eax, 500 */
    static const unsigned rvas[3] = {FH2_MATCHPOLL_RVA_A, FH2_MATCHPOLL_RVA_B, FH2_MATCHPOLL_RVA_C};
    unsigned char *img = find_fh2_image();
    int i, done = 0;

    if (!img) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        unsigned char *at = img + rvas[i];

        if (memcmp(at, now, sizeof now) == 0) {
            done++;
        } else if (code_is("matchpoll", i == 1 ? "the ticket poll timer" : i ? "the ticket retry interval" : "the ticket status handler", img, rvas[i], want,
                           sizeof want) &&
                   plat_patch(i == 1 ? "matchpoll: ticket poll interval (poll timer)" : i ? "matchpoll: ticket retry interval" : "matchpoll: ticket poll interval (status handler)",
                              at + 1, want + 1, now + 1, 4)) {
            done++;
        }
    }
    return done == 3;
}

/*
 * Slow "Finding session" (dev 2026-10-29). The request that starts an online search (the object ticked by FUN_1417b3330) has a first state that
 * gives up after 10 s and a second that gives up after 30 s (`sub eax,[rbx+0x2ec] ; cmp eax,10000` / `... [rbx+0x2f0] ; cmp eax,30000`, then
 * target state 6, the same path as success). A healthy search never gets there: the secure-association state-change event (FUN_1417925d0 sets
 * +0x2f4) moves it on within a second. In some processes that event never comes (3 of 33 game starts in a day of dev logs: every search of the
 * process took 30 s; 20 of 33 had a first search of 11-17 s), and nothing in the request needs the QoS result here, so the wait was dead time.
 * Both limits become 3 s. Disable with env FH2MP_NO_MMTIMEOUT.
 */
#define FH2_MMTIMEOUT_A_RVA 0x17B3361u /* 3d 10 27 00 00 : cmp eax, 10000 (then jbe) */
#define FH2_MMTIMEOUT_B_RVA 0x17B33B1u /* 3d 30 75 00 00 : cmp eax, 30000 (then jbe) */
static int patch_mm_timeouts(void)
{
    static const unsigned char a_old[] = {0x3D, 0x10, 0x27, 0x00, 0x00, 0x76, 0x5B};
    static const unsigned char a_new[] = {0x3D, 0xB8, 0x0B, 0x00, 0x00, 0x76, 0x5B};
    static const unsigned char b_old[] = {0x3D, 0x30, 0x75, 0x00, 0x00, 0x76, 0x0B};
    static const unsigned char b_new[] = {0x3D, 0xB8, 0x0B, 0x00, 0x00, 0x76, 0x0B};
    unsigned char *img = find_fh2_image();
    int done = 0;

    if (!img) {
        return 0;
    }
    if (memcmp(img + FH2_MMTIMEOUT_A_RVA, a_new, sizeof a_new) == 0 ||
        (code_is("mmtimeout", "the match request tick (first-state limit)", img, FH2_MMTIMEOUT_A_RVA, a_old, sizeof a_old) &&
         plat_patch("mmtimeout: match request, first-state limit", img + FH2_MMTIMEOUT_A_RVA + 1, a_old + 1, a_new + 1, 4))) {
        done++;
    }
    if (memcmp(img + FH2_MMTIMEOUT_B_RVA, b_new, sizeof b_new) == 0 ||
        (code_is("mmtimeout", "the match request tick (second-state limit)", img, FH2_MMTIMEOUT_B_RVA, b_old, sizeof b_old) &&
         plat_patch("mmtimeout: match request, second-state limit", img + FH2_MMTIMEOUT_B_RVA + 1, b_old + 1, b_new + 1, 4))) {
        done++;
    }
    return done == 2;
}

/*
 * "Matchmaking failed due to losing your connection to Xbox Live" after a cancelled Car Meet search (user 2026-10-29). The matchmaking-failure
 * handler (RVA 0x80c0a0..) picks the text per reason; for the Live case it shows the toast only when a state byte +0x12d is set and +0x12e is
 * clear (`cmp byte [rax+0x12e],0 ; jne skip`, 75 4b / 75 44 at RVA 0x80c10f / 0x80c285). The game is never really signed in to Xbox Live, so the
 * byte is always clear. Both jumps become unconditional (EB), so that toast is skipped; matchmaking itself is unaffected (it kept working
 * with the toast showing). Disable with env FH2MP_NO_MMTOAST.
 */
static int patch_mm_failed_toast(void)
{
    static const unsigned char cmp12e[] = {0x80, 0xB8, 0x2E, 0x01, 0x00, 0x00, 0x00};
    static const unsigned rvas[2] = {0x80C10Fu - 7, 0x80C285u - 7};
    static const unsigned char jne_a[] = {0x75, 0x4B}, jmp_a[] = {0xEB, 0x4B}, jne_b[] = {0x75, 0x44}, jmp_b[] = {0xEB, 0x44};
    unsigned char *img = find_fh2_image();
    int i, done = 0;

    if (!img) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        unsigned char *at = img + rvas[i];
        const unsigned char *want = i ? jne_b : jne_a, *now = i ? jmp_b : jmp_a;

        if (!code_is("mmtoast", "the matchmaking failure handler (cmp byte [rax+0x12e],0)", img, rvas[i], cmp12e, sizeof cmp12e)) {
            continue;
        }
        if (memcmp(at + 7, now, 2) == 0 ||
            (code_is("mmtoast", "the matchmaking failure handler (jne)", img, rvas[i] + 7, want, 2) &&
             plat_patch("mmtoast: no \"lost Xbox Live\" toast on a failed matchmaking", at + 7, want, now, 2))) {
            done++;
        }
    }
    return done == 2;
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
    if (!code_is("trafficown", "the RequestOwnership trigger (cmp byte [rcx+0xaa],0)", img, FH2_TRAFFIC_REQOWN_RVA - (unsigned)sizeof cmp_ctx, cmp_ctx,
                 sizeof cmp_ctx) ||
        !code_is("trafficown", "the RequestOwnership trigger (je)", img, FH2_TRAFFIC_REQOWN_RVA, je, sizeof je)) {
        return 0;
    }
    return plat_patch("trafficown: RequestOwnership on every collision", at, je, nop2, 2);
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
            static const unsigned char want_gate[9] = {0x80, 0xB8, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x85};
            if (jne[0] == 0xE9) {
                done++;           /* already patched */
                continue;
            }
            log_code_mismatch("livegate", "a Live-only menu gate (cmp byte [rax+0xf0],0 / jne)", cmp_rva[i], c, want_gate, sizeof want_gate);
            continue;
        }
        mem_copy(&rel, jne + 2, 4);
        rel += 1;
        jmp[0] = 0xE9;
        mem_copy(jmp + 1, &rel, 4);
        jmp[5] = 0x90;
        if (!plat_patch("livegate: Live-only menu entry", jne, jne, jmp, 6)) {
            continue;
        }
        done++;
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
    (void)i;
    if (!code_is("hometile", "the Home tile test (cmp [rbx+0x537],r13b / jnz)", img, 0xddb494u, want, sizeof want)) {
        return 0;
    }
    return plat_patch("hometile: Online Road Trip tile", p + 7, want + 7, &jmp, 1);
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
    (void)i;
    if (!code_is("samesession", "the session validation handler", img, 0x17b07beu, want, sizeof want)) {
        return 0;
    }
    return plat_patch("samesession: join a found session with the remembered name", p + 2, want + 2, &jmp, 1);
}

/*
 * "Azimuth stopped: exception 0xc0000005 at game+0x1064408 reading 0x0" when a car appears. FUN_1410643b0 reads an engine's torque curve
 * (TorqueScale, ZeroThrottleTorqueScale, NumTorqueValues, then one value per RPM entry) from the result of
 * "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=<id>"; the callers (FUN_14105d690 for engines, FUN_14105e700 for the electric motor)
 * pass that result in. With an id that has no row (a part id the game's database does not know) the result is null and the first read,
 *     141064405  mov rcx,[rsi]      (rsi = pointer to the result)
 *     141064408  mov rax,[rcx]      <- rcx = 0
 * crashes the client of everybody who loads that car: seen on prod on 2026-10-20 whenever a player joined one particular lobby, the same
 * call chain every time (call chain and registers read from three minidumps). This guard returns when the result is null, leaving the output
 * as it was. This only guards the null database read: remote cars ALSO reach the later physics constructor, which requires a valid curve
 * count (#84/#128/#145). It is not a safe fallback for that constructor. The function is hooked through the platform with a small stub
 * as the detour: it does the check, then jumps on to the platform's
 * `original` (the game's first instructions, 48 8B C4 mov rax,rsp / 48 89 50 10 mov [rax+0x10],rdx, and the rest of the function).
 * Disable with env FH2MP_NO_NULLTORQUE.
 */
#include "speed_pb.inc"

static int patch_speed_pb(void)
{
    static const struct { unsigned rva; unsigned char bytes[16]; } signatures[] = {
        {0x6c2ca0, {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56}},
        {0x6b76b0, {0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0xa8,0x18,0xff}},
        {0x5b0b70, {0x48,0x89,0x54,0x24,0x10,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe}},
        {0x5b0b9d, {0xe8,0x0e,0x08,0x03,0x00,0xc5,0xfa,0x11,0x83,0x20,0x01,0x00,0x00,0x8b,0x83,0x1c}},
        {0x3eb620, {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xda,0xe8,0x52,0x18,0x01,0x00,0x48,0x8b}},
        {0x3d2d70, {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x48}},
        {0x5e13b0, {0xc5,0xfb,0x10,0x81,0xe0,0xa2,0x00,0x00,0xc5,0xf9,0x5a,0xc0,0xc3,0xcc,0xcc,0xcc}},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;
    if (!img) return 0;
    for (i = 0; i < sizeof signatures / sizeof signatures[0]; ++i) {
        if (!code_is("speedpb", "the speed trap/zone completion or HUD comparison code", img, signatures[i].rva, signatures[i].bytes, 16)) {
            return 0;
        }
    }
    g_speed_pb_tls = TlsAlloc();
    if (g_speed_pb_tls == TLS_OUT_OF_INDEXES) {
        log_begin();
        log_raw("speedpb: not patched, no thread-local slot left (TlsAlloc failed, error ");
        log_dec(GetLastError());
        log_raw(")\n");
        return 0;
    }
    g_speed_pb_resolve = (speed_pb_resolve_fn)(img + 0x3eb620);
    g_speed_pb_property = (speed_pb_property_fn)(img + 0x3d2d70);
    /* Partial installation stays inert until every hook is available. */
    if (!plat_hook_call("speedpb: local PB fallback in HUD comparison", img + 0x5b0b9d,
                        (void *)speed_pb_score, (void **)&g_speed_pb_score) ||
        !plat_hook("speedpb: HUD comparison scope", img + 0x5b0b70,
                   (void *)speed_pb_compare, (void **)&g_speed_pb_compare) ||
        !plat_hook("speedpb: saved trap PB before completion", img + 0x6c2ca0,
                   (void *)speed_pb_trap, (void **)&g_speed_pb_trap) ||
        !plat_hook("speedpb: saved zone PB before completion", img + 0x6b76b0,
                   (void *)speed_pb_zone, (void **)&g_speed_pb_zone)) return 0;
    InterlockedExchange(&g_speed_pb_active, 1);
    return 1;
}

#include "torque_scale.inc"

static int patch_null_torque_scale(void)
{
    static const unsigned char want[10] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18};
    unsigned char *img = find_fh2_image();
    unsigned i;
    if (!img) {
        return 0;
    }
    (void)i;
    if (!code_is("nulltorquescale", "the upgrade torque reader", img, 0x105e530u, want, sizeof want)) {
        return 0;
    }
    return plat_hook("nulltorquescale: missing upgrade torque rows", img + 0x105e530u,
                     (void *)torque_scale_guard, (void **)&g_torque_scale_orig);
}

/* #43: startup's Cinematics directory query can return NULL. The adjacent optional-directory queries check this before calling
 * Exists (+0xb0); this one did not (game 1.0.0.64, +0x21f44d). Preserve the existing false/skip branch for a missing directory. */
static unsigned char cinematics_exists(void *directory)
{
    if (!directory) {
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 3) {
            log_begin();
            log_raw("cinematics: the game's media/Cinematics directory query returned null (game+0x21f44d); its optional registration is skipped\n");
        }
        return 0;
    }
    return ((unsigned char (*)(void *))(*(void ***)directory)[0xb0 / 8])(directory);
}

static int patch_missing_cinematics(void)
{
    static const unsigned char want[] = {0x48, 0x8b, 0x01, 0xff, 0x90, 0xb0, 0, 0, 0};
    unsigned char call[] = {0xff, 0x15, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    unsigned char *img = find_fh2_image(), *site, *slot;
    long long delta;
    int disp;
    if (!img) {
        return 0;
    }
    site = img + 0x21f44du;
    if (!code_is("cinematics", "the Cinematics directory check", img, 0x21f44du, want, sizeof want)) {
        return 0;
    }
    if (!(slot = alloc_near(site))) {
        log_begin();
        log_raw("cinematics: not patched, no free page within 2 GB of game+0x21f44d (VirtualAlloc error ");
        log_dec(GetLastError());
        log_raw(")\n");
        return 0;
    }
    delta = (long long)(slot - (site + 6));
    if (delta < -0x80000000ll || delta > 0x7fffffffll) {
        VirtualFree(slot, 0, MEM_RELEASE);
        log_begin();
        log_raw("cinematics: not patched, the page found is out of rel32 reach of game+0x21f44d\n");
        return 0;
    }
    *(void **)slot = (void *)cinematics_exists;
    disp = (int)delta;
    mem_copy(call + 2, &disp, sizeof disp);
    if (!plat_patch("cinematics: missing directory check", site, want, call, sizeof call)) {
        VirtualFree(slot, 0, MEM_RELEASE);
        return 0;
    }
    return 1;
}

static volatile unsigned *g_nulltorque_hits;      /* counts missing-row reads, not rejected car loads */

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
    (void)i;
    if (!code_is("nulltorque", "the torque curve reader", img, 0x10643b0u, want, sizeof want)) {
        return 0;
    }
    stub = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!stub) {
        log_begin();
        log_raw("nulltorque: could not allocate the stub, not patched (VirtualAlloc error ");
        log_dec(GetLastError());
        log_raw(")\n");
        return 0;
    }
    mem_copy(stub, code, sizeof code);
    h = g_api->hook_function(g_mod, plat_tag("nulltorque: torque curve reader"), site, stub, &original);
    if (!h) {
        plat_failed("nulltorque: torque curve reader", site, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    *(void **)(stub + 56) = original;
    *(unsigned *)(stub + 64) = 0;
    FlushInstructionCache(GetCurrentProcess(), stub, 68);
    g_nulltorque_hits = (volatile unsigned *)(stub + 64);
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed("nulltorque: torque curve reader", site, "created but could not be switched on");
        return 0;
    }
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
    if (bytes_first_diff(site, want, sizeof want) >= 0) {
        if (site[3] == 0xff && site[4] == 0x15) {
            return 1;                 /* already patched */
        }
        return code_is("placement", "the CreatePlacementTexture2D call", img, 0x10c8971u, want, sizeof want);
    }
    (void)i;
    g_place_force = GetEnvironmentVariableW(L"FH2MP_PLACEMENT_FORCE", env, 8) != 0;
    slot = alloc_near(site);
    if (!slot) {
        log_begin();
        log_raw("placement: could not allocate a page near the game image, not patched (VirtualAlloc error ");
        log_dec(GetLastError());
        log_raw(")\n");
        return 0;
    }
    *(void **)slot = (void *)placement_hook;
    disp = (long long)(slot - (site + 3 + 6));
    if (disp > 0x7fffffffll || disp < -0x80000000ll) {
        log_begin();
        log_raw("placement: not patched, the call slot is out of rel32 reach of game+0x10c8974\n");
        return 0;
    }
    {
        int d32 = (int)disp;
        mem_copy(call + 2, &d32, 4);
    }
    return plat_patch("placement: CreatePlacementTexture2D fallback", site + 3, want + 3, call, sizeof call);
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
        log_raw("festivalgrid: unexpected bytes in the end-event-grid call at game+0x8131a0: found ");
        log_hexbytes(site, 5);
        log_raw(site[0] == 0xe8 ? " (a call to game+0x" : " (not a call rel32; expected e8 and a target of game+0x627c20)");
        if (site[0] == 0xe8) {
            log_hex((unsigned long long)(site + 5 + d32 - img), 8);
            log_raw(", expected game+0x627c20)");
        }
        log_raw("; not patched\n");
        return 0;
    }
    g_fg_img = img;
    return plat_hook_call("festivalgrid: end-event grid placement", site, (void *)festival_grid_hook, (void **)&g_fg_place);
}

/*
 * Rivals "race this event" gate. Pressing A on a Rivals event (Monthly, Festival Routes, ...) calls FUN_140d7b680, which only
 * starts the race when FUN_140666f40 (RVA 0x666f40) returns true, else it raises FUN_140e68760's "Feature unavailable: cannot connect
 * to Xbox Live" popup. That function begins with the same CLobby+0xf0 test as the Live gates above:
 *     cmp byte [rax+0xf0], 0 ; jne +7 ; xor al,al ; ret      (80 B8 F0 00 00 00 00 75 07 32 C0 ...)
 * then checks the Live sign-in state (FUN_140a42090) and the permissions 0x56..0x58 and the user flag mask 0x80. Those pass once the
 * Logon response carries the permission table (native/internal/lsp/notif.go), so only the byte test is removed: jne (75 07) becomes jmp (EB 07).
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
    (void)i;
    if (!code_is("rivalgate", "the Rivals race gate (cmp byte [rax+0xf0],0 / jne)", img, 0x666f4bu, want, sizeof want)) {
        return 0;
    }
    return plat_patch("rivalgate: Rivals race gate", at + 7, want + 7, &jmp, 1);
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
    (void)i;
    if (bytes_first_diff(at, want, sizeof want) >= 0) {
        if (at[2] == 0x90 && at[3] == 0x90) {
            return 1;       /* already patched */
        }
        return code_is("onlinemeet", "the Car Meet view model tick", img, FH2_ONLINEMEET_TEST_RVA, want, sizeof want);
    }
    return plat_patch("onlinemeet: Car Meet counts as online", at + 2, want + 2, nop2, 2);
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

#include "game_image.inc"
#include "xsapi_image.inc"
#include "net_string_guard.inc"
#include "grid_trace.inc"
#include "diag_trace.inc"
#include "party_hud.inc"
#include "race_toast.inc"
#include "car_pool.inc"
#include "torque_load_guard.inc"
#include "online_setup.inc"
#include "hook_guard.inc"
#include "gameplaylog_guard.inc"
#include "remote_prediction_guard.inc"
#include "picker_pi.inc"
#include "autodrive_rule.inc"

static int patch_torque_load(void)
{
    static const struct { unsigned rva; unsigned char bytes[16]; } signatures[] = {
        {0xfcfe70, {0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0x80}},
        {0x104d5f0, {0x44,0x89,0x44,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x54,0x41}},
        {0x1042d10, {0x48,0xc7,0x01,0xff,0xff,0xff,0xff,0x33,0xd2,0x4c,0x8d,0x41,0x08,0x0f,0x1f,0x00}},
        {0x10482f0, {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x57,0x41,0x55,0x41}},
        {0x1048990, {0x48,0x8b,0xc4,0x48,0x89,0x50,0x10,0x48,0x89,0x48,0x08,0x55,0x57,0x41,0x55,0x41}},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;
    if (!img) return 0;
    for (i = 0; i < sizeof signatures / sizeof signatures[0]; ++i) {
        if (!code_is("torqueload", "the native car preloader/part resolver", img, signatures[i].rva, signatures[i].bytes, 16)) {
            return 0;
        }
    }
    g_torque_load_img = img;
    g_torque_parts_ctor = (torque_parts_ctor_fn)(img + 0x1042d10);
    g_torque_stock = (torque_stock_fn)(img + 0x104d5f0);
    g_torque_car_part = (torque_part_fn)(img + 0x10482f0);
    g_torque_engine_part = (torque_part_fn)(img + 0x1048990);
    return plat_hook("torqueload: native fallback for invalid engine curves", img + 0xfcfe70,
                     (void *)torque_preload_guard, (void **)&g_torque_preload_orig);
}

static int patch_net_string_receivers(void)
{
    static const struct {
        unsigned rva;
        unsigned char bytes[16];
        const char *name;
        void *detour;
        net_string_receive_fn *original;
    } hooks[] = {
        {0x176d570, {0x48,0x8b,0xc4,0x55,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa1},
         "netstrings: OpenConnection bounds", (void *)net_string_open, &g_string_open_orig},
        {0x17f7440, {0x48,0x8b,0xc4,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x68,0xa1,0x48,0x81,0xec,0x90},
         "netstrings: CarMeet session string bounds", (void *)net_string_meet, &g_string_meet_orig},
        {0x181f630, {0x48,0x8b,0xc4,0x55,0x57,0x41,0x54,0x41,0x56,0x41,0x57,0x48,0x8b,0xec,0x48,0x81},
         "netstrings: BucketList partner string bounds", (void *)net_string_bucket, &g_string_bucket_orig},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;
    int ok = 1;
    if (!img) return 0;
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        if (!code_is("netstrings", hooks[i].name + 12, img, hooks[i].rva, hooks[i].bytes, sizeof hooks[i].bytes)) {
            return 0;
        }
    }
    /* Each independent handler is safe as soon as its own hook is enabled; plat_hook names the ones it could not hook. */
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i)
        if (!plat_hook(hooks[i].name, img + hooks[i].rva, hooks[i].detour,
                       (void **)hooks[i].original)) ok = 0;
    return ok;
}

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
            if (read_xsapi_headers((const unsigned char *)mbi.AllocationBase)) {
                return (unsigned char *)mbi.AllocationBase;
            }
        }
        if (!mbi.RegionSize || mbi.RegionSize > (UINT_PTR)-1 - (UINT_PTR)mbi.BaseAddress ||
            (UINT_PTR)mbi.BaseAddress + mbi.RegionSize <= (UINT_PTR)addr) {
            break;
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

/* Retry verdict for a patch whose imports of img are not bound yet: 0 (retry) for up to 60 s from the first miss, then 2
 * (give up). Logs the wait once and the give-up once. *since is the patch's own first-miss tick (0 = none yet). */
static int xs_unbound_wait(const char *tag, const char *what, DWORD *since)
{
    if (!*since) {
        *since = GetTickCount() | 1;
        log_begin();
        log_raw(tag);
        log_raw(": waiting, Microsoft.Xbox.Services.dll is mapped but ");
        log_raw(what);
        log_raw(" is not bound yet\n");
        return 0;
    }
    if (GetTickCount() - *since < 60000) {
        return 0;
    }
    log_begin();
    log_raw(tag);
    log_raw(": not patched, ");
    log_raw(what);
    log_raw(" was still not bound 60 s after Microsoft.Xbox.Services.dll was mapped\n");
    return 2;
}

static void *g_xs_auth; /* the shared Authentication object returned by the stub */

/* How many token requests XSAPI made through the stub, and when the first came (diagnostics for "the online setup after Logon never ran":
 * no request at all means the game never asked XSAPI, a late one means XSAPI waited for something else). */
static volatile LONG g_xs_calls;
static DWORD g_xs_hooked_at;

static void *xs_token_stub(void *a, void *b, void *c, void *d)
{
    void **vt;
    (void)a; (void)b; (void)c; (void)d;
    if (InterlockedIncrement(&g_xs_calls) == 1) {
        char msg[96];
        wsprintfA(msg, "xstoken: first token request %lu ms after the hook\n", (unsigned long)(GetTickCount() - g_xs_hooked_at));
        log_begin();
        log_raw(msg);
    }
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
/* where the xuid in use came from (for the "identity:" line) */
static const char *g_xuid_source = "Azimuth's fixed id (no identity was loaded)";

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
    (void)i;
    if (bytes_first_diff(f1, sig_get, sizeof sig_get) >= 0) {
        log_code_mismatch("xstoken", "Microsoft.Xbox.Services.dll GetTokenAndSignature", XS_GETTOKEN_RVA, f1, sig_get, sizeof sig_get);
        return 2;
    }
    if (bytes_first_diff(f2, sig_all, sizeof sig_all) >= 0) {
        log_code_mismatch("xstoken", "Microsoft.Xbox.Services.dll GetTokenAndSignatureForAllUsers", XS_GETTOKEN_ALL_RVA, f2, sig_all, sizeof sig_all);
        return 2;
    }
    call = f1 + XS_ALLOC_CALL_OFF;
    if (bytes_first_diff(call, alloc_sig, sizeof alloc_sig) >= 0) {
        log_code_mismatch("xstoken", "the allocator call in GetTokenAndSignature", XS_GETTOKEN_RVA + XS_ALLOC_CALL_OFF, call, alloc_sig, sizeof alloc_sig);
        return 2;
    }
    mem_copy(&rel, call + 7, 4);
    alloc = *(void *(**)(ULONGLONG))(call + 11 + rel);
    if (!xs_find_import_slot(img, "WindowsCreateString")) {
        log_begin();
        log_raw("xstoken: not patched, WindowsCreateString is not in the XSAPI import table\n");
        return 2;
    }
    mkstr = (LONG(*)(const WCHAR *, unsigned, void **))xs_find_import(img, "WindowsCreateString");
    {
        static DWORD since;
        if (!xs_bound(img, (void *)alloc) || !xs_bound(img, (void *)mkstr)) {
            return xs_unbound_wait("xstoken", !xs_bound(img, (void *)alloc) ? "its allocator import" : "its WindowsCreateString import", &since);
        }
    }
    ctor = (void *(*)(void *))(img + XS_AUTH_CTOR_RVA);
    if (!alloc || !mkstr) {
        log_begin();
        log_raw("xstoken: could not resolve the allocator (");
        log_raw(alloc ? "found" : "null");
        log_raw(") or WindowsCreateString in the XSAPI import table (");
        log_raw(mkstr ? "found" : "missing");
        log_raw("); not patched\n");
        return 2;
    }
    mem = alloc(0x48);
    if (!mem) {
        log_begin();
        log_raw("xstoken: not patched, the XSAPI allocator returned null for the 0x48-byte Authentication object\n");
        return 2;
    }
    obj = ctor(mem);
    lstrcpyW(tok, tok_head);
    xuid_decimal(tok + (sizeof tok_head / sizeof tok_head[0] - 1));
    tok_len = (unsigned)lstrlenW(tok);
    if (!obj || mkstr(tok, tok_len, &h_tok) < 0 ||
        mkstr(sgn, (unsigned)(sizeof sgn / sizeof sgn[0] - 1), &h_sig) < 0) {
        log_begin();
        log_raw(obj ? "xstoken: not patched, WindowsCreateString failed for the token or signature\n"
                    : "xstoken: not patched, the Authentication constructor returned null\n");
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
        return 2;
    }
    g_xs_hooked_at = GetTickCount();
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
    LONG hr = g_sda_mkstr ? g_sda_mkstr(s, (unsigned)(sizeof s / sizeof s[0] - 1), &h) : (LONG)0x80004005;
    if (hr >= 0) {
        return h; /* the caller owns it and releases it with WindowsDeleteString */
    }
    {
        /* the session member then has an empty secureDeviceAddress and XSAPI refuses to serialize it ("Unable to join session") */
        static volatile LONG told;
        if (InterlockedIncrement(&told) <= 3) {
            log_begin();
            log_raw("sdastr: WindowsCreateString failed, hr=0x");
            log_hex((unsigned long long)(unsigned)hr, 8);
            log_raw("; the game gets a null secure device address string\n");
        }
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
    (void)i;
    if (bytes_first_diff(fn, sig, sizeof sig) >= 0) {
        log_code_mismatch("sdastr", "the secure device address string getter", FH2_SDA_STR_RVA, fn, sig, sizeof sig);
        return 2;
    }
    mk = xs_find_import(img, "WindowsCreateString");
    if (!mk && xs_find_import_slot(xs, "WindowsCreateString")) {
        static DWORD since;
        mk = xs_find_import(xs, "WindowsCreateString");
        if (!xs_bound(xs, mk)) {
            return xs_unbound_wait("sdastr", "its WindowsCreateString import", &since);
        }
    }
    if (!mk) {
        log_begin();
        log_raw("sdastr: WindowsCreateString is in neither the game's nor XSAPI's import table; not patched\n");
        return 2;
    }
    g_sda_mkstr = (LONG(*)(const WCHAR *, unsigned, void **))mk;
    return plat_hook("sdastr: SecureDeviceAddress string", fn, (void *)sda_string_stub, NULL) ? 1 : 2;
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

static void sts_auth_stub(void *a, void *b, void *c, void *d, void **ref)
{
    void **cb = (void **)ref[1];
    msvc_wstr tok;
    msvc_wstr sig;

    (void)a; (void)b; (void)c; (void)d;
    msvc_wstr_set(&tok, L"fh2mp");
    msvc_wstr_set(&sig, L"fh2mp");
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
    (void)i;
    if (!code_is("sts", "StsAuthTokenManager::GetAuthToken", img, FH2_STS_GETAUTH_RVA, sig, sizeof sig)) {
        return 0;
    }
    return plat_hook("sts: StsAuthTokenManager::GetAuthToken", fn, (void *)sts_auth_stub, NULL);
}

static int install_cmdline_hook(void)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    unsigned char *target = NULL;
    unsigned long long h[sizeof g_hook_builds / sizeof g_hook_builds[0]] = {0};
    size_t i;

    for (i = 0; i < sizeof g_hook_builds / sizeof g_hook_builds[0]; i++) {
        unsigned char *cand = exe + g_hook_builds[i].rva;
        if (!IsBadReadPtr(cand, g_hook_builds[i].size)) {
            h[i] = fnv1a64(cand, g_hook_builds[i].size);
            if (h[i] == g_hook_builds[i].fnv1a64) {
                target = cand;
                break;
            }
        }
    }
    if (!target) {
        log_begin();
        log_raw("hook: GetCmdArguments not found in this azimuth.exe, not patched (an Azimuth version this shim does not know); fnv1a64 of the");
        for (i = 0; i < sizeof g_hook_builds / sizeof g_hook_builds[0]; i++) {
            log_raw(i ? ", " : " ");
            log_dec(g_hook_builds[i].size);
            log_raw(" bytes at azimuth.exe+0x");
            log_hex(g_hook_builds[i].rva, 6);
            log_raw(h[i] ? " is " : " (unreadable) ");
            if (h[i]) {
                log_hex(h[i], 16);
            }
            log_raw(" (expected ");
            log_hex(g_hook_builds[i].fnv1a64, 16);
            log_raw(")");
        }
        log_raw("; launch arguments will not be injected\n");
        return 0;
    }
    return plat_hook("hook: Azimuth Platform::Details::GetCmdArguments", target, (void *)hooked_get_cmd_arguments, NULL);
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

/* The platform's cached pointer must still identify the mapped game. */
static unsigned char *find_fh2_image(void)
{
    unsigned char *img = g_api ? (unsigned char *)g_api->game_base() : NULL;
    IMAGE_NT_HEADERS64 nt;
    if (read_game_headers(img, &nt, NULL)) {
        return img;
    }
    if (img) {
        /* a readable image that is not FH2 1.0.0.64 (another build, or another title): every game patch is then skipped; said once per image */
        static void *volatile told;
        IMAGE_DOS_HEADER dos;
        if (told != img && read_game_memory(img, &dos, sizeof dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
            dos.e_lfanew < 0x100000 && read_game_memory(img + dos.e_lfanew, &nt, sizeof nt) && nt.Signature == IMAGE_NT_SIGNATURE) {
            told = img;
            log_begin();
            log_raw("late: the game image at 0x");
            log_hex((unsigned long long)(ULONG_PTR)img, 12);
            log_raw(" is not FH2 1.0.0.64, unexpected TimeDateStamp 0x");
            log_hex(nt.FileHeader.TimeDateStamp, 8);
            log_raw(" (expected 0x");
            log_hex(FH2_TIMESTAMP, 8);
            log_raw(") SizeOfImage 0x");
            log_hex(nt.OptionalHeader.SizeOfImage, 8);
            log_raw(" (expected 0x");
            log_hex(FH2_SIZE_OF_IMAGE, 8);
            log_raw("); no game patch is applied\n");
        }
    }
    return NULL;
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
        log_raw("redirect: ");
        log_raw(name);
        log_raw(" group not redirected, FH2 1.0.0.64 image not found in this process\n");
        return 0;
    }
    lea = img + lea_rva;
    if (lea[0] != 0x4c || lea[1] != 0x8d || lea[2] != 0x05) {
        static const unsigned char want_lea[3] = {0x4c, 0x8d, 0x05};
        log_code_mismatch("redirect", name, lea_rva, lea, want_lea, sizeof want_lea);
        return 0;
    }
    mem_copy(&oldDisp, lea + 3, 4);
    if ((lea + 7 + oldDisp) != (img + str_rva) || !str_equal((const char *)(img + str_rva), oldstr)) {
        log_begin();
        log_raw("redirect: unexpected target, ");
        log_raw(name);
        log_raw(" group not redirected: the lea at game+0x");
        log_hex(lea_rva, 8);
        log_raw(" points to game+0x");
        log_hex((unsigned long long)(lea + 7 + oldDisp - img), 8);
        log_raw(", expected game+0x");
        log_hex(str_rva, 8);
        log_raw(" holding \"");
        log_raw(oldstr);
        log_raw("\"\n");
        return 0;
    }

    newstr = alloc_near(lea);
    if (!newstr) {
        log_begin();
        log_raw("redirect: could not allocate a page near the game image, ");
        log_raw(name);
        log_raw(" group not redirected (VirtualAlloc error ");
        log_dec(GetLastError());
        log_raw(")\n");
        return 0;
    }
    for (i = 0; i < newstr_len; i++) {
        newstr[i] = (unsigned char)newstr_src[i];
    }
    newDisp = (long long)(newstr - (lea + 7));
    if (newDisp > 0x7fffffffll || newDisp < -0x80000000ll) {
        log_begin();
        log_raw("redirect: ");
        log_raw(name);
        log_raw(" group not redirected, the allocated page is out of rel32 reach of game+0x");
        log_hex(lea_rva, 8);
        log_raw("\n");
        return 0;
    }
    {
        int d32 = (int)newDisp;
        return plat_patch("redirect: matchmaking group path", lea + 3, &oldDisp, &d32, 4);
    }
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
 * UDP connection ticket (security audit 2026-10-25, F01). The game's OpenConnection only names an xuid, so the game server could tell
 * players apart by source address alone, and two players behind one NAT could claim each other's id. Before every datagram that starts
 * with an OpenConnection, the same socket sends this 84-byte ticket to the same address; the server binds that address to the xuid when
 * the MAC checks out (fh2mp_server.Hub.ticket). Layout:
 *   0..10  a NerdLib packet header with no messages (ack id 0xFFFF, remote ack 0xFFFE, count 0): servers without tickets drop it unread
 *   11..18 "FH2MPTK1", 19 version 1, 20..27 xuid (LE), 28..35 unix time (LE), 36..51 nonce (single use), 52..83 HMAC-SHA256 of 0..51
 * The key is HMAC-SHA256(session token, "fh2mp udp ticket v1"); the LSP derives the same from the token it sees and hands it to the game
 * server with the sign-in announcement (auth.udp_ticket_key). No token (offline, rigs without a launcher) = no ticket.
 */
static unsigned char g_ticket_key[32];
static int g_ticket_ready;
static volatile LONG g_ticket_n;

static void udp_ticket_key(void)
{
    static const char label[] = "fh2mp udp ticket v1";

    if (g_token[0]) {
        hmac_sha256((const unsigned char *)g_token, (unsigned long long)lstrlenA(g_token), (const unsigned char *)label, sizeof label - 1,
                    g_ticket_key);
        g_ticket_ready = 1;
    }
}

static void udp_ticket_send(fh_sock s, const void *bufs, DWORD cnt, const void *to, int tolen)
{
    static const unsigned char head[19] = {0xFF, 0xFF, 0xFE, 0xFF, 0, 0, 0, 0, 0, 84, 0, 'F', 'H', '2', 'M', 'P', 'T', 'K', '1'};
    const unsigned char *d;
    unsigned char t[84], seed[40], h[32];
    unsigned long long now, v;
    LARGE_INTEGER qpc;
    FILETIME ft;
    sha256_ctx c;
    DWORD sent = 0, x;
    struct {
        ULONG len;
        unsigned char *buf;
    } wb;
    LONG n;
    int i;

    if (!g_ticket_ready || !bufs || cnt < 1 || !g_r_wsasendto) {
        return;
    }
    d = *(unsigned char *const *)((const char *)bufs + 8); /* WSABUF {ULONG len; CHAR *buf} */
    if (*(const ULONG *)bufs < 12 || !d || d[8] == 0 || (d[11] & 0x7F) != 0) {
        return; /* not a packet whose first message is an OpenConnection (type 0) */
    }
    n = InterlockedIncrement(&g_ticket_n);
    GetSystemTimeAsFileTime(&ft);
    now = (((unsigned long long)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL;
    QueryPerformanceCounter(&qpc);
    for (i = 0; i < 19; i++) {
        t[i] = head[i];
    }
    t[19] = 1;
    for (i = 0, v = g_xuid; i < 8; i++, v >>= 8) {
        t[20 + i] = (unsigned char)v;
    }
    for (i = 0, v = now; i < 8; i++, v >>= 8) {
        t[28 + i] = (unsigned char)v;
    }
    /* the nonce only has to be unique, the MAC makes it unforgeable: hash of the clock, the counter and the process/thread ids */
    for (i = 0, v = (unsigned long long)qpc.QuadPart; i < 8; i++, v >>= 8) {
        seed[i] = (unsigned char)v;
    }
    for (i = 0, v = GetTickCount64(); i < 8; i++, v >>= 8) {
        seed[8 + i] = (unsigned char)v;
    }
    for (i = 0, x = GetCurrentProcessId(); i < 4; i++, x >>= 8) {
        seed[16 + i] = (unsigned char)x;
    }
    for (i = 0, x = GetCurrentThreadId(); i < 4; i++, x >>= 8) {
        seed[20 + i] = (unsigned char)x;
    }
    for (i = 0, x = (DWORD)n; i < 4; i++, x >>= 8) {
        seed[24 + i] = (unsigned char)x;
    }
    for (i = 0; i < 12; i++) {
        seed[28 + i] = t[24 + i]; /* high xuid bytes and the time */
    }
    sha256_init(&c);
    sha256_update(&c, seed, sizeof seed);
    sha256_final(&c, h);
    for (i = 0; i < 16; i++) {
        t[36 + i] = h[i];
    }
    hmac_sha256(g_ticket_key, 32, t, 52, t + 52);
    wb.len = sizeof t;
    wb.buf = t;
    if (g_r_wsasendto(s, &wb, 1, &sent, 0, to, tolen, NULL, NULL) != 0) {
        /* without the ticket the server cannot bind this address to the account and refuses the OpenConnection that follows */
        DWORD err = GetLastError();
        if (n <= 10 || n % 50 == 0) {
            log_begin();
            log_raw("ticket: could not send the connection ticket (#");
            log_dec((unsigned long)n);
            log_raw("), WSA error ");
            log_dec(err);
            log_raw("; the server will probably refuse the OpenConnection\n");
        }
        SetLastError(err);
    } else if (n <= 3) {
        log_begin();
        log_raw("ticket: sent before OpenConnection #");
        log_dec((unsigned long)n);
        log_raw("\n");
    }
    lighting_note_ticket(to, tolen, t + 36);
}

#include "perf.inc"
#include "lighting_sync.inc"
#include "honk_sync.inc"
#include "carsel_sync.inc"
#include "event_stock.inc"
#include "stall_trace.inc"

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

/* One exact replacement in a file of one of the game's media zips (stateflow.zip, UI.zip), handed to the platform (set_file <set_path>).
 * Nothing is changed unless the old text occurs exactly once. `tag` starts the log lines. */
static int stateflow_replace(const WCHAR *zip_name, const char *file, const char *set_path, const char *old, size_t olen, const char *neu, size_t nlen,
                             const char *tag)
{
    WCHAR zip[MAX_PATH];
    unsigned char *orig, *out;
    size_t size = 0, at = 0, i, hits = 0;
    int ok;

    if (!g_api->game_dir[0] || lstrlenW(g_api->game_dir) + lstrlenW(zip_name) + 8 >= MAX_PATH) {
        log_begin();
        log_raw(tag);
        log_raw(g_api->game_dir[0] ? ": the game folder path is too long to open the zip, " : ": the platform found no game folder (Azimuth's game folder setting), ");
        log_raw(file);
        log_raw(" not changed\n");
        return 0;
    }
    lstrcpyW(zip, g_api->game_dir);
    lstrcatW(zip, zip_name);
    orig = zip_read_file(zip, file, &size);
    if (!orig) {
        log_begin();
        log_raw(tag);
        log_raw(": could not read ");
        log_raw(file);
        log_raw(" from ");
        log_wide(zip_name + 1);
        log_raw(" (missing, unreadable or not a zip the reader handles), not changed\n");
        return 0;
    }
    for (i = 0; i + olen <= size; i++) {
        if (orig[i] == (unsigned char)old[0] && mem_equal(orig + i, (const unsigned char *)old, olen)) {
            at = i;
            hits++;
        }
    }
    if (hits != 1 || !(out = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size - olen + nlen))) {
        HeapFree(GetProcessHeap(), 0, orig);
        log_begin();
        log_raw(tag);
        log_raw(": the state in ");
        log_raw(file);
        if (hits != 1) {
            log_raw(" is not as expected (the text to replace occurs ");
            log_dec((unsigned long)hits);
            log_raw(" times in ");
            log_dec((unsigned long)size);
            log_raw(" bytes, it must occur once: another mod or a game update changed it), not changed\n");
        } else {
            log_raw(" could not be rewritten (out of memory), not changed\n");
        }
        return 0;
    }
    mem_copy(out, orig, at);
    mem_copy(out + at, neu, nlen);
    mem_copy(out + at + nlen, orig + at + olen, size - at - olen);
    ok = g_api->set_file(g_mod, set_path, out, size - olen + nlen);
    HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, orig);
    if (ok) return ok;
    log_begin();
    log_raw(tag);
    {
        log_raw(": ");
        log_raw(file);
        log_raw(" was not changed (another mod replaces it, see fh2-mods\\platform.log)\n");
    }
    return ok;
}

static int stateflow_rivals_leave_session(void)
{
    return stateflow_replace(L"\\media\\stateflow.zip", "global.xml", "stateflow/global.xml", RIVALS_LAUNCH_OLD, sizeof RIVALS_LAUNCH_OLD - 1, RIVALS_LAUNCH_NEW,
                             sizeof RIVALS_LAUNCH_NEW - 1, "rivalsleave");
}

/*
 * Bucket-list cars in online Free Roam. The game parks a car at every bucket-list location once the BucketListCarLoadingManager has
 * been told EnableBucketListCarLoading (native 0x43ef60). Only the offline flow (stateflow free_roam_start.xml, enter_offline and
 * finalise_free_roam_offline) broadcasts it; network.xml's enter_free_roam, the online entry, never does, so online there are slots
 * and markers but no parked cars (user 2026-10-29: "still no parked cars in online"). The same broadcast goes into the online entry, at the
 * same place as offline: before the wait for streaming around the player's car. Off by default (retail online Free Roam shows no parked bucket-list cars; user footage 2026-10-29); opt in with env FH2MP_BUCKETCARS=1. (Probe 2026-10-29: the loader goes to state 3 then 4
 * and all 12 car components are added after it, yet no car showed; the per-car event EnableBucketListCar is sent too now.)
 */
static const char BUCKET_CARS_OLD[] =
    "    <node state=\"global.wait_for_streaming_around_local_human_car\" />\r\n"
    "    <node state=\"finalise_free_roam_online\" />";
static const char BUCKET_CARS_NEW[] =
    "    <node state=\"global.broadcast_event\" event=\"EnableBucketListCarLoading\" />\r\n"
    "    <node state=\"global.broadcast_event\" event=\"EnableBucketListCar\" />\r\n"
    "    <node state=\"global.wait_for_streaming_around_local_human_car\" />\r\n"
    "    <node state=\"finalise_free_roam_online\" />\r\n"
    "    <node state=\"global.broadcast_event\" event=\"EnableBucketListCarLoading\" />\r\n"
    "    <node state=\"global.broadcast_event\" event=\"EnableBucketListCar\" />";

static int stateflow_bucket_cars_online(void)
{
    return stateflow_replace(L"\\media\\stateflow.zip", "network.xml", "stateflow/network.xml", BUCKET_CARS_OLD, sizeof BUCKET_CARS_OLD - 1, BUCKET_CARS_NEW,
                             sizeof BUCKET_CARS_NEW - 1, "bucketcars");
}

/*
 * Race toast layout (src/race_toast.inc): UI.zip's Controls/NotificationPopupControl.xaml lays the toast title (line 1) out as TitlePreGamertag, the
 * bold gamertag and TitlePostGamertag, and line 2 as 'Press (X) to join'. The shim puts the track's name in TitlePreGamertag (empty for races). For a
 * race event (IsRaceEvent) line 1 no longer shows it, and line 2 starts with it in the gamertag's bold style: 'Castelletto Meadow Circuit | Press (X) to
 * join' (user 2026-11-05). Only converters and styles the file already uses. Disable with env FH2MP_NO_RACETOAST.
 */
static const char RACE_TOAST_OLD[] =
    "                                <local:FZTextBlock Text=\"{Binding TitlePreGamertag}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Title}\"/>\r\n"
    "                                <local:FZTextBlock Text=\"{Binding UserProfile.FormattedGamertag}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Gamertag}\" />\r\n"
    "                                <local:FZTextBlock Text=\"{Binding TitlePostGamertag}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Title}\"/>\r\n"
    "                            </StackPanel>\r\n"
    "                            <Grid Visibility=\"{Binding CanBeActivated, Converter={StaticResource A_FZBooleanToVisibilityConverter}}\">\r\n"
    "                                <Grid Visibility=\"{Binding PlayerHasAccepted, Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\">\r\n"
    "                                    <StackPanel Orientation=\"Horizontal\" \r\n"
    "                                            Visibility=\"{Binding IsInputBlocked, Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\">\r\n"
    "                                        <local:FZTextBlock Text=\"{local:Loc Lobby.IDS_Invite_Join_Prefix}\"\r\n"
    "                                                       Style=\"{StaticResource A_Style_FZTextBlock_Notification_BodyText}\"/>\r\n";
static const char RACE_TOAST_NEW[] =
    "                                <local:FZTextBlock Text=\"{Binding TitlePreGamertag}\"\r\n"
    "                                                   Visibility=\"{Binding IsRaceEvent, Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Title}\"/>\r\n"
    "                                <local:FZTextBlock Text=\"{Binding UserProfile.FormattedGamertag}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Gamertag}\" />\r\n"
    "                                <local:FZTextBlock Text=\"{Binding TitlePostGamertag}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Title}\"/>\r\n"
    "                            </StackPanel>\r\n"
    "                            <Grid Visibility=\"{Binding CanBeActivated, Converter={StaticResource A_FZBooleanToVisibilityConverter}}\">\r\n"
    "                                <Grid Visibility=\"{Binding PlayerHasAccepted, Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\">\r\n"
    "                                    <StackPanel Orientation=\"Horizontal\" \r\n"
    "                                            Visibility=\"{Binding IsInputBlocked, Converter={StaticResource A_FZBooleanToVisibilityConverter_Inverse}}\">\r\n"
    "                                        <local:FZTextBlock Text=\"{Binding TitlePreGamertag}\"\r\n"
    "                                                   Visibility=\"{Binding IsRaceEvent, Converter={StaticResource A_FZBooleanToVisibilityConverter}}\"\r\n"
    "                                                   Style=\"{StaticResource A_Style_FZTextBlock_Notification_Gamertag}\"/>\r\n"
    "                                        <local:FZTextBlock Text=\"{local:Loc Lobby.IDS_Invite_Join_Prefix}\"\r\n"
    "                                                       Style=\"{StaticResource A_Style_FZTextBlock_Notification_BodyText}\"/>\r\n";

static int ui_race_toast_layout(void)
{
    int ok = stateflow_replace(L"\\media\\UI.zip", "Controls/NotificationPopupControl.xaml", "UI/Controls/NotificationPopupControl.xaml", RACE_TOAST_OLD,
                               sizeof RACE_TOAST_OLD - 1, RACE_TOAST_NEW, sizeof RACE_TOAST_NEW - 1, "racetoast layout");
    if (ok) {
        InterlockedExchange(&g_rt_layout, 1);
        log_begin();
        log_raw("racetoast layout: Controls/NotificationPopupControl.xaml handed to the platform, race toasts show the event name in bold at the start of line 2\n");
    }
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
        static DWORD told_err;
        DWORD err = GetLastError();

        lstrcpyA(g_gamedb_hex, "missing");
        g_gamedb_size = g_gamedb_time = 0;
        if (err != told_err) {
            told_err = err;
            log_begin();
            log_raw("gamedb: the game's car database (mediaoverride\\Stripped\\gamedbRC.slt) cannot be found, error ");
            log_dec(err);
            log_raw(": requests say \"missing\" and the server refuses online play\n");
        }
    } else {
        size = (ULONGLONG)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
        wtime = (ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
        if (!g_gamedb_hex[0] || g_gamedb_hex[0] == 'm' || size != g_gamedb_size || wtime != g_gamedb_time) {
            HANDLE f = CreateFileW(g_gamedb_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                   FILE_FLAG_SEQUENTIAL_SCAN, NULL);
            static unsigned char buf[1 << 16];
            sha256_ctx c;
            DWORD got, err;
            int ok = f != INVALID_HANDLE_VALUE;

            sha256_init(&c);
            while (ok && (ok = ReadFile(f, buf, sizeof buf, &got, NULL)) && got) {
                sha256_update(&c, buf, got);
            }
            err = ok ? 0 : GetLastError();
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
            if (!ok) {
                log_raw(" (the file exists but could not be read, error ");
                log_dec(err);
                log_raw(")");
            }
            log_raw("\n");
        }
    }
    for (i = 0; i < 65; i++) {
        out[i] = g_gamedb_hex[i];
    }
    ReleaseSRWLockExclusive(&g_gamedb_lock);
}

#include "restricted_tools.inc"
#include "xhr2.inc"
#include "health.inc"
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

#include "eo_timelock.inc"

/* The plugin as far as this code knows it: each global's use, byte for byte, so another build is left alone */
static unsigned char *eo_timelock_plugin(void)
{
    static const struct { unsigned rva; unsigned char code[13]; unsigned n; } known[] = {
        {0x2d90, {0x31, 0xc0, 0x83, 0x3d, 0x43, 0x79, 0x00, 0x00, 0x00, 0x0f, 0x95, 0xc0, 0xc3}, 13},  /* enabled */
        {0x2dd0, {0x8b, 0x05, 0x0a, 0x79, 0x00, 0x00, 0xc3}, 7},                                       /* generation */
        {0x32b0, {0x8b, 0x05, 0x2e, 0x74, 0x00, 0x00, 0xc3}, 7},                                       /* weather */
        {0x36d8, {0x48, 0x3b, 0x1d, 0x29, 0x70, 0x00, 0x00}, 7},                                       /* held clock */
    };
    static int refused;
    unsigned char *wc;
    unsigned char code[13];
    unsigned i;

    if (refused || !(wc = (unsigned char *)GetModuleHandleW(L"EO_WeatherControl.dll"))) {
        return NULL;
    }
    for (i = 0; i < sizeof known / sizeof known[0]; i++) {
        int readable = read_game_memory(wc + known[i].rva, code, known[i].n);
        if (!readable || memcmp(code, known[i].code, known[i].n)) {
            refused = 1;
            log_begin();
            log_raw("timelock: unexpected EO_WeatherControl.dll (only the one in Extra Options 0.4.4 is known), its time lock is not parked online: at plugin+0x");
            log_hex(known[i].rva, 4);
            if (readable) {
                log_raw(" expected ");
                log_hexbytes(known[i].code, known[i].n);
                log_raw(", found ");
                log_hexbytes(code, known[i].n);
            } else {
                log_raw(" the code cannot be read");
            }
            log_raw("\n");
            return NULL;
        }
    }
    return wc;
}

static void eo_timelock_tick(unsigned char *img)
{
    static struct eo_lock state;
    struct eo_lock before;
    unsigned char *wc = eo_timelock_plugin();
    unsigned char *manager = NULL, *session = NULL, *root = NULL, *players = NULL, *world = NULL, *clock = NULL;
    int did;

    if (!wc) {
        return;
    }
    if (read_game_memory(img + FH2_NET_GAME_MANAGER_RVA, &manager, 8) && manager) {
        read_game_memory(manager + 0xd0, &session, 8);
    }
    if (read_game_memory(img + FH2_WORLD_ROOT_RVA, &root, 8) && root && read_game_memory(root + 0x48, &players, 8) && players &&
        read_game_memory(players + 0x98, &world, 8) && world) {
        read_game_memory(world + 0x480, &clock, 8);
    }
    before = state;
    did = eo_lock_step(&state, wc, session != NULL, clock);
    if (did & EO_LOCK_RESTORED) {
        log_begin();
        log_raw("timelock: session over, EO's settings are back (time lock ");
        log_raw(before.enabled ? "on" : "off");
        log_raw(", weather ");
        log_dec((unsigned long)before.weather);
        log_raw(")\n");
    }
    if (did & EO_LOCK_PARKED) {
        log_begin();
        log_raw("timelock: a session exists, EO's time lock (");
        log_raw(state.enabled ? "on" : "off");
        log_raw(") and weather (");
        log_dec((unsigned long)state.weather);
        log_raw(") are parked in the plugin until it ends\n");
    }
    if (did & EO_LOCK_CLOCK_RELEASED) {
        log_begin();
        log_raw("timelock: the clock EO held at 0x");
        log_hex((unsigned long long)(ULONG_PTR)clock, 12);
        log_raw(" is released, it follows the network clock again\n");
    }
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
    DWORD monitor_start = 0;
    int waited_told = 0;

    (void)unused;
#ifdef FH2MP_DEBUG_BUILD
    weather_dump = GetEnvironmentVariableW(L"FH2MP_WEATHER_DUMP", wd_env, 4) > 0;
#else
    (void)wd_env;
    weather_dump = 0;
#endif
    {
        wchar_t rp[MAX_PATH];

        lstrcpyW(rp, g_dir);
        lstrcatW(rp, L"\\fh2mp-restart.txt");
        DeleteFileW(rp);                     /* a leftover of an earlier run (older shims) is not this run's */
        lstrcpyW(rp, g_dir);
        lstrcatW(rp, L"\\fh2mp-online-setup.txt");
        DeleteFileW(rp);
    }
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
        log_raw("monitor: FH2 image not found after 30 s (the platform reports the game at 0x");
        log_hex((unsigned long long)(ULONG_PTR)(g_api ? g_api->game_base() : NULL), 12);
        log_raw("); the Winsock and request hooks are not kept in place\n");
        return 0;
    }
    opts = img + FH2_OPTIONS_RVA;
    monitor_start = GetTickCount();
    for (tick = 0; ; tick++) { /* for the whole process (2026-11-07): until then it stopped after 24000 ticks = 6.7 h, and with it the EO parking, the gamedb
                                * retries, the import re-hooking and the online-setup watchdog */
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
            } else if (xs_state == 1 && g_xs_calls == 0 && g_xs_hooked_at && GetTickCount() - g_xs_hooked_at > 45000) {
                static int told;
                if (!told) {
                    told = 1;
                    log_begin();
                    log_raw("xstoken: no XSAPI token request 45 s after the hook (the game has not started its online setup)\n");
                }
            }
        }
        {
            /* Online-setup watchdog: diagnosis plus the in-process retry of online_setup.inc (fix 3). It never restarts the game. */
            static int phase;            /* 0 watching, 1 retried (waiting for the request), 2 settled */
            static int retries;
            static DWORD retried_at;
            DWORD first = g_lsp_first_tick, now = GetTickCount();

            if (phase == 1 && g_gsds_seen) {
                phase = 2;
                log_begin();
                log_raw("online-setup: recovered, the cloud configuration sent its request ");
                log_dec(now - retried_at);
                log_raw(" ms after retry ");
                log_dec((unsigned long)retries);
                log_raw(" (no restart needed)\n");
            }
            if (phase < 2 && first && !g_gsds_seen && !GetEnvironmentVariableW(L"FH2MP_OFFLINE", NULL, 0) &&
                ((phase == 0 && now - first > 20000) || (phase == 1 && now - retried_at > 15000))) {
                int state = cloud_config_state(NULL);
                int may_retry = retries < 3 && state == 1 && !GetEnvironmentVariableW(L"FH2MP_NO_CCRETRY", NULL, 0);

                log_begin();
                log_raw(phase == 0 ? "online-setup: the game never started its online setup (no game-server directory request): the first request to the server was "
                                   : "online-setup: still no game-server directory request after the retry; the first request to the server was ");
                log_dec(now - first);
                log_raw(" ms ago; XSAPI token requests ");
                log_dec((unsigned long)g_xs_calls);
                log_raw(", LSP requests ");
                log_dec((unsigned long)g_xhr_seq);
                online_setup_log_state();
                if (may_retry && cloud_config_retry_now()) {
                    retries++;
                    retried_at = now;
                    phase = 1;
                    log_raw("; the cloud configuration is set to the game's own retry state (4, retry now), retry ");
                    log_dec((unsigned long)retries);
                    log_raw("\n");
                } else {
                    wchar_t rp[MAX_PATH];
                    HANDLE rf;
                    DWORD got;
                    static const char why[] = "online setup did not start in this game run: no game-server directory request (cloud configuration), see fh2mp.log\r\n";

                    phase = 2;
                    lstrcpyW(rp, g_dir);
                    lstrcatW(rp, L"\\fh2mp-online-setup.txt");
                    rf = CreateFileW(rp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (rf != INVALID_HANDLE_VALUE) {
                        WriteFile(rf, why, (DWORD)sizeof why - 1, &got, NULL);
                        CloseHandle(rf);
                    }
                    log_raw(state != 1 ? "; not a state this shim can retry, the launcher shows a note (the game is not restarted)\n"
                                       : "; retries used up or switched off, the launcher shows a note (the game is not restarted)\n");
                }
            }
        }
        {
            static int sda_state;
            wchar_t sda_env[8];
            if (sda_state == 0 && GetEnvironmentVariableW(L"FH2MP_NO_SDASTR", sda_env, 8) == 0) {
                sda_state = patch_sda_string();
            }
            if (!waited_told && GetTickCount() - monitor_start > 90000) {
                /* the token, body and secure-address patches all wait for Microsoft.Xbox.Services.dll; none of them says so by itself */
                waited_told = 1;
                if (!find_xsapi_image()) {
                    log_begin();
                    log_raw("xstoken: Microsoft.Xbox.Services.dll is still not in the process 90 s after the monitor started, so the XSAPI token, response body and secure-address patches are not made\n");
                }
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
        eo_timelock_tick(img);
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
                log_raw(" missing torque-curve row read(s) (");
                log_dec(nt_now);
                log_raw(" in all)\n");
                nt_seen = nt_now;
            }
        }
        {
            static unsigned ts_seen_calls, ts_seen_rows;
            unsigned calls = g_torque_scale_calls, rows = g_torque_scale_missing;
            if (calls != ts_seen_calls) {
                log_begin();
                log_raw("nulltorquescale: ");
                log_dec(calls - ts_seen_calls);
                log_raw(" upgrade torque read(s) lacked a rowset, an empty one stood in for ");
                log_dec(rows - ts_seen_rows);
                log_raw(" (");
                log_dec(calls);
                log_raw(" reads in all)\n");
                ts_seen_calls = calls;
                ts_seen_rows = rows;
            }
        }
        {
            static LONG pb_seen;
            LONG pb = g_speed_pb_fault;
            if (pb && pb != pb_seen) {
                pb_seen = pb;
                log_begin();
                log_raw("speedpb: switched itself off, the thread-local slot could not be written (error ");
                log_dec((unsigned long)(pb - 1));
                log_raw("); speed notifications use the online result only\n");
            }
        }
        Sleep(1000);
    }
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
 * crash report; tools/analysis/decode_creates.py reads it). Nothing is sent anywhere. BitReader fields (FUN_141767100 / FUN_141767940): +4 size of the
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
        log_begin();
        log_raw("probe: could not create fh2mp-creates.ring (error ");
        log_dec(GetLastError());
        log_raw("), so a crash report will not show the last ObjectCreate messages\n");
        return 0;
    }
    m = CreateFileMappingW(f, NULL, PAGE_READWRITE, 0, sizeof(probe_ring), NULL);
    if (!m) {
        DWORD err = GetLastError();
        CloseHandle(f);
        log_begin();
        log_raw("probe: could not map fh2mp-creates.ring (error ");
        log_dec(err);
        log_raw(")\n");
        return 0;
    }
    CloseHandle(f);
    g_probe = (probe_ring *)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(probe_ring));
    if (!g_probe) {
        DWORD err = GetLastError();
        CloseHandle(m);
        log_begin();
        log_raw("probe: could not map a view of fh2mp-creates.ring (error ");
        log_dec(err);
        log_raw(")\n");
        return 0;
    }
    CloseHandle(m);
    GetSystemTimeAsFileTime(&ft);
    g_probe->magic = PROBE_MAGIC;
    g_probe->version = 1;
    g_probe->entries = PROBE_ENTRIES;
    g_probe->entry_size = sizeof(probe_entry);
    g_probe->start_tick = GetTickCount();
    g_probe->ft_low = ft.dwLowDateTime;
    g_probe->ft_high = ft.dwHighDateTime;
    if (!plat_hook("probe: ObjectCreate receive", img + FH2_RVA_OBJECT_CREATE_RECV, (void *)probe_object_create, (void **)&g_create_orig)) {
        g_probe = NULL;
        return 0;
    }
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
        IMAGE_NT_HEADERS64 nt;
        IMAGE_SECTION_HEADER sec[96];
        unsigned i, ok = 1;
        img = find_fh2_image();
        if (read_game_headers(img, &nt, sec)) {
            for (i = 0; i < nt.FileHeader.NumberOfSections && ok; i++) {
                if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && sec[i].Misc.VirtualSize &&
                    (sec[i].VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
                     sec[i].Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - sec[i].VirtualAddress ||
                     !page_executable(img + sec[i].VirtualAddress) ||
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
    if (!g_api->wait_for_code(0xFFFFFFFFu)) {
        log_begin();
        log_raw("late: the platform returned no game base from wait_for_code; patches are tried anyway\n");
    }
    wait_for_executable_game();
    {
        wchar_t pr_env[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_PROBE", pr_env, 8) == 0) {
            install_create_probe();
        }
    }
    log_begin();
    log_raw(find_fh2_image() ? "late: game image mapped and executable " : "late: game image not found or not FH2 1.0.0.64 (patches that need it are skipped), ");
    log_dec(GetTickCount() - g_init_tick);
    log_raw(" ms after the shim loaded\n");
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
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_SPEEDPB", env, 8) == 0) patch_speed_pb();
            if (GetEnvironmentVariableW(L"FH2MP_NO_LIGHTING", env, 8) == 0) patch_lighting();
            if (GetEnvironmentVariableW(L"FH2MP_NO_HONK", env, 8) == 0) patch_honk(g_exe_dir, g_dir);
            else honk_switched_off(g_exe_dir, g_dir);
            if (GetEnvironmentVariableW(L"FH2MP_NO_CARSELECT_HOLD", env, 8) == 0) patch_carsel();
        }
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_NETSTRINGS", env, 8) == 0) patch_net_string_receivers();
        }
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_GRIDTRACE", env, 8) == 0) patch_grid_trace();
        }
#ifdef FH2MP_DIAG_BUILD
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_DIAG", env, 8) == 0) patch_diag_trace();
            if (GetEnvironmentVariableW(L"FH2MP_NO_STALLTRACE", env, 8) == 0) patch_stall_trace();
        }
#endif
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_PARTYHUD", env, 8) == 0) patch_party_hud();
            if (GetEnvironmentVariableW(L"FH2MP_NO_RACETOAST", env, 8) == 0) patch_race_toast();
            if (GetEnvironmentVariableW(L"FH2MP_NO_HOOKGUARD", env, 8) == 0) patch_hook_guard();
            if (GetEnvironmentVariableW(L"FH2MP_NO_LOGGUARD", env, 8) == 0) patch_gameplaylog_guard();
            if (GetEnvironmentVariableW(L"FH2MP_NO_PREDGUARD", env, 8) == 0) patch_remote_prediction_guard();
        }
        patch_car_pool();
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_CARWAIT", env, 8) == 0) patch_car_wait();
        }
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
                patch_null_torque_scale();
            }
        }
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_TORQUELOAD", env, 8) == 0) {
                patch_torque_load();
            }
        }
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_PICKERPI", env, 8) == 0) {
                patch_picker_pi();
                if (GetEnvironmentVariableW(L"FH2MP_NO_EVENTSTOCK", env, 8) == 0) {
                    patch_event_stock();
                }
            }
            if (GetEnvironmentVariableW(L"FH2MP_NO_AUTODRIVE_RULE", env, 8) == 0) {
                patch_autodrive_rule();
            }
        }
        {
            wchar_t env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_CINEMATICS", env, 8) == 0) {
                patch_missing_cinematics();
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
            wchar_t mp_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_MATCHPOLL", mp_env, 8) == 0) {
                patch_match_poll();
            }
        }
        {
            wchar_t mt_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_MMTOAST", mt_env, 8) == 0) {
                patch_mm_failed_toast();
            }
        }
        {
            wchar_t mo_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_MMTIMEOUT", mo_env, 8) == 0) {
                patch_mm_timeouts();
            }
        }
        {
            wchar_t qt_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_QOSTIMEOUT", qt_env, 8) == 0) {
                patch_qos_timeout();
            }
        }
        {
            wchar_t sf_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_NO_SIGNINFIX", sf_env, 8) == 0) {
                patch_signin_event();
            }
        }
        patch_webgate_watch();
        {
            wchar_t cp_env[8];
            if (GetEnvironmentVariableW(L"FH2MP_CARPROBE", cp_env, 8) != 0) {
                install_car_probe();
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

#include "session_age.inc"
#include "session_switches.inc"

/*
 * Session handed over by the launcher: fh2mp-session.txt next to the log, "key=value" lines:
 *   xuid=<hex>      the Xbox user id the server assigned to the account (replaces the random per-install id)
 *   token=fh2.xxxx  the session token (the server derives the player's identity from it)
 *   gamedb=<path>   the car database the launcher synced (UTF-8, relative to the Azimuth folder or a Windows path), put in mediaoverride
 *                   (gamedb_place) and hashed there for every request (gamedb_hash)
 * Env FH2MP_TOKEN / FH2MP_XUID override the file (tests, rigs). The file is not deleted when it is used (Azimuth may start the game in a
 * second process that loads this DLL again); the launcher removes it when the game exits. A file older than SESSION_MAX_AGE_TICKS
 * (session_age.inc, 30 minutes; FH2MP_SESSION_MAX_AGE_S in seconds overrides, 0 = no limit) is a leftover of an earlier Play: it is ignored and
 * removed, so a game started without Play is offline. Returns 1 when an xuid was given.
 */
static int session_load(void)
{
    WCHAR path[MAX_PATH], env[100];
    char buf[1536];
    DWORD got = 0, i, n;
    HANDLE f;
    int have_xuid = 0, token_env = 0;
    const char *file_state = "absent";
    DWORD file_age_s = 0, open_err = 0;

    if (GetEnvironmentVariableW(L"FH2MP_TOKEN", env, 100) && env[0]) {
        token_env = 1;
        for (n = 0; env[n] && n < sizeof g_token - 1; n++) {
            g_token[n] = (char)env[n];
        }
        g_token[n] = 0;
    }
    lstrcpyW(path, g_dir);
    lstrcatW(path, L"\\fh2mp-session.txt");
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        open_err = GetLastError();
        if (open_err != ERROR_FILE_NOT_FOUND && open_err != ERROR_PATH_NOT_FOUND) {
            file_state = "could not be opened";
        }
    }
    if (f != INVALID_HANDLE_VALUE) {
        FILETIME written, now_ft;
        ULARGE_INTEGER w, n;
        unsigned long long max_age = SESSION_MAX_AGE_TICKS;
        WCHAR limit[16];
        DWORD len = GetEnvironmentVariableW(L"FH2MP_SESSION_MAX_AGE_S", limit, 16);
        if (len && len < 16) {
            unsigned long long secs = 0;
            DWORD k;
            for (k = 0; limit[k] >= L'0' && limit[k] <= L'9'; k++) {
                secs = secs * 10 + (unsigned)(limit[k] - L'0');
            }
            max_age = secs * 10000000ULL;
        }
        w.QuadPart = 0;
        GetSystemTimeAsFileTime(&now_ft);
        n.LowPart = now_ft.dwLowDateTime, n.HighPart = now_ft.dwHighDateTime;
        if (GetFileTime(f, NULL, NULL, &written)) {
            w.LowPart = written.dwLowDateTime, w.HighPart = written.dwHighDateTime;
        }
        file_state = "used";
        file_age_s = w.QuadPart && n.QuadPart >= w.QuadPart ? (DWORD)((n.QuadPart - w.QuadPart) / 10000000ULL) : 0;
        if (session_is_stale(w.QuadPart, n.QuadPart, max_age)) {
            int removed;
            CloseHandle(f);
            f = INVALID_HANDLE_VALUE;
            removed = DeleteFileW(path);       /* best effort: the launcher also removes leftovers when it starts and quits */
            file_state = "stale";
            log_begin();
            log_raw("session: the launcher's session file is ");
            log_dec((unsigned long)((n.QuadPart - w.QuadPart) / 600000000ULL));
            log_raw(" minutes old (limit ");
            log_dec((unsigned long)(max_age / 600000000ULL));
            log_raw(" minutes), a leftover of an earlier Play: ignored, so this game is offline");
            log_raw(removed ? ", file removed\n" : ", file could not be removed\n");
            if (g_api && g_api->set_notice) {
                g_api->set_notice(g_mod, "FH2MP: not signed in\nPress Play in the FH2MP launcher to play online");
            }
        }
    }
    if (f != INVALID_HANDLE_VALUE) {
        if (!ReadFile(f, buf, sizeof buf - 1, &got, NULL)) {
            file_state = "could not be read";
            open_err = GetLastError();
        } else {
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
                        g_xuid_source = "the launcher's session";
                        have_xuid = 1;
                    }
                } else if (line[0] == 't' && line[1] == 'o' && line[2] == 'k' && line[3] == 'e' && line[4] == 'n' && line[5] == 0 &&
                           !g_token[0]) {
                    for (n = 0; eq[n] && n < sizeof g_token - 1; n++) {
                        g_token[n] = eq[n];
                    }
                    g_token[n] = 0;
                } else if (lstrcmpA(line, "run") == 0 && run_id_ok(eq)) {
                    lstrcpynA(g_run, eq, sizeof g_run);
                } else if (lstrcmpA(line, "switches") == 0) {
                    session_switches(eq);
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
                        log_begin();
                        log_raw(g_api->game_dir[0] ? "session: the game folder path is too long for the car database path; online play will be refused\n"
                                                   : "session: the platform found no game folder (Azimuth's game folder setting); the server's car database cannot be put in place and online play will be refused\n");
                    } else if (MultiByteToWideChar(CP_UTF8, 0, eq, -1, rel, MAX_PATH) &&
                               lstrlenW(g_exe_dir) + lstrlenW(rel) + 2 < MAX_PATH) {
                        lstrcpyW(g_gamedb_src, absolute ? L"" : g_exe_dir);
                        lstrcatW(g_gamedb_src, absolute ? L"" : L"\\");
                        lstrcatW(g_gamedb_src, rel);
                        lstrcpyW(g_gamedb_path, g_api->game_dir);
                        lstrcatW(g_gamedb_path, L"\\mediaoverride\\Stripped\\gamedbRC.slt");
                    } else {
                        log_begin();
                        log_raw("session: the session names a car database path that cannot be used (gamedb= is not valid UTF-8 or is too long); the game runs without the server's database and online play may be refused\n");
                    }
                }
            }
        }
        CloseHandle(f);
    }
    udp_ticket_key();
    if (!g_run[0]) {
        DWORD v = GetTickCount() ^ (GetCurrentProcessId() << 16);
        for (i = 0; i < 8; i++) {
            g_run[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 0xf];
        }
        g_run[8] = 0;
    }
    log_begin();
    log_raw("session: run=");
    log_raw(g_run);
    log_raw(", ");
    log_raw(g_token[0] ? "token present" : "no token");
    log_raw(g_gamedb_path[0] ? ", car database set" : "");
    log_raw(have_xuid ? ", xuid from the launcher" : "");
    log_raw(token_env ? ", token from FH2MP_TOKEN" : "");
    log_raw("; fh2mp-session.txt ");
    log_raw(file_state);
    if (file_state[0] == 'u') {
        log_raw(", written ");
        log_dec(file_age_s);
        log_raw(" s ago");
    } else if (open_err && file_state[0] == 'c') {
        log_raw(" (error ");
        log_dec(open_err);
        log_raw(")");
    }
    log_raw(g_token[0] ? "\n" : ", so this game is offline\n");
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
            g_xuid_source = "the FH2MP_XUID environment variable";
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
            g_xuid_source = "a new random id, saved in fh2mp-identity.txt (no earlier file: a new player profile)";
        } else {
            DWORD err = GetLastError();
            g_xuid_source = "a new random id that could NOT be saved (fh2mp-identity.txt cannot be written), so the next start gets another id";
            log_begin();
            log_raw("identity: could not write fh2mp-identity.txt, error ");
            log_dec(err);
            log_raw("; the id is not kept and the next start is a different player\n");
        }
    } else {
        g_xuid_source = "fh2mp-identity.txt";
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
    log_raw(" from ");
    log_raw(g_xuid_source);
    log_raw(hits ? " (Azimuth's XboxUserId patched)\n" : " (Azimuth's XboxUserId constant NOT found: the game keeps Azimuth's fixed id)\n");
    return (int)hits;
}

#ifndef FH2MOD_PLATFORM_VERSION
#define FH2MOD_PLATFORM_VERSION "2.7.1"
#endif

/* Called by the FH2 Mod Platform on its own thread (outside the loader lock) right after it loads this DLL from fh2-mods. */
#include "title_scope.inc"

/* FH2 was selected but its executable is not the supported build: FH2MP then stays out of the launch (no registration, no files of its own), and this
 * one line is the only trace of why. It goes straight to data\bin\fh2mp.log, which is not open yet. */
#include "online_api.inc"

static void early_refusal_note(const wchar_t *azimuth_dir, const char *why)
{
    wchar_t path[MAX_PATH];
    char line[400];
    HANDLE f;
    DWORD put;
    int n;

    if (!azimuth_dir || lstrlenW(azimuth_dir) >= MAX_PATH - 40) {
        return;
    }
    lstrcpyW(path, azimuth_dir);
    lstrcatW(path, L"\\data\\bin\\fh2mp.log");
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;       /* no log of an FH2MP run here: do not create folders or files in a launch that is not ours */
    }
    n = wsprintfA(line, "[%lu] late: FH2 was selected but FH2MP stays out of this launch (unexpected game executable): %s\r\n", (unsigned long)GetTickCount(), why);
    WriteFile(f, line, (DWORD)n, &put, NULL);
    CloseHandle(f);
}

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi *api)
{
    FH2ModInfo info;
    wchar_t path[MAX_PATH];
    int i;

    if (!api || !api->register_mod) {
        return 1;
    }
    /* This must precede registration, session/identity I/O, host hooks and
     * mediaoverride changes, including the offline-account-save branch. */
    if (!fh2_launch_selected(api->azimuth_dir, GetCommandLineW())) {
        return 0;
    }
    if (!fh2_launch_executable(api->game_dir)) {
        early_refusal_note(api->azimuth_dir, g_fh2_exe_why);
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    info.name = "FH2MP";
    info.version = FH2MP_SHIM_VERSION;
    /* The launcher still replaces this DLL on every Play; the platform's own check is for players who start the game without it (a Steam
     * shortcut to azimuth.exe, 256 of 530 refused IPs in one day of prod logs): it puts "new release" on the title screen and a ! on the MODS tab
     * (at most every 6 hours, from api.github.com, and only for this mod). */
    info.github_repo = "Charelene/FH2MP";
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates = 1;
    info.description = "Forza Horizon 2 online on the FH2MP servers: Free Roam, Road Trip, Rivals, Car Clubs and more. "
                       "Start the game from the FH2MP launcher to play online.";
    g_init_tick = GetTickCount();
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
        api->log(g_mod, "the Azimuth folder path is too long for FH2MP's files; FH2MP does nothing");
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
#ifndef FH2MP_DEBUG_BUILD
    log_keep_load();
#endif
    log_begin();
    log_raw("--- fh2mp shim " FH2MP_SHIM_VERSION " attached (FH2 Mod Platform ");
    log_raw(api->platform_version);
    log_raw("), tid=");
    log_dec(GetCurrentThreadId());
    {   /* the stamps of this log are GetTickCount: this pairs one with the clock the launcher's and the server's logs use */
        char utc[32];
        grid_utc(utc);
        log_raw(", ");
        log_raw(utc);
    }
    log_raw("\n");

    /* Solo play keeps the same XboxUserId/save, without session loading, network hooks or worker threads. */
    {
        wchar_t offline[8];
        if (GetEnvironmentVariableW(L"FH2MP_OFFLINE", offline, 8) != 0) {
            identity_load();
            patch_identity();
            gamedb_put_back();    /* restore the player's database before the platform builds its offline overrides */
            log_begin();
            log_raw("offline: FH2MP_OFFLINE is set, account save profile active; online hooks, session and network patches skipped\n");
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
    online_api_start();
    if (!g_gamedb_src[0]) {
        gamedb_put_back();    /* offline: now, before the platform's database step, which then sees what was put back */
    }
    install_cmdline_hook();
    {
        wchar_t tmp[8];
        if (GetEnvironmentVariableW(L"FH2MP_NO_RIVALSLEAVE", tmp, 8) == 0) {
            stateflow_rivals_leave_session();    /* now: the game reads its stateflow once, early */
        }
        if (GetEnvironmentVariableW(L"FH2MP_BUCKETCARS", tmp, 8) != 0) {    /* opt-in: retail online Free Roam parks no bucket-list cars */
            stateflow_bucket_cars_online();
        }
        if (GetEnvironmentVariableW(L"FH2MP_NO_RACETOAST", tmp, 8) == 0) {
            ui_race_toast_layout();    /* now: the game reads its UI files when the first screen loads */
        }
    }
    {
        wchar_t cp_env[8];
        wchar_t sm_env[8];
        (void)cp_env;
        if (GetEnvironmentVariableW(L"FH2MP_SAMPLE", sm_env, 8) != 0) {
            CloseHandle(CreateThread(NULL, 0, stall_sampler, NULL, 0, NULL));
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
        if (g_token[0]) {
            health_start();
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
