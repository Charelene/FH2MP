/* The slice watch of src/slice_watch.inc reading a buffer the way azimuth.exe 1.0.4 lays it out (offsets from its code: size +0x90, retire queue = std::deque
 * <Slice*> with map +0x118, map size +0x120, offset +0x128, size +0x130; slice list +0x150/+0x158 of 16-byte shared_ptrs; a slice's fence at +0x40; the owner's
 * passed fence at +0x14c0). Built objects only, no Azimuth: the expected values are the ones put into them. */
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

static unsigned char g_res[0x200], g_res2[0x200], g_owner[0x1500], g_slice[0x60];
static void *g_block[2], *g_map[4];
static unsigned char g_list[5 * 16];

static void put8(unsigned char *o, unsigned off, unsigned long long v) { memcpy(o + off, &v, 8); }

__attribute__((noinline)) static void discard_from_here(void *owner, void *res) { slice_watch_sample(owner, res, 0); }

int main(void)
{
    unsigned long long exe = (unsigned long long)(ULONG_PTR)GetModuleHandleW(NULL), unset = ~0ull;
    unsigned top[SLICE_WATCH_TOP], n;
    unsigned size = 4096;
    const slice_watch_res *r;

    setvbuf(stdout, NULL, _IONBF, 0);
    InitializeCriticalSection(&g_cs);
    /* the "game" image is this exe, so the first return address in it is found */
    g_sw_game_lo = exe;
    g_sw_game_hi = exe + 0x1000000;
    /* a 4 KB buffer: 5 slices held, 3 in the retire queue, the front one never stamped (fence ~0) */
    memcpy(g_res + 0x90, &size, 4);
    put8(g_res, 0x150, (unsigned long long)(ULONG_PTR)g_list);
    put8(g_res, 0x158, (unsigned long long)(ULONG_PTR)(g_list + sizeof g_list));
    memcpy(g_slice + 0x40, &unset, 8);
    g_block[1] = g_slice;                    /* offset 5 in a map of 4 blocks: block (5 / 2) & 3 = 2, place 5 & 1 = 1 */
    g_map[2] = g_block;
    put8(g_res, 0x118, (unsigned long long)(ULONG_PTR)g_map);
    put8(g_res, 0x120, 4);
    put8(g_res, 0x128, 5);
    put8(g_res, 0x130, 3);
    put8(g_owner, 0x14c0, 777);
    /* a second buffer whose map size is not a power of two: no front is read */
    put8(g_res2, 0x120, 6);
    put8(g_res2, 0x130, 2);

    InterlockedExchange(&g_sw_on, 1);
    discard_from_here(g_owner, g_res);
    discard_from_here(g_owner, g_res);
    discard_from_here(g_owner, g_res2);
    InterlockedExchange(&g_sw_on, 0);

    CHECK(g_sw.discards == 3);
    CHECK(g_sw.passed == 777);
    n = slice_watch_top(&g_sw, SLICE_WATCH_TOP, top);
    CHECK(n == 2);
    r = &g_sw.r[top[0]];
    CHECK(r->res == (unsigned long long)(ULONG_PTR)g_res);
    CHECK(r->size == 4096);
    CHECK(r->queue_max == 3);
    CHECK(r->list_max == 5);
    CHECK(r->front_ok && r->front_fence == ~0ull);
    CHECK(r->discards == 2);
    CHECK(r->caller != SLICE_WATCH_UNKNOWN && r->caller < 0x1000000);       /* a return address inside this exe */
    r = &g_sw.r[top[1]];
    CHECK(r->res == (unsigned long long)(ULONG_PTR)g_res2 && r->queue_max == 2 && !r->front_ok);
    /* arena blocks in the same window: 3 x 1 MB from a buffer's new slice, 1 x 8 MB from another call, no game frame for that one */
    slice_watch_alloc(&g_sw, 0x1c6552, 0x5b0b70, 1u << 20);
    slice_watch_alloc(&g_sw, 0x1c6552, 0x5b0b70, 1u << 20);
    slice_watch_alloc(&g_sw, 0x1c6552, 0x5b0b70, 1u << 20);
    slice_watch_alloc(&g_sw, 0x1c8ea6, SLICE_WATCH_UNKNOWN, 8u << 20);
    /* the lines */
    slice_watch_finish("test", 21ull << 30);
    CHECK(strstr(g_lines, "health: slice watch (test) over 5 s at commit 21504 MB: 3 buffer discards on 2 buffers, GPU fence passed 777;") != NULL);
    CHECK(strstr(g_lines, "(4096 bytes) queue 3, 5 slices held, front fence unset, 2 discards from game+0x") != NULL);
    CHECK(strstr(g_lines, "(0 bytes) queue 2, 0 slices held, front fence -, 1 discards from game+0x") != NULL);
    CHECK(strstr(g_lines, "health: slice watch: 4 buffer arena blocks, 11 MB asked for; by Azimuth call: azimuth+0x1c8ea6 (1 blocks, 8192 KB), "
                          "azimuth+0x1c6552 (3 blocks, 3072 KB); by game call: no game frame (1 blocks, 8192 KB), game+0x5b0b70 (3 blocks, 3072 KB)\n") != NULL);
    if (!g_failed) {
        puts("slice watch read: all checks pass");
    }
    return g_failed != 0;
}
