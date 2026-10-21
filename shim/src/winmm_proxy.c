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
/* Session token of the player signed in through the launcher (see session_load); sent to the stand-in services as X-FH2MP-Token. */
/*
 * Pages are made writable for a patch and then simply left that way. Restoring the old protection (read-only code) raced with Azimuth,
 * which applies its own game patches on other threads at the same moment when no other winmm proxy delays the game: whichever side
 * restored first made the other one's write fault (0xc0000005 inside memcpy). An extra writable page is harmless; the crash is not.
 */
#define patch_done(addr, len, old) ((void)(addr), (void)(len), (void)(old))

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
                                       "refus", "crash", "xsbody", "bindfix", 0};
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
    patch_done(fn + 3, 1, old);
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
    patch_done(fn, 5, old);
    FlushInstructionCache(GetCurrentProcess(), fn, 5);
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
    DWORD old;
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
    at += 6;
    if (!VirtualProtect(at, 6, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    mem_copy(at, nop6, 6);
    patch_done(at, 6, old);
    FlushInstructionCache(GetCurrentProcess(), at, 6);
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
    unsigned char *img = find_fh2_image();
    unsigned char *a, *b;
    DWORD old;
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
    if (!VirtualProtect(a + 3, 2, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    a[3] = 0x90;
    a[4] = 0x90;
    patch_done(a + 3, 2, old);
    if (!VirtualProtect(b + 6, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    b[6] = 0x01;
    patch_done(b + 6, 1, old);
    FlushInstructionCache(GetCurrentProcess(), a, 8);
    FlushInstructionCache(GetCurrentProcess(), b, 7);
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
static void xs_write_jump(unsigned char *fn, void *target);
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
    xs_write_jump(img + FH2_SECURE_RESOLVE_RVA, (void *)secure_resolve_stub);
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
    xs_write_jump(img + FH2_SETSESSION_RVA, (void *)set_session_stub);
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
    DWORD old;
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
    }
    if (!VirtualProtect(at, 4, PAGE_READWRITE, &old)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        at[i] = (unsigned char)env[i];
    }
    patch_done(at, 4, old);
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
    DWORD old;

    if (!img) {
        return 0;
    }
    at = img + FH2_MPDIFF_INSN_RVA;
    if (memcmp(at, now, sizeof now) == 0) {
        return 1;
    }
    if (memcmp(at, want, sizeof want) != 0 || !VirtualProtect(at, sizeof want, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("mpdiff: unexpected bytes in the assists bonus code, not patched\n");
        return 0;
    }
    memcpy(at, now, sizeof now);
    patch_done(at, sizeof want, old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof now);
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
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    DWORD old;

    if (!img) {
        return 0;
    }
    at = img + FH2_TRAFFIC_REQOWN_RVA;
    if (at[0] == 0x90 && at[1] == 0x90) {
        return 1;
    }
    if (memcmp(at - sizeof cmp_ctx, cmp_ctx, sizeof cmp_ctx) != 0 || at[0] != 0x74 || at[1] != 0x7D ||
        !VirtualProtect(at, 2, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("trafficown: unexpected bytes in the RequestOwnership trigger, not patched\n");
        return 0;
    }
    at[0] = 0x90;
    at[1] = 0x90;
    patch_done(at, 2, old);
    FlushInstructionCache(GetCurrentProcess(), at, 2);
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
    DWORD old;

    if (!img) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        unsigned char *c = img + cmp_rva[i];
        unsigned char *jne = c + 7;
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
        if (!VirtualProtect(jne, 6, PAGE_EXECUTE_READWRITE, &old)) {
            continue;
        }
        jne[0] = 0xE9;
        mem_copy(jne + 1, &rel, 4);
        jne[5] = 0x90;
        patch_done(jne, 6, old);
        FlushInstructionCache(GetCurrentProcess(), jne, 6);
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
    unsigned char *img = find_fh2_image();
    unsigned char *p;
    DWORD old;
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
    if (!VirtualProtect(p + 7, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    p[7] = 0xEB;
    patch_done(p + 7, 1, old);
    FlushInstructionCache(GetCurrentProcess(), p + 7, 1);
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
    unsigned char *img = find_fh2_image();
    unsigned char *p;
    DWORD old;
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
    if (!VirtualProtect(p + 2, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    p[2] = 0xEB;
    patch_done(p + 2, 1, old);
    FlushInstructionCache(GetCurrentProcess(), p + 2, 1);
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
 * it was. Entry of the function, 7 bytes replaced by a jump to a stub near the image (the stub does the check and then runs the replaced
 * instructions):  48 8B C4 (mov rax,rsp)  48 89 50 10 (mov [rax+0x10],rdx).  Disable with env FH2MP_NO_NULLTORQUE.
 */
static volatile unsigned *g_nulltorque_hits;      /* counts the engines the guard skipped (read by the monitor thread, which logs changes) */

static int patch_null_torque_curve(void)
{
    static const unsigned char want[10] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x55, 0x41, 0x56};
    unsigned char *img = find_fh2_image();
    unsigned char *site, *probe, *stub = NULL, *lo, *hi;
    long long disp;
    DWORD old;
    int i, d32;
    unsigned char code[32];

    if (!img) {
        return 0;
    }
    site = img + 0x10643b0u;
    if (site[0] == 0xe9) {
        return 1;                 /* already patched */
    }
    for (i = 0; i < 10; i++) {
        if (site[i] != want[i]) {
            log_begin();
            log_raw("nulltorque: unexpected bytes at the torque curve reader, not patched\n");
            return 0;
        }
    }
    lo = site > (unsigned char *)0x70000000 ? site - 0x70000000 : (unsigned char *)0x10000;
    hi = site + 0x70000000;
    for (probe = (unsigned char *)((ULONG_PTR)(img + FH2_SIZE_OF_IMAGE + 0xffff) & ~(ULONG_PTR)0xffff); probe < hi; probe += 0x10000) {
        stub = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (stub) {
            break;
        }
    }
    if (!stub) {
        for (probe = (unsigned char *)((ULONG_PTR)(img - 0x10000) & ~(ULONG_PTR)0xffff); probe > lo; probe -= 0x10000) {
            stub = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (stub) {
                break;
            }
        }
    }
    if (!stub) {
        log_begin();
        log_raw("nulltorque: could not allocate a page near the game image, not patched\n");
        return 0;
    }
    /* stub: test rdx,rdx ; jz hit ; mov rax,[rdx] ; test rax,rax ; jz hit ; <replaced instructions> ; jmp site+7 ; hit: inc [counter] ; ret */
    code[0] = 0x48; code[1] = 0x85; code[2] = 0xd2;                       /* test rdx,rdx */
    code[3] = 0x74; code[4] = 0x14;                                       /* jz ret (+20) */
    code[5] = 0x48; code[6] = 0x8b; code[7] = 0x02;                       /* mov rax,[rdx] */
    code[8] = 0x48; code[9] = 0x85; code[10] = 0xc0;                      /* test rax,rax */
    code[11] = 0x74; code[12] = 0x0c;                                     /* jz ret (+12) */
    code[13] = 0x48; code[14] = 0x8b; code[15] = 0xc4;                    /* mov rax,rsp */
    code[16] = 0x48; code[17] = 0x89; code[18] = 0x50; code[19] = 0x10;   /* mov [rax+0x10],rdx */
    code[20] = 0xe9;                                                      /* jmp site+7 */
    code[25] = 0xff; code[26] = 0x05;                                     /* hit: inc dword ptr [rip+counter] */
    code[31] = 0xc3;                                                      /* ret */
    disp = (long long)((site + 7) - (stub + 25));
    if (disp > 0x7fffffffll || disp < -0x80000000ll) {
        log_begin();
        log_raw("nulltorque: stub out of rel32 range, not patched\n");
        return 0;
    }
    d32 = (int)disp;
    mem_copy(code + 21, &d32, 4);
    d32 = 64 - (25 + 6);                                                  /* the counter lives at stub+64, the inc instruction ends at stub+31 */
    mem_copy(code + 27, &d32, 4);
    mem_copy(stub, code, 32);
    *(unsigned *)(stub + 64) = 0;
    g_nulltorque_hits = (volatile unsigned *)(stub + 64);
    FlushInstructionCache(GetCurrentProcess(), stub, 68);
    disp = (long long)(stub - (site + 5));
    if (disp > 0x7fffffffll || disp < -0x80000000ll || !VirtualProtect(site, 7, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("nulltorque: entry out of rel32 range, not patched\n");
        return 0;
    }
    d32 = (int)disp;
    site[0] = 0xe9;
    mem_copy(site + 1, &d32, 4);
    site[5] = 0x90;
    site[6] = 0x90;
    patch_done(site, 7, old);
    FlushInstructionCache(GetCurrentProcess(), site, 7);
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
    unsigned char *site, *probe, *slot = NULL, *lo, *hi;
    long long disp;
    wchar_t env[8];
    DWORD old;
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
    lo = site > (unsigned char *)0x70000000 ? site - 0x70000000 : (unsigned char *)0x10000;
    hi = site + 0x70000000;
    for (probe = (unsigned char *)((ULONG_PTR)(img + FH2_SIZE_OF_IMAGE + 0xffff) & ~(ULONG_PTR)0xffff); probe < hi; probe += 0x10000) {
        slot = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (slot) {
            break;
        }
    }
    if (!slot) {
        for (probe = (unsigned char *)((ULONG_PTR)(img - 0x10000) & ~(ULONG_PTR)0xffff); probe > lo; probe -= 0x10000) {
            slot = VirtualAlloc(probe, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (slot) {
                break;
            }
        }
    }
    if (!slot) {
        log_begin();
        log_raw("placement: could not allocate a page near the game image, not patched\n");
        return 0;
    }
    *(void **)slot = (void *)placement_hook;
    disp = (long long)(slot - (site + 3 + 6));
    if (disp > 0x7fffffffll || disp < -0x80000000ll || !VirtualProtect(site + 3, 8, PAGE_EXECUTE_READWRITE, &old)) {
        log_begin();
        log_raw("placement: slot out of rel32 range, not patched\n");
        return 0;
    }
    site[3] = 0xff;                       /* call qword ptr [rip+disp32] */
    site[4] = 0x15;
    {
        int d32 = (int)disp;
        mem_copy(site + 5, &d32, 4);
    }
    site[9] = 0x90;                       /* the original call was 7 bytes: 41 FF 92 00 02 00 00 */
    site[10] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), site, 11);
    log_begin();
    log_raw(g_place_force ? "placement: textures Azimuth cannot place are created as ordinary textures (FORCED for depth-stencil, test mode)\n"
                          : "placement: textures Azimuth cannot place are created as ordinary textures instead of ending the game\n");
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
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    DWORD old;
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
    if (!VirtualProtect(at + 7, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    at[7] = 0xEB;
    patch_done(at + 7, 1, old);
    FlushInstructionCache(GetCurrentProcess(), at + 7, 1);
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
    unsigned char *img = find_fh2_image();
    unsigned char *at;
    DWORD old;
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
    if (!VirtualProtect(at + 2, 2, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    at[2] = 0x90;
    at[3] = 0x90;
    patch_done(at + 2, 2, old);
    FlushInstructionCache(GetCurrentProcess(), at, 4);
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

static void xs_write_jump(unsigned char *fn, void *target)
{
    DWORD old;
    unsigned char j[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0}; /* mov rax,imm64 ; jmp rax */
    ULONGLONG t = (ULONGLONG)(ULONG_PTR)target;
    int i;
    for (i = 0; i < 8; i++) {
        j[2 + i] = (unsigned char)(t >> (8 * i));
    }
    VirtualProtect(fn, 12, PAGE_EXECUTE_READWRITE, &old);
    mem_copy(fn, j, 12);
    patch_done(fn, 12, old);
    FlushInstructionCache(GetCurrentProcess(), fn, 12);
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
    xs_write_jump(f1, (void *)xs_token_stub);
    xs_write_jump(f2, (void *)xs_token_stub);
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
    xs_write_jump(fn, (void *)sda_string_stub);
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
    patch_done(fn, 14, old);
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
    patch_done(target, sizeof stub, old);
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
        patch_done(lea, 8, old);
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
 * Env FH2MP_TOKEN / FH2MP_XUID override the file (tests, rigs). The file is not deleted here (Azimuth may start the game in a second
 * process that loads this DLL again); the launcher removes it when the game exits. Returns 1 when an xuid was given.
 */
static int session_load(void)
{
    WCHAR path[MAX_PATH], env[100];
    char buf[512];
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
                }
            }
        }
        CloseHandle(f);
    }
    log_begin();
    log_raw("session: ");
    log_raw(g_token[0] ? "token present" : "no token");
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
            if (p[j] == 0x49 && p[j + 1] == 0xB8 && mem_equal(p + j, pat, sizeof pat)) {
                DWORD old;
                if (VirtualProtect(p + j + 2, 8, PAGE_EXECUTE_READWRITE, &old)) {
                    mem_copy(p + j + 2, &g_xuid, 8);
                    patch_done(p + j + 2, 8, old);
                    FlushInstructionCache(GetCurrentProcess(), p + j, 10);
                    hits++;
                }
            }
        }
    }
    log_begin();
    log_raw("identity: xuid 0x");
    log_hex(g_xuid, 16);
    log_raw(hits ? " (Azimuth's XboxUserId patched)\n" : " (Azimuth's XboxUserId constant NOT found)\n");
    return (int)hits;
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
    log_raw("--- fh2mp winmm shim " FH2MP_SHIM_VERSION " attached, tid=");
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
    {
        wchar_t tmp[8];
        if (!session_load()) {
            identity_load();
        }
        if (GetEnvironmentVariableW(L"FH2MP_NO_IDENTITY", tmp, 8) == 0) {
            patch_identity();
        }
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
