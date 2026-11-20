/* Every way the game builds a car's physics (game+0x1056210, its ten callers) with a car whose data the database does not have: the build
 * must not reach a read the game makes without a NULL check. Offline: no game, sockets, assets or input.
 *
 * The game here is a fixture: a car database (cars, part rows, torque curves, tire compounds), the game's part resolvers, its stock-part
 * builder and car setup constructor, and the physics builder. The builder stands in for game+0x1056210 and what follows it: it resolves the
 * setup's parts and records where the real game crashes on a missing record, as the crash reports show it:
 *     the drivetrain record (car part slot 1)       game+0x1056bae   #481, 74d6c8b5dbf8380c (the upgrade parts preview, 1.4.3)
 *     the body record (car part slot 2)             game+0x1056cc6
 *     the clutch record (drivetrain slot 30)        game+0x105beec   #328
 *     the tire compound (car part slot 8 + its row) game+0x12e26dd   312f1d36e5a754e8 (the garage rating, car 2479)
 *     the torque curve (engine, camshaft, curve with 2..246 samples)  game+0x12e0de0   #62, #63, #444, 2cb2ec5004c023d3
 * The game calls the builder through whatever the shim hooked at game+0x1056210, else straight. Without the physics gate
 * (src/car_physics_guard.inc, absent before 2026-11-19) the cars below crash it, as they crashed players. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#include "src/game_image.inc"
static char g_log[65536];
static unsigned g_lines;
static void log_begin(void) {}
static void log_raw(const char *s)
{
    if (strlen(g_log) + strlen(s) < sizeof g_log) strcat(g_log, s);
    if (strchr(s, '\n')) ++g_lines;
}
#include "src/torque_load_guard.inc"

static int g_failed, g_checks;
#define CHECK(cond) do { ++g_checks; if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); ++g_failed; } } while (0)

/* --- the platform, as the shim sees it ------------------------------------------------------------------------------------------------ */
static unsigned char *g_hooked_target;
static void *g_hooked_detour;
typedef unsigned long long (*build_fn)(void *, void **, unsigned char *, void *, void *, unsigned long long, unsigned long long,
                                       unsigned long long);
static unsigned long long native_build(void *, void **, unsigned char *, void *, void *, unsigned long long, unsigned long long,
                                       unsigned long long);
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag; (void)what;
    return !memcmp(img + rva, expect, n);
}
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name;
    g_hooked_target = target;
    g_hooked_detour = detour;
    *original = (void *)native_build;
    return 1;
}
static int torque_bind(void) { return g_torque_load_img != NULL; }
#if __has_include("src/car_physics_guard.inc")
#include "src/car_physics_guard.inc"
#define HAVE_GATE 1
#endif

/* --- the car database ---------------------------------------------------------------------------------------------------------------- */
/* Part ids: a car's stock part for slot s is car*1000+s; a part a mod added for slot s is car*1000+100+s (a removed mod, or a part the
 * server's database lacks online, has no row). Engine slot 12 (the camshaft) and drivetrain slot 30 (the clutch) are owned by the car's
 * EngineID / DrivetrainID, both the car's ordinal here. */
typedef struct {
    unsigned id;
    int engine, motor, camshaft, curve, samples, drivetrain, body, clutch, tire, compound, mod_parts;
} car_data;
static car_data g_cars[4];
static car_data *car_of(unsigned id)
{
    for (unsigned i = 0; i < sizeof g_cars / sizeof g_cars[0]; ++i)
        if (g_cars[i].id == id) return &g_cars[i];
    return NULL;
}
static car_data healthy(unsigned id)
{
    return (car_data){id, 1, 0, 1, 1, 81, 1, 1, 1, 1, 1, 1};
}
/* Is there a row for this part, owned by this owner? The one source of truth for the resolvers and for the builder below. */
static int part_row(int slot, unsigned owner, unsigned id)
{
    car_data *c = car_of(owner);
    int stock, mod, present;
    if (!c) return 0;
    stock = id == c->id * 1000 + (unsigned)slot;
    mod = id == c->id * 1000 + 100 + (unsigned)slot;
    if (!stock && !(mod && c->mod_parts)) return 0;
    switch (slot) {
    case 0: present = c->engine; break;
    case 1: present = c->drivetrain; break;
    case 2: present = c->body; break;
    case 3: present = c->motor; break;
    case 8: present = c->tire; break;
    case 12: present = c->camshaft; break;
    case 30: present = c->clutch; break;
    default: present = 1;
    }
    return present;
}

