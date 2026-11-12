/* The component lookup guard ("exception 0xc0000005 at game+0x2d8ea4 reading 0x68", issue #146, 7+ reports, the latest
 * 9729cc0fe759725b about 60 s into online Free Roam).
 * game+0x454f20(entity, out) asks an entity for one component type: it passes the entity's component list (entity+0x68) to
 * game+0x4555e0, which looks the type up in the list with the generic lookup game+0x2d8e90 and hands back a shared pointer in out:
 *     140454f26  add  rcx,0x68              ; entity -> its component list
 *     140454f35  call 0x1404555e0
 *       ...      call 0x1402d8e90           ; the lookup
 *     1402d8ea4  mov  rax,[rcx]             <- rcx = 0x68: the entity is NULL
 * [confirmed: disassembly; the crash stack 455630 <- 454f3a <- 562065 <- 8d175c]. A type that is not in the list gives an empty pointer,
 * and all five callers of 0x454f20 (0x482210, 0x5565c0, 0x559a80, 0x562030, 0x8002e0) test the result for 0 before using it
 * [confirmed: decompile]. Which entity is missing, and why, is not known (the crash came as many remote cars were being mapped; guess).
 * The guard hooks 0x454f20: for a NULL entity it answers an empty pointer ({0, 0}, the shape of a component that is not there) and does
 * not call the game's code; any other entity goes to the game's code. It logs the caller once in a while, to find the missing entity's
 * owner. FH2MP_NO_COMPGUARD=1 switches it off. Online runs only (FH2MP_OFFLINE skips the shim's guards). */
#define CL_FN 0x454f20u
typedef void **(*cl_fn)(void *, void **);
static cl_fn g_cl_orig;
static unsigned char *g_cl_img;
static volatile LONG g_cl_hits;

/* push rbx ; sub rsp,0x30 ; add rcx,0x68 ; mov rbx,rdx ; mov dword [rsp+0x20],0 ; call 0x1404555e0 */
static const unsigned char g_cl_code[23] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x83, 0xc1, 0x68, 0x48, 0x8b, 0xda, 0xc7, 0x44, 0x24,
                                            0x20, 0x00, 0x00, 0x00, 0x00, 0xe8, 0xa6};
/* game+0x2d8ea0: sub rsp,0x20 ; mov rax,[rcx] (the faulting read) */
static const unsigned char g_cl_lookup[7] = {0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0x01};

static void **cl_detour(void *entity, void **out)
{
    LONG n;
    if (entity) {
        return g_cl_orig(entity, out);
    }
    out[0] = NULL;
    out[1] = NULL;
    n = InterlockedIncrement(&g_cl_hits);
    if (n <= 5 || n % 50 == 0) {
        char line[240];
        wsprintfA(line, "complookup: a component lookup on a missing (NULL) entity, called from game+0x%x, answered 'no component' instead of "
                  "reading address 0x68 at game+0x2d8ea4 (#%ld)",
                  (unsigned)((unsigned char *)__builtin_return_address(0) - g_cl_img), (long)n);
        log_begin();
        log_raw(line);
        log_raw("\n");
    }
    return out;
}

static int patch_component_lookup_guard(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("complookup", "the entity component getter", img, CL_FN, g_cl_code, sizeof g_cl_code) ||
        !code_is("complookup", "the component list lookup", img, 0x2d8ea0u, g_cl_lookup, sizeof g_cl_lookup)) {
        return 0;
    }
    g_cl_img = img;
    if (!plat_hook("complookup: entity component getter", img + CL_FN, (void *)cl_detour, (void **)&g_cl_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: complookup: a component lookup on a missing entity answers 'no component' (game+0x454f20)\n");
    return 1;
}
