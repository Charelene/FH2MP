/* Traffic diagnostics (src/diag_trace.inc): the production installer against a fake game image, each detour's log line, that every detour
 * calls the native function unchanged, the refusal on other bytes, and the lines-per-second limit. The platform, the log and the five natives
 * are stand-ins; the offsets inside the traffic car record (+0x98 local authority, +0x9c vehicle id, +0xcc last request) come from the
 * decompile and are not verified here against a running game. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define FH2MP_DIAG_BUILD 1
#define MAX_LINES 256

static char g_lines[MAX_LINES][200];
static int g_nlines;
static char g_all[1024][200];          /* every line the detours wrote, for the parser check below */
static int g_nall;
static char g_raw[1024];
static unsigned char *g_img;
static void *g_hook_target[16], *g_hook_detour[16];
static int g_hooks, g_call_hooks;
static unsigned g_call_site;

static void grid_line(const char *text)
{
    if (g_nall < 1024) {
        strncpy(g_all[g_nall++], text, sizeof g_all[0] - 1);
    }
    if (g_nlines < MAX_LINES) {
        strncpy(g_lines[g_nlines], text, sizeof g_lines[0] - 1);
    }
    g_nlines++;
}
static void grid_utc(char *out) { strcpy(out, "12:00:00.000 UTC"); }
static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void mem_copy(void *d, const void *s, size_t n) { memcpy(d, s, n); }
static unsigned char *find_fh2_image(void) { return g_img; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag;
    if (!memcmp(img + rva, expect, n)) return 1;
    log_raw("mismatch: ");
    log_raw(what);
    log_raw("\n");
    return 0;
}

/* the natives, as the detours call them */
static int g_own_calls, g_req_calls, g_warp_calls, g_take_calls, g_drop_calls, g_denied_calls, g_req_send, g_clock;
static unsigned char g_last_id;
static void fake_own(void *car, unsigned char local) { g_own_calls++; ((unsigned char *)car)[0x98] = local; }
static void fake_req(void *car, void *a2, void *a3, void *a4)
{
    (void)a2; (void)a3; (void)a4;
    g_req_calls++;
    if (g_req_send) {
        *(int *)((unsigned char *)car + 0xcc) = g_clock;
        ((unsigned char *)car)[0x98] = 1;
    }
}
static void fake_take(void *m, unsigned char id, void *a, void *b) { (void)m; (void)a; (void)b; g_take_calls++; g_last_id = id; }
static void fake_drop(void *m, unsigned char id, void *a, void *b) { (void)m; (void)a; (void)b; g_drop_calls++; g_last_id = id; }
static void fake_denied(void *m, unsigned char id, void *a, void *b) { (void)m; (void)a; (void)b; g_denied_calls++; g_last_id = id; }
static void fake_warp(void *placement, char flag) { (void)placement; (void)flag; g_warp_calls++; }

static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    unsigned rva = (unsigned)((unsigned char *)target - g_img);
    (void)name;
    g_hook_target[g_hooks] = target;
    g_hook_detour[g_hooks++] = detour;
    *original = rva == 0x726570 ? (void *)fake_own : rva == 0x717410 ? (void *)fake_req : rva == 0x728e50 ? (void *)fake_take
              : rva == 0x71b180 ? (void *)fake_drop : rva == 0x723fa0 ? (void *)fake_denied : NULL;
    return *original != NULL;
}
static int plat_hook_call(const char *name, void *site, void *detour, void **original)
{
    (void)name; (void)detour;
    g_call_hooks++;
    g_call_site = (unsigned)((unsigned char *)site - g_img);
    *original = (void *)fake_warp;
    return 1;
}

#include "src/diag_trace.inc"

static int has(const char *line, const char *part) { return strstr(line, part) != NULL; }
static void wait_for_new_second(void)
{
    DWORD s = GetTickCount() / 1000;
    while (GetTickCount() / 1000 == s) Sleep(1);
}

