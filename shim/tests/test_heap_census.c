/* The heap census of src/health.inc on a real heap: a private heap grown to about 100 MB, filled with pointers to one function of this exe, must be the largest
 * heap found, and that pointer the one found most. Under Wine the heap has no NT segments, so the census says so and the rest is not checked. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static SRWLOCK g_light_lock = SRWLOCK_INIT;
static struct { ULONGLONG prediction_at; } g_light;
static char g_lines[16384];
static char g_cur[2048];
static int g_n;
static CRITICAL_SECTION g_cs;

static void log_begin(void) { EnterCriticalSection(&g_cs); }
static void log_raw(const char *s)
{
    size_t n = strlen(s);
    if (g_n + n >= sizeof g_cur) n = sizeof g_cur - 1 - g_n;
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
static void log_hex(unsigned long long v, int digits)
{
    char buf[17];
    int i;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    buf[digits] = 0;
    log_raw(buf);
}
static volatile LONG g_place_logged = 0;

#include "src/xhr_stats.inc"
#include "src/health.inc"

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

__attribute__((noinline)) static void marker(void) { puts("marker"); }

int main(void)
{
    HANDLE heap = HeapCreate(0, 0, 0);
    unsigned long long target = (unsigned long long)(ULONG_PTR)&marker, exe = (unsigned long long)(ULONG_PTR)GetModuleHandleW(NULL);
    health_mem_now s;
    char want[64];
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_cs);
    CHECK(heap != NULL);
    for (i = 0; i < 400; i++) {                       /* 400 x 256 KB: under the size a growable heap gives its own VirtualAlloc */
        ULONG_PTR *b = HeapAlloc(heap, 0, 256 * 1024);
        size_t k;
        CHECK(b != NULL);
        for (k = 0; b && k < 256 * 1024 / sizeof *b; k++) {
            b[k] = (ULONG_PTR)target;
        }
    }
    CHECK(health_mem_read(&s));
    s.commit = s.total * 2;                            /* as at the first census */
    health_census(&s);
    CHECK(strstr(g_lines, "health: heap census at commit ") != NULL);
    if (strstr(g_lines, "no NT heap segment found")) {
        puts("heap census: no NT heap here (Wine), the sampling is not checked");
    } else {
        char *h = strstr(g_lines, "heap 0x");
        CHECK(h && strtoull(h + 7, NULL, 16) == (unsigned long long)(ULONG_PTR)heap);   /* ours is the largest */
        CHECK(strstr(g_lines, "(not the process heap)") != NULL);
        char *most = strstr(g_lines, "the pointers into modules found most: ");
        snprintf(want, sizeof want, "x test.exe+0x%llx", target - exe);
        CHECK(most != NULL);
        CHECK(most && strstr(most, want) != NULL && strstr(most, want) == strstr(most, " x ") + 1);   /* and it is the first one listed */
    }
    marker();
    if (!g_failed) {
        puts("heap census: all checks pass");
    }
    return g_failed != 0;
}
