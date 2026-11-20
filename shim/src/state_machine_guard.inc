/* The animation state machine guard ("exception 0xc0000005 at game+0x1710ec4 reading 0x0", issue #502, 2026-11-15).
 * The car animations use Granny's gstate (2.9.20.2, d:\p4\anthem_rc\external\Granny\2.9.20.2\gstate\gstate_state_machine.cpp). The
 * destructor of gstate::state_machine (game+0x1710e80, vftable game+0x2f32ff0) frees its nodes, one per node of its definition:
 *     141710eb3  mov  rax,[rcx+0x5c]      ; the definition (in the animation file's data)
 *     141710eb7  cmp  [rax+0x14],r14d     ; its node count
 *     141710ebb  jle  0x141710f10         ; none: straight to freeing the list
 *     141710ebd  mov  esi,r14d
 *     141710ec0  mov  rax,[rbx+0x6c]      ; this machine's node list
 *     141710ec4  mov  rdi,[rsi+rax]       <- rax = 0
 *     ...                                 ; each node's destructor, the next node
 *     141710f10  mov  r8,[rbx+0x6c]       ; free(list): Granny's free (+0x1717030) returns at once on NULL
 * [confirmed: disassembly]. In the crash the definition said it had nodes, the list was NULL, and the machine's +0x74 was NULL too [confirmed:
 * the crash dump]. The constructor (+0x1710b20) can leave it so: it sizes the list from the definition's count, allocates nothing for a
 * count of 0, and when a node's class is not found ("Found an invalid conditional, possibly obsolete class?") it removes that node from the
 * definition itself, which the file's other machines share, and frees the list once none is left. A count above 0 with no list then means
 * the definition changed after this machine was built: most likely its file was released and the memory reused while a car still had
 * the machine [inferred]. Another player's car had been mapped 2.2 s before; the teardown came from a car presentation's PresentationLoaded
 * handling (+0x130c9f0) on the logic thread [confirmed: fh2mp.log, the call stack].
 * The guard hooks the jle at +0x1710ebb: when the game would walk the nodes and the list is NULL, it sets ZF, so the game takes the jle to the
 * end of the walk, the path of a machine with no nodes. There are none to free. A list that is not NULL but shorter than the count is not
 * caught here. FH2MP_NO_SMGUARD=1 switches it off. */
#include "state_machine_rule.inc"

#define SM_SITE 0x1710ebbu
/* mov rax,[rcx+0x5c] ; cmp [rax+0x14],r14d ; jle +0x53 ; mov esi,r14d ; mov rax,[rbx+0x6c] ; mov rdi,[rsi+rax] */
static const unsigned char g_sm_walk[21] = {0x48, 0x8b, 0x41, 0x5c, 0x44, 0x39, 0x70, 0x14, 0x7e, 0x53, 0x41,
                                            0x8b, 0xf6, 0x48, 0x8b, 0x43, 0x6c, 0x48, 0x8b, 0x3c, 0x06};
/* the jle's target: mov r8,[rbx+0x6c] ; mov edx,0x145 ; lea rcx,[the file name] ; call Granny's free */
static const unsigned char g_sm_free_list[21] = {0x4c, 0x8b, 0x43, 0x6c, 0xba, 0x45, 0x01, 0x00, 0x00, 0x48, 0x8d,
                                                 0x0d, 0x90, 0x89, 0xbb, 0x00, 0xe8, 0x0b, 0x61, 0x00, 0x00};
/* Granny's free: test r8,r8 ; je (the return) */
static const unsigned char g_sm_free[5] = {0x4d, 0x85, 0xc0, 0x74, 0x41};
static volatile LONG g_sm_hits;

static void sm_check(FH2Context *ctx, void *user)
{
    unsigned long long list = 1;
    LONG n;
    (void)user;
    if (!read_game_memory((const void *)(ULONG_PTR)(ctx->rbx + 0x6c), &list, sizeof list) || !sm_skip(ctx->rflags, list)) {
        return;
    }
    ctx->rflags |= SM_ZF;
    n = InterlockedIncrement(&g_sm_hits);
    if (n <= 5 || n % 50 == 0) {
        log_begin();
        log_raw("smguard: an animation state machine whose definition counts nodes but which has no node list was torn down without walking "
                "the list (it would read NULL at game+0x1710ec4) (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
}

static int patch_state_machine_guard(void)
{
    unsigned char *img = find_fh2_image();
    FH2Hook *h;
    if (!img || !code_is("smguard", "the state machine's node walk", img, 0x1710eb3u, g_sm_walk, sizeof g_sm_walk) ||
        !code_is("smguard", "the state machine's list free", img, 0x1710f10u, g_sm_free_list, sizeof g_sm_free_list) ||
        !code_is("smguard", "Granny's free", img, 0x1717030u, g_sm_free, sizeof g_sm_free)) {
        return 0;
    }
    h = g_api->hook_instruction(g_mod, plat_tag("smguard: animation state machine teardown"), img + SM_SITE, sm_check, NULL);
    if (!h) {
        plat_failed("smguard: animation state machine teardown", img + SM_SITE, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, h)) {
        plat_failed("smguard: animation state machine teardown", img + SM_SITE, "created but could not be switched on");
        return 0;
    }
    log_begin();
    log_raw("hook: smguard: an animation state machine with no node list is torn down without walking it (game+0x1710ebb)\n");
    return 1;
}
