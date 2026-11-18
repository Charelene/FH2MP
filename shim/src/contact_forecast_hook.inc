/* The hooks of contact_forecast.inc (game side). Switch 4 of g_copy_flags (copy_path_hook.inc) turns the pause on; off by default. */
#include "contact_forecast.inc"

#define CF_LIMITER_RVA 0x9c6900u /* LocalCarPrediction::vfunc_14: called for each contact of a local car with another car */
#define CF_SEND_RVA 0x9b9350u    /* LocalCarPrediction::helper_9b9350: the look-ahead and stamp of the state that is sent */
#define CF_SPEED_RVA 0x12cd5e0u  /* CCarDynamics::GetSpeedMps_vt928: vmovss xmm0,[rcx+0xe28]; ret */
#define CF_CAR_TYPE 0x4d08u
#define CF_BODY 0x18u            /* LocalCarPrediction+0x18: the car's dynamics interface */
#define CF_SPEED 0xe28u
#define CF_STAMP 0x44u           /* LocalCarPrediction+0x44: the time stamp of the state that is sent */

typedef void (*cf_limiter_fn)(void *self, void *contact, void **other);
typedef void (*cf_send_fn)(void *self, int now);
static cf_limiter_fn g_cf_limiter_orig;
static cf_send_fn g_cf_send_orig;
static cf_slot g_cf_table[CF_SLOTS];
static unsigned long g_cf_paused;

#define CF_SETTINGS_RVA 0x365a9e8u /* g_NetworkSmoothingSettings (pointer) */
#define CF_WATCH_MPH 0x708u        /* its LimitCollVelDiffToWatchMPH (retail 3): read only by the limiter */
#define CF_OTHER_BODY 0x4c08u      /* Car+0x4c08: the car's dynamics interface (FUN_140f605d0, as the limiter gets it) */
#define CF_SLOT_INV_MASS 0x300u    /* dynamics interface: inverse mass (float) */
#define CF_SLOT_VELOCITY 0x288u    /* dynamics interface: linear velocity (out vec4, returns it) */
#define CF_CONTACT_NORMAL 0x20u
#define CF_CONTACT_IMPULSE 0x50u

typedef float (*cf_inv_mass_fn)(void *body);
typedef float *(*cf_velocity_fn)(void *body, float *out);
static unsigned char *g_cf_img;
static unsigned long g_cf_unlimited, g_cf_watched;

/* 1 when the local car is the one ramming in this contact (cf_local_rams), 1 too when a body is missing (the full contact). */
static int cf_rams_now(void *self, const unsigned char *contact, const unsigned char *car)
{
    float vm_buf[4] __attribute__((aligned(16))), vo_buf[4] __attribute__((aligned(16)));
    void *mine = *(void **)((unsigned char *)self + CF_BODY), *theirs = *(void *const *)(car + CF_OTHER_BODY);
    float im, io, *vm, *vo;
    if (!mine || !theirs) {
        return 1;
    }
    im = ((cf_inv_mass_fn)(*(void ***)mine)[CF_SLOT_INV_MASS / 8])(mine);
    vm = ((cf_velocity_fn)(*(void ***)mine)[CF_SLOT_VELOCITY / 8])(mine, vm_buf);
    io = ((cf_inv_mass_fn)(*(void ***)theirs)[CF_SLOT_INV_MASS / 8])(theirs);
    vo = ((cf_velocity_fn)(*(void ***)theirs)[CF_SLOT_VELOCITY / 8])(theirs, vo_buf);
    return cf_local_rams(vm, im, vo, io, (const float *)(contact + CF_CONTACT_NORMAL), (const float *)(contact + CF_CONTACT_IMPULSE));
}

static void cf_limiter_detour(void *self, void *contact, void **other)
{
    const unsigned char *car = other ? (const unsigned char *)*other : NULL;
    unsigned char *settings;
    float keep;
    if (car && *(const int *)(car + CF_CAR_TYPE) == CF_TYPE_REMOTE_PLAYER) {
        cf_touch(g_cf_table, self, GetTickCount64());
        /* switch 8 (copy_path_hook.inc): the full contact, no limiter; with switch 16 only for the car that rams */
        if ((g_copy_flags & 8) && (!(g_copy_flags & 16) || cf_rams_now(self, (const unsigned char *)contact, car))) {
            if (++g_cf_unlimited == 1 || g_cf_unlimited == 100000) {
                log_begin();
                log_raw("contactfc: the local car rams another player's car and takes the full contact, without the collision limiter (");
                log_dec(g_cf_unlimited);
                log_raw(" contacts so far)\n");
            }
            return;
        }
        /* switch 16: rammed or pressed while standing, the limiter also watches contacts slower than its retail 3 mph */
        if ((g_copy_flags & 16) && g_cf_img && (settings = *(unsigned char **)(g_cf_img + CF_SETTINGS_RVA))) {
            keep = *(float *)(settings + CF_WATCH_MPH);
            *(float *)(settings + CF_WATCH_MPH) = 0.0f;
            g_cf_limiter_orig(self, contact, other);
            *(float *)(settings + CF_WATCH_MPH) = keep;
            if (++g_cf_watched == 1 || g_cf_watched == 100000) {
                log_begin();
                log_raw("contactfc: another player's car rams the local car; the collision limiter caps the hit, slow contacts too (");
                log_dec(g_cf_watched);
                log_raw(" contacts so far)\n");
            }
            return;
        }
    }
    g_cf_limiter_orig(self, contact, other);
}

