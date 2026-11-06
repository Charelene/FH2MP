/* Isolated tests of src/picker_pi.inc: the decision (unknown part id or unusable PI -> the stock car's class and PI, a clean car untouched) and the hook
 * around the game's attribute function with a faked game (garage car object, attribute block, database objects). The game is never loaded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_log[16384];
static int g_lines;
static int read_game_memory(const void *p, void *out, SIZE_T n) { if ((UINT_PTR)p < 0x10000) return 0; memcpy(out, p, n); return 1; }
static unsigned char *g_fake_img;
static unsigned char *find_fh2_image(void) { return g_fake_img; }
static int g_code_matches = 1;
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag; (void)what; (void)img; (void)rva; (void)expect; (void)n;
    return g_code_matches;
}
static void *g_hooked_target;
static unsigned char *fake_game_orig(void *car, unsigned char *out);
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name; (void)detour;
    g_hooked_target = target;
    *original = (void *)fake_game_orig;
    return 1;
}
static void log_begin(void) { g_lines++; strcat(g_log, "|"); }
static void log_raw(const char *s) { strcat(g_log, s); }
static void log_dec(unsigned long v) { char b[24]; sprintf(b, "%lu", v); strcat(g_log, b); }
#include "src/picker_pi.inc"

/* ---- a fake database: (table, id) pairs and one Data_Car row ---- */
typedef struct { const char *table; unsigned id; } part_row;
static part_row g_rows[64];
static int g_nrows;
static unsigned g_exists_calls, g_stock_calls;
static int g_db_error, g_no_stock;
static unsigned g_stock_class = 1;
static float g_stock_pi = 0.25f;
static unsigned g_stock_car = 4242;

static int fake_exists(void *ctx, const char *table, unsigned id)
{
    int i;
    (void)ctx;
    g_exists_calls++;
    if (g_db_error) return -1;
    for (i = 0; i < g_nrows; i++) if (!strcmp(g_rows[i].table, table) && g_rows[i].id == id) return 1;
    return 0;
}
static int fake_stock(void *ctx, unsigned car, unsigned *klass, float *pi)
{
    (void)ctx;
    g_stock_calls++;
    if (g_db_error) return -1;
    if (g_no_stock || car != g_stock_car) return 0;
    *klass = g_stock_class; *pi = g_stock_pi;
    return 1;
}
static const ppi_ops g_ops = {fake_exists, fake_stock, NULL};

static void add_row(const char *table, unsigned id) { g_rows[g_nrows].table = table; g_rows[g_nrows].id = id; g_nrows++; }
static void reset(void)
{
    g_log[0] = 0; g_lines = 0;
    memset(g_ppi_cache, 0, sizeof g_ppi_cache); g_ppi_next = 0; g_ppi_logged = 0; g_ppi_declined = 0;
    g_nrows = 0; g_exists_calls = g_stock_calls = 0; g_db_error = g_no_stock = 0;
    g_stock_class = 1; g_stock_pi = 0.25f; g_stock_car = 4242;
}

/* A modified car whose part ids are all known: engine, drivetrain, body, brakes, tires, camshaft. */
static void clean_parts(unsigned *parts)
{
    int k;
    for (k = 0; k < PPI_SLOTS; k++) parts[k] = PPI_NONE;
    parts[0] = 100; parts[1] = 200; parts[2] = 4242000; parts[4] = 300; parts[8] = 500; parts[12] = 400;
    add_row("Engine", 100); add_row("Drivetrain", 200); add_row("CarBody", 4242000); add_row("Brakes", 300);
    add_row("TireCompound", 500); add_row("EngineCamshaft", 400);
}
/* The attribute block as the game fills it: id short at 0x1e, the PI as a double at 8, the class byte at 0x22; every other byte a marker. */
static void make_out(unsigned char *out, unsigned car, unsigned klass, float pi)
{
    short id = (short)car;
    double d = (double)pi;
    memset(out, 0xab, PPI_OUT_SIZE);
    memcpy(out + PPI_OUT_ID, &id, 2); memcpy(out + PPI_OUT_PI, &d, 8); out[PPI_OUT_CLASS] = (unsigned char)klass;
}
static double out_pi(const unsigned char *out) { double d; memcpy(&d, out + PPI_OUT_PI, 8); return d; }
static int others_untouched(const unsigned char *out)
{
    unsigned i;
    for (i = 0; i < PPI_OUT_SIZE; i++) {
        if ((i >= PPI_OUT_PI && i < PPI_OUT_PI + 8) || i == PPI_OUT_ID || i == PPI_OUT_ID + 1 || i == PPI_OUT_CLASS) continue;
        if (out[i] != 0xab) return 0;
    }
    return 1;
}

