/* The pure decisions of event_fix.inc (tested natively by tests/test_event_fix.sh): which installed parts belong to another car, engine or drivetrain
 * than the car's own, the car with only those parts back to its own stock ones, which tune values go back to stock with them, and the garage list
 * query with the fixed class and PI in place of the garage row's. */
#define EF_SLOTS 43
#define EF_NONE 0xffffffffu
#define EF_TUNE 44

/* Who the rows of a slot's table belong to. Game database of 1.0.0.64, List_Upgrade<table> owner columns (queried 2026-11-19, the same columns and
 * precedence as native/internal/catalog/syncdb.go): Ordinal = a car, CarBodyID / CarbodyId = the car whose body installs it (x 1000), EngineID = an
 * engine, DrivetrainID / DrivetrainId = a drivetrain. Slots 3 Motor and 26 QuadTurbo have no table; 38-40 (tire widths, weight) have no owner
 * column; 42 MotorParts belongs to a motor (electric cars), left alone. */
enum { EF_NO_OWNER, EF_CAR, EF_BODY, EF_ENGINE, EF_DRIVETRAIN };
static const unsigned char g_ef_owner[EF_SLOTS] = {
    EF_CAR, EF_CAR, EF_CAR, EF_NO_OWNER, EF_CAR, EF_CAR, EF_CAR, EF_CAR, EF_CAR, EF_CAR, EF_CAR, EF_CAR,         /* 0 Engine .. 11 RimSizeRear */
    EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE,  /* 12 Camshaft .. 21 Manifold */
    EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_ENGINE, EF_NO_OWNER, EF_ENGINE, EF_ENGINE, EF_ENGINE,                      /* 22 RestrictorPlate .. 29 Intercooler */
    EF_DRIVETRAIN, EF_DRIVETRAIN, EF_DRIVETRAIN, EF_DRIVETRAIN,                                                    /* 30 Clutch .. 33 Differential */
    EF_BODY, EF_BODY, EF_BODY, EF_BODY, EF_NO_OWNER, EF_NO_OWNER, EF_NO_OWNER, EF_BODY, EF_NO_OWNER};             /* 34 FrontBumper .. 42 MotorParts */

/* What the rule needs from the game; event_fix.inc fills it from the game database, the test from a fake. */
typedef struct {
    /* 1 and *owner when `id` is a row of slot k's table (owner as g_ef_owner says), 0 when it is not */
    int (*owner)(void *ctx, int k, unsigned id, unsigned *owner);
    /* 1 and *id with the stock row of slot k's table for `owner`, 0 when that owner has none */
    int (*stock)(void *ctx, int k, unsigned owner, unsigned *id);
    /* 1 and *what for row `id` of slot 0 (its EngineID), 1 (its DrivetrainID) or 2 (its CarBodyID / 1000), 0 when the row is not known */
    int (*installs)(void *ctx, int k, unsigned id, unsigned *what);
    void *ctx;
} ef_ops;

enum { EF_CLEAN, EF_FIXED, EF_CANNOT };

/* A part row a mod added to the game's database at run time, not a row of the game's own [C: FH2 Extra Options 0.6.0 winmm.dll SQL]: with
 * AllEngineSwaps it inserts List_UpgradeEngine rows "1073741824 + S.Ordinal*32768 + E.EngineID AS Id, S.Ordinal AS Ordinal, ... E.EngineID" (the donor's
 * engine under the player's own car), with AllTransmissionSwaps List_UpgradeDrivetrainTransmission rows "536870912 + T.DrivetrainID*65536 + ..."
 * (a donor's gearbox under the player's own drivetrain), so by its owner such a row looks like the car's own. The game's own part ids end at 5,000,004
 * (the server's database of 2026-11-19, all 43 tables); anything from 0x20000000 up is a mod's row and goes back to stock like a foreign one. */
#define EF_MOD_ROW_MIN 0x20000000u

/* who a row of slot k must belong to on car `car` with the parts `parts` -> 1 and *who, 0 when it cannot be told */
static int ef_expected(unsigned car, int k, const unsigned *parts, const ef_ops *ops, unsigned *who)
{
    switch (g_ef_owner[k]) {
    case EF_CAR:
        *who = car;
        return 1;
    case EF_BODY:
        return parts[2] != EF_NONE && ops->installs(ops->ctx, 2, parts[2], who);
    case EF_ENGINE:
        return parts[0] != EF_NONE && ops->installs(ops->ctx, 0, parts[0], who);
    case EF_DRIVETRAIN:
        return parts[1] != EF_NONE && ops->installs(ops->ctx, 1, parts[1], who);
    }
    return 0;
}

