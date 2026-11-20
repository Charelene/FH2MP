/* FH2 1.0.0.64, online events: a car whose engine, gearbox or another part belongs to a different car, engine or drivetrain (FH2 Extra Options by
 * RokkuDayo lets a player install any engine or transmission; the PI the game computes from those parts moves the car into another class) is judged, listed and raced
 * with only those parts back to the car's own stock ones and the tune values that belong to them back to stock (user decisions 2026-11-19: drop a
 * car the fix moves out of the event's class from the event's car list, not even a tile; an any-class event lists it; it always races fixed).
 *
 * Where [C: decompile of the Ghidra export, ghidra/export-named; bytes checked against the 1.0.0.64 exe]:
 * - The rule (which parts, which stock parts, which tune values): event_fix_rule.inc, from the game database's List_Upgrade tables, loaded once per
 *   game (one query per part table, about 32,000 rows) at the first event car list.
 * - The class and PI of the fixed car come from the game's own simulation: CarPerformance_BuildSnapshotForGarageCar (+2d0160; out buffer, car id, the
 *   45-word part block: -1, WheelStyle, the 43 part ids) reads the car's Data_Car row and stock brakes, builds the car setup from those parts
 *   (CarSetup_BuildFromDatabase) and runs CarPerformance_SimulateAndComputeRatings into the ratings block [[+365d700]], which
 *   CCarInGarage::UpdatePerformanceColumns (+2cfa50) reads right after: PI at [0x17] (-> the car's +0x260), class at [0x1d] (-> +0x2cc). This saves the
 *   first 0x900 bytes of that block, calls the function on a copy of the block with the fixed parts, reads both values and puts the block back. The
 *   garage car, its row and the save are never written. One answer per (car, part ids), cached; only cars with a foreign part are simulated.
 * - The event car list: a car collection runs its list query through +140c3f0 (collection, sql, &database, ?; the text is copied at once).
 *   GarageCarCollection::vfunc_16 (+c2ec60) builds "SELECT Garage.* , 0 AS IsOwned ... FROM <garage table> AS Garage ... WHERE <filter> ..." with the
 *   event's class and PI terms in <filter> (Garage.ClassId, Garage.PerformanceIndex; CarFilter_BuildGarageWhereClause +984850,
 *   CarCollection_AppendPerfRatingRestriction +c16500). In a network session such a query (one with those terms) is run with each of them replaced by
 *   the fixed values of the garage's cars that have one (event_fix_rule.inc ef_rewrite_sql), so the list takes or drops them by the fixed car. The
 *   garage's part ids come from one query of the garage table, by the columns CCarInGarage's constructor (+2be790) reads: CarId, WheelStyle and the
 *   43 short names of CarPartSlotTable (+2e797f0, 32-byte entries, +8 the name).
 * - The car's details while the event car select runs: QueryCarAttributes (picker_pi.inc) gives the fixed class and PI (ef_attrs), so the game's
 *   own "may this car enter" test (+996240) and the details agree with the list.
 * - The race: event_stock.inc's commit hook (ForzaPlayerChoices slot 59, +28d9a0) puts the fixed parts, the stock tune values and the fixed class and
 *   PI into the pending block before the game commits it to the synced PlayerChoices (ef_commit), which is what the other players and the server get.
 * Safe by default: each function's first bytes are checked before it is called or hooked; anything unexpected (a table or the garage cannot be read,
 * the simulation gives an unusable class or PI, a car cannot be fixed) leaves the car as the player built it, with one line saying which.
 * Evidence: the data flow is confirmed from the decompile; that the list, the details and the commit behave as described in a running game, and the
 * time the rescoring takes, are NOT verified (every list and commit logs it). FH2MP_NO_EVENTFIX=1 turns it off. */
#include "event_fix_rule.inc"

#define EF_SNAPSHOT_RVA 0x2d0160u
#define EF_RATINGS_RVA 0x365d700u
#define EF_RATINGS_PI 0x5cu
#define EF_RATINGS_CLASS 0x74u
#define EF_RATINGS_SAVE 0x900u
#define EF_SNAPSHOT_OUT 0x4000u        /* the snapshot copies 14,548 bytes out (UpdatePerformanceColumns gives it 14,544 + the dword after) */
#define EF_QUERY_RVA 0x140c3f0u
#define EF_SLOT_TABLE_RVA 0x2e797f0u
#define EF_ROW_INT_AT 0xa0u            /* record set: int (row set, row index, column name, default) [C: the row loops at +21xxxx, the constructor] */
#define EF_CACHE 512
#define EF_SQL_MAX (256u * 1024u)

typedef void *(*ef_snapshot_fn)(void *out, unsigned car, const unsigned *block);
typedef void (*ef_query_fn)(void *collection, const char *sql, void *database, void *extra);

typedef struct { unsigned id, owner, installs; unsigned char stock; } ef_own;
typedef struct { unsigned owner, id, order; } ef_stk;
typedef struct { ef_own *rows; unsigned n; ef_stk *stock; unsigned ns; } ef_table;
typedef struct { unsigned car, hash, kind, klass; float pi; unsigned long long changed; unsigned fixed[EF_SLOTS]; } ef_verdict;

