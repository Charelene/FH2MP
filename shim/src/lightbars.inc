/* PoliceLightbars online (suderek22/FH2_Lightbars 0.2.2, MIT: fh2-mods\PoliceLightbars.dll, and the bars' models, which the launcher puts in an
 * Extra Options mod so that they are read online too).
 *
 * What the DLL does [confirmed: its disassembly, 2026-11-21; Notion "PoliceLightbars online (investigation 2026-11-21)"]: it hooks the car
 * presentation's light update (game+0x13b6a90) and, for a car with bar lights (light instances of type 0x31 or 0x32 whose beam preset has the
 * range 8.0625, 8.125, 8.1875 or 8.25 and is amber or blue), writes the car's light channels (presentation+0x1b70 + type*4: +0x1c34, +0x1c38)
 * every frame, from one global switch and one clock: GetTickCount64 / 95 ms, dark on odd steps, else bit 2 of the step picks which of the two is
 * lit, at 1.0. The switch is [Lighting] PoliceLights in fh2-mods\PoliceLightbars.ini (default 1), written at once by its MODS page and by FH2 ANNA's
 * Car > Lights > Lightbar On/Off. Its other hook (SetIntensity, game+0x10f0550) clears a bar preset's day threshold, so bars show by day.
 * So on this game every bar flashes by this player's switch, whoever's car it is. Online the shim puts that right for other players' cars:
 *   - this player's switch goes to the others on the mod state channel (v2, MODS_ADDON_*), read from the ini when its write time changes;
 *   - an instruction hook at the start of the light loop (game+0x13b6c94: after the DLL's detour has written the channels, before the loop reads
 *     them at +0x13b6cdf, whichever hook the platform runs first) writes the two channels of every other player's car that has bar lights, from
 *     its owner's switch with the DLL's own pattern; dark while the owner's state is not known (no fresh reply, an older server, no DLL there).
 * The local car and AI cars keep the DLL's own behaviour. Runs only when PoliceLightbars.dll is loaded. FH2MP_NO_LIGHTBARS=1 turns it off.
 * The car object layout (presentation +0x10 context -> +0x380 light manager, +0x1278 asset -> +0x4be0 light instances) is ug_remote.inc's. */

#define LB_LOOP_RVA 0x13b6c94u          /* mov r13,[rsi+0x4be8] ; sub r13,[rsi+0x4be0]: the light loop's count; r14 = the presentation */
#define LB_READ_RVA 0x13b6cdbu          /* movzx eax,[rdi+4] ; vmovss xmm9,[r14+rax*4+0x1b70]: a light reads its type's channel */
#define LB_CHANNELS 0x1b70u
#define LB_TYPE_A 0x31u
#define LB_TYPE_B 0x32u
#define LB_STEP_MS 95u
#define LB_MAP_MS 250u                  /* other players' cars looked up again */
#define LB_SCAN_MS 2000u                /* a car's lights looked at again for bar lights (a lightbar fitted or taken off) */
#define LB_SEEN_MS 1500u                /* a remote car without a prediction for this long is not driven */
#define LB_INI_MS 1000u
#define LB_CARS 32

static const unsigned char g_lb_loop_code[14] = {0x4c, 0x8b, 0xae, 0xe8, 0x4b, 0x00, 0x00, 0x4c, 0x2b, 0xae, 0xe0, 0x4b, 0x00, 0x00};
static const unsigned char g_lb_read_code[14] = {0x0f, 0xb7, 0x47, 0x04, 0xc4, 0x41, 0x7a, 0x10, 0x8c, 0x86, 0x70, 0x1b, 0x00, 0x00};
static const float g_lb_ranges[4] = {8.0625f, 8.125f, 8.1875f, 8.25f};

typedef struct {
    ULONG_PTR pres;
    unsigned char owner;
    signed char bar;                    /* -1 not looked at, 0 no bar lights, 1 bar lights */
    ULONGLONG scanned;
} lb_car;

