/* Isolated tests of src/race_wait.inc: the game's players, consoles, session flags and car cache are faked in this process; no game is loaded. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FH2_SIZE_OF_IMAGE 0x3a18000u
#define CARSEL_REPORT_TEXT 200u
static unsigned char *g_img;
static int read_game_memory(const void *p, void *out, SIZE_T n) { if (!p) return 0; memcpy(out, p, n); return 1; }
static unsigned char *find_fh2_image(void) { return g_img; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n) {(void)tag; (void)what; return !memcmp(img + rva, expect, n);}
static char g_log[8192];
static void log_begin(void) {strcat(g_log, "|");}
static void log_raw(const char *s) {strcat(g_log, s);}
static void log_dec(unsigned long value) {char b[24]; sprintf(b, "%lu", value); strcat(g_log, b);}
static unsigned g_cache[4];
static int g_cache_ok;
static int car_wait_group(unsigned *out) { if (!g_cache_ok) return 0; memcpy(out, g_cache, sizeof g_cache); return 1; }
static int g_reports;
static unsigned char g_report_code;
static unsigned g_report_detail;
static char g_report_text[256];
static int carsel_report_post(unsigned char code, unsigned detail, const char *text)
{
    g_reports++; g_report_code = code; g_report_detail = detail; strcpy(g_report_text, text);
    return 1;
}
static int (*g_race_wait_step)(void *self, ULONGLONG now, ULONGLONG wait_first, int holding);
#include "src/race_wait.inc"

/* ---- the faked game ---- */
static unsigned char g_cm[0x40], g_pm[0x60], g_mgr[0x100], g_sess[0x100];
static unsigned char g_head[0x40], g_cnode[4][0x40], g_console[4][0x200];
static unsigned char g_sentinel[0x20], g_pnode[5][0x20], g_player[5][0x400], g_choices[5][0x200];
static unsigned char g_self[0x40];
static int g_loaded_calls;
static void *g_loaded_args[2];
static void fake_loaded(void *self, void *arg) {g_loaded_calls++; g_loaded_args[0] = self; g_loaded_args[1] = arg;}

static void put_ptr(unsigned char *at, const void *p) {ULONG_PTR v = (ULONG_PTR)p; memcpy(at, &v, sizeof v);}
static void put_u32(unsigned char *at, unsigned v) {memcpy(at, &v, 4);}

static void console(int i, unsigned id, int participating)
{
    memset(g_console[i], 0, sizeof g_console[i]);
    put_u32(g_console[i] + 0x24, id);
    g_console[i][0xa0 + 0x20 * 2] = (unsigned char)participating;
}
static void player(int i, unsigned id, int local, int has_car, unsigned char c108)
{
    memset(g_player[i], 0, sizeof g_player[i]);
    memset(g_choices[i], 0, sizeof g_choices[i]);
    put_u32(g_player[i] + 0x2c, id);
    put_u32(g_player[i] + 0x40, local ? 0xffffffffu : 7);
    if (has_car) put_ptr(g_player[i] + 0x320, g_choices[i] + 0x100);
    put_ptr(g_player[i] + 0x340, g_choices[i]);
    g_choices[i][0x108] = c108;
}

