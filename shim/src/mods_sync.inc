/* Add-on mods online (user request 2026-11-12): NitroSystem (qiangqiang101/FH2-NitroSystem, fh2-mods\FnFNitro.dll) and FH2-Underglow
 * (qiangqiang101/FH2-Underglow, fh2-mods\Underglow.dll). Both are MIT licensed and are not changed, on disk or in memory.
 *
 * NOS rule (user decision): NitroSystem's NOS is on in online Free Roam lobbies only, off in events, Rivals, Road Trip and everywhere else online.
 * NitroSystem 2.0.0 publishes its off switch as the platform interface "NitroSystem.Control" v1 (its docs/MOD_CONTROL.md, include/
 * NitroSystemControl.h: set_blocked(who, 0|1), is_blocked, is_active, nitro_percent; also exported by FnFNitro.dll as FnFNitro_SetBlocked ...).
 * When this game plays online the shim blocks NOS under the name "FH2MP" as soon as it finds the switch, and lifts the block only while all of
 * these hold, checked every 100 ms on the game's logic thread (the frame step game+0xFA78A0, nos_flames.inc): the game is in a network session
 * (picker_pi.inc ppi_in_session), its event context says free roam and no Road Trip is on (the tests of autodrive_rule.inc), and the game server's
 * reply on the mod state channel (mods_wire.h), at most 1.5 s old, says this player is in a Free Roam world and not in its event. Anything that
 * cannot be read, a server without the channel, or no reply: NOS stays off. NitroSystem sets offline_only, so FH2 Mod Platform 2.8.0 does not load
 * it online unless fh2-mods\platform-online.txt says so (the launcher writes FnFNitro.dll<TAB>1 there for a NitroSystem that has the switch, and
 * 0 for one without it: launcher/fh2mp_launcher/platform_online.py). A NitroSystem without the switch that is loaded anyway is reported (flag
 * MODS_NOS_NOSWITCH) and said once in the log.
 *
 * Visible to others: a worker thread polls the game server's mod state channel (its own magic FH2MPMD1, native/internal/game/modsync.go) with this
 * player's NOS (burning or not) and underglow (what Underglow's own `status` command says, read through the platform's command pipe once a second,
 * ug_status.h), and keeps the other players' states for nos_flames.inc (their exhaust flames) and ug_remote.inc (their neon). The network thread
 * never touches a car. Needs the lighting hooks (server address, ticket epoch, the car id and the remote car table are theirs).
 * FH2MP_NO_MODSYNC=1 turns the sharing off (the NOS rule stays). */
#include "mods_wire.h"
#include "ug_status.h"

#define NOS_WHO "FH2MP"
#define NOS_RULE_MS 100u
#define NOS_SERVER_FRESH_MS 1500u
#define MODS_FRESH_MS 600u          /* the other players' states are shown while the newest reply is this recent */
#define MODS_POLL_BUSY_MS 50u       /* while any NOS burns (ours or another's) */
#define MODS_POLL_IDLE_MS 200u
#define MODS_CHANGE_MS 35u          /* a change goes out at once, but not faster than this */
#define UG_READ_MS 1000u
#define UG_RETRY_MS 10000u          /* after a failed read */
#ifndef UG_LOADED
#define UG_LOADED() (GetModuleHandleW(L"Underglow.dll") != NULL)
#endif

/* NitroSystemControl v1 (NitroSystem's include/NitroSystemControl.h) */
typedef struct {
    unsigned size;
    void (*set_blocked)(const char *who, int blocked);
    int (*is_blocked)(void);
    int (*is_active)(void);
    int (*nitro_percent)(void);
} nos_control_v1;

