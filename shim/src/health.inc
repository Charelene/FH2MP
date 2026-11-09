/* What the game is doing when nothing fails. The release log only has failures, so a game that stops asking the server for anything, a request that never
 * comes back, a game that stops simulating and a process that stopped running all look the same: no lines. The reports that came down to this: Xls calls
 * ending mid-session (dev 2026-11-01), 111 s after being matched (dev 2026-11-01), the freeze in issue #354.
 *
 * One thread wakes once a second and reads what the hooks already record: the time of the last own-car prediction (lighting_send) and the request counters of
 * xhr2.inc. No hook gets slower. It writes a line when one of these changes:
 *   health: process          the thread itself did not run for seconds: the whole process stood still (a debugger, swapping, the machine suspended, Wine)
 *   health: predictions      the game stopped or resumed sending its car; this also happens in loading screens, so it only says when, not why
 * and every HEALTH_SUMMARY_S seconds a summary with the requests of that period, if there were any or some are in flight. FH2MP_NO_HEALTH turns it off.
 *
 * The part above the #ifndef has no Windows calls: shim/tests/test_health.sh runs it natively; shim/tests/test_health_thread.sh runs the thread under Wine. */
#define HEALTH_PROCESS_GAP_MS 3000u     /* a wake this late means the process did not run */
#define HEALTH_FRESH_MS 2000u           /* predictions counted as flowing */
#define HEALTH_SILENT_MS 5000u          /* flowing before, nothing for this long: stopped */
#ifndef HEALTH_SUMMARY_S
#define HEALTH_SUMMARY_S 300u
#endif
#define HEALTH_LINES_MAX 120u           /* stop/resume lines per game; after that every 25th */

enum { HEALTH_NOTHING = 0, HEALTH_PROCESS_PAUSED = 1, HEALTH_PREDICTIONS_STOPPED = 2, HEALTH_PREDICTIONS_RESUMED = 4 };

typedef struct {
    unsigned long long woke, silent_from;
    int flowing, silent;
} health_state;

/* now and last_prediction are GetTickCount64 values; last_prediction 0 = none yet (or the lighting hooks are off). -> HEALTH_ bits; *ms = the length of what ended
 * or, for a stop, how long it has been silent. A process pause reports through *pause_ms. */
static int health_step(health_state *h, unsigned long long now, unsigned long long last_prediction, unsigned long long *ms, unsigned long long *pause_ms)
{
    int out = HEALTH_NOTHING;
    unsigned long long age = last_prediction && now >= last_prediction ? now - last_prediction : ~0ULL;

    if (h->woke && now - h->woke >= HEALTH_PROCESS_GAP_MS) {
        out |= HEALTH_PROCESS_PAUSED;
        *pause_ms = now - h->woke;
    }
    h->woke = now;
    if (h->silent) {
        if (last_prediction != h->silent_from && age < HEALTH_FRESH_MS) {
            h->silent = 0;
            h->flowing = 1;
            out |= HEALTH_PREDICTIONS_RESUMED;
            *ms = last_prediction - h->silent_from;
        }
    } else if (age < HEALTH_FRESH_MS) {
        h->flowing = 1;
    } else if (h->flowing && age != ~0ULL && age >= HEALTH_SILENT_MS) {
        h->silent = 1;
        h->flowing = 0;
        h->silent_from = last_prediction;
        out |= HEALTH_PREDICTIONS_STOPPED;
        *ms = age;
    }
    return out;
}

/* Exactly 8 lowercase hex digits (what the launcher writes and native/internal/diag RunID accepts). */
static int run_id_ok(const char *s)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return s[8] == 0;
}

