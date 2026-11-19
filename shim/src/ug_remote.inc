/* Other players' underglow: the neon FH2-Underglow puts under its owner's car (qiangqiang101/FH2-Underglow 1.1.0, src/neon_mod.cpp, MIT License,
 * Copyright (c) 2026 Bartholomew Ho), drawn under that car on this game too, from the state its owner's shim sends (mods_wire.h). Made the way
 * Underglow makes its own, on the same threads, so it inherits what Underglow has shown to work [C: Underglow's source; not verified in a running game]:
 *
 *   FH2's ForwardPlus renderer keeps a scene's lights in a LightManager (vtable game+0x2E7E600). CreateLight (slot 3, game+0x10C7E00:
 *   (mgr, uint *handle, key, index)) takes a light set registered with the manager and an entry in it; SetIntensity (game+0x10F0550),
 *   SetPosition (game+0x10F1C30, float4 world) and SetDirection (game+0x10F0DA0, a world rotation quaternion, float4) move one. manager+0x08 is
 *   the beam system (records 0x40 bytes: +0x30 preset; presets pool at +0x38, 0xC0 bytes each: +0x30 users, +0x50 rgb, +0x60 range, +0x64 power,
 *   +0x68 inner, +0x6C outer cone, +0x9C/+0xA0 day threshold; +0x100 "presets changed", the upload game+0x10F6C40 sends presets only then),
 *   manager+0x18 the glow system (lens sprites, game+0x10F04C0 sets one's intensity), manager+0x28 the handles (24 bytes: +0 beam, +4 glow record).
 *   game+0x10BE480 allocates a preset, game+0x10F1EB0 gives a record a preset, game+0x10F89B0 is the rotation of a world matrix.
 *   A car: ForzaCar+0x4D38 -> CarPresentation11 (vtable +0x2EC1450 or +0x2EC2B70, through the deferred presentation +0x2EBEF68 at +8); its world
 *   matrix at +0x1A0 (rows x, y, z, position), its context +0x10 -> manager +0x380, its asset +0x1278 -> light instances +0x4BE0 (+0x04 type,
 *   69 the tail glow; +0x50 handle).
 *
 * Here: the lights are a pool (at most UGR_POOL) made from the local car's tail glow (its key, index and preset, as Underglow copies the player's own),
 * each with a preset of its own so each has its own color; made only inside one of the game's own CreateLight calls (never racing its own making),
 * never freed, dark when no car needs them. Every frame, when the game has set the local car's tail glow (its SetIntensity, on the light thread),
 * the nearest UGR_CARS other players' cars within UGR_RANGE whose owners' underglow is lit get lights from the pool: placed on their light
 * rectangle under the car pointing down, colored by ug_effects.h, as bright as the owner's setting. Where the frame goes to the renderer (the beam
 * upload, render thread) they are placed again from the cars' place at that moment, as Underglow does against trailing at speed; with no frame for
 * 250 ms they go dark. The local car's own neon is Underglow's (when this player has it). */
#include <xmmintrin.h>

#define UGR_CREATE_RVA 0x10c7e00u
#define UGR_UPLOAD_RVA 0x10f6c40u
#define UGR_SET_INT_RVA 0x10f0550u
#define UGR_SET_POS_RVA 0x10f1c30u
#define UGR_SET_DIR_RVA 0x10f0da0u
#define UGR_GLOW_INT_RVA 0x10f04c0u
#define UGR_ATTACH_RVA 0x10f1eb0u
#define UGR_ALLOC_RVA 0x10be480u
#define UGR_TO_QUAT_RVA 0x10f89b0u
#define UGR_PRES_WORLD 0x1a0u
#define UGR_PRES_CONTEXT 0x10u
#define UGR_CONTEXT_MANAGER 0x380u
#define UGR_PRES_ASSET 0x1278u
#define UGR_ASSET_INSTANCES 0x4be0u
#define UGR_MGR_BEAMS 0x08u
#define UGR_MGR_GLOWS 0x18u
#define UGR_MGR_MAPPING 0x28u
#define UGR_MAPPING 24u
#define UGR_TYPE_TAIL_GLOW 69
#define UGR_PRESET_POOL 0x38u
#define UGR_BEAM_DIRTY 0x100u
#define UGR_RECORD 0x40u
#define UGR_REC_PRESET 0x30u
#define UGR_PRESET 0xc0u
#define UGR_PRE_USERS 0x30u
#define UGR_PRE_RGB 0x50u
#define UGR_PRE_RANGE 0x60u
#define UGR_PRE_THRESHOLD 0x9cu
#define UGR_MAX_RECORDS 100000u
#define UGR_HANDLES 65536u
#define UGR_POOL 192               /* lights for every other player's car together */
#define UGR_CARS 8                 /* the nearest cars that get neon */
#define UGR_RANGE 150.0f           /* metres: a car further away gets none */
#define UGR_PREBUILD 64            /* lights made ahead as soon as the local car is known: the game makes lights only now and then, so waiting for a lit car came too late */
#define UGR_BUILD_BATCH 32         /* lights made in one game CreateLight call */
#define UGR_RESOLVE_MS 500u
#define UGR_VALIDATE_MS 500u        /* how often the pool's lights are checked to still be ours (each check costs two reads per light) */
#define UGR_DARK_MS 250u
#define UGR_SEEN_MS 1000u

