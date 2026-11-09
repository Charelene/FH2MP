/* Stall tracer for the event load (docs/diag.md "Stall trace"), compiled only into the variant built with `shim/build.sh diag`.
 *
 * Why: prod 2026-11-05 20:14:45 UTC (lobby a25faeb1): the server started a Free Roam race event with 5 participants and none of the five game
 * clients ever reported Console.HasLoadedCarsForAllParticipants (the server saw nothing for 60 s). Earlier events of the same session loaded in
 * 2.5 s. The shim log had nothing about it. This file records, with the UTC time of day, which step of the event load a client is in, and when
 * the starting grid has not arrived 10 s after the event began, a snapshot of everything that can be read from the game's memory every 5 s
 * (at most 12 per event). It only observes: the two hooks call the game's function unchanged, and every game pointer is read through
 * read_game_memory (ReadProcessMemory on ourselves), so a bad pointer gives a failed read, not a crash. No game virtual function is called.
 * Lines are "diag: HH:MM:SS.mmm UTC stall: ..." (the release log filter keeps "diag:"), no line per frame or per packet.
 *
 * What is logged
 *   event begin     the local Console.ParticipatingInRace went 0 -> 1 (the server selected this client), or the state flow entered
 *                   NetworkWaitForCarsLoaded / NetworkSelectDefaultCar / NetworkIsPlayerCarValidForEvent; then the last 12 state flow changes before it ("history", kept in a ring) and the vars.
 *   timeline        +T ms (since the begin) for: every change of the state flow's active path (the behaviors, i.e. the States:: node classes,
 *                   of each active state, root to leaf), every change of a local Console var, the NetworkSetHasLoadedCarsForAllParticipants
 *                   node running, the grid message (grid_trace.inc) or Console.RaceInProgress arriving (that ends the trace), the event being
 *                   left (ParticipatingInRace -> 0), and the state flow not being stepped for 3 s (a blocked game thread).
 *   snapshot        10 s after the begin without a grid, then every 5 s, max 12: state flow paths, the local and every remote Console's 9 vars,
 *                   every CPlayer with the fields NetworkWaitForCarsLoaded tests (and its verdict), the session object's three flags that node
 *                   tests, FH2MP's own car select hold (carsel_sync.inc) and the texture placement fallback count.
 * Switched off with FH2MP_NO_STALLTRACE=1.
 *
 * Natives (forza_durango_release_final.exe 1.0.0.64, RVAs; [C] confirmed in the decompile and the disk exe, [I] inferred):
 *   +19281e0  StateMachine Instance::step [C] (also reached through the vtable slot 3 thunk +868350). Inst+0x28 = first and +0x30 = last element of the
 *             active path, element +0x08 = state definition, +0x18 = next element, state definition +0x88/+0x90 = vector of behavior pointers
 *             (the step calls their vtable slots 4 OnEnter, 6, 14 OnUpdate through it) [C]; a behavior's class name is read from its RTTI [C: checked
 *             on the exe for NetworkWaitForCarsLoaded and others]. Hooked to see the path after each step.
 *   +8d1be0   States::NetworkSetHasLoadedCarsForAllParticipants::Execute [C] (vtable slot 4; calls the local console's vtable +0xf8 setter).
 *   +34af7a0  CConsoleManager* [C]: +0x18 local CConsole, +0x20 std::map head of the remote consoles (node: +0x28 console pointer) [C: FUN_140291410].
 *             A console: +0x20/+0x24 u32 pair (+0x24 = the console id) [C: FUN_1417a5dc0], 9 SyncedVars from +0x88 in steps of 0x20 whose bool
 *             sits at +0x18 of each: RenderReady, SessionEntryFinalised, ParticipatingInRace, RaceInProgress, Ready, HasLoadedCarsForAllParticipants,
 *             HasGrid, AvailableArbitration, MatchTicketArbiter [C for the offsets and getters of vars 0-6; the order of the names is the
 *             registration order of Nerd::ConsoleSynced::RegisterReflection, I].
 *   +34b3e18  CPlayerManager* [C]: +0x48 = std::list sentinel of CPlayer* (node +0x10) [C: vfunc_4 and the loop of +8e0990]. CPlayer: +0x2c console
 *             id, +0x40 u32 > 0xfffffffd = a local player [C: FUN_1417a9610], +0x320 u64 (0 = no driver/car yet [I]), +0x340 ForzaPlayerChoices*
 *             whose bytes at +0x108 and +0x128 the node tests.
 *   +8e0990   States::NetworkWaitForCarsLoaded OnUpdate [C]: not hooked here (carsel_sync.inc hooks it). What it does, from the decompile: when the
 *             session object (*(g_NetworkGameManager+0xd0), g_NetworkGameManager = +34d6348) flag 0xf0 is set, it counts players that are not local,
 *             have choices+0x128 == 0, a console with ParticipatingInRace set, and (choices+0x108 == 1 or CPlayer+0x320 == 0); no such player =
 *             Loaded transition. With the flag clear it waits while flag 0xe0 is set and flag 0x100 is not. The snapshot repeats that rule [I]
 *             and reads the three flags by decoding the plain getter functions behind the session's vtable (no call).
 * Not found: the car loader's own state (which car file is pending), the SessionHostSynced vars (RaceParticipantsSelected), and what the three
 * session flags mean; a stall whose cause is in them shows as "the node waits" with the numbers above, not as a named reason. */
#ifdef FH2MP_DIAG_BUILD

#define STL_RVA_STEP 0x19281e0u
#define STL_RVA_SET_LOADED 0x8d1be0u
#define STL_RVA_CONSOLE_MGR 0x34af7a0u
#define STL_RVA_PLAYER_MGR 0x34b3e18u
#define STL_RVA_NET_MGR 0x34d6348u
#define STL_QUIET_MS 10000u            /* no grid this long after the begin: the first snapshot */
#define STL_SNAP_EVERY_MS 5000u
#define STL_SNAP_MAX 12
#define STL_TIMELINE_MAX 90            /* timeline lines per event */
#define STL_GIVEUP_MS 300000u          /* the trace of one event ends after this long */
#define STL_GRID_QUIET_MS 8000u        /* the grid phase (after the starting grid message): the first snapshot when RenderReady is still not set this long after it */
#define STL_GRID_GIVEUP_MS 240000u     /* the grid phase ends after this long (the server's own backstop is 240 s) */
#define STL_SILENT_MS 3000u            /* the state flow was not stepped this long while an event is traced */
#define STL_COOLDOWN_MS 15000u         /* after an event's trace ended, a state flow marker alone does not start another this soon */
#define STL_INST_MAX 12
#define STL_RING 32
#define STL_HIST 12
#define STL_EL_MAX 16
#define STL_EL_LEN 80
#define STL_CONSOLE_MAX 24
#define STL_PLAYER_MAX 24
#define STL_VARS 9

typedef ULONG_PTR (*stl_step_fn)(void *, void *, void *, void *);
typedef void (*stl_exec_fn)(void *);

