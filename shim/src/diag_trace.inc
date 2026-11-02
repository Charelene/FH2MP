/* Traffic diagnostics for one player's own install (docs/diag.md), compiled only into the variant built with `shim/build.sh diag`. Two real
 * players on dev cannot reproduce what happens on prod: traffic that snaps when its owner changes, a car that bumps about once a second
 * while it is pushed at low speed. These hooks only observe: each calls the native function unchanged and writes one "diag:" line to
 * fh2mp.log, at most DIAG_MAX_LINES_PER_S a second.
 *
 *   diag: 19:58:08.412 UTC server take car 17
 *   diag: 19:58:08.412 UTC authority car 17 local 0 -> 1 from +0x72b1c3
 *   diag: 19:58:09.870 UTC request car 17 sent, 2120 ms after the last one, local was 0, now 1
 *   diag: 19:58:09.905 UTC warp of a remote traffic car (object 0x1a2b3c)
 *
 * Natives (docs/exe, netv2; build 1.0.0.64): SyncTrafficModule::OnTakeOwnership +728e50, OnDropOwnership +71b180 and OnOwnershipRequestDenied
 * +723fa0 (ClientTrafficMessage kinds 6, 7 and 8: module, vehicle id, ...); the traffic car record's authority setter +726570 (car, local:
 * +0x98 is "local authority", +0x9c the vehicle id); the RequestOwnership sender +717410 (the collision path; it sets local authority at
 * once and stamps +0xcc with the network clock, at most every 2000 ms); the warp in NetworkComponent::Update +18ed840, the call to
 * VehiclePlacement::InterpolatePosition at +18ed98b (placement = object + 0x320). */
#ifdef FH2MP_DIAG_BUILD
#define DIAG_MAX_LINES_PER_S 40

typedef void (*diag_own_fn)(void *, unsigned char);
typedef void (*diag_req_fn)(void *, void *, void *, void *);
typedef void (*diag_msg_fn)(void *, unsigned char, void *, void *);
typedef void (*diag_warp_fn)(void *, char);

static diag_own_fn g_diag_own_orig;
static diag_req_fn g_diag_req_orig;
static diag_msg_fn g_diag_take_orig, g_diag_drop_orig, g_diag_denied_orig;
static diag_warp_fn g_diag_warp_orig;
static unsigned char *g_diag_img;
static volatile LONG g_diag_window, g_diag_count, g_diag_dropped;

/* Whether this line may be written. The first line of a new second says how many the last one lost. */
static int diag_line_ok(void)
{
    LONG sec = (LONG)(GetTickCount() / 1000);

    if (InterlockedExchange(&g_diag_window, sec) != sec) {
        LONG lost = InterlockedExchange(&g_diag_dropped, 0);
        InterlockedExchange(&g_diag_count, 0);
        if (lost) {
            char utc[32], line[128];
            grid_utc(utc);
            wsprintfA(line, "diag: %s %ld lines over the limit of %d a second were not written", utc, (long)lost, DIAG_MAX_LINES_PER_S);
            grid_line(line);
        }
    }
    if (InterlockedIncrement(&g_diag_count) > DIAG_MAX_LINES_PER_S) {
        InterlockedIncrement(&g_diag_dropped);
        return 0;
    }
    return 1;
}

static unsigned long diag_rva(const void *p)
{
    return (unsigned long)((const unsigned char *)p - g_diag_img);
}

/* Every change of a traffic car's local authority goes through here: the Take and Drop handlers, a collision's request, a spawn.
 * The caller's address says which. */
static void diag_authority(void *car, unsigned char local)
{
    const unsigned char *c = (const unsigned char *)car;

    if (diag_line_ok()) {
        char utc[32], line[144];
        grid_utc(utc);
        wsprintfA(line, "diag: %s authority car %u local %u -> %u from +0x%lx", utc, (unsigned)c[0x9c], (unsigned)c[0x98], (unsigned)local,
                  diag_rva(__builtin_return_address(0)));
        grid_line(line);
    }
    g_diag_own_orig(car, local);
}

/* The collision path. The native function stamps +0xcc only when it sent the request, so a changed stamp is a request that went out. */
static void diag_request(void *car, void *a2, void *a3, void *a4)
{
    const unsigned char *c = (const unsigned char *)car;
    int before = *(const int *)(c + 0xcc);
    unsigned char was = c[0x98];

    g_diag_req_orig(car, a2, a3, a4);
    if (*(const int *)(c + 0xcc) != before && diag_line_ok()) {
        char utc[32], line[160];
        grid_utc(utc);
        wsprintfA(line, "diag: %s request car %u sent, %lu ms after the last one, local was %u, now %u", utc, (unsigned)c[0x9c],
                  (unsigned long)(unsigned)(*(const int *)(c + 0xcc) - before), (unsigned)was, (unsigned)c[0x98]);
        grid_line(line);
    }
}