typedef void *(*ugr_create_fn)(void *mgr, unsigned *handle, unsigned key, unsigned index);
typedef void (*ugr_upload_fn)(void *system, void *graphics);
typedef void (*ugr_int_fn)(void *mgr, unsigned handle, float v);
typedef void (*ugr_vec_fn)(void *mgr, unsigned handle, const float *v);
typedef void (*ugr_glow_fn)(void *glows, unsigned record, float v);
typedef void (*ugr_attach_fn)(void *beams, unsigned record, unsigned preset);
typedef unsigned (*ugr_alloc_fn)(void *pool);
typedef __m128 (*ugr_quat_fn)(const float *matrix);

typedef struct { ULONG_PTR begin, end, cap; } ugr_vec;
typedef struct {
    unsigned handle, preset;
    float rgb[3];
    unsigned char owner;           /* 0: free */
    unsigned char lit;             /* given an intensity above 0 since it was last darkened */
    unsigned short index;          /* its light in the owner's ring */
} ugr_light;
typedef struct { unsigned char owner; unsigned char *pres; ug_style style; int n; } ugr_placed;

static ugr_create_fn g_ugr_create_orig;
static ugr_upload_fn g_ugr_upload_orig;
static ugr_int_fn g_ugr_int_orig;
static ugr_vec_fn g_ugr_pos, g_ugr_dir;
static ugr_glow_fn g_ugr_glow;
static ugr_attach_fn g_ugr_attach;
static ugr_alloc_fn g_ugr_alloc;
static ugr_quat_fn g_ugr_quat;
static unsigned char *g_ugr_img;
static volatile LONG g_ugr_enabled, g_ugr_trigger;     /* the trigger: the local car's tail glow handle (+1), 0 none */
static volatile LONG_PTR g_ugr_trigger_mgr;
static unsigned g_ugr_key[UGR_HANDLES], g_ugr_index[UGR_HANDLES];
static ULONG_PTR g_ugr_mgr[UGR_HANDLES];
static SRWLOCK g_ugr_lock = SRWLOCK_INIT;                /* g_ugr and the lights: only whoever holds it (always TryAcquire: never waits) */
static struct {
    ULONG_PTR pres, manager, beams, glows;               /* the local car's (the reference) */
    unsigned tkey, tindex, tpreset;
    int tok;
    ULONGLONG resolved_at, frame_at, validated_at;
    ugr_light light[UGR_POOL];
    int count, want;
    unsigned spare[UGR_POOL];
    int spares;
    ugr_placed placed[UGR_CARS];
    int nplaced;
    unsigned built, problems;
} g_ugr;
static ug_audio g_ugr_audio[256];

static int ugr_read_vec(ULONG_PTR at, SIZE_T stride, SIZE_T limit, ugr_vec *v, SIZE_T *count)
{
    if (!read_game_memory((const void *)at, v, sizeof *v) || !v->begin || v->end < v->begin || (v->end - v->begin) % stride) return 0;
    *count = (v->end - v->begin) / stride;
    return *count <= limit;
}
static int ugr_finite(const float *v, int n) { int i; for (i = 0; i < n; i++) if (!(v[i] == v[i] && v[i] - v[i] == 0.0f)) return 0; return 1; }

