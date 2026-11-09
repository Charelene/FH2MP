/*
 * Online setup at game start (build 1.0.0.64), dev 2026-11-01.
 *
 * 1. "Unable to connect to the Live configuration servers" for a whole game run (about one start in six, 11 of 63 starts on dev
 *    2026-10-31..11-01). In every such run the LSP log shows the Logon and the Turn 10 calls, but none of the requests that the game's
 *    profile sign-in event starts: no music profile, no EDS fields, no achievements and no gameserverds variants (the cloud
 *    configuration). In every good run all four come within 2 s of the Logon.
 *    The event: CLoginManager (0x124c4c0 / 0x1247690) posts system event (4,3) with a CSystemEventParamGamerProfile whose byte +0x20
 *    is "this profile is the active (primary) one" at the moment of posting. Network::Live::MultiplayerServices' listener (vslot 1 of
 *    its +8 subobject, 0x183abd0) ignores the event when that byte is 0 (`cmp byte [r9+0x20],0 ; je ret` at 0x183acd7); otherwise it
 *    stores the profile id (+0x10 of the service) and hands it to the CloudConfiguration (MPS+0x70, user at +0xa0, 0x183c490). With
 *    no user, CloudConfiguration's load (0x1837d10) reports failure to every caller without sending a request, on every attempt, for
 *    the rest of the run. COfferManager (EDS) and the achievement/music starters likewise only act when the profile is the active
 *    one at that moment. So the run is bad when Azimuth's sign-in completes before the game has made the profile the active one.
 *    Azimuth has exactly one local player, so a sign-in event for a profile that is not (yet) the active one is still the player's:
 *    the listener is hooked, and for event 3 it sees the byte as 1 (put back after the call, other listeners see the original).
 *    [C] for the code paths, [I] for the ordering (the posting flag itself has not been read in a bad run); the hook logs every
 *    sign-in event it sees, and the watchdog logs the service and configuration fields, so the next bad run settles it.
 *    Disable with env FH2MP_NO_SIGNINFIX.
 *    Settled 2026-11-01 22:39 UTC: two bad runs had the event for the active profile and the profile in both objects, so this is not the
 *    cause of those runs (see 3). The hook stays: it only acts on a sign-in that is not yet the active profile, and its log line helps.
 *
 * 2. ~30 s of "Finding session" before the ticket is even posted (38 of 361 dev searches, all 30-31 s, the rest 0-1 s; every search
 *    right after leaving an online session). When the cloud configuration is loaded, FUN_1417b1b30 (state 0xb) starts
 *    Nerd::MultiplayerSession::MeasureQosToSessionPeers (0x179b030) with a timeout of 10 s on the first call of the run and 30 s on
 *    every later one (`mov ebx,30000 ; mov eax,10000 ; cmp [rcx+0xd4],1 ; cmove ebx,eax` at 0x17b1b58); the timeout goes to
 *    Windows.Xbox.Networking.QualityOfService.MeasureQualityOfServiceAsync (WinRT::ActivateQualityOfService 0x17aabf0, 8 probes).
 *    Azimuth has no real QoS measurement (offline platform layer), so when it does not answer at once the search waits the whole
 *    timeout and then goes on exactly as before. Both values become 1 s. Disable with env FH2MP_NO_QOSTIMEOUT.
 */
#define FH2_MPS_SINGLETON_RVA   0x36e6748u  /* g_MultiplayerServices (pointer) */
#define FH2_MPS_USER_EVENT_RVA  0x183abd0u  /* MultiplayerServices +8 subobject, vslot 1: system events (4,3..6) */
#define FH2_MPS_SIGNIN_TEST_RVA 0x183acd7u  /* cmp byte [r9+0x20],0 ; je 0x183ad22 ; mov rcx,[r9+0x18] */
#define FH2_QOS_TIMEOUT_RVA     0x17b1b52u  /* inc [rcx+0xd4] ; mov ebx,30000 ; mov eax,10000 ; cmp [rcx+0xd4],1 */

typedef char (*mps_user_event_fn)(void *self, void *category, int event, unsigned char *param);
static mps_user_event_fn g_mps_user_event_original;
static volatile LONG g_signin_events;     /* event 3 (profile signed in) as the multiplayer services saw it */
static volatile LONG g_signin_inactive;   /* ... of which the profile was not the active one yet */

