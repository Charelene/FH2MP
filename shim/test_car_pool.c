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
static unsigned char g_fake_request[0x40];
static int *g_cur_type_ptr;
static int g_type_seen_in_find = -2;
static unsigned short g_ids_hi_seen;
static unsigned short g_train_key_seen;
static int g_num_ai_seen = 99, g_ai_calls;
static unsigned char *g_fake_options_for_ai;
static void *fake_train(void *m, void *out, unsigned short *key, void *e) { (void)m; (void)e; if (key) g_train_key_seen = *key; return out; }
static unsigned short fake_ids(void *m, unsigned short lo, unsigned short hi, void *p) { (void)m; (void)lo; (void)p; g_ids_hi_seen = hi; return 0xffff; }
static unsigned char *g_fake_options;
static unsigned char *fake_options(void) { return g_fake_options; }
static void fake_ai(void *m) { (void)m; g_ai_calls++; if (g_fake_options) g_num_ai_seen = *(int *)(g_fake_options + 0x2538); }
static void *fake_find(void *self, void *out, void *vehicle_ref, void *manager, void *extra, unsigned priority)
{
    unsigned char *vehicle = *(unsigned char **)vehicle_ref;
    (void)self; (void)manager; (void)extra; (void)priority;
    g_type_seen_in_find = *g_cur_type_ptr;
    if (!vehicle) {
        return out;
    }
    *(unsigned *)(g_fake_request + 0x20) = *(short *)(vehicle + 0x110 + 0x234) == -1 ? 12 : 1;
    *(short *)(vehicle + 0x110 + 0x234) = 1538;                  /* the id is stored back after the call */
    *(unsigned long long *)out = (unsigned long long)(ULONG_PTR)g_fake_request;
    return out;
}
static int read_game_memory(const void *a, void *out, SIZE_T n) { if (!a) return 0; memcpy(out, a, n); return 1; }
static int plat_hook(const char *name, void *target, void *detour, void **original)
{
    (void)name; (void)target; (void)detour;
    g_hooks++;
    *original = target == g_img + 0xfd08b0 ? (void *)fake_find : target == g_img + 0xfceee0 ? (void *)fake_ids : target == g_img + 0xf21870 ? (void *)fake_train : target == g_img + 0x720960 ? (void *)fake_ai : (void *)fake_tick;
    return 1;
}
static int g_refuse_patch;       /* the platform refuses the n-th patch call (1-based) */
static int g_patch_calls;
static int plat_patch(const char *name, void *at, const void *expect, const void *bytes, unsigned n)
{
    (void)name;
    g_patch_calls++;
    if (g_refuse_patch == g_patch_calls) return 0;
    if (memcmp(at, expect, n)) return 0;
    memcpy(at, bytes, n);
    g_patches++;
    return 1;
}

#include "src/car_pool.inc"

static void bind_cur_type(void) { g_cur_type_ptr = &g_car_pool_cur_type; }

/* CarCacheModule::vfunc_2 @ 0x47a570, first 0x62 bytes, from forza_durango_release_final.exe */
static const unsigned char VFUNC2[] = {
    0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7, 0x40, 0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
    0x48, 0x8b, 0xf9, 0xe8, 0xb0, 0xb3, 0xe1, 0xff, 0x8b, 0x90, 0x38, 0x25, 0x00, 0x00, 0x41, 0xb9, 0x0b, 0x00, 0x00, 0x00, 0x85, 0xd2, 0x44, 0x0f,
    0x49, 0xca, 0x44, 0x89, 0x8c, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xa7, 0x74, 0x9c, 0x01, 0x48, 0x8d, 0x84, 0x24, 0x80, 0x00, 0x00,
    0x00, 0x41, 0x83, 0xf9, 0x0c, 0x48, 0x0f, 0x4d, 0xc1, 0x8b, 0x18};

static const unsigned char TICK_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};