/* the three tables of this frame: handles, beam records, beam presets */
typedef struct { ugr_vec map, rec, pre; SIZE_T nmap, nrec, npre; } ugr_tables;
static int ugr_tables_read(ugr_tables *t)
{
    return ugr_read_vec(g_ugr.manager + UGR_MGR_MAPPING, UGR_MAPPING, UGR_MAX_RECORDS, &t->map, &t->nmap) &&
           ugr_read_vec(g_ugr.beams, UGR_RECORD, UGR_MAX_RECORDS, &t->rec, &t->nrec) &&
           ugr_read_vec(g_ugr.beams + UGR_PRESET_POOL, UGR_PRESET, UGR_MAX_RECORDS, &t->pre, &t->npre);
}
static int ugr_mapping(const ugr_tables *t, unsigned handle, unsigned *beam, unsigned *glow)
{
    unsigned e[2];
    if (handle >= t->nmap || !read_game_memory((const void *)(t->map.begin + (ULONG_PTR)handle * UGR_MAPPING), e, 8)) return 0;
    *beam = e[0]; *glow = e[1];
    return 1;
}
static unsigned char *ugr_preset_at(const ugr_tables *t, unsigned preset) { return preset < t->npre ? (unsigned char *)(t->pre.begin + (ULONG_PTR)preset * UGR_PRESET) : NULL; }

/* One of our presets: the template's (the tail glow's), recolored, Underglow's [Neon] cone (range 1, power 4, inner 60, outer 170), always on. */
static int ugr_write_preset(const ugr_tables *t, unsigned preset, const float rgb[3])
{
    unsigned char *ours = ugr_preset_at(t, preset), *tmpl = ugr_preset_at(t, g_ugr.tpreset), b[UGR_PRESET], keep[8];
    static const float cone[4] = {1.0f, 4.0f, 60.0f, 170.0f};
    unsigned k;
    if (!ours || !tmpl || !read_game_memory(tmpl, b, sizeof b) || !read_game_memory(ours + UGR_PRE_USERS, keep, 8)) return 0;
    for (k = 0; k < 8; k++) b[UGR_PRE_USERS + k] = keep[k];
    mem_copy(b + UGR_PRE_RGB, rgb, 12);
    mem_copy(b + UGR_PRE_RANGE, cone, 16);
    for (k = 0; k < 8; k++) b[UGR_PRE_THRESHOLD + k] = 0;
    mem_copy(ours + 0x10, b + 0x10, sizeof b - 0x10);      /* not the pool links at +0 */
    return 1;
}
static void ugr_dirty(void) { *(volatile unsigned char *)(g_ugr.beams + UGR_BEAM_DIRTY) = 1; }

static void ugr_glow_off(const ugr_tables *t, unsigned handle)
{
    unsigned beam, glow;
    if (g_ugr.glows && ugr_mapping(t, handle, &beam, &glow) && glow != ~0u) g_ugr_glow((void *)g_ugr.glows, glow, 0.0f);
}
static void ugr_dark(const ugr_tables *t, ugr_light *l)
{
    g_ugr_int_orig((void *)g_ugr.manager, l->handle, 0.0f);
    ugr_glow_off(t, l->handle);
    l->lit = 0;
}

