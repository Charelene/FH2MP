/*
 * What the player is doing, for Encore (Discord Rich Presence and friends' status lines, FH2MP issue 539): <data\bin>\encore-activity.json,
 * rewritten when it changes, one JSON object:
 *     {"v":1,"game":"fh2","rp":"RP_FREEROAM","track":542,"x":-1234,"z":5678,"online":1}
 * rp: the game's own rich presence, as it picks it from its stateflow (media/stateflow/rich_presence.xml, global.set_rich_presence) and
 * its Showcases; track: the world loaded (542 Anthem = the mainland, 549 Storm Island; 0 not known yet); x, z: the local car's position
 * in whole metres on the map plane (absent before there is a car); online: an online Play. Encore turns it into "Tuning in Sisteron"
 * (encore-fh2 activity.rs). The game's Live presence stays off (fh2mp_mod.c, the Live gates): the value is taken where it is stored,
 * before any Live code.
 *
 * From the decompile (ghidra/export-named, image base 0x140000000):
 *   States::SetRichPresence::Execute 0x9225c0 looks the event name up in {name, value} pairs at 0x1e63f40 (38 of them, below) and calls
 *   RichPresenceModule::Set 0x66df30 (module, value): prev(+0x74) = cur(+0x70); cur(+0x70) = value; dirty(+0x78) = 1; no Live code.
 *   ShowcaseComponent 0x6bb430 calls the same setter. restore_previous="true" runs 0x66dc40 (module): cur(+0x70) = prev(+0x74).
 *   RichPresenceModule::vfunc_4 0x6622c0 (module, dt) is the module's update, on the game's thread every frame; its Live calls only run
 *   when dirty is set and CLobby+0xf0 is on, which this file never touches. It is the one hook here: once a second, after the update,
 *   it reads cur(+0x70), so a value set before the hook went in counts too.
 * The local car: LocalPlayer from the service provider at 0x34aba60 (0x21e0c0), LocalPlayer::GetCar 0x2da060 (a strong reference,
 * released through its vtable slot +0x18, as lighting_sync.inc does), its dynamics interface Car+0x4c08, the position at interface
 * -0x390 (CCarDynamics+0x10, CommitIntegratedState 0x12e28a0; docs/re/physics-and-cars.md). The world: *(0x365ad70)+8 (the track
 * id SwitchOnActiveTrack 0x93f0f0 reads).
 * Env FH2MP_NO_ACTIVITY turns it off.
 */

#define AR_UPDATE_RVA 0x6622c0u
#define AR_VALUE 0x70u
#define AR_PROVIDER_RVA 0x34aba60u
#define AR_LOCAL_PLAYER_RVA 0x21e0c0u
#define AR_GET_CAR_RVA 0x2da060u
#define AR_CAR_BODY 0x4c08u
#define AR_BODY_POS (-0x390)
#define AR_TRACK_RVA 0x365ad70u
#define AR_COUNT 38
/* a car this far (metres) from the position last written is written again */
#define AR_MOVED 50

/* The game's names for its rich presence values 0..37 (the table at 0x1e63f40, in its order). */
static const char *const g_ar_names[AR_COUNT] = {
    "RP_INVALID", "RP_SPLASH", "RP_FREEROAM", "RP_EVENT_ROADTRIP", "RP_EVENT_CIRCUIT_OR_P2P", "RP_EVENT_STREET",
    "RP_EVENT_CROSSCOUNTRY", "RP_EVENT_SHOWCASE", "RP_EVENT_SHOWCASE_GENERIC", "RP_BUCKETLIST", "RP_PHOTO", "RP_AUTOSHOW",
    "RP_PAINTSHOP", "RP_GARAGE", "RP_ONLINE_ROADTRIP", "RP_ONLINE_FREEROAM", "RP_ONLINE_KING", "RP_ONLINE_INFECTED", "RP_ONLINE_RACE",
    "RP_ONLINE_BUCKETLIST", "RP_ONLINE_WAGER", "RP_CAR_MEET", "RP_ISLAND_IE_DRIVE", "RP_ISLAND_FREEROAM", "RP_ISLAND_EVENT_P2P",
    "RP_ISLAND_EVENT_CIRCUIT", "RP_ISLAND_EVENT_CROSSCOUNTRY_P2P", "RP_ISLAND_EVENT_CROSSCOUNTRY_CIRCUIT", "RP_ISLAND_EVENT_GAUNTLET",
    "RP_ISLAND_BUCKETLIST", "RP_ISLAND_ONLINE_FREEROAM", "RP_ISLAND_ONLINE_ROADTRIP", "RP_ISLAND_ONLINE_KING",
    "RP_ISLAND_ONLINE_INFECTED", "RP_ISLAND_ONLINE_RACE", "RP_ISLAND_ONLINE_BUCKETLIST", "RP_ISLAND_ONLINE_WAGER", "RP_ISLAND_CAR_MEET",
};

