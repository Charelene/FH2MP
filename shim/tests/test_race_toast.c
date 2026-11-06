/* Race toast track name (src/race_toast.inc): the production hook against stand-ins for the game's objects (the lobby options, the database, the
 * string objects and the formatter), which count references. The offsets, vtable slots and prologues come from the decompile; what the toast looks
 * like and whether TrackId is set when the toast is built is not something this can say, nor is it a check against a running game. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static char g_raw[4096];
static unsigned char *g_img;
static int g_call_hooks;

static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void log_dec(unsigned long v) { char b[16]; wsprintfA(b, "%lu", v); log_raw(b); }
static void mem_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static unsigned char *find_fh2_image(void) { return g_img; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag;
    if (!memcmp(img + rva, expect, n)) return 1;
    log_raw("mismatch: ");
    log_raw(what);
    log_raw("\n");
    return 0;
}

/* The game's string objects: vtable slot 2 AddRef (0x10), 3 Release (0x18), 7 text (0x38). */
typedef struct { void **vt; int refs; wchar_t text[96]; } Str;
static int g_live;
static void str_addref(Str *s) { s->refs++; }
static void str_release(Str *s) { if (--s->refs == 0) { g_live--; free(s); } }
static const wchar_t *str_text(Str *s) { return s->text; }
static void *g_str_vt[16] = {0, 0, (void *)str_addref, (void *)str_release, 0, 0, 0, (void *)str_text};
static Str *str_new(const wchar_t *t)
{
    Str *s = calloc(1, sizeof *s);
    s->vt = g_str_vt;
    s->refs = 1;
    wcscpy(s->text, t);
    g_live++;
    return s;
}

/* a result set, a database */
typedef struct { void **vt; int refs; } Rs;
static Str *g_row_name;                 /* what the query's single row holds; NULL: no row */
static char g_last_sql[128];
static char g_last_column[17];
static int g_live_rs;
static void rs_release(Rs *r) { if (--r->refs == 0) { g_live_rs--; free(r); } }
struct stdstr16 { char buf[16]; unsigned long long size, capacity; };
static void **rs_column(Rs *r, void **tmp, struct stdstr16 *col)
{
    static void *slot;
    (void)r;
    memcpy(g_last_column, col->buf, 16);
    g_last_column[16] = 0;
    assert(col->size == strlen(col->buf) && col->capacity == 15);
    *tmp = NULL;
    slot = g_row_name;
    if (g_row_name) { g_row_name->refs++; *tmp = g_row_name; }   /* the column hands out one reference in tmp, like the game's result set */
    return &slot;
}
static void *g_rs_vt[64];
static void db_query(void *db, void **rs, const char *sql)
{
    Rs *r = calloc(1, sizeof *r);
    (void)db;
    r->vt = g_rs_vt;
    r->refs = 1;
    g_live_rs++;
    strncpy(g_last_sql, sql, sizeof g_last_sql - 1);
    *rs = r;
}
static void *g_db_vt[16];
static void *g_db_obj = g_db_vt;
static void **fake_dbslot(void) { static void *slot; slot = &g_db_obj; return &slot; }

