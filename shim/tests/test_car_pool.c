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
static void log_hex(unsigned long long v, int digits) { char b[24], f[8]; wsprintfA(f, "%%0%dlx", digits); wsprintfA(b, f, (unsigned long)v); log_raw(b); }
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
static ULONGLONG g_now;
static ULONGLONG fake_clock(void) { return g_now; }
static int g_wait_answer;
static unsigned long long car_wait(void *module);
/* the game asks every frame: call it every 500 ms of the fake clock for ms, return the first yes or the last answer */
static unsigned long long wait_for(void *module, unsigned ms)
{
    unsigned long long r = 0;
    unsigned t;
    for (t = 0; t < ms && !r; t += 500) { g_now += 500; r = car_wait(module); }
    return r;
}
static unsigned long long fake_wait(void *m) { (void)m; return (unsigned long long)g_wait_answer; }
static unsigned char g_fake_request[0x40];
static int *g_cur_type_ptr;
static int g_type_seen_in_find = -2;
static unsigned short g_ids_result = 0xffff;
static void *g_alloc_result;
static int g_view_a_calls, g_view_b_calls;
static void fake_view_a(void *v) { (void)v; g_view_a_calls++; }
static void fake_view_b(void *v) { (void)v; g_view_b_calls++; }
static unsigned char *g_fake_view_made;                       /* what the game's view maker hands back (NULL: it failed) */
static void *fake_view_new(void *presentation) { (void)presentation; return g_fake_view_made; }
static int g_label_take_calls, g_labels_make_calls;
static void **fake_label_take(void *self, void **out) { (void)self; g_label_take_calls++; out[0] = (void *)(ULONG_PTR)0x1234; out[1] = (void *)(ULONG_PTR)0x5678; return out; }
static void fake_labels_make(void *module) { g_labels_make_calls++; *(unsigned long long *)((unsigned char *)module + 0x68) = 77; }
static void *fake_alloc(void *m, unsigned long long size, unsigned short tag, unsigned long long *p) { (void)m; (void)size; (void)tag; (void)p; return g_alloc_result; }
static unsigned short fake_ids(void *m, unsigned short lo, unsigned short hi, void *p);
static void *fake_find_sharing(void *self, void *out, void *vehicle_ref, void *manager, void *extra, unsigned priority);
static int g_aictl_calls, g_fake_is_local_result = 1;
static void fake_aictl(void *car) { (void)car; g_aictl_calls++; }
static unsigned char fake_is_local(void *nc) { (void)nc; return (unsigned char)g_fake_is_local_result; }
static int g_num_ai_seen = 99, g_ai_calls;
static unsigned short fake_ids(void *m, unsigned short lo, unsigned short hi, void *p) { (void)m; (void)lo; (void)hi; (void)p; return g_ids_result; }
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
    *original = target == g_img + 0xfd08b0 ? (void *)fake_find : target == g_img + 0xfceee0 ? (void *)fake_ids : target == g_img + 0x720960 ? (void *)fake_ai : target == g_img + 0xf73e10 ? (void *)fake_aictl : target == g_img + 0x125d850 ? (void *)fake_alloc : target == g_img + 0x1355ad0 ? (void *)fake_view_a : target == g_img + 0x135b640 ? (void *)fake_view_b : target == g_img + 0x1302160 ? (void *)fake_view_new : target == g_img + 0x46df80 ? (void *)fake_label_take : target == g_img + 0x47a830 ? (void *)fake_labels_make : target == g_img + 0x7cbf10 ? (void *)fake_wait : (void *)fake_tick;
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

static int g_reports; static unsigned char g_report_code; static char g_report_text[256];
static int carsel_report_post(unsigned char code, unsigned detail, const char *text) { (void)detail; g_reports++; g_report_code = code; strncpy(g_report_text, text, sizeof g_report_text - 1); return 1; }

#include "src/car_pool.inc"

static void bind_cur_type(void) { g_cur_type_ptr = &g_car_pool_cur_type; }

/* FindOrQueue as far as the wrapper is concerned: it runs the id search for the vehicle's type */
static void *fake_find_sharing(void *self, void *out, void *vehicle_ref, void *manager, void *extra, unsigned priority)
{
    (void)self; (void)vehicle_ref; (void)manager; (void)extra; (void)priority;
    car_pool_ids(NULL, 0, 0x0b, NULL);
    return out;
}

/* CarCacheModule::vfunc_2 @ 0x47a570, first 0x62 bytes, from forza_durango_release_final.exe */
static const unsigned char VFUNC2[] = {
    0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7, 0x40, 0xb8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18,
    0x48, 0x8b, 0xf9, 0xe8, 0xb0, 0xb3, 0xe1, 0xff, 0x8b, 0x90, 0x38, 0x25, 0x00, 0x00, 0x41, 0xb9, 0x0b, 0x00, 0x00, 0x00, 0x85, 0xd2, 0x44, 0x0f,
    0x49, 0xca, 0x44, 0x89, 0x8c, 0x24, 0x80, 0x00, 0x00, 0x00, 0x48, 0x8d, 0x0d, 0xa7, 0x74, 0x9c, 0x01, 0x48, 0x8d, 0x84, 0x24, 0x80, 0x00, 0x00,
    0x00, 0x41, 0x83, 0xf9, 0x0c, 0x48, 0x0f, 0x4d, 0xc1, 0x8b, 0x18};

