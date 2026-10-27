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
static unsigned count, calls, stock_calls, fallback_count;
static int no_engine, no_camshaft, no_curve, electric, wrong_owner, no_fallback, empty_curve, null_curve;
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
    else {
        assert(!strcmp(q, "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=674000"));
        *out = &fallback_curve;
    }
    if (*out) addref(*out);
    return out;
}
static void **car_part(void **out, void **ref, int slot, unsigned car, unsigned id)
{
    assert(*ref == &db && (slot == 0 || slot == 3));
    selecting_fallback = car == 1215;
    if (slot == 0) {
        if (selecting_fallback) { assert(id == 1215000); *out = no_fallback ? NULL : &fallback_engine; }
        else { assert(car == 123); assert(id == 123000 || id == 999); *out = no_engine || id == 999 ? NULL : &engine; }
    } else {
        *out = !selecting_fallback && electric ? &motor : NULL;
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
static void *parts_ctor(void *parts)
{
    memset(parts, 0xff, 45 * sizeof(unsigned));
    return parts;
}
static void stock(void *parts, void **ref, unsigned car)
{
    assert(car == 123 || car == 1215);
    unsigned *words = parts;
    words[2] = car == 1215 ? 1215000 : stock_invalid ? 999 : 123000;
    words[14] = car == 1215 ? 674000 : 100000;
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
static void reset(void)
{
    no_engine = no_camshaft = no_curve = electric = wrong_owner = no_fallback = empty_curve = null_curve = stock_invalid = 0;
    selecting_fallback = 0;
    count = fallback_count = 81;
    calls = logs = stock_calls = 0;
    memset(vehicle, 0, sizeof vehicle); memset(job, 0, sizeof job);
    unsigned car = 123, words[45];
    unsigned none = 0xffffffffu;
    memcpy(vehicle + 0x400, &none, 4);
    parts_ctor(words); words[2] = 123000; words[14] = 100000;
    memcpy(vehicle + 0x118, &car, 4); memcpy(vehicle + 0x120, words, sizeof words);
    void *v = vehicle;
    memcpy(job + 0x28, &v, sizeof v);
    row *objects[] = {&db,&engine,&camshaft,&curve,&motor,&fallback_engine,&fallback_camshaft,&fallback_curve};
    for (unsigned i = 0; i < sizeof objects / sizeof objects[0]; ++i)
        *objects[i] = (row){methods, i == 0 ? 1u : 0u, i, 1};
}
static void check(int fallback, int cancel)
{
    unsigned char before[sizeof vehicle];
    memcpy(before, vehicle, sizeof before);
    torque_preload_guard((void *)42, &job_ptr);
    assert(calls == (unsigned)!cancel);
    assert(job[0x1c0] == (unsigned)cancel);
    assert(vehicle[0x3fd] == (unsigned)fallback);
    /* Native preloader owns actual car/stock/asset replacement; guard rejects
     * original physics restoration ONLY for invalid data. */
    before[0x3fd] = (unsigned char)fallback;
    if (calls && fallback) memcpy(before + 0x400, before + 0x118, 4);
    if (calls && logs) { unsigned none = 0xffffffffu; memcpy(before + 0x400, &none, 4); }
    assert(!memcmp(before, vehicle, sizeof before));
    assert(db.refs == 1 && !engine.refs && !camshaft.refs && !curve.refs && !motor.refs &&
           !fallback_engine.refs && !fallback_camshaft.refs && !fallback_curve.refs);
    ++checks;
}
int main(void)
{
    methods[2] = (void *)addref; methods[3] = (void *)release;
    methods[9] = (void *)query; methods[34] = (void *)uint_field; methods[41] = (void *)rows;
    g_torque_preload_orig = original; g_torque_stock = stock; g_torque_parts_ctor = parts_ctor;
    g_torque_car_part = car_part; g_torque_engine_part = engine_part;
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
    reset(); d = NULL; memcpy(singleton + 0x40, &d, sizeof d); check(0, 1);
    reset(); d = &db; memcpy(singleton + 0x40, &d, sizeof d);
    torque_preload_guard((void *)42, NULL); assert(!calls);
    void *nil = NULL; torque_preload_guard((void *)42, &nil); assert(!calls);
    DWORD old;
    assert(VirtualProtect(g_torque_load_img + 0x354f000, 4096, PAGE_NOACCESS, &old));
    check(0, 1);
    assert(VirtualFree(g_torque_load_img, 0, MEM_RELEASE));
    printf("torque load: %u checks pass; native fallback/cancellation boundary, stock/electric controls and balanced references\n", checks);
    return 0;
}