/* What is known, as one record. */
typedef struct {
    int value;     /* the rich presence value, -1 before the game set one */
    int track;     /* 0 not known */
    int has_pos;
    long x, z;     /* whole metres */
    int online;
} ar_state;

static unsigned ar_put(char *out, unsigned at, unsigned cap, const char *s)
{
    while (*s && at + 1 < cap) {
        out[at++] = *s++;
    }
    return at;
}

static unsigned ar_put_int(char *out, unsigned at, unsigned cap, long v)
{
    char digits[24];
    unsigned n = 0;
    unsigned long u = v < 0 ? (unsigned long)(-(v + 1)) + 1u : (unsigned long)v;
    if (v < 0 && at + 1 < cap) {
        out[at++] = '-';
    }
    do {
        digits[n++] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u && n < sizeof digits);
    while (n && at + 1 < cap) {
        out[at++] = digits[--n];
    }
    return at;
}

/* The file's text for `s` (NUL-terminated). -> its length. */
static unsigned ar_format(char *out, unsigned cap, const ar_state *s)
{
    unsigned at = 0;
    const char *rp = s->value >= 0 && s->value < AR_COUNT ? g_ar_names[s->value] : "RP_INVALID";
    at = ar_put(out, at, cap, "{\"v\":1,\"game\":\"fh2\",\"rp\":\"");
    at = ar_put(out, at, cap, rp);
    at = ar_put(out, at, cap, "\",\"track\":");
    at = ar_put_int(out, at, cap, s->track);
    if (s->has_pos) {
        at = ar_put(out, at, cap, ",\"x\":");
        at = ar_put_int(out, at, cap, s->x);
        at = ar_put(out, at, cap, ",\"z\":");
        at = ar_put_int(out, at, cap, s->z);
    }
    at = ar_put(out, at, cap, ",\"online\":");
    at = ar_put_int(out, at, cap, s->online ? 1 : 0);
    at = ar_put(out, at, cap, "}\n");
    out[at] = 0;
    return at;
}

/* 1 when `now` is worth writing over `last`: another value, world or Play, a car found or lost, or the car `AR_MOVED` metres away. */
static int ar_changed(const ar_state *last, const ar_state *now)
{
    long dx = now->x - last->x, dz = now->z - last->z;
    if (now->value != last->value || now->track != last->track || now->online != last->online || now->has_pos != last->has_pos) {
        return 1;
    }
    return now->has_pos && (dx > AR_MOVED || dx < -AR_MOVED || dz > AR_MOVED || dz < -AR_MOVED);
}

/* A float in metres as whole metres, NaN and out of range as 0 (a torn read). */
static long ar_metres(float f)
{
    if (!(f > -1.0e7f && f < 1.0e7f)) {
        return 0;
    }
    return (long)(f < 0 ? f - 0.5f : f + 0.5f);
}

#ifndef AR_NO_HOOKS
typedef void (*ar_update_fn)(void *module, float dt);
static ar_update_fn g_ar_update_orig;
static unsigned char *g_ar_img;
static SRWLOCK g_ar_lock = SRWLOCK_INIT;
static ar_state g_ar = {-1, 0, 0, 0, 0, 0};
static ULONGLONG g_ar_sampled;
static ar_state g_ar_written = {-2, 0, 0, 0, 0, 0};
static wchar_t g_ar_path[MAX_PATH], g_ar_tmp[MAX_PATH];

