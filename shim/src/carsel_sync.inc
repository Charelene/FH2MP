/* Free Roam event car select: an early picker waits for the others before its game reports its cars loaded (server/carsel.py, own magic FH2MPCS1).
 *
 * A game that has picked its car leaves the car select at once, reloads its car, and sits in the state flow's wait_for_event_to_start on the
 * "Setting Up Event" screen: the behaviour NetworkWaitForCarsLoaded (network.xml wait_for_cars_loaded_for_race_participants) polls every frame and
 * fires its Loaded transition as soon as no remote participant car is still loading (its per-frame update is vtable slot 14, game+0x8e0990,
 * FUN_1408e0990; the only way out of it is that transition, FUN_141925260 [C: decompile]). A participant that picked later changes its car after
 * that, so an early picker's copy of that car can be stale when the grid starts. The server knows who has left the car select (LobbyPlayer.PlayerStatus
 * CarSelect -> PreRace) and tells each client by a signed datagram whether to keep waiting. While it says so, this hook returns without running the
 * update, so no Loaded transition fires; everything else (the car loading, the loading screen) keeps running.
 *
 * Safe by default, every condition below must hold for a frame to be skipped, otherwise the game's own update runs: a server that has answered
 * within 5 s (an old server never does, so nothing changes against it), its newest answer says "wait" and is under 2.5 s old (a lost reply or a
 * stopped server ends the hold; at the start of a wait the frames are held for up to 1 s until the first answer of that wait), and this wait is
 * under 30 s old (the server's own deadline is 25 s after the event start). A new wait is told by a
 * gap of more than 1.5 s between two calls. FH2MP_NO_CARSELECT_HOLD=1 turns it off (hook and thread). Needs the lighting hooks (server address, ticket
 * epoch).
 *
 * v2 (magic FH2MPCS2): the server's answer also carries the milliseconds left until the group's deadline (every participant's car select ends at the
 * same moment). Two more hooks use it:
 *   C  game+0x8e0b90 NetworkWaitForCountdown update (FUN_1408e0b90): while it runs in the event car select the local UI countdown end time of the network
 *      manager ([[game+0x34d6348]+0x40], network clock ms, what NetworkSetLocalUiCountdown writes) is moved to clock + the group's time left, so the
 *      'SELECT YOUR CAR n' timers of all players agree and the screen ends at the same moment (a late entrant gets less than 20 s).
 *   D  game+0x5e68c0 (FUN_1405e68c0, ShowLoadingMessageNetworkFinaliseRaceStart, 'Setting Up Event'): remembers the loading-message component it is called
 *      with. While the hold is active the shim calls FUN_1405e6950 (ShowLoadingMessageNetworkLaunchCountdown, 'Waiting for Players {0}') on it and sets the
 *      invite countdown end time ([manager+0x44], read per frame by FUN_1405e54a0 through FUN_140a2adc0(1)) to the group's deadline, so the early picker's
 *      loading screen shows the time left; when the hold ends it calls the 'Setting Up Event' handler again and puts [+0x44] back.
 * Not verified in a running game (v1 was: the hold works). */
#include "carsel_wire.h"

#define CS_UPDATE_RVA 0x8e0990u
#define CS_NEW_WAIT_MS 1500u
#define CS_ANSWER_MS 5000u            /* a valid answer this recent = the server supports the hold */
#define CS_FRESH_MS 2500u             /* the newest answer must be this recent to keep holding */
#define CS_PENDING_MS 1000u           /* at the start of a wait the old answer is not trusted: held up to this long for one from this wait */
#define CS_MAX_HOLD_MS 30000u
#define CS_POLL_IDLE_MS 1000u
#define CS_POLL_ARMED_MS 100u
#define CS_ARMED_MS 600u              /* the hook ran this recently: poll fast */
#define CS_SILENT_POLLS 200u
#define CS_MGR_SLOT_RVA 0x34d6348u    /* DAT_1434d6348: the network manager pointer */
#define CS_CLOCK_SLOT_RVA 0x36e2450u  /* DAT_1436e2450: the network clock object (vtable slot 7 = u32 milliseconds) */
#define CS_COUNTDOWN_RVA 0x8e0b90u    /* NetworkWaitForCountdown update */
#define CS_MSG_FINAL_RVA 0x5e68c0u    /* loading message: mode 3, 'Setting Up Event' */
#define CS_MSG_WAIT_RVA 0x5e6950u     /* loading message: mode 2, 'Waiting for Players {0}' */
#define CS_LOCAL_END_OFFSET 0x40u     /* manager: local UI countdown end time */
#define CS_INVITE_END_OFFSET 0x44u    /* manager: invite countdown end time */
#define CS_MSG_MODE_OFFSET 0x9cu      /* loading message component: mode (0..4) */
#define CS_DEADLINE_STALE_MS 2500u    /* a deadline from an answer older than this is not used */
#define CS_DEADLINE_MAX_MS 60000u     /* a deadline further away than this is not believed */
#define CS_WRITE_TOLERANCE_MS 200     /* the local timer is moved only when it is this far from the group's deadline */
#define CS_COMP_BEFORE_MS 3000u       /* the loading message component is from this wait when it was seen at most this long before it began */
#define CS_COMP_MAX_AGE_MS 120000u
#ifndef CARSEL_NOW
#define CARSEL_NOW() GetTickCount64()
#endif