static char mps_user_event(void *self, void *category, int event, unsigned char *param)
{
    unsigned char active;
    LONG n;
    char r;

    if (event != 3 || !param) {
        return g_mps_user_event_original(self, category, event, param);
    }
    active = param[0x20];
    n = InterlockedIncrement(&g_signin_events);
    if (!active) {
        InterlockedIncrement(&g_signin_inactive);
        param[0x20] = 1;
        r = g_mps_user_event_original(self, category, event, param);
        param[0x20] = active;
    } else {
        r = g_mps_user_event_original(self, category, event, param);
    }
    if (n <= 4) {
        log_begin();
        log_raw("online-setup: profile sign-in event #");
        log_dec((unsigned long)n);
        log_raw(active ? " for the active profile (the game's own path)\n"
                       : " arrived before the profile was the active one; the multiplayer services take it anyway (one local player), "
                         "otherwise this run could not load the cloud configuration\n");
    }
    return r;
}

static int patch_signin_event(void)
{
    static const unsigned char want[] = {0x41, 0x80, 0x79, 0x20, 0x00, 0x74, 0x44, 0x49, 0x8B, 0x49, 0x18};
    static const unsigned char head[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF9};
    unsigned char *img = find_fh2_image();

    if (!img || g_mps_user_event_original) {
        return g_mps_user_event_original != NULL;
    }
    if (!code_is("online-setup", "the multiplayer services' sign-in test", img, FH2_MPS_SIGNIN_TEST_RVA, want, sizeof want) ||
        !code_is("online-setup", "the multiplayer services' event listener", img, FH2_MPS_USER_EVENT_RVA, head, sizeof head)) {
        return 0;
    }
    return plat_hook("online-setup: sign-in event reaches the multiplayer services", img + FH2_MPS_USER_EVENT_RVA, (void *)mps_user_event,
                     (void **)&g_mps_user_event_original);
}

