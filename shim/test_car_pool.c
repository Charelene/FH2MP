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
static int g_patches;
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

static void fresh(void)
{
    memset(g_img, 0, 0x1000000);
    memcpy(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2);
    g_raw[0] = 0;
    g_patches = 0;
}

int main(void)
{
    int i;
    g_img = VirtualAlloc(NULL, 0x1000000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
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
          assert(patch_car_pool() == 1 && g_patches == 2);
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

    printf("car pool: ok\n");
    return 0;
}
