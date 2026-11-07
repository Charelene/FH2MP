/* The shim's log: fh2mp.log in the Azimuth datain folder (g_log, opened by fh2mod_init). One line at a time: log_begin() takes the lock and
 * stamps the line, log_raw/log_dec/log_hex/log_wide append to it, and a string ending in a newline writes it and releases the lock. Release builds
 * keep only the lines line_wanted() accepts (a keep-list plus FH2MP_LOG_KEEP); the debug build writes everything. Included by fh2mp_mod.c, which
 * declares g_log; shim/tests/test_log.sh runs it on its own. */
static volatile LONG g_log_lock;
static volatile DWORD g_log_owner;

static void log_lock(void)
{
    DWORD me = GetCurrentThreadId();

    if (g_log_owner == me) {
        return; /* re-entrant on the same thread */
    }
    while (InterlockedCompareExchange(&g_log_lock, 1, 0) != 0) {
        SwitchToThread();
    }
    g_log_owner = me;
}

static void log_unlock(void)
{
    if (g_log_owner == GetCurrentThreadId()) {
        g_log_owner = 0;
        InterlockedExchange(&g_log_lock, 0);
    }
}

/* log_begin() takes the lock; a string ending in '\n' releases it, so each line is atomic */
#ifndef FH2MP_DEBUG_BUILD
static char g_line[1024];
static int g_ll;
/* FH2MP_LOG_KEEP: more of the release log without a debug build. "all", or words like the ones below, separated by commas ("xhr#,arg["). */
static char g_log_keep[128];

static int line_has(const char *l, int n, const char *pat)
{
    int i, k;
    for (i = 0; i < n; i++) {
        for (k = 0; pat[k] && i + k < n && l[i + k] == pat[k]; k++) {
        }
        if (!pat[k]) {
            return 1;
        }
    }
    return 0;
}

static int line_wanted(const char *l, int n)
{
    static const char *const keep[] = {"attached", "session:", "identity:", "late:", "not patched", "NOT ", "unexpected", "fail", "error", "could not",
                                       "refus", "crash", "xsbody", "xstoken", "mmtimeout", "mmtoast", "online-setup", "bindfix", "gamedb:", "timelock:", "rivalsleave", "bucketcars", "matchpoll", "sample", "carprobe", "probe:", "lighting:", "honk:", "grid:",
                                       "xhr:", "winsock:", "placement:", "netstrings:", "torqueload:", "nulltorque", "speedpb:", "cinematics:",
                                       "festivalgrid:", "invitetype:", "ticket:", "tools:", "offline:", "rebound", "redirect:", "serverport:", "secureip:", "hook:", "perf:", "health:", "diag:", "partyhud:", "carpool:", 0};
    int i;
    const char *word;
    for (i = 0; keep[i]; i++) {
        if (line_has(l, n, keep[i])) {
            return 1;
        }
    }
    for (word = g_log_keep; *word; word += lstrlenA(word) + 1) {
        if ((word[0] == 'a' && word[1] == 'l' && word[2] == 'l' && !word[3]) || line_has(l, n, word)) {
            return 1;
        }
    }
    return 0;
}

static void log_keep_load(void)
{
    DWORD i, n = GetEnvironmentVariableA("FH2MP_LOG_KEEP", g_log_keep, sizeof g_log_keep - 1);

    if (n >= sizeof g_log_keep - 1) {
        n = 0;
    }
    g_log_keep[n] = g_log_keep[n + 1] = 0;
    for (i = 0; i < n; i++) {
        if (g_log_keep[i] == ',') {
            g_log_keep[i] = 0;
        }
    }
}
#endif

static void log_raw(const char *s)
{
    DWORD written;
    DWORD n = 0;

    /* No early return without a log file: log_begin() took the lock, and only the newline below gives it back. Returning here used to leave
     * the lock with the first thread that logged, and every other thread that logged afterwards spun forever (the whole shim stood still
     * whenever fh2mp.log could not be opened). The line is assembled as usual and simply not written. */
    while (s[n]) {
        n++;
    }
#ifndef FH2MP_DEBUG_BUILD
    /* Release builds keep the log short: lines are assembled here and only the ones that matter to a player are written
     * (start-up summary, anything that failed). The debug build logs everything. */
    {
        DWORD i;
        for (i = 0; i < n; i++) {
            if (g_ll < (int)sizeof g_line - 1) {
                g_line[g_ll++] = s[i];
            }
        }
        if (n > 0 && s[n - 1] == '\n') {
            g_line[g_ll] = 0;
            if (g_log != INVALID_HANDLE_VALUE && line_wanted(g_line, g_ll)) {
                WriteFile(g_log, g_line, (DWORD)g_ll, &written, NULL);
            }
            g_ll = 0;
            log_unlock();
        }
        return;
    }
#endif
    if (g_log != INVALID_HANDLE_VALUE) {
        WriteFile(g_log, s, n, &written, NULL);
    }
    if (n > 0 && s[n - 1] == '\n') {
        log_unlock();
    }
}

static void log_hex(unsigned long long v, int digits)
{
    char buf[17];
    int i;

    for (i = digits - 1; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    buf[digits] = '\0';
    log_raw(buf);
}

static void log_dec(unsigned long v)
{
    char buf[12];
    int i = 11;

    buf[i] = '\0';
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    log_raw(buf + i);
}

static void log_wide(const wchar_t *w)
{
    char buf[3 * MAX_ARG_CHARS + 1];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof buf, NULL, NULL);

    log_raw(n > 0 ? buf : "?");
}

static void log_begin(void)
{
    log_lock();
    log_raw("[");
    log_dec(GetTickCount());
    log_raw("] ");
}