static ef_table g_ef_tab[EF_SLOTS];
static volatile LONG g_ef_state;                /* 0 not loaded, 1 loaded, -1 the tables could not be read (off for this game) */
static int g_ef_on;
static ef_snapshot_fn g_ef_snapshot;
static ef_query_fn g_ef_query_orig;
static char g_ef_short[EF_SLOTS][40];           /* the garage table's part columns */
static int g_ef_short_ok;
static ef_verdict g_ef_cache[EF_CACHE];
static unsigned g_ef_cache_n, g_ef_cache_next;
static unsigned char *g_ef_snap_out;
static unsigned char g_ef_save[EF_RATINGS_SAVE];
static volatile LONG g_ef_declined, g_ef_lists, g_ef_commits, g_ef_busy;
static volatile LONG64 g_ef_fixed_at;            /* tick of the last commit this file changed (the PI check hook below acts only after one) */

/* No thread ever waits on this file while it calls into the game (both rigs hung at their first event car select on 2026-11-19 with a lock held
 * across the game's database calls, which take the database's own critical section, CDatabase slot 8 +108f720; the lock order could invert).
 * - The work (loading the part tables, judging cars: the game's database and simulation) is taken with a flag, never waited for: a thread that
 *   finds it taken (another thread, or the game calling back into a hook while the work runs) leaves its list or car as the game has it.
 * - The verdict cache has its own lock, held only to copy an entry in or out, with no call into the game inside. */
static volatile LONG g_ef_work;
static SRWLOCK g_ef_cache_lock = SRWLOCK_INIT;

static int ef_work_begin(void)
{
    if (InterlockedCompareExchange(&g_ef_work, 1, 0) == 0) {
        return 1;
    }
    {
        LONG n = InterlockedIncrement(&g_ef_busy);
        if (n <= 5 || n % 100 == 0) {
            log_begin();
            log_raw("eventfix: a car list or car arrived while another was being judged; it is left as the game has it (#");
            log_dec((unsigned long)n);
            log_raw(")\n");
        }
    }
    return 0;
}

static void ef_work_end(void)
{
    InterlockedExchange(&g_ef_work, 0);
}

static unsigned ef_hash(unsigned wheel, const unsigned parts[EF_SLOTS])
{
    unsigned hash = 2166136261u ^ wheel, i;
    for (i = 0; i < EF_SLOTS; i++) {
        hash = (hash ^ parts[i]) * 16777619u;
    }
    return hash;
}

