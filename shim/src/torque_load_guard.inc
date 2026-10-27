/* FH2 1.0.0.64. Reject invalid ICE curve data before the asynchronous car
 * preloader (fcfe70), using its EXISTING unavailable-car branch. That branch
 * replaces the whole car with NUL_Car_00, fills its real stock parts and loads
 * its assets. Never skip a void physics constructor or invent curve samples.
 * Native CParts is two header words followed by 43 part IDs (180 bytes).
 */
typedef void (*torque_preload_fn)(void *, void **);
typedef void *(*torque_parts_ctor_fn)(void *);
typedef void (*torque_stock_fn)(void *, void **, unsigned);
typedef void **(*torque_part_fn)(void **, void **, int, unsigned, unsigned);
static torque_preload_fn g_torque_preload_orig;
static torque_parts_ctor_fn g_torque_parts_ctor;
static torque_stock_fn g_torque_stock;
static torque_part_fn g_torque_car_part, g_torque_engine_part;
static unsigned char *g_torque_load_img;

/* Borrowed, const MSVC 2012 std::string. Native query/getter methods do not
 * retain or destroy it; no native allocation or foreign heap free is needed.
 */
typedef struct { union { char small[16]; const char *ptr; } text; size_t size, capacity; } torque_key;
static torque_key torque_key_for(const char *text)
{
    torque_key key = {0};
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
static void *torque_method(void *obj, unsigned offset) { return (*(void ***)obj)[offset / 8]; }
static void torque_addref(void *obj) { ((void (*)(void *))torque_method(obj, 0x10))(obj); }
static void torque_release(void *obj)
{
    if (obj) ((void (*)(void *))torque_method(obj, 0x18))(obj);
}
static int torque_rows(void *row)
{
    return row ? ((int (*)(void *))torque_method(row, 0x148))(row) : 0;
}
static unsigned torque_uint(void *row, const char *field)
{
    torque_key key = torque_key_for(field);
    return ((unsigned (*)(void *, const torque_key *))torque_method(row, 0x110))(row, &key);
}
static void *torque_query(void *db, const char *sql)
{
    void *row = NULL;
    torque_key key = torque_key_for(sql);
    ((void **(*)(void *, void **, const torque_key *))torque_method(db, 0x48))(db, &row, &key);
    return row;
}
static void *torque_part(void *db, torque_part_fn fn, int slot, unsigned owner, unsigned id)
{
    void *row = NULL, *ref = db;
    torque_addref(db); /* These native helpers consume the passed reference. */
    fn(&row, &ref, slot, owner, id);
    return row;
}

/* Validate the SAME resolved IDs and ownership checks as 1056210. Electric
 * motors have their own constructor/curve and must not require an ICE camshaft.
 * This guard covers missing/empty/one-sample/oversized ICE curves, not every
 * possible physics-data defect.
 */
static int torque_parts_valid(void *db, unsigned car, const unsigned parts[45])
{
    void *engine = torque_part(db, g_torque_car_part, 0, car, parts[2]);
    void *camshaft = NULL, *curve = NULL;
    int ok = 0;
    if (torque_rows(engine) > 0) {
        unsigned engine_id = torque_uint(engine, "EngineID");
        camshaft = torque_part(db, g_torque_engine_part, 12, engine_id, parts[14]);
        if (torque_rows(camshaft) > 0) {
            char sql[96] = "SELECT * FROM List_TorqueCurve WHERE TorqueCurveID=";
            char digits[10];
            unsigned n = 0, pos = (unsigned)lstrlenA(sql);
            unsigned curve_id = torque_uint(camshaft, "TorqueCurveFullThrottleID");
            do { digits[n++] = (char)('0' + curve_id % 10); curve_id /= 10; } while (curve_id);
            while (n) sql[pos++] = digits[--n];
            sql[pos] = 0;
            curve = torque_query(db, sql);
            if (torque_rows(curve) > 0) {
                unsigned count = torque_uint(curve, "NumTorqueValues");
                ok = count >= 2 && count <= 246;
            }
        }
    } else {
        void *motor = torque_part(db, g_torque_car_part, 3, car, parts[5]);
        ok = torque_rows(motor) > 0;
        torque_release(motor);
    }
    torque_release(curve);
    torque_release(camshaft);
    torque_release(engine);
    return ok;
}
static void torque_stock_parts(void *db, unsigned car, unsigned parts[45])
{
    void *ref = db;
    g_torque_parts_ctor(parts);
    torque_addref(db);
    g_torque_stock(parts, &ref, car);
}

static void torque_preload_guard(void *loader, void **job_ref)
{
    void *job = NULL, *vehicle = NULL, *singleton = NULL, *db = NULL;
    unsigned char *info;
    unsigned car, parts[45];
    unsigned char stock, placeholder;
    int reject_physics;
    if (!job_ref || !read_game_memory(job_ref, &job, sizeof job) || !job ||
        !read_game_memory((unsigned char *)job + 0x28, &vehicle, sizeof vehicle) || !vehicle) return;
    info = (unsigned char *)vehicle + 0x110; /* Native 1762870 is precisely this accessor. */
    if (!read_game_memory(info + 8, &car, sizeof car) ||
        !read_game_memory(info + 0x10, parts, sizeof parts) ||
        !read_game_memory(info + 0x175, &stock, 1) ||
        !read_game_memory(info + 0x2ed, &placeholder, 1) ||
        !read_game_memory(g_torque_load_img + 0x354f4f8, &singleton, sizeof singleton) || !singleton ||
        !read_game_memory((unsigned char *)singleton + 0x40, &db, sizeof db) || !db) {
        /* No physics has been allocated; the worker checks this native cancel
         * flag and completes the request in its existing state-10 branch. */
        *(unsigned char *)((unsigned char *)job + 0x1c0) = 1;
        return;
    }
    if (stock) torque_stock_parts(db, car, parts);
    reject_physics = !torque_parts_valid(db, car, parts);
    if (reject_physics || placeholder) {
        unsigned fallback[45];
        torque_stock_parts(db, 1215, fallback); /* Native NUL_Car_00 ordinal. */
        if (!torque_parts_valid(db, 1215, fallback)) {
            *(unsigned char *)((unsigned char *)job + 0x1c0) = 1;
            log_begin();
            log_raw("torqueload: unavailable placeholder curve; cancelling car load before physics\n");
            return;
        }
    }
    if (reject_physics) {
        /* fcfe70 tests this flag before selecting assets. Its own f68c60
         * and 104d5f0 do the complete substitution and ownership updates. */
        info[0x2ed] = 1;
        log_begin();
        log_raw("torqueload: invalid engine/camshaft/curve; using native unavailable-car fallback\n");
    }
    g_torque_preload_orig(loader, job_ref);
    if (reject_physics) {
        /* IMPORTANT: native f68c60 preserves the old car in +2f0/+318.
         * With option +247b, f67270 otherwise restores that old car's
         * physics even while displaying the placeholder. The native -1
         * sentinel disables that restoration for REJECTED physics only.
         * Saved strings/parts remain owned by the native vehicle destructor.
         */
        unsigned none = 0xffffffffu;
        memcpy(info + 0x2f0, &none, sizeof none);
    }
}