static SRWLOCK g_cs_lock = SRWLOCK_INIT;
static struct { int valid; unsigned char hold; ULONGLONG at; unsigned remaining; } g_cs;
static volatile LONG g_cs_enabled;
static volatile LONG64 g_cs_armed_at;               /* the cars-loaded hook (A) ran at this tick */
static volatile LONG64 g_cs_count_armed_at;         /* the countdown hook (C) ran at this tick */
static void (*g_cs_original)(void *, ULONGLONG, ULONGLONG, ULONGLONG);
static void (*g_cs_count_original)(void *);
static void (*g_cs_final_original)(void *);
static void (*g_cs_wait_message)(void *);           /* game+0x5e6950, called on the component; NULL = not verified */
static unsigned char *g_cs_mgr_slot, *g_cs_clock_slot;
static unsigned char *g_cs_fn_lo, *g_cs_fn_hi;      /* the clock's function must lie in the game image */
static void *volatile g_cs_comp;                    /* the loading message component FUN_1405e68c0 was last called with (game thread only) */
static ULONGLONG g_cs_comp_at;
/* race_wait.inc: called on every frame of a cars-loaded wait; 1 = it ran the node's Loaded transition itself, so the update is skipped. NULL = not installed. */
static int (*g_race_wait_step)(void *self, ULONGLONG now, ULONGLONG wait_first, int holding);

enum { CS_NONE = 0, CS_STARTED, CS_RELEASED, CS_NO_ANSWER, CS_LIMIT };
/* What one wait has seen; only the game thread touches it. */
typedef struct { ULONGLONG last_call, wait_first, hold_since; int holding, expired, reported; } carsel_wait;

/* One call of the hooked update at `now`, with the newest valid answer (hold flag and its tick, 0 = none yet). Returns 1 to skip the game's update;
 * *event says what changed (CS_STARTED / CS_RELEASED / CS_NO_ANSWER / CS_LIMIT) so the caller can log it once. */
static int carsel_decide(carsel_wait *w, ULONGLONG now, int enabled, unsigned char hold, ULONGLONG answer_at, int *event)
{
    int answered, want, pending;
    *event = CS_NONE;
    if (!w->last_call || now - w->last_call > CS_NEW_WAIT_MS) {          /* a new wait began */
        w->wait_first = now; w->holding = 0; w->expired = 0; w->reported = 0;
    }
    w->last_call = now;
    answered = answer_at && now - answer_at <= CS_ANSWER_MS;
    want = enabled && answered && answer_at >= w->wait_first && hold == 1 && now - answer_at <= CS_FRESH_MS;
    /* the answers of a server that supports the hold are older than this wait for the first ~100 ms of it: hold for the first fresh one, so a game
     * whose cars are already loaded does not leave in its first frame */
    pending = enabled && answered && answer_at < w->wait_first && now - w->wait_first < CS_PENDING_MS;
    if (w->expired) return 0;
    if (now - w->wait_first >= CS_MAX_HOLD_MS) {
        w->expired = 1;
        if (w->holding) {w->holding = 0; *event = CS_LIMIT;}
        return 0;
    }
    if (want) {
        if (!w->holding) {w->holding = 1; w->hold_since = now; *event = CS_STARTED;}
        return 1;
    }
    if (pending && !w->holding) return 1;
    if (w->holding) {
        w->holding = 0;
        *event = (answered && now - answer_at <= CS_FRESH_MS) ? CS_RELEASED : CS_NO_ANSWER;
    }
    return 0;
}

/* ---- pure rules of the group deadline (no game, no clock: testable) ---- */
/* The time left to the group's deadline at `now`, from an answer that said `remaining` ms at tick `at`. -> 1 with *left (0 = the deadline has come), 0 when there
 * is nothing to use: no deadline in the answer, no answer, or one older than CS_DEADLINE_STALE_MS. */