static int ef_cache_get(unsigned car, unsigned hash, ef_verdict *out)
{
    unsigned i;
    int found = 0;
    AcquireSRWLockShared(&g_ef_cache_lock);
    for (i = 0; i < g_ef_cache_n; i++) {
        if (g_ef_cache[i].car == car && g_ef_cache[i].hash == hash) {
            *out = g_ef_cache[i];
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&g_ef_cache_lock);
    return found;
}

static void ef_cache_put(const ef_verdict *v)
{
    AcquireSRWLockExclusive(&g_ef_cache_lock);
    g_ef_cache[g_ef_cache_next] = *v;
    g_ef_cache_next = (g_ef_cache_next + 1) % EF_CACHE;
    if (g_ef_cache_n < EF_CACHE) g_ef_cache_n++;
    ReleaseSRWLockExclusive(&g_ef_cache_lock);
}

static void ef_cat_dec(char *d, unsigned cap, unsigned v)
{
    char r[12], s[12];
    unsigned n = 0, i;
    do { r[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) s[i] = r[n - 1 - i];
    s[n] = 0;
    {
        unsigned len = 0;
        while (d[len]) len++;
        for (i = 0; s[i] && len + 1 < cap; i++) d[len++] = s[i];
        d[len] = 0;
    }
}

static void ef_log_decline(const char *what, unsigned car)
{
    LONG n = InterlockedIncrement(&g_ef_declined);
    if (n > 10 && n % 100) {
        return;
    }
    log_begin();
    log_raw("eventfix: ");
    if (car) {
        log_raw("car ");
        log_dec(car);
        log_raw(" left as the player built it: ");
    }
    log_raw(what);
    log_raw(" (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

static double ef_ms(LARGE_INTEGER a, LARGE_INTEGER b)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart : 0;
}

/* ---- the part tables ---- */
static void ef_sort_rows(ef_own *a, unsigned n)
{
    unsigned gap, i, j;
    for (gap = n / 2; gap; gap /= 2) {
        for (i = gap; i < n; i++) {
            ef_own t = a[i];
            for (j = i; j >= gap && a[j - gap].id > t.id; j -= gap) a[j] = a[j - gap];
            a[j] = t;
        }
    }
}

/* by owner, then by table order, so the first stock row of an owner is the one the server takes (syncdb.go keeps the first) */
static void ef_sort_stock(ef_stk *a, unsigned n)
{
    unsigned gap, i, j;
    for (gap = n / 2; gap; gap /= 2) {
        for (i = gap; i < n; i++) {
            ef_stk t = a[i];
            for (j = i; j >= gap && (a[j - gap].owner > t.owner || (a[j - gap].owner == t.owner && a[j - gap].order > t.order)); j -= gap) a[j] = a[j - gap];
            a[j] = t;
        }
    }
}

/* The record set's int getter (row set, row index, column name, default), looked up once per record set: ppi_method checks the method with
 * VirtualQuery, which per value made a big load slow (2026-11-20). */
typedef int (*ef_get_fn)(void *row, int index, const ppi_key *column, int def);

static ef_get_fn ef_getter(void *row)
{
    return (ef_get_fn)ppi_method(row, EF_ROW_INT_AT);
}

static void ef_cat(char *d, unsigned cap, const char *s)
{
    unsigned n = 0;
    while (d[n]) n++;
    while (*s && n + 1 < cap) d[n++] = *s++;
    d[n] = 0;
}

static int ef_load_table(void *db, int k)
{
    static const char *const owner_col[] = {NULL, "Ordinal", "CarBodyID / 1000", "EngineID", "DrivetrainID"};
    static const char *const installs_col[3] = {"EngineID", "DrivetrainID", "CarBodyID / 1000"};
    char sql[256];
    void *row;
    int n, i;
    ef_table *t = &g_ef_tab[k];
    ef_get_fn get;
    ppi_key ka = ppi_key_for("a"), kb = ppi_key_for("b"), kc = ppi_key_for("c"), kd = ppi_key_for("d");

    /* Only the game's own rows: Extra Options' swap switches add about 535,000 gearbox and 125,000 engine rows (its catalogs: 357 drivetrains x
     * 1,500 gearboxes, 353 cars x 355 engines in the server's database), all from 0x20000000 up, which the rule judges by their id alone. Loading
     * them all through the game's record set took the game thread long enough to look hung (win1, dev, 2026-11-20 01:06 UTC). */
    sql[0] = 0;
    ef_cat(sql, sizeof sql, "SELECT Id AS a, ");
    ef_cat(sql, sizeof sql, owner_col[g_ef_owner[k]]);
    ef_cat(sql, sizeof sql, " AS b, IsStock AS c");
    if (k <= 2) {
        ef_cat(sql, sizeof sql, ", ");
        ef_cat(sql, sizeof sql, installs_col[k]);
        ef_cat(sql, sizeof sql, " AS d");
    }
    ef_cat(sql, sizeof sql, " FROM List_Upgrade");
    ef_cat(sql, sizeof sql, g_ppi_table[k]);
    ef_cat(sql, sizeof sql, " WHERE Id < 536870912");
    row = ppi_query(db, sql);
    if (!row) {
        return 0;
    }
    n = ppi_rows(row);
    get = ef_getter(row);
    if (n < 0 || (n > 0 && !get)) {
        ppi_release(row);
        return 0;
    }
    t->rows = n ? (ef_own *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)n * sizeof *t->rows) : NULL;
    t->stock = n ? (ef_stk *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)n * sizeof *t->stock) : NULL;
    if (n && (!t->rows || !t->stock)) {
        ppi_release(row);
        return 0;
    }
    for (i = 0; i < n; i++) {
        ef_own *o = &t->rows[t->n];
        o->id = (unsigned)get(row, i, &ka, -1);
        o->owner = (unsigned)get(row, i, &kb, -1);
        o->stock = get(row, i, &kc, 0) > 0;
        o->installs = k <= 2 ? (unsigned)get(row, i, &kd, -1) : EF_NONE;
        if (o->stock) {
            t->stock[t->ns].owner = o->owner;
            t->stock[t->ns].id = o->id;
            t->stock[t->ns].order = (unsigned)i;
            t->ns++;
        }
        t->n++;
    }
    ppi_release(row);
    ef_sort_rows(t->rows, t->n);
    ef_sort_stock(t->stock, t->ns);
    return 1;
}

static void *ef_database(void)
{
    void *holder = NULL, *db = NULL;
    if (!g_ppi_img || !read_game_memory(g_ppi_img + PPI_HOLDER_RVA, &holder, sizeof holder) || !holder ||
        !read_game_memory((const unsigned char *)holder + PPI_HOLDER_DB_OFFSET, &db, sizeof db)) {
        return NULL;
    }
    return db;
}

/* the part tables and the garage's part columns, once per game -> 1 when ready (only with the work flag taken, ef_work_begin) */
static int ef_ready(void)
{
    LARGE_INTEGER t0, t1;
    void *db;
    unsigned rows = 0;
    int k;

    if (g_ef_state) {
        return g_ef_state > 0;
    }
    db = ef_database();
    if (!db) {
        return 0;               /* not yet: asked again at the next list */
    }
    log_begin();
    log_raw("eventfix: loading the part tables of the game database (first event car list or car of this game)\n");
    QueryPerformanceCounter(&t0);
    for (k = 0; k < EF_SLOTS; k++) {
        const char *name = NULL;
        unsigned i;
        if (!read_game_memory(g_ppi_img + EF_SLOT_TABLE_RVA + 32u * (unsigned)k + 8u, &name, sizeof name) || !name ||
            !read_game_memory(name, g_ef_short[k], sizeof g_ef_short[k] - 1)) {
            break;
        }
        g_ef_short[k][sizeof g_ef_short[k] - 1] = 0;
        for (i = 0; g_ef_short[k][i]; i++) {
            if (!ef_word_char(g_ef_short[k][i])) break;
        }
        if (!i || g_ef_short[k][i]) break;
    }
    g_ef_short_ok = k == EF_SLOTS && lstrcmpA(g_ef_short[0], "Engine") == 0;
    for (k = 0; k < EF_SLOTS; k++) {
        if (g_ef_owner[k] == EF_NO_OWNER || !g_ppi_table[k]) {
            continue;
        }
        if (!ef_load_table(db, k)) {
            g_ef_state = -1;
            log_begin();
            log_raw("eventfix: the part table List_Upgrade");
            log_raw(g_ppi_table[k]);
            log_raw(" cannot be read from the game database, so cars with parts of other cars race as built in this game\n");
            return 0;
        }
        rows += g_ef_tab[k].n;
    }
    QueryPerformanceCounter(&t1);
    g_ef_state = 1;
    log_begin();
    log_raw("eventfix: ");
    log_dec(rows);
    log_raw(" part rows of the game database loaded in ");
    log_dec((unsigned long)ef_ms(t0, t1));
    log_raw(" ms; the garage's part columns are ");
    log_raw(g_ef_short_ok ? "known" : "NOT known (the event car lists stay as the game builds them)");
    log_raw("\n");
    return 1;
}

static const ef_own *ef_find(int k, unsigned id)
{
    const ef_table *t = &g_ef_tab[k];
    unsigned lo = 0, hi = t->n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (t->rows[mid].id < id) lo = mid + 1;
        else hi = mid;
    }
    return lo < t->n && t->rows[lo].id == id ? &t->rows[lo] : NULL;
}