static void test_decision(void)
{
    unsigned parts[PPI_SLOTS];
    unsigned char out[PPI_OUT_SIZE], before[PPI_OUT_SIZE];
    ppi_verdict v;

    /* a clean car, however fast, is passed through untouched and silently */
    reset(); clean_parts(parts); make_out(out, 4242, 5, 0.9f); memcpy(before, out, sizeof out);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops));
    assert(!memcmp(out, before, sizeof out) && g_lines == 0 && g_stock_calls == 0);

    /* an unknown part id (the camshaft slot): class and PI become the stock car's, nothing else changes, one line names car, values and the part */
    reset(); clean_parts(parts); parts[12] = 4242999; make_out(out, 4242, 5, 0.9f);
    assert(ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops));
    assert(out[PPI_OUT_CLASS] == 1 && out_pi(out) == (double)0.25f && others_untouched(out));
    assert(g_lines == 1 && strstr(g_log, "car 4242 ") && strstr(g_log, "stock class 1 and PI 0.2500") && strstr(g_log, "class 5 and PI 0.9000") &&
           strstr(g_log, "slot 12 holds part id 4242999") && strstr(g_log, "in no part table"));

    /* the same car again: decided from the cache (no new part queries), substituted again, the line rate limit counts it */
    { unsigned calls = g_exists_calls; make_out(out, 4242, 5, 0.9f);
      assert(ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && g_exists_calls == calls && out[PPI_OUT_CLASS] == 1); }

    /* an unknown id in an ordinary slot (rear wing) is caught; the same id as a row of another table is only reported by the server, so it passes */
    reset(); clean_parts(parts); parts[9] = 777; make_out(out, 4242, 3, 0.6f);
    assert(ppi_fixup(4242, parts, 3, 0.6f, out, &g_ops) && strstr(g_log, "slot 9 holds part id 777"));
    reset(); clean_parts(parts); parts[9] = 777; add_row("Engine", 777); make_out(out, 4242, 3, 0.6f);
    assert(!ppi_fixup(4242, parts, 3, 0.6f, out, &g_ops) && g_lines == 0);

    /* the camshaft and tire slots must hold a row of their own table */
    reset(); clean_parts(parts); parts[8] = 777; add_row("Engine", 777); make_out(out, 4242, 3, 0.6f);
    assert(ppi_fixup(4242, parts, 3, 0.6f, out, &g_ops) && strstr(g_log, "slot 8 holds part id 777") && strstr(g_log, "row of another part table"));
    reset(); clean_parts(parts); parts[12] = 777; add_row("Brakes", 777); make_out(out, 4242, 3, 0.6f);
    v = ppi_check_parts(parts, &g_ops);
    assert(v.kind == PPI_WRONG_TABLE && v.slot == 12 && v.id == 777);

    /* an electric car's slot 0 holds its Motor row */
    reset(); clean_parts(parts); parts[0] = 2101000; add_row("Motor", 2101000); make_out(out, 4242, 2, 0.5f);
    assert(!ppi_fixup(4242, parts, 2, 0.5f, out, &g_ops));

    /* -1 and the slots without a part type (3, 26) are never judged */
    reset(); clean_parts(parts); parts[3] = 123456; parts[26] = 654321; make_out(out, 4242, 2, 0.5f);
    assert(ppi_check_parts(parts, &g_ops).kind == PPI_CLEAN);

    /* an unusable PI (NaN, negative, huge) on a car whose parts are all known */
    { const float bad[3] = {NAN, -0.5f, 1e30f}; int i;
      for (i = 0; i < 3; i++) {
          reset(); clean_parts(parts); make_out(out, 4242, 4, bad[i]);
          assert(ppi_fixup(4242, parts, 4, bad[i], out, &g_ops) && out[PPI_OUT_CLASS] == 1 && out_pi(out) == (double)0.25f);
          assert(strstr(g_log, "PI is not a usable number"));
      } }

    /* a stock car that already reports what the stock car has: nothing to change, no line */
    reset(); clean_parts(parts); parts[12] = 999; g_stock_class = 4; g_stock_pi = 0.5f; make_out(out, 4242, 4, 0.5f);
    assert(!ppi_fixup(4242, parts, 4, 0.5f, out, &g_ops) && g_lines == 0);

    /* declines: the attributes do not carry the car's values (wrong id, class, PI); no stock row; the database cannot answer */
    reset(); clean_parts(parts); parts[12] = 999; make_out(out, 4243, 5, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 5 && strstr(g_log, "layout is not as expected") && g_stock_calls == 0);
    reset(); clean_parts(parts); parts[12] = 999; make_out(out, 4242, 6, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 6);
    reset(); clean_parts(parts); parts[12] = 999; make_out(out, 4242, 5, 0.8f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out_pi(out) == (double)0.8f);
    reset(); clean_parts(parts); parts[12] = 999; g_no_stock = 1; make_out(out, 4242, 5, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 5 && strstr(g_log, "no stock class and PI"));
    reset(); clean_parts(parts); g_db_error = 1; make_out(out, 4242, 5, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 5 && strstr(g_log, "could not be queried"));
    /* an answer that failed is not cached: once the database answers, the car is judged */
    g_db_error = 0; clean_parts(parts); parts[12] = 999;
    assert(ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 1);
    /* a stock row with a nonsense class or PI is not used */
    reset(); clean_parts(parts); parts[12] = 999; g_stock_pi = NAN; make_out(out, 4242, 5, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 5);
    reset(); clean_parts(parts); parts[12] = 999; g_stock_class = 4000; make_out(out, 4242, 5, 0.9f);
    assert(!ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops) && out[PPI_OUT_CLASS] == 5);

    /* log rate limit: past 20 lines only every 50th */
    reset(); clean_parts(parts);
    { int i, base; for (i = 0; i < 100; i++) { parts[12] = 1000 + (unsigned)i; make_out(out, 4242, 5, 0.9f); assert(ppi_fixup(4242, parts, 5, 0.9f, out, &g_ops)); }
      base = g_lines; assert(base == 22); }
}