static stl_step_fn g_stl_step_orig;
static stl_exec_fn g_stl_exec_orig;
static unsigned char *g_stl_img;
static int g_stl_step_hooked;
static volatile DWORD g_stl_last_step;
static volatile LONG g_stl_steps;
static volatile LONG g_stl_marker_set;
static char g_stl_marker[STL_EL_LEN];
static SRWLOCK g_stl_lock = SRWLOCK_INIT;

static const char *const g_stl_var_name[STL_VARS] = {"RenderReady", "SessionEntryFinalised", "ParticipatingInRace", "RaceInProgress", "Ready",
                                                   "HasLoadedCarsForAllParticipants", "HasGrid", "AvailableArbitration", "MatchTicketArbiter"};
static const char g_stl_var_letter[STL_VARS] = {'R', 'S', 'P', 'I', 'Y', 'L', 'G', 'A', 'M'};

/* ---- text buffer -------------------------------------------------------------------------------------------------------------------- */
typedef struct { char s[900]; int n; } stl_buf;

static void stl_s(stl_buf *b, const char *t)
{
    while (*t && b->n < (int)sizeof b->s - 1) b->s[b->n++] = *t++;
    b->s[b->n] = 0;
}

static void stl_u(stl_buf *b, unsigned long long v)
{
    char t[24];
    int i = 23;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    stl_s(b, t + i);
}