static int carsel_remaining(unsigned remaining, ULONGLONG at, ULONGLONG now, unsigned *left)
{
    ULONGLONG age;
    if (remaining == CARSEL_NO_DEADLINE || !at || now < at) return 0;
    age = now - at;
    if (age > CS_DEADLINE_STALE_MS) return 0;
    *left = remaining > age ? (unsigned)(remaining - age) : 0;
    return 1;
}
/* The car select timer: -> 1 and *end (network clock ms, u32 arithmetic) when the game's local countdown end `own_end` (0 = none running) is to be moved to the
 * group's deadline `left` ms from `clock`. */
static int carsel_timer_target(unsigned clock, unsigned own_end, unsigned left, unsigned *end)
{
    int diff;
    if (!own_end || !left || left > CS_DEADLINE_MAX_MS) return 0;
    *end = clock + left;
    diff = (int)(*end - own_end);
    return diff > CS_WRITE_TOLERANCE_MS || diff < -CS_WRITE_TOLERANCE_MS;
}
/* The loading message: what to do now. SHOW = call the 'Waiting for Players' handler once, RESTORE = call the 'Setting Up Event' handler once, MISSING = the
 * component was not seen in this wait (said once). `ended` stops it from showing again in the same wait. */
enum { CS_MSG_NONE = 0, CS_MSG_SHOW, CS_MSG_RESTORE, CS_MSG_MISSING };
typedef struct { int shown, ended, noted; void *comp; } carsel_msg;
static int carsel_msg_step(carsel_msg *m, int holding, int left_valid, unsigned left, int comp_ok)
{
    if (holding && left_valid && left > 0 && !m->shown && !m->ended) {
        if (!comp_ok) {
            if (m->noted) return CS_MSG_NONE;
            m->noted = 1;
            return CS_MSG_MISSING;
        }
        m->shown = 1;
        return CS_MSG_SHOW;
    }
    if (m->shown && (!holding || !left_valid || left == 0)) {
        m->shown = 0; m->ended = 1;
        return CS_MSG_RESTORE;
    }
    return CS_MSG_NONE;
}
/* The component captured by hook D belongs to the wait that began at `wait_first`. */
static int carsel_comp_ok(void *comp, ULONGLONG seen_at, ULONGLONG wait_first, ULONGLONG now)
{
    return comp && seen_at + CS_COMP_BEFORE_MS >= wait_first && now >= seen_at && now - seen_at <= CS_COMP_MAX_AGE_MS;
}
/* The state byte of a request: which hooks ran in the last CS_ARMED_MS. */
static unsigned char carsel_state_bits(ULONGLONG now, ULONGLONG armed_a, ULONGLONG armed_c)
{
    return (unsigned char)(((armed_a && now - armed_a <= CS_ARMED_MS) ? 1u : 0u) | ((armed_c && now - armed_c <= CS_ARMED_MS) ? 2u : 0u));
}

/* ---- reports to the server (carsel_wire.h, type 3) ----
 * Why this game sits where it sits, in the server's log next to what the server knows: a long cars-loaded wait, a car cache that does not change, a stall trace
 * (diag build). Posted from any thread, sent by the poll thread (CS_REPORT_SENDS times, 400 ms apart, the same sequence number; the server drops repeats), never
 * answered, never waited for. At most one per code every CS_REPORT_SAME_MS and CS_REPORT_MAX_TOTAL in a session, so a loop cannot flood the server. */
#define CS_REPORT_SLOTS 4
#define CS_REPORT_SENDS 3
#define CS_REPORT_RESEND_MS 400u
#define CS_REPORT_SAME_MS 8000u
#define CS_REPORT_MAX_TOTAL 60u
#define CS_REPORT_EXPIRE_MS 60000u
#define CS_REPORT_WAIT_MS 20000u      /* a cars-loaded wait this long is reported once */
enum { CS_RPT_CARS_LOADED = 1, CS_RPT_CAR_CACHE = 2, CS_RPT_STALL = 3, CS_RPT_STALL_END = 4, CS_RPT_OTHER = 5, CS_RPT_RACE_WAIT = 6 /* race_wait.inc */ };
typedef struct { int used; unsigned char code; unsigned detail, sends; ULONGLONG seq, posted_at, next_at; char text[CARSEL_REPORT_TEXT + 1]; } carsel_report_slot;
static carsel_report_slot g_cs_reports[CS_REPORT_SLOTS];
static SRWLOCK g_cs_report_lock = SRWLOCK_INIT;
static ULONGLONG g_cs_report_last[8];
static unsigned g_cs_report_total;

/* A small text builder for the report body (plain ASCII, cut at the report's limit). */
typedef struct { char s[CARSEL_REPORT_TEXT + 1]; unsigned n; } cs_text;
static void cs_t(cs_text *b, const char *t)
{
    while (*t && b->n < CARSEL_REPORT_TEXT) b->s[b->n++] = *t++;
    b->s[b->n] = 0;
}
static void cs_u(cs_text *b, unsigned long long v)
{
    char t[24]; int i = 23;
    t[i] = 0;
    do {t[--i] = (char)('0' + v % 10); v /= 10;} while (v);
    cs_t(b, t + i);
}