static SRWLOCK g_lb_lock = SRWLOCK_INIT;        /* g_lb: the hook only ever tries it (never waits) */
static struct {
    lb_car car[LB_CARS];
    int n;
    ULONGLONG mapped;
    unsigned char flags[256];                    /* owner -> MODS_ADDON_* of the newest fresh reply */
    unsigned char shown[256];                    /* owner -> 1 + whether its bar was last driven lit, 0 never (for the log) */
} g_lb;
static unsigned char *g_lb_img;
static volatile LONG g_lb_loaded;               /* PoliceLightbars.dll is loaded: this player's flags say so */
static volatile LONG g_lb_switch = 1;           /* its Police lights switch, as the ini says */
static volatile LONG g_lb_hooked;
static volatile LONG g_lb_driven;
static wchar_t g_lb_ini[MAX_PATH + 40];

/* The DLL's pattern at ms -> the channels of types 0x31 (a) and 0x32 (b). */
static void lb_pattern(ULONGLONG ms, int lit, float *a, float *b)
{
    ULONGLONG step = ms / LB_STEP_MS;
    *a = *b = 0.0f;
    if (!lit || (step & 1)) return;
    if (step & 4) *b = 1.0f; else *a = 1.0f;
}

/* This player's MODS_ADDON_* now. */
static unsigned char lb_flags(void)
{
    if (!g_lb_loaded) return 0;
    return (unsigned char)(MODS_ADDON_LIGHTBARS | (g_lb_switch ? MODS_ADDON_LIGHTS_ON : 0));
}