/* ---- the reference: the local car's tail glow (render thread, every UGR_RESOLVE_MS) ---- */
static void ugr_resolve(ULONG_PTR system, ULONGLONG now)
{
    void *car;
    unsigned char *pres, *context = NULL, *asset = NULL;
    ULONG_PTR manager = 0, beams = 0, glows = 0;
    ugr_vec inst;
    SIZE_T ninst = 0, i;
    ugr_tables t;
    AcquireSRWLockShared(&g_light_lock);
    car = g_light.local_hint;
    ReleaseSRWLockShared(&g_light_lock);
    pres = car_presentation(g_ugr_img, car);
    if (!pres || !read_game_memory(pres + UGR_PRES_CONTEXT, &context, 8) || !context || !read_game_memory(context + UGR_CONTEXT_MANAGER, &manager, 8) ||
        !manager || !read_game_memory((void *)(manager + UGR_MGR_BEAMS), &beams, 8) || beams != system) return;   /* another light system's upload */
    g_ugr.resolved_at = now;
    if (!read_game_memory((void *)(manager + UGR_MGR_GLOWS), &glows, 8) || !read_game_memory(pres + UGR_PRES_ASSET, &asset, 8) || !asset ||
        !ugr_read_vec((ULONG_PTR)asset + UGR_ASSET_INSTANCES, 8, 512, &inst, &ninst)) return;
    if (manager != g_ugr.manager || beams != g_ugr.beams) {          /* another light manager: start over (its lights are gone with it) */
        g_ugr.count = g_ugr.spares = g_ugr.nplaced = 0;
        g_ugr.want = UGR_PREBUILD;
        g_ugr.manager = manager; g_ugr.beams = beams;
    }
    g_ugr.glows = glows;
    g_ugr.pres = (ULONG_PTR)pres;
    if (!ugr_tables_read(&t)) return;
    for (i = 0; i < ninst; i++) {
        unsigned char *instance = NULL, data[0x54];
        unsigned short type;
        unsigned handle, beam, glow, preset;
        if (!read_game_memory((void *)(inst.begin + i * 8), &instance, 8) || !instance || !read_game_memory(instance, data, sizeof data)) continue;
        mem_copy(&type, data + 4, 2);
        mem_copy(&handle, data + 0x50, 4);
        if (type != UGR_TYPE_TAIL_GLOW || !ugr_mapping(&t, handle, &beam, &glow) || beam >= t.nrec ||
            !read_game_memory((void *)(t.rec.begin + (ULONG_PTR)beam * UGR_RECORD + UGR_REC_PRESET), &preset, 4) || preset >= t.npre) continue;
        InterlockedExchangePointer((void *volatile *)&g_ugr_trigger_mgr, (void *)manager);
        InterlockedExchange(&g_ugr_trigger, (LONG)(handle + 1));
        if (handle < UGR_HANDLES && g_ugr_mgr[handle] == manager) {
            g_ugr.tkey = g_ugr_key[handle]; g_ugr.tindex = g_ugr_index[handle]; g_ugr.tpreset = preset; g_ugr.tok = 1;
            if (g_ugr.want < UGR_PREBUILD && g_mods_share) g_ugr.want = UGR_PREBUILD;
        } else if (!g_ugr.tok && !(g_ugr.problems & 1)) {
            g_ugr.problems |= 1;
            log_begin(); log_raw("underglow: other players' neon waits: this car's lights were made before the shim watched (change car or fast travel once)\n");
        }
        return;
    }
}

/* ---- the frame (light thread, lock held) ---- */
typedef struct { unsigned char owner; unsigned char *pres; float world[16]; float dist; const unsigned char *ug; } ugr_car;