/* Car `car` with the part ids `in`: every part that belongs to another owner than the car's own (an engine row of another car, a gearbox of another
 * drivetrain, a camshaft of another engine, ...) becomes the stock part of the car's own owner; nothing else changes. The slots go in order, so the
 * engine, drivetrain and body (0-2) are the car's own before the parts that belong to them are judged: when a foreign engine goes back to stock,
 * the swapped engine's own parts are foreign to the stock engine and follow it. A part slot whose own owner has no stock row becomes empty (no turbo
 * on an engine that has none); the engine, drivetrain, body and tires cannot, so such a car is EF_CANNOT and left as it is. A mod's added row
 * (EF_MOD_ROW_MIN) is foreign whatever owner it claims; another part id that is no row of its slot's table is not judged here (picker_pi.inc,
 * event_stock.inc and the server handle those).
 * -> EF_CLEAN (out = in), EF_FIXED (out and *changed, one bit per slot replaced) or EF_CANNOT. */
static int ef_fix(unsigned car, const unsigned in[EF_SLOTS], const ef_ops *ops, unsigned out[EF_SLOTS], unsigned long long *changed)
{
    int k;
    unsigned have = 0, want = 0, stock = 0;

    *changed = 0;
    for (k = 0; k < EF_SLOTS; k++) {
        out[k] = in[k];
    }
    for (k = 0; k < EF_SLOTS; k++) {
        int added;
        if (g_ef_owner[k] == EF_NO_OWNER || out[k] == EF_NONE) {
            continue;
        }
        added = out[k] >= EF_MOD_ROW_MIN;
        if (!added && !ops->owner(ops->ctx, k, out[k], &have)) {
            continue;
        }
        if (!ef_expected(car, k, out, ops, &want) || (!added && have == want)) {
            continue;
        }
        if (ops->stock(ops->ctx, k, want, &stock)) {
            out[k] = stock;
        } else if (k <= 2 || k == 8) {
            return EF_CANNOT;
        } else {
            out[k] = EF_NONE;
        }
        *changed |= 1ull << k;
    }
    return *changed ? EF_FIXED : EF_CLEAN;
}

/* The 44 tune values of a car setup [C: CCarDynamics::ApplyTuneSlidersToSetup +12e3680 and ApplyAxleTuneSlidersToSetup +12e3a90 name each one to
 * Tune_ResolveSliderFraction; the Toe name is the string at +22224c0]: 0 FrontDownforce, 1 RearDownforce, 2 FinalDrive, 3 BrakePressure, 4 BrakeBias,
 * 5 Handbrake, 6 CenterDiffTorqueSplit, 7 STMScale, 8 TCSTakeOffSlip, 9 TCSMovingSlip, 10 SteerSpeed, 11 SteerAmount, 12-22 front axle and 23-33 rear
 * axle (each: TirePressure, Camber, Toe, Caster, SpringRate, Swaybar, RideHeight, DampBump, DampRebound, LSDTorqueAccel, LSDTorqueDecel), 34 FuelLoad,
 * 35-43 GearRatio (35, the first, the game sets to 0.5 itself).
 * Which values go back to stock with a replaced part [my reading of what each part makes tunable, not from the game]: the gearbox, the drivetrain and
 * the engine the gearing (final drive, gear ratios: they were set for the swapped part); the drivetrain and the differential the centre split and both
 * LSDs; the springs and dampers the alignment, springs, ride height and dampers; each anti-roll bar its swaybar; the tire compound both tire pressures;
 * the rear wing and the front bumper their downforce; the brakes pressure and bias. */
#define EF_AXLE(a) ((1ull << (12 + (a))) | (1ull << (23 + (a))))
#define EF_GEARING ((1ull << 2) | (0x1ffull << 35))
static unsigned long long ef_tune_mask(unsigned long long changed)
{
    unsigned long long m = 0;
    if (changed & ((1ull << 0) | (1ull << 1) | (1ull << 31))) m |= EF_GEARING;
    if (changed & ((1ull << 1) | (1ull << 33))) m |= (1ull << 6) | EF_AXLE(9) | EF_AXLE(10);
    if (changed & (1ull << 5)) m |= EF_AXLE(1) | EF_AXLE(2) | EF_AXLE(3) | EF_AXLE(4) | EF_AXLE(6) | EF_AXLE(7) | EF_AXLE(8);
    if (changed & (1ull << 6)) m |= 1ull << 17;
    if (changed & (1ull << 7)) m |= 1ull << 28;
    if (changed & (1ull << 8)) m |= EF_AXLE(0);
    if (changed & (1ull << 9)) m |= 1ull << 1;
    if (changed & (1ull << 34)) m |= 1ull << 0;
    if (changed & (1ull << 4)) m |= (1ull << 3) | (1ull << 4);
    return m;
}

