/* The physics gate (crash family "heap-corruption-game-12e0de0", game+0x1056bae; #62, #63, #328, #444, #481, reports 2cb2ec5004c023d3,
 * 312f1d36e5a754e8, 74d6c8b5dbf8380c): every car's physics is built from data the game's database has.
 *
 * game+0x1056210 builds a car's physics description from a car setup: its ordinal, its CParts (one part id per slot) and its tune. Every way
 * the game makes a car's physics or its performance numbers goes through it [confirmed: all ten calls to it in FH2 1.0.0.64, by the return
 * address the callers leave, with the class whose vtable leads there]:
 *     game+0x2d03fb   the garage rating, CCarInGarage::UpdatePerformanceColumns (garage_rating_guard.inc checks before it)
 *     game+0x346e1e   the upgrade parts preview (UpgradeCarParts and its siblings, vtable slot 188)
 *     game+0x375f50, +0x376327, +0x37c2cd, +0x37c59e, +0x37c8b8
 *                     the upgrade wizard's search (UpgradeWizard, run by an AsyncFunctionWrapper1 on a worker thread)
 *     game+0xaf1683   the car stats (CarStatsViewModel, ForzaPlayerChoices, CWatchReplayComponent)
 *     game+0xf671b7   a car's physics in the world (Car vtable slots 7 and 9; torque_load_guard.inc checks slot 7's loads before it)
 *     game+0x1046d39  the auto upgrade (AutoUpgrade, CUpgradeWizardSearch)
 * The description then becomes the car's dynamics (CCarDynamics, game+0x12bfee0) and its curves. The builder and what follows it read part
 * records without a NULL check, so a part id the database has no row for crashes the game: inside the builder at game+0x1056bae (the
 * drivetrain, #481) or game+0x105beec (the clutch, #328), or after it in the torque curve constructor game+0x12e0de0 / +0x12e0df5 (an
 * empty curve) or the tire curve copy game+0x12e26dd (no tire compound row, 312f1d36e5a754e8). Crash reports up to 2026-11-19 show five of
 * these ways crashing: the world (39 reports), the garage rating (31), the car stats (22), the upgrade parts preview (3, the last on
 * 1.4.3) and the upgrade wizard (1); only the first two had a guard. The builder and its unchecked readers are also described in Avalon
 * (Trinculo54)'s FH2 Mod Platform, docs/game-notes.md.
 *
 * The gate hooks the builder and checks the setup with torque_parts_valid(), the resolution and the records the builder reads unchecked.
 * A setup that passes is built as it is, untouched. One that does not is built from a copy of it, the first of these that passes:
 *   1. the car's own stock parts with the default tune (a part from a removed mod, or one the server's database lacks online);
 *   2. the placeholder car 1215 (NUL_Car_00) with its stock parts and the default tune (a car pack car whose own data is gone).
 * "Stock parts with the default tune" is what the game itself makes for a car setup: its constructor game+0x339aa0 (CParts unset, every
 * tune value -1), then use-stock 1, so the builder takes the stock parts (game+0x104d5f0). A car's own tune never goes with other parts:
 * the placeholder with a car pack car's tune made NaN physics that ended a whole world (#454). The caller's setup is never written.
 * When even the placeholder fails the check, the game builds the setup as it is, as before, and the log says so.
 * The car setup is 0x180 bytes: the ordinal at +0, CParts at +4 (0xb4), the tune at +0xb8, use-stock at +0x168, assist and flag fields up to
 * +0x17c (read by the builder at game+0x1057f5d..0x1057fb5) [confirmed: game+0x10428b0 makes it, game+0x2d0160 fills one on its stack].
 * FH2MP_NO_CARPHYSICS=1 switches the gate off (switches=NO_CARPHYSICS from the hotfix list). Offline runs too (offline_guards). */

#define PG_FN 0x1056210u
#define PG_SETUP_CTOR 0x339aa0u
#define PG_SETUP_SIZE 0x180u
#define PG_USE_STOCK 0x168u
#define PG_PLACEHOLDER 1215u     /* NUL_Car_00, the game's own unavailable-car ordinal */

typedef unsigned long long (*pg_build_fn)(void *, void **, unsigned char *, void *, void *, unsigned long long, unsigned long long,
                                          unsigned long long);