static void stl_h(stl_buf *b, unsigned long long v)
{
    char t[24];
    int i = 23;
    t[i] = 0;
    do {
        t[--i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v);
    stl_s(b, "0x");
    stl_s(b, t + i);
}

/* "diag: HH:MM:SS.mmm UTC stall: <body>" */
static void stl_emit(const stl_buf *body)
{
    char utc[32], line[1000];
    int n = 0;
    const char *p;

    grid_utc(utc);
    for (p = "diag: "; *p && n < 990; p++) line[n++] = *p;
    for (p = utc; *p && n < 990; p++) line[n++] = *p;
    for (p = " stall: "; *p && n < 990; p++) line[n++] = *p;
    for (p = body->s; *p && n < 999; p++) line[n++] = *p;
    line[n] = 0;
    grid_line(line);
}

static void stl_say(const char *t)
{
    stl_buf b;
    b.n = 0;
    b.s[0] = 0;
    stl_s(&b, t);
    stl_emit(&b);
}

/* ---- safe reads --------------------------------------------------------------------------------------------------------------------- */
static int stl_read(ULONG_PTR a, void *out, SIZE_T n)
{
    return a > 0x10000 && a < 0x00007ffffffe0000ull && read_game_memory((const void *)a, out, n);
}

static int stl_u64(ULONG_PTR a, ULONG_PTR *v)
{
    unsigned long long t;
    if (!stl_read(a, &t, 8)) return 0;
    *v = (ULONG_PTR)t;
    return 1;
}

static int stl_u32(ULONG_PTR a, unsigned *v) { return stl_read(a, v, 4); }

static int stl_in_img(ULONG_PTR p) { return g_stl_img && p - (ULONG_PTR)g_stl_img < FH2_SIZE_OF_IMAGE; }

static int stl_cstr(ULONG_PTR a, char *out, int cap)
{
    int n = 0;
    while (n < cap - 1) {
        char chunk[16];
        int i, got = 16;
        if (!stl_read(a + (ULONG_PTR)n, chunk, 16)) {
            got = 0;
            for (i = 0; i < 16; i++) {
                if (!stl_read(a + (ULONG_PTR)n + (ULONG_PTR)i, chunk + i, 1)) break;
                got++;
            }
            if (!got) break;
        }
        for (i = 0; i < got && n < cap - 1; i++) {
            if (!chunk[i]) {
                out[n] = 0;
                return n;
            }
            out[n++] = chunk[i];
        }
        if (got < 16) break;
    }
    out[n] = 0;
    return n;
}

/* The class name of an object with a virtual table, from the MSVC RTTI (vtable[-1] = complete object locator, +12 = type descriptor RVA, name
 * at +16 as ".?AVName@Scope@@"): "NetworkWaitForCarsLoaded" for States::, "Scope::Name" for other scopes. 0 when anything does not read. */
static struct { volatile ULONG_PTR vt; char name[48]; } g_stl_cn[32];

static int stl_name_of_vtable(ULONG_PTR vt, char *out, int cap)
{
    ULONG_PTR col;
    unsigned td;
    char raw[96], tok[3][40];
    int i, n, t = 0, k = 0;

    for (i = 0; i < 32; i++) {
        if (g_stl_cn[i].vt == vt) {
            for (n = 0; g_stl_cn[i].name[n] && n < cap - 1; n++) out[n] = g_stl_cn[i].name[n];
            out[n] = 0;
            return n > 0;
        }
    }
    if (!stl_in_img(vt) || !stl_u64(vt - 8, &col) || !stl_in_img(col) || !stl_u32(col + 12, &td) || td < 0x1000 || td >= FH2_SIZE_OF_IMAGE) return 0;
    if (!stl_cstr((ULONG_PTR)g_stl_img + td + 16, raw, sizeof raw) || raw[0] != '.' || raw[1] != '?' || raw[2] != 'A') return 0;
    for (i = 4; raw[i] && t < 3; i++) {
        if (raw[i] == '@') {
            tok[t][k] = 0;
            if (!k) break;
            t++;
            k = 0;
        } else if (k < 39) {
            tok[t][k++] = raw[i];
        }
    }
    if (t == 0) return 0;
    n = 0;
    if (t >= 2 && lstrcmpA(tok[1], "States") != 0) {
        for (i = 0; tok[1][i] && n < cap - 3; i++) out[n++] = tok[1][i];
        out[n++] = ':';
        out[n++] = ':';
    }
    for (i = 0; tok[0][i] && n < cap - 1; i++) out[n++] = tok[0][i];
    out[n] = 0;
    {
        static volatile LONG next;
        int slot = (int)((unsigned)InterlockedIncrement(&next) % 32u);
        g_stl_cn[slot].vt = 0;
        for (i = 0; i < n && i < 47; i++) g_stl_cn[slot].name[i] = out[i];
        g_stl_cn[slot].name[i < 47 ? i : 47] = 0;
        g_stl_cn[slot].vt = vt;
    }
    return n > 0;
}

static int stl_class_of(ULONG_PTR obj, char *out, int cap)
{
    ULONG_PTR vt;
    return stl_u64(obj, &vt) && stl_name_of_vtable(vt, out, cap);
}

/* ---- the state flow path ------------------------------------------------------------------------------------------------------------- */
typedef struct { int n; char el[STL_EL_MAX][STL_EL_LEN]; } stl_path;

/* The active path of one state machine instance: one element per active state, each the class names of the state's behaviors joined by "+".
 * Only the last STL_EL_MAX elements are kept. 0 when the instance does not read as a state machine. */
static int stl_get_path(ULONG_PTR inst, stl_path *p)
{
    ULONG_PTR e, leaf;
    int k;

    p->n = 0;
    if (!stl_u64(inst + 0x28, &e) || !stl_u64(inst + 0x30, &leaf) || !e || !leaf) return 0;
    for (k = 0; e && k < 40; k++) {
        ULONG_PTR def, b0, b1, next;
        char *dst;
        int cnt, i, len = 0;

        if (!stl_u64(e + 8, &def) || !def || !stl_u64(def + 0x88, &b0) || !stl_u64(def + 0x90, &b1) || b1 < b0 || b1 - b0 > 8 * 16) break;
        if (p->n == STL_EL_MAX) {
            for (i = 1; i < STL_EL_MAX; i++) memcpy(p->el[i - 1], p->el[i], STL_EL_LEN);
            p->n--;
        }
        dst = p->el[p->n++];
        dst[0] = 0;
        cnt = (int)((b1 - b0) / 8);
        for (i = 0; i < cnt; i++) {
            ULONG_PTR bo;
            char nm[48];
            int j;
            if (!stl_u64(b0 + 8 * (ULONG_PTR)i, &bo) || !stl_class_of(bo, nm, sizeof nm)) lstrcpyA(nm, "?");
            if (i && len < STL_EL_LEN - 2) dst[len++] = '+';
            for (j = 0; nm[j] && len < STL_EL_LEN - 1; j++) dst[len++] = nm[j];
            dst[len] = 0;
        }
        if (!len) lstrcpyA(dst, "(no behavior)");
        if (e == leaf) return 1;
        if (!stl_u64(e + 0x18, &next)) break;
        e = next;
    }
    return p->n > 0;
}

/* the last `max_el` elements, "A > B > C" */
static void stl_join(const stl_path *p, int max_el, stl_buf *b)
{
    int from = p->n > max_el ? p->n - max_el : 0, i;
    if (from > 0) stl_s(b, "... > ");
    for (i = from; i < p->n; i++) {
        if (i > from) stl_s(b, " > ");
        stl_s(b, p->el[i]);
    }
}

static int stl_has(const char *s, const char *pat)
{
    int i, k;
    for (i = 0; s[i]; i++) {
        for (k = 0; pat[k] && s[i + k] == pat[k]; k++) {
        }
        if (!pat[k]) return 1;
    }
    return 0;
}

/* state flow nodes that only an event's setup passes through (a client the server selected but whose Console.ParticipatingInRace did not change
 * locally still begins a trace when its state flow gets here) */
static int stl_is_marker(const char *el)
{
    return stl_has(el, "NetworkWaitForCarsLoaded") || stl_has(el, "NetworkSelectDefaultCar") || stl_has(el, "NetworkIsPlayerCarValidForEvent");
}

typedef struct {
    ULONG_PTR inst, head, leaf, def;
    volatile LONG steps;
    DWORD changed;
    char sig[400];
} stl_inst;

typedef struct { DWORD tick; WORD h, m, s, ms; int slot; char text[200]; } stl_ring_ent;

static stl_inst g_stl_inst[STL_INST_MAX];
static int g_stl_ninst;
static int g_stl_extra_inst;                 /* instance slots that were given to a newer instance */
static stl_ring_ent g_stl_ring[STL_RING];
static unsigned g_stl_ring_n;

/* ---- the event ------------------------------------------------------------------------------------------------------------------ */
static struct {
    int active, snaps, timeline, timeline_dropped, limit_told, silent_told, ever_ended, grid_phase;
    DWORD tick0, next_snap, grid_base, ended_at, grid_at;
    LONG place_base;
    LONG steps_at_snap;
    char cause[96];
} g_stl_ev;
static unsigned char g_stl_vars[STL_VARS];
static int g_stl_vars_valid;
static unsigned g_stl_local_id;
static HANDLE g_stl_thread;

/* "+1234 ms" of the active event, or 0 when none (so the caller leaves the line out). */
static int stl_event_ms(DWORD now, DWORD *ms)
{
    int active;
    AcquireSRWLockShared(&g_stl_lock);
    active = g_stl_ev.active;
    *ms = now - g_stl_ev.tick0;
    ReleaseSRWLockShared(&g_stl_lock);
    return active;
}

/* A timeline line while an event is traced (at most STL_TIMELINE_MAX, then one count). */
static void stl_timeline(const stl_buf *what)
{
    DWORD ms = 0;
    stl_buf b;
    int ok = 0;

    AcquireSRWLockExclusive(&g_stl_lock);
    if (g_stl_ev.active) {
        ms = GetTickCount() - g_stl_ev.tick0;
        if (g_stl_ev.timeline < STL_TIMELINE_MAX) {
            g_stl_ev.timeline++;
            ok = 1;
        } else {
            g_stl_ev.timeline_dropped++;
        }
    }
    ReleaseSRWLockExclusive(&g_stl_lock);
    if (!ok) return;
    b.n = 0;
    b.s[0] = 0;
    stl_s(&b, "+");
    stl_u(&b, ms);
    stl_s(&b, " ms ");
    stl_s(&b, what->s);
    stl_emit(&b);
}

/* ---- consoles and players ------------------------------------------------------------------------------------------------------------ */
typedef struct { ULONG_PTR addr; unsigned kind, id; unsigned char v[STL_VARS]; int ok; } stl_console;

static int stl_read_console(ULONG_PTR c, stl_console *o)
{
    unsigned char blk[0x20 * 8 + 1];
    unsigned meta[2];
    int i;

    o->ok = 0;
    o->addr = c;
    if (!stl_read(c + 0x20, meta, 8) || !stl_read(c + 0xa0, blk, sizeof blk)) return 0;
    o->kind = meta[0];
    o->id = meta[1];
    for (i = 0; i < STL_VARS; i++) o->v[i] = blk[0x20 * i];
    for (i = 0; i < 7; i++) {
        if (o->v[i] > 1) return 0;
    }
    if (o->v[7] > 2 || o->v[8] > 2) return 0;
    o->ok = 1;
    return 1;
}

static int stl_console_mgr(ULONG_PTR *cm)
{
    ULONG_PTR p;
    if (!g_stl_img || !stl_u64((ULONG_PTR)g_stl_img + STL_RVA_CONSOLE_MGR, &p) || p < 0x10000) return 0;
    *cm = p;
    return 1;
}

static int stl_local_console(stl_console *lc)
{
    ULONG_PTR cm, c;
    return stl_console_mgr(&cm) && stl_u64(cm + 0x18, &c) && c > 0x10000 && stl_read_console(c, lc);
}

/* The remote consoles: a depth-first walk of the std::map tree (node: +0 left, +8 parent, +0x10 right, +0x19 is-nil, value pointer +0x28). */
static int stl_remote_consoles(stl_console *out, int max)
{
    ULONG_PTR cm, head, root, stack[64];
    int sp = 0, n = 0, visited = 0;

    if (!stl_console_mgr(&cm) || !stl_u64(cm + 0x20, &head) || head < 0x10000 || !stl_u64(head + 8, &root)) return 0;
    if (root == head) return 0;
    stack[sp++] = root;
    while (sp > 0 && n < max && visited++ < 96) {
        ULONG_PTR node = stack[--sp], l, r, c;
        unsigned char nil;
        if (node < 0x10000 || node == head || !stl_read(node + 0x19, &nil, 1) || nil) continue;
        if (stl_u64(node + 0x28, &c) && c > 0x10000) {
            stl_console tmp;
            memset(&tmp, 0, sizeof tmp);
            stl_read_console(c, &tmp);
            out[n++] = tmp;
        }
        if (stl_u64(node + 0x10, &r) && sp < 63) stack[sp++] = r;
        if (stl_u64(node, &l) && sp < 63) stack[sp++] = l;
    }
    return n;
}

typedef struct {
    ULONG_PTR addr, choices;
    unsigned id, local_field;
    unsigned long long driver;
    unsigned char c108, c128;
    int choices_ok, local, console_found, participating, counted;
} stl_player;

static int stl_players(stl_player *out, int max, const stl_console *lc, const stl_console *rc, int nrc)
{
    ULONG_PTR pm, sent, node;
    int n = 0, guard = 0;

    if (!g_stl_img || !stl_u64((ULONG_PTR)g_stl_img + STL_RVA_PLAYER_MGR, &pm) || pm < 0x10000 || !stl_u64(pm + 0x48, &sent) || sent < 0x10000 ||
        !stl_u64(sent, &node)) {
        return -1;
    }
    while (node && node != sent && n < max && guard++ < 64) {
        ULONG_PTR p, next;
        stl_player *o = &out[n];
        unsigned long long drv;
        int i;

        if (!stl_u64(node + 0x10, &p) || !stl_u64(node, &next) || p < 0x10000) break;
        memset(o, 0, sizeof *o);
        o->addr = p;
        if (!stl_u32(p + 0x2c, &o->id) || !stl_u32(p + 0x40, &o->local_field)) {
            node = next;
            n++;
            continue;
        }
        if (stl_read(p + 0x320, &drv, 8)) o->driver = drv;
        o->local = o->local_field > 0xfffffffdu;
        if (stl_u64(p + 0x340, &o->choices) && o->choices > 0x10000 && stl_read(o->choices + 0x108, &o->c108, 1) && stl_read(o->choices + 0x128, &o->c128, 1)) {
            o->choices_ok = 1;
        }
        if (o->local) {
            o->console_found = lc != NULL && lc->ok;
            o->participating = o->console_found && lc->v[2];
        } else {
            for (i = 0; i < nrc; i++) {
                if (rc[i].ok && rc[i].id == o->id) {
                    o->console_found = 1;
                    o->participating = rc[i].v[2];
                    break;
                }
            }
        }
        /* the rule of NetworkWaitForCarsLoaded (OnUpdate +8e0990) [I] */
        o->counted = o->choices_ok && o->c128 == 0 && !o->local && o->console_found && o->participating && (o->c108 == 1 || o->driver == 0);
        n++;
        node = next;
    }
    return n;
}

/* ---- the session object's flags (read by decoding the getters, never called) ------------------------------------------------------------- */
/* movzx eax,byte [rcx+d] ; ret   (0f b6 81 d32 c3 | 0f b6 41 d8 c3 | 0f b6 01 c3)   mov al,[rcx+d] ; ret   (8a 81 d32 c3 | 8a 41 d8 c3 | 8a 01 c3).
 * Follows up to two `jmp rel32`. Returns 1 and the displacement; 0 when the function is anything else. */
static int stl_decode_getter(ULONG_PTR fn, unsigned *disp)
{
    unsigned char c[16];
    int hop;

    for (hop = 0; hop < 3; hop++) {
        int rel;
        if (!stl_in_img(fn) || !stl_read(fn, c, 16)) return 0;
        if (c[0] == 0xe9) {
            memcpy(&rel, c + 1, 4);
            fn = fn + 5 + (ULONG_PTR)(LONG_PTR)rel;
            continue;
        }
        break;
    }
    if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x81 && c[7] == 0xc3) {
        memcpy(disp, c + 3, 4);
        return 1;
    }
    if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x41 && c[4] == 0xc3) {
        *disp = c[3];
        return 1;
    }
    if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x01 && c[3] == 0xc3) {
        *disp = 0;
        return 1;
    }
    if (c[0] == 0x8a && c[1] == 0x81 && c[6] == 0xc3) {
        memcpy(disp, c + 2, 4);
        return 1;
    }
    if (c[0] == 0x8a && c[1] == 0x41 && c[3] == 0xc3) {
        *disp = c[2];
        return 1;
    }
    if (c[0] == 0x8a && c[1] == 0x01 && c[2] == 0xc3) {
        *disp = 0;
        return 1;
    }
    return 0;
}

