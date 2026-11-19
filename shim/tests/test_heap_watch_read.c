/* The heap watch of src/heap_watch.inc on the real process heap: its three hooks called the way ntdll's exports would be (the platform is not here to
 * place them), with the real RtlAllocateHeap, RtlReAllocateHeap and RtlFreeHeap behind. What a function of this exe allocates and keeps must be held
 * by this exe in the line, and what it frees must not count. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "fh2mod.h"

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
/* what slice_watch.inc takes from fh2mp_mod.c; the hooks themselves are not made in this test */
#define FH2_SIZE_OF_IMAGE 0x3a18000u
static const FH2ModApi *g_api;
static FH2Mod *g_mod;
static unsigned char *find_fh2_image(void) { return NULL; }
static int bytes_first_diff(const void *a, const void *b, unsigned n) { return memcmp(a, b, n) ? 0 : -1; }
static const char *plat_tag(const char *name) { return name; }
static void plat_failed(const char *name, const void *at, const char *step) { (void)name, (void)at, (void)step; }

#include "src/xhr_stats.inc"
#define HEALTH_SLICE_WATCH
#include "src/health.inc"

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

#define BLOCKS 10
static void *g_blocks[BLOCKS];

__attribute__((noinline)) static void keep_from_here(void)
{
    int i;
    for (i = 0; i < BLOCKS; i++) {
        g_blocks[i] = heap_watch_alloc_hook(GetProcessHeap(), 0, 4u << 20);
    }
}

int main(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    unsigned long long exe = (unsigned long long)(ULONG_PTR)GetModuleHandleW(NULL);
    void *small, *grown;
    char *held;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_cs);
    g_hw_alloc_orig = (heap_watch_alloc_fn)(void *)GetProcAddress(ntdll, "RtlAllocateHeap");
    g_hw_realloc_orig = (heap_watch_realloc_fn)(void *)GetProcAddress(ntdll, "RtlReAllocateHeap");
    g_hw_free_orig = (heap_watch_free_fn)(void *)GetProcAddress(ntdll, "RtlFreeHeap");
    CHECK(g_hw_alloc_orig && g_hw_realloc_orig && g_hw_free_orig);
    g_hw = (heap_watch_table *)VirtualAlloc(NULL, sizeof *g_hw, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    CHECK(g_hw != NULL);
    g_hw_heap = GetProcessHeap();
    heap_watch_skip_module(L"ntdll.dll");
    heap_watch_skip_module(L"KERNELBASE.dll");
    heap_watch_skip_module(L"kernel32.dll");
    InterlockedExchange(&g_hw_on, 1);

    keep_from_here();                                                   /* 10 x 4 MB kept */
    for (i = 0; i < 4; i++) {                                           /* 4 of them freed again */
        CHECK(heap_watch_free_hook(GetProcessHeap(), 0, g_blocks[i]));
    }
    small = heap_watch_alloc_hook(GetProcessHeap(), 0, 100);            /* under 4 KB: not followed */
    grown = heap_watch_realloc_hook(GetProcessHeap(), 0, g_blocks[4], 8u << 20);   /* one kept block grows to 8 MB, from main */
    CHECK(small && grown);
    heap_watch_free_hook(GetProcessHeap(), 0, small);
    CHECK(g_hw->allocs == 11 && g_hw->frees == 5);
    heap_watch_finish();
    /* 10 x 4 MB + 8 MB asked for; 4 x 4 MB freed, and the grown block's old 4 MB; 5 x 4 MB + 8 MB still held */
    CHECK(strstr(g_lines, "health: heap watch over 5 s: 11 process-heap allocations of 4 KB or more, 48 MB, 20 MB of it freed again; still held 28 MB in 6;")
          != NULL);
    held = strstr(g_lines, "held by: ");
    CHECK(held != NULL);
    if (held) {
        unsigned long long off = strtoull(strstr(held, "+0x") + 3, NULL, 16);
        CHECK(strncmp(held + 9, "test.exe+0x", 11) == 0);              /* the first caller outside ntdll and kernel32 is this exe */
        CHECK(off < 0x100000 && exe);
        CHECK(strstr(held, " 20 MB (5), test.exe+0x") != NULL);          /* keep_from_here's five first, then main's grown block */
        CHECK(strstr(held, " 8 MB (1)") != NULL);
    }
    for (i = 5; i < BLOCKS; i++) HeapFree(GetProcessHeap(), 0, g_blocks[i]);
    HeapFree(GetProcessHeap(), 0, grown);
    if (!g_failed) {
        puts("heap watch read: all checks pass");
    }
    return g_failed != 0;
}
