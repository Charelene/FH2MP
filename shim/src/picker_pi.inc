/* FH2 1.0.0.64: the Free Roam event car picker lists a heavily modified car as pickable although the server then kicks it for its class.
 *
 * Where the numbers come from [C: decompile of the Ghidra export, ghidra/export-named]. The picker (CarSelectLobby, CarSelectOnlineFreeRoam is one of
 * its classes) asks CarRestrictions_QueryCarAttributes (+2db240, FUN_1402db240) for the attributes of the garage car and gives them to
 * CarRestrictions_IsCarAllowed (+996240): CarSelectLobby::vfunc_186 (+cd0140, the car was clicked) and vfunc_187 (+ccec90). The same function fills the
 * attributes ForzaPlayerChoices::SetCarFromId (+2a0dc0) and the local choices (+2a0c30) cache for the lobby. It reads a CCarInGarage (vftable +2c5f818,
 * 0xab8 bytes), and copies from it: +0x40 the car id, +0x260 a float, the performance index in Data_Car's 0..1 scale (-> out+0x08 as a double), +0x2cc the
 * class id (-> out+0x22), power and weight, flags. Nothing is read from the lobby objects the server resets, which is why the server's reset of the car
 * never reaches the picker.
 *
 * +0x260 and +0x2cc are not stored values. CCarInGarage's constructor (+2be790) reads the part ids of the garage row (43 slots in CarPartSlotTable order,
 * then WheelStyle) into the car at +0x48 (header word, WheelStyle at +0x4c, the 43 part ids from +0x50), then calls SaveUpgradeIdsToDb (+2f5120), which
 * calls UpdatePerformanceColumns (+2cfa50): it simulates the car from those part ids (CarPerformance_BuildSnapshotForGarageCar +2d0160) and writes the
 * result into +0x260..+0x2cc (and into the garage row). A part id that no part table has gives the simulation nothing to work with, so the class and PI
 * it ends with are whatever the simulation left; a NaN PI passes IsCarAllowed's range tests (both are `<` / `<=` comparisons that NaN fails).
 *
 * This hooks QueryCarAttributes itself, so the picker, the choices and everything else that asks get the same answer. It lets the game fill the
 * attributes, then looks at the car: when one of its 43 part ids is in no part table of the game database (the same rule as native/internal/parts
 * must_replace: the camshaft and tire slots must hold a row of their own table, any other slot a row of any table; slot 0 may hold an electric
 * car's Motor row), or its PI is not a usable number (NaN, negative, huge), it replaces class and PI in the attributes by the stock car's, the
 * Data_Car row of the car id (ClassID, PerformanceIndex), which is what the server's repaired car starts from. Everything else about the car, and every
 * car without that problem, is passed through untouched. The garage row, the car object and the save are never written.
 *
 * Safe by default: the function's first bytes are checked before the hook goes in. At a call, anything unexpected leaves the game's attributes as they
 * are, with one line saying which: the car is not a CCarInGarage, a field or a database object is unreadable or a method is not code, the attributes
 * do not carry the values the car holds (the layout is checked before a byte is written), the database gives no stock row. Every pointer is read through
 * read_game_memory, every database method is checked to be committed executable memory. The decision per (car, part ids) is cached.
 * FH2MP_NO_PICKERPI=1 turns it off.
 * Evidence level: the data flow above is confirmed from the decompile; that a real car with an unknown part is rejected by the picker after this is
 * NOT verified in a running game, and the PI scale and class numbering of Data_Car equal the car's (inferred: PlayerChoices fills both from the Data_Car
 * row, +2a1010 InitFromCarRow, and native/internal/catalog relies on it). */