/* Queues one report. Returns 1 when it was queued, 0 when dropped (off, rate limit, queue full). Logs one line when queued. */
static int carsel_report_post(unsigned char code, unsigned detail, const char *text)
{
    ULONGLONG now = CARSEL_NOW(); int i, slot = -1;
    if (!g_cs_enabled || !text) return 0;
    AcquireSRWLockExclusive(&g_cs_report_lock);
    if (g_cs_report_total >= CS_REPORT_MAX_TOTAL || (g_cs_report_last[code & 7] && now - g_cs_report_last[code & 7] < CS_REPORT_SAME_MS)) {
        ReleaseSRWLockExclusive(&g_cs_report_lock);
        return 0;
    }
    for (i = 0; i < CS_REPORT_SLOTS; i++) {
        if (!g_cs_reports[i].used) {slot = i; break;}
    }
    if (slot < 0) {
        ReleaseSRWLockExclusive(&g_cs_report_lock);
        return 0;
    }
    g_cs_report_last[code & 7] = now; g_cs_report_total++;
    memset(&g_cs_reports[slot], 0, sizeof g_cs_reports[slot]);
    g_cs_reports[slot].used = 1; g_cs_reports[slot].code = code; g_cs_reports[slot].detail = detail; g_cs_reports[slot].posted_at = now;
    for (i = 0; text[i] && (unsigned)i < CARSEL_REPORT_TEXT; i++) g_cs_reports[slot].text[i] = text[i];
    ReleaseSRWLockExclusive(&g_cs_report_lock);
    log_begin(); log_raw("report: sent to the server, code "); log_dec(code); log_raw(": "); log_raw(text); log_raw("\n");
    return 1;
}

/* The next report that is due, copied out (and its send counted) under the lock; 0 when none. Reports older than CS_REPORT_EXPIRE_MS are dropped unsent. */
static int carsel_report_take(ULONGLONG now, ULONGLONG *seq, carsel_report_slot *out)
{
    int i, found = 0;
    AcquireSRWLockExclusive(&g_cs_report_lock);
    for (i = 0; i < CS_REPORT_SLOTS && !found; i++) {
        carsel_report_slot *r = &g_cs_reports[i];
        if (!r->used) continue;
        if (now - r->posted_at > CS_REPORT_EXPIRE_MS) {r->used = 0; continue;}
        if (now < r->next_at) continue;
        if (!r->seq) r->seq = ++*seq;
        *out = *r; found = 1;
        r->next_at = now + CS_REPORT_RESEND_MS;
        if (++r->sends >= CS_REPORT_SENDS) r->used = 0;
    }
    ReleaseSRWLockExclusive(&g_cs_report_lock);
    return found;
}

/* ---- the game's memory (game thread only; every pointer is read through read_game_memory first) ---- */
static unsigned char *carsel_manager(void)
{
    unsigned char *mgr = NULL;
    if (!g_cs_mgr_slot || !read_game_memory(g_cs_mgr_slot, &mgr, sizeof mgr) || !mgr) return NULL;
    return mgr;
}
static int carsel_clock(unsigned *out)
{
    unsigned char *obj = NULL; void **vtable = NULL; void *fn = NULL;
    if (!g_cs_clock_slot || !read_game_memory(g_cs_clock_slot, &obj, sizeof obj) || !obj || !read_game_memory(obj, &vtable, sizeof vtable) || !vtable ||
        !read_game_memory((unsigned char *)vtable + 7 * sizeof(void *), &fn, sizeof fn) || !fn ||
        (g_cs_fn_lo && ((unsigned char *)fn < g_cs_fn_lo || (unsigned char *)fn >= g_cs_fn_hi))) {
        return 0;
    }
    *out = ((unsigned (*)(void *))fn)(obj);
    return 1;
}
static int carsel_read_end(unsigned offset, unsigned *value)
{
    unsigned char *mgr = carsel_manager();
    return mgr && read_game_memory(mgr + offset, value, sizeof *value);
}
static int carsel_write_end(unsigned offset, unsigned value)
{
    unsigned char *mgr = carsel_manager(); unsigned old;
    if (!mgr || !read_game_memory(mgr + offset, &old, sizeof old)) return 0;
    *(volatile unsigned *)(mgr + offset) = value;
    return 1;
}
static int carsel_comp_sane(void *comp)
{
    unsigned mode;
    return comp && read_game_memory((unsigned char *)comp + CS_MSG_MODE_OFFSET, &mode, sizeof mode) && mode <= 4;
}

static carsel_wait g_cs_wait;
static carsel_msg g_cs_msg;
static unsigned g_cs_saved_invite; static int g_cs_saved_invite_valid;      /* [manager+0x44] before the hold wrote it */

