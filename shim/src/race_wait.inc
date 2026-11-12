/* Race load wait: which participants the game still waits for, and a way through when only full car slots hold it (prod 2026-11-11).
 *
 * Why: in a full world (12 players, 11 remote cars, the stock car cache) V's game sat in the event's cars-loaded wait (States::NetworkWaitForCarsLoaded,
 * game+0x8e0990, hooked by carsel_sync.inc) for 37 s after the car select hold had let it go, while four other racers reported loaded the second theirs
 * ended; the server dropped it. Nothing on the client said which car its game waited for. In every build, when one wait has lasted RW_FIRST_MS and
 * RW_SECOND_MS, this file applies the node's own rule to the game's players and sends the result to the server (client report code 6, carsel_wire.h)
 * and to the log ("racewait:"):
 *   still loading N of R racers [id:why ...]  the remote participants the node counts: id = the player's console id (CPlayer+0x2c), why d = no car yet
 *                                              (CPlayer+0x320 == 0), c = car choice pending (ForzaPlayerChoices+0x108 == 1)
 *   racers id,id,...                           every remote console with ParticipatingInRace set (the server matches the ids to its owners)
 *   cache free F wait W load L done D          the car cache's Non-LOD0 group (car_pool.inc car_wait_group): free slots, cars waiting for a slot, loading, loaded
 *   session f0= e0= 100=                       the three session flags the node reads first (each getter's byte, decoded, never called)
 * It only reads, through read_game_memory.
 *
 * The node [C: decompile of game+0x8e0990]: with session flag 0xf0 set it counts the players of its list for which vfunc 0x190 is set, 0x128 clear, 0x130
 * gives a console whose 0xc0 (ParticipatingInRace) is set, and whose choices' 0x80 returns 1 or whose 0x90 gives a null car; none = the Loaded transition
 * FUN_141925260(self, self + 0x28). With 0xf0 clear it waits while 0xe0 is set and 0x100 is not. The fields read here stand for those calls [I: the getters,
 * as in stall_trace.inc]: CPlayer+0x40 > 0xfffffffd = local, +0x320 the car, +0x340 ForzaPlayerChoices (+0x108, +0x128), the console with id +0x2c.
 *
 * Way through (opt-in FH2MP_RACEWAIT_PASS=1, not verified in a running game): when the car cache is full (no free slot, at least one car waiting for one,
 * nothing loading) and unchanged for RW_PASS_STABLE_MS, every participant the node counts has no car at all (d only), the session takes the players branch,
 * FH2MP's own hold is not holding, and the wait is RW_PASS_AFTER_MS old, the shim runs the node's Loaded transition (its bytes checked at install) instead
 * of the node's update: the game goes on as once the cars are in; the cars without a slot show when one frees up. The same reasoning as the Free Roam wait's
 * pass in car_pool.inc (game+0x7cbf10). Unset, the game's update runs and the server drops a client that does not load. */

#define RW_RVA_CONSOLE_MGR 0x34af7a0u   /* CConsoleManager*: +0x18 local console, +0x20 std::map head of the remote consoles [C] */
#define RW_RVA_PLAYER_MGR 0x34b3e18u    /* CPlayerManager*: +0x48 std::list sentinel of CPlayer* (node +0x10) [C] */
#define RW_RVA_NET_MGR 0x34d6348u       /* the network manager: +0xd0 the session object [C] */
#define RW_RVA_LOADED 0x1925260u        /* FUN_141925260, the node's Loaded transition (self, self + 0x28) [C] */
#define RW_REPORT_CODE 6
#define RW_FIRST_MS 20000u
#define RW_SECOND_MS 40000u
#define RW_PASS_AFTER_MS 15000u
#define RW_PASS_STABLE_MS 5000u
#define RW_PASS_SCAN_MS 1000u
#define RW_MAX 24

typedef struct { unsigned id; unsigned char why; } rw_stuck;           /* why: bit 0 no car yet, bit 1 car choice pending */
typedef struct {
    int players;                 /* players in the list, -1 = the list did not validate */
    int counted;                 /* participants the node counts as still loading */
    rw_stuck stuck[RW_MAX];
    int nracers;
    unsigned racers[RW_MAX];     /* console ids of the remote consoles with ParticipatingInRace */
    int cache_ok;
    unsigned cache[4];           /* free slots, waiting for a slot, loading, loaded */
    int f0, e0, f100;            /* session flags, -1 = not readable */
} rw_scan;
typedef struct { char s[CARSEL_REPORT_TEXT + 1]; unsigned n; } rw_text;

static unsigned char *g_rw_img;
static void (*g_rw_loaded)(void *, void *);
static int g_rw_pass_on;
static ULONGLONG g_rw_wait_first, g_rw_last_scan, g_rw_cache_changed;  /* game thread only */
static unsigned g_rw_cache_sig[4];
static int g_rw_sent;
static unsigned g_rw_passes;