/* "ab12cd34.57": the game (run) and the request (the xhr# of fh2mp.log), for X-FH2MP-Run. out holds 20 characters. */
static void run_request_text(char *out, const char *run, unsigned long seq)
{
    char digits[11];
    int n = 0, i = 0;

    do {
        digits[n++] = (char)('0' + seq % 10);
        seq /= 10;
    } while (seq);
    while (run[i]) {
        out[i] = run[i];
        i++;
    }
    out[i++] = '.';
    while (n) {
        out[i++] = digits[--n];
    }
    out[i] = 0;
}

#ifndef HEALTH_LOGIC_ONLY
static void health_counts(const char *label, int cls)
{
    LONG ok = InterlockedExchange(&g_xhr_ok[cls], 0), bad = InterlockedExchange(&g_xhr_bad[cls], 0);
    ULONGLONG last = g_xhr_last[cls];

    log_raw(label);
    log_raw(" ");
    log_dec((unsigned long)ok);
    log_raw(" ok ");
    log_dec((unsigned long)bad);
    log_raw(" bad, last ");
    if (last) {
        log_dec((unsigned long)((GetTickCount64() - last) / 1000));
        log_raw(" s ago");
    } else {
        log_raw("never");
    }
}

static DWORD WINAPI health_thread(LPVOID unused)
{
    health_state h = {0, 0, 0, 0};
    unsigned ticks = 0, lines = 0;

    (void)unused;
    for (;;) {
        unsigned long long now, ms = 0, pause_ms = 0, last;
        int what;

        Sleep(1000);
        AcquireSRWLockShared(&g_light_lock);
        last = g_light.prediction_at;
        ReleaseSRWLockShared(&g_light_lock);
        now = GetTickCount64();
        what = health_step(&h, now, last, &ms, &pause_ms);
        if (what & HEALTH_PROCESS_PAUSED) {
            log_begin();
            log_raw("health: process did not run for ");
            log_dec((unsigned long)pause_ms);
            log_raw(" ms (a 1 s timer woke that late)\n");
        }
        if ((what & (HEALTH_PREDICTIONS_STOPPED | HEALTH_PREDICTIONS_RESUMED)) && (++lines <= HEALTH_LINES_MAX || lines % 25 == 0)) {
            log_begin();
            log_raw(what & HEALTH_PREDICTIONS_STOPPED ? "health: predictions stopped, none for " : "health: predictions resumed after ");
            log_dec((unsigned long)ms);
            log_raw(" ms, ");
            log_dec((unsigned long)g_xhr_in_flight);
            log_raw(" requests in flight\n");
        }
        if (++ticks >= HEALTH_SUMMARY_S) {
            LONG flying = g_xhr_in_flight;
            ULONGLONG oldest = xhr_oldest_start();

            ticks = 0;
            if (InterlockedCompareExchange(&g_xhr_activity, 0, 1) == 0 && !flying) {
                continue;     /* nothing asked for since the last summary and nothing waiting: the line before this still holds */
            }
            log_begin();
            log_raw("health: ");
            log_dec(HEALTH_SUMMARY_S);
            log_raw(" s: ");
            health_counts("xls", XHR_CLASS_XLS);
            log_raw("; ");
            health_counts("other", XHR_CLASS_OTHER);
            log_raw("; in flight ");
            log_dec((unsigned long)flying);
            if (oldest) {
                log_raw(", oldest ");
                log_dec((unsigned long)((GetTickCount64() - oldest) / 1000));
                log_raw(" s");
            }
            log_raw("; last prediction ");
            if (last) {
                log_dec((unsigned long)((now - last) / 1000));
                log_raw(" s ago\n");
            } else {
                log_raw("never\n");
            }
        }
    }
    return 0;
}

static void health_start(void)
{
    wchar_t off[8];
    HANDLE t;

    if (GetEnvironmentVariableW(L"FH2MP_NO_HEALTH", off, 8) != 0) {
        return;
    }
    t = CreateThread(NULL, 0, health_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        DWORD err = GetLastError();
        log_begin();
        log_raw("health: could not start the watcher thread, error ");
        log_dec(err);
        log_raw("\n");
    }
}
#endif
