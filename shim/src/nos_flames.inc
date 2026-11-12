/* Other players' NitroSystem flames, and the frame step the NOS rule runs on (mods_sync.inc).
 *
 * NitroSystem 2.0.0 (qiangqiang101/FH2-NitroSystem, src/nitro_mod.cpp, MIT License, Copyright (c) 2026 Bartholomew Ho) shows a Drivatar's NOS by
 * running the game's own backfire routine on that car: this does the same for each other player whose NOS the server says is burning, the way
 * NitroSystem's Drivatar flames do it (read_locator_tips, note_car_locators, drive_backfire_tips):
 *   - ForzaCar::Update (game+0x2A9400, the platform's FH2_GAME_FORZACAR_UPDATE) builds the car's exhaust tips from its own exhaust locators: eight
 *     weak pointers at ForzaCar+0x88 (object, control block with the use count at +8); a live one's locator matrix (its vtable slot 2) and flag
 *     (+0x40) make the tip, 0x60 bytes each. Pointers that are still empty are resolved by the car's own method (vtable slot 58, game+0xF7DB40)
 *     at most every 0.5 s, as the game does before its own backfire message.
 *   - the game's logic thread step (game+0xFA78A0, the thread the backfire routine runs on, learned from the routine's real calls) runs the backfire
 *     routine (game+0x13B0D70, CarPresentation11 vtable slot 76) for the car's presentation every 40 ms with the tips as its message: the game then
 *     spawns its backfire effect at each flagged tip (NitroSystem's blue Backfire.xml when the viewer has NitroSystem, the game's own otherwise).
 * Only cars of other players, only while the newest mod state reply lists their NOS burning, and only with tips built in the last 100 ms (a car
 * that is no longer updated is not touched). The local car and Drivatars are NitroSystem's own business. Sound is not played. */
#define NF_FRAME_RVA 0xfa78a0u
#define NF_BACKFIRE_RVA 0x13b0d70u
#define NF_CAR_UPDATE_RVA 0x2a9400u
#define NF_RESOLVE_RVA 0xf7db40u
#define NF_FORZACAR_VT_RVA 0x2c5cb80u
#define NF_RESOLVE_SLOT 58
#define NF_EXHAUSTS 0x88u
#define NF_TIPS 8
#define NF_TIP 0x60u
#define NF_TIP_FLAG 0x40u
#define NF_DRIVE_MS 40u
#define NF_TIPS_FRESH_MS 100u
#define NF_RESOLVE_MS 500u
#define NF_REMOTE_SEEN_MS 1000u      /* a remote car counts while its owner's predictions arrived this recently */
#define NF_CARS MODS_MAX
#define CAR_PRES_OFFSET 0x4d38u
#define CAR_DEFERRED_PRES_VT 0x2ebef68u
#define CAR_PRES_VT1 0x2ec1450u
#define CAR_PRES_VT2 0x2ec2b70u

typedef struct {
    SRWLOCK lock;
    void *car;
    unsigned char *pres;
    ULONGLONG tick, resolved;
    unsigned mask;
    unsigned char tips[NF_TIPS * NF_TIP];
} nf_slot;
static nf_slot g_nf_slots[NF_CARS];
static void *volatile g_nf_want[NF_CARS];      /* the remote cars whose NOS burns now (the frame step writes, the cars' update reads) */
static void (*g_nf_car_update_orig)(void *car, float dt, float b, unsigned flag);
static unsigned long long (*g_nf_backfire_orig)(void *a, void *b, void *c, void *d);
static unsigned long long (*g_nf_frame_orig)(void *self, void *flag);
static volatile LONG g_nf_bf_tid, g_nf_driving, g_nf_driven, g_nf_share;
static unsigned char *g_nf_img;

/* A car's CarPresentation11 (through the deferred presentation), NULL when it is not one. */
static unsigned char *car_presentation(const unsigned char *img, void *car)
{
    unsigned char *pres = NULL, *vt = NULL;
    if (!car || !read_game_memory((unsigned char *)car + CAR_PRES_OFFSET, &pres, 8) || !pres || !read_game_memory(pres, &vt, 8)) return NULL;
    if (vt == img + CAR_DEFERRED_PRES_VT && (!read_game_memory(pres + 8, &pres, 8) || !pres || !read_game_memory(pres, &vt, 8))) return NULL;
    return vt == img + CAR_PRES_VT1 || vt == img + CAR_PRES_VT2 ? pres : NULL;
}