/* ---- the hook with a faked game ---- */
static unsigned char car_obj[0xab8];
static unsigned char holder_obj[0x80];
static void *g_vt_db[16], *g_vt_row[64];
static void *db_obj[2];
static int g_open_rows;
typedef struct { void **vt; int n; } fake_row;
static fake_row g_row_pool[8];
static int g_pool_used;
static int g_hook_orig_calls;

static const char *key_text(const ppi_key *key) { return key->capacity < 16 ? key->text.small : key->text.ptr; }
static void **fake_query(void *db, void **out, const ppi_key *sql)
{
    const char *q = key_text(sql);
    fake_row *r = &g_row_pool[g_pool_used++ % 8];
    unsigned id;
    assert(db == db_obj && strlen(q) == sql->size);
    r->vt = g_vt_row; r->n = 0;
    if (!strncmp(q, "SELECT Id FROM List_Upgrade", 27)) {
        char table[64]; const char *w = strstr(q, " WHERE Id=");
        assert(w && (size_t)(w - q - 27) < sizeof table);
        memcpy(table, q + 27, (size_t)(w - q - 27)); table[w - q - 27] = 0;
        id = (unsigned)strtoul(w + 10, NULL, 10);
        r->n = fake_exists(NULL, table, id);
    } else {
        assert(!strncmp(q, "SELECT ClassID, PerformanceIndex FROM Data_Car WHERE Id=", 56));
        r->n = g_no_stock || (unsigned)strtoul(q + 56, NULL, 10) != g_stock_car ? 0 : 1;
    }
    g_open_rows++;
    *out = r;
    return out;
}
static void fake_row_release(void *row) { (void)row; assert(g_open_rows > 0); g_open_rows--; }
static int fake_row_count(void *row) { return ((fake_row *)row)->n; }
static unsigned fake_row_uint(void *row, const ppi_key *key) { (void)row; assert(!strcmp(key_text(key), "ClassID")); return g_stock_class; }
static float fake_row_float(void *row, const ppi_key *key) { (void)row; assert(!strcmp(key_text(key), "PerformanceIndex")); return g_stock_pi; }

static unsigned char *fake_game_orig(void *car, unsigned char *out)
{
    const unsigned char *c = car;
    int id = 0; float pi = 0; int klass = 0; short sid; double d;
    g_hook_orig_calls++;
    if (!car) { memset(out, 0xab, PPI_OUT_SIZE); return out; }
    memcpy(&id, c + 0x40, 4); memcpy(&pi, c + 0x260, 4); memcpy(&klass, c + 0x2cc, 4);
    memset(out, 0xab, PPI_OUT_SIZE);
    sid = (short)id; d = (double)pi;
    memcpy(out + PPI_OUT_ID, &sid, 2); memcpy(out + PPI_OUT_PI, &d, 8); out[PPI_OUT_CLASS] = (unsigned char)klass;
    return out;
}
static void set_car(unsigned car, const unsigned *parts, unsigned klass, float pi)
{
    memset(car_obj, 0, sizeof car_obj);
    memcpy(car_obj, &(void *){g_fake_img + PPI_VTABLE_RVA}, 8);
    memcpy(car_obj + 0x40, &car, 4); memcpy(car_obj + 0x50, parts, 4 * PPI_SLOTS);
    memcpy(car_obj + 0x260, &pi, 4); memcpy(car_obj + 0x2cc, &klass, 4);
}