/* Puts [manager+0x44] back. */
static void carsel_restore_invite(void)
{
    if (g_cs_saved_invite_valid) carsel_write_end(CS_INVITE_END_OFFSET, g_cs_saved_invite);
    g_cs_saved_invite_valid = 0;
}

/* The group deadline during a hold: the invite countdown end and the loading message. `event` is what carsel_decide just reported. */
static void carsel_hold_screen(ULONGLONG now, int holding, int event, unsigned remaining, ULONGLONG answer_at)
{
    unsigned left = 0, clock; int valid = carsel_remaining(remaining, answer_at, now, &left), action, comp_ok;
    if (event == CS_STARTED && !g_cs_saved_invite_valid && carsel_read_end(CS_INVITE_END_OFFSET, &g_cs_saved_invite)) g_cs_saved_invite_valid = 1;
    if (holding && valid && left > 0 && left <= CS_DEADLINE_MAX_MS && carsel_clock(&clock)) carsel_write_end(CS_INVITE_END_OFFSET, clock + left);
    comp_ok = holding && g_cs_wait_message && g_cs_final_original && carsel_comp_ok(g_cs_comp, g_cs_comp_at, g_cs_wait.wait_first, now) &&
              carsel_comp_sane(g_cs_comp);
    action = carsel_msg_step(&g_cs_msg, holding, valid, left, comp_ok);
    if (action == CS_MSG_SHOW) {
        g_cs_msg.comp = g_cs_comp;
        g_cs_wait_message(g_cs_comp);
        log_begin(); log_raw("carsel: loading screen shows Waiting for Players ("); log_dec(left); log_raw(" ms left)\n");
    } else if (action == CS_MSG_RESTORE) {
        if (g_cs_msg.comp && g_cs_msg.comp == g_cs_comp && g_cs_final_original && carsel_comp_sane(g_cs_comp)) g_cs_final_original(g_cs_comp);
        g_cs_msg.comp = NULL;
        log_begin(); log_raw("carsel: loading screen back to Setting Up Event\n");
    } else if (action == CS_MSG_MISSING) {
        log_begin(); log_raw("carsel: the loading message of this wait was not seen, the loading screen keeps Setting Up Event while the others choose\n");
    }
    if (!holding && g_cs_saved_invite_valid) carsel_restore_invite();
}

static void carsel_update(void *self, ULONGLONG a, ULONGLONG b, ULONGLONG c)
{
    ULONGLONG now = CARSEL_NOW(), since, previous = g_cs_wait.last_call; int event, skip, hold_flag; ULONGLONG answer_at; unsigned remaining;
    InterlockedExchange64(&g_cs_armed_at, (LONG64)now);
    AcquireSRWLockShared(&g_cs_lock);
    hold_flag = g_cs.valid ? g_cs.hold : 0; answer_at = g_cs.valid ? g_cs.at : 0; remaining = g_cs.valid ? g_cs.remaining : CARSEL_NO_DEADLINE;
    ReleaseSRWLockShared(&g_cs_lock);
    since = g_cs_wait.hold_since;
    if (!previous || now - previous > CS_NEW_WAIT_MS) {          /* a new wait: nothing of the last one is kept (its component may be gone) */
        carsel_restore_invite();
        memset(&g_cs_msg, 0, sizeof g_cs_msg);
    }
    skip = carsel_decide(&g_cs_wait, now, g_cs_enabled != 0, (unsigned char)hold_flag, answer_at, &event);
    if (event == CS_STARTED) {
        log_begin(); log_raw("carsel: holding the cars-loaded wait of this event until every participant has left the car select (server said wait)\n");
    } else if (event) {
        log_begin(); log_raw("carsel: hold over after "); log_dec((unsigned long)(now - since)); log_raw(" ms: ");
        log_raw(event == CS_RELEASED ? "the server released it" :
                event == CS_LIMIT ? "the 30 s limit of one wait was reached, the game continues without the server" :
                "no fresh answer from the server (a reply was lost or the server stopped answering), the game continues");
        log_raw("\n");
    }
    carsel_hold_screen(now, g_cs_wait.holding, event, remaining, answer_at);
    if (!g_cs_wait.reported && now - g_cs_wait.wait_first >= CS_REPORT_WAIT_MS) {
        cs_text t;
        g_cs_wait.reported = 1;
        t.n = 0; t.s[0] = 0;
        cs_t(&t, "the cars-loaded wait of this event has lasted "); cs_u(&t, (now - g_cs_wait.wait_first) / 1000); cs_t(&t, " s; ");
        if (g_cs_wait.holding) {cs_t(&t, "held by the server for "); cs_u(&t, (now - g_cs_wait.hold_since) / 1000); cs_t(&t, " s");}
        else cs_t(&t, g_cs_wait.expired ? "the server's hold gave up (30 s limit)" : "not held by the server");
        if (answer_at) {
            cs_t(&t, "; last answer "); cs_u(&t, now - answer_at); cs_t(&t, " ms ago, hold="); cs_u(&t, (unsigned)hold_flag);
            if (remaining != CARSEL_NO_DEADLINE) {cs_t(&t, ", group deadline in "); cs_u(&t, remaining); cs_t(&t, " ms at that answer");}
        } else cs_t(&t, "; the server never answered");
        carsel_report_post(CS_RPT_CARS_LOADED, (unsigned)((now - g_cs_wait.wait_first) / 1000), t.s);
    }
    if (g_race_wait_step && g_race_wait_step(self, now, g_cs_wait.wait_first, g_cs_wait.holding)) return;
    if (skip) return;
    if (g_cs_original) g_cs_original(self, a, b, c);
}

