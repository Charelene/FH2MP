/* The hooks of copy_path.inc (game side) and the switches of the contact changes for other players' cars.
 *
 * g_copy_flags (FH2MP_COPY_FLAGS=<0..63> at start, default 63 = every switch on, 0 = every switch off; the install line gives its
 * address so a debug build can change it live):
 *   1  another player's car restarts its blend from its owner's path, not from where this game's physics moved it (copy_path.inc)
 *   2  contacts add no offset to another player's car (RemoteCarPrediction::AddPositionOffset 0x9ecf80, called from the car's
 *      contact handler ForzaCar::vfunc_0_vt16 0x296ac0 through helper_272710; the offset is drawn on top of the blend)
 *   4  the local car is sent without the look-ahead while it touches another player's car (contact_forecast_hook.inc)
 *   8  the local car takes the full contact from another player's car: the game's collision limiter (LocalCarPrediction::vfunc_14,
 *      0x9c6900, retail ini CarNetwork\LimitLocalRemoteCollisions) is skipped for those contacts (contact_forecast_hook.inc). It
 *      caps the local car's change of speed to 1.2x a fully soft hit and was there for copies that give way; with switch 1 a copy
 *      keeps its owner's path, and with the limiter the local car drove into it (measured 2026-11-18: the two owners' cars 1.15 m
 *      apart centre to centre where touching is about 2.9 m; with the limiter off they stayed at 2.9 m).
 *  16  with 8: only the car that rams takes the full contact; a car being rammed, or pressed while standing, keeps the limiter
 *      and the limiter also watches contacts slower than its retail 3 mph (contact_forecast.inc, cf_local_rams).
 *  32  with 4: the pause starts when the local car is about to hit another player's car, not at the contact
 *      (contact_forecast.inc, cf_will_touch; positions from g_cp_seen below).
 */
#include "copy_path.inc"

#define CP_APPLY_RVA 0x9ebc20u  /* RemoteCarPrediction::ApplyUnpackedState */
#define CP_OFFSET_RVA 0x9ecf80u /* RemoteCarPrediction::AddPositionOffset */
#define CP_CAR 0x08u            /* RemoteCarPrediction+0x8: the car */
#define CP_BODY 0x10u           /* RemoteCarPrediction+0x10: the car's dynamics interface */
#define CP_STATE_A 0xa0u        /* blend start state */
#define CP_STATE_B 0x1d0u
#define CP_STATE_LEN 0x110u
#define CP_SNAPPED 0x1au
#define CP_T0 0x2cu
#define CP_T1 0x30u
#define CP_LAST_FRAME 0x330u
#define CP_STALE 0x354u
#define CP_HERMITE_SLOT 0xb90u /* the body's blend: (body, out, a, b, seconds, fraction), as helper_9effa0 calls it at 0x9f00e5 */
#define CP_CAR_TYPE 0x4d08u

static volatile long g_copy_flags = 63;

/* Where other players' cars are, for switch 32 (contact_forecast_hook.inc): copied from each copy's body when a sample arrives. */
#define CP_SEEN_SLOTS 16
#define CP_BODY_POS (-0x390) /* from the dynamics interface: position (3 floats) */
#define CP_BODY_VEL (-0x350) /* linear velocity */
typedef struct {
    const void *car;
    unsigned long long at;
    float pos[3], vel[3];
} cp_seen;
static cp_seen g_cp_seen[CP_SEEN_SLOTS];

static void cp_note(const void *car, const unsigned char *body, unsigned long long now)
{
    int i, pick = 0;
    for (i = 0; i < CP_SEEN_SLOTS; i++) {
        if (g_cp_seen[i].car == car) {
            pick = i;
            break;
        }
        if (g_cp_seen[i].at < g_cp_seen[pick].at) {
            pick = i;
        }
    }
    g_cp_seen[pick].car = car;
    g_cp_seen[pick].at = now;
    memcpy(g_cp_seen[pick].pos, body + CP_BODY_POS, sizeof g_cp_seen[pick].pos);
    memcpy(g_cp_seen[pick].vel, body + CP_BODY_VEL, sizeof g_cp_seen[pick].vel);
}

typedef unsigned long long (*cp_apply_fn)(unsigned char *self, void *state, void *extra, int stamp);
typedef void (*cp_offset_fn)(unsigned char *self, const float *v);
typedef void (*cp_hermite_fn)(void *body, void *out, const void *a, const void *b, float seconds, float fraction);
static cp_apply_fn g_cp_apply_orig;
static cp_offset_fn g_cp_offset_orig;
static unsigned long g_cp_kept, g_cp_dropped;

static int cp_is_player_copy(const unsigned char *self)
{
    const unsigned char *car = *(const unsigned char *const *)(self + CP_CAR);
    return car && *(const int *)(car + CP_CAR_TYPE) == CP_TYPE_REMOTE_PLAYER;
}