enum { NOS_UNKNOWN, NOS_SWITCH, NOS_NO_SWITCH, NOS_ABSENT };
static void (*g_nos_set)(const char *, int);
static int (*g_nos_active)(void);
static volatile LONG g_nos_kind;          /* NOS_*: what was found once every mod had started */
static volatile LONG g_nos_allowed;       /* 1 while the shim lets NOS burn (its block is off) */
static volatile LONG g_nos_server_ok;     /* the newest reply's MODS_SERVER_NOS_OK */
static volatile LONG64 g_nos_server_at;   /* when that reply came (GetTickCount64), 0 none */
static unsigned char *g_mods_img;
static volatile LONG g_mods_enabled;      /* the worker runs */
static volatile LONG g_mods_share;        /* this player's underglow is read and sent (not FH2MP_NO_MODSYNC) */

static SRWLOCK g_mods_lock = SRWLOCK_INIT;
static struct { mods_record records[MODS_MAX]; unsigned count; ULONGLONG response_at; } g_mods;

/* ---- NitroSystem ---- */
static void nos_find(void)
{
    const nos_control_v1 *nos = NULL;
    const char *version = g_api->mod_version ? g_api->mod_version("NitroSystem") : NULL;
    HMODULE m;
    if (g_api->get_interface) nos = (const nos_control_v1 *)g_api->get_interface("NitroSystem.Control", 1);
    if (nos && nos->size >= sizeof *nos && nos->set_blocked && nos->is_active) {
        g_nos_set = nos->set_blocked;
        g_nos_active = nos->is_active;
    } else if ((m = GetModuleHandleW(L"FnFNitro.dll")) != NULL) {
        g_nos_set = (void (*)(const char *, int))(void *)GetProcAddress(m, "FnFNitro_SetBlocked");
        g_nos_active = (int (*)(void))(void *)GetProcAddress(m, "FnFNitro_IsActive");
        if (!g_nos_set || !g_nos_active) g_nos_set = NULL, g_nos_active = NULL;
    }
    log_begin();
    if (g_nos_set) {
        g_nos_set(NOS_WHO, 1);
        InterlockedExchange(&g_nos_allowed, 0);
        InterlockedExchange(&g_nos_kind, NOS_SWITCH);
        log_raw("nitro: NitroSystem ");
        log_raw(version ? version : "(version unknown)");
        log_raw(" found with its off switch: NOS is off online, and on only while this game is in an online Free Roam lobby\n");
    } else if (GetModuleHandleW(L"FnFNitro.dll")) {
        InterlockedExchange(&g_nos_kind, NOS_NO_SWITCH);
        log_raw("nitro: NitroSystem ");
        log_raw(version ? version : "(version unknown)");
        log_raw(" is loaded online and has no off switch (NitroSystem.Control, 2.0.0 and newer): its NOS cannot be kept off in events; the launcher keeps "
                "such a NitroSystem off for online play\n");
    } else {
        InterlockedExchange(&g_nos_kind, NOS_ABSENT);
        log_raw("nitro: NitroSystem is not loaded (not installed, or off for online play on the MODS tab)\n");
    }
}

