/* FH2 1.0.0.64, online: a mod's Auto Drive (FH2 ANNA's, Extra Options') may drive the player's car in online Free Roam, not in an online event and
 * nowhere in Road Trip (a Road Trip is one long event). User decision 2026-11-08.
 *
 * Where [C: decompile; FH2 ANNA 1.1.0 source, drive.cpp]. Every Auto Drive hands the car to the game's AI with 0xF79F10(car, ai)
 * (States::SetCarAIDriving::helper_f79f10): it flips the driver mode at car+0x4D30 (1 the player, 0 the AI) and tells the driver (vtable +0x48). The
 * game's own caller is SetCarAIDriving::Execute (+0x9112E0: intros, the finish line); ANNA and Extra Options call it from their own DLLs. This hooks it:
 * a call that asks for the AI (ai != 0) from outside the game image, while this game is in a network session (picker_pi.inc ppi_in_session), is
 * refused unless the session is Free Roam: the event context ([0x34C0F40] -> 0x2129A0(holder, &shared), its +0x60; 2 is free roam, the test ANNA
 * itself makes before a sat-nav drive) says free roam and no Road Trip is on (the Road Trip manager [0x34D6AC8]'s RoadTripSynced at +0x28 is
 * set [inferred: the synced object exists only in a Road Trip session]). A refused call returns without a change, so ANNA sees "the AI didn't take
 * the car" and unwinds. The game's own calls, every call that gives the car back (ai = 0), and every call outside a session pass unchanged.
 * Evidence: the call path is confirmed from the decompile and ANNA's source; what the two state reads say in a Free Roam event and between Road
 * Trip races is NOT verified in a running game, so the first decisions are logged with both values. FH2MP_NO_AUTODRIVE_RULE=1 turns it off. */
#define AD_SET_AI_RVA 0xf79f10u
#define AD_CONTEXT_HOLDER_RVA 0x34c0f40u
#define AD_EVENT_CONTEXT_RVA 0x2129a0u
#define AD_ROADTRIP_MGR_RVA 0x34d6ac8u
#define AD_FREE_ROAM 2

typedef void (*ad_set_ai_fn)(void *car, char ai);
typedef void (*ad_context_fn)(void *holder, void **shared_out);
static ad_set_ai_fn g_ad_orig;
static const unsigned char *g_ad_img;
static SIZE_T g_ad_img_size;
static volatile LONG g_ad_refused, g_ad_allowed;

/* The event context's type (+0x60), -1 when it cannot be read. The shared_ptr the game hands out is released as the game does (strong count +8,
 * weak +0xc of the control block, its vtable slots 0 and 1). */
static int ad_context_type(void)
{
    void *holder = NULL, *shared[2] = {NULL, NULL};
    int type = -1;
    if (!read_game_memory(g_ad_img + AD_CONTEXT_HOLDER_RVA, &holder, sizeof holder) || !holder) {
        return -1;
    }
    ((ad_context_fn)(g_ad_img + AD_EVENT_CONTEXT_RVA))(holder, shared);
    if (shared[0] && !read_game_memory((unsigned char *)shared[0] + 0x60, &type, sizeof type)) {
        type = -1;
    }
    if (shared[1]) {
        unsigned char *cb = (unsigned char *)shared[1];
        void **vt = *(void ***)cb;
        if (InterlockedDecrement((volatile LONG *)(cb + 8)) == 0) {
            ((void (*)(void *))vt[0])(cb);
            if (InterlockedDecrement((volatile LONG *)(cb + 0xc)) == 0) {
                ((void (*)(void *))vt[1])(cb);
            }
        }
    }
    return type;
}

/* 1 when a Road Trip is on, 0 when not, -1 when the manager cannot be read */
static int ad_road_trip(void)
{
    const unsigned char *mgr = NULL;
    void *synced = NULL;
    if (!read_game_memory(g_ad_img + AD_ROADTRIP_MGR_RVA, &mgr, sizeof mgr) || !mgr || !read_game_memory(mgr + 0x28, &synced, sizeof synced)) {
        return -1;
    }
    return synced != NULL;
}

static void ad_log(const char *what, int context, int road_trip, volatile LONG *counter)
{
    LONG n = InterlockedIncrement(counter);
    if (n > 10 && n % 50) {
        return;
    }
    log_begin();
    log_raw("autodrive: ");
    log_raw(what);
    log_raw(" (event context ");
    if (context < 0) log_raw("unreadable"); else log_dec((unsigned long)context);
    log_raw(context == AD_FREE_ROAM ? " = free roam" : "");
    log_raw(", Road Trip ");
    log_raw(road_trip < 0 ? "unreadable" : road_trip ? "on" : "off");
    log_raw(") (#");
    log_dec((unsigned long)n);
    log_raw(")\n");
}

static void ad_hook(void *car, char ai)
{
    const unsigned char *from = (const unsigned char *)__builtin_return_address(0);
    if (ai && g_ad_img && !(from >= g_ad_img && from < g_ad_img + g_ad_img_size) && ppi_in_session(g_ad_img)) {
        int context = ad_context_type(), road_trip = ad_road_trip();
        if (context != AD_FREE_ROAM || road_trip != 0) {
            ad_log("a mod's Auto Drive refused: online it only drives in Free Roam, not in an event or Road Trip", context, road_trip, &g_ad_refused);
            return;
        }
        ad_log("a mod's Auto Drive allowed in online Free Roam", context, road_trip, &g_ad_allowed);
    }
    g_ad_orig(car, ai);
}

static int patch_autodrive_rule(void)
{
    static const unsigned char prologue[16] = {0x48, 0x8b, 0xc1, 0x48, 0x8b, 0x89, 0x10, 0x4d, 0x00, 0x00, 0x48, 0x85, 0xc9, 0x74, 0x4a, 0x48};
    unsigned char *img = find_fh2_image();
    const IMAGE_NT_HEADERS64 *nt;

    if (!img || !code_is("autodrive", "SetCarAIDriving helper", img, AD_SET_AI_RVA, prologue, sizeof prologue)) {
        return 0;
    }
    nt = (const IMAGE_NT_HEADERS64 *)(img + ((const IMAGE_DOS_HEADER *)img)->e_lfanew);
    g_ad_img_size = nt->OptionalHeader.SizeOfImage;
    g_ad_img = img;
    if (!plat_hook("autodrive: online rule", img + AD_SET_AI_RVA, (void *)ad_hook, (void **)&g_ad_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: autodrive: online, a mod's Auto Drive only drives in Free Roam, not in events or Road Trip (game+0xf79f10)\n");
    return 1;
}