/* One flag of the session object: the vtable entry `slot_off`; -1 not readable, -2 not a plain getter, else the byte. */
static int stl_session_flag(ULONG_PTR sess, ULONG_PTR vt, unsigned slot_off, ULONG_PTR *fn_out)
{
    ULONG_PTR fn;
    unsigned disp;
    unsigned char v;

    *fn_out = 0;
    if (!stl_u64(vt + slot_off, &fn)) return -1;
    *fn_out = fn;
    if (!stl_decode_getter(fn, &disp)) return -2;
    if (!stl_read(sess + disp, &v, 1)) return -1;
    return v;
}

static void stl_flag_text(stl_buf *b, const char *name, int v, ULONG_PTR fn)
{
    stl_s(b, name);
    stl_s(b, "=");
    if (v >= 0) {
        stl_u(b, (unsigned)v);
    } else if (v == -2) {
        stl_s(b, "(not a plain getter, game+");
        stl_h(b, (unsigned long long)(fn - (ULONG_PTR)g_stl_img));
        stl_s(b, ")");
    } else {
        stl_s(b, "(unreadable)");
    }
    stl_s(b, " ");
}

/* ---- snapshot ------------------------------------------------------------------------------------------------------------------------ */
static void stl_console_vars(stl_buf *b, const stl_console *c, int letters)
{
    int i;
    for (i = 0; i < STL_VARS; i++) {
        if (letters) {
            char t[3];
            t[0] = g_stl_var_letter[i];
            t[1] = (char)('0' + c->v[i]);
            t[2] = 0;
            stl_s(b, t);
            stl_s(b, " ");
        } else {
            stl_s(b, g_stl_var_name[i]);
            stl_s(b, "=");
            stl_u(b, c->v[i]);
            stl_s(b, " ");
        }
    }
}