static int ef_op_owner(void *ctx, int k, unsigned id, unsigned *owner)
{
    const ef_own *o = ef_find(k, id);
    (void)ctx;
    if (!o) return 0;
    *owner = o->owner;
    return 1;
}

static int ef_op_stock(void *ctx, int k, unsigned owner, unsigned *id)
{
    const ef_table *t = &g_ef_tab[k];
    unsigned lo = 0, hi = t->ns;
    (void)ctx;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (t->stock[mid].owner < owner) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= t->ns || t->stock[lo].owner != owner) return 0;
    *id = t->stock[lo].id;
    return 1;
}

static int ef_op_installs(void *ctx, int k, unsigned id, unsigned *what)
{
    const ef_own *o = k <= 2 ? ef_find(k, id) : NULL;
    (void)ctx;
    if (!o || o->installs == EF_NONE) return 0;
    *what = o->installs;
    return 1;
}

/* ---- the fixed car's class and PI, from the game's simulation ---- */
static int ef_writable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!p || !VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT ||
        !(m.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) || (m.Protect & PAGE_GUARD)) {
        return 0;
    }
    return (const unsigned char *)p + n <= (const unsigned char *)m.BaseAddress + m.RegionSize;
}

static int ef_rescore(unsigned car, const unsigned block[2 + EF_SLOTS], unsigned *klass, float *pi)
{
    unsigned char *ratings = NULL;
    unsigned k = 0;
    float p = 0;

    if (!g_ef_snapshot || !g_ef_snap_out || !read_game_memory(g_ppi_img + EF_RATINGS_RVA, &ratings, sizeof ratings) || !ratings ||
        !ef_writable(ratings, EF_RATINGS_SAVE) || !read_game_memory(ratings, g_ef_save, sizeof g_ef_save)) {
        return 0;
    }
    g_ef_snapshot(g_ef_snap_out, car, block);
    memcpy(&p, ratings + EF_RATINGS_PI, sizeof p);
    memcpy(&k, ratings + EF_RATINGS_CLASS, sizeof k);
    memcpy(ratings, g_ef_save, sizeof g_ef_save);
    if (k > 15 || !ppi_pi_usable((double)p)) {
        return 0;
    }
    *klass = k;
    *pi = p;
    return 1;
}

/* The verdict on car `car` with WheelStyle `wheel` and parts `parts` into *v (only with the work flag taken, after ef_ready) */
static void ef_judge(unsigned car, unsigned wheel, const unsigned parts[EF_SLOTS], double *ms, ef_verdict *v)
{
    ef_ops ops = {ef_op_owner, ef_op_stock, ef_op_installs, NULL};
    unsigned hash = ef_hash(wheel, parts), block[2 + EF_SLOTS];
    LARGE_INTEGER t0, t1;

    if (ef_cache_get(car, hash, v)) {
        return;
    }
    memset(v, 0, sizeof *v);
    v->car = car;
    v->hash = hash;
    v->kind = (unsigned)ef_fix(car, parts, &ops, v->fixed, &v->changed);
    if (v->kind == EF_FIXED) {
        block[0] = EF_NONE;
        block[1] = wheel;
        memcpy(block + 2, v->fixed, sizeof v->fixed);
        QueryPerformanceCounter(&t0);
        if (!ef_rescore(car, block, &v->klass, &v->pi)) {
            v->kind = EF_CANNOT;
            ef_log_decline("the game's performance simulation gave no usable class and PI for the fixed parts", car);
        }
        QueryPerformanceCounter(&t1);
        if (ms) *ms += ef_ms(t0, t1);
    } else if (v->kind == EF_CANNOT) {
        ef_log_decline("its engine, drivetrain, body or tires belong to another car and the car's own have no stock row", car);
    }
    ef_cache_put(v);
}

/* ---- the event car list ---- */
static volatile LONG64 g_ef_list_at;            /* tick of the last event car list (a garage list with class or PI terms, in a session) */
static volatile LONG g_ef_first_list;