/* consoles 8 (root), 3 (left), 11 (right), 6 (right of 11, not participating); players: local 2, 3 and 8 with cars, 11 without, 6 without (not racing). */
static void game_setup(void)
{
    int i;
    g_img = VirtualAlloc(NULL, FH2_SIZE_OF_IMAGE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(g_img);
    put_ptr(g_img + RW_RVA_CONSOLE_MGR, g_cm);
    put_ptr(g_cm + 0x20, g_head);
    g_head[0x19] = 1;
    put_ptr(g_head + 8, g_cnode[0]);
    console(0, 8, 1); console(1, 3, 1); console(2, 11, 1); console(3, 6, 0);
    for (i = 0; i < 4; i++) {
        put_ptr(g_cnode[i], g_head);
        put_ptr(g_cnode[i] + 0x10, g_head);
        put_ptr(g_cnode[i] + 0x28, g_console[i]);
    }
    put_ptr(g_cnode[0], g_cnode[1]);
    put_ptr(g_cnode[0] + 0x10, g_cnode[2]);
    put_ptr(g_cnode[2] + 0x10, g_cnode[3]);

    put_ptr(g_img + RW_RVA_PLAYER_MGR, g_pm);
    put_ptr(g_pm + 0x48, g_sentinel);
    put_ptr(g_sentinel, g_pnode[0]);
    for (i = 0; i < 5; i++) {
        put_ptr(g_pnode[i], i < 4 ? (void *)g_pnode[i + 1] : (void *)g_sentinel);
        put_ptr(g_pnode[i] + 0x10, g_player[i]);
    }
    player(0, 2, 1, 1, 0); player(1, 3, 0, 1, 0); player(2, 8, 0, 1, 0); player(3, 11, 0, 0, 0); player(4, 6, 0, 0, 0);

    /* session: vtable and three getters inside the image: movzx eax, byte [rcx+0x10/0x11/0x12]; ret (the 0x100 one behind a jmp) */
    put_ptr(g_img + RW_RVA_NET_MGR, g_mgr);
    put_ptr(g_mgr + 0xd0, g_sess);
    put_ptr(g_sess, g_img + 0x100000);
    put_ptr(g_img + 0x100000 + 0xf0, g_img + 0x200000);
    put_ptr(g_img + 0x100000 + 0xe0, g_img + 0x200010);
    put_ptr(g_img + 0x100000 + 0x100, g_img + 0x200020);
    memcpy(g_img + 0x200000, "\x0f\xb6\x41\x10\xc3", 5);
    memcpy(g_img + 0x200010, "\x0f\xb6\x41\x11\xc3", 5);
    {
        int rel = 0x200030 - (0x200020 + 5);
        g_img[0x200020] = 0xe9;
        memcpy(g_img + 0x200021, &rel, 4);
        memcpy(g_img + 0x200030, "\x8a\x41\x12\xc3", 4);
    }
    g_sess[0x10] = 1; g_sess[0x11] = 0; g_sess[0x12] = 0;
    memcpy(g_img + RW_RVA_LOADED, "\x48\x83\x7a\x10\x00\x8b\x0d\x6d\x99\xc2\x01\x65\x48\x8b\x04\x25\x58\x00\x00\x00", 20);
    g_cache_ok = 1;
    g_cache[0] = 0; g_cache[1] = 1; g_cache[2] = 0; g_cache[3] = 11;
}

static const char *WANT = "still loading 1 of 3 racers [11:d]; racers 8,3,11; cache free 0 wait 1 load 0 done 11; session f0=1 e0=0 100=0";

static void test_scan_and_text(void)
{
    rw_scan s;
    rw_text t;
    char want[256];
    rw_scan_now(&s);
    assert(s.players == 5 && s.counted == 1 && s.stuck[0].id == 11 && s.stuck[0].why == 1);
    assert(s.nracers == 3 && s.f0 == 1 && s.e0 == 0 && s.f100 == 0);
    rw_describe(&s, 20, 0, &t);
    sprintf(want, "race wait 20 s: %s", WANT);
    if (strcmp(t.s, want)) {printf("got  %s\nwant %s\n", t.s, want); assert(0);}
    rw_describe(&s, 25, 1, &t);
    assert(strstr(t.s, "race wait 25 s (FH2MP car select hold on): still loading 1"));
    assert(rw_pass_ok(&s, RW_PASS_STABLE_MS));
    assert(!rw_pass_ok(&s, RW_PASS_STABLE_MS - 1));
    g_cache[0] = 1;                                      /* a free slot: the car is not starved of one */
    rw_scan_now(&s); assert(!rw_pass_ok(&s, 99999)); g_cache[0] = 0;
    g_choices[3][0x108] = 1;                             /* a car choice pending as well: not only a slot */
    rw_scan_now(&s); assert(s.stuck[0].why == 3 && !rw_pass_ok(&s, 99999)); g_choices[3][0x108] = 0;
    g_sess[0x10] = 0;                                    /* the session branch, not the players: never */
    rw_scan_now(&s); assert(!rw_pass_ok(&s, 99999)); g_sess[0x10] = 1;
    g_console[2][0xa0 + 0x40] = 0;                       /* 11 not racing: nobody counted */
    rw_scan_now(&s); assert(s.counted == 0 && s.nracers == 2 && !rw_pass_ok(&s, 99999)); g_console[2][0xa0 + 0x40] = 1;
    g_console[1][0xa0] = 7;                              /* a block that is not a console is skipped */
    rw_scan_now(&s); assert(s.nracers == 2); g_console[1][0xa0] = 0;
    put_ptr(g_pm + 0x48, NULL);
    rw_scan_now(&s); rw_describe(&s, 20, 0, &t); assert(strstr(t.s, "player list unreadable")); put_ptr(g_pm + 0x48, g_sentinel);
}

static void test_reports_at_20_and_40_s(void)
{
    ULONGLONG first = 1000000;
    char want[256];
    g_reports = 0;
    assert(race_wait_step(g_self, first + 19999, first, 0) == 0 && g_reports == 0);
    assert(race_wait_step(g_self, first + 20000, first, 0) == 0 && g_reports == 1);
    sprintf(want, "race wait 20 s: %s", WANT);
    assert(g_report_code == 6 && g_report_detail == 20 && !strcmp(g_report_text, want));
    assert(strstr(g_log, "|racewait: race wait 20 s: still loading 1 of 3 racers [11:d]"));
    race_wait_step(g_self, first + 30000, first, 1);
    assert(g_reports == 1);
    race_wait_step(g_self, first + 40500, first, 1);
    assert(g_reports == 2 && g_report_detail == 40 && strstr(g_report_text, "race wait 40 s (FH2MP car select hold on)"));
    race_wait_step(g_self, first + 90000, first, 0);
    assert(g_reports == 2);
    race_wait_step(g_self, first + 2000000 + 20000, first + 2000000, 0);  /* a new wait reports again */
    assert(g_reports == 3);
    assert(g_loaded_calls == 0);                                         /* the way through is off by default */
}

static void test_way_through(void)
{
    ULONGLONG first = 5000000;
    SetEnvironmentVariableW(L"FH2MP_RACEWAIT_PASS", L"1");
    g_log[0] = 0;
    assert(patch_race_wait() && g_rw_pass_on && g_rw_loaded == (void (*)(void *, void *))(g_img + RW_RVA_LOADED));
    assert(strstr(g_log, "let through when only full car slots hold it"));
    g_rw_loaded = fake_loaded;
    assert(race_wait_step(g_self, first + 9000, first, 0) == 0);        /* too early to look */
    assert(race_wait_step(g_self, first + 10000, first, 0) == 0);       /* the cache's state is first seen here */
    assert(race_wait_step(g_self, first + 13000, first, 0) == 0);       /* full for 3 s only */
    assert(race_wait_step(g_self, first + 15000, first, 1) == 0);       /* FH2MP's hold is on: never */
    assert(race_wait_step(g_self, first + 15500, first, 0) == 1);
    assert(g_loaded_calls == 1 && g_loaded_args[0] == g_self && g_loaded_args[1] == g_self + 0x28);
    assert(strstr(g_log, "|racewait: let through after 15 s because only full car slots held it"));
    g_cache[2] = 1;                                                     /* a car loading: wait for it */
    first += 1000000;
    race_wait_step(g_self, first + 9000, first, 0);
    assert(race_wait_step(g_self, first + 20000, first, 0) == 0 && g_loaded_calls == 1);
    g_cache[2] = 0;
    SetEnvironmentVariableW(L"FH2MP_RACEWAIT_PASS", NULL);
    g_rw_pass_on = 0;
    g_rw_loaded = NULL;
    assert(patch_race_wait() && !g_rw_pass_on && !g_rw_loaded);
    g_img[RW_RVA_LOADED] = 0xcc;                                        /* other bytes: the way through stays off even when asked for */
    SetEnvironmentVariableW(L"FH2MP_RACEWAIT_PASS", L"1");
    assert(patch_race_wait() && !g_rw_pass_on && !g_rw_loaded);
    assert(g_race_wait_step == race_wait_step);
}

int main(void)
{
    game_setup();
    assert(patch_race_wait() && !g_rw_pass_on);
    test_scan_and_text();
    test_reports_at_20_and_40_s();
    test_way_through();
    puts("race wait tests passed");
    return 0;
}