static int rw_read(ULONG_PTR a, void *out, SIZE_T n) { return a >= 0x10000 && read_game_memory((const void *)a, out, n); }
static int rw_ptr(ULONG_PTR a, ULONG_PTR *v) { *v = 0; return rw_read(a, v, sizeof *v) && *v >= 0x10000; }
static int rw_in_img(ULONG_PTR p) { return g_rw_img && p - (ULONG_PTR)g_rw_img < FH2_SIZE_OF_IMAGE; }

/* A console's id and ParticipatingInRace; 0 when the block does not look like a console (vars 0-6 bools, 7-8 enums 0..2, as stall_trace.inc checks). */
static int rw_console(ULONG_PTR c, unsigned *id, int *participating)
{
    unsigned char blk[0x20 * 8 + 1];
    unsigned meta[2];
    int i;
    if (!rw_read(c + 0x20, meta, sizeof meta) || !rw_read(c + 0xa0, blk, sizeof blk)) return 0;
    for (i = 0; i < 7; i++) if (blk[0x20 * i] > 1) return 0;
    if (blk[0x20 * 7] > 2 || blk[0x20 * 8] > 2) return 0;
    *id = meta[1];
    *participating = blk[0x20 * 2];
    return 1;
}

/* The remote consoles: a walk of the std::map tree (node +0 left, +8 parent, +0x10 right, +0x19 is-nil, value pointer +0x28). */
static int rw_consoles(unsigned *ids, unsigned char *part, int max)
{
    ULONG_PTR cm, head, root, stack[64];
    int sp = 0, n = 0, visited = 0;
    if (!rw_ptr((ULONG_PTR)g_rw_img + RW_RVA_CONSOLE_MGR, &cm) || !rw_ptr(cm + 0x20, &head) || !rw_ptr(head + 8, &root) || root == head) return 0;
    stack[sp++] = root;
    while (sp > 0 && n < max && visited++ < 96) {
        ULONG_PTR node = stack[--sp], l, r, c;
        unsigned char nil;
        unsigned id;
        int p;
        if (node == head || !rw_read(node + 0x19, &nil, 1) || nil) continue;
        if (rw_ptr(node + 0x28, &c) && rw_console(c, &id, &p)) {
            ids[n] = id;
            part[n] = (unsigned char)p;
            n++;
        }
        if (rw_ptr(node + 0x10, &r) && sp < 63) stack[sp++] = r;
        if (rw_ptr(node, &l) && sp < 63) stack[sp++] = l;
    }
    return n;
}

/* The players the node counts: remote, choices +0x128 clear, a participating console, and (choices +0x108 == 1 or no car). */
static void rw_players(rw_scan *s, const unsigned *ids, const unsigned char *part, int nc)
{
    ULONG_PTR pm, sent, node;
    int guard = 0;
    s->players = -1;
    if (!rw_ptr((ULONG_PTR)g_rw_img + RW_RVA_PLAYER_MGR, &pm) || !rw_ptr(pm + 0x48, &sent) || !rw_ptr(sent, &node)) return;
    s->players = 0;
    while (node != sent && guard++ < 64) {
        ULONG_PTR p, next, choices;
        unsigned id, local_field;
        unsigned long long car = 0;
        unsigned char c108, c128;
        int i, participating = 0;
        if (!rw_ptr(node + 0x10, &p) || !rw_ptr(node, &next)) break;
        s->players++;
        node = next;
        if (!rw_read(p + 0x2c, &id, 4) || !rw_read(p + 0x40, &local_field, 4) || local_field > 0xfffffffdu) continue;
        if (!rw_ptr(p + 0x340, &choices) || !rw_read(choices + 0x108, &c108, 1) || !rw_read(choices + 0x128, &c128, 1) || c128 != 0) continue;
        rw_read(p + 0x320, &car, sizeof car);
        for (i = 0; i < nc; i++) {
            if (ids[i] == id) {participating = part[i]; break;}
        }
        if (!participating || (c108 != 1 && car != 0)) continue;
        if (s->counted < RW_MAX) {
            s->stuck[s->counted].id = id;
            s->stuck[s->counted].why = (unsigned char)((car == 0 ? 1 : 0) | (c108 == 1 ? 2 : 0));
        }
        s->counted++;
    }
}

/* One session flag: the byte the getter in vtable entry `slot` returns, read without calling it (movzx/mov al from [rcx+d], up to two jmp hops); -1 when
 * the getter is anything else or nothing reads. */