static void diag_server(const char *what, unsigned char id)
{
    if (diag_line_ok()) {
        char utc[32], line[96];
        grid_utc(utc);
        wsprintfA(line, "diag: %s server %s car %u", utc, what, (unsigned)id);
        grid_line(line);
    }
}

static void diag_take(void *module, unsigned char id, void *a, void *b)
{
    diag_server("take", id);
    g_diag_take_orig(module, id, a, b);
}

static void diag_drop(void *module, unsigned char id, void *a, void *b)
{
    diag_server("drop", id);
    g_diag_drop_orig(module, id, a, b);
}

static void diag_denied(void *module, unsigned char id, void *a, void *b)
{
    diag_server("denied", id);
    g_diag_denied_orig(module, id, a, b);
}

/* NetworkComponent::Update calls this only when the received position is farther than MaxDistanceUntilWarp (15 m by default). */
static void diag_warp(void *placement, char flag)
{
    if (diag_line_ok()) {
        char utc[32], line[128];
        grid_utc(utc);
        wsprintfA(line, "diag: %s warp of a remote traffic car (object 0x%lx)", utc, (unsigned long)((ULONG_PTR)placement - 0x320u));
        grid_line(line);
    }
    g_diag_warp_orig(placement, flag);
}

static int patch_diag_trace(void)
{
    static const unsigned char msg_prologue[] = {0x88, 0x54, 0x24, 0x10, 0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x83, 0xec, 0x60, 0x49, 0xc7, 0x43, 0xb8};
    static const unsigned char warp_before[] = {0x81, 0xc1, 0x20, 0x03, 0x00, 0x00, 0xb2, 0x01}; /* add ecx,0x320 ; mov dl,1 */
    static const struct {
        unsigned rva;
        unsigned char bytes[16];
        const char *name;
        void *detour;
        void **original;
    } hooks[] = {
        {0x726570, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x88, 0x91, 0x98, 0x00, 0x00, 0x00, 0xc6},
         "diag: traffic car authority setter", (void *)diag_authority, (void **)&g_diag_own_orig},
        {0x717410, {0x40, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48},
         "diag: traffic RequestOwnership sender", (void *)diag_request, (void **)&g_diag_req_orig},
        {0x728e50, {0}, "diag: SyncTrafficModule::OnTakeOwnership", (void *)diag_take, (void **)&g_diag_take_orig},
        {0x71b180, {0}, "diag: SyncTrafficModule::OnDropOwnership", (void *)diag_drop, (void **)&g_diag_drop_orig},
        {0x723fa0, {0}, "diag: SyncTrafficModule::OnOwnershipRequestDenied", (void *)diag_denied, (void **)&g_diag_denied_orig},
    };
    const unsigned warp_site = 0x18ed98bu;
    unsigned char *img = find_fh2_image();
    unsigned i;
    int ok = 1, d32;

    if (!img) {
        return 0;
    }
    g_diag_img = img;
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        const void *expect = i >= 2 ? (const void *)msg_prologue : (const void *)hooks[i].bytes;
        if (!code_is("diag", hooks[i].name + 6, img, hooks[i].rva, expect, 16)) {
            return 0;
        }
    }
    mem_copy(&d32, img + warp_site + 1, 4);
    if (!code_is("diag", "warp call setup", img, warp_site - (unsigned)sizeof warp_before, warp_before, sizeof warp_before) ||
        img[warp_site] != 0xe8 || img + warp_site + 5 + d32 != img + 0x18e14e0u) {
        log_begin();
        log_raw("diag: the warp call at game+0x18ed98b is not the call to VehiclePlacement::InterpolatePosition (+0x18e14e0); not patched\n");
        return 0;
    }
    for (i = 0; i < sizeof hooks / sizeof hooks[0]; ++i) {
        if (!plat_hook(hooks[i].name, img + hooks[i].rva, hooks[i].detour, hooks[i].original)) ok = 0;
    }
    if (!plat_hook_call("diag: traffic warp", img + warp_site, (void *)diag_warp, (void **)&g_diag_warp_orig)) ok = 0;
    log_begin();
    log_raw(ok ? "diag: traffic trace on, lines are written as \"diag:\" with the UTC time of day\n"
               : "diag: the traffic trace is incomplete, the hooks named above failed\n");
    return ok;
}
#endif