int main(int argc, char **argv)
{
    static const unsigned char own_p[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x88, 0x91, 0x98, 0x00, 0x00, 0x00, 0xc6};
    static const unsigned char req_p[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48};
    static const unsigned char msg_p[] = {0x88, 0x54, 0x24, 0x10, 0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x83, 0xec, 0x60, 0x49, 0xc7, 0x43, 0xb8};
    static const unsigned char warp_before[] = {0x81, 0xc1, 0x20, 0x03, 0x00, 0x00, 0xb2, 0x01};
    unsigned char car[0x100];
    int rel, i, allowed;

    g_img = VirtualAlloc(NULL, 0x1900000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_img);

    /* other bytes: nothing is hooked and the log says which function did not match */
    assert(patch_diag_trace() == 0);
    assert(g_hooks == 0 && g_call_hooks == 0 && has(g_raw, "mismatch: traffic car authority setter"));

    memcpy(g_img + 0x726570, own_p, 16);
    memcpy(g_img + 0x717410, req_p, 16);
    memcpy(g_img + 0x728e50, msg_p, 16);
    memcpy(g_img + 0x71b180, msg_p, 16);
    memcpy(g_img + 0x723fa0, msg_p, 16);
    memcpy(g_img + 0x18ed98b - sizeof warp_before, warp_before, sizeof warp_before);
    /* the warp call must go to InterpolatePosition: another target is refused */
    g_img[0x18ed98b] = 0xe8;
    rel = 0x18e14e0 + 0x10 - (0x18ed98b + 5);
    memcpy(g_img + 0x18ed98b + 1, &rel, 4);
    g_raw[0] = 0;
    assert(patch_diag_trace() == 0 && g_hooks == 0 && has(g_raw, "not the call to VehiclePlacement::InterpolatePosition"));
    rel = 0x18e14e0 - (0x18ed98b + 5);
    memcpy(g_img + 0x18ed98b + 1, &rel, 4);

    g_raw[0] = 0;
    assert(patch_diag_trace() == 1);
    assert(g_hooks == 5 && g_call_hooks == 1 && g_call_site == 0x18ed98bu);
    assert(has(g_raw, "diag: traffic trace on"));

    /* authority: the line says the car, the change and the caller; the native still runs */
    memset(car, 0, sizeof car);
    car[0x9c] = 17;
    wait_for_new_second();
    g_nlines = 0;
    diag_authority(car, 1);
    assert(g_own_calls == 1 && car[0x98] == 1);
    assert(g_nlines == 1 && has(g_lines[0], "diag: 12:00:00.000 UTC authority car 17 local 0 -> 1 from +0x"));

    /* a request that went out (the stamp changed) is logged with the gap; one the native refused (2 s limit) is not */
    car[0x98] = 0;
    *(int *)(car + 0xcc) = 1000;
    g_clock = 3120;
    g_req_send = 1;
    g_nlines = 0;
    diag_request(car, NULL, NULL, NULL);
    assert(g_req_calls == 1 && g_nlines == 1);
    assert(has(g_lines[0], "request car 17 sent, 2120 ms after the last one, local was 0, now 1"));
    g_req_send = 0;
    diag_request(car, NULL, NULL, NULL);
    assert(g_req_calls == 2 && g_nlines == 1);

    /* the three server messages and the warp */
    diag_take(NULL, 5, NULL, NULL);
    diag_drop(NULL, 6, NULL, NULL);
    diag_denied(NULL, 7, NULL, NULL);
    assert(g_take_calls == 1 && g_drop_calls == 1 && g_denied_calls == 1 && g_last_id == 7);
    assert(has(g_lines[1], "server take car 5") && has(g_lines[2], "server drop car 6") && has(g_lines[3], "server denied car 7"));
    diag_warp((void *)((ULONG_PTR)0x1000000u + 0x320u), 1);
    assert(g_warp_calls == 1 && has(g_lines[4], "warp of a remote traffic car (object 0x1000000)"));

    /* at most DIAG_MAX_LINES_PER_S a second; the next second says how many it lost */
    wait_for_new_second();
    g_nlines = 0;
    allowed = 0;
    for (i = 0; i < 100; i++) allowed += diag_line_ok();
    assert(allowed == DIAG_MAX_LINES_PER_S);
    wait_for_new_second();
    g_nlines = 0;
    assert(diag_line_ok() == 1 && g_nlines == 1 && has(g_lines[0], "60 lines over the limit of 40 a second were not written"));

    if (argc > 1 && !strcmp(argv[1], "--lines")) {
        /* the lines as fh2mp.log has them: "[tick] " in front; tools/analysis/diag_report.py must read every kind */
        for (i = 0; i < g_nall; i++) printf("[%d] %s\n", i, g_all[i]);
        return 0;
    }
    printf("diag trace: ok\n");
    return 0;
}