static void stl_snapshot(int n, DWORD ms)
{
    stl_buf b;
    stl_console lc, rc[STL_CONSOLE_MAX];
    stl_player pl[STL_PLAYER_MAX];
    stl_inst copy[STL_INST_MAX];
    int nrc = 0, npl, i, lc_ok, ncopy, loading = 0, sess_known = 0, sess_flag_f0 = -9, sess_e0 = -9, sess_100 = -9;
    LONG steps, place;
    DWORD last_step = g_stl_last_step, now = GetTickCount();
    unsigned long long steps_since;

    AcquireSRWLockShared(&g_stl_lock);
    ncopy = g_stl_ninst;
    memcpy(copy, g_stl_inst, sizeof copy);
    steps_since = (unsigned long long)(unsigned)(g_stl_steps - g_stl_ev.steps_at_snap);
    ReleaseSRWLockShared(&g_stl_lock);
    AcquireSRWLockExclusive(&g_stl_lock);
    g_stl_ev.steps_at_snap = g_stl_steps;
    ReleaseSRWLockExclusive(&g_stl_lock);
    steps = g_stl_steps;
    place = g_place_logged;

    b.n = 0; b.s[0] = 0;
    stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " +"); stl_u(&b, ms);
    if (g_stl_ev.grid_phase) {
        stl_s(&b, " ms after the event began, +"); stl_u(&b, (unsigned)(now - g_stl_ev.grid_at));
        stl_s(&b, " ms after the starting grid message, local RenderReady not set yet; state flow ");
    } else {
        stl_s(&b, " ms after the event began, no starting grid message yet; state flow ");
    }
    if (g_stl_step_hooked) {
        stl_s(&b, "last stepped "); stl_u(&b, last_step ? (unsigned)(now - last_step) : 0); stl_s(&b, " ms ago, ");
        stl_u(&b, (unsigned)steps); stl_s(&b, " steps in all, "); stl_u(&b, steps_since); stl_s(&b, " since the last snapshot");
    } else {
        stl_s(&b, "step hook not installed");
    }
    if (g_stl_extra_inst) { stl_s(&b, "; "); stl_u(&b, (unsigned)g_stl_extra_inst); stl_s(&b, " state machine slot(s) were reused for newer instances"); }
    stl_s(&b, "; texture placement fallbacks so far "); stl_u(&b, (unsigned)place);
    stl_s(&b, " ("); stl_u(&b, (unsigned)(place - g_stl_ev.place_base)); stl_s(&b, " since the event began)");
    stl_emit(&b);

    /* the state flow: the instances that changed most recently first */
    for (i = 0; i < ncopy && i < 4; i++) {
        int best = i, j;
        stl_inst t;
        for (j = i + 1; j < ncopy; j++) {
            if ((int)(copy[j].changed - copy[best].changed) > 0) best = j;
        }
        t = copy[i]; copy[i] = copy[best]; copy[best] = t;
        b.n = 0; b.s[0] = 0;
        stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " state flow ");
        stl_h(&b, copy[i].inst); stl_s(&b, " unchanged for "); stl_u(&b, now - copy[i].changed); stl_s(&b, " ms, "); stl_u(&b, (unsigned)copy[i].steps);
        stl_s(&b, " steps: "); stl_s(&b, copy[i].sig);
        stl_emit(&b);
    }
    if (ncopy == 0) {
        b.n = 0; b.s[0] = 0;
        stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " state flow: no instance was stepped since the shim attached");
        stl_emit(&b);
    }

    lc_ok = stl_local_console(&lc);
    b.n = 0; b.s[0] = 0;
    stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " local console ");
    if (lc_ok) {
        stl_h(&b, lc.id); stl_s(&b, " "); stl_console_vars(&b, &lc, 0);
    } else {
        stl_s(&b, "not readable (the console manager or the local console pointer did not validate)");
    }
    stl_emit(&b);

    nrc = stl_remote_consoles(rc, STL_CONSOLE_MAX);
    if (nrc == 0) {
        stl_buf z;
        z.n = 0; z.s[0] = 0;
        stl_s(&z, "snapshot #"); stl_u(&z, (unsigned)n); stl_s(&z, " remote consoles: none known");
        stl_emit(&z);
    }
    {
        int from = 0;
        while (from < nrc) {
            b.n = 0; b.s[0] = 0;
            stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " remote consoles "); stl_u(&b, (unsigned)(from + 1)); stl_s(&b, "-");
            for (i = from; i < nrc && i < from + 5; i++) {
                if (i > from) stl_s(&b, " | ");
                stl_h(&b, rc[i].id); stl_s(&b, ": ");
                if (rc[i].ok) stl_console_vars(&b, &rc[i], 1); else stl_s(&b, "invalid vars");
            }
            stl_emit(&b);
            from = i;
        }
    }

    npl = stl_players(pl, STL_PLAYER_MAX, lc_ok ? &lc : NULL, rc, nrc);
    if (npl < 0) {
        b.n = 0; b.s[0] = 0;
        stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " players: the player manager or its list did not validate");
        stl_emit(&b);
    } else {
        int from = 0;
        for (i = 0; i < npl; i++) loading += pl[i].counted;
        if (n == 1 || n == 3) {          /* the server's log gets the gist: where the state flow sits, who the game still waits for, the local console */
            stl_buf r;
            int listed = 0;
            r.n = 0; r.s[0] = 0;
            stl_s(&r, "stall snapshot #"); stl_u(&r, (unsigned)n); stl_s(&r, " +"); stl_u(&r, ms); stl_s(&r, " ms: still loading "); stl_u(&r, (unsigned)loading); stl_s(&r, " of "); stl_u(&r, (unsigned)npl);
            for (i = 0; i < npl && listed < 4; i++) {
                if (pl[i].counted) { stl_s(&r, listed ? "," : " ["); stl_h(&r, pl[i].id); listed++; }
            }
            if (listed) stl_s(&r, "]");
            stl_s(&r, "; flow "); stl_s(&r, ncopy > 0 ? copy[0].sig : "none");
            if (lc_ok) { stl_s(&r, "; local "); stl_console_vars(&r, &lc, 0); }
            carsel_report_post(CS_RPT_STALL, (unsigned)n, r.s);
        }
        while (from < npl) {
            b.n = 0; b.s[0] = 0;
            stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " players "); stl_u(&b, (unsigned)(from + 1)); stl_s(&b, "-");
            for (i = from; i < npl && i < from + 4; i++) {
                if (i > from) stl_s(&b, " | ");
                stl_s(&b, "console "); stl_h(&b, pl[i].id); stl_s(&b, pl[i].local ? " local" : " remote");
                stl_s(&b, " driver="); stl_h(&b, pl[i].driver);
                if (pl[i].choices_ok) { stl_s(&b, " choices+108="); stl_u(&b, pl[i].c108); stl_s(&b, " +128="); stl_u(&b, pl[i].c128); }
                else stl_s(&b, " choices unreadable");
                stl_s(&b, pl[i].console_found ? (pl[i].participating ? " participating" : " not participating") : " no console");
                if (pl[i].counted) stl_s(&b, " STILL LOADING");
            }
            stl_emit(&b);
            from = i;
        }
        if (npl == 0) stl_say("players: the list is empty");
    }

    /* the session object NetworkWaitForCarsLoaded asks first */
    {
        ULONG_PTR mgr, sess, vt, f0 = 0, fe0 = 0, f100 = 0;
        char cls[48];

        b.n = 0; b.s[0] = 0;
        stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " session object ");
        if (g_stl_img && stl_u64((ULONG_PTR)g_stl_img + STL_RVA_NET_MGR, &mgr) && mgr > 0x10000 && stl_u64(mgr + 0xd0, &sess) && sess > 0x10000 && stl_u64(sess, &vt) &&
            stl_in_img(vt)) {
            sess_flag_f0 = stl_session_flag(sess, vt, 0xf0, &f0);
            sess_e0 = stl_session_flag(sess, vt, 0xe0, &fe0);
            sess_100 = stl_session_flag(sess, vt, 0x100, &f100);
            if (!stl_name_of_vtable(vt, cls, sizeof cls)) lstrcpyA(cls, "?");
            stl_h(&b, sess); stl_s(&b, " ("); stl_s(&b, cls); stl_s(&b, "): ");
            stl_flag_text(&b, "flag0xf0", sess_flag_f0, f0);
            stl_flag_text(&b, "flag0xe0", sess_e0, fe0);
            stl_flag_text(&b, "flag0x100", sess_100, f100);
            sess_known = sess_flag_f0 >= 0 && sess_e0 >= 0 && sess_100 >= 0;
        } else {
            stl_s(&b, "not readable (network manager +0xd0)");
        }
        stl_emit(&b);
    }

    b.n = 0; b.s[0] = 0;
    stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " NetworkWaitForCarsLoaded by the game's rule [inferred]: ");
    if (sess_known && sess_flag_f0 == 0) {
        stl_s(&b, sess_e0 && !sess_100 ? "session flags 0xe0 set and 0x100 clear: the node keeps waiting for the session" : "session flags let the node fire its Loaded transition");
    } else if (npl >= 0 && (sess_flag_f0 == 1 || !sess_known)) {
        if (loading) {
            stl_u(&b, (unsigned)loading); stl_s(&b, " participant(s) still loading (STILL LOADING above): the node waits");
        } else {
            stl_s(&b, "no participant counts as loading: the node fires its Loaded transition");
        }
        if (!sess_known) stl_s(&b, " (the session flags were not all readable, so the players branch is assumed)");
    } else {
        stl_s(&b, "not decidable from what was readable");
    }
    stl_emit(&b);

