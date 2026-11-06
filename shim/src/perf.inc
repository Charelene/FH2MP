/* Cost of FH2MP's own per-frame code (issue #354). Compiled only with -DFH2MP_PERF (FH2MP_PERF=1 shim/build.sh): QueryPerformanceCounter around OUR part of
 * each hook (the game's original is not counted), the game's frame interval seen at the audio update, and one log line every PERF_PERIOD_S seconds.
 * The release build has none of this. */
#ifdef FH2MP_PERF
#define PERF_PERIOD_S 15
enum { PERF_SEND, PERF_RECEIVE, PERF_PUBLISH, PERF_ANIM, PERF_HORN, PERF_N };
static const char *const g_perf_name[PERF_N] = {"send", "receive", "publish", "anim", "horn"};
typedef struct { volatile LONG64 ticks, max; volatile LONG calls; } perf_slot;
static perf_slot g_perf[PERF_N];
static volatile LONG64 g_perf_frames, g_perf_frame_sum, g_perf_frame_max, g_perf_last_frame;
static volatile LONG g_perf_over33, g_perf_over50, g_perf_over100;
static LONG64 g_perf_freq;

static LONG64 perf_now(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
static void perf_add(int id, LONG64 ticks)
{
    LONG64 seen;
    InterlockedExchangeAdd64(&g_perf[id].ticks, ticks);
    InterlockedIncrement(&g_perf[id].calls);
    while ((seen = g_perf[id].max) < ticks && InterlockedCompareExchange64(&g_perf[id].max, ticks, seen) != seen) {}
}
/* called once per game frame (the audio update) */
static void perf_frame(void)
{
    LONG64 now = perf_now(), last = InterlockedExchange64(&g_perf_last_frame, now), dt, seen, ms10;
    if (!last) return;
    dt = now - last;
    InterlockedIncrement64(&g_perf_frames);
    InterlockedExchangeAdd64(&g_perf_frame_sum, dt);
    while ((seen = g_perf_frame_max) < dt && InterlockedCompareExchange64(&g_perf_frame_max, dt, seen) != seen) {}
    ms10 = dt * 10000 / g_perf_freq;
    if (ms10 > 330) InterlockedIncrement(&g_perf_over33);
    if (ms10 > 500) InterlockedIncrement(&g_perf_over50);
    if (ms10 > 1000) InterlockedIncrement(&g_perf_over100);
}
/* The light material publisher runs once per car per frame: the first car seen counts the frames, another car takes over when it has been silent for 1 s. */
static void *volatile g_perf_car;
static void perf_tick(void *car)
{
    void *mine = g_perf_car;
    if (mine != car) {
        if (mine && perf_now() - g_perf_last_frame < g_perf_freq) return;
        g_perf_car = car; g_perf_last_frame = 0;
    }
    perf_frame();
}
static unsigned long perf_us(LONG64 ticks) { return (unsigned long)(ticks * 1000000 / g_perf_freq); }
static DWORD WINAPI perf_worker(void *unused)
{
    (void)unused;
    for (;;) {
        LONG64 frames, sum, fmax, snap[PERF_N][2]; LONG calls[PERF_N], o33, o50, o100; int i;
        Sleep(PERF_PERIOD_S * 1000);
        frames = InterlockedExchange64(&g_perf_frames, 0); sum = InterlockedExchange64(&g_perf_frame_sum, 0);
        fmax = InterlockedExchange64(&g_perf_frame_max, 0);
        o33 = InterlockedExchange(&g_perf_over33, 0); o50 = InterlockedExchange(&g_perf_over50, 0); o100 = InterlockedExchange(&g_perf_over100, 0);
        for (i = 0; i < PERF_N; i++) {
            snap[i][0] = InterlockedExchange64(&g_perf[i].ticks, 0); snap[i][1] = InterlockedExchange64(&g_perf[i].max, 0);
            calls[i] = InterlockedExchange(&g_perf[i].calls, 0);
        }
        if (!frames) continue;
        log_begin();
        log_raw("perf: "); log_dec(PERF_PERIOD_S); log_raw(" s, ");
        log_dec((unsigned long)frames); log_raw(" frames, avg "); log_dec(perf_us(sum / frames)); log_raw(" us, max "); log_dec(perf_us(fmax));
        log_raw(" us, over 33/50/100 ms "); log_dec((unsigned long)o33); log_raw("/"); log_dec((unsigned long)o50); log_raw("/"); log_dec((unsigned long)o100);
        log_raw(" | FH2MP us per call (avg/max, calls):");
        for (i = 0; i < PERF_N; i++) {
            log_raw(" "); log_raw(g_perf_name[i]); log_raw(" ");
            log_dec(calls[i] ? perf_us(snap[i][0] / calls[i]) : 0); log_raw("/"); log_dec(perf_us(snap[i][1])); log_raw(","); log_dec((unsigned long)calls[i]);
        }
        {
            LONG64 total = 0;
            for (i = 0; i < PERF_N; i++) total += snap[i][0];
            log_raw(" | FH2MP total "); log_dec(perf_us(total) / 1000); log_raw(" ms"); log_raw("\n");
        }
    }
    return 0;
}
static void perf_start(void)
{
    LARGE_INTEGER f; HANDLE t;
    QueryPerformanceFrequency(&f); g_perf_freq = f.QuadPart ? f.QuadPart : 1;
    t = CreateThread(NULL, 0, perf_worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    log_begin(); log_raw("perf: measuring build, one line every "); log_dec(PERF_PERIOD_S); log_raw(" s while the game runs frames\n");
}
#endif
