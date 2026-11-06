/* The GameplayLog guard (issue #318: 18 reports at game+0x9c898f, writing 0x8, 10-30 s after the game starts).
 * CGameplayLogManager::vfunc_1 (game+0x9c88e0) handles a gameplay event for one user: it looks the user's record up with std::map::operator[]
 * (helper_9b1a70, map at this+0x20, key = the 64-bit value at event+0x1300) and writes through the record's Xls::Ref<CGameplayLog> at
 * record+0x38. A user with no record gets a new one whose Ref is NULL, and the write faults [confirmed: decompile of 0x9c88e0 and 0x9b1a70,
 * #318's dumps]. The record is made when the LSP's Logon succeeds; 17 reports came from a test that answered Logon with an empty result
 * (2026-10-30, reverted), one from 1.1.5 after a normal Logon on a save another profile made a minute earlier [inferred].
 * This hooks the handler: it finds the record without inserting one and leaves the event out when there is none or its log is NULL.
 * GameplayLog is telemetry the LSP answers with an empty OK, so a dropped event changes nothing a player sees. FH2MP_NO_LOGGUARD switches
 * it off. */
#define GL_HANDLER 0x9c88e0u

static const unsigned char g_gl_prologue[8] = {0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x54, 0x41};   /* mov rax,rsp ; push rsi ; push rdi ; push r12 */

static void (*g_gl_orig)(void *, void *);
static volatile LONG g_gl_dropped;

/* The map is MSVC's std::map: this+0x20 points at the head node; node: [0] left, [1] parent, [2] right, byte +0x19 = is-nil, key at +0x20,
 * the value from +0x28 (the Ref's pointer at value+0x38 = node+0x60). -> the CGameplayLog of `key`, NULL when the user has no record. */
static void *gl_log_of(const unsigned char *manager, unsigned long long key)
{
    const unsigned char *head = *(const unsigned char *const *)(manager + 0x20);
    const unsigned char *node, *best;
    if (!head) {
        return NULL;
    }
    node = *(const unsigned char *const *)(head + 0x08);
    best = head;
    while (node && !node[0x19]) {
        if (*(const unsigned long long *)(node + 0x20) < key) {
            node = *(const unsigned char *const *)(node + 0x10);
        } else {
            best = node;
            node = *(const unsigned char *const *)node;
        }
    }
    if (best == head || *(const unsigned long long *)(best + 0x20) != key) {
        return NULL;
    }
    return *(void *const *)(best + 0x60);
}

static void gl_handler(void *manager, void *event)
{
    if (!gl_log_of((const unsigned char *)manager, *(const unsigned long long *)((const unsigned char *)event + 0x1300))) {
        if (InterlockedIncrement(&g_gl_dropped) == 1) {
            log_begin();
            log_raw("hook: gameplay log guard: an event for a user with no CGameplayLog (no record from Logon) is left out; the game would have "
                    "written through a NULL log at game+0x9c898f\n");
        }
        return;
    }
    g_gl_orig(manager, event);
}

static int patch_gameplaylog_guard(void)
{
    unsigned char *img = find_fh2_image();
    if (!img || !code_is("logguard", "CGameplayLogManager's event handler", img, GL_HANDLER, g_gl_prologue, sizeof g_gl_prologue)) {
        return 0;
    }
    return plat_hook("logguard: gameplay log event", img + GL_HANDLER, (void *)gl_handler, (void **)&g_gl_orig);
}
