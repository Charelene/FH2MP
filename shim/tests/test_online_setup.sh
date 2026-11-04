#!/usr/bin/env bash
# The online-setup fixes (src/online_setup.inc): the sign-in event listener and the QoS measurement timeout, on a fake game image.
# Pure logic with the platform calls stubbed, so it runs natively without Wine.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef long LONG;
typedef size_t SIZE_T;
#define InterlockedIncrement(p) (++*(p))
static unsigned char *g_img;
static unsigned char *find_fh2_image(void) { return g_img; }
static void log_begin(void) {}
static void log_raw(const char *s) { (void)s; }
static void log_dec(unsigned long v) { (void)v; }
static void log_hex(unsigned long long v, int d) { (void)v; (void)d; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{
    (void)tag; (void)what;
    return memcmp(img + rva, expect, n) == 0;
}
static int read_game_memory(const void *a, void *out, SIZE_T n) { if (!a) return 0; memcpy(out, a, n); return 1; }
static int readable_game_range(const void *a, SIZE_T n) { (void)n; return a != NULL; }
typedef unsigned long DWORD;
static DWORD g_now = 123456;
static DWORD GetTickCount(void) { return g_now; }
static void *GetCurrentProcess(void) { return NULL; }
static int WriteProcessMemory(void *p, void *at, const void *src, SIZE_T n, SIZE_T *w) { (void)p; memcpy(at, src, n); *w = n; return 1; }
static void *g_hook_target, *g_hook_detour;
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name;
    g_hook_target = target;
    g_hook_detour = detour;
    *original = NULL;
    return 1;
}
static int g_patches;
static unsigned char *g_patch_at[4];
static int plat_patch(const char *name, void *at, const void *expect, const void *bytes, unsigned n)
{
    (void)name;
    if (memcmp(at, expect, n)) return 0;
    memcpy(at, bytes, n);
    g_patch_at[g_patches++] = at;
    return 1;
}
#include "online_setup.inc"

static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)
static int seen_event, seen_flag;
static char fake_original(void *self, void *category, int event, unsigned char *param)
{
    (void)self; (void)category;
    seen_event = event;
    seen_flag = param ? param[0x20] : -1;
    return 0;
}

static int sends;
static long fake_send(unsigned char *self, const char *svc, const char *method, void *a4, void *a5, void *a6, void *a7, void *a8, void *a9,
                      void *a10, void *a11, void *a12, void *a13)
{
    (void)self; (void)svc; (void)method; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; (void)a12; (void)a13;
    sends++;
    return 77;
}