static int patch_qos_timeout(void)
{
    static const unsigned char want[] = {0xFF, 0x81, 0xD4, 0x00, 0x00, 0x00, 0xBB, 0x30, 0x75, 0x00, 0x00, 0xB8, 0x10, 0x27, 0x00, 0x00,
                                         0x83, 0xB9, 0xD4, 0x00, 0x00, 0x00, 0x01};
    static const unsigned char one_s[] = {0xE8, 0x03, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    int done = 0;

    if (!img) {
        return 0;
    }
    if (memcmp(img + FH2_QOS_TIMEOUT_RVA + 7, one_s, 4) == 0 && memcmp(img + FH2_QOS_TIMEOUT_RVA + 12, one_s, 4) == 0) {
        return 1;
    }
    if (!code_is("mmtimeout", "the QoS measurement start (FUN_1417b1b30)", img, FH2_QOS_TIMEOUT_RVA, want, sizeof want)) {
        return 0;
    }
    done += plat_patch("mmtimeout: QoS measurement timeout, later searches", img + FH2_QOS_TIMEOUT_RVA + 7, want + 7, one_s, 4);
    done += plat_patch("mmtimeout: QoS measurement timeout, first search", img + FH2_QOS_TIMEOUT_RVA + 12, want + 12, one_s, 4);
    return done == 2;
}

/*
 * 2b. The QoS measurement itself fails (win1 -> dev 2026-11-08, debug-shim trace, docs/re/online-services.md "Free Roam join stall").
 *    FUN_1417b1b30 runs Nerd::MultiplayerSession::MeasureQosToSessionPeers (0x179b030, result = {int hr, text}) as a task on a worker thread.
 *    Under Azimuth the platform's Windows.Xbox.Networking.QualityOfService has nothing to measure with: the call comes back at once with
 *    hr 0x800704cf (network unreachable), an empty text and no metrics [C: breakpoints at 0x17a3b91 and 0x17a3bf1, 3 of 3 hand-played Free
 *    Roam joins]. The match request is never built, the server sees only `GET qosservers`, and the game shows "Unable to join session. Please
 *    check your network connection and try again." (once, the same step let a join through [C]). The measurement only ranks servers and
 *    nothing in a join here needs it: its answer becomes hr 0 with no metrics [I: that this lets the search go on is what the next hand-played
 *    join shows]. Any other hr passes untouched. Disable with env FH2MP_NO_QOSRESULT.
 */
#define FH2_QOS_MEASURE_RVA 0x179b030u
#define FH2_QOS_UNREACHABLE ((int)0x800704CF)
typedef int *(*qos_measure_fn)(int *result, unsigned probes, unsigned timeout_ms, unsigned limit);
static qos_measure_fn g_qos_measure_original;
static volatile LONG g_qos_measure_fixed;

static int *qos_measure(int *result, unsigned probes, unsigned timeout_ms, unsigned limit)
{
    int *r = g_qos_measure_original(result, probes, timeout_ms, limit);

    if (r && *r == FH2_QOS_UNREACHABLE) {
        *r = 0;
        if (InterlockedIncrement(&g_qos_measure_fixed) == 1) {
            log_begin();
            log_raw("online-setup: the QoS measurement (MeasureQosToSessionPeers) came back with 0x800704cf, network unreachable, because the platform cannot measure; reported as success with no metrics so the search goes on (first time this run, later ones are not written)\n");
        }
    }
    return r;
}

static int patch_qos_measure_result(void)
{
    static const unsigned char head[] = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x48, 0x08, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    unsigned char *img = find_fh2_image();

    if (!img) {
        return 0;
    }
    if (!code_is("qosresult", "the QoS measurement (MeasureQosToSessionPeers)", img, FH2_QOS_MEASURE_RVA, head, sizeof head)) {
        return 0;
    }
    return plat_hook("qosresult: MeasureQosToSessionPeers reports success", img + FH2_QOS_MEASURE_RVA, (void *)qos_measure,
                     (void **)&g_qos_measure_original);
}

/*
 * 3. The bad runs after fix 1 (dev 2026-11-01 22:39 and 22:39:53 UTC): the sign-in event came for the active profile, the services and the
 *    cloud configuration hold the player's profile, and the configuration sits in state 1 (0x1837d10: "variants request issued") for the
 *    rest of the run. nginx shows that in such a run not one request of the game's Live REST path (User-Agent ATGHTTP_DURANGO:
 *    music profile, EDS fields, achievements, gameserverds variants; Xls::HttpProtocol2::SendLiveMessageTask 0x1b7d380) reaches the
 *    server, while the Turn 10 calls (User-Agent "Xls Client") and XSAPI's own ("fh2mp") do. All four are issued by the sign-in
 *    consumers at the moment of sign-in and all go through Xls::TSTransportClient::SendWebGateRequest (0xeea3b0), which refuses a call
 *    with 0x8050002c, silently for these callers (Client_InvokeWebService 0xeda580 returns nothing), when the transport's +0x140 is still
 *    0 (0xeea690); +0x140 is filled by the transport's logon handling (OnLogonChanged 0xeeb490 and 0xee8790/0xeec4e9). The cloud
 *    configuration's Update (0x183dc40) has no timeout for state 1, so a refused variants request leaves it there for good [C for the
 *    code paths, I for the refusal being the one in these runs]. The hook below counts and logs every refused call (service, method), so
 *    the next bad run says it directly.
 *    Recovery without a restart: when the watchdog sees state 1 and no gameserverds request 20 s after the first server request, it moves
 *    the configuration to the game's own "failed, retry now" state (state 4 with the retry time +0xa8 = now); its Update then sets state 0
 *    and loads again (0x1837d10), by then the transport is logged on. Disable with env FH2MP_NO_CCRETRY (diagnosis stays).
 */
#define FH2_WEBGATE_SEND_RVA  0xeea3b0u   /* Xls::TSTransportClient::SendWebGateRequest (13 arguments) */
#define FH2_WEBGATE_CHECK_RVA 0xeea41bu   /* mov edx,[r9] ; call 0xeea690 (refuses with 0x8050002c while this+0x140 is 0) */

typedef long (*webgate_send_fn)(unsigned char *self, const char *svc, const char *method, void *a4, void *a5, void *a6, void *a7, void *a8,
                                void *a9, void *a10, void *a11, void *a12, void *a13);
static webgate_send_fn g_webgate_send_original;
static volatile LONG g_webgate_refused;
static volatile LONG g_webgate_refused_live; /* of which Forza.WebServices.Live* / XboxMusic: the cloud configuration and its siblings */

static int ascii_starts(const char *s, const char *p)
{
    while (*p) {
        if (*s++ != *p++) {
            return 0;
        }
    }
    return 1;
}

static void log_game_ascii(const char *s, int max)
{
    char buf[80];
    int i;

    if (max > (int)sizeof buf - 1) {
        max = (int)sizeof buf - 1;
    }
    for (i = 0; i < max; i++) {
        char c = 0;
        if (!read_game_memory(s + i, &c, 1) || !c) {
            break;
        }
        buf[i] = (c >= 32 && c < 127) ? c : '?';
    }
    buf[i] = 0;
    log_raw(i ? buf : "?");
}

static long webgate_send(unsigned char *self, const char *svc, const char *method, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9,
                         void *a10, void *a11, void *a12, void *a13)
{
    void *logon = NULL;

    if (self && read_game_memory(self + 0x140, &logon, sizeof logon) && !logon) {
        LONG n = InterlockedIncrement(&g_webgate_refused);
        char head[24];
        int live = 0, i;

        for (i = 0; i < (int)sizeof head - 1; i++) {
            head[i] = 0;
            if (!svc || !read_game_memory(svc + i, &head[i], 1) || !head[i]) {
                break;
            }
        }
        head[sizeof head - 1] = 0;
        if (ascii_starts(head, "Forza.WebServices.Live") || ascii_starts(head, "Forza.WebServices.XboxM")) {
            live = 1;
            InterlockedIncrement(&g_webgate_refused_live);
        }
        if (n <= 8) {
            log_begin();
            log_raw("online-setup: the game's web transport refuses ");
            if (svc) {
                log_game_ascii(svc, 60);
            }
            log_raw(" ");
            if (method) {
                log_game_ascii(method, 40);
            }
            log_raw(live ? " before its logon is in place (0x8050002c, dropped without an error to the caller)\n"
                         : " before its logon is in place (0x8050002c)\n");
        }
    }
    return g_webgate_send_original(self, svc, method, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
}

static int patch_webgate_watch(void)
{
    static const unsigned char head[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9};
    static const unsigned char check[] = {0x41, 0x8B, 0x11, 0xE8, 0x6D, 0x02, 0x00, 0x00, 0x85, 0xC0};
    unsigned char *img = find_fh2_image();

    if (!img || g_webgate_send_original) {
        return g_webgate_send_original != NULL;
    }
    if (!code_is("online-setup", "Xls::TSTransportClient::SendWebGateRequest", img, FH2_WEBGATE_SEND_RVA, head, sizeof head) ||
        !code_is("online-setup", "the web transport's logon check", img, FH2_WEBGATE_CHECK_RVA, check, sizeof check)) {
        return 0;
    }
    return plat_hook("online-setup: web requests refused before the logon are logged", img + FH2_WEBGATE_SEND_RVA, (void *)webgate_send,
                     (void **)&g_webgate_send_original);
}

/* The cloud configuration (MultiplayerServices+0x70): -> state (+8), or -1 when it cannot be read; *cc_out gets its address. */
static int cloud_config_state(unsigned char **cc_out)
{
    unsigned char *img = find_fh2_image();
    unsigned char *mps = NULL;
    int state = -1;

    if (img && read_game_memory(img + FH2_MPS_SINGLETON_RVA, &mps, sizeof mps) && mps &&
        read_game_memory(mps + 0x70 + 8, &state, sizeof state)) {
        if (cc_out) {
            *cc_out = mps + 0x70;
        }
        return state;
    }
    return -1;
}

/* Fix 3's recovery: state 1 with no request -> the game's "failed, retry now". -> 1 when it was done. */
static int cloud_config_retry_now(void)
{
    unsigned char *cc = NULL;
    int state = cloud_config_state(&cc);
    DWORD now = GetTickCount();
    int four = 4;
    SIZE_T w = 0;

    if (state != 1 || !cc || !readable_game_range(cc, 0xb0)) {
        return 0;
    }
    /* the retry time first, then the state: Update (0x183dc40) turns 4 into 0 only once the time has passed */
    if (!WriteProcessMemory(GetCurrentProcess(), cc + 0xa8, &now, sizeof now, &w) || w != sizeof now ||
        !WriteProcessMemory(GetCurrentProcess(), cc + 8, &four, sizeof four, &w) || w != sizeof four) {
        return 0;
    }
    return 1;
}

/* For the watchdog line: what the multiplayer services and the cloud configuration hold. */
static void online_setup_log_state(void)
{
    unsigned char *img = find_fh2_image();
    unsigned char *mps = NULL;
    unsigned long long user = 0, cc_user = 0;
    int cc_state = -1;

    if (img) {
        read_game_memory(img + FH2_MPS_SINGLETON_RVA, &mps, sizeof mps);
    }
    if (mps) {
        read_game_memory(mps + 0x10, &user, sizeof user);
        read_game_memory(mps + 0x70 + 8, &cc_state, sizeof cc_state);
        read_game_memory(mps + 0x70 + 0xa0, &cc_user, sizeof cc_user);
    }
    log_raw("; multiplayer services ");
    log_raw(mps ? "present" : "absent");
    log_raw(", their profile 0x");
    log_hex(user, 16);
    log_raw(", cloud configuration state ");
    log_dec((unsigned long)(cc_state < 0 ? 0 : cc_state));
    log_raw(cc_state < 0 ? " (unread)" : "");
    log_raw(" with profile 0x");
    log_hex(cc_user, 16);
    log_raw(", sign-in events seen ");
    log_dec((unsigned long)g_signin_events);
    log_raw(" (");
    log_dec((unsigned long)g_signin_inactive);
    log_raw(" before the profile was active), web requests refused before the logon ");
    log_dec((unsigned long)g_webgate_refused);
    log_raw(" (");
    log_dec((unsigned long)g_webgate_refused_live);
    log_raw(" of them Live REST)");
}
