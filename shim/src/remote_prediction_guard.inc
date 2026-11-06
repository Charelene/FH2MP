/* The remote prediction guard ("Azimuth stopped: exception 0xc0000005 at game+0x9224dd reading 0x0", reported 2026-11-06).
 * States::SetRemotePrediction's vfunc 4 (game+0x922350) walks the session's cars. For each one whose owner is a network player (car+0x4d08
 * == 3) it asks that CPlayer (car+0x4d00) for its user through CPlayer's vtable +0x130 (game+0x323bb0), then asks the user's vtable +0xc0
 * whether to set car+0x110:
 *     1409224d7  call [rdx+0x130]   -> rax = the user
 *     1409224dd  mov  rdx,[rax]     <- rax = 0
 *     1409224e3  call [rdx+0xc0]
 *     1409224e9  test al,al / je 1409224f9
 * For a player who isn't local, +0x130 looks the player's id up in the session's member list (game+0x291410) and returns NULL when the
 * player isn't in it, so a car whose player has just left the session (or isn't in it yet) faults here [confirmed: disassembly; inferred:
 * that a player leaving or joining as the state runs is what leaves the gap]. The game's other caller of the car's player
 * (game+0x9c77a0) checks for NULL; this one doesn't.
 * The guard hooks the instruction at 0x9224dd: when rax is NULL it points rax at a stand-in user whose +0xc0 answers false, so the game takes
 * the je, the same path as a user that says no, and car+0x110 stays as it was. Nothing else uses rax afterwards. FH2MP_NO_PREDGUARD switches
 * it off. */
#define RP_SITE 0x9224ddu

/* mov rdx,[rax] ; mov rcx,rax ; call [rdx+0xc0] ; test al,al ; je +0x0c */
static const unsigned char g_rp_code[16] = {0x48, 0x8b, 0x10, 0x48, 0x8b, 0xc8, 0xff, 0x92, 0xc0, 0x00, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x0c};

static int rp_says_no(void *self)
{
    (void)self;
    return 0;
}

static void *g_rp_vtable[0xc0 / 8 + 1];
static struct { void **vtable; unsigned char rest[0x40]; } g_rp_standin = {g_rp_vtable, {0}};
static volatile LONG g_rp_hits;

static void rp_check(FH2Context *ctx, void *user)
{
    (void)user;
    if (ctx->rax) {
        return;
    }
    ctx->rax = (unsigned long long)(ULONG_PTR)&g_rp_standin;
    if (InterlockedIncrement(&g_rp_hits) == 1) {
        log_begin();
        log_raw("hook: remote prediction guard: a car's player has no user in the session (left or not joined yet); the car is skipped "
                "instead of the game reading NULL at game+0x9224dd\n");
    }
}

static int patch_remote_prediction_guard(void)
{
    unsigned char *img = find_fh2_image();
    FH2Hook *h;
    unsigned i;
    if (!img || !code_is("predguard", "SetRemotePrediction's player check", img, RP_SITE, g_rp_code, sizeof g_rp_code)) {
        return 0;
    }
    for (i = 0; i < sizeof g_rp_vtable / sizeof g_rp_vtable[0]; ++i) {
        g_rp_vtable[i] = (void *)rp_says_no;
    }
    h = g_api->hook_instruction(g_mod, plat_tag("predguard: remote prediction player"), img + RP_SITE, rp_check, NULL);
    if (!h) {
        plat_failed("predguard: remote prediction player", img + RP_SITE, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed("predguard: remote prediction player", img + RP_SITE, "created but could not be switched on");
        return 0;
    }
    return 1;
}
