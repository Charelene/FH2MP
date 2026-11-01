/* FH2 1.0.0.64: the client's side of the starting grid. The server only ever sees the final Console.RenderReady, so when a client took
 * 83 s instead of 25 s (prod 2026-11-01, lobby 42557c65) nothing said which step it was in. These hooks only observe: each calls the native
 * function unchanged and writes one line, the first time per grid, with the milliseconds since the starting-grid message and the UTC time
 * of day (to put next to the server's "grid:" lines).
 *
 *   grid: starting grid message at 19:58:08.412 UTC
 *   grid: PrepareForRaceMode +1200 ms
 *   grid: RenderReady set +83100 ms (19:59:31.512 UTC)
 *   grid: race start message +111700 ms, RenderReady had been set 28600 ms ago
 *
 * Natives (netv2-messages.tsv, docs/exe, stateflow race_flow.xml prerace_prepare): MsgStartingGrid::Receive +1824040, MsgStartRace::Receive
 * +18241e0, ConsoleSynced::SetRenderReady +17deda0, and the Execute (entry) of the pre-race state nodes below.
 */
typedef unsigned (*grid_msg_fn)(void *, void *, void *);
typedef void (*grid_node_fn)(void *);
typedef void (*grid_ready_fn)(void *, unsigned);

enum { GRID_STAGE_RESET, GRID_STAGE_PREPARE, GRID_STAGE_TRAFFIC, GRID_STAGE_STREAMING, GRID_STAGE_PLACE, GRID_STAGE_COUNT };
static const char *const g_grid_stage_name[GRID_STAGE_COUNT] = {"ResetGameForRace", "PrepareForRaceMode", "DespawnTraffic", "EnableTrackStreaming", "PlaceCarsOnGrid"};

static grid_msg_fn g_grid_msg_orig, g_grid_start_orig;
static grid_ready_fn g_grid_ready_orig;
static grid_node_fn g_grid_node_orig[GRID_STAGE_COUNT];
static volatile LONG g_grid_stage_seen[GRID_STAGE_COUNT];
static volatile LONG g_grid_epoch, g_grid_ready_value = -1;
static volatile DWORD g_grid_tick0, g_grid_ready_tick;