/* Hook C: the event car select's countdown behaviour. Moves the game's local countdown to the group's deadline, then runs the game's update. */
static ULONGLONG g_cs_count_last; static int g_cs_count_logged;           /* game thread only */
static void carsel_countdown(void *self)
{
    ULONGLONG now = CARSEL_NOW(), at; unsigned remaining, left, clock, own, end; int have;
    InterlockedExchange64(&g_cs_count_armed_at, (LONG64)now);
    if (!g_cs_count_last || now - g_cs_count_last > CS_NEW_WAIT_MS) g_cs_count_logged = 0;
    g_cs_count_last = now;
    if (g_cs_enabled) {
        AcquireSRWLockShared(&g_cs_lock);
        have = g_cs.valid; at = g_cs.valid ? g_cs.at : 0; remaining = g_cs.valid ? g_cs.remaining : CARSEL_NO_DEADLINE;
        ReleaseSRWLockShared(&g_cs_lock);
        if (have && carsel_remaining(remaining, at, now, &left) && carsel_read_end(CS_LOCAL_END_OFFSET, &own) && carsel_clock(&clock) &&
            carsel_timer_target(clock, own, left, &end) && carsel_write_end(CS_LOCAL_END_OFFSET, end) && !g_cs_count_logged) {
            int own_left = (int)(own - clock);
            g_cs_count_logged = 1;
            log_begin(); log_raw("carsel: car select timer set to the group deadline, "); log_dec(left); log_raw(" ms left (own timer had ");
            log_dec((unsigned long)(own_left > 0 ? own_left : 0)); log_raw(")\n");
        }
    }
    if (g_cs_count_original) g_cs_count_original(self);
}

/* Hook D: the loading message component that 'Setting Up Event' is shown on. Pass-through. */
static void carsel_final_message(void *comp)
{
    g_cs_comp = comp; g_cs_comp_at = CARSEL_NOW();
    if (g_cs_final_original) g_cs_final_original(comp);
}