#define PPI_FN_RVA 0x2db240u
#define PPI_VTABLE_RVA 0x2c5f818u       /* CCarInGarage::vftable */
#define PPI_HOLDER_RVA 0x354f4f8u       /* g_GameDatabaseHolder: +0x40 the database (as torque_load_guard.inc) */
#define PPI_HOLDER_DB_OFFSET 0x40u
#define PPI_DB_QUERY_SLOT 0x48u         /* database vtable: query(sql as the game's string) -> row set in *out */
#define PPI_ROW_RELEASE_SLOT 0x18u
#define PPI_ROW_COUNT_SLOT 0x148u
#define PPI_ROW_UINT_SLOT 0x110u        /* by column name, the current row */
#define PPI_ROW_FLOAT_SLOT 0x130u       /* by column name, returns float in xmm0 (GetDouble +1090bf0 ends in vcvtsd2ss) */
#define PPI_CAR_ID 0x40u
#define PPI_CAR_PARTS 0x50u
#define PPI_CAR_PI 0x260u
#define PPI_CAR_CLASS 0x2ccu
#define PPI_OUT_SIZE 0x30u
#define PPI_OUT_PI 0x08u
#define PPI_OUT_ID 0x1eu
#define PPI_OUT_CLASS 0x22u
#define PPI_SLOTS 43
#define PPI_NONE 0xffffffffu
#define PPI_CACHE 8
#define PPI_LOG_MAX 20

/* Table names (after List_Upgrade) in CarPartSlotTable order; NULL: the slot holds no part type (3 Motor, 26 QuadTurbo: -1 in every unmodified game). */
static const char *const g_ppi_table[PPI_SLOTS] = {
    "Engine", "Drivetrain", "CarBody", NULL, "Brakes", "SpringDamper", "AntiSwayFront", "AntiSwayRear", "TireCompound", "RearWing", "RimSizeFront",
    "RimSizeRear", "EngineCamshaft", "EngineValves", "EngineDisplacement", "EnginePistonsCompression", "EngineFuelSystem", "EngineIgnition",
    "EngineExhaust", "EngineIntake", "EngineFlywheel", "EngineManifold", "EngineRestrictorPlate", "EngineOilCooling", "EngineTurboSingle",
    "EngineTurboTwin", NULL, "EngineCSC", "EngineDSC", "EngineIntercooler", "DrivetrainClutch", "DrivetrainTransmission", "DrivetrainDriveline",
    "DrivetrainDifferential", "CarBodyFrontBumper", "CarBodyRearBumper", "CarBodyHood", "CarBodySideSkirt", "CarBodyTireWidthFront",
    "CarBodyTireWidthRear", "CarBodyWeight", "CarBodyChassisStiffness", "MotorParts"};
static const char g_ppi_motor[] = "Motor";

/* What the decision needs from the game; the hook fills it from the game database, the test with a fake. */
typedef struct {
    /* 1 present, 0 absent, -1 the database could not answer */
    int (*exists)(void *ctx, const char *table, unsigned id);
    /* 1 with the stock class and PI of the car, 0 no such car, -1 the database could not answer */
    int (*stock)(void *ctx, unsigned car, unsigned *klass, float *pi);
    void *ctx;
} ppi_ops;

enum { PPI_CLEAN, PPI_UNKNOWN_PART, PPI_WRONG_TABLE, PPI_BAD_PI, PPI_NO_ANSWER };
typedef struct { int kind; int slot; unsigned id; } ppi_verdict;

static int ppi_pi_usable(double pi)
{
    return pi >= 0.0 && pi < 1.0e6;     /* false for NaN */
}

/* 1 when `w` is a row of some part table other than slot k's own (the Motor table too), 0 when of none, -1 when the database could not say */
static int ppi_elsewhere(const ppi_ops *ops, int k, unsigned w)
{
    int j, r = ops->exists(ops->ctx, g_ppi_motor, w);

    for (j = 0; j < PPI_SLOTS && r == 0; j++) {
        if (g_ppi_table[j] && j != k) {
            r = ops->exists(ops->ctx, g_ppi_table[j], w);
        }
    }
    return r;
}

