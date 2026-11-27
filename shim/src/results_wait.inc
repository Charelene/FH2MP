/* Results wait report (FH2MP issue 547). Log and server report only; nothing here changes what the game does. FH2MP_NO_RESULTS_REPORT=1 turns it off.
 *
 * Why: after an online race some players' games stay on the results screen until Alt+F4 (prod 2026-11-10 .. 22: 3-5% of the players that finish, many
 * different players). The game server's log shows what the stuck client never does: a client that is back in Free Roam sends Console.RenderReady = 0,
 * MsgReadyRequest ready = 0 and RaceInProgress = 0 about 33 s after the race ended (the server sent SessionHost var 6 (InRace) = 0 at that moment);
 * the stuck one sends none of the three, and keeps its connection. Nothing on the client said which side it is waiting on, and the server cannot tell
 * whether the race-over update ever arrived.
 *
 * What it reports (client report code 7, carsel_wire.h; "results:" in the log), once a second checked from the monitor:
 *   Episode 1, the race-over update arrived: net_watch.inc saw SessionHost var 6 go 1 -> 0, and the local console still has ParticipatingInRace set
 *   RS_FIRST_MS later (the normal exit takes about 33 s). Reported at RS_FIRST_MS and again at RS_SECOND_MS.
 *   Episode 2, no race-over update arrived: the local console has ParticipatingInRace and RaceInProgress set, the last InRace value received is not 0
 *   (1, or none), and the car has not moved RS_MOVED_M metres for RS_FIRST_MS (a game that missed the update sits still on the results screen; a player
 *   that stops mid-race reports too, and the text says which of the two it saw). Reported at RS_FIRST_MS and RS_SECOND_MS of the car sitting still.
 * The text: "results wait 47 s: race over received 47 s ago (update #12); console R1 S1 P1 I1 Y0 L1 G1; rp RP_ONLINE_RACE" or
 * "results wait 47 s: no race over received (InRace last 1, 300 s ago); car still 47 s; console ...; rp ...". Console letters (stall_trace.inc, the
 * order of Nerd::ConsoleSynced's vars): R RenderReady, S SessionEntryFinalised, P ParticipatingInRace, I RaceInProgress, Y Ready,
 * L HasLoadedCarsForAllParticipants, G HasGrid; ? = not readable. rp = the game's rich presence (activity_report.inc), when its hook is in.
 * The local console: CConsoleManager at game+0x34af7a0, +0x18 the local console, its vars from +0xa0 in steps of 0x20 (race_wait.inc rw_console). */

#define RS_CODE 7
#define RS_FIRST_MS 45000u
#define RS_SECOND_MS 120000u
#define RS_MOVED_M 5
#define RS_TEXT 200

enum { RS_NONE, RS_OVER_SEEN, RS_OVER_MISSING };

typedef struct {
    int part, in_race;            /* the local console's ParticipatingInRace and RaceInProgress; -1 = not readable */
    int hooked;                   /* net_watch.inc sees the server's messages */
    int host;                     /* the last SessionHost InRace value received, -1 = none */
    unsigned long long over_at;   /* tick (ms) of the last 1 -> 0, 0 = none since a start */
    unsigned long long host_at;   /* tick of the last InRace update, 0 = none */
    unsigned seq;                 /* its update number */
    int idle_known;
    unsigned long long idle_since;
} rs_in;

typedef struct {
    int kind;
    unsigned long long since;
    int sent;
} rs_state;

/* Which episode `in` is in, and since when (ticks). */
static int rs_episode(const rs_in *in, unsigned long long *since)
{
    if (in->part != 1) return RS_NONE;
    if (in->over_at && in->host == 0) {
        *since = in->over_at;
        return RS_OVER_SEEN;
    }
    if (in->in_race == 1 && in->host != 0 && in->idle_known) {
        /* counted from the start at the earliest: the car stands still on the grid (dev 2026-11-23: a report 33 s into a race, 12 s of it the grid) */
        *since = in->host == 1 && in->host_at > in->idle_since ? in->host_at : in->idle_since;
        return RS_OVER_MISSING;
    }
    return RS_NONE;
}

/* One check. -> the episode to report now (RS_NONE = nothing) and the seconds it has lasted in *secs. */
static int rs_step(rs_state *st, const rs_in *in, unsigned long long now, unsigned *secs)
{
    unsigned long long since = 0, age;
    int kind = rs_episode(in, &since);
    if (kind == RS_NONE) {
        st->kind = RS_NONE;
        st->sent = 0;
        return RS_NONE;
    }
    if (kind != st->kind || since != st->since) {
        st->kind = kind;
        st->since = since;
        st->sent = 0;
    }
    age = now > since ? now - since : 0;
    if ((st->sent == 0 && age >= RS_FIRST_MS) || (st->sent == 1 && age >= RS_SECOND_MS)) {
        st->sent++;
        *secs = (unsigned)(age / 1000);
        return kind;
    }
    return RS_NONE;
}

typedef struct { char s[RS_TEXT + 1]; unsigned n; } rs_text;
static void rs_put(rs_text *b, const char *t)
{
    while (*t && b->n < RS_TEXT) b->s[b->n++] = *t++;
    b->s[b->n] = 0;
}
static void rs_num(rs_text *b, unsigned long long v)
{
    char d[24], one[2] = {0, 0};
    int k = 0;
    do d[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) {one[0] = d[--k]; rs_put(b, one);}
}