typedef void *(*pg_setup_ctor_fn)(void *);
static pg_build_fn g_pg_orig;
static pg_setup_ctor_fn g_pg_setup_ctor;
static volatile LONG g_pg_hits;          /* builds the gate gave other data: the guard count (X-FH2MP-Guards carries it to the LSP) */
static volatile LONG g_pg_unchecked;     /* builds it could not read */
static volatile LONG g_pg_seen_n, g_pg_lines;
static unsigned long long g_pg_seen[64]; /* car, outcome and caller of the lines written: a screen that rebuilds a car every frame logs it once */
enum { PG_OWN_STOCK = 1, PG_PLACEHOLDER_STOCK, PG_AS_IT_IS, PG_LINES_MAX = 40 };

/* The way into the builder that a return address names, or NULL. */
static const char *pg_route(unsigned rva)
{
    switch (rva) {
    case 0x2d03fbu: return "the garage rating";
    case 0x346e1eu: return "the upgrade parts preview";
    case 0x375f50u: case 0x376327u: case 0x37c2cdu: case 0x37c59eu: case 0x37c8b8u: return "the upgrade wizard's search";
    case 0xaf1683u: return "the car stats";
    case 0xf671b7u: return "a car's physics in the world";
    case 0x1046d39u: return "the auto upgrade";
    }
    return NULL;
}

/* -> 1 when this car, outcome and caller have no line yet and the log has room (racy: at worst a line twice). After PG_LINES_MAX lines one
 * more says the rest are counted only. */
static int pg_new_line(unsigned car, int outcome, unsigned rva)
{
    unsigned long long key = ((unsigned long long)car << 32) | ((unsigned long long)outcome << 28) | (rva & 0x0fffffffu);
    LONG n = g_pg_seen_n, i;
    for (i = 0; i < n && i < 64; ++i) {
        if (g_pg_seen[i] == key) return 0;
    }
    n = InterlockedIncrement(&g_pg_seen_n) - 1;
    g_pg_seen[n & 63] = key;
    if (n >= 64) InterlockedExchange(&g_pg_seen_n, 64);
    n = InterlockedIncrement(&g_pg_lines);
    if (n == PG_LINES_MAX + 1) {
        log_begin();
        log_raw("carphysics: more cars and callers than the log keeps; the next ones are only counted (the guard count)\n");
    }
    return n <= PG_LINES_MAX;
}

/* " from game+0x346e1e (the upgrade parts preview)"; the caller is the return address the builder was called with (another mod's hook on the
 * builder, if any, is the caller then). */
static void pg_from(char *out, unsigned rva)
{
    const char *route = pg_route(rva);
    if (rva >= FH2_SIZE_OF_IMAGE) lstrcpyA(out, " from outside the game's code (another mod's hook on the builder)");
    else if (route) wsprintfA(out, " from game+0x%x (%s)", rva, route);
    else wsprintfA(out, " from game+0x%x", rva);
}

/* The setup the game makes for a car's stock parts and the default tune, in place, keeping the caller's ordinal and flags. */
static void pg_stock_setup(unsigned char *setup)
{
    g_pg_setup_ctor(setup + 4);
    setup[PG_USE_STOCK] = 1;
}