static DWORD WINAPI carsel_worker(void *unused)
{
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    SOCKET s = INVALID_SOCKET; ULONGLONG seq = 0, received = 0, generation = 0, last_send = 0, polls = 0;
    SOCKET (WSAAPI *make_socket)(int,int,int);
    int (WSAAPI *nonblocking)(SOCKET,long,u_long *);
    int (WSAAPI *receive)(SOCKET,char *,int,int,struct sockaddr *,int *);
    int (WSAAPI *option)(SOCKET,int,int,const char *,int);
    int (WSAAPI *close_socket)(SOCKET);
    u_long nb = 1; int v6only = 0, socket_told = 0, silent_told = 0; unsigned rejected = 0;
    (void)unused;
    if (ws) {
        make_socket = (void *)GetProcAddress(ws, "socket"); nonblocking = (void *)GetProcAddress(ws, "ioctlsocket");
        receive = (void *)GetProcAddress(ws, "recvfrom"); option = (void *)GetProcAddress(ws, "setsockopt");
        close_socket = (void *)GetProcAddress(ws, "closesocket");
    } else make_socket = NULL, nonblocking = NULL, receive = NULL, option = NULL, close_socket = NULL;
    if (!make_socket || !nonblocking || !receive || !option || !close_socket) {
        log_begin(); log_raw("carsel: the early-picker hold is off, ws2_32.dll could not be used (error "); log_dec(GetLastError()); log_raw(")\n");
        InterlockedExchange(&g_cs_enabled, 0);
        return 0;
    }
    while (g_cs_enabled) {
        unsigned char address[28], epoch[16], packet[CARSEL_REQUEST + 8], hold, state; unsigned remaining;
        int length, i, n, drained; ULONGLONG predicted, current_generation, now = GetTickCount64(), gap;
        if (!g_light_enabled) {Sleep(1000); continue;}
        AcquireSRWLockShared(&g_light_lock);
        length = g_light.address_len; predicted = g_light.prediction_at; current_generation = g_light.generation;
        light_copy(address, g_light.address, (unsigned)length);
        for (i = 0; i < 16; i++) epoch[i] = g_light.epoch[i];
        ReleaseSRWLockShared(&g_light_lock);
        if (generation != current_generation) {
            if (s != INVALID_SOCKET) close_socket(s);
            s = INVALID_SOCKET; received = 0; generation = current_generation; polls = 0; rejected = 0; silent_told = 0;
            AcquireSRWLockExclusive(&g_cs_lock);
            g_cs.valid = 0; g_cs.at = 0; g_cs.hold = 0; g_cs.remaining = CARSEL_NO_DEADLINE;
            ReleaseSRWLockExclusive(&g_cs_lock);
        }
        if (!length || !predicted || now - predicted > 1000 || !g_ticket_ready || !g_r_wsasendto) {Sleep(100); continue;}
        if (s == INVALID_SOCKET) {
            s = make_socket(*(const unsigned short *)address, SOCK_DGRAM, IPPROTO_UDP);
            if (s == INVALID_SOCKET) {
                if (!socket_told) {socket_told = 1; log_begin(); log_raw("carsel: could not open the poll socket, error "); log_dec(GetLastError()); log_raw(", retrying every 0.5 s\n");}
                Sleep(500); continue;
            }
            if (*(const unsigned short *)address == AF_INET6) option(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only, sizeof v6only);
            if (nonblocking(s, FIONBIO, &nb)) {
                if (!socket_told) {socket_told = 1; log_begin(); log_raw("carsel: could not make the poll socket non-blocking, error "); log_dec(GetLastError()); log_raw(", retrying every 0.5 s\n");}
                close_socket(s); s = INVALID_SOCKET; Sleep(500); continue;
            }
            socket_told = 0;
        }
        for (drained = 0; drained < 32 && (n = receive(s, (char *)packet, sizeof packet, 0, NULL, NULL)) > 0; drained++) {
            if (carsel_response(packet, (unsigned)n, g_ticket_key, g_xuid, seq, received, epoch, &hold, &remaining)) {
                int first;
                AcquireSRWLockExclusive(&g_cs_lock);
                first = !g_cs.valid;
                if (g_light.generation == generation) {
                    g_cs.valid = 1; g_cs.hold = hold; g_cs.at = GetTickCount64(); g_cs.remaining = remaining;
                    received = light_u64(packet + 28);
                }
                ReleaseSRWLockExclusive(&g_cs_lock);
                if (first && received) {
                    log_begin(); log_raw("carsel: server answers, early pickers can be held (first authenticated reply after "); log_dec((unsigned long)polls);
                    log_raw(" polls, hold="); log_dec(hold); log_raw(")\n");
                }
            } else rejected++;
        }
        state = carsel_state_bits(now, (ULONGLONG)g_cs_armed_at, (ULONGLONG)g_cs_count_armed_at);
        gap = state ? CS_POLL_ARMED_MS : CS_POLL_IDLE_MS;
        if (now - last_send >= gap) {
            WSABUF b; DWORD sent = 0;
            carsel_request(packet, g_ticket_key, g_xuid, ++seq, epoch, state);
            b.len = CARSEL_REQUEST; b.buf = (char *)packet;
            g_r_wsasendto(s, &b, 1, &sent, 0, address, length, NULL, NULL);
            last_send = now;
            if (++polls % CS_SILENT_POLLS == 0) {
                if (!received && !silent_told) {
                    silent_told = 1;
                    log_begin(); log_raw("carsel: no reply to "); log_dec((unsigned long)polls);
                    log_raw(" polls, the server does not answer car select polls (a server without the hold, or UDP replies blocked); early pickers are not held\n");
                }
                if (rejected) {
                    log_begin(); log_raw("carsel: "); log_dec(rejected); log_raw(" reply datagram(s) refused (not a valid, authentic, in-sequence reply for this session)\n");
                    rejected = 0;
                }
            }
        }
        {
            carsel_report_slot due; ULONGLONG report_seq = seq;
            if (carsel_report_take(now, &report_seq, &due)) {
                unsigned char datagram[CARSEL_REPORT_MAX]; WSABUF rb; DWORD rsent = 0; unsigned total;
                if (report_seq > seq) seq = report_seq;
                total = carsel_report(datagram, g_ticket_key, g_xuid, due.seq, epoch, due.code, due.detail, due.text);
                rb.len = total; rb.buf = (char *)datagram;
                g_r_wsasendto(s, &rb, 1, &rsent, 0, address, length, NULL, NULL);
            }
        }
        Sleep(20);
    }
    if (s != INVALID_SOCKET) close_socket(s);
    return 0;
}