#ifdef CS_UPDATE_RVA
    {
        /* FH2MP's own hold of that node (carsel_sync.inc): it skips the game's update while the server says a participant is still in the car select */
        int valid = 0, hold = 0;
        ULONGLONG tnow = CARSEL_NOW(), armed = (ULONGLONG)g_cs_armed_at, at = 0;
        unsigned remaining = 0;

        if (TryAcquireSRWLockShared(&g_cs_lock)) {
            valid = g_cs.valid;
            hold = g_cs.hold;
            at = g_cs.at;
            remaining = g_cs.remaining;
            ReleaseSRWLockShared(&g_cs_lock);
        }
        b.n = 0; b.s[0] = 0;
        stl_s(&b, "snapshot #"); stl_u(&b, (unsigned)n); stl_s(&b, " car select hold (FH2MP): ");
        stl_s(&b, g_cs_enabled ? "enabled" : "not enabled");
        stl_s(&b, ", the hooked NetworkWaitForCarsLoaded update ");
        if (armed) { stl_s(&b, "last ran "); stl_u(&b, (unsigned long long)(tnow - armed)); stl_s(&b, " ms ago"); } else stl_s(&b, "has not run");
        if (valid) {
            stl_s(&b, "; the server's last answer said ");
            stl_s(&b, hold ? "HOLD" : "release");
            stl_s(&b, " ("); stl_u(&b, (unsigned long long)(tnow - at)); stl_s(&b, " ms old, "); stl_u(&b, remaining); stl_s(&b, " ms to the group deadline)");
        } else {
            stl_s(&b, "; no server answer read");
        }
        stl_emit(&b);
    }
#endif
}

/* ---- event begin / end ------------------------------------------------------------------------------------------------------------- */
static void stl_begin(DWORD now, const char *cause, const stl_console *lc)
{
    stl_ring_ent hist[STL_HIST];
    int nh = 0, i;
    unsigned total;
    stl_buf b;
    stl_console rc[STL_CONSOLE_MAX];
    int nrc;

    AcquireSRWLockExclusive(&g_stl_lock);
    memset(&g_stl_ev, 0, sizeof g_stl_ev);
    g_stl_ev.active = 1;
    g_stl_ev.tick0 = now;
    g_stl_ev.next_snap = now + STL_QUIET_MS;
    g_stl_ev.grid_base = g_grid_tick0;
    g_stl_ev.place_base = g_place_logged;
    g_stl_ev.steps_at_snap = g_stl_steps;
    for (i = 0; cause[i] && i < (int)sizeof g_stl_ev.cause - 1; i++) g_stl_ev.cause[i] = cause[i];
    total = g_stl_ring_n;
    {
        unsigned have = total < STL_RING ? total : STL_RING, take = have < STL_HIST ? have : STL_HIST, k;
        for (k = take; k > 0; k--) hist[nh++] = g_stl_ring[(total - k) % STL_RING];
    }
    ReleaseSRWLockExclusive(&g_stl_lock);

    b.n = 0; b.s[0] = 0;
    stl_s(&b, "event begin: "); stl_s(&b, cause);
    stl_s(&b, "; local console ");
    if (lc && lc->ok) { stl_h(&b, lc->id); stl_s(&b, " "); stl_console_vars(&b, lc, 0); } else stl_s(&b, "not readable");
    nrc = stl_remote_consoles(rc, STL_CONSOLE_MAX);
    stl_s(&b, "; "); stl_u(&b, (unsigned)nrc); stl_s(&b, " remote console(s) (snapshot letters: R RenderReady, S SessionEntryFinalised, P ParticipatingInRace, I RaceInProgress, Y Ready, L HasLoadedCarsForAllParticipants, G HasGrid, A/M the two arbitration enums). A snapshot follows every 5 s from +10 s unless the starting grid message arrives first");
    stl_emit(&b);
    for (i = 0; i < nh; i++) {
        char t[48];
        b.n = 0; b.s[0] = 0;
        wsprintfA(t, "%02u:%02u:%02u.%03u UTC", (unsigned)hist[i].h, (unsigned)hist[i].m, (unsigned)hist[i].s, (unsigned)hist[i].ms);
        stl_s(&b, "history, before the event: "); stl_s(&b, t); stl_s(&b, " "); stl_s(&b, hist[i].text);
        stl_emit(&b);
    }
}

static void stl_end(const char *why, int arrived)
{
    stl_buf b;
    DWORD ms = 0;
    int snaps, dropped;

    AcquireSRWLockExclusive(&g_stl_lock);
    if (!g_stl_ev.active) {
        ReleaseSRWLockExclusive(&g_stl_lock);
        return;
    }
    ms = GetTickCount() - g_stl_ev.tick0;
    snaps = g_stl_ev.snaps;
    dropped = g_stl_ev.timeline_dropped;
    g_stl_ev.active = 0;
    g_stl_ev.ever_ended = 1;
    g_stl_ev.ended_at = GetTickCount();
    ReleaseSRWLockExclusive(&g_stl_lock);
    b.n = 0; b.s[0] = 0;
    stl_s(&b, arrived ? "event load finished: " : "event trace ended: ");
    stl_s(&b, why);
    stl_s(&b, " +"); stl_u(&b, ms); stl_s(&b, " ms after the begin, "); stl_u(&b, (unsigned)snaps); stl_s(&b, " snapshot(s) written");
    if (dropped) { stl_s(&b, ", "); stl_u(&b, (unsigned)dropped); stl_s(&b, " timeline line(s) over the limit of 90 were not written"); }
    stl_emit(&b);
    if (snaps > 0) carsel_report_post(CS_RPT_STALL_END, (unsigned)(ms / 1000), b.s);     /* only a trace that had stalled long enough for a snapshot */
}