static unsigned long long cp_apply_detour(unsigned char *self, void *state, void *extra, int stamp)
{
    unsigned char a[CP_STATE_LEN] __attribute__((aligned(16))), b[CP_STATE_LEN] __attribute__((aligned(16)));
    unsigned char out[CP_STATE_LEN + 0x10] __attribute__((aligned(16)));
    unsigned long long r;
    int t0, t1, tlast, stale;
    void *body;
    float frac;
    if (!cp_is_player_copy(self) || !(body = *(void **)(self + CP_BODY))) {
        return g_cp_apply_orig(self, state, extra, stamp);
    }
    if (!(g_copy_flags & 1)) {
        r = g_cp_apply_orig(self, state, extra, stamp);
        if (g_copy_flags & 32) {
            cp_note(*(void **)(self + CP_CAR), (const unsigned char *)body, GetTickCount64());
        }
        return r;
    }
    t0 = *(int *)(self + CP_T0);
    t1 = *(int *)(self + CP_T1);
    tlast = *(int *)(self + CP_LAST_FRAME);
    stale = *(int *)(self + CP_STALE);
    memcpy(a, self + CP_STATE_A, CP_STATE_LEN);
    memcpy(b, self + CP_STATE_B, CP_STATE_LEN);
    r = g_cp_apply_orig(self, state, extra, stamp);
    if (g_copy_flags & 32) {
        cp_note(*(void **)(self + CP_CAR), (const unsigned char *)body, GetTickCount64());
    }
    frac = cp_frac(t0, t1, tlast);
    if (frac < 0.0f || !cp_restarted_from_body(stale, self[CP_SNAPPED] != 0, *(int *)(self + CP_T0), tlast)) {
        return r;
    }
    ((cp_hermite_fn)(*(void ***)body)[CP_HERMITE_SLOT / 8])(body, out, a, b, (float)(t1 - t0) * 0.001f, frac);
    memcpy(self + CP_STATE_A, out, CP_STATE_LEN);
    if (++g_cp_kept == 1 || g_cp_kept == 100000) {
        log_begin();
        log_raw("copypath: another player's car restarts its blend from its owner's path, not from where this game moved it (");
        log_dec(g_cp_kept);
        log_raw(" samples so far)\n");
    }
    return r;
}

static void cp_offset_detour(unsigned char *self, const float *v)
{
    if ((g_copy_flags & 2) && cp_is_player_copy(self)) {
        if (++g_cp_dropped == 1 || g_cp_dropped == 10000) {
            log_begin();
            log_raw("copypath: a contact offset for another player's car is dropped; the car stays where its owner has it (");
            log_dec(g_cp_dropped);
            log_raw(" so far)\n");
        }
        return;
    }
    g_cp_offset_orig(self, v);
}

/* 1 when both hooks are in place. */
static int patch_copy_path(void)
{
    static const unsigned char apply_head[12] = {0x48, 0x8B, 0xC4, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60};
    static const unsigned char offset_body[21] = {0xC5, 0xF8, 0x28, 0x81, 0x20, 0x03, 0x00, 0x00, 0xC5, 0xF8, 0x58,
                                                  0x0A, 0xC5, 0xF8, 0x29, 0x89, 0x20, 0x03, 0x00, 0x00, 0xC3};
    char v[8];
    unsigned char *img;
    int n, i;
    if (GetEnvironmentVariableA("FH2MP_COPY_FLAGS", v, sizeof v) != 0) {
        for (n = 0, i = 0; v[i] >= '0' && v[i] <= '9' && i < 2; i++) {
            n = n * 10 + (v[i] - '0');
        }
        if (i > 0 && v[i] == 0 && n <= 63) {
            g_copy_flags = n;
        }
    }
    img = find_fh2_image();
    if (!img || !code_is("copypath", "RemoteCarPrediction::ApplyUnpackedState", img, CP_APPLY_RVA, apply_head, sizeof apply_head) ||
        !code_is("copypath", "RemoteCarPrediction::AddPositionOffset", img, CP_OFFSET_RVA, offset_body, sizeof offset_body)) {
        return 0;
    }
    if (!plat_hook("copypath: blend restart", img + CP_APPLY_RVA, (void *)cp_apply_detour, (void **)&g_cp_apply_orig) ||
        !plat_hook("copypath: contact offset", img + CP_OFFSET_RVA, (void *)cp_offset_detour, (void **)&g_cp_offset_orig)) {
        return 0;
    }
    log_begin();
    log_raw("copypath: switches ");
    log_dec((unsigned long)g_copy_flags);
    log_raw(" (1 path, 2 no contact offset, 4 no look-ahead in contact, 8 no limiter with player cars, 16 limiter kept for the car"
            " being rammed, 32 no look-ahead just before a hit) at 0x");
    log_hex((unsigned long long)(ULONG_PTR)&g_copy_flags, 16);
    log_raw("\n");
    return 1;
}
