/* The travel XP guard ("exception 0xc0000094 at game+0x66a492", an integer divide by zero, issue #428: 36 reports from 2026-11-09 02:26 to 2026-11-10
 * 07:02 UTC, all launchers 1.3.0-1.3.1 with Extra Options 0.4.4, none since). game+0x66a310 is the SatNavRoutePlanned handler of the component that
 * awards XP for driving to a destination: its constructor game+0x64da90 registers XPPerKM, RoundXPTo, MinXP, MaxXP, SecondsPerKM_ClassD..X, RoundTimeTo,
 * MinTime, MaxTime and AwardTravelXP, and the getters of the first four read this+0xd0, +0xf0, +0x110 and +0x130. When the player sets a route, the
 * handler turns the route's length into XP and rounds it up to a multiple of RoundXPTo:
 *     14066a467  mov  ecx,[rbp+0xf0]            ; RoundXPTo
 *     14066a489  cvttss2si eax,xmm1 ; dec eax ; add eax,ecx ; cdq ; idiv ecx   <- ecx = 0
 *     14066a49d  ... clamped to MinXP..MaxXP, stored at +0x430
 * so a component whose RoundXPTo is 0 ends the game at the first route on the map [C: disassembly of 1.0.0.64; which entity had 0, and why only with
 * EO 0.4.4 and launchers 1.3.0-1.3.1, is not known]. The guard runs at game+0x66a48d (dec eax), before the divide: a RoundXPTo of 0 is taken as 1, no
 * rounding, so the route's XP is its own, still clamped to MinXP..MaxXP. FH2MP_NO_TRAVELXPGUARD=1 switches it off. */
#include "travelxp_rule.inc"

#define TXG_SITE 0x66a48du
static const unsigned char g_txg_load[6] = {0x8b, 0x8d, 0xf0, 0x00, 0x00, 0x00};                                  /* +0x66a467: mov ecx,[rbp+0xf0] */
static const unsigned char g_txg_code[11] = {0xc5, 0xfa, 0x2c, 0xc1, 0xff, 0xc8, 0x03, 0xc1, 0x99, 0xf7, 0xf9};   /* +0x66a489: cvttss2si eax,xmm1 ; dec ; add ; cdq ; idiv */
static volatile LONG g_txg_hits;

static void txg_check(FH2Context *ctx, void *user)
{
    LONG n;
    (void)user;
    if ((unsigned)ctx->rcx) {
        return;
    }
    ctx->rcx = txg_divisor(ctx->rcx);
    n = InterlockedIncrement(&g_txg_hits);
    if (n <= 3 || n % 100 == 0) {
        log_begin();
        log_raw("travelxp: a route's XP was not rounded: the travel XP component's RoundXPTo is 0 (it would divide by zero at game+0x66a492); "
                "the XP is the route's own, clamped to MinXP..MaxXP (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
}

static int patch_travelxp_guard(void)
{
    unsigned char *img = find_fh2_image();
    FH2Hook *h;
    if (!img || !code_is("travelxp", "the route XP's RoundXPTo load", img, 0x66a467u, g_txg_load, sizeof g_txg_load) ||
        !code_is("travelxp", "the route XP's rounding", img, 0x66a489u, g_txg_code, sizeof g_txg_code)) {
        return 0;
    }
    h = g_api->hook_instruction(g_mod, plat_tag("travelxp: route XP rounding"), img + TXG_SITE, txg_check, NULL);
    if (!h) {
        plat_failed("travelxp: route XP rounding", img + TXG_SITE, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed("travelxp: route XP rounding", img + TXG_SITE, "created but could not be switched on");
        return 0;
    }
    log_begin();
    log_raw("hook: travelxp: a route's XP with a RoundXPTo of 0 is left unrounded instead of dividing by zero (game+0x66a48d)\n");
    return 1;
}
