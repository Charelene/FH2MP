/* The speedometer hook guard (crash family 1: issues #320 #321 #327 #353 #355-#358 #362, 26 reports on 1.1.3-1.1.5, Windows only).
 * In every dump HUDSpeedometerComponent::Update calls HUDStatusSpeedometer::UpdateGauges (call game+0xAD6070 at game+0x783E7E) and execution
 * ends at 0xDFFExxxx, memory that is free by then: the jump another mod's hook put at game+0xAD6070 (HonkHonk 0.3.4 or NitroSystem, through
 * the platform's MinHook) leads into a block that was released. What releases it is not known [confirmed: the call and the free target in
 * 9 dumps; guess: the releasing code].
 * This hooks that one call. Before each call it follows the jumps at game+0xAD6070 (jmp rel32, jmp rel8, jmp [rip+disp32]) and checks that
 * each one lands in committed executable memory. When one does not, it puts the first 5 bytes of the game's own prologue back in one aligned
 * 8-byte write (the function starts 16-aligned) and logs it once; the game then runs its own UpdateGauges, without the other mod's detour.
 * FH2MP_NO_HOOKGUARD switches it off. */
#define HG_CALL_SITE 0x783e7eu
#define HG_TARGET 0xad6070u
#define HG_MAX_HOPS 8

static const unsigned char g_hg_call[5] = {0xe8, 0xed, 0x21, 0x35, 0x00};             /* call game+0xAD6070 */
static const unsigned char g_hg_prologue[5] = {0x40, 0x53, 0x48, 0x83, 0xec};          /* push rbx ; sub rsp,(0x20) */

static unsigned char *g_hg_fn;
static void (*g_hg_orig)(void *, float);
static volatile LONG g_hg_logged;

/* 1 when [p, p+n) is committed memory the CPU may execute (and so read). */
static int hg_executable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return 0;
    }
    if (!(m.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        return 0;
    }
    return (const unsigned char *)p + n <= (const unsigned char *)m.BaseAddress + m.RegionSize;
}

/* 1 when [p, p+n) can be read. */
static int hg_readable(const void *p, SIZE_T n)
{
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(p, &m, sizeof m) || m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        return 0;
    }
    return (const unsigned char *)p + n <= (const unsigned char *)m.BaseAddress + m.RegionSize;
}

/* Follows the jumps from `at`. -> 1 when every hop is in executable memory (more than HG_MAX_HOPS jumps also count as fine: nothing to judge),
 * 0 with *bad = the first address that is not. */
static int hg_chain_ok(const unsigned char *at, const unsigned char **bad)
{
    int hop;
    for (hop = 0; hop < HG_MAX_HOPS; hop++) {
        if (!hg_executable(at, 6)) {
            *bad = at;
            return 0;
        }
        if (at[0] == 0xe9) {
            int rel;
            mem_copy(&rel, at + 1, 4);
            at = at + 5 + rel;
        } else if (at[0] == 0xeb) {
            at = at + 2 + (signed char)at[1];
        } else if (at[0] == 0xff && at[1] == 0x25) {
            int disp;
            const unsigned char *slot;
            mem_copy(&disp, at + 2, 4);
            slot = at + 6 + disp;
            if (!hg_readable(slot, 8)) {
                *bad = slot;
                return 0;
            }
            mem_copy(&at, slot, 8);
        } else {
            return 1;
        }
    }
    return 1;
}

/* Writes the first 5 bytes of `orig` over `fn` in one 8-byte compare-exchange. -> 1 when they are in place afterwards. */
static int hg_restore(unsigned char *fn, const unsigned char *orig5)
{
    DWORD old, unused;
    LONG64 cur, want;
    if (!VirtualProtect(fn, 8, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    cur = *(volatile LONG64 *)fn;
    want = cur;
    mem_copy(&want, orig5, 5);
    InterlockedCompareExchange64((volatile LONG64 *)fn, want, cur);
    VirtualProtect(fn, 8, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), fn, 8);
    return memcmp(fn, orig5, 5) == 0;
}

static void hg_speedometer(void *self, float rpm)
{
    const unsigned char *bad = NULL;
    if (!hg_chain_ok(g_hg_fn, &bad)) {
        int fixed = hg_restore(g_hg_fn, g_hg_prologue);
        if (InterlockedIncrement(&g_hg_logged) == 1) {
            log_begin();
            log_raw("hook: speedometer guard: the jump at game+0xad6070 (UpdateGauges, hooked by another mod) leads to 0x");
            log_hex((unsigned long long)(ULONG_PTR)bad, 16);
            log_raw(", which is not executable memory; ");
            log_raw(fixed ? "the game's own code is put back (that mod's detour no longer runs)\n"
                          : "the game's code could not be put back, so the gauges are not updated\n");
        }
        if (!fixed) {
            return;
        }
    }
    g_hg_orig(self, rpm);
}

static int patch_hook_guard(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("hookguard", "the call to UpdateGauges", img, HG_CALL_SITE, g_hg_call, sizeof g_hg_call)) {
        return 0;
    }
    g_hg_fn = img + HG_TARGET;
    if (!plat_hook_call("hookguard: speedometer call", img + HG_CALL_SITE, (void *)hg_speedometer, (void **)&g_hg_orig)) {
        return 0;
    }
    log_begin();
    log_raw("hook: speedometer guard: the call to UpdateGauges at game+0x783e7e checks the jump at game+0xad6070 before each call\n");
    return 1;
}