static int rw_flag(ULONG_PTR sess, ULONG_PTR vt, unsigned slot)
{
    ULONG_PTR fn;
    unsigned char c[16], v;
    unsigned disp = 0;
    int hop, rel;
    if (!rw_ptr(vt + slot, &fn)) return -1;
    for (hop = 0; hop < 3; hop++) {
        if (!rw_in_img(fn) || !rw_read(fn, c, sizeof c)) return -1;
        if (c[0] != 0xe9) break;
        memcpy(&rel, c + 1, 4);
        fn = fn + 5 + (ULONG_PTR)(LONG_PTR)rel;
    }
    if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x81 && c[7] == 0xc3) memcpy(&disp, c + 3, 4);
    else if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x41 && c[4] == 0xc3) disp = c[3];
    else if (c[0] == 0x0f && c[1] == 0xb6 && c[2] == 0x01 && c[3] == 0xc3) disp = 0;
    else if (c[0] == 0x8a && c[1] == 0x81 && c[6] == 0xc3) memcpy(&disp, c + 2, 4);
    else if (c[0] == 0x8a && c[1] == 0x41 && c[3] == 0xc3) disp = c[2];
    else if (c[0] == 0x8a && c[1] == 0x01 && c[2] == 0xc3) disp = 0;
    else return -1;
    return rw_read(sess + disp, &v, 1) ? v != 0 : -1;
}

static void rw_scan_now(rw_scan *s)
{
    unsigned ids[RW_MAX];
    unsigned char part[RW_MAX];
    ULONG_PTR mgr, sess, vt;
    int nc, i;
    memset(s, 0, sizeof *s);
    s->f0 = s->e0 = s->f100 = -1;
    nc = rw_consoles(ids, part, RW_MAX);
    for (i = 0; i < nc; i++) {
        if (part[i] && s->nracers < RW_MAX) s->racers[s->nracers++] = ids[i];
    }
    rw_players(s, ids, part, nc);
    s->cache_ok = car_wait_group(s->cache);
    if (rw_ptr((ULONG_PTR)g_rw_img + RW_RVA_NET_MGR, &mgr) && rw_ptr(mgr + 0xd0, &sess) && rw_ptr(sess, &vt) && rw_in_img(vt)) {
        s->f0 = rw_flag(sess, vt, 0xf0);
        s->e0 = rw_flag(sess, vt, 0xe0);
        s->f100 = rw_flag(sess, vt, 0x100);
    }
}

static void rw_t(rw_text *b, const char *t)
{
    while (*t && b->n < CARSEL_REPORT_TEXT) b->s[b->n++] = *t++;
    b->s[b->n] = 0;
}
static void rw_u(rw_text *b, unsigned long long v)
{
    char d[24], one[2] = {0, 0};
    int k = 0;
    do d[k++] = (char)('0' + v % 10); while (v /= 10);
    while (k) {one[0] = d[--k]; rw_t(b, one);}
}
static void rw_f(rw_text *b, int v) { if (v < 0) rw_t(b, "?"); else rw_u(b, (unsigned)v); }

/* "race wait 20 s: still loading 1 of 6 racers [11:d]; racers 1,3,4,5,8,11; cache free 0 wait 1 load 0 done 11; session f0=1 e0=0 100=0" */
static void rw_describe(const rw_scan *s, unsigned secs, int holding, rw_text *b)
{
    int i;
    b->n = 0;
    b->s[0] = 0;
    rw_t(b, "race wait "); rw_u(b, secs); rw_t(b, holding ? " s (FH2MP car select hold on): " : " s: ");
    if (s->players < 0) {
        rw_t(b, "player list unreadable");
    } else {
        rw_t(b, "still loading "); rw_u(b, (unsigned)s->counted); rw_t(b, " of "); rw_u(b, (unsigned)s->nracers); rw_t(b, " racers");
        for (i = 0; i < s->counted && i < 6 && i < RW_MAX; i++) {
            rw_t(b, i ? " " : " ["); rw_u(b, s->stuck[i].id); rw_t(b, ":");
            if (s->stuck[i].why & 1) rw_t(b, "d");
            if (s->stuck[i].why & 2) rw_t(b, "c");
        }
        if (s->counted) rw_t(b, "]");
    }
    rw_t(b, "; racers ");
    for (i = 0; i < s->nracers; i++) {if (i) rw_t(b, ","); rw_u(b, s->racers[i]);}
    if (!s->nracers) rw_t(b, "none");
    if (s->cache_ok) {
        rw_t(b, "; cache free "); rw_u(b, s->cache[0]); rw_t(b, " wait "); rw_u(b, s->cache[1]);
        rw_t(b, " load "); rw_u(b, s->cache[2]); rw_t(b, " done "); rw_u(b, s->cache[3]);
    } else {
        rw_t(b, "; cache unreadable");
    }
    rw_t(b, "; session f0="); rw_f(b, s->f0); rw_t(b, " e0="); rw_f(b, s->e0); rw_t(b, " 100="); rw_f(b, s->f100);
}

