/* The camera animation guard ("exception 0xc0000005 at game+0x1031ece reading 0x0", issue #479, two reports 2026-11-13/14, two PCs, free roam,
 * no mod on the stack). game+0x1031e40(camera, pos) turns pos (four doubles) through the camera's space provider at camera+8 when the camera's mode
 * (*(camera+0x188)) is 1, 3 or 4, and through the same provider another way for mode 2; any other mode leaves pos as it is (+0x1031ef2):
 *     141031e46  mov  rax,[rcx+0x188] ; mov eax,[rax] ; cmp eax,1 ...
 *     141031e93  mov  rcx,[rcx+8]
 *     141031ece  mov  rax,[rcx]        <- rcx = 0
 *     141031ed1  call [rax+0x60]
 *     141031ef8  mov  rcx,[rcx+8] ; lea rdx,[rsp+0x30] ; mov rax,[rcx] ; call [rax+8]   (mode 2)
 * In both dumps (a85d3a91c5074e41, a520ebd213a8da40) the camera is a Camera::CCamAnimPlayer (vtable game+0x2e779d8, the game's RTTI) in mode 4 with
 * no provider (camera+8 = 0), called from its update at game+0x103c34a [confirmed: dumps, disassembly; why the provider is missing is not known: it
 * is the game's own state, no hook of ours or of another mod is on these paths]. The guard hooks game+0x1031e40: without a provider it leaves pos
 * as the function does for the modes it does not turn, and the camera keeps its last place for that frame. FH2MP_NO_CAMGUARD=1 switches it off. */
#define CAG_FN 0x1031e40u
typedef void *(*cag_fn)(void *, double *);
static cag_fn g_cag_orig;
static volatile LONG g_cag_hits;

/* push rbx ; sub rsp,0x70 ; mov rax,[rcx+0x188] ; mov rbx,rdx ; mov eax,[rax] ; cmp eax,1 */
static const unsigned char g_cag_head[21] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x70, 0x48, 0x8b, 0x81, 0x88, 0x01, 0x00, 0x00, 0x48, 0x8b, 0xda,
                                             0x8b, 0x00, 0x83, 0xf8, 0x01};
static const unsigned char g_cag_load[4] = {0x48, 0x8b, 0x49, 0x08};                        /* +0x1031e93 and +0x1031ef8: mov rcx,[rcx+8] */
static const unsigned char g_cag_call[6] = {0x48, 0x8b, 0x01, 0xff, 0x50, 0x60};            /* +0x1031ece: mov rax,[rcx] ; call [rax+0x60] */
static const unsigned char g_cag_keep[6] = {0x48, 0x83, 0xc4, 0x70, 0x5b, 0xc3};            /* +0x1031ef2: the exit that leaves pos as it is */

/* What the hook decides, apart from the game: -> 1 when the camera has no space provider. */
static int cag_no_provider(const void *camera)
{
    return camera && !*(void *const *)((const char *)camera + 8);
}

static void *cag_detour(void *camera, double *pos)
{
    LONG n;
    if (!cag_no_provider(camera)) {
        return g_cag_orig(camera, pos);
    }
    n = InterlockedIncrement(&g_cag_hits);
    if (n <= 3 || n % 500 == 0) {
        log_begin();
        log_raw("camanim: a camera animation player (mode ");
        log_dec((unsigned long)**(const unsigned *const *)((const char *)camera + 0x188));
        log_raw(") has no space provider; its position stays as it is instead of a NULL read at game+0x1031ece (#");
        log_dec((unsigned long)n);
        log_raw(")\n");
    }
    return pos;
}

static int patch_camanim_guard(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("camanim", "the camera transform", img, CAG_FN, g_cag_head, sizeof g_cag_head) ||
        !code_is("camanim", "its provider load", img, 0x1031e93u, g_cag_load, sizeof g_cag_load) ||
        !code_is("camanim", "its provider call", img, 0x1031eceu, g_cag_call, sizeof g_cag_call) ||
        !code_is("camanim", "its unchanged exit", img, 0x1031ef2u, g_cag_keep, sizeof g_cag_keep) ||
        !code_is("camanim", "its mode-2 provider load", img, 0x1031ef8u, g_cag_load, sizeof g_cag_load)) {
        return 0;
    }
    if (!plat_hook("camanim: camera transform", img + CAG_FN, (void *)cag_detour, (void **)&g_cag_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: camanim: a camera animation player without a space provider keeps its position (game+0x1031e40)\n");
    return 1;
}
