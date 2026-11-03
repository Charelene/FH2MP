/* Remote car slots (src/car_pool.inc) against a fake image holding the bytes of CarCacheModule::vfunc_2 as read from the exe on disk (1.0.0.64).
 * What the game does with a bigger group is not something this can say. */
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char g_raw[1024];
static unsigned char *g_img;
static void log_begin(void) {}
static void log_raw(const char *s) { strncat(g_raw, s, sizeof g_raw - strlen(g_raw) - 1); }
static void log_dec(unsigned long v) { char b[16]; wsprintfA(b, "%lu", v); log_raw(b); }
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
static int g_patches, g_hooks;
static unsigned long long fake_tick(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; return 7; }
static int read_game_memory(const void *a, void *out, SIZE_T n) { if (!a) return 0; memcpy(out, a, n); return 1; }
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name; (void)target; (void)detour;
    g_hooks++;
    *original = (void *)fake_tick;
    return 1;
}
static int plat_patch(const char *name, void *at, const void *expect, const void *bytes, unsigned n)
{
    (void)name;
    if (memcmp(at, expect, n)) return 0;
    memcpy(at, bytes, n);
    g_patches++;
    return 1;
}

#include "src/car_pool.inc"

/* CarCacheModule::vfunc_2 @ 0x47a570, first 0x62 bytes, from forza_durango_release_final.exe */
static const unsigned char VFUNC2[] = {
    0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7, 0x40, 0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
    0x48, 0x8b, 0xf9, 0xe8, 0xb0, 0xb3, 0xe1, 0xff, 0x8b, 0x90, 0x38, 0x25, 0x00, 0x00, 0x41, 0xb9, 0x0b, 0x00, 0x00, 0x00, 0x85, 0xd2, 0x44, 0x0f,
    0x49, 0xca, 0x44, 0x89, 0x8c, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xa7, 0x74, 0x9c, 0x01, 0x48, 0x8d, 0x84, 0x24, 0x80, 0x00, 0x00,
    0x00, 0x41, 0x83, 0xf9, 0x0c, 0x48, 0x0f, 0x4d, 0xc1, 0x8b, 0x18};

static const unsigned char TICK_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};

static void fresh(void)
{
    memset(g_img, 0, 0x4000000);
    memcpy(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2);
    memcpy(g_img + CAR_POOL_TICK_RVA, TICK_HEAD, sizeof TICK_HEAD);
    g_raw[0] = 0;
    g_patches = 0;
    g_hooks = 0;
}