/* The rule, on the game's logic thread (nos_flames.inc's frame hook). */
static void nos_rule_tick(void)
{
    static ULONGLONG last;
    static LONG transitions;
    ULONGLONG now = GetTickCount64(), server_at;
    int in_session, context = -1, road_trip = -1, server, allowed;
    if (g_nos_kind != NOS_SWITCH || now - last < NOS_RULE_MS) return;
    last = now;
    in_session = ppi_in_session(g_mods_img);
    if (in_session) {
        context = ad_context_type(g_mods_img);
        road_trip = ad_road_trip(g_mods_img);
    }
    server_at = (ULONGLONG)g_nos_server_at;
    server = g_nos_server_ok && server_at && now - server_at <= NOS_SERVER_FRESH_MS;
    allowed = in_session && context == AD_FREE_ROAM && road_trip == 0 && server;
    if (allowed == g_nos_allowed) return;
    g_nos_set(NOS_WHO, !allowed);
    InterlockedExchange(&g_nos_allowed, allowed);
    {
        LONG n = InterlockedIncrement(&transitions);
        if (n > 20 && n % 50) return;
        log_begin();
        log_raw(allowed ? "nitro: NOS on (online Free Roam" : "nitro: NOS off (");
        if (!allowed) {
            log_raw(!in_session ? "not in a network session" : context != AD_FREE_ROAM ? "the event context is not free roam" :
                    road_trip != 0 ? "a Road Trip is on, or unreadable" : "the server does not say Free Roam");
        }
        log_raw("; event context ");
        if (context < 0) log_raw("unreadable"); else log_dec((unsigned long)context);
        log_raw(", Road Trip ");
        log_raw(road_trip < 0 ? "unreadable" : road_trip ? "on" : "off");
        log_raw(", server ");
        log_raw(!server_at ? "has not answered" : server ? "Free Roam" : now - server_at > NOS_SERVER_FRESH_MS ? "silent" : "event, Road Trip or Car Meet");
        log_raw(") (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
}

static unsigned char nos_flags(void)
{
    LONG kind = g_nos_kind;
    if (kind == NOS_NO_SWITCH) return MODS_NOS_NOSWITCH;
    if (kind != NOS_SWITCH) return 0;
    return (unsigned char)(MODS_NOS_SWITCH | (g_nos_allowed ? MODS_NOS_ALLOWED : 0) | (g_nos_active() ? MODS_NOS_BURNING : 0));
}

/* ---- Underglow: the platform's command pipe ---- */
static wchar_t g_ug_ini[MAX_PATH + 40];

static void ug_load_customs(ug_customs *cc)
{
    unsigned k;
    for (k = 0; k < UG_CUSTOMS; k++) {
        wchar_t key[16], w[96];
        char text[96];
        ug_copy3(cc->rgb[k], ug_custom_defaults[k]);
        cc->name[k][0] = 0;
        if (!g_ug_ini[0]) continue;
        wsprintfW(key, L"Color%u", k + 1);
        GetPrivateProfileStringW(L"CustomColors", key, L"", w, 96, g_ug_ini);
        if (!w[0] && k == 0) GetPrivateProfileStringW(L"Settings", L"CustomColor", L"", w, 96, g_ug_ini);
        if (w[0] && WideCharToMultiByte(CP_UTF8, 0, w, -1, text, sizeof text, NULL, NULL) > 0) {
            float rgb[3];
            if (ug_parse_color(text, rgb)) ug_copy3(cc->rgb[k], rgb);
        }
        wsprintfW(key, L"Name%u", k + 1);
        GetPrivateProfileStringW(L"CustomColors", key, L"", w, 96, g_ug_ini);
        if (!w[0] || WideCharToMultiByte(CP_UTF8, 0, w, -1, cc->name[k], sizeof cc->name[k], NULL, NULL) <= 0) cc->name[k][0] = 0;
    }
}

/* Underglow's `status` -> the wire state; 1 read (lit or dark), 0 not read (said once per reason). */
static int ug_read(unsigned char ug[MODS_UG])
{
    static const char request[] = "Underglow status";
    static char told[200];
    static int found_told;
    wchar_t pipe[48];
    char reply[4096];
    DWORD got = 0;
    const char *why = NULL, *start;
    ug_customs cc;
    for (got = 0; got < MODS_UG; got++) ug[got] = 0;   /* not read: dark, until a read says otherwise */
    got = 0;
    if (!UG_LOADED()) return 0;
    if (!found_told) {
        const char *version = g_api->mod_version ? g_api->mod_version("Underglow") : NULL;
        found_told = 1;
        log_begin();
        log_raw("underglow: Underglow ");
        log_raw(version ? version : "(version unknown)");
        log_raw(" found: its lights on this car are shown to the other players (read from its status command)\n");
    }
    wsprintfW(pipe, L"\\\\.\\pipe\\fh2mod-%lu", GetCurrentProcessId());
    if (!CallNamedPipeW(pipe, (void *)request, sizeof request - 1, reply, sizeof reply - 1, &got, 300)) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY || err == ERROR_SEM_TIMEOUT) return 1;   /* another command at this moment: dark until the next read, in 1 s */
        why = err == ERROR_FILE_NOT_FOUND ? "the platform's command pipe is not there" : "the platform's command pipe did not answer";
    } else {
        reply[got] = 0;
        start = ug_starts(reply, "neon o") ? reply : ug_find(reply, "\nneon o");
        if (start && start != reply) start++;
        ug_load_customs(&cc);
        if (ug_status_to_wire(start, &cc, ug, &why)) {
            if (told[0]) { told[0] = 0; log_begin(); log_raw("underglow: the status command is read again, the lights are shared\n"); }
            return 1;
        }
        if (ug_starts(reply, "error: ")) why = "the status command answered with an error";
    }
    if (lstrcmpA(told, why)) {
        lstrcpynA(told, why, sizeof told);
        log_begin();
        log_raw("underglow: the lights on this car are not shared, ");
        log_raw(why);
        log_raw(" (expected Underglow 1.1.0's status lines); retried every 10 s\n");
    }
    return 0;
}