/* One pass of the watcher (every 100 ms): the local console's vars, the event's begin, its end, the 10 s / 5 s snapshots. */
static void stl_tick(void)
{
    DWORD now = GetTickCount();
    stl_console lc;
    int have = stl_local_console(&lc), i;
    int active, marker;
    char marker_name[STL_EL_LEN];

    if (have) {
        g_stl_local_id = lc.id;
        if (g_stl_vars_valid) {
            for (i = 0; i < STL_VARS; i++) {
                if (lc.v[i] != g_stl_vars[i]) {
                    stl_buf b;
                    b.n = 0; b.s[0] = 0;
                    stl_s(&b, "local Console."); stl_s(&b, g_stl_var_name[i]); stl_s(&b, " "); stl_u(&b, g_stl_vars[i]); stl_s(&b, " -> "); stl_u(&b, lc.v[i]);
                    stl_timeline(&b);
                }
            }
        }
    }
    AcquireSRWLockShared(&g_stl_lock);
    active = g_stl_ev.active;
    ReleaseSRWLockShared(&g_stl_lock);

    marker = InterlockedExchange(&g_stl_marker_set, 0);
    marker_name[0] = 0;
    if (marker) lstrcpynA(marker_name, g_stl_marker, sizeof marker_name);

    if (!active) {
        char cause[STL_EL_LEN + 64];
        int begin = 0;
        if (have && g_stl_vars_valid && g_stl_vars[2] == 0 && lc.v[2] == 1) {
            lstrcpyA(cause, "local Console.ParticipatingInRace went 0 -> 1 (the server selected this client for the event)");
            begin = 1;
        } else if (marker && (!g_stl_ev.ever_ended || now - g_stl_ev.ended_at > STL_COOLDOWN_MS)) {
            lstrcpyA(cause, "the state flow entered ");
            lstrcatA(cause, marker_name);
            begin = 1;
        }
        if (have) {
            memcpy(g_stl_vars, lc.v, STL_VARS);
            g_stl_vars_valid = 1;
        }
        if (begin) stl_begin(now, cause, have ? &lc : NULL);
        return;
    }

    if (have && g_stl_vars_valid) {
        if (g_stl_vars[2] == 1 && lc.v[2] == 0) {
            memcpy(g_stl_vars, lc.v, STL_VARS);
            stl_end("local Console.ParticipatingInRace went 1 -> 0, this client left the event or it was cancelled", 0);
            return;
        }
        if (g_stl_vars[3] == 0 && lc.v[3] == 1) {
            memcpy(g_stl_vars, lc.v, STL_VARS);
            stl_end("local Console.RaceInProgress went 0 -> 1", 1);
            return;
        }
    }
    if (have) {
        memcpy(g_stl_vars, lc.v, STL_VARS);
        g_stl_vars_valid = 1;
    }

    if (!g_stl_ev.grid_phase && g_grid_tick0 != g_stl_ev.grid_base) {
        /* The load is over, the grid phase begins: RenderReady comes only after the cars are placed and the scene is built. On prod 2026-11-01 a client took
         * 83 s for it and on 2026-11-06 82 s (others 20 to 24 s) with the game running at full speed, and the trace had stopped at the grid message. It goes on
         * until RenderReady is set (the grid_trace.inc hook or the local console var), the race starts, the event is left, or 240 s. */
        stl_buf b;
        g_stl_ev.grid_phase = 1;
        g_stl_ev.grid_at = now;
        g_stl_ev.snaps = 0;
        g_stl_ev.limit_told = 0;
        g_stl_ev.next_snap = now + STL_GRID_QUIET_MS;
        b.n = 0; b.s[0] = 0;
        stl_s(&b, "the starting grid message arrived +"); stl_u(&b, now - g_stl_ev.tick0);
        stl_s(&b, " ms after the begin; tracing the grid phase until RenderReady is set (a snapshot every 5 s from +8 s)");
        stl_emit(&b);
        return;
    }
    if (g_stl_ev.grid_phase) {
        if (g_grid_ready_value == 1 || (have && lc.v[0] == 1)) {
            stl_end("local RenderReady is set (the grid phase is over)", 1);
            return;
        }
        if (!g_grid_epoch) {
            stl_end("the race start message arrived before RenderReady was seen", 1);
            return;
        }
        if (now - g_stl_ev.grid_at > STL_GRID_GIVEUP_MS) {
            stl_end("giving up, RenderReady was not set in the grid phase", 0);
            return;
        }
    } else if (now - g_stl_ev.tick0 > STL_GIVEUP_MS) {
        stl_end("giving up, the starting grid message never arrived", 0);
        return;
    }
    if (g_stl_step_hooked) {
        DWORD last = g_stl_last_step;
        if (last && now - last > STL_SILENT_MS && !g_stl_ev.silent_told) {
            stl_buf b;
            g_stl_ev.silent_told = 1;
            b.n = 0; b.s[0] = 0;
            stl_s(&b, "the state flow has not been stepped for "); stl_u(&b, now - last); stl_s(&b, " ms (the game thread that runs it is blocked or not running frames)");
            stl_timeline(&b);
        } else if (last && now - last < 1000 && g_stl_ev.silent_told) {
            g_stl_ev.silent_told = 0;
            stl_say("the state flow is being stepped again");
        }
    }
    if ((int)(now - g_stl_ev.next_snap) >= 0) {
        if (g_stl_ev.snaps < STL_SNAP_MAX) {
            g_stl_ev.snaps++;
            g_stl_ev.next_snap = now + STL_SNAP_EVERY_MS;
            stl_snapshot(g_stl_ev.snaps, now - g_stl_ev.tick0);
        } else if (!g_stl_ev.limit_told) {
            g_stl_ev.limit_told = 1;
            stl_say("snapshot limit of 12 reached for this event; the timeline continues");
        }
    }
}

static DWORD WINAPI stl_watch(void *unused)
{
    (void)unused;
    for (;;) {
        Sleep(100);
        stl_tick();
    }
    return 0;
}

