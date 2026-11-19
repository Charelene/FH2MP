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
    /* A game-bug guard's hit is the one line that says a crash was turned away, so the release log keeps it. One hit line per guard of the
     * offline guard list in fh2mp_mod.c, as each guard writes it. Until 2026-11-19 modelguard, dtconv, camanim, complookup and garagerating
     * (and the then new travelxp and carroom) were not on the keep-list, so a player's fh2mp.log never showed those guards working. */
    {
        static const char *const hits[] = {
            "hook: speedometer guard: the jump at game+0xad6070 (UpdateGauges, hooked by another mod) leads to 0x0000000000000000, which is not executable memory; the game's own code is put back\n",
            "hook: gameplay log guard: an event for a user with no CGameplayLog (no record from Logon) is left out\n",
            "hook: remote prediction guard: a car's player has no user in the session (left or not joined yet); the car is skipped\n",
            "modelguard: a car's swap to the placeholder model was skipped: the car has no model loader (it would read NULL at game+0xf6d928); it keeps its model until its next change (#1)\n",
            "dtconv: a car binding names car ordinal 7, which is not in the car table; the drivetrain converter answers no value instead of reading NULL at game+0xb99cd3 (#1)\n",
            "camanim: a camera animation player (mode 3) has no space provider; its position stays as it is instead of a NULL read at game+0x1031ece (#1)\n",
            "complookup: a component lookup on a missing (NULL) entity, called from game+0x2d8e00, answered 'no component' instead of reading address 0x68 at game+0x2d8ea4 (#1)\n",
            "travelxp: a route's XP was not rounded: the travel XP component's RoundXPTo is 0 (it would divide by zero at game+0x66a492); the XP is the route's own, clamped to MinXP..MaxXP (#1)\n",
            "carroom: a new world's car list has room for 256 cars before it would move (the list moved under a worker thread in #486) (#1)\n",
            "nulltorque: skipped a car's torque curve scale\n",
            "torqueload: a car's engine curve is not usable; the native fallback is used\n",
            "garagerating: garage car 5 (save row 2, engine part 3, camshaft part 4) is not rated (#1): x, value 0; it stays in the garage with empty performance numbers\n",
            "carphysics: car 2479 from game+0x346e1e (the upgrade parts preview) has physics data the game cannot build (#1, engine part 2479000, camshaft part 3919000): x, value 0; built from its own stock parts with the default tune instead\n",
            "cinematics: the game's media/Cinematics directory query returned null (game+0x21f44d); its optional registration is skipped\n",
            "placement: Azimuth would stop the game for this texture, created it as an ordinary texture instead:\n",
            /* and the probes and fallbacks that report what the game did: a lost server connection, a car wait let through */
            "netwatch: the game closed its connection to the game server (close #1, connection state 6): reason 0x80600008, nothing arrived from the server within the time limit\n",
            "carwait: the wait for every player's car is let through after 41 s: nothing in the car cache changed for 20 s (free slots 0, cars waiting for a slot 2, loading 0, loaded 9)\n",
            0};
        wchar_t path[MAX_PATH];
        char buf[8192];
        DWORD n = 0;
        int i;
        GetTempPathW(MAX_PATH, path);
        lstrcatW(path, L"fh2mp-test-log-guards.txt");
        DeleteFileW(path);
        g_log = CreateFileW(path, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(g_log != INVALID_HANDLE_VALUE);
        for (i = 0; hits[i]; i++) {
            log_begin();
            log_raw(hits[i]);
        }
        SetFilePointer(g_log, 0, NULL, FILE_BEGIN);
        ReadFile(g_log, buf, sizeof buf - 1, &n, NULL);
        buf[n] = 0;
        for (i = 0; hits[i]; i++) {
            char head[24];
            lstrcpynA(head, hits[i], sizeof head);
            if (!strstr(buf, head)) {
                printf("FAIL: a line the release log must keep is dropped: %s", hits[i]);
                failed = 1;
            }
        }
        CloseHandle(g_log);
        DeleteFileW(path);
    }
    puts(failed ? "log: FAIL" : "log: all checks pass");
    return failed;
}
