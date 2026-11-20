#!/usr/bin/env bash
# Parts of other cars in online events (src/event_fix_rule.inc): which parts go back to the car's own stock ones, which tune values follow them, and
# the garage list query with the fixed class and PI, natively. The expected values come from the game database's ownership model (an Engine row belongs
# to a car, a camshaft to an engine, a gearbox to a drivetrain, a bumper to the car whose body installs it) and the user's rules of 2026-11-19.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include "event_fix_rule.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

/* A small parts world. Car 1005 (the player's): stock engine row 1005001 (engine 501), a legitimate swap 1005002 (engine 502, offered to 1005), stock
 * drivetrain 1005100 (drivetrain 61), body 1005000 (its own body). Car 2000 (a donor): engine row 2000001 (engine 900), drivetrain 2000100 (90),
 * bumper 2000340, tires 2000080. Car 3000 has its own engine, drivetrain and body but no stock tires; car 4000 has no rows at all. */
typedef struct { int slot; unsigned id, owner, stock, installs; } row;
static const row g_rows[] = {
    {0, 1005001, 1005, 1, 501}, {0, 1005002, 1005, 0, 502}, {0, 2000001, 2000, 1, 900},
    {1, 1005100, 1005, 1, 61}, {1, 2000100, 2000, 1, 90},
    {2, 1005000, 1005, 1, 1005}, {2, 2000000, 2000, 1, 2000},
    {0, 3000001, 3000, 1, 503}, {1, 3000100, 3000, 1, 63}, {2, 3000000, 3000, 1, 3000},
    {8, 1005080, 1005, 1, 0}, {8, 2000080, 2000, 1, 0},
    {12, 50100, 501, 1, 0}, {12, 50101, 501, 0, 0}, {12, 50200, 502, 1, 0}, {12, 50201, 502, 0, 0}, {12, 90001, 900, 1, 0}, {12, 90002, 900, 0, 0},
    {24, 90024, 900, 0, 0},
    {31, 61031, 61, 1, 0}, {31, 61032, 61, 0, 0}, {31, 90031, 90, 0, 0},
    {34, 1005340, 1005, 1, 0}, {34, 2000340, 2000, 1, 0},
    /* rows Extra Options 0.6.0 adds at run time, under the player's own car / drivetrain (its SQL's numbering): the donor engine 900 for car 1005,
     * and gearbox source key 90*8+1 (drivetrain 90, level 1) for drivetrain 61 */
    {0, 1073741824u + 1005u * 32768u + 900u, 1005, 0, 900},
    {31, 536870912u + 61u * 65536u + 90u * 8u + 1u, 61, 0, 0},
};
#define NROWS (sizeof g_rows / sizeof g_rows[0])

static int f_owner(void *c, int k, unsigned id, unsigned *o)
{
    unsigned i;
    (void)c;
    for (i = 0; i < NROWS; i++) if (g_rows[i].slot == k && g_rows[i].id == id) { *o = g_rows[i].owner; return 1; }
    return 0;
}
static int f_stock(void *c, int k, unsigned owner, unsigned *id)
{
    unsigned i;
    (void)c;
    for (i = 0; i < NROWS; i++) if (g_rows[i].slot == k && g_rows[i].owner == owner && g_rows[i].stock) { *id = g_rows[i].id; return 1; }
    return 0;
}
static int f_installs(void *c, int k, unsigned id, unsigned *w)
{
    unsigned i;
    (void)c;
    for (i = 0; i < NROWS; i++) if (g_rows[i].slot == k && g_rows[i].id == id) { *w = g_rows[i].installs; return 1; }
    return 0;
}
static const ef_ops g_ops = {f_owner, f_stock, f_installs, NULL};

/* car 1005 as it comes from the dealer, plus a race camshaft and a race gearbox of its own */
static void base(unsigned p[EF_SLOTS])
{
    int k;
    for (k = 0; k < EF_SLOTS; k++) p[k] = EF_NONE;
    p[0] = 1005001; p[1] = 1005100; p[2] = 1005000; p[8] = 1005080; p[12] = 50101; p[31] = 61032; p[34] = 1005340;
}