/* The way through applies to this scan: the node takes the players branch, it counts only participants without a car, and the car cache is full, idle
 * and unchanged for RW_PASS_STABLE_MS. The caller adds the switch, FH2MP's hold and the wait's age. */
static int rw_pass_ok(const rw_scan *s, ULONGLONG cache_stable_ms)
{
    int i;
    if (s->players < 0 || s->counted == 0 || s->counted > RW_MAX || s->f0 != 1 || !s->cache_ok) return 0;
    if (s->cache[0] != 0 || s->cache[1] == 0 || s->cache[2] != 0 || cache_stable_ms < RW_PASS_STABLE_MS) return 0;
    for (i = 0; i < s->counted; i++) {
        if (s->stuck[i].why != 1) return 0;
    }
    return 1;
}

/* Called by carsel_update on every frame of a cars-loaded wait (game thread). Reports at RW_FIRST_MS and RW_SECOND_MS; with FH2MP_RACEWAIT_PASS returns 1
 * after it ran the Loaded transition itself, and the caller then skips the node's update. */
static int race_wait_step(void *self, ULONGLONG now, ULONGLONG wait_first, int holding)
{
    rw_scan s;
    rw_text t;
    ULONGLONG age = now - wait_first;
    int due, scan_for_pass;
    if (!g_rw_img) return 0;
    if (wait_first != g_rw_wait_first) {
        g_rw_wait_first = wait_first;
        g_rw_sent = 0;
        g_rw_last_scan = 0;
        g_rw_cache_changed = now;
        memset(g_rw_cache_sig, 0xff, sizeof g_rw_cache_sig);
    }
    due = (g_rw_sent == 0 && age >= RW_FIRST_MS) || (g_rw_sent == 1 && age >= RW_SECOND_MS);
    scan_for_pass = g_rw_pass_on && g_rw_loaded && !holding && age + RW_PASS_STABLE_MS >= RW_PASS_AFTER_MS && now - g_rw_last_scan >= RW_PASS_SCAN_MS;
    if (!due && !scan_for_pass) return 0;
    g_rw_last_scan = now;
    rw_scan_now(&s);
    if (s.cache_ok && memcmp(s.cache, g_rw_cache_sig, sizeof g_rw_cache_sig)) {
        memcpy(g_rw_cache_sig, s.cache, sizeof g_rw_cache_sig);
        g_rw_cache_changed = now;
    }
    if (due) {
        g_rw_sent++;
        rw_describe(&s, (unsigned)(age / 1000), holding, &t);
        log_begin(); log_raw("racewait: "); log_raw(t.s); log_raw("\n");
        carsel_report_post(RW_REPORT_CODE, (unsigned)(age / 1000), t.s);
    }
    if (scan_for_pass && age >= RW_PASS_AFTER_MS && rw_pass_ok(&s, now - g_rw_cache_changed)) {
        rw_describe(&s, (unsigned)(age / 1000), holding, &t);
        g_rw_passes++;
        g_rw_sent = 2;
        log_begin(); log_raw("racewait: let through after "); log_dec((unsigned long)(age / 1000));
        log_raw(" s because only full car slots held it (FH2MP_RACEWAIT_PASS); the participants without a car show once a slot frees up: "); log_raw(t.s); log_raw("\n");
        g_rw_loaded(self, (unsigned char *)self + 0x28);
        return 1;
    }
    return 0;
}

static int patch_race_wait(void)
{
    static const unsigned char loaded_head[] = {0x48, 0x83, 0x7a, 0x10, 0x00, 0x8b, 0x0d, 0x6d, 0x99, 0xc2, 0x01, 0x65, 0x48, 0x8b, 0x04, 0x25, 0x58, 0x00, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    wchar_t env[8];
    if (!img) return 0;
    g_rw_img = img;
    if (GetEnvironmentVariableW(L"FH2MP_RACEWAIT_PASS", env, 8) && env[0] == L'1' &&
        code_is("racewait", "the cars-loaded wait's Loaded transition (game+0x1925260)", img, RW_RVA_LOADED, loaded_head, sizeof loaded_head)) {
        g_rw_loaded = (void (*)(void *, void *))(img + RW_RVA_LOADED);
        g_rw_pass_on = 1;
    }
    g_race_wait_step = race_wait_step;
    log_begin();
    log_raw(g_rw_pass_on ? "racewait: a race load wait is reported at 20 and 40 s; it is let through when only full car slots hold it (FH2MP_RACEWAIT_PASS)\n"
                         : "racewait: a race load wait is reported at 20 and 40 s\n");
    return 1;
}