static unsigned long long pg_detour(void *out, void **db_ref, unsigned char *setup, void *a4, void *a5, unsigned long long a6,
                                    unsigned long long a7, unsigned long long a8)
{
    unsigned char copy[PG_SETUP_SIZE];
    unsigned parts[45], car;
    void *db = NULL;
    torque_fault fault = {0, 0}, own = {0, 0}, placeholder = {0, 0};
    unsigned char *ret = __builtin_return_address(0);
    unsigned rva = ret >= g_torque_load_img && (UINT_PTR)(ret - g_torque_load_img) < FH2_SIZE_OF_IMAGE ? (unsigned)(ret - g_torque_load_img)
                                                                                                        : 0xffffffffu;
    const char *unread = NULL;
    int own_stock_ok = 0;
    char line[640], from[96];
    LONG n;

    if (!db_ref || !read_game_memory(db_ref, &db, sizeof db) || !db) unread = "the game database";
    else if (!setup || !read_game_memory(setup, copy, sizeof copy)) unread = "the car setup";
    if (unread) {
        n = InterlockedIncrement(&g_pg_unchecked);
        if (n <= 5 || n % 100 == 0) {
            pg_from(from, rva);
            wsprintfA(line, "carphysics: a car physics build%s was not checked (#%ld): %s cannot be read; the game builds it as it is", from,
                      (long)n, unread);
            log_begin();
            log_raw(line);
            log_raw("\n");
        }
        return g_pg_orig(out, db_ref, setup, a4, a5, a6, a7, a8);
    }
    memcpy(&car, copy, sizeof car);
    if (copy[PG_USE_STOCK]) torque_stock_parts(db, car, parts);
    else memcpy(parts, copy + 4, sizeof parts);
    if (torque_parts_valid(db, car, parts, &fault)) {
        return g_pg_orig(out, db_ref, setup, a4, a5, a6, a7, a8);    /* the caller's own setup, untouched */
    }

    n = InterlockedIncrement(&g_pg_hits);
    if (!copy[PG_USE_STOCK]) {
        unsigned stock[45];
        torque_stock_parts(db, car, stock);
        own_stock_ok = torque_parts_valid(db, car, stock, &own);
    } else {
        own = fault;            /* the setup was the car's stock parts already */
    }
    if (!own_stock_ok) {
        unsigned stock[45];
        torque_stock_parts(db, PG_PLACEHOLDER, stock);
        if (!torque_parts_valid(db, PG_PLACEHOLDER, stock, &placeholder)) {
            if (pg_new_line(car, PG_AS_IT_IS, rva)) {
                pg_from(from, rva);
                wsprintfA(line, "carphysics: car %u%s has physics data the game cannot build (#%ld, engine part %u, camshaft part %u%s): %s, value %u; "
                          "the placeholder car %u is unusable too (%s, value %u), so the game builds it as it is",
                          car, from, (long)n, parts[2], parts[14], copy[PG_USE_STOCK] ? ", stock parts" : "", fault.why, fault.detail,
                          PG_PLACEHOLDER, placeholder.why, placeholder.detail);
                log_begin();
                log_raw(line);
                log_raw("\n");
            }
            return g_pg_orig(out, db_ref, setup, a4, a5, a6, a7, a8);
        }
    }
    if (pg_new_line(car, own_stock_ok ? PG_OWN_STOCK : PG_PLACEHOLDER_STOCK, rva)) {
        pg_from(from, rva);
        if (own_stock_ok) {
            wsprintfA(line, "carphysics: car %u%s has physics data the game cannot build (#%ld, engine part %u, camshaft part %u): %s, value %u; "
                      "built from its own stock parts with the default tune instead",
                      car, from, (long)n, parts[2], parts[14], fault.why, fault.detail);
        } else {
            wsprintfA(line, "carphysics: car %u%s has physics data the game cannot build (#%ld, engine part %u, camshaft part %u%s): %s, value %u; "
                      "its stock parts cannot be built either (%s, value %u), so it is built from the placeholder car %u's stock parts with the default tune",
                      car, from, (long)n, parts[2], parts[14], copy[PG_USE_STOCK] ? ", stock parts" : "", fault.why, fault.detail, own.why,
                      own.detail, PG_PLACEHOLDER);
        }
        log_begin();
        log_raw(line);
        log_raw("\n");
    }
    pg_stock_setup(copy);
    if (!own_stock_ok) {
        unsigned ph = PG_PLACEHOLDER;
        memcpy(copy, &ph, sizeof ph);
    }
    return g_pg_orig(out, db_ref, copy, a4, a5, a6, a7, a8);
}

static int patch_car_physics(void)
{
    /* mov rax,rsp ; mov [rax+0x18],r8 ; mov [rax+0x10],rdx ; mov [rax+8],rcx ; push rbp */
    static const unsigned char head[16] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55};
    /* mov [rsp+8],rbx ; push rdi ; sub rsp,0x20 ; mov rdi,rcx ; call game+0x1042d10 (the CParts constructor) */
    static const unsigned char ctor[18] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9,
                                           0xe8, 0x5e, 0x92, 0xd0, 0x00};
    /* cmp byte [r12+0x168],0: the builder's use-stock test */
    static const unsigned char use_stock[9] = {0x41, 0x80, 0xbc, 0x24, 0x68, 0x01, 0x00, 0x00, 0x00};
    if (!torque_bind()) {
        return 0;
    }
    if (!code_is("carphysics", "the car physics builder", g_torque_load_img, PG_FN, head, sizeof head) ||
        !code_is("carphysics", "the builder's use-stock test", g_torque_load_img, 0x1056721u, use_stock, sizeof use_stock) ||
        !code_is("carphysics", "the car setup constructor", g_torque_load_img, PG_SETUP_CTOR, ctor, sizeof ctor)) {
        return 0;
    }
    g_pg_setup_ctor = (pg_setup_ctor_fn)(g_torque_load_img + PG_SETUP_CTOR);
    if (!plat_hook("carphysics: car physics builder", g_torque_load_img + PG_FN, (void *)pg_detour, (void **)&g_pg_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: carphysics: a car's physics is built from data the database has: its own parts, else its stock parts, else the placeholder car 1215's (game+0x1056210)\n");
    return 1;
}
