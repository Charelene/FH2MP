/* The shim's log (src/log.inc) without a log file: every thread that logs must get the line lock back. Until 2026-11-07 log_raw() returned
 * before the newline released the lock when fh2mp.log could not be opened, so the first thread that logged kept the lock forever and every
 * other thread spun in log_begin() (the whole shim stood still). Also: with a log file the lines are written. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define MAX_ARG_CHARS 256
static HANDLE g_log = INVALID_HANDLE_VALUE;
#include "src/log.inc"

static int failed;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failed = 1; } } while (0)

static DWORD WINAPI logger(void *arg)
{
    log_begin();
    log_raw((const char *)arg);
    return 0;
}

/* -> 1 when a second thread could log a whole line within 2 s */
static int second_thread_logs(void)
{
    HANDLE t = CreateThread(NULL, 0, logger, (void *)"second: thread line\n", 0, NULL);
    DWORD rc = WaitForSingleObject(t, 2000);
    CloseHandle(t);
    return rc == WAIT_OBJECT_0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* no log file: the first thread's line must release the lock for the second thread */
    {
        HANDLE t = CreateThread(NULL, 0, logger, (void *)"first: no file line\n", 0, NULL);
        CHECK(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0);
        CloseHandle(t);
    }
    CHECK(second_thread_logs());
    CHECK(g_log_lock == 0 && g_log_owner == 0);
    /* a line that stops in the middle keeps the lock on purpose (its thread finishes it); the same thread may go on logging (re-entrant) */
    log_begin();
    log_raw("part");
    CHECK(g_log_lock == 1);
    log_raw(" of a line\n");
    CHECK(g_log_lock == 0);
    /* with a log file the lines are written: words of the keep-list pass the release filter, the rest is dropped */
    {
        wchar_t path[MAX_PATH];
        char buf[4096];
        DWORD n = 0;
        GetTempPathW(MAX_PATH, path);
        lstrcatW(path, L"fh2mp-test-log.txt");
        DeleteFileW(path);
        g_log = CreateFileW(path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(g_log != INVALID_HANDLE_VALUE);
        log_begin();
        log_raw("session: kept because of the keep-list\n");
        log_begin();
        log_raw("chatter the release build drops\n");
        CHECK(second_thread_logs());
        SetFilePointer(g_log, 0, NULL, FILE_BEGIN);
        ReadFile(g_log, buf, sizeof buf - 1, &n, NULL);
        buf[n] = 0;
        CHECK(strstr(buf, "session: kept because of the keep-list") != NULL);
        CHECK(strstr(buf, "chatter") == NULL);
        CHECK(strstr(buf, "second: thread line") == NULL);   /* not on the keep-list either */
        CloseHandle(g_log);
        DeleteFileW(path);
    }
    puts(failed ? "log: FAIL" : "log: all checks pass");
    return failed;
}
