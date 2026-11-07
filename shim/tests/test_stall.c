/* Stall tracer (src/stall_trace.inc) against a fake game image: the installer and its refusals, RTTI class names, the state flow path and its
 * timeline, the event begin / end, the 10 s / 5 s snapshots with consoles, players, the session flags and the rule of NetworkWaitForCarsLoaded,
 * and that garbage pointers only make reads fail. The object layouts (+0x28/+0x30 path, +0x88 behaviors, console +0xa0, CPlayer +0x320/+0x340...)
 * come from the decompile and are not verified here against a running game. Built twice: plain, and with -DTEST_CARSEL for the car select hold line. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FH2MP_DIAG_BUILD 1
#define STL_TEST_NO_WATCHER 1
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u

#define MAX_LINES 400
static char g_lines[MAX_LINES][1024];
static int g_nlines;
static char g_raw[2048];
static unsigned char *g_img;
static int g_hooks;
static unsigned g_hook_rva[8];
static volatile DWORD g_grid_tick0;
static volatile LONG g_grid_ready_value = -1, g_grid_epoch = 1;
static volatile LONG g_place_logged;

static int g_echo;
static void grid_line(const char *text)
{
    if (g_echo) puts(text);
    if (g_nlines < MAX_LINES) strncpy(g_lines[g_nlines], text, sizeof g_lines[0] - 1);
    g_nlines++;
}
static void grid_utc(char *out) { strcpy(out, "12:00:00.000 UTC"); }
static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
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

static int g_step_calls, g_exec_calls;
static unsigned char *g_console_local;
static ULONG_PTR fake_step(void *inst, void *a2, void *a3, void *a4) { (void)inst; (void)a2; (void)a3; (void)a4; g_step_calls++; return 7; }
static void fake_exec(void *node) { (void)node; g_exec_calls++; g_console_local[0xa0 + 0x20 * 5] = 1; }

static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    unsigned rva = (unsigned)((unsigned char *)target - g_img);
    (void)name; (void)detour;
    g_hook_rva[g_hooks++] = rva;
    *original = rva == 0x19281e0u ? (void *)fake_step : (void *)fake_exec;
    return 1;
}

#include "src/game_image.inc"

#ifdef TEST_CARSEL
#define CS_UPDATE_RVA 0x8e0990u
#define CARSEL_NOW() GetTickCount64()
static SRWLOCK g_cs_lock = SRWLOCK_INIT;
static struct { int valid; unsigned char hold; ULONGLONG at; unsigned remaining; } g_cs;
static volatile LONG g_cs_enabled;
static volatile LONG64 g_cs_armed_at;
#endif

#include "src/stall_trace.inc"

static int has(const char *line, const char *part) { return strstr(line, part) != NULL; }
static int find_line(int from, const char *part)
{
    int i;
    for (i = from; i < g_nlines && i < MAX_LINES; i++) if (has(g_lines[i], part)) return i;
    return -1;
}

/* ---- the fake game ---------------------------------------------------------------------------------------------------------------- */
static unsigned char *g_heap;
static size_t g_heap_used;
static void *halloc(size_t n)
{
    void *p = g_heap + g_heap_used;
    g_heap_used += (n + 15) & ~(size_t)15;
    return p;
}
#define P8(base, off) (*(ULONG_PTR *)((unsigned char *)(base) + (off)))
#define P4(base, off) (*(unsigned *)((unsigned char *)(base) + (off)))

/* a vtable with MSVC RTTI for `name` ("NetworkWaitForCarsLoaded@States@@") at image RVA vt_rva (vtable[-1] = locator, +12 = descriptor RVA) */
static ULONG_PTR make_class(unsigned vt_rva, unsigned col_rva, unsigned td_rva, const char *mangled)
{
    unsigned char *col = g_img + col_rva;
    P8(g_img, vt_rva - 8) = (ULONG_PTR)col;
    P4(col, 0) = 1;
    P4(col, 12) = td_rva;
    strcpy((char *)g_img + td_rva + 16, mangled);
    return (ULONG_PTR)g_img + vt_rva;
}

static void *make_obj(ULONG_PTR vt)
{
    unsigned char *o = halloc(0x40);
    P8(o, 0) = vt;
    return o;
}