static const unsigned char TICK_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};

static const unsigned char WAIT_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x81, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9, 0x48, 0x8b, 0x18, 0x48};
static const unsigned char MAX_SETTER[] = {0x44, 0x89, 0x82, 0xf0, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc};
static const unsigned char MIN_SETTER[] = {0x44, 0x89, 0x82, 0xb0, 0x00, 0x00, 0x00, 0xc3, 0xcc, 0xcc, 0xcc};
static const unsigned char AI_HEAD[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc9, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00};
static const unsigned char BUDGET_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20};
static const unsigned char ALLOC_HEAD[] = {0x4c, 0x8b, 0xdc, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x60, 0x02, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x58};
static const unsigned char VIEW_HEAD[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x5c, 0x24, 0x48};
static const unsigned char LABELS_HEAD[] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x20};
static const unsigned char LABELS_CMP[] = {0xff, 0xc7, 0x83, 0xff, 0x0c, 0x0f, 0x82};
static const unsigned char AICTL_HEAD[] = {0x4c, 0x8b, 0xdc, 0x57, 0x48, 0x81, 0xec, 0x50, 0x03, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24, 0x40, 0xfe, 0xff, 0xff, 0xff};
static const unsigned char IDS_HEAD[] = {0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7, 0x40, 0xc8, 0xfe, 0xff, 0xff, 0xff, 0x48, 0x89, 0x58, 0x10};
static const unsigned char FIND_HEAD[] = {0x4c, 0x89, 0x4c, 0x24, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x60};

/* the bytes of CPresentation::vfunc_19 (game+0x1302160, from the exe on disk) and of the LOD table accesses in CPresentationView::vfunc_42 (from the TTD trace) */
static const unsigned char VIEWNEW_HEAD[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xf9, 0xc6, 0x81, 0xa8, 0x26, 0x00, 0x00, 0x01, 0xb9, 0xf0, 0x4d, 0x02, 0x00};
static const unsigned char LABEL_TAKE_HEAD[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff,
                                                0x48, 0x89, 0x5c, 0x24, 0x50, 0x48, 0x89, 0x6c, 0x24, 0x60, 0x48, 0x8b, 0xfa, 0x48, 0x8b, 0xe9, 0x33, 0xc0, 0x89, 0x44, 0x24, 0x20, 0x48, 0x8b, 0x59, 0x60};
static const unsigned char LOD_CLEAR[] = {0x48, 0x8d, 0x8e, 0x54, 0x04, 0x00, 0x00, 0x33, 0xd2, 0x41, 0xb8, 0x80, 0x00, 0x00, 0x00};
static const unsigned char LOD_STORE_A[] = {0x46, 0x89, 0xb4, 0xae, 0x54, 0x04, 0x00, 0x00};
static const unsigned char LOD_STORE_B[] = {0x42, 0xc7, 0x84, 0xae, 0x54, 0x04, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00};
static const unsigned char LOD_STORE_C[] = {0x42, 0x89, 0x84, 0xae, 0x54, 0x04, 0x00, 0x00};
static const unsigned char LOD_LOAD[] = {0x42, 0x8b, 0x84, 0xae, 0x54, 0x04, 0x00, 0x00};

static void fresh(void)
{
    memset(g_img, 0, 0x4000000);
    memcpy(g_img + 0x1302160, VIEWNEW_HEAD, sizeof VIEWNEW_HEAD);
    memcpy(g_img + 0x46df80, LABEL_TAKE_HEAD, sizeof LABEL_TAKE_HEAD);
    memcpy(g_img + 0x1346332, LOD_CLEAR, sizeof LOD_CLEAR);
    memcpy(g_img + 0x1346639, LOD_STORE_A, sizeof LOD_STORE_A);
    memcpy(g_img + 0x1346649, LOD_STORE_B, sizeof LOD_STORE_B);
    memcpy(g_img + 0x1346664, LOD_STORE_C, sizeof LOD_STORE_C);
    memcpy(g_img + 0x134666c, LOD_LOAD, sizeof LOD_LOAD);
    memcpy(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2);
    memcpy(g_img + CAR_POOL_TICK_RVA, TICK_HEAD, sizeof TICK_HEAD);
    memcpy(g_img + CAR_WAIT_RVA, WAIT_HEAD, sizeof WAIT_HEAD);
    g_car_pool_tick_orig = NULL;
    g_car_wait_orig = NULL;
    g_car_cache_self = NULL;
    g_reports = 0;
    g_car_wait_reported = 0;
    memcpy(g_img + CAR_POOL_FIND_RVA, FIND_HEAD, sizeof FIND_HEAD);
    memcpy(g_img + CAR_POOL_IDS_RVA, IDS_HEAD, sizeof IDS_HEAD);
    memcpy(g_img + CAR_POOL_AICTL_RVA, AICTL_HEAD, sizeof AICTL_HEAD);
    memcpy(g_img + CAR_POOL_LABELS_RVA, LABELS_HEAD, sizeof LABELS_HEAD);
    memcpy(g_img + CAR_POOL_BUDGET_RVA, BUDGET_HEAD, sizeof BUDGET_HEAD);
    memcpy(g_img + CAR_POOL_ALLOC_RVA, ALLOC_HEAD, sizeof ALLOC_HEAD);
    memcpy(g_img + CAR_POOL_VIEW_A_RVA, VIEW_HEAD, sizeof VIEW_HEAD);
    memcpy(g_img + CAR_POOL_VIEW_B_RVA, VIEW_HEAD, sizeof VIEW_HEAD);
    memcpy(g_img + CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP, LABELS_CMP, sizeof LABELS_CMP);
    memcpy(g_img + 0x5430d0, MAX_SETTER, sizeof MAX_SETTER);
    memcpy(g_img + 0x653780, MIN_SETTER, sizeof MIN_SETTER);
    memcpy(g_img + 0x720960, AI_HEAD, sizeof AI_HEAD);
    g_car_pool_share_ids = 0;
    g_car_pool_share_next = 0;
    g_raw[0] = 0;
    g_patches = 0;
    g_hooks = 0;
    g_patch_calls = 0;
    g_refuse_patch = 0;
}