static const unsigned char MAX_SETTER[] = {0x44, 0x89, 0x82, 0xf0, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc};
static const unsigned char MIN_SETTER[] = {0x44, 0x89, 0x82, 0xb0, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc};
static const unsigned char AI_HEAD[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc9, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00};
static const unsigned char TRAIN_HEAD[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40};
static const unsigned char IDS_HEAD[] = {0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7, 0x40, 0xc8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x58, 0x10};
static const unsigned char FIND_HEAD[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60};

static void fresh(void)
{
    memset(g_img, 0, 0x4000000);
    memcpy(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2);
    memcpy(g_img + CAR_POOL_TICK_RVA, TICK_HEAD, sizeof TICK_HEAD);
    memcpy(g_img + CAR_POOL_FIND_RVA, FIND_HEAD, sizeof FIND_HEAD);
    memcpy(g_img + CAR_POOL_IDS_RVA, IDS_HEAD, sizeof IDS_HEAD);
    memcpy(g_img + CAR_POOL_TRAIN_RVA, TRAIN_HEAD, sizeof TRAIN_HEAD);
    memcpy(g_img + 0x5430d0, MAX_SETTER, sizeof MAX_SETTER);
    memcpy(g_img + 0x653780, MIN_SETTER, sizeof MIN_SETTER);
    memcpy(g_img + 0x720960, AI_HEAD, sizeof AI_HEAD);
    g_car_pool_ids_hi = 0;
    g_raw[0] = 0;
    g_patches = 0;
    g_hooks = 0;
    g_patch_calls = 0;
    g_refuse_patch = 0;
}