/* The car's live exhaust locators as the game's tips -> their mask. */
static unsigned nf_read_tips(const unsigned char *img, const unsigned char *car, unsigned char tips[NF_TIPS * NF_TIP])
{
    unsigned i, mask = 0;
    for (i = 0; i < NF_TIPS * NF_TIP; i++) tips[i] = 0;
    for (i = 0; i < NF_TIPS; i++) {
        void *obj = NULL, **vt = NULL, *fn = NULL;
        unsigned char *ctrl = NULL;
        int uses = 0;
        const float *m;
        if (!read_game_memory(car + NF_EXHAUSTS + 0x10 * i, &obj, 8) || !read_game_memory(car + NF_EXHAUSTS + 0x10 * i + 8, &ctrl, 8) || !obj || !ctrl ||
            !read_game_memory(ctrl + 8, &uses, 4) || uses <= 0 || !read_game_memory(obj, &vt, 8) ||
            (unsigned char *)vt < img || (unsigned char *)vt >= img + FH2_SIZE_OF_IMAGE || !read_game_memory(vt + 2, &fn, 8) ||
            (unsigned char *)fn < img || (unsigned char *)fn >= img + FH2_SIZE_OF_IMAGE) continue;
        m = ((const float *(*)(void *))fn)(obj);
        if (!m || !read_game_memory(m, tips + i * NF_TIP, 0x40)) continue;
        tips[i * NF_TIP + NF_TIP_FLAG] = 1;
        mask |= 1u << i;
    }
    return mask;
}

static int nf_wanted(void *car)
{
    unsigned i;
    for (i = 0; i < NF_CARS; i++) if (g_nf_want[i] == car) return 1;
    return 0;
}

static void nf_car_update(void *car, float dt, float b, unsigned flag)
{
    g_nf_car_update_orig(car, dt, b, flag);
    if (car && g_nf_share && nf_wanted(car)) {
        ULONGLONG now = GetTickCount64();
        unsigned char tips[NF_TIPS * NF_TIP], *pres = car_presentation(g_nf_img, car);
        nf_slot *s = &g_nf_slots[0];
        unsigned i, mask;
        int same;
        if (!pres) return;
        for (i = 0; i < NF_CARS; i++) {     /* only this thread picks slots */
            if (g_nf_slots[i].car == car) { s = &g_nf_slots[i]; break; }
            if (g_nf_slots[i].tick < s->tick) s = &g_nf_slots[i];
        }
        same = s->car == car && s->pres == pres;
        mask = nf_read_tips(g_nf_img, car, tips);
        if (!mask && (!same || now - s->resolved >= NF_RESOLVE_MS)) {
            void **vt = NULL, *fn = NULL;
            if (read_game_memory(car, &vt, 8) && (unsigned char *)vt == g_nf_img + NF_FORZACAR_VT_RVA && read_game_memory(vt + NF_RESOLVE_SLOT, &fn, 8) &&
                (unsigned char *)fn == g_nf_img + NF_RESOLVE_RVA) {
                ((void (*)(void *))fn)(car);
                mask = nf_read_tips(g_nf_img, car, tips);
            }
            AcquireSRWLockExclusive(&s->lock);
            if (!same) { s->car = car; s->pres = pres; s->mask = 0; }
            s->resolved = now;
            ReleaseSRWLockExclusive(&s->lock);
            same = 1;
        }
        AcquireSRWLockExclusive(&s->lock);
        if (!same) { s->car = car; s->pres = pres; s->mask = 0; }
        if (mask) {   /* the game empties the pointers now and then: the last good tips stay until they are back */
            s->mask = mask;
            for (i = 0; i < sizeof tips; i++) s->tips[i] = tips[i];
        }
        s->tick = now;
        ReleaseSRWLockExclusive(&s->lock);
    }
}

static unsigned long long nf_backfire(void *a, void *b, void *c, void *d)
{
    if (!g_nf_driving) InterlockedExchange(&g_nf_bf_tid, (LONG)GetCurrentThreadId());
    return g_nf_backfire_orig(a, b, c, d);
}