/* the game's string maker and formatter */
static void **fake_mkstr(void **out, const char *text, int len)
{
    wchar_t w[96];
    int i;
    (void)len;
    for (i = 0; text[i]; i++) w[i] = (unsigned char)text[i];
    w[i] = 0;
    *out = str_new(w);
    return out;
}
static void **fake_format(void **out, void **fmt, void **arg)
{
    Str *f = *fmt, *a = *arg;
    wchar_t buf[192];
    const wchar_t *p = f->text;
    int n = 0;
    while (*p) {
        if (p[0] == L'{' && p[1] == L'0' && p[2] == L'}') {
            wcscpy(buf + n, a->text);
            n += (int)wcslen(a->text);
            p += 3;
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = 0;
    *out = str_new(buf);
    return out;
}

/* the original string builder: an empty pre-gamertag string, as for IDS_EventInvite_Title_PreGamertag_Race */
static int g_orig_calls;
static char g_orig_type;
static void **fake_pre(void **out, char type, unsigned cls, char flag)
{
    (void)cls; (void)flag;
    g_orig_calls++;
    g_orig_type = type;
    *out = str_new(L"");
    return out;
}

static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    (void)name; (void)detour;
    g_call_hooks++;
    *original = (unsigned char *)site - g_img == 0x819767 ? (void *)fake_pre : NULL;
    return *original != NULL;
}

#include "src/race_toast.inc"

static int has(const char *s, const char *part) { return strstr(s, part) != NULL; }

int main(void)
{
    static const unsigned char pre_fn[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b, 0xd9, 0x0f, 0xb6, 0xca};
    static const unsigned char before[] = {0x45, 0x0f, 0xb6, 0xcc, 0x44, 0x8b, 0xc0, 0x40, 0x0f, 0xb6, 0xd6, 0x48, 0x8d, 0x4c, 0x24, 0x30};
    static const unsigned char mkstr[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48};
    static const unsigned char format[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57};
    static const unsigned char opts_fn[] = {0x48, 0x8b, 0x81, 0xb8, 0x00, 0x00, 0x00, 0xc3};
    static const unsigned char db_fn[] = {0x48, 0x8b, 0x05, 0x39, 0xe3, 0x7b, 0x02, 0x48, 0x83, 0xc0, 0x40, 0xc3};
    unsigned char mgr[0x200], opts[0x200];
    void *mgr_ptr = mgr;
    Str *pre = NULL;
    void **out = (void **)&pre;
    int rel;

    g_img = VirtualAlloc(NULL, 0x3a18000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_img);
    g_rs_vt[3] = (void *)rs_release;
    g_rs_vt[32] = (void *)rs_column;
    g_db_vt[8] = (void *)db_query;

    /* other bytes: nothing is hooked, and the log says which function it was */
    assert(patch_race_toast() == 0 && g_call_hooks == 0 && has(g_raw, "mismatch: the pre-gamertag string builder") && has(g_raw, "not installed"));
    memcpy(g_img + 0x80cbb0, pre_fn, sizeof pre_fn);
    memcpy(g_img + 0x819767 - sizeof before, before, sizeof before);
    memcpy(g_img + 0x323310, mkstr, sizeof mkstr);
    memcpy(g_img + 0x123b660, format, sizeof format);
    memcpy(g_img + 0xa2ae90, opts_fn, sizeof opts_fn);
    memcpy(g_img + 0xced720, db_fn, sizeof db_fn);
    g_img[0x819767] = 0xe8;
    rel = 0x80cbb0 - (0x819767 + 5);
    memcpy(g_img + 0x819767 + 1, &rel, 4);
    g_img[0x81972f] = 0x48; g_img[0x81972f + 1] = 0x8b; g_img[0x81972f + 2] = 0x0d;
    rel = 0x34d6348 - (0x81972f + 7);
    memcpy(g_img + 0x81972f + 3, &rel, 4);

    /* a call that goes elsewhere, a manager load of another global: refused */
    rel = 0x80cbb0 + 0x10 - (0x819767 + 5);
    memcpy(g_img + 0x819767 + 1, &rel, 4);
    g_raw[0] = 0;
    assert(patch_race_toast() == 0 && g_call_hooks == 0 && has(g_raw, "is not a call to game+0x80cbb0"));
    rel = 0x80cbb0 - (0x819767 + 5);
    memcpy(g_img + 0x819767 + 1, &rel, 4);
    g_raw[0] = 0;
    assert(patch_race_toast() == 1 && g_call_hooks == 1 && has(g_raw, "name the event's track at the start of line 2"));
    /* the installer pointed the game's functions into the (empty) image; the stand-ins take their place */
    g_rt_mkstr = fake_mkstr;
    g_rt_format = fake_format;
    g_rt_dbslot = fake_dbslot;
    memcpy(g_img + 0x34d6348, &mgr_ptr, sizeof mgr_ptr);

    /* options with a track: the pre string becomes '<name> | ', in whatever language the name object holds */
    memset(mgr, 0, sizeof mgr);
    memset(opts, 0, sizeof opts);
    { void *o = opts; memcpy(mgr + 0xb8, &o, sizeof o); }
    *(unsigned *)(opts + 0x128) = 573;
    *(unsigned *)(opts + 0x148) = 0;
    g_row_name = str_new(L"Castelletto Meadow Circuit");
    /* the layout was not changed (the platform refused the XAML): the game's own string stays, one log line says why */
    g_raw[0] = 0;
    pre = NULL;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L"") && has(g_raw, "its layout was not changed") && g_live_rs == 0);
    str_release(pre);
    g_rt_layout = 1;
    g_raw[0] = 0;
    pre = NULL;
    race_toast_pre(out, 1, 3, 1);
    assert(g_orig_calls == 2 && pre && !wcscmp(pre->text, L"Castelletto Meadow Circuit | "));
    assert(!strcmp(g_last_sql, "SELECT DisplayName FROM Tracks WHERE Id=573") && !strcmp(g_last_column, "DisplayName"));
    assert(has(g_raw, "racetoast: the race toast names the track at the start of line 2 (TrackId 573, RouteId 0)"));
    assert(pre->refs == 1);                                   /* the one reference the game's caller releases */
    str_release(pre);
    assert(g_row_name->refs == 1 && g_live == 1 && g_live_rs == 0);   /* nothing of ours is left: only the test's own row string */

    /* the same toast in another language */
    wcscpy(g_row_name->text, L"Prato Castelletto");
    pre = NULL;
    race_toast_pre(out, 0, 3, 0);
    assert(!wcscmp(pre->text, L"Prato Castelletto | "));
    str_release(pre);

    /* King and Infected: the game's string is left alone */
    pre = NULL;
    g_raw[0] = 0;
    race_toast_pre(out, 7, 3, 1);
    assert(!wcscmp(pre->text, L"") && g_raw[0] == 0);
    str_release(pre);
    pre = NULL;
    race_toast_pre(out, 6, 3, 1);
    assert(!wcscmp(pre->text, L"") && g_raw[0] == 0);
    str_release(pre);

    /* no track id (0, or past the table): left as is, the line says what the options hold */
    *(unsigned *)(opts + 0x128) = 0;
    *(unsigned *)(opts + 0x148) = 9;
    pre = NULL;
    g_raw[0] = 0;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L"") && has(g_raw, "no usable track id (TrackId 0, RouteId 9)"));
    str_release(pre);
    *(unsigned *)(opts + 0x128) = 0x7fffffff;
    pre = NULL;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L""));
    str_release(pre);
    *(unsigned *)(opts + 0x128) = 573;

    /* no row, an empty name: left as is, no reference leaks */
    { Str *keep = g_row_name; g_row_name = NULL;
      pre = NULL;
      g_raw[0] = 0;
      race_toast_pre(out, 1, 3, 1);
      assert(!wcscmp(pre->text, L"") && has(g_raw, "the Tracks row has no DisplayName"));
      str_release(pre);
      g_row_name = keep; }
    g_row_name->text[0] = 0;
    pre = NULL;
    g_raw[0] = 0;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L"") && has(g_raw, "the track's name is empty"));
    str_release(pre);
    assert(g_row_name->refs == 1 && g_live == 1 && g_live_rs == 0);
    wcscpy(g_row_name->text, L"Castelletto Meadow Circuit");

    /* no event options, no manager */
    { void *none = NULL; memcpy(mgr + 0xb8, &none, sizeof none); }
    pre = NULL;
    g_raw[0] = 0;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L"") && has(g_raw, "the lobby has no event options"));
    str_release(pre);
    { void *none = NULL; memcpy(g_img + 0x34d6348, &none, sizeof none); }
    pre = NULL;
    g_raw[0] = 0;
    race_toast_pre(out, 1, 3, 1);
    assert(!wcscmp(pre->text, L"") && has(g_raw, "there is no network manager"));
    str_release(pre);

    /* the log is bounded */
    { int i; g_raw[0] = 0;
      for (i = 0; i < 100; i++) { pre = NULL; race_toast_pre(out, 1, 3, 1); str_release(pre); }
      { int lines = 0; const char *p = g_raw; while ((p = strstr(p, "racetoast:")) != NULL) { lines++; p++; } printf("log lines after 100 toasts: %d\n", lines); assert(lines == 11); } }   /* the cap is 20 lines for the whole run, the layout case used one */

    puts("race toast: ok");
    return 0;
}
