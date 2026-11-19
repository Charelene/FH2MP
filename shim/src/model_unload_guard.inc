/* The car model unload guard ("exception 0xc0000005 at game+0xf6d928 reading 0x0", issue #449, 2026-11-11).
 * ForzaCar's event handler (game+0xf6d0d0, called from the car's vfunc 1 at +0x298af0 during the race/world update) takes event group 10,
 * event 0x10007 for its own car key (car+0x32c, test at +0x107ea50). With the event's byte +0x20 at 0 it swaps the car to NUL_CAR_00 at once:
 *     140f6d8f8  call 0x14107ea50     ; this car's key?
 *     140f6d8fd  test al,al
 *     140f6d8ff  je   0x140f6da16     ; no: to the end
 *     140f6d905  cmp  byte [rbx+0x20],0
 *     140f6d909  je   0x140f6d921
 *     ...                             ; byte set: deferred swap (+0x51c4 = 1, +0xf7c880)
 *     140f6d921  mov  rcx,[r14+0x4d20] ; the car's model loader
 *     140f6d928  mov  rax,[rcx]       <- rcx = 0
 *     140f6d92b  call [rax+0x48]
 * and later calls the same loader's +0x40 to load NUL_CAR_00 [confirmed: disassembly, decompile]. In the crash the loader was NULL. Prod
 * server log, same second: another player in the world switched to a car pack car that is not in the game data, so every other game was
 * told to draw the placeholder car 1215 (NUL_Car_00) [confirmed: lobby 7741dc1d 01:10:43]; that 181 such swaps in a day crashed once points
 * at a timing gap in the game's own model streaming [inferred].
 * The guard hooks the je at +0xf6d8ff: when the event is this car's, the loader is NULL and the swap would be immediate, it sets ZF, so the
 * game takes the je to the function's end, the path of an event that is not this car's. The car keeps the model it has until its next change.
 * FH2MP_NO_MODELGUARD=1 switches it off. */
#include "model_unload_rule.inc"

#define MU_SITE 0xf6d8ffu
/* je +0x111 ; cmp byte [rbx+0x20],0 ; je +0x16 ; ... ; mov rcx,[r14+0x4d20] ; mov rax,[rcx] ; call [rax+0x48] */
static const unsigned char g_mu_code[12] = {0x0f, 0x84, 0x11, 0x01, 0x00, 0x00, 0x80, 0x7b, 0x20, 0x00, 0x74, 0x16};
static const unsigned char g_mu_load[13] = {0x49, 0x8b, 0x8e, 0x20, 0x4d, 0x00, 0x00, 0x48, 0x8b, 0x01, 0xff, 0x50, 0x48};
static volatile LONG g_mu_hits;

static void mu_check(FH2Context *ctx, void *user)
{
    unsigned long long loader = 1;
    unsigned char deferred = 1;
    LONG n;
    (void)user;
    if (ctx->rflags & MU_ZF) {
        return;                                                   /* not this car's event: nothing to look at */
    }
    if (!read_game_memory((const void *)(ULONG_PTR)(ctx->r14 + 0x4d20), &loader, sizeof loader) ||
        !read_game_memory((const void *)(ULONG_PTR)(ctx->rbx + 0x20), &deferred, sizeof deferred) || !mu_skip(ctx->rflags, loader, deferred)) {
        return;
    }
    ctx->rflags |= MU_ZF;
    n = InterlockedIncrement(&g_mu_hits);
    if (n <= 5 || n % 50 == 0) {
        log_begin();
        log_raw("modelguard: a car's swap to the placeholder model was skipped: the car has no model loader (it would read NULL at "
                "game+0xf6d928); it keeps its model until its next change (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
}

static int patch_model_unload_guard(void)
{
    unsigned char *img = find_fh2_image();
    FH2Hook *h;
    if (!img || !code_is("modelguard", "the car event's key test", img, MU_SITE, g_mu_code, sizeof g_mu_code) ||
        !code_is("modelguard", "the car's model loader call", img, 0xf6d921u, g_mu_load, sizeof g_mu_load)) {
        return 0;
    }
    h = g_api->hook_instruction(g_mod, plat_tag("modelguard: car model unload"), img + MU_SITE, mu_check, NULL);
    if (!h) {
        plat_failed("modelguard: car model unload", img + MU_SITE, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed("modelguard: car model unload", img + MU_SITE, "created but could not be switched on");
        return 0;
    }
    log_begin();
    log_raw("hook: modelguard: a car event that would swap a car without a model loader to the placeholder is skipped (game+0xf6d8ff)\n");
    return 1;
}