/* an element of the active path with one state definition holding the given behaviors */
static unsigned char *make_element(void *const *behaviors, int n)
{
    unsigned char *e = halloc(0x40), *def = halloc(0x100), *vec = halloc(8 * 16);
    int i;
    for (i = 0; i < n; i++) P8(vec, 8 * i) = (ULONG_PTR)behaviors[i];
    P8(def, 0x88) = (ULONG_PTR)vec;
    P8(def, 0x90) = (ULONG_PTR)vec + 8 * (ULONG_PTR)n;
    P8(e, 8) = (ULONG_PTR)def;
    return e;
}

static void make_console_vars(unsigned char *c, unsigned id, const unsigned char *v)
{
    int i;
    P4(c, 0x20) = 1;
    P4(c, 0x24) = id;
    for (i = 0; i < 9; i++) c[0xa0 + 0x20 * i] = v[i];
}

static unsigned char *make_player(unsigned id, int local, unsigned long long driver, unsigned char c108, unsigned char c128)
{
    unsigned char *p = halloc(0x400), *ch = halloc(0x200);
    P4(p, 0x2c) = id;
    P4(p, 0x40) = local ? 0xffffffffu : 7u;
    *(unsigned long long *)(p + 0x320) = driver;
    P8(p, 0x340) = (ULONG_PTR)ch;
    ch[0x108] = c108;
    ch[0x128] = c128;
    return p;
}

