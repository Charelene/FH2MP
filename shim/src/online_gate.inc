/* Going online through the platform (FH2 Mod Platform 2.9.0, FH2Platform.Online in fh2mod.h; docs/api.md "Online play").
 *
 * The platform keeps mods that shouldn't run online (an author's offline_only, or the player's "Load when playing online" switch off) out of a
 * game started from the FH2MP launcher: it decides before any mod loads, from FH2MP's session file. When the session turns up only later (the
 * launcher wrote it while the game was starting, a game started with --skip-launcher, issue #22 of the platform), such a mod may already have
 * run: its hooks went in, or its text, SQL or files were written for the game, which nothing can undo. going_online reports exactly that before
 * it changes anything: 0, and the game counts as online from then on (the platform stops the others); over 0, nothing was changed, and FH2MP
 * must stay offline. It also counts the car packs in fh2-cars when Extra Options had the game read their database instead of mediaoverride's.
 *
 * online_allowed() asks once, the first time anything online is about to go in (the Winsock and request imports, the server's car database),
 * from FH2MP's own threads (going_online may wait on the platform's lock). With a session and a refusal, this game stays offline: the
 * network layer stays out (the game keeps Azimuth's offline stubs), the server's car database is not placed (the game keeps its own), and
 * FH2MP.Online says so (online_held, is_online 0), so the Guide stays offline too and tells the player. Without a session there is nothing to
 * ask, and an older platform has no interface: both go on as before. */
static INIT_ONCE g_gate_once = INIT_ONCE_STATIC_INIT;
static volatile LONG g_net_held;          /* 1: going_online named mods that ran first; this game stays offline */
static char g_held_names[256];            /* their names, "Nitro, Other" (UTF-8) */
static int g_held_count;                  /* how many going_online reported */

static BOOL CALLBACK online_gate_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    const FH2PlatformOnline *o = NULL;
    char names[sizeof g_held_names];
    int n;

    (void)once, (void)param, (void)ctx;
    if (!g_token[0]) {
        return TRUE;                      /* no session: the platform counts the game offline as well */
    }
    if (g_api->api_version >= 2 && g_api->get_interface) {
        o = (const FH2PlatformOnline *)g_api->get_interface(FH2_ONLINE_INTERFACE, 1);
    }
    if (!o || o->size < sizeof *o || !o->going_online) {
        log_begin();
        log_raw("online-gate: the platform has no " FH2_ONLINE_INTERFACE " (older than 2.9.0); the game goes online as before\n");
        return TRUE;
    }
    names[0] = 0;
    n = o->going_online(names, sizeof names);
    if (n <= 0) {
        log_begin();
        log_raw(o->playing_online && o->playing_online() ? "online-gate: no mod that shouldn't run online has run in this game; the platform counts it as online\n"
                                                         : "online-gate: going_online answered 0 but the platform does not count the game as online; it goes online as before\n");
        return TRUE;
    }
    lstrcpynA(g_held_names, names, sizeof g_held_names);
    g_held_count = n;
    InterlockedExchange(&g_net_held, 1);
    log_begin();
    log_dec((unsigned long)n);
    log_raw(n == 1 ? " mod that shouldn't run online has" : " mods that shouldn't run online have");
    log_raw(" already run in this game (");
    log_raw(g_held_names[0] ? g_held_names : "no names given");
    log_raw("): online-gate keeps this game offline (no network layer, the game's own car database); started again with Play it goes online\n");
    return TRUE;
}

/* 1 when this game may go online (or has no session to go online with), 0 when going_online said to stay offline. Blocks the first time. */
static int online_allowed(void)
{
    InitOnceExecuteOnce(&g_gate_once, online_gate_once, NULL, NULL);
    return !g_net_held;
}