int main(void)
{
    static const unsigned char signin[] = {0x41, 0x80, 0x79, 0x20, 0x00, 0x74, 0x44, 0x49, 0x8B, 0x49, 0x18};
    static const unsigned char head[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x49, 0x8B, 0xF9};
    static const unsigned char qos[] = {0xFF, 0x81, 0xD4, 0x00, 0x00, 0x00, 0xBB, 0x30, 0x75, 0x00, 0x00, 0xB8, 0x10, 0x27, 0x00, 0x00,
                                        0x83, 0xB9, 0xD4, 0x00, 0x00, 0x00, 0x01};
    unsigned char param[0x28];
    unsigned v;

    g_img = calloc(1, 0x3700000);
    if (!g_img) return 2;

    /* the listener: the game's code differs -> no hook */
    CHECK(!patch_signin_event());
    CHECK(g_hook_target == NULL);
    memcpy(g_img + FH2_MPS_SIGNIN_TEST_RVA, signin, sizeof signin);
    CHECK(!patch_signin_event());                       /* the function head still differs */
    memcpy(g_img + FH2_MPS_USER_EVENT_RVA, head, sizeof head);
    CHECK(patch_signin_event());
    CHECK(g_hook_target == g_img + FH2_MPS_USER_EVENT_RVA && g_hook_detour == (void *)mps_user_event);
    g_mps_user_event_original = fake_original;

    /* sign-in before the profile is the active one: the services see it as active, the event keeps its own byte */
    memset(param, 0, sizeof param);
    mps_user_event(NULL, (void *)4, 3, param);
    CHECK(seen_event == 3 && seen_flag == 1);
    CHECK(param[0x20] == 0);
    CHECK(g_signin_events == 1 && g_signin_inactive == 1);
    /* the game's own path is unchanged */
    param[0x20] = 1;
    mps_user_event(NULL, (void *)4, 3, param);
    CHECK(seen_flag == 1 && param[0x20] == 1);
    CHECK(g_signin_events == 2 && g_signin_inactive == 1);
    /* sign-out and the other events pass through untouched */
    param[0x20] = 0;
    mps_user_event(NULL, (void *)4, 4, param);
    CHECK(seen_event == 4 && seen_flag == 0 && g_signin_events == 2);
    mps_user_event(NULL, (void *)4, 3, NULL);
    CHECK(seen_flag == -1 && g_signin_events == 2);

    /* the QoS timeout: other code -> nothing; the expected code -> both immediates 1000; again -> already done */
    CHECK(!patch_qos_timeout());
    CHECK(g_patches == 0);
    memcpy(g_img + FH2_QOS_TIMEOUT_RVA, qos, sizeof qos);
    CHECK(patch_qos_timeout());
    CHECK(g_patches == 2);
    CHECK(g_patch_at[0] == g_img + 0x17b1b59 && g_patch_at[1] == g_img + 0x17b1b5e);
    memcpy(&v, g_img + 0x17b1b59, 4); CHECK(v == 1000);
    memcpy(&v, g_img + 0x17b1b5e, 4); CHECK(v == 1000);
    CHECK(g_img[0x17b1b58] == 0xBB && g_img[0x17b1b5d] == 0xB8 && g_img[0x17b1b62] == 0x83);
    CHECK(patch_qos_timeout());
    CHECK(g_patches == 2);

    /* the web transport watch: hooked only on the expected code; a call before the logon is counted (Live ones apart) and passed on */
    {
        static const unsigned char wg_head[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xF9};
        static const unsigned char wg_check[] = {0x41, 0x8B, 0x11, 0xE8, 0x6D, 0x02, 0x00, 0x00, 0x85, 0xC0};
        static unsigned char transport[0x200];
        void *logon = (void *)1;
        CHECK(!patch_webgate_watch());
        memcpy(g_img + FH2_WEBGATE_SEND_RVA, wg_head, sizeof wg_head);
        CHECK(!patch_webgate_watch());
        memcpy(g_img + FH2_WEBGATE_CHECK_RVA, wg_check, sizeof wg_check);
        CHECK(patch_webgate_watch());
        CHECK(g_hook_target == g_img + FH2_WEBGATE_SEND_RVA && g_hook_detour == (void *)webgate_send);
        g_webgate_send_original = fake_send;
        CHECK(webgate_send(transport, "Forza.WebServices.LiveThunderhead, Forza.WebServices", "GSDSRequest", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 77);
        CHECK(g_webgate_refused == 1 && g_webgate_refused_live == 1 && sends == 1);
        CHECK(webgate_send(transport, "Forza.WebServices.User, Forza.WebServices", "Logon", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) == 77);
        CHECK(g_webgate_refused == 2 && g_webgate_refused_live == 1 && sends == 2);
        memcpy(transport + 0x140, &logon, sizeof logon);
        webgate_send(transport, "Forza.WebServices.XboxMusic, Forza.WebServices", "GetUserPermissions", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        CHECK(g_webgate_refused == 2 && sends == 3);
    }

    /* the cloud configuration retry: only from state 1, retry time before state, the game's own state 4 */
    {
        unsigned char *mps = calloc(1, 0x200);
        int st;
        DWORD at;
        memcpy(g_img + FH2_MPS_SINGLETON_RVA, &mps, sizeof mps);
        st = 3; memcpy(mps + 0x78, &st, 4);
        CHECK(cloud_config_state(NULL) == 3);
        CHECK(!cloud_config_retry_now());
        memcpy(&st, mps + 0x78, 4); CHECK(st == 3);
        st = 1; memcpy(mps + 0x78, &st, 4);
        g_now = 999000;
        CHECK(cloud_config_retry_now());
        memcpy(&st, mps + 0x78, 4); CHECK(st == 4);
        memcpy(&at, mps + 0x70 + 0xa8, 4); CHECK(at == 999000);
        CHECK(!cloud_config_retry_now());               /* already 4: left alone */
        free(mps);
    }

    if (!failed) puts("online setup: all checks pass");
    free(g_img);
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wno-unused-function -I src -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