/* The first of the 43 part ids no part table accepts (native/internal/parts MustReplace): a slot holds -1, or a row of its own table, or (all slots but
 * the camshaft 12 and the tire 8) a row of another table, which the server only reports. PPI_UNKNOWN_PART: in no table at all. PPI_WRONG_TABLE: a
 * camshaft or tire slot holding another table's row. PPI_NO_ANSWER: the database could not say. Else PPI_CLEAN. */
static ppi_verdict ppi_check_parts(const unsigned *parts, const ppi_ops *ops)
{
    ppi_verdict v = {PPI_CLEAN, -1, 0};
    int k;

    for (k = 0; k < PPI_SLOTS; k++) {
        unsigned w = parts[k];
        int own, other;

        if (w == PPI_NONE || !g_ppi_table[k]) {
            continue;
        }
        own = ops->exists(ops->ctx, g_ppi_table[k], w);
        if (own > 0 || (k == 0 && own == 0 && ops->exists(ops->ctx, g_ppi_motor, w) > 0)) {
            continue;                       /* slot 0: an electric car's Motor row is as good as an Engine row */
        }
        other = own < 0 ? -1 : ppi_elsewhere(ops, k, w);
        if (other < 0) {
            v.kind = PPI_NO_ANSWER;
        } else if (other == 0) {
            v.kind = PPI_UNKNOWN_PART;
        } else if (k == 8 || k == 12) {
            v.kind = PPI_WRONG_TABLE;
        } else {
            continue;
        }
        v.slot = k; v.id = w;
        return v;
    }
    return v;
}

/* ---- the decision cache: one answer per (car, part ids) ---- */
typedef struct { int used; unsigned car, hash; ppi_verdict v; } ppi_entry;
static ppi_entry g_ppi_cache[PPI_CACHE];
static unsigned g_ppi_next;
static SRWLOCK g_ppi_lock = SRWLOCK_INIT;
static volatile LONG g_ppi_logged, g_ppi_declined;

static unsigned ppi_hash(const unsigned *parts)
{
    unsigned h = 2166136261u;
    int k;
    for (k = 0; k < PPI_SLOTS; k++) {
        h = (h ^ parts[k]) * 16777619u;
    }
    return h;
}

