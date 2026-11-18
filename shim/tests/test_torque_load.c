/* Offline native-boundary fixtures. No game, sockets, assets or input. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#include "src/game_image.inc"
static unsigned logs;
static void log_begin(void) {}
static void log_raw(const char *s) { assert(s); ++logs; }
#include "src/torque_load_guard.inc"

typedef struct { void **vtable; unsigned refs, kind, count; } row;
static void *methods[42];
static row db, engine, camshaft, curve, motor, fallback_engine, fallback_camshaft, fallback_curve;
/* The other records 1056210 reads unchecked: one set for the car (123) and one for the placeholder (1215). */
static row drivetrain, body, clutch, tire, compound, fallback_drivetrain, fallback_body, fallback_clutch, fallback_tire, fallback_compound;
static unsigned count, calls, stock_calls, fallback_count;
static int no_engine, no_camshaft, no_curve, electric, wrong_owner, no_fallback, empty_curve, null_curve;
static int no_drivetrain, no_body, no_clutch, clutch_wrong_owner, no_tire, no_compound, fallback_no_clutch;
static int selecting_fallback, stock_invalid;
static unsigned char vehicle[0x500], job[0x1c8], singleton[0x48];
static void *job_ptr = job;
static unsigned checks;
static const char *key_text(const torque_key *key)
{
    assert(key->capacity >= key->size);
    const char *s = key->capacity < 16 ? key->text.small : key->text.ptr;
    assert(strlen(s) == key->size);
    return s;
}
static void addref(void *p) { ++((row *)p)->refs; }
static void release(void *p) { assert(((row *)p)->refs); --((row *)p)->refs; }
static int rows(void *p) { return (int)((row *)p)->count; }
static unsigned uint_field(void *p, const torque_key *key)
{
    row *r = p;
    const char *field = key_text(key);
    if (!strcmp(field, "EngineID")) {
        assert(r == &engine || r == &fallback_engine);
        return r == &engine ? 100 : 674;
    }
    if (!strcmp(field, "TorqueCurveFullThrottleID")) {
        assert(r == &camshaft || r == &fallback_camshaft);
        return r == &camshaft ? 4294967295u : 674000;
    }
    if (!strcmp(field, "DrivetrainID")) {
        assert(r == &drivetrain || r == &fallback_drivetrain);
        return r == &drivetrain ? 7 : 9;
    }
    if (!strcmp(field, "TireCompoundID")) {
        assert(r == &tire || r == &fallback_tire);
        return r == &tire ? 55 : 66;
    }
    assert(!strcmp(field, "NumTorqueValues"));
    assert(r == &curve || r == &fallback_curve);
    return r == &curve ? count : fallback_count;
}
static void **query(void *self, void **out, const torque_key *sql)
{
    assert(self == &db);
    const char *q = key_text(sql);
    if (!strcmp(q, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=4294967295"))
        *out = null_curve ? NULL : &curve;
    else if (!strcmp(q, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=674000"))
        *out = &fallback_curve;
    else if (!strcmp(q, "SELECT * FROM List_TireCompound WHERE TireCompoundID=55"))
        *out = no_compound ? NULL : &compound;
    else {
        assert(!strcmp(q, "SELECT * FROM List_TireCompound WHERE TireCompoundID=66"));
        *out = &fallback_compound;
    }
    if (*out) addref(*out);
    return out;
}
static void **car_part(void **out, void **ref, int slot, unsigned car, unsigned id)
{
    assert(*ref == &db && (slot == 0 || slot == 1 || slot == 2 || slot == 3 || slot == 8));
    selecting_fallback = car == 1215;
    if (slot == 0) {
        if (selecting_fallback) { assert(id == 1215000); *out = no_fallback ? NULL : &fallback_engine; }
        else { assert(car == 123); assert(id == 123000 || id == 999); *out = no_engine || id == 999 ? NULL : &engine; }
    } else if (slot == 3) {
        *out = !selecting_fallback && electric ? &motor : NULL;
    } else {
        /* Slot s is parts[s + 2]: the drivetrain (1), body (2) and tire compound (8) parts carry the car's own ids. */
        assert(id == (selecting_fallback ? 1215000u + (unsigned)slot : 123000u + (unsigned)slot));
        if (slot == 1) *out = selecting_fallback ? &fallback_drivetrain : no_drivetrain ? NULL : &drivetrain;
        else if (slot == 2) *out = selecting_fallback ? &fallback_body : no_body ? NULL : &body;
        else *out = selecting_fallback ? &fallback_tire : no_tire ? NULL : &tire;
    }
    release(*ref);
    if (*out) addref(*out);
    return out;
}
static void **engine_part(void **out, void **ref, int slot, unsigned owner, unsigned id)
{
    assert(*ref == &db && slot == 12);
    assert(owner == (selecting_fallback ? 674u : 100u));
    if (selecting_fallback) { assert(id == 674000); *out = &fallback_camshaft; }
    else { assert(id == 100000); *out = no_camshaft || wrong_owner ? NULL : &camshaft; }
    release(*ref);
    if (*out) addref(*out);
    return out;
}
static void **drivetrain_part(void **out, void **ref, int slot, unsigned owner, unsigned id)
{
    /* The clutch is drivetrain slot 30 (parts[32]), owned by the DrivetrainID of car part slot 1. */
    assert(*ref == &db && slot == 30);
    assert(owner == (selecting_fallback ? 9u : 7u));
    if (selecting_fallback) { assert(id == 1215030); *out = fallback_no_clutch ? NULL : &fallback_clutch; }
    else { assert(id == 123030); *out = no_clutch || clutch_wrong_owner ? NULL : &clutch; }
    release(*ref);
    if (*out) addref(*out);
    return out;
}
static void *parts_ctor(void *parts)
{
    memset(parts, 0xff, 45 * sizeof(unsigned));
    return parts;
}
static void set_records(unsigned *words, unsigned car)
{
    words[3] = car * 1000 + 1;      /* slot 1: drivetrain */
    words[4] = car * 1000 + 2;      /* slot 2: body */
    words[10] = car * 1000 + 8;     /* slot 8: tire compound */
    words[32] = car * 1000 + 30;    /* slot 30: clutch */
}
static void stock(void *parts, void **ref, unsigned car)
{
    assert(car == 123 || car == 1215);
    unsigned *words = parts;
    words[2] = car == 1215 ? 1215000 : stock_invalid ? 999 : 123000;
    words[14] = car == 1215 ? 674000 : 100000;
    set_records(words, car);
    ++stock_calls;
    release(*ref);
}
static void original(void *loader, void **ref)
{
    assert(loader == (void *)42 && ref == &job_ptr && *ref == job);
    /* The native unavailable-car branch keeps the old ordinal for optional
     * original-car physics. Rejecting that physics MUST disable this backup. */
    if (vehicle[0x3fd]) memcpy(vehicle + 0x400, vehicle + 0x118, 4);
    ++calls;
}
static row *const objects[] = {&db, &engine, &camshaft, &curve, &motor, &fallback_engine, &fallback_camshaft, &fallback_curve,
                               &drivetrain, &body, &clutch, &tire, &compound, &fallback_drivetrain, &fallback_body, &fallback_clutch,
                               &fallback_tire, &fallback_compound};
static void reset(void)
{
    no_engine = no_camshaft = no_curve = electric = wrong_owner = no_fallback = empty_curve = null_curve = stock_invalid = 0;
    no_drivetrain = no_body = no_clutch = clutch_wrong_owner = no_tire = no_compound = fallback_no_clutch = 0;
    selecting_fallback = 0;
    count = fallback_count = 81;
    calls = logs = stock_calls = 0;
    memset(vehicle, 0, sizeof vehicle); memset(job, 0, sizeof job);
    unsigned car = 123, words[45];
    unsigned none = 0xffffffffu;
    memcpy(vehicle + 0x400, &none, 4);
    parts_ctor(words); words[2] = 123000; words[14] = 100000; set_records(words, 123);
    memcpy(vehicle + 0x118, &car, 4); memcpy(vehicle + 0x120, words, sizeof words);
    void *v = vehicle;
    memcpy(job + 0x28, &v, sizeof v);
    for (unsigned i = 0; i < sizeof objects / sizeof objects[0]; ++i)
        *objects[i] = (row){methods, i == 0 ? 1u : 0u, i, 1};
}
static void check(int fallback, int cancel)
{
    unsigned char before[sizeof vehicle];
    LONG rejects = g_torque_rejects;
    memcpy(before, vehicle, sizeof before);
    torque_preload_guard((void *)42, &job_ptr);
    assert(calls == (unsigned)!cancel);
    assert(job[0x1c0] == (unsigned)cancel);
    assert(vehicle[0x3fd] == (unsigned)fallback);
    /* Native preloader owns actual car/stock/asset replacement; guard rejects
     * original physics restoration ONLY for invalid data. */
    before[0x3fd] = (unsigned char)fallback;
    if (calls && fallback) memcpy(before + 0x400, before + 0x118, 4);
    if (calls && g_torque_rejects != rejects) { unsigned none = 0xffffffffu; memcpy(before + 0x400, &none, 4); }
    assert(!memcmp(before, vehicle, sizeof before));
    assert(db.refs == 1);
    for (unsigned i = 1; i < sizeof objects / sizeof objects[0]; ++i) assert(!objects[i]->refs);
    ++checks;
}
/* The rule alone, for the reason it gives. */
static void reason(const char *why, unsigned detail)
{
    unsigned words[45];
    torque_fault fault = {0, 0};
    memcpy(words, vehicle + 0x120, sizeof words);
    assert(!torque_parts_valid(&db, 123, words, &fault));
    assert(fault.why && !strcmp(fault.why, why) && fault.detail == detail);
    assert(db.refs == 1);
    for (unsigned i = 1; i < sizeof objects / sizeof objects[0]; ++i) assert(!objects[i]->refs);
    ++checks;
}
int main(void)
{
    methods[2] = (void *)addref; methods[3] = (void *)release;
    methods[9] = (void *)query; methods[34] = (void *)uint_field; methods[41] = (void *)rows;
    g_torque_preload_orig = original; g_torque_stock = stock; g_torque_parts_ctor = parts_ctor;
    g_torque_car_part = car_part; g_torque_engine_part = engine_part; g_torque_drivetrain_part = drivetrain_part;
    g_torque_load_img = VirtualAlloc(NULL, 0x3550000, MEM_RESERVE, PAGE_NOACCESS);
    assert(g_torque_load_img);
    assert(VirtualAlloc(g_torque_load_img + 0x354f000, 4096, MEM_COMMIT, PAGE_READWRITE));
    void *s = singleton, *d = &db;
    memcpy(g_torque_load_img + 0x354f4f8, &s, sizeof s); memcpy(singleton + 0x40, &d, sizeof d);
    for (unsigned n = 0; n < 512; ++n) { reset(); count = n; check(n < 2 || n > 246, 0); }
    reset(); count = 0xffffffffu; check(1, 0);
    reset(); no_engine = 1; check(1, 0);
    reset(); no_camshaft = 1; check(1, 0);
    reset(); wrong_owner = 1; check(1, 0);
    reset(); null_curve = 1; check(1, 0);
    reset(); curve.count = 0; check(1, 0);
    reset(); no_engine = electric = 1; check(0, 0); /* Pure electric car: no ICE curve. */
    reset(); no_engine = electric = 1; count = 0; check(0, 0);
    reset(); vehicle[0x285] = 1; /* Native stock flag resolves IDs before validation. */
    memset(vehicle + 0x120, 0xff, 180); check(0, 0); assert(stock_calls == 1);
    reset(); vehicle[0x285] = 1; stock_invalid = 1; check(1, 0); assert(stock_calls == 2);
    reset(); count = 0; no_fallback = 1; check(0, 1); assert(logs);
    reset(); count = 0; fallback_count = 1; check(0, 1);
    reset(); vehicle[0x3fd] = 1; check(1, 0);
    reset(); vehicle[0x3fd] = 1; fallback_count = 0; check(1, 1);
    /* A car whose curve is fine but whose other records the builder reads unchecked are missing goes to the placeholder (#328: the
     * clutch; report 312f1d36e5a754e8: the tire compound row; #481: the drivetrain). */
    reset(); no_drivetrain = 1; check(1, 0);
    reset(); no_body = 1; check(1, 0);
    reset(); no_clutch = 1; check(1, 0);
    reset(); clutch_wrong_owner = 1; check(1, 0);
    reset(); no_tire = 1; check(1, 0);
    reset(); no_compound = 1; check(1, 0);
    reset(); compound.count = 0; check(1, 0);
    reset(); clutch.count = 0; check(1, 0);
    reset(); no_engine = electric = 1; no_clutch = 1; check(1, 0); /* An electric car needs its drivetrain records too. */
    reset(); no_clutch = 1; fallback_no_clutch = 1; check(0, 1); /* The placeholder itself must pass the same checks. */
    reset(); vehicle[0x3fd] = 1; fallback_no_clutch = 1; check(1, 1);
    reset(); no_drivetrain = 1; reason("the car's drivetrain part row is missing (wrong id or owner)", 123001);
    reset(); no_body = 1; reason("the car's body part row is missing (wrong id or owner)", 123002);
    reset(); no_clutch = 1; reason("the clutch row is missing for the car's drivetrain (wrong id or owner)", 123030);
    reset(); no_tire = 1; reason("the car's tire compound part row is missing (wrong id or owner)", 123008);
    reset(); no_compound = 1; reason("the tire compound part's TireCompoundID names no List_TireCompound row", 55);
    reset(); no_engine = 1; no_clutch = 1; reason("neither an engine row nor an electric motor row exists for the car's part ids", 123000);
    reset(); d = NULL; memcpy(singleton + 0x40, &d, sizeof d); check(0, 1);
    reset(); d = &db; memcpy(singleton + 0x40, &d, sizeof d);
    torque_preload_guard((void *)42, NULL); assert(!calls);
    void *nil = NULL; torque_preload_guard((void *)42, &nil); assert(!calls);
    DWORD old;
    assert(VirtualProtect(g_torque_load_img + 0x354f000, 4096, PAGE_NOACCESS, &old));
    check(0, 1);
    assert(VirtualFree(g_torque_load_img, 0, MEM_RELEASE));
    printf("torque load: %u checks pass; native fallback/cancellation boundary, stock/electric controls, unchecked part records and balanced references\n", checks);
    return 0;
}