int main(void)
{
    int i;
    g_img = VirtualAlloc(NULL, 0x4000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(g_img);

    /* the offsets in the .inc are the ones in the real bytes */
    assert(VFUNC2[0x26] == 0x41 && VFUNC2[0x27] == 0xb9 && VFUNC2[0x28] == 0x0b && VFUNC2[0x49] == 0x41 && VFUNC2[0x4c] == 0x0c && VFUNC2[0x4d] == 0x48);

    /* unset or out of range: nothing is touched */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    fresh();
    assert(patch_car_pool() == 0 && g_patches == 0 && !memcmp(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2));
    { static const char *const bad[] = {"11", "10", "76", "abc", "2x", "", "-4", "99999999", "0"};
      for (i = 0; i < 9; i++) {
          SetEnvironmentVariableA("FH2MP_CAR_SLOTS", bad[i]);
          fresh();
          assert(patch_car_pool() == 0 && g_patches == 0 && !memcmp(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2));
      } }

    /* 12 to 75: the 11 becomes the value, the clamp is nopped, nothing else moves */
    { static const char *const good[] = {"12", "40", "75"};
      static const int want[] = {12, 40, 75};
      for (i = 0; i < 3; i++) {
          unsigned char expect[sizeof VFUNC2];
          memcpy(expect, VFUNC2, sizeof VFUNC2);
          expect[0x28] = (unsigned char)want[i];
          memset(expect + 0x4d, 0x90, 4);
          SetEnvironmentVariableA("FH2MP_CAR_SLOTS", good[i]);
          fresh();
          assert(patch_car_pool() == 1 && g_patches == 2 && g_hooks == 1);
          assert(!memcmp(g_img + CAR_POOL_RVA, expect, sizeof expect));
          assert(strstr(g_raw, "remote car slots 11 -> "));
      } }

    /* other bytes (another build): refused and the log says which part */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
    fresh();
    g_img[CAR_POOL_RVA + 0x28] = 0x0c;
    assert(patch_car_pool() == 0 && g_patches == 0 && strstr(g_raw, "mismatch: the Non-LOD0 slot count"));
    fresh();
    g_img[CAR_POOL_RVA + 0x4c] = 0x10;
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && strstr(g_raw, "mismatch: the clamp to 12 slots"));
    fresh();
    g_img[CAR_POOL_RVA] = 0x40;
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && strstr(g_raw, "mismatch: the car cache's module init"));

    /* the diagnostic hook: runs the game's tick, logs the group state once and again only when it changes */
    { static unsigned char slots[100 * 0x590];
      static unsigned char request[0x5d0], request2[0x5d0], request3[0x5d0];
      unsigned char module[0x400], queue_obj[0x40];
      unsigned long long qptr = (unsigned long long)(ULONG_PTR)queue_obj;
      unsigned char *group = module + CAR_POOL_GROUP;
      fresh();
      memset(module, 0, sizeof module);
      memset(queue_obj, 0, sizeof queue_obj);
      memcpy(g_img + CAR_POOL_QUEUE_PTR_RVA, &qptr, 8);
      g_car_pool_tick_orig = (car_pool_tick_fn)fake_tick;
      g_car_pool_seen_any = 0;
      g_car_pool_scan_at = 0;
      *(unsigned long long *)(group + 0x60) = (unsigned long long)(ULONG_PTR)slots;
      *(unsigned long long *)(group + 0x68) = (unsigned long long)(ULONG_PTR)(slots + 74 * 0x590);
      *(unsigned *)(group + 0x90) = 74;
      *(unsigned *)(group + 0x94) = 74;
      assert(car_pool_tick(module, NULL, NULL, NULL) == 7);
      assert(strstr(g_raw, "slots 74, free 74, budget 74, queued requests 0, global load queue idle, cars 0, assigned 0, unloading 0, request state 1 0, state 2 0, ready flag 0"));
      g_raw[0] = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(g_raw[0] == 0);                                              /* nothing changed: no line */
      *(unsigned long long *)(group + 0x78) = 0x20000;
      *(unsigned long long *)(group + 0x80) = 0x20000 + 3 * 8;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "queued requests 3, global load queue idle"));
      g_raw[0] = 0;
      *(unsigned *)(queue_obj + 0x14) = 5;                                /* +0x14 non-zero: busy */
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "global load queue busy"));
      /* slot 0: car built, request assigned in state 1, not ready; slot 5: request in state 2 and ready; slot 9: a car being unloaded */
      *(unsigned long long *)(slots + 0x560) = 0x1111;
      *(unsigned long long *)(slots + 0x558) = (unsigned long long)(ULONG_PTR)request;
      *(unsigned *)(request + 0x598) = 1;
      *(unsigned long long *)(slots + 5 * 0x590 + 0x560) = 0x2222;
      *(unsigned long long *)(slots + 5 * 0x590 + 0x558) = (unsigned long long)(ULONG_PTR)request2;
      *(unsigned *)(request2 + 0x598) = 2;
      request2[0x5a8] = 1;
      *(unsigned long long *)(slots + 7 * 0x590 + 0x558) = (unsigned long long)(ULONG_PTR)request3;     /* a second request in state 1 */
      *(unsigned *)(request3 + 0x598) = 1;
      *(unsigned long long *)(slots + 9 * 0x590 + 0x568) = 0x3333;
      g_raw[0] = 0;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "cars 2, assigned 3, unloading 1, request state 1 2, state 2 1, ready flag 1"));
      /* a slot vector that is not whole slots, or bigger than the scan buffer, is not scanned */
      *(unsigned long long *)(group + 0x68) = (unsigned long long)(ULONG_PTR)(slots + 74 * 0x590 - 1);
      g_raw[0] = 0;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(g_raw[0] == 0 || !strstr(g_raw, "cars "));
      /* more slots than the scan buffer holds: the counts are left out */
      *(unsigned long long *)(group + 0x68) = (unsigned long long)(ULONG_PTR)(slots + 97 * 0x590);
      g_raw[0] = 0;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "slots 97,") && !strstr(g_raw, "cars "));
    }

    printf("car pool: ok\n");
    return 0;
}