/* The backfire routine for each other player's car whose NOS burns, every 40 ms, on the routine's own thread. */
static void nf_drive(void)
{
    static ULONGLONG last;
    static __attribute__((aligned(16))) unsigned char message[0x400];
    static int told;
    mods_record records[MODS_MAX];
    ULONGLONG now = GetTickCount64();
    unsigned i, k, n, wanted = 0;
    void *cars[NF_CARS];
    if (!g_nf_share || now - last < NF_DRIVE_MS) return;
    last = now;
    n = mods_snapshot(records, now);
    for (k = 0; k < n && wanted < NF_CARS; k++) {
        void *car;
        if (!(records[k].nos & MODS_NOS_BURNING) || !(car = mods_remote_car(records[k].owner, records[k].car, now, NF_REMOTE_SEEN_MS))) continue;
        cars[wanted++] = car;
    }
    for (i = 0; i < NF_CARS; i++) g_nf_want[i] = i < wanted ? cars[i] : NULL;
    if (!wanted || !g_nf_bf_tid || (DWORD)g_nf_bf_tid != GetCurrentThreadId()) return;
    for (i = 0; i < wanted; i++) {
        unsigned char *pres = NULL;
        unsigned mask = 0, j;
        for (k = 0; k < NF_CARS && !pres; k++) {
            nf_slot *s = &g_nf_slots[k];
            if (s->car != cars[i]) continue;
            AcquireSRWLockShared(&s->lock);
            if (s->car == cars[i] && s->mask && now - s->tick <= NF_TIPS_FRESH_MS) {
                for (j = 0; j < NF_TIPS * NF_TIP; j++) message[j] = s->tips[j];
                for (; j < sizeof message; j++) message[j] = 0;
                pres = s->pres;
                mask = s->mask;
            }
            ReleaseSRWLockShared(&s->lock);
        }
        if (!pres) continue;
        InterlockedExchange(&g_nf_driving, 1);
        g_nf_backfire_orig(pres, message, NULL, NULL);
        InterlockedExchange(&g_nf_driving, 0);
        if (InterlockedIncrement(&g_nf_driven) == 1 && !told) {
            told = 1;
            log_begin(); log_raw("mods: another player's NOS flames shown (exhaust tips "); log_hex(mask, 2); log_raw(")\n");
        }
    }
}

static unsigned long long nf_frame(void *self, void *flag)
{
    unsigned long long r = g_nf_frame_orig(self, flag);
    nos_rule_tick();
    nf_drive();
    return r;
}

/* An entry that is the game's code (1) or starts with another mod's hook in front of the game's code (2): the platform chains ours. */
static int mods_entry_ok(const unsigned char *img, unsigned rva, const unsigned char *want, unsigned n, const char *what)
{
    unsigned k;
    for (k = 0; k < n && img[rva + k] == want[k]; k++) {}
    if (k == n) return 1;
    if (img[rva] == 0xe9 || (img[rva] == 0xff && img[rva + 1] == 0x25)) {
        log_begin(); log_raw("mods: "); log_raw(what); log_raw(" at game+0x"); log_hex(rva, 8);
        log_raw(" starts with another mod's hook; ours goes behind it through the platform\n");
        return 2;
    }
    log_begin(); log_raw("mods: unexpected bytes in "); log_raw(what); log_raw(" at game+0x"); log_hex(rva + k, 8);
    log_raw(": expected "); log_hex(want[k], 2); log_raw(", found "); log_hex(img[rva + k], 2); log_raw("\n");
    return 0;
}

static int patch_nos_flames(unsigned char *img)
{
    static const unsigned char frame[8] = {0x40, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41};
    static const unsigned char backfire[8] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x55};
    static const unsigned char update[8] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x57};
    wchar_t env[8];
    g_nf_img = img;
    if (!mods_entry_ok(img, NF_FRAME_RVA, frame, 8, "the logic thread's frame step") ||
        !plat_hook("mods: logic frame step", img + NF_FRAME_RVA, (void *)nf_frame, (void **)&g_nf_frame_orig)) return 0;
    if (GetEnvironmentVariableW(L"FH2MP_NO_MODSYNC", env, 8) != 0) return 1;
    if (!mods_entry_ok(img, NF_BACKFIRE_RVA, backfire, 8, "the backfire routine") ||
        !mods_entry_ok(img, NF_CAR_UPDATE_RVA, update, 8, "ForzaCar::Update") ||
        !plat_hook("mods: backfire routine", img + NF_BACKFIRE_RVA, (void *)nf_backfire, (void **)&g_nf_backfire_orig) ||
        !plat_hook("mods: car update", img + NF_CAR_UPDATE_RVA, (void *)nf_car_update, (void **)&g_nf_car_update_orig)) {
        log_begin(); log_raw("mods: other players' NOS flames are not shown (a hook could not be made); the NOS rule stays\n");
        return 1;
    }
    InterlockedExchange(&g_nf_share, 1);
    log_begin(); log_raw("hook: mods: the NOS rule on the logic frame step (game+0xfa78a0), other players' NOS flames (game+0x13b0d70, +0x2a9400)\n");
    return 1;
}
