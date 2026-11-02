/* What health.inc reports: requests of the last period by kind, and the ones still waiting for an answer (a slot holds the time a request started). */
enum { XHR_CLASS_XLS, XHR_CLASS_OTHER, XHR_CLASSES };
#define XHR_OPEN_SLOTS 32
static volatile LONG g_xhr_ok[XHR_CLASSES], g_xhr_bad[XHR_CLASSES], g_xhr_in_flight, g_xhr_activity;
static volatile LONG64 g_xhr_last[XHR_CLASSES], g_xhr_open[XHR_OPEN_SLOTS];

static int xhr_stat_begin(void)
{
    LONG64 now = (LONG64)GetTickCount64();
    int i;

    InterlockedIncrement(&g_xhr_in_flight);
    for (i = 0; i < XHR_OPEN_SLOTS; i++) {
        if (InterlockedCompareExchange64(&g_xhr_open[i], now, 0) == 0) {
            return i;
        }
    }
    return -1;
}

static void xhr_stat_end(int slot, int cls, int good)
{
    if (slot >= 0) {
        InterlockedExchange64(&g_xhr_open[slot], 0);
    }
    InterlockedDecrement(&g_xhr_in_flight);
    InterlockedIncrement(good ? &g_xhr_ok[cls] : &g_xhr_bad[cls]);
    InterlockedExchange64(&g_xhr_last[cls], (LONG64)GetTickCount64());
    InterlockedExchange(&g_xhr_activity, 1);
}

static ULONGLONG xhr_oldest_start(void)
{
    ULONGLONG oldest = 0;
    int i;

    for (i = 0; i < XHR_OPEN_SLOTS; i++) {
        ULONGLONG t = (ULONGLONG)g_xhr_open[i];
        if (t && (!oldest || t < oldest)) {
            oldest = t;
        }
    }
    return oldest;
}