/* ---- the channel ---- */
static void mods_store(const mods_record *records, unsigned count, unsigned char server, ULONGLONG now)
{
    unsigned k;
    AcquireSRWLockExclusive(&g_mods_lock);
    for (k = 0; k < count; k++) g_mods.records[k] = records[k];
    g_mods.count = count;
    g_mods.response_at = now;
    ReleaseSRWLockExclusive(&g_mods_lock);
    InterlockedExchange(&g_nos_server_ok, (server & MODS_SERVER_NOS_OK) != 0);
    InterlockedExchange64(&g_nos_server_at, (LONG64)now);
}

/* The other players' states now (renderers); 0 when the newest reply is too old to show. */
static unsigned mods_snapshot(mods_record out[MODS_MAX], ULONGLONG now)
{
    unsigned k, n = 0;
    AcquireSRWLockShared(&g_mods_lock);
    if (g_mods.response_at && now - g_mods.response_at <= MODS_FRESH_MS) {
        n = g_mods.count;
        for (k = 0; k < n; k++) out[k] = g_mods.records[k];
    }
    ReleaseSRWLockShared(&g_mods_lock);
    return n;
}

/* The remote car of a record (lighting_sync's table, filled on the game thread), NULL when it is not mapped or its owner's predictions stopped
 * arriving max_ms ago. */
static void *mods_remote_car(unsigned owner, unsigned car, ULONGLONG now, ULONGLONG max_ms)
{
    void *found = NULL;
    unsigned i;
    AcquireSRWLockShared(&g_light_lock);
    for (i = 0; i < 32; i++)
        if (g_light_cars[i].car && g_light_cars[i].owner == owner && g_light_cars[i].id == car && now - g_light_cars[i].seen <= max_ms) {
            found = g_light_cars[i].car;
            break;
        }
    ReleaseSRWLockShared(&g_light_lock);
    return found;
}