/* an online event's car select ran within 2 minutes: its countdown hook, or an event car list. The list can come before the countdown's first frame
 * and the car's details must agree with it; the picked car is committed when it loads (States::LoadPlayerCar +8ae8e0 calls the choices' slot 59
 * [C: decompile]), after the select screen has closed: win1 2026-11-20 01:11 UTC committed a car with Extra Options swaps unfixed while the
 * window was 3 s after the countdown's last frame. */
#define EF_EVENT_WINDOW_MS 120000u
static int ef_in_select(void)
{
    ULONGLONG now = GetTickCount64(), armed = (ULONGLONG)g_cs_count_armed_at, listed = (ULONGLONG)g_ef_list_at;
    return (armed && now - armed <= EF_EVENT_WINDOW_MS) || (listed && now - listed <= EF_EVENT_WINDOW_MS);
}

/* the garage list query `sql` (an event car list of garage table `table`, ef_garage_query) with the fixed values in place, in a HeapAlloc'd buffer
 * the caller frees, or NULL to run it as it is (only with the work flag taken) */
static char *ef_list_query(const char *sql, const char *table)
{
    char scan[2048], base[128], *text = NULL;
    void *db, *row;
    ef_row *rows;
    int n, i, k, replaced;
    unsigned fixed = 0, cannot = 0, foreign_cars = 0;
    double sim_ms = 0;
    LARGE_INTEGER t0, t1;
    ef_get_fn get;
    ppi_key kgi, kgc, kgw, kp[EF_SLOTS];

    if (!ef_ready() || !g_ef_short_ok || !(db = ef_database())) {
        return NULL;
    }
    QueryPerformanceCounter(&t0);
    scan[0] = 0;
    ef_cat(scan, sizeof scan, "SELECT Id AS gi, CarId AS gc, WheelStyle AS gw");
    for (k = 0; k < EF_SLOTS; k++) {
        ef_cat(scan, sizeof scan, ", ");
        ef_cat(scan, sizeof scan, g_ef_short[k]);
        ef_cat(scan, sizeof scan, " AS gp");
        ef_cat_dec(scan, sizeof scan, (unsigned)k);
    }
    if (!ef_garage_table(table, base, sizeof base)) {
        return NULL;
    }
    ef_cat(scan, sizeof scan, " FROM ");
    ef_cat(scan, sizeof scan, base);
    row = ppi_query(db, scan);
    if (!row) {
        LONG nd = InterlockedIncrement(&g_ef_declined);
        if (nd <= 10 || nd % 100 == 0) {
            log_begin();
            log_raw("eventfix: the garage table ");
            log_raw(base);
            log_raw(" (the list reads ");
            log_raw(table);
            log_raw(") cannot be read by its part columns, so this event car list is the game's (#");
            log_dec((unsigned long)nd);
            log_raw(")\n");
        }
        return NULL;
    }
    n = ppi_rows(row);
    get = ef_getter(row);
    rows = n > 0 ? (ef_row *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)n * sizeof *rows) : NULL;
    if ((n > 0 && !rows) || (n > 0 && !get)) {
        if (rows) HeapFree(GetProcessHeap(), 0, rows);
        ppi_release(row);
        return NULL;
    }
    kgi = ppi_key_for("gi");
    kgc = ppi_key_for("gc");
    kgw = ppi_key_for("gw");
    for (k = 0; k < EF_SLOTS; k++) {
        char col[8] = "gp";
        ef_cat_dec(col, sizeof col, (unsigned)k);
        kp[k] = ppi_key_for(col);   /* short names are kept inside the key itself (under 16 bytes) */
    }
    for (i = 0; i < n; i++) {
        unsigned parts[EF_SLOTS], car, wheel, gid;
        ef_verdict verdict, *vd = &verdict;
        gid = (unsigned)get(row, i, &kgi, -1);
        car = (unsigned)get(row, i, &kgc, -1);
        wheel = (unsigned)get(row, i, &kgw, -1);
        for (k = 0; k < EF_SLOTS; k++) {
            parts[k] = (unsigned)get(row, i, &kp[k], -1);
        }
        ef_judge(car, wheel, parts, &sim_ms, vd);
        if (vd->kind == EF_FIXED) {
            rows[fixed].garage_id = gid;
            rows[fixed].klass = vd->klass;
            rows[fixed].pi = vd->pi;
            fixed++;
            foreign_cars++;
        } else if (vd->kind == EF_CANNOT) {
            cannot++;
            foreign_cars++;
        }
    }
    ppi_release(row);
    replaced = 0;
    if (fixed) {
        text = (char *)HeapAlloc(GetProcessHeap(), 0, EF_SQL_MAX);
        replaced = text ? ef_rewrite_sql(sql, rows, fixed, text, EF_SQL_MAX) : -1;
        if (replaced <= 0 && text) {
            HeapFree(GetProcessHeap(), 0, text);
            text = NULL;
        }
    }
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    QueryPerformanceCounter(&t1);
    if (!foreign_cars && InterlockedExchange(&g_ef_first_list, 1) == 0) {
        log_begin();
        log_raw("eventfix: first event car list of this game: ");
        log_dec((unsigned long)(n > 0 ? n : 0));
        log_raw(" garage cars, none with parts of other cars; took ");
        log_dec((unsigned long)ef_ms(t0, t1));
        log_raw(" ms\n");
    }
    if (foreign_cars) {
        LONG nl = InterlockedIncrement(&g_ef_lists);
        g_ef_first_list = 1;
        if (nl <= 20 || nl % 50 == 0) {
            log_begin();
            log_raw("eventfix: event car list: ");
            log_dec((unsigned long)(n > 0 ? n : 0));
            log_raw(" garage cars, ");
            log_dec(foreign_cars);
            log_raw(" with parts of other cars (");
            log_dec(fixed);
            log_raw(" judged by their fixed class and PI, ");
            log_dec(cannot);
            log_raw(" left as built); ");
            if (replaced < 0) {
                log_raw("the rewritten query does not fit, so the list is the game's; ");
            }
            log_raw("took ");
            log_dec((unsigned long)ef_ms(t0, t1));
            log_raw(" ms, of it simulation ");
            log_dec((unsigned long)sim_ms);
            log_raw(" ms (#");
            log_dec((unsigned long)nl);
            log_raw(")\n");
        }
    }
    return text;
}