/* ---- the hooks ---------------------------------------------------------------------------------------------------------------------- */
/* After each step: has this instance's active path changed? Fast path: three pointers compared; the names are read only on a change. */
static void stl_observe(void *inst_p)
{
    ULONG_PTR inst = (ULONG_PTR)inst_p, head = 0, leaf = 0, def = 0;
    stl_inst *me = NULL;
    int i, idx = -1;
    stl_path path;
    stl_buf b, t;
    char sig[400];

    InterlockedIncrement(&g_stl_steps);
    g_stl_last_step = GetTickCount();
    for (i = 0; i < g_stl_ninst; i++) {
        if (g_stl_inst[i].inst == inst) {
            me = &g_stl_inst[i];
            idx = i;
            break;
        }
    }
    if (!me) {
        AcquireSRWLockExclusive(&g_stl_lock);
        for (i = 0; i < g_stl_ninst; i++) {
            if (g_stl_inst[i].inst == inst) { me = &g_stl_inst[i]; idx = i; break; }
        }
        if (!me) {
            DWORD now = GetTickCount(), worst = 0;
            if (g_stl_ninst < STL_INST_MAX) {
                idx = g_stl_ninst++;
            } else {                         /* full: the instance that changed longest ago makes room (a new one means the old one is gone) */
                idx = 0;
                for (i = 0; i < STL_INST_MAX; i++) {
                    if (now - g_stl_inst[i].changed >= worst) { worst = now - g_stl_inst[i].changed; idx = i; }
                }
                g_stl_extra_inst++;
            }
            me = &g_stl_inst[idx];
            memset(me, 0, sizeof *me);
            me->inst = inst;
            me->changed = now;
        }
        ReleaseSRWLockExclusive(&g_stl_lock);
    }
    InterlockedIncrement(&me->steps);
    if (!stl_u64(inst + 0x28, &head) || !stl_u64(inst + 0x30, &leaf)) return;
    if (leaf) stl_u64(leaf + 8, &def);
    if (head == me->head && leaf == me->leaf && def == me->def) return;
    me->head = head;
    me->leaf = leaf;
    me->def = def;
    if (!stl_get_path(inst, &path)) {
        sig[0] = 0;
        lstrcpyA(sig, "(no active state)");
    } else {
        b.n = 0; b.s[0] = 0;
        stl_join(&path, 8, &b);
        lstrcpynA(sig, b.s, sizeof sig);
    }
    if (!lstrcmpA(sig, me->sig)) return;
    {
        DWORD now = GetTickCount();
        SYSTEMTIME tm;
        stl_ring_ent *r;
        int is_marker = 0;

        GetSystemTime(&tm);
        for (i = 0; i < path.n; i++) {
            if (stl_is_marker(path.el[i])) {
                is_marker = 1;
                lstrcpynA(g_stl_marker, path.el[i], sizeof g_stl_marker);
            }
        }
        t.n = 0; t.s[0] = 0;
        stl_s(&t, "sm"); stl_u(&t, (unsigned)(idx + 1)); stl_s(&t, " "); stl_join(&path, 5, &t);
        AcquireSRWLockExclusive(&g_stl_lock);
        lstrcpyA(me->sig, sig);
        me->changed = now;
        r = &g_stl_ring[g_stl_ring_n++ % STL_RING];
        r->tick = now; r->h = tm.wHour; r->m = tm.wMinute; r->s = tm.wSecond; r->ms = tm.wMilliseconds; r->slot = idx;
        lstrcpynA(r->text, t.s, sizeof r->text);
        ReleaseSRWLockExclusive(&g_stl_lock);
        if (is_marker) InterlockedExchange(&g_stl_marker_set, 1);
        {
            stl_buf w;
            w.n = 0; w.s[0] = 0;
            stl_s(&w, "state flow "); stl_s(&w, t.s);
            stl_timeline(&w);
        }
    }
}

static ULONG_PTR stl_step_hook(void *inst, void *a2, void *a3, void *a4)
{
    ULONG_PTR r = g_stl_step_orig(inst, a2, a3, a4);
    stl_observe(inst);
    return r;
}

/* The state flow node that sets the local Console.HasLoadedCarsForAllParticipants (what the server waits for). */
static void stl_set_loaded_hook(void *node)
{
    stl_console before, after;
    int had_before = stl_local_console(&before);
    stl_buf b;
    int had_after;

    g_stl_exec_orig(node);
    had_after = stl_local_console(&after);
    b.n = 0; b.s[0] = 0;
    stl_s(&b, "node NetworkSetHasLoadedCarsForAllParticipants ran: local Console.HasLoadedCarsForAllParticipants ");
    if (had_before) stl_u(&b, before.v[5]); else stl_s(&b, "?");
    stl_s(&b, " -> ");
    if (had_after) stl_u(&b, after.v[5]); else stl_s(&b, "?");
    {
        DWORD ms;
        if (stl_event_ms(GetTickCount(), &ms)) {
            stl_buf w;
            w.n = 0; w.s[0] = 0;
            stl_s(&w, "+"); stl_u(&w, ms); stl_s(&w, " ms "); stl_s(&w, b.s);
            stl_emit(&w);
            return;
        }
    }
    stl_emit(&b);
}

static int patch_stall_trace(void)
{
    static const unsigned char step_p[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
    static const unsigned char exec_p[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8};
    unsigned char *img = find_fh2_image();
    int hooks = 0, ok_step = 0, ok_exec = 0;
    stl_buf b;

    if (!img) return 0;
    g_stl_img = img;
    if (code_is("diag: stall trace", "state machine step (Instance::step, game+0x19281e0)", img, STL_RVA_STEP, step_p, sizeof step_p) &&
        plat_hook("diag: stall state machine step", img + STL_RVA_STEP, (void *)stl_step_hook, (void **)&g_stl_step_orig)) {
        ok_step = 1;
        g_stl_step_hooked = 1;
        hooks++;
    }
    if (code_is("diag: stall trace", "NetworkSetHasLoadedCarsForAllParticipants::Execute (game+0x8d1be0)", img, STL_RVA_SET_LOADED, exec_p, sizeof exec_p) &&
        plat_hook("diag: stall HasLoaded node", img + STL_RVA_SET_LOADED, (void *)stl_set_loaded_hook, (void **)&g_stl_exec_orig)) {
        ok_exec = 1;
        hooks++;
    }
#ifndef STL_TEST_NO_WATCHER
    g_stl_thread = CreateThread(NULL, 0, stl_watch, NULL, 0, NULL);
    if (g_stl_thread) CloseHandle(g_stl_thread);
#else
    g_stl_thread = (HANDLE)1;           /* tests step the watcher by hand */
#endif
    b.n = 0; b.s[0] = 0;
    stl_s(&b, "stall trace on: watcher ");
    stl_s(&b, g_stl_thread ? "running" : "NOT started (CreateThread failed, error ");
    if (!g_stl_thread) { stl_u(&b, GetLastError()); stl_s(&b, ")"); }
    stl_s(&b, "; hooks: state machine step game+0x19281e0 ");
    stl_s(&b, ok_step ? "installed" : "NOT installed (no state flow timeline or history)");
    stl_s(&b, ", HasLoaded node game+0x8d1be0 ");
    stl_s(&b, ok_exec ? "installed" : "NOT installed");
    stl_s(&b, ". Snapshots need no hook: the console, player and session memory is read by the watcher");
    stl_emit(&b);
    return hooks > 0 || g_stl_thread != NULL;
}
#endif