/* Hooks C and D (the group deadline). Each is optional: when one cannot be installed the hold (hook A) goes on without that part. */
static void carsel_attach_deadline(unsigned char *img)
{
    static const unsigned char countdown_code[9] = {0x40,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xf9};
    static const unsigned char final_code[20] = {0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xc7,0x81,0x9c,0x00,0x00,0x00,0x03};
    static const unsigned char wait_code[20] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0xd9,0xc7,0x81,0x9c,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x48};
    g_cs_mgr_slot = img + CS_MGR_SLOT_RVA; g_cs_clock_slot = img + CS_CLOCK_SLOT_RVA; g_cs_fn_lo = img; g_cs_fn_hi = img + FH2_SIZE_OF_IMAGE;
    if (honk_hooked_by_mod(img, FH2_SIZE_OF_IMAGE, img + CS_COUNTDOWN_RVA) || !code_is("carsel", "the car select countdown update", img, CS_COUNTDOWN_RVA, countdown_code, sizeof countdown_code) ||
        !plat_hook("carsel: car select countdown update", img + CS_COUNTDOWN_RVA, (void *)carsel_countdown, (void **)&g_cs_count_original)) {
        log_begin(); log_raw("carsel: the car select timers are not set to a common deadline (the countdown update at game+0x8e0b90 could not be hooked)\n");
    } else {
        log_begin(); log_raw("carsel: the car select timer follows the group's deadline (the update at game+0x8e0b90)\n");
    }
    if (honk_hooked_by_mod(img, FH2_SIZE_OF_IMAGE, img + CS_MSG_FINAL_RVA) || !code_is("carsel", "the loading message handler (Setting Up Event)", img, CS_MSG_FINAL_RVA, final_code, sizeof final_code) ||
        !code_is("carsel", "the loading message handler (Waiting for Players)", img, CS_MSG_WAIT_RVA, wait_code, sizeof wait_code) ||
        !plat_hook("carsel: loading message Setting Up Event", img + CS_MSG_FINAL_RVA, (void *)carsel_final_message, (void **)&g_cs_final_original)) {
        g_cs_final_original = NULL;
        log_begin(); log_raw("carsel: the loading screen keeps Setting Up Event while an early picker waits (the loading message handlers at game+0x5e68c0 / +0x5e6950 could not be used)\n");
    } else {
        g_cs_wait_message = (void (*)(void *))(img + CS_MSG_WAIT_RVA);
        log_begin(); log_raw("carsel: an early picker's loading screen shows Waiting for Players with the group's time left\n");
    }
}

/* -> 1 when the hook and the thread are in place. Call after patch_lighting and patch_honk (it uses their address book and entry check). */
static int patch_carsel(void)
{
    static const unsigned char want[15] = {0x40,0x57,0x48,0x83,0xec,0x40,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff};
    unsigned char *img = find_fh2_image(); HANDLE thread;
    if (!img) {
        log_begin(); log_raw("carsel: early pickers are not held (the game is not FH2 1.0.0.64)\n");
        return 0;
    }
    if (honk_hooked_by_mod(img, FH2_SIZE_OF_IMAGE, img + CS_UPDATE_RVA)) {
        log_begin(); log_raw("carsel: early pickers are not held, the cars-loaded update at game+0x8e0990 is hooked by another mod\n");
        return 0;
    }
    if (!code_is("carsel", "the cars-loaded wait update", img, CS_UPDATE_RVA, want, sizeof want)) {
        log_begin(); log_raw("carsel: early pickers are not held, the cars-loaded update is not the expected code\n");
        return 0;
    }
    InterlockedExchange(&g_cs_enabled, 1);
    if (!plat_hook("carsel: cars-loaded wait update", img + CS_UPDATE_RVA, (void *)carsel_update, (void **)&g_cs_original)) {
        InterlockedExchange(&g_cs_enabled, 0);
        log_begin(); log_raw("carsel: early pickers are not held, the cars-loaded update could not be hooked\n");
        return 0;
    }
    thread = CreateThread(NULL, 0, carsel_worker, NULL, 0, NULL);
    if (!thread) {
        DWORD err = GetLastError();
        InterlockedExchange(&g_cs_enabled, 0);      /* the hook stays but never holds: no server answer, so every frame passes through */
        log_begin(); log_raw("carsel: early pickers are not held, the poll thread could not be created, error "); log_dec(err); log_raw("\n");
        return 0;
    }
    CloseHandle(thread);
    log_begin(); log_raw("carsel: early pickers wait for the others before their cars-loaded wait ends (the update at game+0x8e0990 is checked against the server)\n");
    carsel_attach_deadline(img);
    return 1;
}