/* ---- the garage list query ----
 * GarageCarCollection::vfunc_16 (+c2ec60) lists the garage with "SELECT Garage.* , ... FROM <garage table> AS Garage ... WHERE <filter> AND ...",
 * the filter from CarFilter_BuildGarageWhereClause (+984850: "Garage.ClassId=", "Garage.PerformanceIndex>=" / "<") or
 * CarCollection_AppendPerfRatingRestriction (+c16500: Garage.PerformanceIndex ranges) [C: decompile]. Those are the only qualified uses of the two
 * columns in that query (the select list takes Garage.*, the order is by the unqualified PerformanceIndex), so each one becomes the fixed value for
 * the cars that have one: "(CASE Garage.Id WHEN <id> THEN <value> ... ELSE Garage.ClassId END)". A car whose fix keeps the class its tile shows
 * (the view's ClassId) is listed by its fixed PI. A car the fix moves into another class (`drop`) gets NULL in every term, so no comparison of the
 * filter holds for it and it is not listed at all, not even as a tile (user rule 2026-11-19: "if ANY of these changes lead to class downgrade,
 * immediately remove it from list"; dev 2026-11-20 03:11 UTC: a Mercedes shown S1 807, A 745 with its own gearbox, was listed and raced in an
 * A event). A list without class or PI terms (an any-class event) is not rewritten: every car is in it and races fixed. */
typedef struct {
    unsigned garage_id, klass, drop;
    float pi;
} ef_row;

typedef struct {
    char *p;
    unsigned len, cap;
    int full;
} ef_buf;

static void ef_put(ef_buf *b, const char *s, unsigned n)
{
    if (b->full || b->len + n + 1 > b->cap) {
        b->full = 1;
        return;
    }
    while (n--) b->p[b->len++] = *s++;
    b->p[b->len] = 0;
}

static void ef_puts(ef_buf *b, const char *s)
{
    unsigned n = 0;
    while (s[n]) n++;
    ef_put(b, s, n);
}

static void ef_putu(ef_buf *b, unsigned long long v)
{
    char d[24];
    unsigned n = 0, i;
    char r[24];
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    for (i = 0; i < n; i++) r[i] = d[n - 1 - i];
    ef_put(b, r, n);
}

/* a PI as a decimal with nine places (the stored values are floats of the 0..1 scale; nine places keep every float of it apart) */
static void ef_putpi(ef_buf *b, float pi)
{
    double v = pi < 0 ? 0 : (double)pi;
    unsigned long long whole = (unsigned long long)v, frac = (unsigned long long)((v - (double)whole) * 1e9 + 0.5);
    char d[10];
    int i;
    if (frac >= 1000000000ull) {
        whole++;
        frac -= 1000000000ull;
    }
    ef_putu(b, whole);
    for (i = 8; i >= 0; i--) {
        d[i] = (char)('0' + frac % 10);
        frac /= 10;
    }
    ef_put(b, ".", 1);
    ef_put(b, d, 9);
}

static void ef_put_case(ef_buf *b, const ef_row *rows, unsigned n, int pi, const char *column)
{
    unsigned i;
    ef_puts(b, "(CASE Garage.Id");
    for (i = 0; i < n; i++) {
        ef_puts(b, " WHEN ");
        ef_putu(b, rows[i].garage_id);
        ef_puts(b, " THEN ");
        if (rows[i].drop) {
            ef_puts(b, "NULL");
        } else if (pi) {
            ef_putpi(b, rows[i].pi);
        } else {
            ef_putu(b, rows[i].klass);
        }
    }
    ef_puts(b, " ELSE ");
    ef_puts(b, column);
    ef_puts(b, " END)");
}

static int ef_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* 1 when `s` starts with `w` as a whole word */
static int ef_at_word(const char *s, const char *w)
{
    unsigned i = 0;
    for (; w[i]; i++) {
        if (s[i] != w[i]) return 0;
    }
    return !ef_word_char(s[i]);
}