static void ugr_frame(void)
{
    static __attribute__((aligned(16))) float world[16], pos[4], rot[4];
    static const float down[4] = {0.70710678f, 0.0f, 0.0f, 0.70710678f};   /* the light's +z turned to -y */
    mods_record records[MODS_MAX];
    ugr_car cars[UGR_CARS], cand;
    ugr_tables t;
    ULONGLONG now = GetTickCount64();
    double secs = (double)(now % 86400000ull) / 1000.0;
    float me[3];
    unsigned k, n;
    int i, j, ncars = 0, want = 0, dirty = 0;
    g_ugr.frame_at = now;
    if (!g_ugr.tok) return;
    n = g_mods_share ? mods_snapshot(records, now) : 0;
    for (k = 0; k < n && !(records[k].ug[0] & 1); k++) {}
    if (k == n) {                                      /* nobody's underglow is lit (the usual case): no reads, only lights left lit go dark */
        int any = 0;
        for (j = 0; j < g_ugr.count; j++) any |= g_ugr.light[j].lit;
        if (any && ugr_tables_read(&t)) for (j = 0; j < g_ugr.count; j++) { if (g_ugr.light[j].lit) ugr_dark(&t, &g_ugr.light[j]); g_ugr.light[j].owner = 0; }
        g_ugr.nplaced = 0;
        g_ugr.want = UGR_PREBUILD;
        return;
    }
    if (!ugr_tables_read(&t)) return;
    if (now - g_ugr.validated_at >= UGR_VALIDATE_MS) {
        /* lights the game took back (their records no longer use our preset) are forgotten; their presets kept for new lights */
        g_ugr.validated_at = now;
        for (i = j = 0; i < g_ugr.count; i++) {
            ugr_light *l = &g_ugr.light[i];
            unsigned beam, glow, preset = ~0u;
            if (ugr_mapping(&t, l->handle, &beam, &glow) && beam < t.nrec &&
                read_game_memory((void *)(t.rec.begin + (ULONG_PTR)beam * UGR_RECORD + UGR_REC_PRESET), &preset, 4) && preset == l->preset) g_ugr.light[j++] = *l;
            else if (l->preset < t.npre && g_ugr.spares < UGR_POOL) g_ugr.spare[g_ugr.spares++] = l->preset;
        }
        g_ugr.count = j;
    }
    /* the cars: lit underglow, mapped, within range, the nearest first */
    if (!read_game_memory((void *)(g_ugr.pres + UGR_PRES_WORLD), world, sizeof world) || !ugr_finite(world, 16)) return;
    me[0] = world[12]; me[1] = world[13]; me[2] = world[14];
    for (k = 0; k < n; k++) {
        const unsigned char *ug = records[k].ug;
        void *car;
        float dx, dy, dz;
        if (!(ug[0] & 1) || !ug[1] || !(car = mods_remote_car(records[k].owner, records[k].car, now, UGR_SEEN_MS)) ||
            !(cand.pres = car_presentation(g_ugr_img, car)) || !read_game_memory(cand.pres + UGR_PRES_WORLD, cand.world, sizeof cand.world) ||
            !ugr_finite(cand.world, 16)) continue;
        dx = cand.world[12] - me[0]; dy = cand.world[13] - me[1]; dz = cand.world[14] - me[2];
        cand.dist = dx * dx + dy * dy + dz * dz;
        if (cand.dist > UGR_RANGE * UGR_RANGE) continue;
        cand.owner = records[k].owner;
        cand.ug = ug;
        for (i = ncars; i > 0 && cars[i - 1].dist > cand.dist; i--) if (i < UGR_CARS) cars[i] = cars[i - 1];
        if (i < UGR_CARS) { cars[i] = cand; if (ncars < UGR_CARS) ncars++; }
    }
    /* lights: kept by their car, freed when it no longer needs them, then given to the cars that lack some, the nearest first */
    {
        unsigned char used[UGR_CARS][UG_MAX_NEON];
        int need[UGR_CARS];
        for (i = 0; i < ncars; i++) {
            need[i] = 2 * cars[i].ug[2] + cars[i].ug[3] + cars[i].ug[4];
            for (j = 0; j < UG_MAX_NEON; j++) used[i][j] = 0;
        }
        for (j = 0; j < g_ugr.count; j++) {
            ugr_light *l = &g_ugr.light[j];
            int c = -1;
            for (i = 0; i < ncars && c < 0; i++) if (cars[i].owner == l->owner) c = i;
            if (c < 0 || l->index >= need[c] || used[c][l->index]) {
                if (l->lit) ugr_dark(&t, l);
                l->owner = 0;
                continue;
            }
            used[c][l->index] = 1;
        }
        for (i = 0; i < ncars; i++) {
            int idx;
            want += need[i];
            for (idx = 0, j = 0; idx < need[i]; idx++) {
                if (used[i][idx]) continue;
                while (j < g_ugr.count && g_ugr.light[j].owner) j++;
                if (j == g_ugr.count) break;
                g_ugr.light[j].owner = cars[i].owner;
                g_ugr.light[j].index = (unsigned short)idx;
                used[i][idx] = 1;
            }
        }
        g_ugr.want = want < UGR_PREBUILD ? UGR_PREBUILD : want < UGR_POOL ? want : UGR_POOL;
    }
    /* each car's ring */
    g_ugr.nplaced = 0;
    for (i = 0; i < ncars; i++) {
        ugr_placed *pl = &g_ugr.placed[g_ugr.nplaced++];
        ug_style s;
        ug_pixel px[UG_MAX_NEON];
        ug_audio *au = &g_ugr_audio[cars[i].owner];
        __attribute__((aligned(16))) float q[4];
        float bright = (float)cars[i].ug[15] / 64.0f;
        unsigned parts = cars[i].ug[1];
        int total;
        ug_style_of(cars[i].ug, &s);
        total = 2 * s.side + s.front + s.rear;
        pl->owner = cars[i].owner; pl->pres = cars[i].pres; pl->style = s; pl->n = total;
        ug_pixels(&s, cars[i].ug + 5, parts, px, total);
        ug_audio_frame(au, s.speed, secs, now);
        for (k = 0; k < 16; k++) world[k] = cars[i].world[k];
        _mm_store_ps(q, g_ugr_quat(world));
        if (!ugr_finite(q, 4)) continue;
        {
            float r[4] = {q[3] * down[0] + q[0] * down[3] + q[1] * down[2] - q[2] * down[1], q[3] * down[1] - q[0] * down[2] + q[1] * down[3] + q[2] * down[0],
                          q[3] * down[2] + q[0] * down[1] - q[1] * down[0] + q[2] * down[3], q[3] * down[3] - q[0] * down[0] - q[1] * down[1] - q[2] * down[2]};
            for (k = 0; k < 4; k++) rot[k] = r[k];
        }
        for (j = 0; j < g_ugr.count; j++) {
            ugr_light *l = &g_ugr.light[j];
            float rgb[3], level, intensity, local[3], tv[3], w[3];
            unsigned char *preset;
            if (l->owner != cars[i].owner || l->index >= total) continue;
            ug_effect_now(&s, au, cars[i].ug[5 + px[l->index].zone], &px[l->index], secs, rgb, &level);
            if (!((parts >> px[l->index].zone) & 1)) level = 0.0f;
            intensity = level > 0.01f ? bright * level : 0.0f;
            if (intensity > 0.0f && (fabsf(rgb[0] - l->rgb[0]) + fabsf(rgb[1] - l->rgb[1]) + fabsf(rgb[2] - l->rgb[2]) > 0.002f) &&
                (preset = ugr_preset_at(&t, l->preset)) != NULL) {
                mem_copy(preset + UGR_PRE_RGB, rgb, 12);
                ug_copy3(l->rgb, rgb);
                dirty = 1;
            }
            ug_edge_point(&s, l->index, local);
            tv[0] = 2.0f * (q[1] * local[2] - q[2] * local[1]); tv[1] = 2.0f * (q[2] * local[0] - q[0] * local[2]); tv[2] = 2.0f * (q[0] * local[1] - q[1] * local[0]);
            w[0] = local[0] + q[3] * tv[0] + (q[1] * tv[2] - q[2] * tv[1]);
            w[1] = local[1] + q[3] * tv[1] + (q[2] * tv[0] - q[0] * tv[2]);
            w[2] = local[2] + q[3] * tv[2] + (q[0] * tv[1] - q[1] * tv[0]);
            pos[0] = cars[i].world[12] + w[0]; pos[1] = cars[i].world[13] + w[1]; pos[2] = cars[i].world[14] + w[2]; pos[3] = 1.0f;
            g_ugr_pos((void *)g_ugr.manager, l->handle, pos);
            g_ugr_dir((void *)g_ugr.manager, l->handle, rot);
            g_ugr_int_orig((void *)g_ugr.manager, l->handle, intensity);
            ugr_glow_off(&t, l->handle);
            l->lit = intensity > 0.0f;
        }
    }
    if (dirty) ugr_dirty();
    if (ncars && !(g_ugr.problems & 2)) {
        g_ugr.problems |= 2;
        log_begin(); log_raw("underglow: another player's neon shown (owner "); log_dec(cars[0].owner); log_raw(", "); log_dec((unsigned long)g_ugr.count);
        log_raw(" light(s) in the pool)\n");
    }
}