static DWORD WINAPI mods_worker(void *unused)
{
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    SOCKET s = INVALID_SOCKET;
    ULONGLONG seq = 0, received = 0, generation = 0, last_send = 0, polls = 0, ug_next = 0;
    SOCKET (WSAAPI *make_socket)(int, int, int) = NULL;
    int (WSAAPI *nonblocking)(SOCKET, long, u_long *) = NULL;
    int (WSAAPI *receive)(SOCKET, char *, int, int, struct sockaddr *, int *) = NULL;
    int (WSAAPI *option)(SOCKET, int, int, const char *, int) = NULL;
    int (WSAAPI *close_socket)(SOCKET) = NULL;
    u_long nb = 1;
    int v6only = 0, socket_told = 0, silent_told = 0;
    unsigned rejected = 0;
    unsigned char ug[MODS_UG], sent_ug[MODS_UG], sent_nos = 0xff;
    unsigned u;
    (void)unused;
    for (u = 0; u < MODS_UG; u++) ug[u] = sent_ug[u] = 0;
    g_api->wait_for_mods(30000);
    nos_find();
    if (ws) {
        make_socket = (void *)GetProcAddress(ws, "socket"); nonblocking = (void *)GetProcAddress(ws, "ioctlsocket");
        receive = (void *)GetProcAddress(ws, "recvfrom"); option = (void *)GetProcAddress(ws, "setsockopt");
        close_socket = (void *)GetProcAddress(ws, "closesocket");
    }
    if (!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin(); log_raw("mods: the mod state channel is off, ws2_32.dll could not be used (error "); log_dec(GetLastError());
        log_raw("); NitroSystem's NOS stays off online\n");
        return 0;
    }
    while (g_mods_enabled) {
        unsigned char address[28], epoch[16], packet[MODS_LIMIT], car, nos, server = 0;
        mods_record records[MODS_MAX];
        int length, i, n, drained, changed, busy;
        unsigned count = 0;
        ULONGLONG predicted, current_generation, now = GetTickCount64(), gap;
        if (g_mods_share && now >= ug_next) ug_next = now + (ug_read(ug) || !UG_LOADED() ? UG_READ_MS : UG_RETRY_MS);
        nos = nos_flags();
        AcquireSRWLockShared(&g_light_lock);
        length = g_light.address_len; predicted = g_light.prediction_at; car = g_light.car; current_generation = g_light.generation;
        light_copy(address, g_light.address, (unsigned)length);
        for (i = 0; i < 16; i++) epoch[i] = g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if (generation != current_generation) {
            if (s != INVALID_SOCKET) close_socket(s);
            s = INVALID_SOCKET; received = 0; generation = current_generation; polls = 0; rejected = 0; silent_told = 0; sent_nos = 0xff;
            AcquireSRWLockExclusive(&g_mods_lock);
            g_mods.count = 0; g_mods.response_at = 0;
            ReleaseSRWLockExclusive(&g_mods_lock);
            InterlockedExchange(&g_nos_server_ok, 0);
        }
        if (!length || !predicted || now - predicted > 1000 || !g_ticket_ready || !g_r_wsasendto) { Sleep(100); continue; }
        if (s == INVALID_SOCKET) {
            s = make_socket(*(const unsigned short *)address, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                if (!socket_told) { socket_told = 1; log_begin(); log_raw("mods: could not open the poll socket, error "); log_dec(GetLastError()); log_raw(", retrying every 0.5 s\n"); }
                Sleep(500); continue;
            }
            if (*(const unsigned short *)address == AF_INET6) option(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only);
            if (nonblocking(s, FIONBIO, &nb)) {
                if (!socket_told) { socket_told = 1; log_begin(); log_raw("mods: could not make the poll socket non-blocking, error "); log_dec(GetLastError()); log_raw(", retrying every 0.5 s\n"); }
                close_socket(s); s = INVALID_SOCKET; Sleep(500); continue;
            }
            socket_told = 0;
        }
        for (drained = 0; drained < 32 && (n = receive(s, (char *)packet, sizeof packet, 0, NULL, NULL)) > 0; drained++) {
            if (mods_response(packet, (unsigned)n, g_ticket_key, g_xuid, seq, received, epoch, &server, records, &count)) {
                int first = !received;
                ULONGLONG at;
                AcquireSRWLockShared(&g_light_lock);
                at = g_light.generation;
                ReleaseSRWLockShared(&g_light_lock);
                if (at == generation) {
                    mods_store(records, count, server, GetTickCount64());
                    received = light_u64(packet + 28);
                }
                if (first && received) {
                    log_begin(); log_raw("mods: authenticated mod state received after "); log_dec((unsigned long)polls);
                    log_raw(server & MODS_SERVER_NOS_OK ? " polls (the server allows NOS here)\n" : " polls (the server keeps NOS off here)\n");
                }
            } else rejected++;
        }
        changed = nos != sent_nos;
        for (i = 0; i < (int)MODS_UG && !changed; i++) changed = ug[i] != sent_ug[i];
        busy = (nos & MODS_NOS_BURNING) != 0;
        if (!busy) {
            mods_record others[MODS_MAX];
            unsigned k, m = mods_snapshot(others, now);
            for (k = 0; k < m && !busy; k++) busy = (others[k].nos & MODS_NOS_BURNING) != 0;
        }
        gap = busy ? MODS_POLL_BUSY_MS : MODS_POLL_IDLE_MS;
        if (changed ? now - last_send >= MODS_CHANGE_MS : now - last_send >= gap) {
            WSABUF b; DWORD sent = 0;
            mods_request(packet, g_ticket_key, g_xuid, ++seq, epoch, car, nos, ug);
            b.len = MODS_REQUEST; b.buf = (char *)packet;
            g_r_wsasendto(s, &b, 1, &sent, 0, address, length, NULL, NULL);
            last_send = now; sent_nos = nos;
            for (i = 0; i < (int)MODS_UG; i++) sent_ug[i] = ug[i];
            if (++polls % 300 == 0) {
                if (!received && !silent_told) {
                    silent_told = 1;
                    log_begin(); log_raw("mods: no mod state reply to "); log_dec((unsigned long)polls);
                    log_raw(" polls, the server does not answer them (a server without the channel, or UDP replies blocked); NitroSystem's NOS stays off, "
                            "other players' NOS and underglow are not shown\n");
                }
                if (rejected) {
                    log_begin(); log_raw("mods: "); log_dec(rejected);
                    log_raw(" reply datagram(s) refused (not a valid, authentic, in-sequence mod state reply for this session)\n");
                    rejected = 0;
                }
            }
        }
        Sleep(20);
    }
    if (s != INVALID_SOCKET) close_socket(s);
    return 0;
}