/* The value the module holds now, the local car's position and the world, on the game's thread. */
static void ar_sample(void *module)
{
    int value = -1;
    void *provider = NULL, *player = NULL, *car = NULL, *body = NULL, *loader = NULL, *vtable = NULL, *release = NULL;
    float pos[3];
    int track = 0, has = 0;
    long x = 0, z = 0;
    read_game_memory((unsigned char *)module + AR_VALUE, &value, 4);
    if (read_game_memory(g_ar_img + AR_TRACK_RVA, &loader, 8) && loader) {
        read_game_memory((unsigned char *)loader + 8, &track, 4);
    }
    if (read_game_memory(g_ar_img + AR_PROVIDER_RVA, &provider, 8) && provider) {
        player = ((void *(*)(void *, int))(g_ar_img + AR_LOCAL_PLAYER_RVA))(provider, 0);
    }
    if (player) {
        ((void *(*)(void *, void **))(g_ar_img + AR_GET_CAR_RVA))(player, &car);
    }
    if (car) {
        if (read_game_memory((unsigned char *)car + AR_CAR_BODY, &body, 8) && body &&
            read_game_memory((unsigned char *)body + AR_BODY_POS, pos, sizeof pos)) {
            x = ar_metres(pos[0]);
            z = ar_metres(pos[2]);
            has = x != 0 || z != 0;
        }
        /* GetCar's reference goes back through the car's own release slot, checked to be game code (lighting_sync.inc) */
        if (read_game_memory(car, &vtable, 8) && read_game_memory((unsigned char *)vtable + 0x18, &release, 8) &&
            (unsigned char *)release >= g_ar_img + 0x1000 && (unsigned char *)release < g_ar_img + 0x1dcc000) {
            ((void (*)(void *))release)(car);
        }
    }
    AcquireSRWLockExclusive(&g_ar_lock);
    if (value >= 0 && value < AR_COUNT) {
        g_ar.value = value;
    }
    g_ar.track = track;
    g_ar.has_pos = has;
    if (has) {
        g_ar.x = x;
        g_ar.z = z;
    }
    ReleaseSRWLockExclusive(&g_ar_lock);
}

static void ar_update_detour(void *module, float dt)
{
    ULONGLONG now = GetTickCount64();
    g_ar_update_orig(module, dt);
    if (now - g_ar_sampled >= 1000) {
        if (!g_ar_sampled) {
            log_begin();
            log_raw("activity: the rich presence module updates (first sample)\n");
        }
        g_ar_sampled = now;
        ar_sample(module);
    }
}

/* 1 when the hook is in place. */
static int patch_activity_report(void)
{
    static const unsigned char update_head[19] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
                                                  0x41, 0x57, 0x48, 0x81, 0xEC, 0x30, 0x01, 0x00, 0x00};
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("activity", "RichPresenceModule::Update", img, AR_UPDATE_RVA, update_head, sizeof update_head)) {
        return 0;
    }
    g_ar_img = img;
    if (!plat_hook("activity: rich presence and car position", img + AR_UPDATE_RVA, (void *)ar_update_detour, (void **)&g_ar_update_orig)) {
        return 0;
    }
    return 1;
}

static void ar_join(wchar_t *out, const wchar_t *dir, const wchar_t *name)
{
    unsigned i = 0, k = 0;
    while (dir[k] && i < MAX_PATH - 2) {
        out[i++] = dir[k++];
    }
    if (i && out[i - 1] != L'\\') {
        out[i++] = L'\\';
    }
    for (k = 0; name[k] && i < MAX_PATH - 1; k++) {
        out[i++] = name[k];
    }
    out[i] = 0;
}

/* From the monitor (once a second): the file, when what it says changed. The first call removes a leftover of an earlier game. */
static void activity_write(int online)
{
    static int ready;
    ar_state now;
    char text[160];
    unsigned len;
    DWORD put = 0;
    HANDLE f;
    if (!ready) {
        ready = 1;
        ar_join(g_ar_path, g_dir, L"encore-activity.json");
        ar_join(g_ar_tmp, g_dir, L"encore-activity.json.tmp");
        DeleteFileW(g_ar_path);
    }
    AcquireSRWLockShared(&g_ar_lock);
    now = g_ar;
    ReleaseSRWLockShared(&g_ar_lock);
    now.online = online;
    if ((now.value <= 0 && !now.has_pos) || !ar_changed(&g_ar_written, &now)) {
        return;
    }
    len = ar_format(text, sizeof text, &now);
    f = CreateFileW(g_ar_tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!WriteFile(f, text, len, &put, NULL) || put != len) {
        CloseHandle(f);
        return;
    }
    CloseHandle(f);
    if (!MoveFileExW(g_ar_tmp, g_ar_path, MOVEFILE_REPLACE_EXISTING)) {
        return;
    }
    if (now.value != g_ar_written.value) {
        log_begin();
        log_raw("activity: ");
        log_raw(now.value >= 0 && now.value < AR_COUNT ? g_ar_names[now.value] : "?");
        log_raw(" on track ");
        log_dec((unsigned long)now.track);
        log_raw(now.has_pos ? " with a car position\n" : " without a car\n");
    }
    g_ar_written = now;
}
#endif