/* The lights placed again from the cars' place now (render thread, lock held): Underglow's place_again. */
static void ugr_place_again(void)
{
    static __attribute__((aligned(16))) float world[16], pos[4], rot[4];
    static const float down[4] = {0.70710678f, 0.0f, 0.0f, 0.70710678f};
    int i, j;
    for (i = 0; i < g_ugr.nplaced; i++) {
        const ugr_placed *pl = &g_ugr.placed[i];
        __attribute__((aligned(16))) float q[4];
        if (!read_game_memory(pl->pres + UGR_PRES_WORLD, world, sizeof world) || !ugr_finite(world, 16)) continue;
        _mm_store_ps(q, g_ugr_quat(world));
        if (!ugr_finite(q, 4)) continue;
        rot[0] = q[3] * down[0] + q[0] * down[3] + q[1] * down[2] - q[2] * down[1];
        rot[1] = q[3] * down[1] - q[0] * down[2] + q[1] * down[3] + q[2] * down[0];
        rot[2] = q[3] * down[2] + q[0] * down[1] - q[1] * down[0] + q[2] * down[3];
        rot[3] = q[3] * down[3] - q[0] * down[0] - q[1] * down[1] - q[2] * down[2];
        for (j = 0; j < g_ugr.count; j++) {
            const ugr_light *l = &g_ugr.light[j];
            float local[3], tv[3], w[3];
            if (l->owner != pl->owner || l->index >= pl->n) continue;
            ug_edge_point(&pl->style, l->index, local);
            tv[0] = 2.0f * (q[1] * local[2] - q[2] * local[1]); tv[1] = 2.0f * (q[2] * local[0] - q[0] * local[2]); tv[2] = 2.0f * (q[0] * local[1] - q[1] * local[0]);
            w[0] = local[0] + q[3] * tv[0] + (q[1] * tv[2] - q[2] * tv[1]);
            w[1] = local[1] + q[3] * tv[1] + (q[2] * tv[0] - q[0] * tv[2]);
            w[2] = local[2] + q[3] * tv[2] + (q[0] * tv[1] - q[1] * tv[0]);
            pos[0] = world[12] + w[0]; pos[1] = world[13] + w[1]; pos[2] = world[14] + w[2]; pos[3] = 1.0f;
            g_ugr_pos((void *)g_ugr.manager, l->handle, pos);
            g_ugr_dir((void *)g_ugr.manager, l->handle, rot);
        }
    }
}

