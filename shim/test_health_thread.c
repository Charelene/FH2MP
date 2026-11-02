/* The watcher thread of src/health.inc on real Win32 timers and threads, with the request counters of src/xhr_stats.inc. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SRWLOCK g_light_lock = SRWLOCK_INIT;
static struct { ULONGLONG prediction_at; } g_light;
static char g_lines[8192];
static char g_cur[256];
static int g_n;
static CRITICAL_SECTION g_cs;

static void log_begin(void) { EnterCriticalSection(&g_cs); }
static void log_raw(const char *s)
{
    size_t n = strlen(s);
    memcpy(g_cur + g_n, s, n);
    g_n += (int)n;
    g_cur[g_n] = 0;
    if (n && s[n - 1] == '\n') {
        strcat(g_lines, g_cur);
        fputs(g_cur, stdout);
        g_n = 0;
        LeaveCriticalSection(&g_cs);
    }
}
static void log_dec(unsigned long v) { char b[16]; sprintf(b, "%lu", v); log_raw(b); }

#define HEALTH_SUMMARY_S 2u
#include "src/xhr_stats.inc"
#include "src/health.inc"

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

static void predictions(int ms)
{
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        AcquireSRWLockExclusive(&g_light_lock);
        g_light.prediction_at = GetTickCount64();
        ReleaseSRWLockExclusive(&g_light_lock);
        Sleep(20);
    }
}

int main(void)
{
    HANDLE t;
    int slot;

    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_cs);
    xhr_stat_end(xhr_stat_begin(), XHR_CLASS_XLS, 1);
    xhr_stat_end(xhr_stat_begin(), XHR_CLASS_XLS, 0);
    xhr_stat_end(xhr_stat_begin(), XHR_CLASS_OTHER, 1);
    slot = xhr_stat_begin();          /* stays in flight */
    CHECK(xhr_oldest_start() != 0);
    t = CreateThread(NULL, 0, health_thread, NULL, 0, NULL);
    CHECK(t != NULL);
    predictions(3000);
    CHECK(strstr(g_lines, "health: 2 s: xls 1 ok 1 bad, last ") != NULL);
    CHECK(strstr(g_lines, "other 1 ok 0 bad, last ") != NULL);
    CHECK(strstr(g_lines, "in flight 1, oldest ") != NULL);
    CHECK(strstr(g_lines, "last prediction 0 s ago") != NULL);
    Sleep(6500);                      /* the game stops sending its car */
    CHECK(strstr(g_lines, "health: predictions stopped, none for ") != NULL);
    CHECK(strstr(g_lines, ", 1 requests in flight") != NULL);
    predictions(2000);
    CHECK(strstr(g_lines, "health: predictions resumed after ") != NULL);
    xhr_stat_end(slot, XHR_CLASS_OTHER, 0);
    CHECK(xhr_oldest_start() == 0 && g_xhr_in_flight == 0);
    SuspendThread(t);                 /* what a stopped process looks like from inside: the timer wakes seconds late */
    Sleep(4000);
    ResumeThread(t);
    Sleep(1500);
    CHECK(strstr(g_lines, "health: process did not run for ") != NULL);
    if (!g_failed) {
        puts("health thread: all checks pass");
    }
    fflush(stdout);
    ExitProcess(g_failed != 0);
}