int main(int argc, char **argv)
{
    static const unsigned char step_p[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41};
    static const unsigned char exec_p[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8};
    static const unsigned char lv0[9] = {0, 1, 0, 0, 1, 0, 0, 1, 1};
    unsigned char vars[9];
    unsigned char *cm, *map_head, *n1, *n2, *rem1, *rem2, *pm, *sent, *ln1, *ln2, *ln3, *p1, *p2, *p3, *mgr, *sess, *inst, *e1, *e2, *e3, *e4;
    ULONG_PTR vt_carsloaded, vt_other, vt_sess;
    void *b1[2], *b2[1], *b3[1];
    char cn[48];
    int i, a, b;
    DWORD now;

    g_echo = argc > 1 && !strcmp(argv[1], "--lines");   /* print every line the tracer writes */
    g_img = VirtualAlloc(NULL, FH2_SIZE_OF_IMAGE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_heap = VirtualAlloc(NULL, 0x100000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_img && g_heap);

    /* other bytes: nothing is hooked, both refusals say which function */
    assert(patch_stall_trace() == 1);                       /* the watcher alone still counts as installed */
    assert(g_hooks == 0 && has(g_raw, "mismatch: state machine step") && has(g_raw, "mismatch: NetworkSetHasLoadedCarsForAllParticipants"));
    a = find_line(0, "stall trace on");
    assert(a >= 0 && has(g_lines[a], "state machine step game+0x19281e0 NOT installed") && has(g_lines[a], "HasLoaded node game+0x8d1be0 NOT installed"));
    assert(has(g_lines[a], "diag: 12:00:00.000 UTC stall: "));

    memcpy(g_img + 0x19281e0, step_p, sizeof step_p);
    memcpy(g_img + 0x8d1be0, exec_p, sizeof exec_p);
    g_nlines = 0;
    assert(patch_stall_trace() == 1 && g_hooks == 2 && g_hook_rva[0] == 0x19281e0u && g_hook_rva[1] == 0x8d1be0u);
    a = find_line(0, "stall trace on");
    assert(a >= 0 && has(g_lines[a], "step game+0x19281e0 installed") && has(g_lines[a], "node game+0x8d1be0 installed"));
    assert(g_stl_step_hooked == 1);

    /* class names from the RTTI */
    vt_carsloaded = make_class(0x2d140a0, 0x24eca08, 0x3275718, ".?AVNetworkWaitForCarsLoaded@States@@");
    vt_other = make_class(0x2f48d80, 0x26e9400, 0x2f9b6a0, ".?AVConsoleSynced@Nerd@@");
    assert(stl_name_of_vtable(vt_carsloaded, cn, sizeof cn) && !strcmp(cn, "NetworkWaitForCarsLoaded"));
    assert(stl_name_of_vtable(vt_other, cn, sizeof cn) && !strcmp(cn, "Nerd::ConsoleSynced"));
    assert(!stl_name_of_vtable(0x1234, cn, sizeof cn) && !stl_name_of_vtable((ULONG_PTR)g_img + 0x500000, cn, sizeof cn));
    {
        ULONG_PTR vt_load = make_class(0x2d11b98, 0x24e8a48, 0x3271210, ".?AVNetworkSetInitialCarLoading@States@@");
        ULONG_PTR vt_ui = make_class(0x2d12000, 0x24e9000, 0x3272000, ".?AVShowUIScreen@States@@");
        b1[0] = make_obj(vt_load);
        b1[1] = make_obj(vt_ui);
    }
    b2[0] = make_obj(vt_carsloaded);
    b3[0] = make_obj(vt_other);

    /* consoles: local id 0x10 (P=0), remote 0x21 (P=1, L=0), remote 0x22 (P=1, L=1); a std::map of two nodes */
    cm = halloc(0x40);
    g_console_local = halloc(0x300);
    make_console_vars(g_console_local, 0x10, lv0);
    P8(cm, 0x18) = (ULONG_PTR)g_console_local;
    map_head = halloc(0x40);
    n1 = halloc(0x40);
    n2 = halloc(0x40);
    rem1 = halloc(0x300);
    rem2 = halloc(0x300);
    memcpy(vars, lv0, 9);
    vars[2] = 1;
    make_console_vars(rem1, 0x21, vars);
    vars[5] = 1;
    make_console_vars(rem2, 0x22, vars);
    map_head[0x19] = 1;
    P8(map_head, 0) = (ULONG_PTR)n2; P8(map_head, 8) = (ULONG_PTR)n1; P8(map_head, 0x10) = (ULONG_PTR)n1;
    P8(n1, 0) = (ULONG_PTR)n2; P8(n1, 8) = (ULONG_PTR)map_head; P8(n1, 0x10) = (ULONG_PTR)map_head; P8(n1, 0x28) = (ULONG_PTR)rem1;
    P8(n2, 0) = (ULONG_PTR)map_head; P8(n2, 8) = (ULONG_PTR)n1; P8(n2, 0x10) = (ULONG_PTR)map_head; P8(n2, 0x28) = (ULONG_PTR)rem2;
    P8(cm, 0x20) = (ULONG_PTR)map_head;
    P8(g_img, 0x34af7a0) = (ULONG_PTR)cm;

    /* players: local, remote 0x21 (no driver -> counted), remote 0x22 (loaded) */
    pm = halloc(0x80);
    sent = halloc(0x20);
    ln1 = halloc(0x20); ln2 = halloc(0x20); ln3 = halloc(0x20);
    p1 = make_player(0x10, 1, 0x1111, 0, 0);
    p2 = make_player(0x21, 0, 0, 0, 0);
    p3 = make_player(0x22, 0, 0x2222, 0, 0);
    P8(sent, 0) = (ULONG_PTR)ln1;
    P8(ln1, 0) = (ULONG_PTR)ln2; P8(ln1, 0x10) = (ULONG_PTR)p1;
    P8(ln2, 0) = (ULONG_PTR)ln3; P8(ln2, 0x10) = (ULONG_PTR)p2;
    P8(ln3, 0) = (ULONG_PTR)sent; P8(ln3, 0x10) = (ULONG_PTR)p3;
    P8(pm, 0x48) = (ULONG_PTR)sent;
    P8(g_img, 0x34b3e18) = (ULONG_PTR)pm;

    /* the session object: three plain getters behind its vtable (0xf0 = byte +0x30, 0xe0 = byte +0x31 through a jmp, 0x100 = not a getter) */
    mgr = halloc(0x100);
    sess = halloc(0x100);
    vt_sess = make_class(0x2e00000, 0x2500000, 0x3280000, ".?AVNetSession@Net@@");
    P8(mgr, 0xd0) = (ULONG_PTR)sess;
    P8(sess, 0) = vt_sess;
    sess[0x30] = 1;
    sess[0x31] = 1;
    {
        static const unsigned char getter_f0[] = {0x0f, 0xb6, 0x41, 0x30, 0xc3};
        static const unsigned char getter_e0[] = {0x8a, 0x81, 0x31, 0x00, 0x00, 0x00, 0xc3};
        unsigned char *f0 = g_img + 0x600000, *e0 = g_img + 0x600100, *thunk = g_img + 0x600200, *nope = g_img + 0x600300;
        int rel = (int)((unsigned char *)e0 - (thunk + 5));
        memcpy(f0, getter_f0, sizeof getter_f0);
        memcpy(e0, getter_e0, sizeof getter_e0);
        thunk[0] = 0xe9;
        memcpy(thunk + 1, &rel, 4);
        nope[0] = 0x48; nope[1] = 0x89; nope[2] = 0xc8; nope[3] = 0xc3;
        P8(g_img, 0x2e00000 + 0xf0) = (ULONG_PTR)f0;
        P8(g_img, 0x2e00000 + 0xe0) = (ULONG_PTR)thunk;
        P8(g_img, 0x2e00000 + 0x100) = (ULONG_PTR)nope;
    }
    P8(g_img, 0x34d6348) = (ULONG_PTR)mgr;

    /* the state flow: instance with a two element path (+0x28 head, +0x30 leaf, element +8 definition, +0x18 next) */
    inst = halloc(0x80);
    e1 = make_element(b3, 1);
    e2 = make_element(b1, 2);
    P8(e1, 0x18) = (ULONG_PTR)e2;
    P8(inst, 0x28) = (ULONG_PTR)e1;
    P8(inst, 0x30) = (ULONG_PTR)e2;

    {
        stl_path path;
        assert(stl_get_path((ULONG_PTR)inst, &path) && path.n == 2);
        assert(!strcmp(path.el[0], "Nerd::ConsoleSynced") && !strcmp(path.el[1], "NetworkSetInitialCarLoading+ShowUIScreen"));
    }

    /* outside an event the path changes only go to the ring, no log line */
    g_nlines = 0;
    assert(stl_step_hook(inst, NULL, NULL, NULL) == 7 && g_step_calls == 1);
    assert(g_nlines == 0 && g_stl_ring_n == 1 && g_stl_ninst == 1 && g_stl_steps == 1);
    stl_step_hook(inst, NULL, NULL, NULL);              /* unchanged path: nothing new */
    assert(g_stl_ring_n == 1 && g_nlines == 0);

    /* the first tick only learns the vars; PIR 0 -> 1 begins the event, with the history */
    stl_tick();
    assert(g_nlines == 0 && g_stl_vars_valid == 1);
    g_console_local[0xa0 + 0x20 * 2] = 1;
    stl_tick();
    a = find_line(0, "event begin: local Console.ParticipatingInRace went 0 -> 1");
    assert(a >= 0 && has(g_lines[a], "local console 0x10 ") && has(g_lines[a], "ParticipatingInRace=1") && has(g_lines[a], "2 remote console(s)"));
    b = find_line(a, "history, before the event: ");
    assert(b > a && has(g_lines[b], "sm1 Nerd::ConsoleSynced > NetworkSetInitialCarLoading+ShowUIScreen"));
    assert(g_stl_ev.active);

    /* inside the event: a path change is a timeline line; the marker class is noticed */
    e3 = make_element(b2, 1);
    P8(e2, 0x18) = (ULONG_PTR)e3;
    P8(inst, 0x30) = (ULONG_PTR)e3;
    g_nlines = 0;
    stl_step_hook(inst, NULL, NULL, NULL);
    a = find_line(0, "state flow sm1 ");
    assert(a >= 0 && has(g_lines[a], "stall: +") && has(g_lines[a], "NetworkWaitForCarsLoaded"));
    assert(g_stl_marker_set == 1);

    /* a local console var change while traced */
    g_nlines = 0;
    g_console_local[0xa0 + 0x20 * 4] = 0;
    stl_tick();
    a = find_line(0, "local Console.Ready 1 -> 0");
    assert(a >= 0);

    /* the HasLoaded node: before -> after */
    g_nlines = 0;
    stl_set_loaded_hook(NULL);
    a = find_line(0, "node NetworkSetHasLoadedCarsForAllParticipants ran: local Console.HasLoadedCarsForAllParticipants 0 -> 1");
    assert(g_exec_calls == 1 && a >= 0 && has(g_lines[a], "stall: +"));
    g_console_local[0xa0 + 0x20 * 5] = 0;
    stl_tick();

    /* no snapshot before 10 s */
    g_nlines = 0;
    stl_tick();
    assert(find_line(0, "snapshot #") < 0);

    /* the snapshot: consoles, players, the session flags, the verdict */
    g_stl_ev.next_snap = GetTickCount() - 1;
    g_stl_last_step = GetTickCount() - 20;
    g_place_logged = 3;
    g_nlines = 0;
    stl_tick();
    assert(find_line(0, "snapshot #1 +") >= 0 && has(g_lines[find_line(0, "snapshot #1 +")], "no starting grid message yet; state flow last stepped"));
    a = find_line(0, "snapshot #1 state flow ");
    assert(a >= 0 && has(g_lines[a], "NetworkWaitForCarsLoaded") && has(g_lines[a], "Nerd::ConsoleSynced"));
    assert(find_line(0, "snapshot #1 local console 0x10 RenderReady=0 ") >= 0);
    a = find_line(0, "snapshot #1 remote consoles 1-");
    assert(a >= 0 && has(g_lines[a], "0x21: R0 S1 P1 I0 Y1 L0 G0 A1 M1") && has(g_lines[a], "0x22: R0 S1 P1 I0 Y1 L1 G0 A1 M1"));
    a = find_line(0, "snapshot #1 players 1-");
    assert(a >= 0 && has(g_lines[a], "console 0x10 local driver=0x1111") && has(g_lines[a], "console 0x21 remote driver=0x0 choices+108=0 +128=0 participating STILL LOADING") &&
           has(g_lines[a], "console 0x22 remote driver=0x2222") && !has(strstr(g_lines[a], "console 0x22"), "STILL LOADING"));
    a = find_line(0, "snapshot #1 session object ");
    assert(a >= 0 && has(g_lines[a], "(Net::NetSession)") && has(g_lines[a], "flag0xf0=1 flag0xe0=1 flag0x100=(not a plain getter, game+0x600300)"));
    a = find_line(0, "snapshot #1 NetworkWaitForCarsLoaded by the game's rule [inferred]: 1 participant(s) still loading");
    assert(a >= 0 && has(g_lines[a], "the session flags were not all readable"));
    assert(has(g_lines[find_line(0, "snapshot #1 +")], "texture placement fallbacks so far 3 (3 since"));
#ifdef TEST_CARSEL
    {
        a = find_line(0, "snapshot #1 car select hold (FH2MP): not enabled, the hooked NetworkWaitForCarsLoaded update has not run; no server answer read");
        assert(a >= 0);
    }
#endif
    assert(g_stl_ev.snaps == 1);

    /* 5 s apart, at most 12 */
    g_nlines = 0;
    stl_tick();
    assert(find_line(0, "snapshot #") < 0);
    for (i = 2; i <= 12; i++) {
        g_stl_ev.next_snap = GetTickCount() - 1;
        stl_tick();
    }
    assert(g_stl_ev.snaps == 12 && find_line(0, "snapshot #12 +") >= 0);
    g_stl_ev.next_snap = GetTickCount() - 1;
    stl_tick();
    stl_tick();
    g_stl_ev.next_snap = GetTickCount() - 1;
    stl_tick();
    assert(g_stl_ev.snaps == 12 && find_line(0, "snapshot #13") < 0);
    a = find_line(0, "snapshot limit of 12 reached");
    assert(a >= 0 && find_line(a + 1, "snapshot limit of 12 reached") < 0);

    /* a blocked game thread */
    g_nlines = 0;
    g_stl_last_step = GetTickCount() - 4000;
    stl_tick();
    a = find_line(0, "the state flow has not been stepped for 4");
    assert(a >= 0);
    stl_tick();
    assert(find_line(a + 1, "has not been stepped") < 0);
    g_stl_last_step = GetTickCount();
    stl_tick();
    assert(find_line(0, "the state flow is being stepped again") >= 0);

    /* the grid message starts the grid phase, the trace goes on until RenderReady */
    g_nlines = 0;
    g_grid_tick0 = 12345;
    stl_tick();
    a = find_line(0, "the starting grid message arrived +");
    assert(a >= 0 && has(g_lines[a], "tracing the grid phase") && g_stl_ev.active && g_stl_ev.grid_phase && g_stl_ev.snaps == 0);
    assert(find_line(0, "event load finished") < 0);
    /* its first snapshot comes STL_GRID_QUIET_MS after the grid message and says it is the grid phase */
    g_nlines = 0;
    stl_tick();
    assert(g_nlines == 0 || find_line(0, "snapshot #1") < 0);
    g_stl_ev.next_snap = GetTickCount() - 1;
    stl_tick();
    a = find_line(0, "snapshot #1 +");
    assert(a >= 0 && has(g_lines[a], "ms after the starting grid message, local RenderReady not set yet; state flow"));
    /* RenderReady ends the grid phase */
    g_nlines = 0;
    g_grid_ready_value = 1;
    stl_tick();
    a = find_line(0, "event load finished: local RenderReady is set");
    assert(a >= 0 && has(g_lines[a], "1 snapshot(s) written") && !g_stl_ev.active);
    g_grid_ready_value = -1;
    /* after the end a path change is not logged, and the marker alone does not begin an event within the cool-down */
    g_nlines = 0;
    e4 = make_element(b3, 1);
    P8(e3, 0x18) = (ULONG_PTR)e4;
    P8(inst, 0x30) = (ULONG_PTR)e4;
    stl_step_hook(inst, NULL, NULL, NULL);
    InterlockedExchange(&g_stl_marker_set, 1);
    stl_tick();
    assert(g_nlines == 0 && !g_stl_ev.active);

    /* leaving the event ends a new one; a marker begins one after the cool-down */
    g_stl_ev.ended_at = GetTickCount() - STL_COOLDOWN_MS - 10;
    InterlockedExchange(&g_stl_marker_set, 1);
    lstrcpyA(g_stl_marker, "NetworkWaitForCarsLoaded");
    stl_tick();
    a = find_line(0, "event begin: the state flow entered NetworkWaitForCarsLoaded");
    assert(a >= 0 && g_stl_ev.active);
    g_nlines = 0;
    g_console_local[0xa0 + 0x20 * 2] = 0;
    stl_tick();
    assert(find_line(0, "event trace ended: local Console.ParticipatingInRace went 1 -> 0") >= 0 && !g_stl_ev.active);

    /* RaceInProgress also ends it */
    g_console_local[0xa0 + 0x20 * 2] = 1;
    stl_tick();
    assert(g_stl_ev.active);
    g_nlines = 0;
    g_console_local[0xa0 + 0x20 * 3] = 1;
    stl_tick();
    assert(find_line(0, "event load finished: local Console.RaceInProgress went 0 -> 1") >= 0 && !g_stl_ev.active);

    /* a long timeline stops at the limit and says how many were left out */
    g_console_local[0xa0 + 0x20 * 3] = 0;
    g_console_local[0xa0 + 0x20 * 2] = 0;
    stl_tick();
    g_console_local[0xa0 + 0x20 * 2] = 1;
    stl_tick();
    assert(g_stl_ev.active);
    g_nlines = 0;
    for (i = 0; i < 100; i++) {
        g_console_local[0xa0 + 0x20 * 6] ^= 1;
        stl_tick();
    }
    assert(g_nlines == STL_TIMELINE_MAX);
    g_console_local[0xa0 + 0x20 * 2] = 0;
    g_nlines = 0;
    stl_tick();
    a = find_line(0, "11 timeline line(s) over the limit of 90 were not written");
    assert(a >= 0);

    /* garbage everywhere: every read fails, nothing crashes, the snapshot says what did not validate */
    P8(g_img, 0x34af7a0) = 0xdeadbeef0000ull;
    P8(g_img, 0x34b3e18) = 0x10;
    P8(g_img, 0x34d6348) = 0xdeadbeef0000ull;
    P8(inst, 0x28) = 0xdeadbeef0000ull;
    g_nlines = 0;
    stl_observe(inst);
    {
        stl_path path;
        assert(!stl_get_path((ULONG_PTR)inst, &path));
        assert(!stl_get_path(0xdeadbeef0000ull, &path) && !stl_get_path(0, &path));
    }
    now = GetTickCount();
    stl_begin(now, "test", NULL);
    g_nlines = 0;
    stl_snapshot(1, 10000);
    assert(find_line(0, "local console not readable") >= 0 && find_line(0, "remote consoles: none known") >= 0 &&
           find_line(0, "players: the player manager or its list did not validate") >= 0 && find_line(0, "session object not readable") >= 0 &&
           find_line(0, "not decidable from what was readable") >= 0);
    stl_tick();     /* no console: nothing happens */

    /* a cyclic player list and a cyclic map are bounded */
    P8(g_img, 0x34b3e18) = (ULONG_PTR)pm;
    P8(ln3, 0) = (ULONG_PTR)ln1;
    {
        stl_player pl[STL_PLAYER_MAX];
        assert(stl_players(pl, STL_PLAYER_MAX, NULL, NULL, 0) == STL_PLAYER_MAX);
    }
    P8(g_img, 0x34af7a0) = (ULONG_PTR)cm;
    P8(n2, 0) = (ULONG_PTR)n1;
    P8(n2, 0x10) = (ULONG_PTR)n1;
    {
        stl_console rc[STL_CONSOLE_MAX];
        assert(stl_remote_consoles(rc, STL_CONSOLE_MAX) <= STL_CONSOLE_MAX);
    }

    printf("stall trace: ok\n");
    return 0;
}