/* ---- making lights (inside one of the game's CreateLight calls, lock held) ---- */
static void ugr_build(void *mgr)
{
    static const float start[3] = {0.0f, 1.0f, 1.0f};
    ugr_tables t;
    int made = 0;
    if (!g_ugr.tok || (ULONG_PTR)mgr != g_ugr.manager || !ugr_tables_read(&t)) return;
    while (g_ugr.count < g_ugr.want && g_ugr.count < UGR_POOL && made < UGR_BUILD_BATCH) {
        unsigned p = g_ugr.spares ? g_ugr.spare[--g_ugr.spares] : ~0u, h = ~0u, beam, glow;
        unsigned char *preset;
        if (p == ~0u) {
            p = g_ugr_alloc((void *)(g_ugr.beams + UGR_PRESET_POOL));
            if (p == ~0u || !ugr_tables_read(&t) || !(preset = ugr_preset_at(&t, p))) {
                if (!(g_ugr.problems & 4)) { g_ugr.problems |= 4; log_begin(); log_raw("underglow: the game has no free light preset for other players' neon\n"); }
                g_ugr.want = g_ugr.count;
                break;
            }
            {
                static const unsigned no_users[2] = {0, 0};
                mem_copy(preset + UGR_PRE_USERS, no_users, 8);
            }
        }
        if (!ugr_write_preset(&t, p, start)) { if (g_ugr.spares < UGR_POOL) g_ugr.spare[g_ugr.spares++] = p; break; }
        g_ugr_create_orig(mgr, &h, g_ugr.tkey, g_ugr.tindex);
        if (h == ~0u || !ugr_tables_read(&t) || !ugr_mapping(&t, h, &beam, &glow) || beam == ~0u) {
            if (g_ugr.spares < UGR_POOL) g_ugr.spare[g_ugr.spares++] = p;
            if (!(g_ugr.problems & 8)) { g_ugr.problems |= 8; log_begin(); log_raw("underglow: CreateLight gave other players' neon no light\n"); }
            g_ugr.want = g_ugr.count;
            break;
        }
        g_ugr_attach((void *)g_ugr.beams, beam, p);
        g_ugr.light[g_ugr.count].handle = h;
        g_ugr.light[g_ugr.count].preset = p;
        ug_copy3(g_ugr.light[g_ugr.count].rgb, start);
        g_ugr.light[g_ugr.count].owner = 0;
        g_ugr.light[g_ugr.count].index = 0;
        ugr_dark(&t, &g_ugr.light[g_ugr.count]);
        g_ugr.count++;
        made++;
    }
    if (made) {
        ugr_dirty();
        g_ugr.built += (unsigned)made;
        if (g_ugr.built == (unsigned)made) {
            log_begin(); log_raw("underglow: "); log_dec((unsigned long)made); log_raw(" light(s) made for other players' neon\n");
        }
    }
}

/* ---- the hooks ---- */
static void *ugr_create(void *mgr, unsigned *handle, unsigned key, unsigned index)
{
    void *r = g_ugr_create_orig(mgr, handle, key, index);
    unsigned h = ~0u;
    if (handle && read_game_memory(handle, &h, 4) && h < UGR_HANDLES) {
        g_ugr_key[h] = key; g_ugr_index[h] = index; g_ugr_mgr[h] = (ULONG_PTR)mgr;
    }
    if (g_ugr_enabled && g_ugr.want > g_ugr.count && TryAcquireSRWLockExclusive(&g_ugr_lock)) {
        ugr_build(mgr);
        ReleaseSRWLockExclusive(&g_ugr_lock);
    }
    return r;
}

static void ugr_set_int(void *mgr, unsigned handle, float v)
{
    g_ugr_int_orig(mgr, handle, v);
    if (g_ugr_enabled && (LONG)(handle + 1) == g_ugr_trigger && (LONG_PTR)mgr == g_ugr_trigger_mgr && TryAcquireSRWLockExclusive(&g_ugr_lock)) {
        if ((ULONG_PTR)mgr == g_ugr.manager) ugr_frame();
        ReleaseSRWLockExclusive(&g_ugr_lock);
    }
}