/* `vars`: the local console's bools R S P I Y L G (0, 1, or -1 = not readable). */
static void rs_describe(rs_text *b, int kind, unsigned secs, const rs_in *in, unsigned long long now, const signed char vars[7], const char *rp)
{
    static const char letters[8] = "RSPIYLG";
    int i;
    b->n = 0;
    b->s[0] = 0;
    rs_put(b, "results wait "); rs_num(b, secs); rs_put(b, " s: ");
    if (kind == RS_OVER_SEEN) {
        rs_put(b, "race over received "); rs_num(b, secs); rs_put(b, " s ago (update #"); rs_num(b, in->seq); rs_put(b, ")");
    } else if (!in->hooked) {
        rs_put(b, "no race over seen, the message hook is off; car still ");
        rs_num(b, secs); rs_put(b, " s");
    } else {
        rs_put(b, "no race over received (InRace last ");
        if (in->host < 0) rs_put(b, "none"); else rs_num(b, (unsigned)in->host);
        if (in->host_at) {rs_put(b, ", "); rs_num(b, (now > in->host_at ? now - in->host_at : 0) / 1000); rs_put(b, " s ago");}
        rs_put(b, "); car still "); rs_num(b, secs); rs_put(b, " s");
    }
    rs_put(b, "; console");
    for (i = 0; i < 7; i++) {
        char one[3] = {' ', letters[i], 0};
        rs_put(b, one);
        if (vars[i] < 0) rs_put(b, "?"); else rs_num(b, (unsigned)vars[i]);
    }
    if (rp && *rp) {rs_put(b, "; rp "); rs_put(b, rp);}
}

#ifndef RS_LOGIC_ONLY

static rs_state g_rs;
static int g_rs_off = -1;   /* -1 not read yet */
static unsigned char *g_rs_img;
static int g_rs_idle_has;
static long g_rs_idle_x, g_rs_idle_z;
static unsigned long long g_rs_idle_since;

/* The local console's seven bools; -> 1 when the block looks like a console (race_wait.inc rw_console's check). */
static int rs_console(signed char vars[7], int *part, int *in_race)
{
    unsigned char blk[0x20 * 8 + 1];
    void *cm = NULL, *con = NULL;
    int i;
    for (i = 0; i < 7; i++) vars[i] = -1;
    *part = *in_race = -1;
    if (!read_game_memory(g_rs_img + RW_RVA_CONSOLE_MGR, &cm, 8) || !cm || !read_game_memory((unsigned char *)cm + 0x18, &con, 8) || !con) return 0;
    if (!read_game_memory((unsigned char *)con + 0xa0, blk, sizeof blk)) return 0;
    for (i = 0; i < 7; i++) {
        if (blk[0x20 * i] > 1) return 0;
    }
    if (blk[0x20 * 7] > 2 || blk[0x20 * 8] > 2) return 0;
    for (i = 0; i < 7; i++) vars[i] = (signed char)blk[0x20 * i];
    *part = vars[2];
    *in_race = vars[3];
    return 1;
}

/* From the monitor, once a second. */
static void results_wait_tick(void)
{
    rs_in in;
    rs_text t;
    signed char vars[7];
    unsigned long long now = GetTickCount64();
    unsigned secs = 0;
    int kind;
    const char *rp = NULL;
    long x = 0, z = 0;
    int has = 0, value = -1;
    if (g_rs_off < 0) {
        wchar_t env[8];
        g_rs_off = GetEnvironmentVariableW(L"FH2MP_NO_RESULTS_REPORT", env, 8) != 0;
        g_rs_img = find_fh2_image();
    }
    if (g_rs_off || !g_rs_img) return;
    memset(&in, 0, sizeof in);
    if (!rs_console(vars, &in.part, &in.in_race) || in.part != 1) {
        g_rs.kind = RS_NONE;
        g_rs.sent = 0;
        g_rs_idle_has = 0;
        return;
    }
    in.hooked = g_nw_inrace_hooked != 0;
    in.host = g_nw_inrace;
    in.over_at = (unsigned long long)g_nw_over_at;
    in.host_at = (unsigned long long)g_nw_inrace_at;
    in.seq = (unsigned)g_nw_inrace_seq;
#ifndef AR_NO_HOOKS
    AcquireSRWLockShared(&g_ar_lock);
    has = g_ar.has_pos;
    x = g_ar.x;
    z = g_ar.z;
    value = g_ar.value;
    ReleaseSRWLockShared(&g_ar_lock);
    if (value >= 0 && value < AR_COUNT) rp = g_ar_names[value];
#endif
    if (has) {
        long dx = x - g_rs_idle_x, dz = z - g_rs_idle_z;
        if (!g_rs_idle_has || dx > RS_MOVED_M || dx < -RS_MOVED_M || dz > RS_MOVED_M || dz < -RS_MOVED_M) {
            g_rs_idle_has = 1;
            g_rs_idle_x = x;
            g_rs_idle_z = z;
            g_rs_idle_since = now;
        }
    }
    if (g_rs_idle_has) {   /* a car that is gone (the results screen may remove it) stays where it was last seen */
        in.idle_known = 1;
        in.idle_since = g_rs_idle_since;
    }
    kind = rs_step(&g_rs, &in, now, &secs);
    if (kind == RS_NONE) return;
    rs_describe(&t, kind, secs, &in, now, vars, rp);
    log_begin();
    log_raw("results: ");
    log_raw(t.s);
    log_raw("\n");
    carsel_report_post(RS_CODE, secs, t.s);
}

#endif