static void ef_query_hook(void *collection, const char *sql, void *database, void *extra)
{
    char *use = NULL, table[128];
    if (g_ef_on && sql && ef_garage_query(sql, table, sizeof table) && ppi_in_session(g_ppi_img)) {
        g_ef_list_at = (LONG64)GetTickCount64();
        if (ef_work_begin()) {
            use = ef_list_query(sql, table);
            ef_work_end();
        }
    }
    g_ef_query_orig(collection, use ? use : sql, database, extra);
    if (use) {
        HeapFree(GetProcessHeap(), 0, use);
    }
}

/* ---- the car's details (QueryCarAttributes, picker_pi.inc) ---- */
/* While an online event car select runs: the attribute block of garage car `car` (CCarInGarage) gets the fixed class and PI that the event car list
 * judged it by. Only the cache is read here, never the game: QueryCarAttributes runs on any thread, in the middle of anything. -> 1 when changed. */
static int ef_attrs(void *car, unsigned char *out)
{
    unsigned id = 0, wheel = 0, parts[EF_SLOTS];
    ef_verdict v;
    double dpi;

    if (!g_ef_on || !ef_in_select() || !ppi_in_session(g_ppi_img) ||
        !read_game_memory((const unsigned char *)car + PPI_CAR_ID, &id, sizeof id) ||
        !read_game_memory((const unsigned char *)car + PPI_CAR_PARTS - 4, &wheel, sizeof wheel) ||
        !read_game_memory((const unsigned char *)car + PPI_CAR_PARTS, parts, sizeof parts) ||
        !ef_cache_get(id, ef_hash(wheel, parts), &v) || v.kind != EF_FIXED) {
        return 0;
    }
    dpi = (double)v.pi;
    memcpy(out + PPI_OUT_PI, &dpi, sizeof dpi);
    out[PPI_OUT_CLASS] = (unsigned char)v.klass;
    return 1;
}

/* ---- the race (event_stock.inc's commit hook) ---- */
/* The pending block `self` of the online choices (ES_* offsets): a car with parts of other cars gets the fixed parts, the stock tune values that belong
 * to them and the fixed class and PI before the game commits it. -> 1 when changed. */