int main(void)
{
    int i;
    SetEnvironmentVariableA("FH2MP_NO_CARSLOTS", "1");                 /* the cases below are about the switches one by one; the default profile has its own */
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
          assert(patch_car_pool() == 1 && g_patches == 3 && g_hooks == 4 && g_car_pool_share_ids == 0);
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

    /* the platform refuses one of the three patches: not reported as done, no hooks, no "slots" line */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
    for (i = 1; i <= 3; i++) {
        fresh();
        g_refuse_patch = i;
        assert(patch_car_pool() == 0 && g_hooks == 0 && !strstr(g_raw, "remote car slots 11 ->"));
    }
    fresh();
    assert(patch_car_pool() == 1 && g_hooks == 4 && strstr(g_raw, "remote car slots 11 -> 30 (FH2MP_CAR_SLOTS)"));

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

    /* the id search hook: a player car (type 3) that finds the range 0..11 full shares an id in 1..11; nothing else changes */
    { static unsigned char vehicle[0x700];
      unsigned char *vehicle_ref = vehicle;
      unsigned long long out = 0;
      int k;
      fresh();
      g_car_pool_ids_orig = (car_pool_ids_fn)fake_ids;
      g_car_pool_find_orig = (car_pool_find_fn)fake_find;
      g_car_pool_share_ids = 1;
      g_car_pool_cur_type = 3;
      g_ids_result = 0xffff;
      for (k = 0; k < 24; k++) {                                              /* in turn: 1..11, then round again */
          assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 1 + k % 11);
      }
      assert(strstr(g_raw, "player car shares id 1 (FH2MP_CAR_IDS)") && strstr(g_raw, "shares id 5 ") && !strstr(g_raw, "shares id 6 "));    /* five lines at most */
      g_ids_result = 7;                                                       /* an id was free: used as it is */
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 7);
      g_ids_result = 0xffff;
      g_car_pool_cur_type = 1;                                                /* a Drivatar: the range stays full for it */
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff);
      g_car_pool_cur_type = 0;
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff);
      g_car_pool_cur_type = 4;
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff);
      g_car_pool_cur_type = 3;                                                /* other ranges: the AI 12..29, the 0..29 default */
      assert(car_pool_ids(NULL, 0x0c, 0x1d, NULL) == 0xffff);
      assert(car_pool_ids(NULL, 0, 0x1d, NULL) == 0xffff);
      g_car_pool_cur_type = -1;                                               /* outside FindOrQueue */
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff);
      g_car_pool_cur_type = 3;
      g_car_pool_share_ids = 0;                                               /* not configured */
      assert(car_pool_ids(NULL, 0, 0x0b, NULL) == 0xffff);
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
    /* a shared id gets its own second half in the car's key (+0x236), an ordinary one is left alone */
    { static unsigned char vehicle[0x700];
      unsigned char *vehicle_ref = vehicle;
      unsigned long long out = 0;
      fresh();
      g_car_pool_ids_orig = (car_pool_ids_fn)fake_ids;
      g_car_pool_find_orig = (car_pool_find_fn)fake_find_sharing;
      g_car_pool_share_ids = 1;
      g_ids_result = 0xffff;
      *(int *)(vehicle + 0x110 + 0x488) = 3;
      *(short *)(vehicle + 0x110 + 0x236) = 0;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(*(unsigned short *)(vehicle + 0x110 + 0x236) == 0x4000 + 1);
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(*(unsigned short *)(vehicle + 0x110 + 0x236) == 0x4000 + 2);        /* unique for each */
      g_ids_result = 5;                                                           /* an id was free: nothing shared, id2 untouched */
      *(short *)(vehicle + 0x110 + 0x236) = 77;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(*(short *)(vehicle + 0x110 + 0x236) == 77 && g_car_pool_shared_now == 0);
      g_car_pool_share_ids = 0;
      g_ids_result = 0xffff;
      car_pool_find(NULL, &out, &vehicle_ref, NULL, NULL, 1);
      assert(*(short *)(vehicle + 0x110 + 0x236) == 77);
    }
    /* the name tag pool: its size is the slot count + 2, capped at 127, nothing else near it moves */
    { static const struct { const char *slots; unsigned char want; } sizes[] = {{"14", 16}, {"40", 42}, {"75", 77}, {"12", 14}};
      for (i = 0; i < 4; i++) {
          SetEnvironmentVariableA("FH2MP_CAR_SLOTS", sizes[i].slots);
          fresh();
          assert(patch_car_pool() == 1 && g_img[CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4] == sizes[i].want);
          assert(!memcmp(g_img + CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP, LABELS_CMP, 4) && !memcmp(g_img + CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 5, LABELS_CMP + 5, 2));
      }
      SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
      fresh();
      g_img[CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4] = 0x10;           /* another build: nothing at all is patched */
      g_raw[0] = 0;
      assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the name tag pool's size"));
      fresh();
      g_img[CAR_POOL_LABELS_RVA] = 0x41;
      g_raw[0] = 0;
      assert(patch_car_pool() == 0 && g_patches == 0 && strstr(g_raw, "mismatch: the name tag pool's start"));
    }
    /* FH2MP_CAR_NOBUDGET=1: the allocator budget check becomes `mov al, 1; ret`, only its first three bytes change */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "14");
    SetEnvironmentVariableA("FH2MP_CAR_NOBUDGET", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 4);
    assert(!memcmp(g_img + CAR_POOL_BUDGET_RVA, "\xb0\x01\xc3", 3) && !memcmp(g_img + CAR_POOL_BUDGET_RVA + 3, BUDGET_HEAD + 3, 17));
    fresh();
    g_img[CAR_POOL_BUDGET_RVA + 7] ^= 0xff;                                   /* another build: nothing at all is patched */
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && strstr(g_raw, "mismatch: the allocator's budget check"));
    for (i = 1; i <= 4; i++) {                                                /* the platform refuses any one: not done, no hooks */
        fresh();
        g_refuse_patch = i;
        assert(patch_car_pool() == 0 && g_hooks == 0);
    }
    SetEnvironmentVariableA("FH2MP_CAR_NOBUDGET", NULL);
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 3 && !memcmp(g_img + CAR_POOL_BUDGET_RVA, BUDGET_HEAD, 20));   /* off by default */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);

    /* FH2MP_CAR_ALLOCDIAG=1: failed allocations are logged, successful ones are not, thirty lines at most; nothing else is patched */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_ALLOCDIAG", "1");
    fresh();
    assert(patch_car_pool() == 0 && g_hooks == 1 && g_patches == 0);
    { static unsigned char manager[0x40];
      unsigned k;
      manager[0x18] = 2;
      g_car_pool_alloc_orig = (car_pool_alloc_fn)fake_alloc;
      g_car_pool_alloc_failed = 0;
      g_alloc_result = manager;                                              /* success: no line */
      g_raw[0] = 0;
      assert(car_pool_alloc(manager, 0x3ea0, 0x1234, NULL) == manager && g_raw[0] == 0);
      g_alloc_result = NULL;
      assert(car_pool_alloc(manager, 0x3ea0, 0x1234, NULL) == NULL);
      assert(strstr(g_raw, "carpool: allocation failed, 16032 bytes, tag 0x1234, heap mode 2\n"));
      g_raw[0] = 0;
      car_pool_alloc(manager, 0, 0x1234, NULL);                              /* a zero-byte request is not a failure */
      assert(g_raw[0] == 0);
      for (k = 0; k < 40; k++) car_pool_alloc(manager, 64, 1, NULL);
      assert(g_car_pool_alloc_failed == 30);                                 /* thirty lines at most */
    }
    fresh();
    g_img[CAR_POOL_ALLOC_RVA] = 0x40;                                         /* another build: no hook */
    assert(patch_car_pool() == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_ALLOCDIAG", NULL);

    /* FH2MP_CAR_VIEWGUARD=1: a view with no frame data (+0x4d8 NULL) is not rendered, a view with it is passed to the game's function */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_VIEWGUARD", "1");
    fresh();
    assert(patch_car_pool() == 0 && g_hooks == 2 && g_patches == 0);
    { static unsigned char view[0x600], frame[16];
      g_car_pool_view_a_orig = (car_pool_view_fn)fake_view_a;
      g_car_pool_view_b_orig = (car_pool_view_fn)fake_view_b;
      g_car_pool_view_skipped = 0;
      g_view_a_calls = g_view_b_calls = 0;
      *(void **)(view + 0x4d8) = frame;                                          /* ready: both go through */
      car_pool_view_a(view);
      car_pool_view_b(view);
      assert(g_view_a_calls == 1 && g_view_b_calls == 1 && g_car_pool_view_skipped == 0);
      *(void **)(view + 0x4d8) = NULL;                                           /* not ready: skipped, logged with the class */
      *(unsigned long long *)view = (unsigned long long)(ULONG_PTR)g_img + 0x2c11111;
      g_raw[0] = 0;
      car_pool_view_a(view);
      assert(g_view_a_calls == 1 && g_car_pool_view_skipped == 1);
      assert(strstr(g_raw, "carpool: view without frame data skipped (a), #1, vtable game+0x02c11111\n"));
      car_pool_view_b(view);
      assert(g_view_b_calls == 1 && strstr(g_raw, "(b), #2"));
      car_pool_view_a(NULL);                                                     /* no view: nothing is called */
      assert(g_view_a_calls == 1);
      { unsigned k; g_raw[0] = 0; for (k = 0; k < 20; k++) car_pool_view_a(view); assert(g_car_pool_view_skipped == 23 && !strstr(g_raw, "#12,")); }   /* ten lines at most */
    }
    fresh();
    g_img[CAR_POOL_VIEW_B_RVA] = 0x41;                                            /* another build of either: no hook on either */
    assert(patch_car_pool() == 0 && g_hooks == 0);
    fresh();
    g_img[CAR_POOL_VIEW_A_RVA] = 0x41;
    assert(patch_car_pool() == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_VIEWGUARD", NULL);

    /* FH2MP_CAR_VIEWLOD=1: the LOD table moves behind the view, the allocation grows by it, the new part is cleared */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    fresh();
    assert(patch_car_pool() == 0 && g_hooks == 0 && g_patches == 0);                 /* env not set: nothing touched */
    SetEnvironmentVariableA("FH2MP_CAR_VIEWLOD", "1");
    fresh();
    assert(patch_car_pool() == 0 && g_hooks == 1 && g_patches == 7);                 /* hook, allocation, 5 accesses, memset size */
    assert(strstr(g_raw, "carpool: view LOD table 32 -> 1024 entries at the end of the view (FH2MP_CAR_VIEWLOD)\n"));
    assert(*(unsigned *)(g_img + 0x1302160 + 0x15) == 0x24df0 + 0x1000);             /* the allocation */
    assert(*(unsigned *)(g_img + 0x1346332 + 3) == 0x24df0 && *(unsigned *)(g_img + 0x1346332 + 11) == 0x1000);
    assert(*(unsigned *)(g_img + 0x1346639 + 4) == 0x24df0 && *(unsigned *)(g_img + 0x1346649 + 4) == 0x24df0);
    assert(*(unsigned *)(g_img + 0x1346664 + 4) == 0x24df0 && *(unsigned *)(g_img + 0x134666c + 4) == 0x24df0);
    assert(g_img[0x1346639] == 0x46 && g_img[0x1346649 + 8] == 0x01 && g_img[0x134666c] == 0x42);   /* nothing else in them moved */
    { static unsigned char view[0x24df0 + 0x1000 + 16];
      memset(view, 0xaa, sizeof view);
      g_fake_view_made = view;
      assert(car_pool_view_new(NULL) == view);
      assert(view[0x24df0 - 1] == 0xaa && view[0x24df0] == 0 && view[0x24df0 + 0x1000 - 1] == 0 && view[0x24df0 + 0x1000] == 0xaa);   /* the new table, exactly */
      g_fake_view_made = NULL;
      assert(car_pool_view_new(NULL) == NULL);                                         /* the game's own failure passes through */
    }
    fresh();
    g_refuse_patch = 1;                                                                /* the allocation is refused: no access moves (it would run off a small view) */
    patch_car_pool();
    assert(g_patches == 0 && *(unsigned *)(g_img + 0x1346639 + 4) == 0x454 && *(unsigned *)(g_img + 0x1302160 + 0x15) == 0x24df0);
    for (i = 2; i <= 7; i++) {                                                         /* any later step refused: partly moved, and reported */
        fresh();
        g_refuse_patch = (int)i;
        patch_car_pool();
        assert(g_patches == 6 && strstr(g_raw, "carpool: view LOD table only partly moved, 7 of 8 steps done (FH2MP_CAR_VIEWLOD)\n"));
    }
    for (i = 0; i < 6; i++) {                                                          /* another build of any of the six places: nothing is touched */
        static const unsigned rvas[] = {0x1302160, 0x1346332, 0x1346639, 0x1346649, 0x1346664, 0x134666c};
        fresh();
        g_img[rvas[i]] ^= 0xff;
        assert(patch_car_pool() == 0 && g_hooks == 0 && g_patches == 0 && strstr(g_raw, "mismatch: "));
    }
    SetEnvironmentVariableA("FH2MP_CAR_VIEWLOD", NULL);

    /* the name tag allocator: an empty pool answers "no label" (the game reads the empty list head's garbage otherwise, dev 2026-11-04), a filled one runs the game's function */
    { static unsigned char self[0x100], head[0x20], node[0x20];
      void *out[2] = {(void *)(ULONG_PTR)1, (void *)(ULONG_PTR)2};
      unsigned k;
      g_car_pool_label_take_orig = (car_pool_label_take_fn)fake_label_take;
      g_car_pool_label_empty = 0;
      *(unsigned char **)(self + 0x60) = head;
      *(unsigned long long *)(self + 0x58) = 12;                                   /* 12 in use */
      *(unsigned char **)(head + 8) = head;                                         /* empty: the head's _Prev is the head */
      g_label_take_calls = 0;
      g_raw[0] = 0;
      assert(car_pool_label_take(self, out) == (void **)out && out[0] == NULL && out[1] == NULL && g_label_take_calls == 0);
      assert(strstr(g_raw, "carpool: name tag pool empty with 12 in use, the car gets no name tag (#1)\n"));
      *(unsigned char **)(head + 8) = node;                                         /* one free label: the game's own function */
      assert(car_pool_label_take(self, out) == (void **)out && g_label_take_calls == 1 && out[0] == (void *)(ULONG_PTR)0x1234 && out[1] == (void *)(ULONG_PTR)0x5678);
      *(unsigned char **)(head + 8) = head;
      g_raw[0] = 0;
      for (k = 0; k < 10; k++) car_pool_label_take(self, out);                      /* five lines at most, every call still answered */
      assert(strstr(g_raw, "(#5)\n") && !strstr(g_raw, "(#6)") && g_label_take_calls == 1 && g_car_pool_label_empty == 11);
      *(unsigned char **)(self + 0x60) = NULL;                                      /* no list at all: not ours to judge, the game's function runs */
      car_pool_label_take(self, out);
      assert(g_label_take_calls == 2);
    }
    /* the pool maker's wrapper says how big the pool came out, and what the loop bound in memory was at that moment */
    { static unsigned char module[0x100];
      g_car_pool_labels_make_orig = (car_pool_labels_make_fn)fake_labels_make;
      fresh();
      g_img[CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4] = 0x4d;
      g_raw[0] = 0;
      g_labels_make_calls = 0;
      car_pool_labels_make(module);
      assert(g_labels_make_calls == 1 && strstr(g_raw, "carpool: name tag pool built with 77 labels, loop bound in memory 77\n"));
      g_img[CAR_POOL_LABELS_RVA + CAR_POOL_LABELS_CMP + 4] = 0x0c;                  /* patched too late: the line shows the stock bound */
      g_raw[0] = 0;
      car_pool_labels_make(module);
      assert(strstr(g_raw, "loop bound in memory 12\n"));
    }
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");
    fresh();
    g_img[CAR_POOL_LABEL_TAKE_RVA] ^= 0xff;                                         /* another build of the allocator: nothing is patched or hooked */
    assert(patch_car_pool() == 0 && g_hooks == 0 && g_patches == 0 && strstr(g_raw, "mismatch: the name tag allocator"));
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);

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
    g_img[CAR_POOL_TICK_RVA] = 0x40;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0);
    SetEnvironmentVariableA("FH2MP_CAR_IDS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);

    /* FH2MP_CAR_IDS=1 (experimental): the id search hooked, player cars share ids */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "40");
    SetEnvironmentVariableA("FH2MP_CAR_IDS", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 3 && g_hooks == 5 && g_car_pool_share_ids == 1 && strstr(g_raw, "player cars share stand-in ids"));
    SetEnvironmentVariableA("FH2MP_CAR_IDS", NULL);

    /* FH2MP_CAR_NODRIVATARS=1: the slot count ignores NumAICars, both Drivatar setters store 0, the festival AI connect is hooked */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "14");
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_patches == 6 && g_hooks == 5 && g_car_pool_share_ids == 0 && strstr(g_raw, "remote car slots 11 -> 14, no Drivatars, parts cxna (FH2MP_CAR_SLOTS)"));
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
    /* the platform refuses any one of the six patches: no hooks, not reported as done */
    for (i = 1; i <= 6; i++) {
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
    /* parts: letters pick which of the four are applied */
    { static const struct { const char *value; int patches, hooks; unsigned set[3]; } cases[] = {
          {"c", 4, 4, {0x47a570 + 0x2e, 0, 0}},                 /* cache count only */
          {"x", 4, 4, {0x5430d0, 0, 0}},
          {"n", 4, 4, {0x653780, 0, 0}},
          {"a", 3, 5, {0, 0, 0}},                               /* the AI hook only: no byte patch beyond the slots */
          {"xn", 5, 4, {0x5430d0, 0x653780, 0}},
          {"cxna", 6, 5, {0x47a570 + 0x2e, 0x5430d0, 0x653780}},
          {"zz", 3, 4, {0, 0, 0}} };                            /* unknown letters pick nothing */
      for (i = 0; i < 7; i++) {
          int k;
          SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", cases[i].value);
          fresh();
          assert(patch_car_pool() == 1 && g_patches == cases[i].patches && g_hooks == cases[i].hooks);
          for (k = 0; k < 3; k++) {
              if (cases[i].set[k] && cases[i].set[k] != 0x47a570 + 0x2e) assert(g_img[cases[i].set[k]] != MAX_SETTER[0] || g_img[cases[i].set[k] + 1] != 0x89);
              if (cases[i].set[k] == 0x47a570 + 0x2e) assert(g_img[cases[i].set[k]] == 0x90);
          }
      }
      SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", "x");
      fresh();
      assert(patch_car_pool() == 1 && g_img[0x5430d0] == 0xc7 && g_img[0x653780] == 0x44 && g_img[0x47a570 + 0x2e] == 0x44);   /* only the maximum setter changed */
      SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", "n");
      fresh();
      assert(patch_car_pool() == 1 && g_img[0x5430d0] == 0x44 && g_img[0x653780] == 0xc7);
      SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", "a");
      fresh();
      g_img[0x5430d0] ^= 0xff;                                          /* a setter that does not match is irrelevant when it is not selected */
      assert(patch_car_pool() == 1);
    }
    /* FH2MP_CAR_NOAICTL=1: the AI controller attach is hooked; only a remote network car skips it */
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "14");
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_NOAICTL", "1");
    fresh();
    assert(patch_car_pool() == 1 && g_hooks == 5 && g_patches == 3);
    fresh();
    g_img[CAR_POOL_AICTL_RVA] = 0x40;                                  /* another build: nothing is patched */
    g_raw[0] = 0;
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the AI controller attach"));
    { static unsigned char car[0x5b00];
      static unsigned char network_car[16];
      static void *vtable[40];
      static void *no_vtable[1];
      g_car_pool_aictl_orig = (car_pool_aictl_fn)fake_aictl;
      g_car_pool_aictl_skipped = 0;
      g_aictl_calls = 0;
      vtable[0xc0 / 8] = (void *)fake_is_local;
      *(void ***)network_car = vtable;
      *(unsigned char **)(car + 0x5aa8) = network_car;
      g_fake_is_local_result = 0;                                      /* a remote car: skipped, and said so */
      g_raw[0] = 0;
      car_pool_aictl(car);
      assert(g_aictl_calls == 0 && g_car_pool_aictl_skipped == 1 && strstr(g_raw, "no AI controller for remote network car #1"));
      g_fake_is_local_result = 1;                                      /* the local player's own car keeps it */
      car_pool_aictl(car);
      assert(g_aictl_calls == 1);
      *(unsigned char **)(car + 0x5aa8) = NULL;                        /* not a network car (traffic, a Drivatar) */
      car_pool_aictl(car);
      assert(g_aictl_calls == 2);
      *(unsigned char **)(car + 0x5aa8) = network_car;
      *(void ***)network_car = NULL;                                   /* no vtable: not touched */
      car_pool_aictl(car);
      assert(g_aictl_calls == 3);
      *(void ***)network_car = (void **)no_vtable;
      no_vtable[0] = NULL;
      car_pool_aictl(NULL);                                            /* no car: the game's function runs */
      assert(g_aictl_calls == 4);
      g_car_pool_aictl_skipped = 100;                                  /* past five, no more log lines */
      *(void ***)network_car = vtable;
      g_fake_is_local_result = 0;
      g_raw[0] = 0;
      car_pool_aictl(car);
      assert(g_raw[0] == 0 && g_aictl_calls == 4);
    }
    SetEnvironmentVariableA("FH2MP_CAR_NOAICTL", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);



    /* the default profile: no variable set, 75 slots, the working set of switches (dev 2026-11-03: 72 cars on a real client) */
    SetEnvironmentVariableA("FH2MP_NO_CARSLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_NODRIVATARS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_IDS", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_NOAICTL", NULL);
    SetEnvironmentVariableA("FH2MP_CAR_VIEWLOD", NULL);
    fresh();
    assert(patch_car_pool() == 1);
    assert(g_img[CAR_POOL_RVA + 0x28] == 75);                             /* slots 11 -> 75 */
    assert(g_img[0x5430d0] == 0xc7 && g_img[0x653780] == 0x44 && g_img[0x47a570 + 0x2e] == 0x44);    /* only the Drivatar maximum is 0 (parts "x") */
    assert(g_car_pool_share_ids == 1);                                    /* IDS */
    assert(strstr(g_raw, "carpool: view LOD table 32 -> 1024 entries"));  /* VIEWLOD */
    assert(*(unsigned *)(g_img + 0x1302160 + 0x15) == 0x24df0 + 0x1000);
    { int hooks_default = g_hooks;
      SetEnvironmentVariableA("FH2MP_CAR_NOAICTL", "0");                  /* an own value wins */
      SetEnvironmentVariableA("FH2MP_CAR_VIEWLOD", "0");
      fresh();
      assert(patch_car_pool() == 1 && g_hooks == hooks_default - 2 && !strstr(g_raw, "view LOD table"));
      SetEnvironmentVariableA("FH2MP_CAR_NOAICTL", NULL);
      SetEnvironmentVariableA("FH2MP_CAR_VIEWLOD", NULL);
    }
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", "30");                    /* an own slot count wins */
    fresh();
    assert(patch_car_pool() == 1 && g_img[CAR_POOL_RVA + 0x28] == 30);
    SetEnvironmentVariableA("FH2MP_CAR_SLOTS", NULL);
    SetEnvironmentVariableA("FH2MP_NO_CARSLOTS", "1");                    /* the off switch: the game's own 11 */
    fresh();
    assert(patch_car_pool() == 0 && g_patches == 0 && g_hooks == 0 && !memcmp(g_img + CAR_POOL_RVA, VFUNC2, sizeof VFUNC2));
    SetEnvironmentVariableA("FH2MP_NO_CARSLOTS", NULL);

    /* the wait for every player's car: let through when the cache is full and idle, or stalled; never while it is still working */
    {
        static unsigned char module[0x400], slots[3 * CAR_POOL_SLOT], requests[3][0x600], queue[8];
        unsigned char *group = module + CAR_POOL_GROUP;
        unsigned i;

        fresh();
        assert(patch_car_wait() == 1 && g_hooks == 2 && g_car_pool_tick_orig == fake_tick);      /* its own tick hook, and the wait */
        fresh();
        g_img[CAR_WAIT_RVA] = 0x40;
        assert(patch_car_wait() == 0 && g_hooks == 0 && strstr(g_raw, "mismatch: the player car wait"));
        fresh();
        g_img[CAR_POOL_TICK_RVA] = 0x40;
        assert(patch_car_wait() == 0 && g_hooks == 0);
        SetEnvironmentVariableA("FH2MP_CAR_DIAG", "1");
        fresh();
        patch_car_pool();                                                  /* car_pool's own tick hook is there: not hooked twice */
        g_hooks = 0;
        assert(patch_car_wait() == 1 && g_hooks == 1);
        SetEnvironmentVariableA("FH2MP_CAR_DIAG", NULL);

        fresh();
        assert(patch_car_wait() == 1);
        g_car_wait_orig = fake_wait;
        g_car_wait_clock = fake_clock;
        g_now = 100000;
        g_wait_answer = 1;
        assert(car_wait(module) == 1);                                    /* the game's yes stands */
        g_wait_answer = 0;
        assert(car_wait(module) == 0 && g_car_wait_passed == 0);          /* no cache known yet: the game's no stands */
        car_wait_tick(module, NULL, NULL, NULL);
        assert(g_car_cache_self == module);
        assert(car_wait(module) == 0);                                    /* an empty group: the game's no stands */

        *(unsigned long long *)(group + 0x60) = (unsigned long long)(ULONG_PTR)slots;
        *(unsigned long long *)(group + 0x68) = (unsigned long long)(ULONG_PTR)(slots + sizeof slots);
        *(unsigned long long *)(group + 0x78) = (unsigned long long)(ULONG_PTR)queue;
        *(unsigned long long *)(group + 0x80) = (unsigned long long)(ULONG_PTR)(queue + 8);     /* one car waits for a slot */
        for (i = 0; i < 3; i++) {
            *(unsigned long long *)(slots + (size_t)i * CAR_POOL_SLOT + 0x558) = (unsigned long long)(ULONG_PTR)requests[i];
            *(unsigned *)(requests[i] + 0x598) = 2;                       /* loaded */
        }
        *(unsigned *)(group + 0x90) = 0;                                  /* no free slot */

        g_now = 200000;
        assert(car_wait(module) == 0);                                    /* the first look: it may still change */
        assert(wait_for(module, CAR_WAIT_FULL_MS - 500) == 0);
        g_raw[0] = 0;
        assert(wait_for(module, 500) == 1 && g_car_wait_passed == 1 && strstr(g_raw, "all car slots are taken") && strstr(g_raw, "cars waiting for a slot 1"));

        g_now = 300000;                                                   /* a new wait; one slot's car is still loading: it keeps waiting */
        *(unsigned *)(requests[1] + 0x598) = 1;
        assert(car_wait(module) == 0);
        assert(wait_for(module, CAR_WAIT_FULL_MS + 5000) == 0 && g_car_wait_passed == 1);
        *(unsigned *)(requests[1] + 0x598) = 2;                           /* done loading: counts as a change, then 1.5 s of quiet */
        assert(wait_for(module, CAR_WAIT_FULL_MS) == 0);                  /* the first look after the change is at +0.5 s */
        assert(wait_for(module, 500) == 1 && g_car_wait_passed == 2);

        g_now = 400000;                                                   /* a free slot: the game decides, until the 20 s stall */
        *(unsigned *)(group + 0x90) = 1;
        assert(car_wait(module) == 0);
        assert(wait_for(module, 10000) == 0 && g_car_wait_passed == 2);
        *(unsigned *)(requests[2] + 0x598) = 1;                           /* progress resets the stall timer */
        assert(wait_for(module, CAR_WAIT_STALL_MS) == 0);
        g_raw[0] = 0;
        assert(wait_for(module, 500) == 1 && g_car_wait_passed == 3 && strstr(g_raw, "nothing in the car cache changed"));
        assert(g_reports == 1 && g_report_code == 2 && strstr(g_report_text, "lasted 20 s; free slots 1, cars waiting for a slot 1, loading 1, loaded 2"));   /* told to the server once per wait */

        g_now = 500000;                                                   /* an unsane group (slots not a whole number): the game's no stands */
        *(unsigned long long *)(group + 0x68) += 5;
        assert(car_wait(module) == 0);
        assert(wait_for(module, CAR_WAIT_STALL_MS * 2) == 0 && g_car_wait_passed == 3);

        g_now = 600000;                                                   /* a pause between two waits starts a new one */
        *(unsigned long long *)(group + 0x68) -= 5;
        assert(car_wait(module) == 0);
        g_now += CAR_WAIT_STALL_MS * 2;
        assert(car_wait(module) == 0 && g_car_wait_passed == 3);
        g_car_wait_clock = NULL;
    }

    printf("car pool: ok\n");
    return 0;
}