static void grid_utc(char *out)
{
    SYSTEMTIME t;
    GetSystemTime(&t);
    wsprintfA(out, "%02u:%02u:%02u.%03u UTC", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
}

static void grid_line(const char *text)
{
    log_begin();
    log_raw(text);
    log_raw("\n");
}

static unsigned grid_msg_grid(void *a, void *b, void *c)
{
    char utc[32], line[96];
    int i;
    g_grid_tick0 = GetTickCount();
    g_grid_ready_value = -1;
    g_grid_ready_tick = 0;
    for (i = 0; i < GRID_STAGE_COUNT; ++i) g_grid_stage_seen[i] = 0;
    InterlockedExchange(&g_grid_epoch, 1);
    grid_utc(utc);
    wsprintfA(line, "grid: starting grid message at %s", utc);
    grid_line(line);
    return g_grid_msg_orig(a, b, c);
}

static void grid_stage(int stage, void *node)
{
    if (g_grid_epoch && !InterlockedExchange(&g_grid_stage_seen[stage], 1)) {
        char line[96];
        wsprintfA(line, "grid: %s +%lu ms", g_grid_stage_name[stage], (unsigned long)(GetTickCount() - g_grid_tick0));
        grid_line(line);
    }
    g_grid_node_orig[stage](node);
}

#define GRID_STAGE_STUB(n) static void grid_stage_##n(void *node) { grid_stage(n, node); }
GRID_STAGE_STUB(GRID_STAGE_RESET)
GRID_STAGE_STUB(GRID_STAGE_PREPARE)
GRID_STAGE_STUB(GRID_STAGE_TRAFFIC)
GRID_STAGE_STUB(GRID_STAGE_STREAMING)
GRID_STAGE_STUB(GRID_STAGE_PLACE)

static void grid_set_ready(void *console, unsigned value)
{
    LONG v = (LONG)(value & 0xff) != 0;
    if (g_grid_epoch && InterlockedExchange(&g_grid_ready_value, v) != v) {
        char utc[32], line[128];
        DWORD now = GetTickCount();
        if (v) g_grid_ready_tick = now;
        grid_utc(utc);
        wsprintfA(line, "grid: RenderReady %s +%lu ms (%s)", v ? "set" : "cleared", (unsigned long)(now - g_grid_tick0), utc);
        grid_line(line);
    }
    g_grid_ready_orig(console, value);
}

static unsigned grid_msg_start(void *a, void *b, void *c)
{
    if (g_grid_epoch) {
        char line[160];
        DWORD now = GetTickCount();
        if (g_grid_ready_tick)
            wsprintfA(line, "grid: race start message +%lu ms, RenderReady had been set %lu ms ago", (unsigned long)(now - g_grid_tick0),
                      (unsigned long)(now - g_grid_ready_tick));
        else
            wsprintfA(line, "grid: race start message +%lu ms, RenderReady NOT set yet: this client is still preparing the grid",
                      (unsigned long)(now - g_grid_tick0));
        grid_line(line);
        InterlockedExchange(&g_grid_epoch, 0);
    }
    return g_grid_start_orig(a, b, c);
}

static int patch_grid_trace(void)
{
    static const struct {
        unsigned rva;
        unsigned char bytes[16];
        const char *name;
        void *detour;
        void **original;
    } hooks[] = {
        {0x1824040, {0x48,0x89,0x6c,0x24,0x18,0x56,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x90,0x00,0x00},
         "grid: MsgStartingGrid::Receive", (void *)grid_msg_grid, (void **)&g_grid_msg_orig},
        {0x18241e0, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x30,0x48},
         "grid: MsgStartRace::Receive", (void *)grid_msg_start, (void **)&g_grid_start_orig},
        {0x17deda0, {0x48,0x81,0xc1,0x88,0x00,0x00,0x00,0x38,0x51,0x18,0x74,0x08,0x88,0x51,0x18,0xe9},
         "grid: ConsoleSynced::SetRenderReady", (void *)grid_set_ready, (void **)&g_grid_ready_orig},
        {0x8fdcd0, {0x48,0x83,0xec,0x28,0xe8,0x27,0x66,0xfd,0xff,0x48,0x8b,0x0d,0x80,0xdd,0xba,0x02},
         "grid: ResetGameForRace", (void *)grid_stage_GRID_STAGE_RESET, (void **)&g_grid_node_orig[GRID_STAGE_RESET]},
        {0x8fc390, {0x48,0x83,0xec,0x28,0xe8,0xa7,0xf0,0x8d,0xff,0x48,0x8b,0x0d,0xc0,0xf6,0xba,0x02},
         "grid: PrepareForRaceMode", (void *)grid_stage_GRID_STAGE_PREPARE, (void **)&g_grid_node_orig[GRID_STAGE_PREPARE]},
        {0x87c4a0, {0x48,0x8b,0xc4,0x41,0x56,0x48,0x83,0xec,0x60,0x48,0xc7,0x40,0xb8,0xfe,0xff,0xff},
         "grid: DespawnTraffic", (void *)grid_stage_GRID_STAGE_TRAFFIC, (void **)&g_grid_node_orig[GRID_STAGE_TRAFFIC]},
        {0x88cc00, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48},
         "grid: EnableTrackStreaming", (void *)grid_stage_GRID_STAGE_STREAMING, (void **)&g_grid_node_orig[GRID_STAGE_STREAMING]},
        {0x8ea730, {0x40,0x57,0x48,0x81,0xec,0x80,0x00,0x00,0x00,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff},
         "grid: PlaceCarsOnGrid", (void *)grid_stage_GRID_STAGE_PLACE, (void **)&g_grid_node_orig[GRID_STAGE_PLACE]},
    };
    unsigned char *img = find_fh2_image();
    unsigned i;
    int ok = 1;
    if (!img) return 0;
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        if (memcmp(img + hooks[i].rva, hooks[i].bytes, sizeof hooks[i].bytes)) {
            log_begin();
            log_raw("grid: unexpected game code at ");
            log_raw(hooks[i].name);
            log_raw(", the starting grid is not traced\n");
            return 0;
        }
    }
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i)
        if (!plat_hook(hooks[i].name, img + hooks[i].rva, hooks[i].detour, hooks[i].original)) ok = 0;
    if (!ok) {
        log_begin();
        log_raw("grid: some hooks could not be installed, the starting grid trace is incomplete (fh2-mods\\platform.log)\n");
    }
    return ok;
}