/* Rows as the game's query objects: refcounted, read through vtable methods (add-ref 0x10, release 0x18, query 0x48, uint 0x110, rows 0x148). */
typedef struct { void **vtable; long refs; unsigned car, kind; } row;
enum { R_DB, R_ENGINE, R_MOTOR, R_CAMSHAFT, R_CURVE, R_DRIVETRAIN, R_BODY, R_CLUTCH, R_TIRE, R_COMPOUND, R_OTHER };
static void *g_methods[42];
static row g_db, g_pool[64];
static unsigned g_pool_used;
static row *new_row(unsigned car, unsigned kind)
{
    row *r = &g_pool[g_pool_used++ % 64];
    *r = (row){g_methods, 1, car, kind};
    return r;
}
static void addref(void *p) { ++((row *)p)->refs; }
static void release(void *p) { --((row *)p)->refs; }
static int rows(void *p) { return 1; (void)p; }
static const char *key_text(const torque_key *key) { return key->capacity < 16 ? key->text.small : key->text.ptr; }
static unsigned uint_field(void *p, const torque_key *key)
{
    row *r = p;
    const char *f = key_text(key);
    car_data *c = car_of(r->car);
    if (!strcmp(f, "EngineID") || !strcmp(f, "DrivetrainID")) return r->car;
    if (!strcmp(f, "TorqueCurveFullThrottleID")) return r->car * 10;
    if (!strcmp(f, "TireCompoundID")) return r->car * 10 + 5;
    if (!strcmp(f, "NumTorqueValues")) return c ? (unsigned)c->samples : 0;
    return 0;
}
static void **query(void *self, void **out, const torque_key *sql)
{
    const char *q = key_text(sql);
    unsigned id = 0;
    car_data *c;
    (void)self;
    *out = NULL;
    if (sscanf(q, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=%u", &id) == 1) {
        if ((c = car_of(id / 10)) && c->curve) *out = new_row(c->id, R_CURVE);
    } else if (sscanf(q, "SELECT * FROM List_TireCompound WHERE TireCompoundID=%u", &id) == 1) {
        if ((c = car_of((id - 5) / 10)) && c->compound) *out = new_row(c->id, R_COMPOUND);
    }
    return out;
}
static const unsigned g_slot_kind[9] = {R_ENGINE, R_DRIVETRAIN, R_BODY, R_MOTOR, R_OTHER, R_OTHER, R_OTHER, R_OTHER, R_TIRE};
/* game+0x10482f0, +0x1048990, +0x1048640: they consume the database reference passed in. */
static void **car_part(void **out, void **ref, int slot, unsigned owner, unsigned id)
{
    release(*ref);
    *out = part_row(slot, owner, id) ? new_row(owner, slot < 9 ? g_slot_kind[slot] : R_OTHER) : NULL;
    return out;
}
static void **engine_part(void **out, void **ref, int slot, unsigned owner, unsigned id)
{
    release(*ref);
    *out = part_row(slot, owner, id) ? new_row(owner, R_CAMSHAFT) : NULL;
    return out;
}
static void **drivetrain_part(void **out, void **ref, int slot, unsigned owner, unsigned id)
{
    release(*ref);
    *out = part_row(slot, owner, id) ? new_row(owner, R_CLUTCH) : NULL;
    return out;
}
/* game+0x1042d10 (CParts: every id unset) and game+0x104d5f0 (a car's stock parts). */
static void *parts_ctor(void *parts)
{
    memset(parts, 0xff, 45 * sizeof(unsigned));
    return parts;
}
static void stock_parts(void *parts, void **ref, unsigned car)
{
    unsigned *w = parts, s;
    release(*ref);
    for (s = 0; s < 43; ++s) w[s + 2] = car * 1000 + s;
}
/* game+0x339aa0, the car setup constructor: CParts unset, then the tune and nine more values at -1 (game+0x283150, game+0x339ac1). */
static void *setup_ctor(void *setup)
{
    float minus_one = -1.0f;
    parts_ctor(setup);
    for (unsigned off = 0xb4; off < 0x164; off += 4) memcpy((unsigned char *)setup + off, &minus_one, 4);
    return setup;
}

/* --- the physics builder (game+0x1056210 and the dynamics built from its description) ------------------------------------------------- */
static struct {
    unsigned calls, car, use_stock;
    unsigned char *setup;        /* the setup it was given */
    unsigned char seen[0x180];   /* its contents at the time */
    const char *crash;           /* where the real game crashes on this setup, NULL when it builds */
} g_built;
static unsigned long long native_build(void *out, void **db_ref, unsigned char *setup, void *a4, void *a5, unsigned long long a6,
                                       unsigned long long a7, unsigned long long a8)
{
    unsigned parts[45], car;
    (void)out; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8;
    ++g_built.calls;
    g_built.setup = setup;
    if (!setup || !db_ref || *db_ref != &g_db) return 0x1234;      /* the unreadable cases below: only where it went */
    memcpy(g_built.seen, setup, sizeof g_built.seen);
    memcpy(&car, setup, 4);
    g_built.car = car;
    g_built.use_stock = setup[0x168];
    if (setup[0x168]) { void *ref = &g_db; addref(&g_db); stock_parts(parts, &ref, car); }
    else memcpy(parts, setup + 4, sizeof parts);
    g_built.crash = NULL;
    if (!part_row(1, car, parts[3])) g_built.crash = "game+0x1056bae (DrivetrainID of a NULL drivetrain record)";
    else if (!part_row(2, car, parts[4])) g_built.crash = "game+0x1056cc6 (CarBodyID of a NULL body record)";
    else if (!part_row(30, car, parts[32])) g_built.crash = "game+0x105beec (ClutchMaxTorque of a NULL clutch record)";
    else if (!part_row(8, car, parts[10]) || !car_of(car)->compound) g_built.crash = "game+0x12e26dd (tire curves from no tire compound)";
    else if (!part_row(3, car, parts[5])) {
        car_data *c = car_of(car);
        if (!part_row(0, car, parts[2]) || !part_row(12, car, parts[14]) || !c->curve || c->samples < 2 || c->samples > 246)
            g_built.crash = "game+0x12e0de0 (a torque curve with no samples)";
    }
    return 0x1234;
}

/* --- the game's calls ---------------------------------------------------------------------------------------------------------------- */
static unsigned char *g_img;
static unsigned long long game_builds(unsigned char *setup)
{
    unsigned char out[16];
    void *ref = &g_db;
    build_fn entry = g_hooked_target == g_img + 0x1056210u && g_hooked_detour ? (build_fn)g_hooked_detour : native_build;
    memset(&g_built, 0, sizeof g_built);
    return entry(out, &ref, setup, (void *)0x68, NULL, 1, 0, 1);
}
/* A car setup as the game's callers fill one: ordinal, installed parts, the player's tune, use-stock, assists. */
static unsigned char g_setup[0x180];
static void make_setup(unsigned car, int use_stock)
{
    static const float tune[4] = {1.0f, 0.158f, 0.474f, 0.5f};    /* the start of the tune in #454 */
    unsigned parts[45], s;
    memset(g_setup, 0, sizeof g_setup);
    memcpy(g_setup, &car, 4);
    parts_ctor(parts);
    for (s = 0; s < 43; ++s) parts[s + 2] = car * 1000 + s;
    memcpy(g_setup + 4, parts, sizeof parts);
    for (s = 0; s < 0xb0 / 4; ++s) memcpy(g_setup + 0xb8 + s * 4, &tune[s % 4], 4);
    g_setup[0x168] = (unsigned char)use_stock;
    g_setup[0x169] = 1; g_setup[0x16a] = 1; g_setup[0x16b] = 1; g_setup[0x16c] = 1;
    s = 1; memcpy(g_setup + 0x170, &s, 4);
}
static void install_mod_part(unsigned car, int slot)
{
    unsigned id = car * 1000 + 100 + (unsigned)slot;
    memcpy(g_setup + 4 + (slot + 2) * 4, &id, 4);
}
static void database(car_data a, car_data b)
{
    memset(g_cars, 0, sizeof g_cars);
    g_cars[0] = a;
    g_cars[1] = b;
    g_cars[2] = healthy(1215);   /* NUL_Car_00, the placeholder */
    g_cars[3] = healthy(200);    /* a stock car of the game */
}
/* Every row the shim was handed was released, and the database reference is where it was. */
static void references_balanced(void)
{
    CHECK(g_db.refs == 1);
    for (unsigned i = 0; i < 64; ++i)
        if (g_pool[i].vtable) CHECK(g_pool[i].refs == 0);
}
/* Built from the stock parts of `car` with the game's default tune, the caller's assists kept, the caller's setup untouched. */
static void built_stock(unsigned car)
{
    float minus_one = -1.0f;
    unsigned unset = 0xffffffffu;
    CHECK(g_built.calls == 1);
    CHECK(g_built.crash == NULL);
    CHECK(g_built.car == car);
    CHECK(g_built.use_stock == 1);
    CHECK(g_built.setup != g_setup);
    for (unsigned off = 0xb8; off < 0x168; off += 4) CHECK(!memcmp(g_built.seen + off, &minus_one, 4));
    for (unsigned off = 4; off < 0xb8; off += 4) CHECK(!memcmp(g_built.seen + off, &unset, 4));
    CHECK(!memcmp(g_built.seen + 0x169, g_setup + 0x169, 0x180 - 0x169));
}

int main(void)
{
    static const unsigned char head[16] = {0x48, 0x8b, 0xc4, 0x4c, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x55};
    static const unsigned char use_stock[9] = {0x41, 0x80, 0xbc, 0x24, 0x68, 0x01, 0x00, 0x00, 0x00};
    static const unsigned char ctor[18] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9,
                                           0xe8, 0x5e, 0x92, 0xd0, 0x00};
    unsigned char before[sizeof g_setup];
    car_data c;

    g_methods[2] = (void *)addref; g_methods[3] = (void *)release;
    g_methods[9] = (void *)query; g_methods[34] = (void *)uint_field; g_methods[41] = (void *)rows;
    g_db = (row){g_methods, 1, 0, R_DB};
    g_torque_stock = stock_parts; g_torque_parts_ctor = parts_ctor;
    g_torque_car_part = car_part; g_torque_engine_part = engine_part; g_torque_drivetrain_part = drivetrain_part;
    /* The game's code where the shim checks it: FH2 1.0.0.64's bytes at the builder, its use-stock test and the setup constructor. */
    g_img = VirtualAlloc(NULL, FH2_SIZE_OF_IMAGE, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(g_img && VirtualAlloc(g_img + 0x339000u, 0x1000, MEM_COMMIT, PAGE_READWRITE) && VirtualAlloc(g_img + 0x1056000u, 0x1000, MEM_COMMIT, PAGE_READWRITE));
    memcpy(g_img + 0x1056210u, head, sizeof head);
    memcpy(g_img + 0x1056721u, use_stock, sizeof use_stock);
    memcpy(g_img + 0x339aa0u, ctor, sizeof ctor);
    g_torque_load_img = g_img;
#ifdef HAVE_GATE
    CHECK(patch_car_physics());
    CHECK(g_pg_setup_ctor == (pg_setup_ctor_fn)(g_img + 0x339aa0u));
    g_pg_setup_ctor = setup_ctor;      /* the fixture's constructor stands in for the game's */
#endif
    g_log[0] = 0; g_lines = 0;         /* the hook's own line */

    /* A stock car of the game: built from the caller's own setup, untouched, nothing logged. */
    database(healthy(123), healthy(124));
    make_setup(200, 0); memcpy(before, g_setup, sizeof before);
    CHECK(game_builds(g_setup) == 0x1234);
    CHECK(g_built.calls == 1 && g_built.setup == g_setup && g_built.crash == NULL && !memcmp(before, g_setup, sizeof before));
    CHECK(g_lines == 0);
    /* A car with a mod's part installed while the mod is there: as it is. */
    make_setup(123, 0); install_mod_part(123, 1);
    game_builds(g_setup);
    CHECK(g_built.setup == g_setup && g_built.crash == NULL);
    CHECK(g_lines == 0);
    /* An electric car (no engine, a motor) has no torque curve to check. */
    c = healthy(123); c.engine = 0; c.motor = 1; c.camshaft = 0; c.curve = 0;
    database(c, healthy(124)); make_setup(123, 0);
    game_builds(g_setup);
    CHECK(g_built.setup == g_setup && g_built.crash == NULL);
    references_balanced();

    /* #481 / 74d6c8b5dbf8380c: a saved drivetrain part from a mod the game no longer has (or the server's database online). The car's own
     * stock parts are all there: it is built from them, with the default tune, not the player's tune for the missing part. */
    c = healthy(123); c.mod_parts = 0;
    database(c, healthy(124)); make_setup(123, 0); install_mod_part(123, 1); memcpy(before, g_setup, sizeof before);
    game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: car 123 with a missing drivetrain part reached %s\n", g_built.crash);
    built_stock(123);
    CHECK(!memcmp(before, g_setup, sizeof before));
    CHECK(strstr(g_log, "carphysics: car 123 ") && strstr(g_log, "the car's drivetrain part row is missing") &&
          strstr(g_log, "built from its own stock parts with the default tune instead\n"));
    references_balanced();
    /* The same for the other records the builder reads unchecked: the body (slot 2), the clutch (#328), the tire compound part. */
    make_setup(123, 0); install_mod_part(123, 2); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: a missing body part reached %s\n", g_built.crash);
    built_stock(123);
    make_setup(123, 0); install_mod_part(123, 30); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: a missing clutch part reached %s\n", g_built.crash);
    built_stock(123);
    make_setup(123, 0); install_mod_part(123, 8); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: a missing tire compound part reached %s\n", g_built.crash);
    built_stock(123);
    references_balanced();

    /* A car pack car whose own data is gone: its stock parts fail too, so it is built as the placeholder car 1215 (NUL_Car_00) with that
     * car's stock parts and the default tune (#454: never the placeholder with another car's tune). */
    c = healthy(123); c.engine = 0;                     /* no engine row (#62, #63, #444) */
    database(c, healthy(124)); make_setup(123, 0);
    game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: car 123 without an engine row reached %s\n", g_built.crash);
    built_stock(1215);
    CHECK(strstr(g_log, "so it is built from the placeholder car 1215's stock parts with the default tune\n") != NULL);
    c = healthy(123); c.samples = 0;                    /* an empty torque curve */
    database(c, healthy(124)); make_setup(123, 0); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: car 123 with an empty torque curve reached %s\n", g_built.crash);
    built_stock(1215);
    for (unsigned n = 0; n < 300; ++n) {                /* the curve constructor's range is 2..246 samples */
        c = healthy(123); c.samples = (int)n;
        database(c, healthy(124)); make_setup(123, 0); game_builds(g_setup);
        CHECK(g_built.crash == NULL);
        CHECK(g_built.car == (n >= 2 && n <= 246 ? 123u : 1215u));
    }
    c = healthy(123); c.compound = 0;                   /* 312f1d36e5a754e8: car 2479's tire compound names no row */
    database(c, healthy(124)); make_setup(123, 0); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: car 123 whose tire compound has no row reached %s\n", g_built.crash);
    built_stock(1215);
    c = healthy(123); c.drivetrain = 0;                 /* the car's own drivetrain row is gone */
    database(c, healthy(124)); make_setup(123, 0); game_builds(g_setup);
    built_stock(1215);
    /* A caller that asks for the stock parts itself (use-stock 1) gets the placeholder when they fail. */
    c = healthy(123); c.clutch = 0;
    database(c, healthy(124)); make_setup(123, 1); game_builds(g_setup);
    if (g_built.crash) printf("the game crashes: car 123's stock setup without a clutch row reached %s\n", g_built.crash);
    built_stock(1215);
    references_balanced();

    /* The placeholder itself fails: the game builds the setup as it is, as before (nothing is invented), and the log says so. */
    c = healthy(123); c.engine = 0;
    database(c, healthy(124)); g_cars[2].clutch = 0; make_setup(123, 0);
    g_log[0] = 0;
    game_builds(g_setup);
    CHECK(g_built.setup == g_setup && g_built.calls == 1);
    CHECK(strstr(g_log, "the placeholder car 1215 is unusable too (the clutch row is missing") != NULL);
    references_balanced();
    /* A setup or database the shim cannot read: built as it is, counted. */
    database(healthy(123), healthy(124));
    {
        build_fn entry = g_hooked_detour ? (build_fn)g_hooked_detour : native_build;
        void *nil = NULL, *ref = &g_db;
        unsigned char out[16];
        memset(&g_built, 0, sizeof g_built);
        entry(out, &nil, g_setup, NULL, NULL, 0, 0, 0);
        CHECK(g_built.calls == 0 || g_built.setup == g_setup);
        memset(&g_built, 0, sizeof g_built);
        CHECK(entry(out, &ref, NULL, NULL, NULL, 0, 0, 0) == 0x1234);
        CHECK(g_built.setup == NULL);
    }

    /* One line per car and caller: a screen that rebuilds a car every frame does not fill the log. */
    c = healthy(124); c.mod_parts = 0;
    database(healthy(123), c);
    g_log[0] = 0; g_lines = 0;
    for (unsigned n = 0; n < 50; ++n) { make_setup(124, 0); install_mod_part(124, 1); game_builds(g_setup); }
    CHECK(g_lines == 1);
    /* Cars the database does not know at all (a removed car pack): the placeholder, a line each up to 40, then one line that the rest are
     * only counted. */
    for (unsigned n = 0; n < 60; ++n) {
        make_setup(5000 + n, 0); game_builds(g_setup);
        CHECK(g_built.crash == NULL && g_built.car == 1215);
    }
    CHECK(strstr(g_log, "carphysics: car 5000 ") != NULL);
    CHECK(strstr(g_log, "the next ones are only counted") != NULL);
    CHECK(strstr(g_log, "carphysics: car 5059 ") == NULL);
    CHECK(g_lines <= 41);
#ifdef HAVE_GATE
    /* The callers, named by the return address each leaves (the ten calls to game+0x1056210 in FH2 1.0.0.64). */
    CHECK(!strcmp(pg_route(0x2d03fbu), "the garage rating"));
    CHECK(!strcmp(pg_route(0x346e1eu), "the upgrade parts preview"));
    CHECK(!strcmp(pg_route(0x375f50u), "the upgrade wizard's search") && !strcmp(pg_route(0x376327u), "the upgrade wizard's search"));
    CHECK(!strcmp(pg_route(0x37c2cdu), "the upgrade wizard's search") && !strcmp(pg_route(0x37c59eu), "the upgrade wizard's search"));
    CHECK(!strcmp(pg_route(0x37c8b8u), "the upgrade wizard's search"));
    CHECK(!strcmp(pg_route(0xaf1683u), "the car stats"));
    CHECK(!strcmp(pg_route(0xf671b7u), "a car's physics in the world"));
    CHECK(!strcmp(pg_route(0x1046d39u), "the auto upgrade"));
    CHECK(pg_route(0x2d03f6u) == NULL);
    CHECK(g_pg_hits > 0);
#endif
    if (g_failed) {
        printf("car physics: %d of %d checks FAIL\n", g_failed, g_checks);
        return 1;
    }
    printf("car physics: %d checks pass; every caller of the physics builder gets a car the database can build: its own parts, else its "
           "stock parts, else the placeholder's, with the default tune, the caller's setup untouched\n", g_checks);
    return 0;
}