static int ef_commit(unsigned char *self, unsigned car_at, unsigned wheel_at, unsigned parts_at, unsigned tune_at, unsigned attrs_at)
{
    unsigned car, wheel, parts[EF_SLOTS], before_class, i, nslots = 0, ntune = 0;
    unsigned long long tune;
    short attr_id;
    double before_pi = 0, dpi;
    ef_verdict copy;
    LONG n;

    if (!g_ef_on) {
        return 0;
    }
    memcpy(&car, self + car_at, sizeof car);
    memcpy(&wheel, self + wheel_at, sizeof wheel);
    memcpy(parts, self + parts_at, sizeof parts);
    memcpy(&attr_id, self + attrs_at + PPI_OUT_ID, sizeof attr_id);
    if (!ef_cache_get(car, ef_hash(wheel, parts), &copy)) {
        /* not judged by a list (the picked car was not listed through the hook): judge it here, on the game thread that commits it */
        if (!ef_work_begin()) {
            return 0;
        }
        if (!ef_ready()) {
            ef_work_end();
            return 0;
        }
        ef_judge(car, wheel, parts, NULL, &copy);
        ef_work_end();
    }
    if (copy.kind != EF_FIXED) {
        LONG nc = InterlockedIncrement(&g_ef_commits);
        if (nc <= 10 || nc % 50 == 0) {
            log_begin();
            log_raw("eventfix: car ");
            log_dec(car);
            log_raw(copy.kind == EF_CLEAN ? " committed for an online event with its own parts (#" : " committed for an online event as built: it cannot be fixed (#");
            log_dec((unsigned long)nc);
            log_raw(")\n");
        }
        return 0;
    }
    if (attr_id != (short)car) {
        ef_log_decline("the attribute block does not carry this car, so its layout is not as expected", car);
        return 0;
    }
    before_class = self[attrs_at + PPI_OUT_CLASS];
    memcpy(&before_pi, self + attrs_at + PPI_OUT_PI, sizeof before_pi);
    for (i = 0; i < EF_SLOTS; i++) {
        if (copy.changed & (1ull << i)) {
            memcpy(self + parts_at + 4 * i, &copy.fixed[i], 4);
            nslots++;
        }
    }
    tune = ef_tune_mask(copy.changed);
    for (i = 0; i < EF_TUNE; i++) {
        if (tune & (1ull << i)) {
            unsigned t = 0xbf800000u;      /* -1.0f: the car's default */
            memcpy(self + tune_at + 4 * i, &t, sizeof t);
            ntune++;
        }
    }
    self[attrs_at + PPI_OUT_CLASS] = (unsigned char)copy.klass;
    dpi = (double)copy.pi;
    memcpy(self + attrs_at + PPI_OUT_PI, &dpi, sizeof dpi);
    g_ef_fixed_at = (LONG64)GetTickCount64();
    n = InterlockedIncrement(&g_ef_commits);
    if (n <= 20 || n % 50 == 0) {
        log_begin();
        log_raw("eventfix: car ");
        log_dec(car);
        log_raw(" races in this online event with ");
        log_dec(nslots);
        log_raw(" part(s) of other cars back to its own stock ones (slots");
        for (i = 0; i < EF_SLOTS; i++) {
            if (copy.changed & (1ull << i)) {
                log_raw(" ");
                log_dec(i);
                log_raw(":");
                log_dec(parts[i]);
                log_raw("->");
                if (copy.fixed[i] == EF_NONE) log_raw("none");
                else log_dec(copy.fixed[i]);
            }
        }
        log_raw("), ");
        log_dec(ntune);
        log_raw(" tune value(s) back to stock; class ");
        log_dec(before_class);
        log_raw(" PI ");
        ppi_log_pi(before_pi);
        log_raw(" -> class ");
        log_dec(copy.klass);
        log_raw(" PI ");
        ppi_log_pi((double)copy.pi);
        log_raw("; the garage car is unchanged (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
    return 1;
}

/* ---- the car as raced: the game's own PI check when the race car loads ----
 * Car::DetectPerformanceTampering (+f58a90, called from Car::LoadCarModelAndPresentation for the local player's car) copies the car's setup
 * (car+0x1200), runs CarPerformance_SimulatePIScore (+1046e00; [[+365d6f0]], the copy, the car's CarClassModifier, 0, 0, 0) and compares the
 * result with the PI the car's info object reports ([car+0x390] slot 0x100 -> slot 0x120); more than 0.001 apart, or above the reported class's
 * top, and it sets car+0x11f5 and writes PI 0.9998 (slot 0x118, a float) and class 7 (slot 0x78, an int) [C: decompile and the bytes at
 * +f58dc3..+f58e0c]. A car this file fixed reported the PI of its fixed parts while the race car's info still had its garage car's (win1 dev,
 * 2026-11-20 01:46 UTC: car 1500 fixed to A 0.7457, raced as class 7 PI 1000 and the server put it out of the event) [inferred: which source the
 * info object takes]. For such a car (a fix committed within the event window), when the check has flagged it, the info gets the PI the
 * same simulation gives for the car as it will race, and the class of that PI (the class threshold table [[+365d6e0]]), and
 * the flag is cleared: the reported numbers are the car's true ones, not the check's marker. Every other car the check flags stays flagged. */
#define EF_TAMPER_RVA 0xf58a90u
#define EF_SIMPI_RVA 0x1046e00u
#define EF_PI_SETTINGS_RVA 0x365d6f0u
#define EF_CLASS_TABLE_RVA 0x365d6e0u
#define EF_CAR_SETUP 0x1200u
#define EF_CAR_TAMPERED 0x11f5u
#define EF_CAR_INFO 0x390u
#define EF_SETUP_BYTES 0x3a00u
typedef unsigned long long (*ef_tamper_fn)(void *car, void *row);
typedef double (*ef_simpi_fn)(void *settings, void *setup, double modifier, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c);
static ef_tamper_fn g_ef_tamper_orig;
static ef_simpi_fn g_ef_simpi;
static unsigned char *g_ef_setup_copy;

static unsigned long long ef_tamper_hook(void *car, void *row)
{
    unsigned long long r = g_ef_tamper_orig(car, row);
    const unsigned char *c = (const unsigned char *)car;
    ULONGLONG now = GetTickCount64(), fixed_at = (ULONGLONG)g_ef_fixed_at;
    unsigned char flagged = 0;
    void *settings = NULL, *table = NULL, *holder = NULL, *info = NULL, *get_mod, *set_pi, *set_class, *get_info;
    ppi_key key = ppi_key_for("CarClassModifier");
    float modifier;
    double pi;
    int klass;

    if (!(r & 0xff) || !g_ef_on || !g_ef_simpi || !g_ef_setup_copy || !fixed_at || now - fixed_at > EF_EVENT_WINDOW_MS ||
        !ppi_in_session(g_ppi_img) || !read_game_memory(c + EF_CAR_TAMPERED, &flagged, 1) || flagged != 1) {
        return r;
    }
    if (!read_game_memory(g_ppi_img + EF_PI_SETTINGS_RVA, &settings, sizeof settings) || !settings ||
        !read_game_memory(g_ppi_img + EF_CLASS_TABLE_RVA, &table, sizeof table) || !table ||
        !read_game_memory(c + EF_CAR_INFO, &holder, sizeof holder) || !holder || !(get_info = ppi_method(holder, 0x100)) ||
        !(get_mod = ppi_method(row, PPI_ROW_FLOAT_SLOT)) || !read_game_memory(c + EF_CAR_SETUP, g_ef_setup_copy, EF_SETUP_BYTES)) {
        ef_log_decline("the game's PI check flagged the fixed car and its setup or info object cannot be read: it stays flagged", 0);
        return r;
    }
    info = ((void *(*)(void *))get_info)(holder);
    if (!info || !(set_pi = ppi_method(info, 0x118)) || !(set_class = ppi_method(info, 0x78))) {
        ef_log_decline("the game's PI check flagged the fixed car and its info object has no PI setter: it stays flagged", 0);
        return r;
    }
    modifier = ((float (*)(void *, const ppi_key *))get_mod)(row, &key);
    pi = g_ef_simpi(settings, g_ef_setup_copy, (double)modifier, 0, 0, 0);
    if (!ppi_pi_usable(pi)) {
        ef_log_decline("the game's PI check flagged the fixed car and the lap simulation gave no usable PI: it stays flagged", 0);
        return r;
    }
    /* The class of a PI: the first of the 8 classes whose top PI (a double at table + 16 * class, the entry FUN_14104c9d0 returns) is not below
     * it, the last one above all [C: FUN_14104c8d0 finds the same index, then returns the 100..999 display number: a class of 800 was set
     * with it on win1, dev 2026-11-20 02:55 UTC]. */
    {
        double clamped = pi < 0 ? 0 : pi > 1 ? 1 : pi, top;
        for (klass = 0; klass < 8; klass++) {
            if (!read_game_memory((const unsigned char *)table + 16u * (unsigned)klass, &top, sizeof top)) {
                ef_log_decline("the game's PI check flagged the fixed car and the class table cannot be read: it stays flagged", 0);
                return r;
            }
            if (clamped <= top) break;
        }
        if (klass == 8) klass = 7;
    }
    ((void (*)(void *, float))set_pi)(info, (float)pi);
    ((void (*)(void *, int))set_class)(info, klass);
    *((unsigned char *)car + EF_CAR_TAMPERED) = 0;
    log_begin();
    log_raw("eventfix: the fixed car loaded for the race: the game's PI check did not match its reported PI, so it reports what the game's own lap "
            "simulation of the race setup gives, class ");
    log_dec((unsigned long)klass);
    log_raw(" PI ");
    ppi_log_pi(pi);
    log_raw(" (not the check's class 7 PI 0.9998)\n");
    return r & ~0xffull;
}

static int patch_event_fix(void)
{
    static const unsigned char snap[16] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0xd0, 0xc1, 0xff, 0xff};
    static const unsigned char query[16] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48};
    unsigned char *img = find_fh2_image();

    if (!img || !g_ppi_img || !code_is("eventfix", "CarPerformance_BuildSnapshotForGarageCar", img, EF_SNAPSHOT_RVA, snap, sizeof snap)) {
        return 0;
    }
    g_ef_snap_out = (unsigned char *)VirtualAlloc(NULL, EF_SNAPSHOT_OUT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_ef_snap_out) {
        return 0;
    }
    g_ef_snapshot = (ef_snapshot_fn)(img + EF_SNAPSHOT_RVA);
    g_ef_on = 1;
    if (code_is("eventfix", "the car collection's list query", img, EF_QUERY_RVA, query, sizeof query) &&
        plat_hook("eventfix: event car lists by the fixed class and PI", img + EF_QUERY_RVA, (void *)ef_query_hook, (void **)&g_ef_query_orig)) {
        log_begin();
        log_raw("hook: eventfix: online, an event car list takes a car with parts of other cars by its class and PI with its own stock parts (game+0x140c3f0)\n");
    }
    log_begin();
    log_raw("hook: eventfix: online, a car with parts of other cars races an event with those parts and their tune values back to stock (car details, commit)\n");
    {
        static const unsigned char tamper[16] = {0x40, 0x57, 0xb8, 0x20, 0x3a, 0x00, 0x00, 0xe8, 0xc4, 0xf3, 0xfb, 0xff, 0x48, 0x2b, 0xe0, 0x48};
        static const unsigned char simpi[16] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0xc5, 0xfb, 0x11, 0x54, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54};
        g_ef_setup_copy = (unsigned char *)VirtualAlloc(NULL, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (g_ef_setup_copy && code_is("eventfix", "CarPerformance_SimulatePIScore", img, EF_SIMPI_RVA, simpi, sizeof simpi) &&
            code_is("eventfix", "Car::DetectPerformanceTampering", img, EF_TAMPER_RVA, tamper, sizeof tamper)) {
            g_ef_simpi = (ef_simpi_fn)(img + EF_SIMPI_RVA);
            if (plat_hook("eventfix: the fixed car's PI as raced", img + EF_TAMPER_RVA, (void *)ef_tamper_hook, (void **)&g_ef_tamper_orig)) {
                log_begin();
                log_raw("hook: eventfix: a fixed car the game's PI check flags reports the PI of the game's lap simulation of its race setup (game+0xf58a90)\n");
            }
        }
    }
    return 1;
}