static int patch_nos_flames(unsigned char *img);
static int patch_ug_remote(unsigned char *img);

/* Without the frame step the rule cannot run: the switch is still found and pulled, so NOS stays off online. */
static DWORD WINAPI nos_find_only(void *unused)
{
    (void)unused;
    g_api->wait_for_mods(30000);
    nos_find();
    return 0;
}

/* Online, after the lighting hooks: the NOS rule (its frame hook is nos_flames.inc's) and, unless FH2MP_NO_MODSYNC, the sharing. */
static void patch_mods(const wchar_t *azimuth_dir)
{
    wchar_t env[8];
    unsigned char *img = find_fh2_image();
    HANDLE thread;
    int share = GetEnvironmentVariableW(L"FH2MP_NO_MODSYNC", env, 8) == 0;
    g_ug_ini[0] = 0;
    if (azimuth_dir && lstrlenW(azimuth_dir) < MAX_PATH) { lstrcpyW(g_ug_ini, azimuth_dir); lstrcatW(g_ug_ini, L"\\fh2-mods\\Underglow.ini"); }
    if (!img || !g_light_enabled) {
        log_begin();
        log_raw(!img ? "mods: the game is not FH2 1.0.0.64" : "mods: the lighting hooks are off");
        log_raw(": NitroSystem's NOS stays off online, other players' NOS and underglow are not shown\n");
        /* the switch is still pulled: a NOS that nothing can turn on again is better than one that burns in events */
        thread = CreateThread(NULL, 0, nos_find_only, NULL, 0, NULL);
        if (thread) CloseHandle(thread);
        return;
    }
    g_mods_img = img;
    InterlockedExchange(&g_mods_share, share);
    if (!patch_nos_flames(img)) {
        log_begin();
        log_raw("mods: the game's frame step could not be hooked: NitroSystem's NOS stays off online, other players' NOS flames are not shown\n");
    }
    if (share && !patch_ug_remote(img)) {
        log_begin();
        log_raw("mods: the light hooks could not be made: other players' underglow is not shown\n");
    }
    InterlockedExchange(&g_mods_enabled, 1);
    thread = CreateThread(NULL, 0, mods_worker, NULL, 0, NULL);
    if (!thread) {
        DWORD err = GetLastError();
        InterlockedExchange(&g_mods_enabled, 0);
        log_begin(); log_raw("mods: the mod state thread could not be created, error "); log_dec(err); log_raw("; NitroSystem's NOS is not switched\n");
        return;
    }
    CloseHandle(thread);
    if (!share) { log_begin(); log_raw("mods: FH2MP_NO_MODSYNC is set: other players' NOS and underglow are not shown (the NOS rule stays)\n"); }
}