static void test_hook(void)
{
    unsigned parts[PPI_SLOTS];
    unsigned char out[PPI_OUT_SIZE];
    unsigned char *r;

    g_fake_img = calloc(1, 0x3a18000u);
    assert(g_fake_img);
    g_vt_db[PPI_DB_QUERY_SLOT / 8] = (void *)fake_query;
    g_vt_row[PPI_ROW_RELEASE_SLOT / 8] = (void *)fake_row_release;
    g_vt_row[PPI_ROW_COUNT_SLOT / 8] = (void *)fake_row_count;
    g_vt_row[PPI_ROW_UINT_SLOT / 8] = (void *)fake_row_uint;
    g_vt_row[PPI_ROW_FLOAT_SLOT / 8] = (void *)fake_row_float;
    db_obj[0] = g_vt_db;
    { void *h = holder_obj, *d = db_obj; memcpy(g_fake_img + PPI_HOLDER_RVA, &h, 8); memcpy(holder_obj + PPI_HOLDER_DB_OFFSET, &d, 8); }

    /* the patch: refuses and logs nothing itself when the game's bytes differ (code_is logs), hooks the function otherwise */
    reset(); g_code_matches = 0; g_hooked_target = NULL;
    assert(!patch_picker_pi() && !g_hooked_target);
    g_code_matches = 1;
    assert(patch_picker_pi() && g_hooked_target == g_fake_img + PPI_FN_RVA && g_ppi_orig == fake_game_orig && strstr(g_log, "hook: pickerpi:"));

    /* a clean car through the real database calls: untouched, every row set released */
    reset(); clean_parts(parts); set_car(4242, parts, 5, 0.9f);
    r = ppi_hook(car_obj, out);
    assert(r == out && out[PPI_OUT_CLASS] == 5 && out_pi(out) == (double)0.9f && others_untouched(out) && g_open_rows == 0 && g_lines == 0);

    /* a car with an unknown part: stock class and PI, the car object itself untouched, the stock query's rows released */
    reset(); clean_parts(parts); parts[13] = 31337; set_car(4242, parts, 5, 0.9f);
    { unsigned char car_before[sizeof car_obj]; memcpy(car_before, car_obj, sizeof car_obj);
      r = ppi_hook(car_obj, out);
      assert(r == out && out[PPI_OUT_CLASS] == 1 && out_pi(out) == (double)0.25f && others_untouched(out) && g_open_rows == 0);
      assert(!memcmp(car_before, car_obj, sizeof car_obj) && strstr(g_log, "slot 13 holds part id 31337")); }

    /* a car whose PI came out as NaN */
    reset(); clean_parts(parts); set_car(4242, parts, 5, NAN);
    r = ppi_hook(car_obj, out);
    assert(out[PPI_OUT_CLASS] == 1 && out_pi(out) == (double)0.25f && g_open_rows == 0);

    /* not a garage car, or the database object is missing: the game's attributes stay */
    reset(); clean_parts(parts); parts[13] = 31337; set_car(4242, parts, 5, 0.9f);
    memset(car_obj, 0x11, 8);
    r = ppi_hook(car_obj, out);
    assert(r == out && out[PPI_OUT_CLASS] == 5 && strstr(g_log, "not a garage car"));
    reset(); set_car(4242, parts, 5, 0.9f);
    { void *none = NULL; unsigned char saved[8]; memcpy(saved, holder_obj + PPI_HOLDER_DB_OFFSET, 8); memcpy(holder_obj + PPI_HOLDER_DB_OFFSET, &none, 8);
      r = ppi_hook(car_obj, out);
      assert(out[PPI_OUT_CLASS] == 5 && strstr(g_log, "game database object"));
      memcpy(holder_obj + PPI_HOLDER_DB_OFFSET, saved, 8); }
    /* a database whose method table points at data instead of code: declined, not called */
    reset(); clean_parts(parts); parts[13] = 31337; set_car(4242, parts, 5, 0.9f);
    { void *saved = g_vt_db[PPI_DB_QUERY_SLOT / 8]; static unsigned char data_page[64]; g_vt_db[PPI_DB_QUERY_SLOT / 8] = data_page;
      r = ppi_hook(car_obj, out);
      assert(out[PPI_OUT_CLASS] == 5 && strstr(g_log, "could not be queried"));
      g_vt_db[PPI_DB_QUERY_SLOT / 8] = saved; }
    /* a null car or attribute block goes straight to the game's function */
    reset(); g_hook_orig_calls = 0;
    assert(ppi_hook(NULL, out) == out && g_hook_orig_calls == 1);
    free(g_fake_img);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    test_decision();
    test_hook();
    puts("picker_pi: ok");
    return 0;
}