#define CF_POS (-0x390) /* from the dynamics interface: the body's position (3 floats) */
#define CF_VEL (-0x350) /* and its linear velocity */
#define CF_SEEN_FRESH_MS 300u
static unsigned long g_cf_ahead;

/* switch 32: 1 when the local car (dynamics interface `body`) is about to hit another player's car (cf_will_touch) */
static int cf_about_to_hit(const unsigned char *body, unsigned long long now)
{
    int i, hit = 0;
    for (i = 0; i < CP_SEEN_SLOTS && !hit; i++) {
        if (g_cp_seen[i].car && now >= g_cp_seen[i].at && now - g_cp_seen[i].at <= CF_SEEN_FRESH_MS) {
            hit = cf_will_touch((const float *)(body + CF_POS), (const float *)(body + CF_VEL), g_cp_seen[i].pos, g_cp_seen[i].vel);
        }
    }
    return hit;
}

static void cf_send_detour(void *self, int now)
{
    unsigned char *body = *(unsigned char **)((unsigned char *)self + CF_BODY);
    unsigned long long t = GetTickCount64();
    float *speed, keep;
    if (body && (g_copy_flags & 36) == 36 && !cf_recent(g_cf_table, self, t, CF_HOLD_MS) && cf_about_to_hit(body, t)) {
        cf_touch(g_cf_table, self, t);
        if (++g_cf_ahead == 1 || g_cf_ahead == 1000) {
            log_begin();
            log_raw("contactfc: the local car is about to hit another player's car; it is sent without the look-ahead from now (");
            log_dec(g_cf_ahead);
            log_raw(" times so far)\n");
        }
    }
    if (!(g_copy_flags & 4) || !body || !cf_recent(g_cf_table, self, t, CF_HOLD_MS)) {
        g_cf_send_orig(self, now);
        return;
    }
    /* The look-ahead runs only for a moving car (speed >= 0.001); the call restores the car's state at its end, and the real speed
     * is put back after it. */
    speed = (float *)(body + CF_SPEED);
    keep = *speed;
    *speed = 0.0f;
    g_cf_send_orig(self, now);
    *speed = keep;
    /* The game stamps the sent state with the time the look-ahead reaches (now + lead + 186 ms, `mov [rbx+0x44],eax` at
     * 0x9b9671) even when it skipped it; the other game then shows this state that much later, and the car stood still there for
     * half a second each time the pause began (2026-11-18). It is stamped with the time it is from; the other game carries it on
     * to its usual 120 ms play-out itself (RemoteCarPrediction::ExtrapolateToPlayoutTime 0x9e9f70). */
    *(int *)((unsigned char *)self + CF_STAMP) = now;
    if (++g_cf_paused == 1 || g_cf_paused == 1000 || g_cf_paused == 100000) {
        log_begin();
        log_raw("contactfc: the local car touches another player's car; it is sent as it is, without the look-ahead (");
        log_dec(g_cf_paused);
        log_raw(" sends so far)\n");
    }
}

/* 1 when the hooks are in place (the pause itself follows switch 4). */
static int patch_contact_forecast(void)
{
    static const unsigned char limiter_head[13] = {0x40, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
    static const unsigned char send_head[11] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70};
    static const unsigned char speed_get[9] = {0xC5, 0xFA, 0x10, 0x81, 0x28, 0x0E, 0x00, 0x00, 0xC3};
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("contactfc", "LocalCarPrediction::vfunc_14", img, CF_LIMITER_RVA, limiter_head, sizeof limiter_head) ||
        !code_is("contactfc", "LocalCarPrediction::helper_9b9350", img, CF_SEND_RVA, send_head, sizeof send_head) ||
        !code_is("contactfc", "CCarDynamics::GetSpeedMps", img, CF_SPEED_RVA, speed_get, sizeof speed_get)) {
        return 0;
    }
    g_cf_img = img;
    if (!plat_hook("contactfc: contact watch", img + CF_LIMITER_RVA, (void *)cf_limiter_detour, (void **)&g_cf_limiter_orig)) {
        return 0;
    }
    if (!plat_hook("contactfc: send without look-ahead", img + CF_SEND_RVA, (void *)cf_send_detour, (void **)&g_cf_send_orig)) {
        return 0;
    }
    log_begin();
    log_raw("contactfc: in place; with switch 4 a car touching another player's car is sent without the look-ahead, until ");
    log_dec(CF_HOLD_MS);
    log_raw(" ms after the last contact\n");
    return 1;
}