static void ugr_upload(void *system, void *graphics)
{
    if (g_ugr_enabled && TryAcquireSRWLockExclusive(&g_ugr_lock)) {
        ULONGLONG now = GetTickCount64();
        if (now - g_ugr.resolved_at >= UGR_RESOLVE_MS) ugr_resolve((ULONG_PTR)system, now);   /* only the local car's light system counts */
        if ((ULONG_PTR)system == g_ugr.beams && g_ugr.manager) {
            if (g_ugr.frame_at && now - g_ugr.frame_at > UGR_DARK_MS) {
                ugr_tables t;
                int j, any = 0;
                for (j = 0; j < g_ugr.count; j++) any |= g_ugr.light[j].lit;
                if (any && ugr_tables_read(&t)) for (j = 0; j < g_ugr.count; j++) if (g_ugr.light[j].lit) ugr_dark(&t, &g_ugr.light[j]);
                g_ugr.nplaced = 0;
            } else ugr_place_again();
        }
        ReleaseSRWLockExclusive(&g_ugr_lock);
    }
    g_ugr_upload_orig(system, graphics);
}

static int patch_ug_remote(unsigned char *img)
{
    static const struct { unsigned rva; unsigned char bytes[8]; const char *what; } fns[] = {
        {UGR_CREATE_RVA, {0x48, 0x89, 0x5c, 0x24, 0x20, 0x44, 0x89, 0x44}, "LightManager::CreateLight"},
        {UGR_UPLOAD_RVA, {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55}, "the beam preset upload"},
        {UGR_SET_INT_RVA, {0x48, 0x83, 0xec, 0x28, 0x44, 0x8b, 0xc2, 0x4c}, "LightManager::SetIntensity"},
        {UGR_SET_POS_RVA, {0x40, 0x53, 0x48, 0x83, 0xec, 0x10, 0x49, 0x8b}, "LightManager::SetPosition"},
        {UGR_SET_DIR_RVA, {0x40, 0x53, 0x48, 0x83, 0xec, 0x10, 0x49, 0x8b}, "LightManager::SetDirection"},
        {UGR_GLOW_INT_RVA, {0xc5, 0xf8, 0x28, 0xda, 0x83, 0xfa, 0xff, 0x74}, "the glow intensity setter"},
        {UGR_ATTACH_RVA, {0x48, 0x83, 0xec, 0x28, 0x44, 0x8b, 0xca, 0x4c}, "the preset attach"},
        {UGR_ALLOC_RVA, {0x48, 0x8b, 0x51, 0x18, 0x41, 0x83, 0xca, 0xff}, "the preset allocator"},
        {UGR_TO_QUAT_RVA, {0x48, 0x8b, 0xc4, 0x48, 0x83, 0xec, 0x68, 0xc5}, "the matrix rotation"},
    };
    unsigned i;
    g_ugr_img = img;
    /* a function only called (not hooked) that another mod hooked is called through that hook, as the game calls it */
    for (i = 0; i < sizeof fns / sizeof fns[0]; i++) if (!mods_entry_ok(img, fns[i].rva, fns[i].bytes, 8, fns[i].what)) return 0;
    g_ugr_pos = (ugr_vec_fn)(void *)(img + UGR_SET_POS_RVA);
    g_ugr_dir = (ugr_vec_fn)(void *)(img + UGR_SET_DIR_RVA);
    g_ugr_glow = (ugr_glow_fn)(void *)(img + UGR_GLOW_INT_RVA);
    g_ugr_attach = (ugr_attach_fn)(void *)(img + UGR_ATTACH_RVA);
    g_ugr_alloc = (ugr_alloc_fn)(void *)(img + UGR_ALLOC_RVA);
    g_ugr_quat = (ugr_quat_fn)(void *)(img + UGR_TO_QUAT_RVA);
    if (!plat_hook("underglow: light creation", img + UGR_CREATE_RVA, (void *)ugr_create, (void **)&g_ugr_create_orig) ||
        !plat_hook("underglow: light intensity", img + UGR_SET_INT_RVA, (void *)ugr_set_int, (void **)&g_ugr_int_orig) ||
        !plat_hook("underglow: beam upload", img + UGR_UPLOAD_RVA, (void *)ugr_upload, (void **)&g_ugr_upload_orig)) return 0;
    InterlockedExchange(&g_ugr_enabled, 1);
    log_begin(); log_raw("hook: underglow: other players' neon (game+0x10c7e00, +0x10f0550, +0x10f6c40)\n");
    return 1;
}