int main(void)
{
    int i;
    bind_cur_type();
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

    /* FH2MP_CAR_DIAG=1 alone: the passive hook only, no patch */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_DIAG", "1");
    fresh();
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 2 && !memcmp(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2));
    g_img[CAR_POOL_TICK_RVA] = 0x40;                                      /* another build: no hook */
    fresh();
    g_img[CAR_POOL_TICK_RVA] = 0x40;
    assert(patch_car_pool() == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_DIAG", "0");
    fresh();
    assert(patch_car_pool() == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_DIAG", NULL);

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
          assert(patch_car_pool() == 1 && g_patches == 2 && g_hooks == 2 && g_car_pool_ids_hi == 0);
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
      static unsigned char handle1[0x40], handle2[0x40];
      static unsigned char vehicle1[0x700], vehicle2[0x700];
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
      *(unsigned long long *)(slots + 0x560) = (unsigned long long)(ULONG_PTR)handle1;
      *(unsigned *)(handle1 + 0x20) = 3;                                  /* load state 3: not loaded */
      *(unsigned long long *)(slots + 0x558) = (unsigned long long)(ULONG_PTR)request;
      *(unsigned *)(request + 0x598) = 1;
      *(unsigned long long *)(request + 0x568) = (unsigned long long)(ULONG_PTR)vehicle1;
      *(short *)(vehicle1 + 0x110 + 0x234) = -1;                          /* no id: the pool is asked */
      *(short *)(vehicle1 + 0x110 + 0x236) = 5;
      *(int *)(vehicle1 + 0x110 + 0x488) = 3;
      *(unsigned long long *)(slots + 5 * 0x590 + 0x560) = (unsigned long long)(ULONG_PTR)handle2;
      *(unsigned *)(handle2 + 0x20) = 7;                                  /* load state 7: loaded */
      *(unsigned long long *)(slots + 5 * 0x590 + 0x558) = (unsigned long long)(ULONG_PTR)request2;
      *(unsigned *)(request2 + 0x598) = 2;
      *(unsigned long long *)(request2 + 0x568) = (unsigned long long)(ULONG_PTR)vehicle2;
      *(short *)(vehicle2 + 0x110 + 0x234) = 1399;
      *(short *)(vehicle2 + 0x110 + 0x236) = 6;
      *(int *)(vehicle2 + 0x110 + 0x488) = 2;
      request2[0x5a8] = 1;
      *(unsigned long long *)(slots + 7 * 0x590 + 0x558) = (unsigned long long)(ULONG_PTR)request3;     /* a second request in state 1 */
      *(unsigned *)(request3 + 0x598) = 1;
      *(unsigned long long *)(slots + 9 * 0x590 + 0x568) = 0x3333;
      g_raw[0] = 0;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "cars 2, assigned 3, unloading 1, request state 1 2, state 2 1, ready flag 1, car load states 3=1 7=1"));
      assert(strstr(g_raw, "carpool: slot 0 vehicle id 65535/5 type 3, request state 1, load state 3\n"));
      assert(strstr(g_raw, "carpool: slot 5 vehicle id 1399/6 type 2, request state 2, load state 7\n"));
      /* the same values again: no new vehicle line */
      g_raw[0] = 0;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(!strstr(g_raw, "vehicle id"));
      /* a changed id is logged once */
      *(short *)(vehicle1 + 0x110 + 0x234) = 4;
      g_car_pool_scan_at = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(strstr(g_raw, "carpool: slot 0 vehicle id 4/5 type 3, request state 1, load state 3\n"));
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

    /* the load request log: the vehicle's id and type before, the request's state and the id after */
    { static unsigned char vehicle[0x700];
      unsigned char *vehicle_ref = vehicle;
      unsigned long long out = 0;
      fresh();
      g_car_pool_find_orig = (car_pool_find_fn)fake_find;
      g_car_pool_find_lines = 0;
      *(short *)(vehicle + 0x110 + 0x234) = -1;
      *(int *)(vehicle + 0x110 + 0x488) = 15;
      assert(car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1) == &out);
      assert(strstr(g_raw, "carpool: load request for vehicle id 65535 type 15 -> state 12, id then 1538\n"));
      g_raw[0] = 0;
      *(short *)(vehicle + 0x110 + 0x234) = 1538;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(strstr(g_raw, "vehicle id 1538 type 15 -> state 1, id then 1538\n"));
      /* no vehicle behind the reference: the game's function still runs, nothing is logged */
      g_raw[0] = 0;
      { unsigned char *none = NULL;
        car_pool_find(NULL, &out, &none, NULL, NULL, 1);
        assert(g_raw[0] == 0); }
      /* the line budget */
      g_car_pool_find_lines = 300;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(g_raw[0] == 0);
    }
    /* another build of the request builder: no hook on it, the cache hook stays */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_DIAG", "1");
    fresh();
    g_img[CAR_POOL_FIND_RVA] = 0x40;
    assert(patch_car_pool() == 0 && g_hooks == 1);
    SetEnvironmentVariableA("FH2MP_CAR_DIAG", NULL);

    /* the platform refuses one of the two patches: not reported as done, no hooks, no "slots" line */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
    for (i = 1; i <= 2; i++) {
        fresh();
        g_refuse_patch = i;
        assert(patch_car_pool() == 0 && g_hooks == 0 && !strstr(g_raw, "remote car slots 11 ->"));
    }
    fresh();
    assert(patch_car_pool() == 1 && g_hooks == 2 && strstr(g_raw, "remote car slots 11 -> 30 (FH2MP_CAR_SLOTS)"));

    /* FH2MP_CAR_NOAI=1: NumAICars (options+0x2538) is set to 0 once, from the first tick */
    { static unsigned char options[0x3000];
      static unsigned char module[0x400];
      SetEnvironmentVariableA("FH2MP_CAR_NOAI", "1");
      fresh();
      g_car_pool_options = fake_options;
      g_car_pool_no_ai_done = 0;
      g_car_pool_tick_orig = (car_pool_tick_fn)fake_tick;
      memset(options, 0, sizeof options);
      *(int *)(options + 0x2538) = -1;
      g_fake_options = options;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(*(int *)(options + 0x2538) == 0 && strstr(g_raw, "carpool: NumAICars 4294967295 -> 0 (FH2MP_CAR_NOAI)"));
      /* once only: a later value is left alone */
      *(int *)(options + 0x2538) = 5;
      g_raw[0] = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(*(int *)(options + 0x2538) == 5 && !strstr(g_raw, "NumAICars"));
      /* unset or another value: untouched */
      SetEnvironmentVariableA("FH2MP_CAR_NOAI", "0");
      g_car_pool_no_ai_done = 0;
      *(int *)(options + 0x2538) = -1;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(*(int *)(options + 0x2538) == -1);
      SetEnvironmentVariableA("FH2MP_CAR_NOAI", NULL);
      g_car_pool_no_ai_done = 0;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(*(int *)(options + 0x2538) == -1);
      /* the options singleton unreadable: nothing is written */
      SetEnvironmentVariableA("FH2MP_CAR_NOAI", "1");
      g_car_pool_no_ai_done = 0;
      g_fake_options = NULL;
      car_pool_tick(module, NULL, NULL, NULL);
      assert(*(int *)(options + 0x2538) == -1);
      SetEnvironmentVariableA("FH2MP_CAR_NOAI", NULL);
      g_car_pool_options = NULL;
    }

    /* the id search hook: only a type 3 vehicle's default range 0..11 is raised */
    { static unsigned char vehicle[0x700];
      unsigned char *vehicle_ref = vehicle;
      unsigned long long out = 0;
      fresh();
      g_car_pool_ids_orig = (car_pool_ids_fn)fake_ids;
      g_car_pool_find_orig = (car_pool_find_fn)fake_find;
      g_car_pool_ids_hi = 40;
      g_car_pool_cur_type = 3;
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff && g_ids_hi_seen == 40);
      g_car_pool_cur_type = 1;                                                /* a Drivatar AI car: its table has 12 entries */
      car_pool_ids(NULL, 0, 0x0b, NULL);
      assert(g_ids_hi_seen == 0x0b);
      g_car_pool_cur_type = 0;
      car_pool_ids(NULL, 0, 0x0b, NULL);
      assert(g_ids_hi_seen == 0x0b);
      g_car_pool_cur_type = 4;
      car_pool_ids(NULL, 0, 0x0b, NULL);
      assert(g_ids_hi_seen == 0x0b);
      g_car_pool_cur_type = 3;                                                /* another range (AI 12..29, the 0..29 default): untouched */
      car_pool_ids(NULL, 0x0c, 0x1d, NULL);
      assert(g_ids_hi_seen == 0x1d);
      car_pool_ids(NULL, 0, 0x1d, NULL);
      assert(g_ids_hi_seen == 0x1d);
      g_car_pool_cur_type = -1;                                               /* outside FindOrQueue */
      car_pool_ids(NULL, 0, 0x0b, NULL);
      assert(g_ids_hi_seen == 0x0b);
      g_car_pool_cur_type = 3;
      g_car_pool_ids_hi = 0;                                                  /* not configured */
      car_pool_ids(NULL, 0, 0x0b, NULL);
      assert(g_ids_hi_seen == 0x0b);
      /* FindOrQueue's wrapper tells the id search which type it is working on, and forgets it afterwards */
      *(short *)(vehicle + 0x110 + 0x234) = -1;
      *(int *)(vehicle + 0x110 + 0x488) = 3;
      g_type_seen_in_find = -2;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(g_type_seen_in_find == 3 && g_car_pool_cur_type == -1);
      g_car_pool_find_lines = 300;                                            /* the log budget does not stop the type from being known */
      g_type_seen_in_find = -2;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(g_type_seen_in_find == 3 && g_car_pool_cur_type == -1);
      g_car_pool_find_lines = 0;
    }
    /* the AI training manager's request: a key past the table is mapped into it for the call and put back */
    { unsigned short key = 27, small = 5, edge = 12, last = 11;
      void *out = &key;
      g_car_pool_train_orig = (car_pool_train_fn)fake_train;
      assert(car_pool_train(NULL, out, &key, NULL) == out && g_train_key_seen == 3 && key == 27);       /* 27 % 12 */
      car_pool_train(NULL, out, &small, NULL);
      assert(g_train_key_seen == 5 && small == 5);
      car_pool_train(NULL, out, &edge, NULL);
      assert(g_train_key_seen == 0 && edge == 12);
      car_pool_train(NULL, out, &last, NULL);
      assert(g_train_key_seen == 11 && last == 11);
      key = 74;
      car_pool_train(NULL, out, &key, NULL);
      assert(g_train_key_seen == 2 && key == 74);
      assert(car_pool_train(NULL, out, NULL, NULL) == out);                                              /* no key: passed through */
    }
    /* another build of the id search or the request builder: nothing is patched (a bigger cache with 12 ids makes every car invalid) */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
    SetEnvironmentVariableA("FH2MP_CAR_IDS", "1");
    fresh();
    g_img[CAR_POOL_IDS_RVA] = 0x40;
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the stand-in id search"));
    fresh();
    g_img[CAR_POOL_FIND_RVA] = 0x40;
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the net vehicle load request builder"));
    fresh();
    g_img[CAR_POOL_TRAIN_RVA] = 0x40;
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the AI training manager's request"));
    fresh();
    g_img[CAR_POOL_TICK_RVA] = 0x40;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_IDS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);

    /* FH2MP_CAR_IDS=1 (experimental): the training and id hooks too, the ids raised */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "40");
    SetEnvironmentVariableA("FH2MP_CAR_IDS", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 2 && g_hooks == 4 && g_car_pool_ids_hi == 40 && strstr(g_raw, "player car stand-in ids raised"));
    SetEnvironmentVariableA("FH2MP_CAR_IDS", NULL);

    /* FH2MP_CAR_NODRIVATARS=1: the slot count ignores NumAICars, both Drivatar setters store 0, the festival AI connect is hooked */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "14");
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 5 && g_hooks == 3 && g_car_pool_ids_hi == 0 && strstr(g_raw, "remote car slots 11 -> 14, no Drivatars (FH2MP_CAR_SLOTS)"));
    assert(g_img[CAR_POOL_RVA + 0x28] == 14);
    assert(!memcmp(g_img + CAR_POOL_RVA + 0x2e, "\x90\x90\x90\x90", 4) && !memcmp(g_img + CAR_POOL_RVA + 0x2c, VFUNC2 + 0x2c, 2));
    assert(!memcmp(g_img + 0x5430d0, "\xc7\x82\xf0\x00\x00\x00\x00\x00\x00\x00\xc3", 11));
    assert(!memcmp(g_img + 0x653780, "\xc7\x82\xb0\x00\x00\x00\x00\x00\x00\x00\xc3", 11));
    /* any of the new sites different (another build): nothing at all is patched, the cache included */
    { static const unsigned rvas[] = {0x5430d0, 0x653780, 0x720960};
      static const char *const what[] = {"the Drivatar maximum setter", "the Drivatar minimum setter", "the festival AI module's connect"};
      for (i = 0; i < 3; i++) {
          fresh();
          g_img[rvas[i]] ^= 0xff;
          g_raw[0] = 0;
          assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, what[i]) && !strstr(g_raw, "remote car slots 11 ->"));
      } }
    /* the platform refuses any one of the five patches: no hooks, not reported as done */
    for (i = 1; i <= 5; i++) {
        fresh();
        g_refuse_patch = i;
        assert(patch_car_pool() == 0 && g_hooks == 0 && !strstr(g_raw, "remote car slots 11 ->"));
    }
    /* the festival AI connect runs with NumAICars at 0 and puts the value back */
    { static unsigned char options[0x3000];
      static unsigned char module[8];
      g_car_pool_options = fake_options;
      g_fake_options = options;
      g_car_pool_ai_orig = (car_pool_ai_fn)fake_ai;
      *(int *)(options + 0x2538) = -1;
      car_pool_ai_connect(module);
      assert(g_num_ai_seen == 0 && *(int *)(options + 0x2538) == -1);
      *(int *)(options + 0x2538) = 7;
      car_pool_ai_connect(module);
      assert(g_num_ai_seen == 0 && *(int *)(options + 0x2538) == 7);
      g_fake_options = NULL;                                                 /* options unreadable: the game's function still runs */
      g_num_ai_seen = 99;
      car_pool_ai_connect(module);
      assert(g_num_ai_seen == 99 && g_ai_calls == 3);
      g_car_pool_options = NULL;
    }
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);

    printf("car pool: ok\n");
    return 0;
}