static int same(const unsigned *a, const unsigned *b)
{
    return memcmp(a, b, EF_SLOTS * sizeof *a) == 0;
}

int main(void)
{
    unsigned in[EF_SLOTS], out[EF_SLOTS], want[EF_SLOTS];
    unsigned long long changed;
    char sql[8192];
    ef_row rows[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
    int n;

    /* the car's own parts, upgraded or not, are left alone */
    base(in);
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_CLEAN && changed == 0 && same(in, out));

    /* an engine swap the game offers this car (row of car 1005) with that engine's own camshaft is legitimate */
    base(in);
    in[0] = 1005002; in[12] = 50201;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_CLEAN && same(in, out));

    /* Extra Options: the donor's engine, its race camshaft and its turbo. The engine goes back to the car's stock one; the camshaft and turbo belong to
     * the donor engine, not to the stock engine, so they become the stock engine's stock camshaft and no turbo (engine 501 has none). Nothing else. */
    base(in);
    in[0] = 2000001; in[12] = 90002; in[24] = 90024;
    base(want);
    want[12] = 50100;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_FIXED);
    CHECK(changed == ((1ull << 0) | (1ull << 12) | (1ull << 24)));
    CHECK(same(out, want));

    /* Extra Options: another drivetrain's gearbox. Only the gearbox goes back, to the stock gearbox of the car's own drivetrain (61). */
    base(in);
    in[31] = 90031;
    base(want);
    want[31] = 61031;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_FIXED && changed == (1ull << 31) && same(out, want));

    /* Extra Options' own engine swap: its added row claims car 1005 as owner, but no row of the game's database has that id. It goes back to the
     * stock engine, and the donor engine's camshaft follows it, exactly as for a donor car's row. */
    base(in);
    in[0] = 1073741824u + 1005u * 32768u + 900u; in[12] = 90002;
    base(want);
    want[12] = 50100;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_FIXED && changed == ((1ull << 0) | (1ull << 12)) && same(out, want));

    /* Extra Options' own gearbox swap: a donor gearbox under the car's own drivetrain goes back to that drivetrain's stock gearbox */
    base(in);
    in[31] = 536870912u + 61u * 65536u + 90u * 8u + 1u;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_FIXED && changed == (1ull << 31) && out[31] == 61031);

    /* a bumper of another car's body goes back to the car's own body's stock bumper */
    base(in);
    in[34] = 2000340;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_FIXED && changed == (1ull << 34) && out[34] == 1005340);

    /* a part id that is no row of its table is not judged here (the unknown-part checks own it) */
    base(in);
    in[12] = 77777;
    CHECK(ef_fix(1005, in, &g_ops, out, &changed) == EF_CLEAN && same(in, out));

    /* a car whose foreign tires or engine have no stock row of its own to go back to cannot be fixed: left as built */
    base(in);
    in[0] = 3000001; in[1] = 3000100; in[2] = 3000000; in[8] = 2000080; in[12] = EF_NONE; in[31] = EF_NONE; in[34] = EF_NONE;
    CHECK(ef_fix(3000, in, &g_ops, out, &changed) == EF_CANNOT);
    in[8] = EF_NONE;
    CHECK(ef_fix(3000, in, &g_ops, out, &changed) == EF_CLEAN);                    /* the same car without those tires is clean */
    base(in);
    in[0] = 2000001;
    CHECK(ef_fix(4000, in, &g_ops, out, &changed) == EF_CANNOT);

    /* the input is never written */
    base(in);
    in[31] = 90031;
    ef_fix(1005, in, &g_ops, out, &changed);
    CHECK(in[31] == 90031);

    /* tune values that go back to stock with a replaced part (the game's 44 tune values: 2 FinalDrive, 35-43 GearRatio, 12+a / 23+a the axles) */
    CHECK(ef_tune_mask(1ull << 31) == ((1ull << 2) | (0x1ffull << 35)));                 /* gearbox: final drive and gear ratios */
    CHECK(ef_tune_mask(1ull << 0) == ((1ull << 2) | (0x1ffull << 35)));                  /* engine: the gearing set for the swapped engine */
    CHECK(ef_tune_mask(1ull << 12) == 0);                                                 /* an engine part alone: nothing to tune */
    CHECK(ef_tune_mask(1ull << 6) == (1ull << 17) && ef_tune_mask(1ull << 7) == (1ull << 28));  /* anti-roll bars: their swaybar */
    CHECK(ef_tune_mask(1ull << 8) == ((1ull << 12) | (1ull << 23)));                     /* tires: both pressures */
    CHECK(ef_tune_mask(1ull << 33) == ((1ull << 6) | (1ull << 21) | (1ull << 22) | (1ull << 32) | (1ull << 33)));  /* differential: centre, LSDs */
    CHECK(ef_tune_mask(1ull << 5) == ((1ull << 13) | (1ull << 14) | (1ull << 15) | (1ull << 16) | (1ull << 18) | (1ull << 19) | (1ull << 20) |
                                      (1ull << 24) | (1ull << 25) | (1ull << 26) | (1ull << 27) | (1ull << 29) | (1ull << 30) | (1ull << 31)));
    CHECK((ef_tune_mask(~0ull) & ((1ull << 5) | (0x7ull << 7) | (0x3ull << 10) | (1ull << 34))) == 0);  /* handbrake, assists, steering, fuel: never */

    /* the garage list query (the shape of GarageCarCollection::vfunc_16 with CarFilter_BuildGarageWhereClause's terms) */
    {
        const char *q = "SELECT Garage.* , 0 AS IsOwned, (Garage.Id = 4) AS IsCurrent FROM Profile0_Garage AS Garage  LEFT JOIN Data_Car ON Garage.ModelId=Data_Car.Id "
                        "WHERE Garage.ClassId=3 AND Garage.PerformanceIndex>=0.500000 AND Garage.PerformanceIndex<0.600000 AND 1 AND Garage.Id!=1215 "
                        "ORDER BY IsCurrent DESC, PerformanceIndex DESC";
        const char *expect = "SELECT Garage.* , 0 AS IsOwned, (Garage.Id = 4) AS IsCurrent FROM Profile0_Garage AS Garage  LEFT JOIN Data_Car ON Garage.ModelId=Data_Car.Id "
                             "WHERE (CASE Garage.Id WHEN 7 THEN 2 WHEN 9 THEN 3 ELSE Garage.ClassId END)=3 AND "
                             "(CASE Garage.Id WHEN 7 THEN 0.500000000 WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END)>=0.500000 AND "
                             "(CASE Garage.Id WHEN 7 THEN 0.500000000 WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END)<0.600000 AND 1 AND Garage.Id!=1215 "
                             "ORDER BY IsCurrent DESC, PerformanceIndex DESC";
        char table[64];
        rows[0].garage_id = 7; rows[0].klass = 2; rows[0].pi = 0.5f;
        rows[1].garage_id = 9; rows[1].klass = 3; rows[1].pi = 0.75f;
        CHECK(ef_garage_query(q, table, sizeof table) && strcmp(table, "Profile0_Garage") == 0);
        n = ef_rewrite_sql(q, rows, 2, sql, sizeof sql);
        CHECK(n == 3);
        if (strcmp(sql, expect) != 0) printf("got  %s\nwant %s\n", sql, expect);
        CHECK(strcmp(sql, expect) == 0);
        CHECK(ef_rewrite_sql(q, rows, 2, sql, 100) == -1);                           /* too small: the caller runs the game's text */
        /* an any-class list (no class or PI term) is not a query to rewrite */
        CHECK(!ef_garage_query("SELECT Garage.* , 0 AS IsOwned FROM G AS Garage  WHERE 1 AND 1 AND Garage.Id!=1215 ORDER BY PerformanceIndex DESC", table, sizeof table));
        /* other queries are not either */
        CHECK(!ef_garage_query("SELECT Id FROM List_UpgradeEngine WHERE Garage.ClassId=3", table, sizeof table));
        /* CarCollection_AppendPerfRatingRestriction's shape: a car that could be upgraded into the class is listed too; a fixed car is listed only
         * by its fixed PI (S1 starts at 0.800 here: the fixed car 7, 0.5, is out by both branches) */
        {
            const char *q2 = "SELECT Garage.* , 0 AS IsOwned FROM G AS Garage  WHERE 1 AND ((Garage.MaxUpgradeWizardPerfRating >= 0.800000 AND StockPerformanceRating <= 0.900000) "
                             "OR (Garage.PerformanceIndex >= 0.800000 AND Garage.PerformanceIndex <= 0.900000)) AND Garage.Id!=1215";
            const char *expect2 = "SELECT Garage.* , 0 AS IsOwned FROM G AS Garage  WHERE 1 AND (("
                                  "(CASE Garage.Id WHEN 7 THEN 0.500000000 WHEN 9 THEN 0.750000000 ELSE Garage.MaxUpgradeWizardPerfRating END) >= 0.800000 AND StockPerformanceRating <= 0.900000) "
                                  "OR ((CASE Garage.Id WHEN 7 THEN 0.500000000 WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END) >= 0.800000 AND "
                                  "(CASE Garage.Id WHEN 7 THEN 0.500000000 WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END) <= 0.900000)) AND Garage.Id!=1215";
            CHECK(ef_garage_query(q2, table, sizeof table) && strcmp(table, "G") == 0);
            n = ef_rewrite_sql(q2, rows, 2, sql, sizeof sql);
            CHECK(n == 3);
            if (strcmp(sql, expect2) != 0) printf("got  %s\nwant %s\n", sql, expect2);
            CHECK(strcmp(sql, expect2) == 0);
        }
        /* a car the fix moves into another class than its tile shows (dropped) gets NULL in every term: no comparison holds for it, so neither
         * branch lists it, while car 9 (fixed in its class) stays by its fixed PI */
        {
            const char *q3 = "SELECT Garage.* , 0 AS IsOwned FROM G AS Garage  WHERE Garage.ClassId=3 AND ((Garage.MaxUpgradeWizardPerfRating >= 0.7 AND StockPerformanceRating <= 0.8) "
                             "OR (Garage.PerformanceIndex >= 0.7 AND Garage.PerformanceIndex <= 0.8))";
            const char *expect3 = "SELECT Garage.* , 0 AS IsOwned FROM G AS Garage  WHERE (CASE Garage.Id WHEN 7 THEN NULL WHEN 9 THEN 3 ELSE Garage.ClassId END)=3 AND (("
                                  "(CASE Garage.Id WHEN 7 THEN NULL WHEN 9 THEN 0.750000000 ELSE Garage.MaxUpgradeWizardPerfRating END) >= 0.7 AND StockPerformanceRating <= 0.8) "
                                  "OR ((CASE Garage.Id WHEN 7 THEN NULL WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END) >= 0.7 AND "
                                  "(CASE Garage.Id WHEN 7 THEN NULL WHEN 9 THEN 0.750000000 ELSE Garage.PerformanceIndex END) <= 0.8))";
            rows[0].drop = 1;
            n = ef_rewrite_sql(q3, rows, 2, sql, sizeof sql);
            CHECK(n == 4);
            if (strcmp(sql, expect3) != 0) printf("got  %s\nwant %s\n", sql, expect3);
            CHECK(strcmp(sql, expect3) == 0);
            rows[0].drop = 0;
        }
        /* the list reads the game's view View_<prefix>Career_Garage (no part columns); the parts are in the table under it */
        CHECK(ef_garage_table("View_Profile0_Career_Garage", table, sizeof table) && strcmp(table, "Profile0_Career_Garage") == 0);
        CHECK(ef_garage_table("Profile0_Career_Garage", table, sizeof table) && strcmp(table, "Profile0_Career_Garage") == 0);
        CHECK(!ef_garage_table("View_Profile0_Career_Garage", table, 8));                /* does not fit */
        CHECK(ef_garage_table("View_", table, sizeof table) && strcmp(table, "View_") == 0);  /* nothing after the prefix: taken as it is */
        /* whole words only */
        CHECK(ef_rewrite_sql("x Garage.PerformanceIndexes MyGarage.ClassId y", rows, 2, sql, sizeof sql) == 0);
    }
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("event fix rule: all checks passed\n");
    return 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