/* The switch from the ini when its write time changed (the worker, about once a second). */
static void lb_read_switch(ULONGLONG now)
{
    static ULONGLONG checked, stamp;
    static int seen;
    WIN32_FILE_ATTRIBUTE_DATA a;
    ULONGLONG t = 0;
    LONG on;
    if (!g_lb_loaded || !g_lb_ini[0] || (seen && now - checked < LB_INI_MS)) return;
    checked = now;
    if (GetFileAttributesExW(g_lb_ini, GetFileExInfoStandard, &a)) t = (ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    if (seen && t == stamp) return;
    stamp = t;
    on = GetPrivateProfileIntW(L"Lighting", L"PoliceLights", 1, g_lb_ini) != 0;   /* the DLL's own default: on */
    if (!seen || on != g_lb_switch) {
        log_begin();
        log_raw(on ? "lightbars: this player's Police lights are on: the others see the bar flash\n"
                   : "lightbars: this player's Police lights are off: the others see the bar dark\n");
    }
    seen = 1;
    InterlockedExchange(&g_lb_switch, on);
}

/* 1 when the presentation has bar lights, as the DLL tells them (its +0x13e0 and +0x14b0). */
static int lb_has_bar(const unsigned char *pres)
{
    unsigned char *context = NULL, *asset = NULL;
    ULONG_PTR manager = 0, beams = 0;
    ugr_vec inst, map, rec, pre;
    SIZE_T ninst = 0, nmap = 0, nrec = 0, npre = 0, i;
    int k;
    if (!read_game_memory(pres + UGR_PRES_CONTEXT, &context, 8) || !context || !read_game_memory(context + UGR_CONTEXT_MANAGER, &manager, 8) || !manager ||
        !read_game_memory((void *)(manager + UGR_MGR_BEAMS), &beams, 8) || !beams || !read_game_memory(pres + UGR_PRES_ASSET, &asset, 8) || !asset ||
        !ugr_read_vec((ULONG_PTR)asset + UGR_ASSET_INSTANCES, 8, 512, &inst, &ninst) ||
        !ugr_read_vec(manager + UGR_MGR_MAPPING, UGR_MAPPING, UGR_MAX_RECORDS, &map, &nmap) ||
        !ugr_read_vec(beams, UGR_RECORD, UGR_MAX_RECORDS, &rec, &nrec) || !ugr_read_vec(beams + UGR_PRESET_POOL, UGR_PRESET, UGR_MAX_RECORDS, &pre, &npre)) return 0;
    for (i = 0; i < ninst; i++) {
        unsigned char *instance = NULL, data[0x54];
        unsigned short type;
        unsigned handle, beam, preset;
        float rgb[3], range;
        if (!read_game_memory((void *)(inst.begin + i * 8), &instance, 8) || !instance || !read_game_memory(instance, data, sizeof data)) continue;
        mem_copy(&type, data + 4, 2);
        mem_copy(&handle, data + 0x50, 4);
        if ((type != LB_TYPE_A && type != LB_TYPE_B) || handle >= nmap ||
            !read_game_memory((void *)(map.begin + (ULONG_PTR)handle * UGR_MAPPING), &beam, 4) || beam >= nrec ||
            !read_game_memory((void *)(rec.begin + (ULONG_PTR)beam * UGR_RECORD + UGR_REC_PRESET), &preset, 4) || preset >= npre ||
            !read_game_memory((void *)(pre.begin + (ULONG_PTR)preset * UGR_PRESET + UGR_PRE_RGB), rgb, 12) ||
            !read_game_memory((void *)(pre.begin + (ULONG_PTR)preset * UGR_PRESET + UGR_PRE_RANGE), &range, 4)) continue;
        for (k = 0; k < 4; k++)
            if (range == g_lb_ranges[k] && (k < 2 ? rgb[0] > 0.6f && rgb[2] < 0.1f : rgb[2] > 0.6f && rgb[0] < 0.1f)) return 1;
    }
    return 0;
}

/* Other players' cars and their owners' flags, again (lock held). */
static void lb_map(ULONGLONG now)
{
    light_remote_car cars[32];
    mods_record records[MODS_MAX];
    lb_car next[LB_CARS];
    unsigned i, k, n;
    int m = 0, j;
    if (!TryAcquireSRWLockShared(&g_light_lock)) return;   /* the next frame looks again */
    for (i = 0; i < 32; i++) cars[i] = g_light_cars[i];
    ReleaseSRWLockShared(&g_light_lock);
    g_lb.mapped = now;
    for (i = 0; i < 32 && m < LB_CARS; i++) {
        unsigned char *pres;
        if (!cars[i].car || !cars[i].owner || cars[i].owner > 254 || now - cars[i].seen > LB_SEEN_MS) continue;
        if (!(pres = car_presentation(g_lb_img, cars[i].car))) continue;
        next[m].pres = (ULONG_PTR)pres; next[m].owner = (unsigned char)cars[i].owner; next[m].bar = -1; next[m].scanned = 0;
        for (j = 0; j < g_lb.n; j++)
            if (g_lb.car[j].pres == next[m].pres) { next[m].bar = g_lb.car[j].bar; next[m].scanned = g_lb.car[j].scanned; break; }
        m++;
    }
    for (j = 0; j < m; j++) g_lb.car[j] = next[j];
    g_lb.n = m;
    for (k = 0; k < 256; k++) g_lb.flags[k] = 0;
    n = mods_snapshot(records, now);
    for (k = 0; k < n; k++) g_lb.flags[records[k].owner] = records[k].addons;
}

/* The hook: one car's lights are about to be updated (the game's frame, its car update thread). */
static void lb_check(FH2Context *ctx, void *user)
{
    ULONG_PTR pres = (ULONG_PTR)ctx->r14;
    ULONGLONG now;
    int i, lit;
    unsigned char owner;
    float a, b;
    (void)user;
    if (!TryAcquireSRWLockExclusive(&g_lb_lock)) return;
    now = GetTickCount64();
    if (now - g_lb.mapped >= LB_MAP_MS) lb_map(now);
    for (i = 0; i < g_lb.n && g_lb.car[i].pres != pres; i++) {}
    if (i == g_lb.n) { ReleaseSRWLockExclusive(&g_lb_lock); return; }        /* this player's car, an AI car: the DLL's */
    if (g_lb.car[i].bar < 0 || now - g_lb.car[i].scanned >= LB_SCAN_MS) {
        g_lb.car[i].bar = (signed char)lb_has_bar((const unsigned char *)pres);
        g_lb.car[i].scanned = now;
    }
    if (!g_lb.car[i].bar) { ReleaseSRWLockExclusive(&g_lb_lock); return; }
    owner = g_lb.car[i].owner;
    lit = (g_lb.flags[owner] & MODS_ADDON_LIGHTS_ON) != 0;
    lb_pattern(now, lit, &a, &b);
    *(volatile float *)(pres + LB_CHANNELS + 4 * LB_TYPE_A) = a;
    *(volatile float *)(pres + LB_CHANNELS + 4 * LB_TYPE_B) = b;
    if (g_lb.shown[owner] != 1 + lit) {
        LONG n = InterlockedIncrement(&g_lb_driven);
        g_lb.shown[owner] = (unsigned char)(1 + lit);
        if (n <= 40 || n % 100 == 0) {
            log_begin();
            log_raw("lightbars: owner ");
            log_dec(owner);
            log_raw(lit ? "'s car has a lightbar: flashing (its owner's Police lights are on)" :
                    !g_lb.flags[owner] ? "'s car has a lightbar: dark (its owner's state is not known: no PoliceLightbars there, or no v2 reply)" :
                    "'s car has a lightbar: dark (its owner's Police lights are off)");
            log_raw(" (#");
            log_dec((unsigned long)n);
            log_raw(")\n");
        }
    }
    ReleaseSRWLockExclusive(&g_lb_lock);
}

#ifndef LB_HOOK_INSTRUCTION
static int lb_hook_instruction(const char *name, void *at, FH2InstructionFn fn)
{
    FH2Hook *h = g_api->hook_instruction(g_mod, plat_tag(name), at, fn, NULL);
    if (!h) {
        plat_failed(name, at, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed(name, at, "created but could not be switched on");
        return 0;
    }
    return 1;
}
#define LB_HOOK_INSTRUCTION lb_hook_instruction
#endif

/* After every mod has started (the mod state worker): finds PoliceLightbars and, online with the lighting hooks, hooks the light loop. */
static void lightbars_start(unsigned char *img, const wchar_t *azimuth_dir, int share)
{
    wchar_t env[8];
    const char *version;
    if (!GetModuleHandleW(L"PoliceLightbars.dll")) {
        log_begin();
        log_raw("lightbars: PoliceLightbars is not loaded (not installed, or off for online play on the MODS tab): other players' bars stay dark here\n");
        return;
    }
    g_lb_ini[0] = 0;
    if (azimuth_dir && lstrlenW(azimuth_dir) < MAX_PATH) { lstrcpyW(g_lb_ini, azimuth_dir); lstrcatW(g_lb_ini, L"\\fh2-mods\\PoliceLightbars.ini"); }
    InterlockedExchange(&g_lb_loaded, 1);
    version = g_api && g_api->mod_version ? g_api->mod_version("PoliceLightbars") : NULL;
    log_begin();
    log_raw("lightbars: PoliceLightbars ");
    log_raw(version ? version : "(version unknown)");
    log_raw(" found: this player's Police lights switch is shared");
    if (GetEnvironmentVariableW(L"FH2MP_NO_LIGHTBARS", env, 8) != 0) { log_raw("; FH2MP_NO_LIGHTBARS is set: other players' bars follow this player's switch\n"); return; }
    if (!share || !img) { log_raw("; without the lighting hooks other players' bars follow this player's switch\n"); return; }
    log_raw("\n");
    g_lb_img = img;
    if (!mods_entry_ok(img, LB_LOOP_RVA, g_lb_loop_code, sizeof g_lb_loop_code, "the car light loop") ||
        !mods_entry_ok(img, LB_READ_RVA, g_lb_read_code, sizeof g_lb_read_code, "the light channel read")) return;
    if (!LB_HOOK_INSTRUCTION("lightbars: other players' roof lightbars", img + LB_LOOP_RVA, lb_check)) return;
    InterlockedExchange(&g_lb_hooked, 1);
    log_begin();
    log_raw("hook: lightbars: other players' roof lightbars flash by their owners' switches (game+0x13b6c94)\n");
}