/* `sql` with every "Garage.ClassId" and "Garage.PerformanceIndex" replaced by the fixed values of `rows` into out[cap]. "Garage.MaxUpgradeWizardPerfRating"
 * (CarCollection_AppendPerfRatingRestriction's other branch: a car that could be upgraded into the class is listed too) gets the fixed PI as
 * well, so a fixed car is only listed by what it is: nothing is upgraded in an online event (dev 2026-11-20 01:11 UTC: a car with swaps, S1 807
 * as built, A 748 to the server, was listed and picked in an S1 event). -> the number of replacements (0: nothing to replace, out is not to be
 * used), -1 when the result does not fit. */
static int ef_rewrite_sql(const char *sql, const ef_row *rows, unsigned n, char *out, unsigned cap)
{
    static const char k_class[] = "Garage.ClassId", k_pi[] = "Garage.PerformanceIndex", k_max[] = "Garage.MaxUpgradeWizardPerfRating";
    ef_buf b = {out, 0, cap, 0};
    int replaced = 0;
    const char *s = sql;
    if (!cap) return -1;
    out[0] = 0;
    while (*s) {
        int prev_ok = s == sql || !ef_word_char(s[-1]);
        if (prev_ok && ef_at_word(s, k_class)) {
            ef_put_case(&b, rows, n, 0, k_class);
            s += sizeof k_class - 1;
            replaced++;
        } else if (prev_ok && ef_at_word(s, k_pi)) {
            ef_put_case(&b, rows, n, 1, k_pi);
            s += sizeof k_pi - 1;
            replaced++;
        } else if (prev_ok && ef_at_word(s, k_max)) {
            ef_put_case(&b, rows, n, 1, k_max);
            s += sizeof k_max - 1;
            replaced++;
        } else {
            ef_put(&b, s, 1);
            s++;
        }
    }
    if (b.full) return -1;
    return replaced;
}

/* The table that holds the garage's part ids for the list's FROM name. The list reads the view CareerDb_CreateGarageView (+986240) makes,
 * "View_" + <profile prefix>Career_Garage, which keeps the class, PI and car columns but no part ids; they are in the table it is made from, the
 * same name without "View_" [C: decompile; the template NewProfile_Career_Garage in the car database has Id, CarId, the 43 part columns by
 * CarPartSlotTable's short names and WheelStyle]. A name without "View_" is taken as it is. -> 1 and out[cap], 0 when it does not fit. */
static int ef_garage_table(const char *from, char *out, unsigned cap)
{
    static const char view[] = "View_";
    unsigned i = 0, n;
    while (view[i] && from[i] == view[i]) i++;
    if (view[i] || !from[i]) i = 0;
    for (n = 0; from[i + n]; n++) {
        if (n + 1 >= cap) return 0;
        out[n] = from[i + n];
    }
    out[n] = 0;
    return n > 0;
}

/* the garage list query of GarageCarCollection::vfunc_16 (its fixed start) whose filter names a class or PI -> 1, and the garage table name of
 * "FROM <name> AS Garage" into name[cap] */
static int ef_garage_query(const char *sql, char *name, unsigned cap)
{
    static const char head[] = "SELECT Garage.* , 0 AS IsOwned", from[] = " FROM ", as[] = " AS Garage ";
    const char *s = sql, *t;
    unsigned i, n;
    for (i = 0; head[i]; i++) {
        if (sql[i] != head[i]) return 0;
    }
    for (; *s; s++) {
        if (ef_at_word(s, "Garage.ClassId") || ef_at_word(s, "Garage.PerformanceIndex")) break;
    }
    if (!*s) return 0;
    for (s = sql; *s; s++) {
        for (i = 0; from[i] && s[i] == from[i]; i++) {}
        if (from[i]) continue;
        t = s + i;
        for (n = 0; t[n] && t[n] != ' '; n++) {}
        for (i = 0; as[i] && t[n + i] == as[i]; i++) {}
        if (as[i] || n == 0 || n >= cap) continue;
        for (i = 0; i < n; i++) name[i] = t[i];
        name[n] = 0;
        return 1;
    }
    return 0;
}

/* An online event's car select is open at `now` (ticks, ms): its countdown ran (`armed`) or an event car list was made (`listed`) within
 * `window` ms, and not before the last race start (`race_started`; 0 = none yet). After the race start a committed car is the player's own
 * again, never the event's (event_fix.inc ef_in_select). */
static int ef_select_open(unsigned long long now, unsigned long long armed, unsigned long long listed, unsigned long long race_started,
                          unsigned long long window)
{
    if (armed && armed <= race_started) armed = 0;
    if (listed && listed <= race_started) listed = 0;
    return (armed && now - armed <= window) || (listed && now - listed <= window);
}