static int ppi_cache_get(unsigned car, unsigned hash, ppi_verdict *v)
{
    int i, found = 0;
    AcquireSRWLockExclusive(&g_ppi_lock);
    for (i = 0; i < PPI_CACHE; i++) {
        if (g_ppi_cache[i].used && g_ppi_cache[i].car == car && g_ppi_cache[i].hash == hash) {
            *v = g_ppi_cache[i].v;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_ppi_lock);
    return found;
}

static void ppi_cache_put(unsigned car, unsigned hash, const ppi_verdict *v)
{
    AcquireSRWLockExclusive(&g_ppi_lock);
    g_ppi_cache[g_ppi_next % PPI_CACHE].used = 1;
    g_ppi_cache[g_ppi_next % PPI_CACHE].car = car;
    g_ppi_cache[g_ppi_next % PPI_CACHE].hash = hash;
    g_ppi_cache[g_ppi_next % PPI_CACHE].v = *v;
    g_ppi_next++;
    ReleaseSRWLockExclusive(&g_ppi_lock);
}

/* ---- logging: what happened, which car, claimed against reported, why; a rate limit; a decline is one line per kind of reason ---- */
static void ppi_log_pi(double pi)
{
    unsigned long v;
    if (!(pi >= 0.0 && pi < 1.0e6)) {
        log_raw(pi != pi ? "NaN" : "out of range");
        return;
    }
    v = (unsigned long)(pi * 10000.0 + 0.5);
    log_dec(v / 10000);
    log_raw(".");
    log_dec(v % 10000 / 1000);
    log_dec(v % 1000 / 100);
    log_dec(v % 100 / 10);
    log_dec(v % 10);
}

static void ppi_log_sub(unsigned car, unsigned claimed_class, double claimed_pi, unsigned klass, double pi, const ppi_verdict *v)
{
    LONG n = InterlockedIncrement(&g_ppi_logged);
    if (n > PPI_LOG_MAX && n % 50) {
        return;
    }
    log_begin();
    log_raw("hook: pickerpi: car ");
    log_dec(car);
    log_raw(" reported with its stock class ");
    log_dec(klass);
    log_raw(" and PI ");
    ppi_log_pi(pi);
    log_raw(" instead of the garage car's class ");
    log_dec(claimed_class);
    log_raw(" and PI ");
    ppi_log_pi(claimed_pi);
    if (v->kind == PPI_BAD_PI) {
        log_raw(": the garage car's PI is not a usable number");
    } else {
        log_raw(": slot ");
        log_dec((unsigned long)v->slot);
        log_raw(" holds part id ");
        log_dec(v->id);
        log_raw(v->kind == PPI_WRONG_TABLE ? ", which is a row of another part table" : ", which is in no part table");
    }
    log_raw(" (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

static void ppi_log_decline(const char *what, unsigned car)
{
    LONG n = InterlockedIncrement(&g_ppi_declined);
    if (n > 5 && n % 100) {
        return;
    }
    log_begin();
    log_raw("hook: pickerpi: attributes of car ");
    log_dec(car);
    log_raw(" left as the game made them: ");
    log_raw(what);
    log_raw(" (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

/* The whole decision on what the game computed. `parts` are the car's 43 part ids, claimed_* the car's class and PI, out the attributes the game
 * filled (PPI_OUT_SIZE bytes, writable). -> 1 when class and PI in `out` were replaced. */
static int ppi_fixup(unsigned car, const unsigned *parts, unsigned claimed_class, float claimed_pi, unsigned char *out, const ppi_ops *ops)
{
    ppi_verdict v;
    unsigned klass = 0;
    float pi = 0;
    double dpi;
    unsigned hash = ppi_hash(parts);
    short out_id;
    double out_pi;
    int r;

    memcpy(&out_id, out + PPI_OUT_ID, sizeof out_id);
    memcpy(&out_pi, out + PPI_OUT_PI, sizeof out_pi);
    dpi = (double)claimed_pi;
    /* the layout is checked before a byte is written: the attributes must say what the car holds */
    if (out_id != (short)car || out[PPI_OUT_CLASS] != (unsigned char)claimed_class || memcmp(&out_pi, &dpi, sizeof dpi)) {
        ppi_log_decline("the attributes do not carry the car's id, class and PI, so their layout is not as expected", car);
        return 0;
    }
    if (!ppi_cache_get(car, hash, &v)) {
        v = ppi_check_parts(parts, ops);
        if (v.kind != PPI_NO_ANSWER) {
            ppi_cache_put(car, hash, &v);
        }
    }
    if (v.kind == PPI_NO_ANSWER) {
        ppi_log_decline("the game database could not be queried", car);
        return 0;
    }
    if (v.kind == PPI_CLEAN && !ppi_pi_usable(dpi)) {
        v.kind = PPI_BAD_PI; v.slot = -1; v.id = 0;
    }
    if (v.kind == PPI_CLEAN) {
        return 0;
    }
    r = ops->stock(ops->ctx, car, &klass, &pi);
    if (r <= 0 || klass > 255 || !ppi_pi_usable((double)pi)) {
        ppi_log_decline(r < 0 ? "the game database could not be queried for the stock car" : "the database has no stock class and PI for the car", car);
        return 0;
    }
    if (klass == claimed_class && (double)pi == dpi) {
        return 0;
    }
    out[PPI_OUT_CLASS] = (unsigned char)klass;
    dpi = (double)pi;
    memcpy(out + PPI_OUT_PI, &dpi, sizeof dpi);
    ppi_log_sub(car, claimed_class, (double)claimed_pi, klass, dpi, &v);
    return 1;
}

/* ---- the game database ---- */
/* MSVC 2012 std::string as the game's database methods take it (as torque_load_guard.inc) */
typedef struct { union { char small[16]; const char *ptr; } text; size_t size, capacity; } ppi_key;

static ppi_key ppi_key_for(const char *text)
{
    ppi_key key;
    memset(&key, 0, sizeof key);
    key.size = (size_t)lstrlenA(text);
    if (key.size < 16) {
        memcpy(key.text.small, text, key.size + 1);
        key.capacity = 15;
    } else {
        key.text.ptr = text;
        key.capacity = key.size;
    }
    return key;
}

/* 1 when [p, p+n) is committed memory the CPU may execute */
static int ppi_is_code(const void *p)
{
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return 0;
    }
    return (m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

/* The method at byte offset `slot` of obj's vtable, or NULL when obj, its vtable or the method is not readable memory / code. */
static void *ppi_method(void *obj, unsigned slot)
{
    void *vt = NULL, *fn = NULL;
    if (!obj || !read_game_memory(obj, &vt, sizeof vt) || !vt || !read_game_memory((const unsigned char *)vt + slot, &fn, sizeof fn) || !ppi_is_code(fn)) {
        return NULL;
    }
    return fn;
}

typedef struct { void *db; } ppi_dbctx;

/* Runs `sql`; -> the row set (release it with ppi_release) or NULL. */
static void *ppi_query(void *db, const char *sql)
{
    void *row = NULL;
    ppi_key key = ppi_key_for(sql);
    void *fn = ppi_method(db, PPI_DB_QUERY_SLOT);
    if (!fn) {
        return NULL;
    }
    ((void **(*)(void *, void **, const ppi_key *))fn)(db, &row, &key);
    return row;
}

static void ppi_release(void *row)
{
    void *fn = ppi_method(row, PPI_ROW_RELEASE_SLOT);
    if (fn) {
        ((void (*)(void *))fn)(row);
    }
}

/* the number of rows, or -1 when the row set cannot be asked */
static int ppi_rows(void *row)
{
    void *fn = ppi_method(row, PPI_ROW_COUNT_SLOT);
    return fn ? ((int (*)(void *))fn)(row) : -1;
}

/* head + table + mid + decimal id into sql[size]; -> 1, 0 when it does not fit */
static int ppi_sql(char *sql, unsigned size, const char *head, const char *table, const char *mid, unsigned id)
{
    char digits[12];
    unsigned len = 0, pos = 0, i;

    if (lstrlenA(head) + lstrlenA(table) + lstrlenA(mid) + 11 > (int)size) {
        return 0;
    }
    while (*head) sql[pos++] = *head++;
    while (*table) sql[pos++] = *table++;
    while (*mid) sql[pos++] = *mid++;
    do { digits[len++] = (char)('0' + id % 10); id /= 10; } while (id);
    for (i = len; i > 0; i--) sql[pos++] = digits[i - 1];
    sql[pos] = 0;
    return 1;
}

static int ppi_db_exists(void *ctx, const char *table, unsigned id)
{
    char sql[160];
    void *row;
    int n;
    if (!ppi_sql(sql, sizeof sql, "SELECT Id FROM List_Upgrade", table, " WHERE Id=", id)) {
        return -1;
    }
    row = ppi_query(((ppi_dbctx *)ctx)->db, sql);
    if (!row) {
        return -1;
    }
    n = ppi_rows(row);
    ppi_release(row);
    return n < 0 ? -1 : n > 0;
}

static int ppi_db_stock(void *ctx, unsigned car, unsigned *klass, float *pi)
{
    char sql[160];
    void *row, *fn_u, *fn_f;
    int n, ok = 0;
    ppi_key k_class = ppi_key_for("ClassID"), k_pi = ppi_key_for("PerformanceIndex");
    if (!ppi_sql(sql, sizeof sql, "SELECT ClassID, PerformanceIndex FROM Data_Car WHERE Id=", "", "", car)) {
        return -1;
    }
    row = ppi_query(((ppi_dbctx *)ctx)->db, sql);
    if (!row) {
        return -1;
    }
    n = ppi_rows(row);
    fn_u = ppi_method(row, PPI_ROW_UINT_SLOT);
    fn_f = ppi_method(row, PPI_ROW_FLOAT_SLOT);
    if (n < 0 || !fn_u || !fn_f) {
        ppi_release(row);
        return -1;
    }
    if (n > 0) {
        *klass = ((unsigned (*)(void *, const ppi_key *))fn_u)(row, &k_class);
        *pi = ((float (*)(void *, const ppi_key *))fn_f)(row, &k_pi);
        ok = 1;
    }
    ppi_release(row);
    return ok;
}

/* ---- the hook ---- */
typedef unsigned char *(*ppi_query_fn)(void *car, unsigned char *out);
static ppi_query_fn g_ppi_orig;
static const unsigned char *g_ppi_img;

static unsigned char *ppi_hook(void *car, unsigned char *out)
{
    unsigned char *ret = g_ppi_orig(car, out);
    unsigned char attrs[PPI_OUT_SIZE];
    unsigned parts[PPI_SLOTS], id = 0, klass = 0;
    float pi = 0;
    void *vt = NULL, *holder = NULL;
    ppi_dbctx ctx;
    ppi_ops ops;
    const unsigned char *c = (const unsigned char *)car;

    if (!car || !out || !g_ppi_img) {
        return ret;
    }
    if (!read_game_memory(c, &vt, sizeof vt) || vt != (const void *)(g_ppi_img + PPI_VTABLE_RVA)) {
        ppi_log_decline("the object asked about is not a garage car (CCarInGarage)", 0);
        return ret;
    }
    if (!read_game_memory(c + PPI_CAR_ID, &id, sizeof id) || !read_game_memory(c + PPI_CAR_PARTS, parts, sizeof parts) ||
        !read_game_memory(c + PPI_CAR_PI, &pi, sizeof pi) || !read_game_memory(c + PPI_CAR_CLASS, &klass, sizeof klass) ||
        !read_game_memory(out, attrs, sizeof attrs)) {
        ppi_log_decline("the car's id, part ids, class or PI, or the attribute block, cannot be read", id);
        return ret;
    }
    if (!read_game_memory(g_ppi_img + PPI_HOLDER_RVA, &holder, sizeof holder) || !holder ||
        !read_game_memory((const unsigned char *)holder + PPI_HOLDER_DB_OFFSET, &ctx.db, sizeof ctx.db) || !ctx.db) {
        ppi_log_decline("the game database object (game+0x354f4f8, +0x40) is not there", id);
        return ret;
    }
    ops.exists = ppi_db_exists;
    ops.stock = ppi_db_stock;
    ops.ctx = &ctx;
    memcpy(attrs, out, sizeof attrs);                  /* ppi_fixup edits a copy; written back below only when it changed it */
    if (ppi_fixup(id, parts, klass, pi, attrs, &ops)) {
        memcpy(out + PPI_OUT_PI, attrs + PPI_OUT_PI, 8);
        out[PPI_OUT_CLASS] = attrs[PPI_OUT_CLASS];
    }
    return ret;
}

static int patch_picker_pi(void)
{
    static const unsigned char prologue[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x50};
    unsigned char *img = find_fh2_image();

    if (!img || !code_is("pickerpi", "CarRestrictions_QueryCarAttributes", img, PPI_FN_RVA, prologue, sizeof prologue)) {
        return 0;
    }
    g_ppi_img = img;
    if (!plat_hook("pickerpi: garage car attributes", img + PPI_FN_RVA, (void *)ppi_hook, (void **)&g_ppi_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: pickerpi: the garage car's class and PI for the car pickers are the stock car's when a part id is in no part table (game+0x2db240)\n");
    return 1;
}
