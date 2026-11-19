/* FH2 1.0.0.64: speed completion gates uploads on the saved PersonalBest,
 * but HUDLeaderboardComparison::OnDownload compares against userRow.score.
 * Retain the saved value BEFORE completion updates it, for that comparison
 * component and speed. A weak reference prevents an old entry matching a new
 * component allocated at the same address. Never create a leaderboard row.
 */
typedef struct { void *object; void *control; } speed_pb_ref;
typedef struct { char text[16]; unsigned long long size, capacity; } speed_pb_string;
typedef speed_pb_ref *(*speed_pb_resolve_fn)(void *, speed_pb_ref *);
typedef float (*speed_pb_property_fn)(void *, speed_pb_string *);
typedef void (*speed_pb_trap_fn)(void *, void *);
typedef void (*speed_pb_zone_fn)(void *);
typedef void (*speed_pb_compare_fn)(void *, speed_pb_ref *);
typedef float (*speed_pb_score_fn)(void *);
static speed_pb_resolve_fn g_speed_pb_resolve;
static speed_pb_property_fn g_speed_pb_property;
static speed_pb_trap_fn g_speed_pb_trap;
static speed_pb_zone_fn g_speed_pb_zone;
static speed_pb_compare_fn g_speed_pb_compare;
static speed_pb_score_fn g_speed_pb_score;

#define SPEED_PB_SLOTS 64
typedef struct {
    speed_pb_ref weak;
    float previous, speed;
} speed_pb_entry;
static speed_pb_entry g_speed_pb_entries[SPEED_PB_SLOTS];
static unsigned g_speed_pb_next;
static SRWLOCK g_speed_pb_lock = SRWLOCK_INIT;
static DWORD g_speed_pb_tls = TLS_OUT_OF_INDEXES;
static volatile LONG g_speed_pb_active;
/* Set (to the TLS error + 1) when the feature switched itself off at run time; the monitor thread logs it. */
static volatile LONG g_speed_pb_fault;
typedef struct { void *request; float previous; } speed_pb_scope;

static int speed_pb_finite_nonnegative(float value)
{
    union { float f; unsigned u; } bits;
    bits.f = value;
    return value >= 0.0f && (bits.u & 0x7f800000u) != 0x7f800000u;
}

/* std::_Ref_count_base on this build: vptr, strong count, weak count.
 * Releasing our temporary native lock exactly matches the native callers.
 */
static void speed_pb_weak_release(speed_pb_ref *ref)
{
    LONG *counts = ref->control;
    if (counts && InterlockedDecrement(counts + 3) == 0)
        ((void (*)(void *))(*(void ***)counts)[1])(counts);
    ref->object = ref->control = NULL;
}

static void speed_pb_release(speed_pb_ref *ref)
{
    LONG *counts = ref->control;
    if (counts && InterlockedDecrement(counts + 2) == 0) {
        ((void (*)(void *))(*(void ***)counts)[0])(counts);
        speed_pb_weak_release(ref);
    }
    ref->object = ref->control = NULL;
}

static int speed_pb_alive(const speed_pb_ref *ref)
{
    return ref->control && InterlockedCompareExchange((LONG *)ref->control + 2, 0, 0) > 0;
}

static void speed_pb_remember(const speed_pb_ref *comparison, float previous, float speed)
{
    unsigned i, slot = SPEED_PB_SLOTS;
    AcquireSRWLockExclusive(&g_speed_pb_lock);
    for (i = 0; i < SPEED_PB_SLOTS; ++i) {
        speed_pb_entry *e = &g_speed_pb_entries[i];
        if (e->weak.control && !speed_pb_alive(&e->weak)) speed_pb_weak_release(&e->weak);
        if (e->weak.object == comparison->object && e->weak.control == comparison->control) {
            slot = i;
            break;
        }
        if (!e->weak.control && slot == SPEED_PB_SLOTS) slot = i;
    }
    if (slot == SPEED_PB_SLOTS) slot = g_speed_pb_next++ % SPEED_PB_SLOTS;
    speed_pb_weak_release(&g_speed_pb_entries[slot].weak);
    InterlockedIncrement((LONG *)comparison->control + 3);
    g_speed_pb_entries[slot].weak = *comparison;
    g_speed_pb_entries[slot].previous = previous;
    g_speed_pb_entries[slot].speed = speed;
    ReleaseSRWLockExclusive(&g_speed_pb_lock);
}

static int speed_pb_previous(void *comparison, float speed, float *previous)
{
    unsigned i;
    int found = 0;
    AcquireSRWLockExclusive(&g_speed_pb_lock);
    for (i = 0; i < SPEED_PB_SLOTS; ++i) {
        speed_pb_entry *e = &g_speed_pb_entries[i];
        if (e->weak.control && !speed_pb_alive(&e->weak)) speed_pb_weak_release(&e->weak);
        if (e->weak.object == comparison && e->weak.control && e->speed == speed) {
            *previous = e->previous;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_speed_pb_lock);
    return found;
}

static void speed_pb_capture(void *trigger, unsigned saved_offset, unsigned hud_offset, unsigned speed_offset)
{
    unsigned char *self = trigger;
    speed_pb_ref saved = {0}, hud = {0}, comparison = {0};
    speed_pb_string key = {"PersonalBest", 12, 15};
    float previous, speed = *(float *)(self + speed_offset);
    if (!speed_pb_finite_nonnegative(speed)) return;
    g_speed_pb_resolve(self + saved_offset, &saved);
    g_speed_pb_resolve(self + hud_offset, &hud);
    if (saved.object && hud.object) {
        g_speed_pb_resolve((unsigned char *)hud.object + 0x380, &comparison);
        if (comparison.object && comparison.control) {
            previous = g_speed_pb_property(saved.object, &key);
            if (speed_pb_finite_nonnegative(previous)) speed_pb_remember(&comparison, previous, speed);
        }
    }
    speed_pb_release(&comparison);
    speed_pb_release(&hud);
    speed_pb_release(&saved);
}

static void speed_pb_trap(void *trigger, void *player)
{
    if (g_speed_pb_active) speed_pb_capture(trigger, 0x190, 0x148, 0xb0);
    g_speed_pb_trap(trigger, player);
}

/* #509 (report 38b1b2beedda46aa). The zone handler game+0x6B76B0 starts
 *     +0 mov rax,rsp   +3 push rbp   +4 push rdi   +5 push r12   +7 push r14 ...
 * The platform (MinHook) patches 5 bytes, so its trampoline runs +0..+4 and continues at +5. DriftZones 0.7.5 (an Extra Options
 * plugin, not a platform mod) patches 7 bytes later on, `jmp` + `nop nop`, and chains into the jump it found there, i.e. into us:
 * the trampoline then continues into its nops, push r12 is lost, and the handler runs 8 bytes off. Its own call of the PersonalBest
 * getter then faults on the getter's aligned xmm6 save (game+0x3D2D8B, "reading 0xFFFFFFFFFFFFFFFF"); its epilogue would pop r12
 * that was never pushed. Confirmed from the dump: entry rsp aligned, rbp/rdi/r14/r15 saved with no r12 between, bytes at +0
 * `e9 .. .. .. .. 90 90 41 56`. When +5 no longer holds push r12, speed_pb_zone_head runs the handler's first 7 bytes itself and
 * continues at +7, so the handler gets its whole frame whoever patched over the trampoline's return. */
static const unsigned char *g_speed_pb_zone_tail;           /* game+0x6B76B5: push r12 (41 54) */
__attribute__((used)) static void *g_speed_pb_zone_cont;    /* game+0x6B76B7: push r14 */
static volatile LONG g_speed_pb_head_used;                  /* set once the head ran instead of the trampoline; logged by the monitor */
void speed_pb_zone_head(void *trigger);
__asm__(".text\n.p2align 4\n.def speed_pb_zone_head; .scl 3; .type 32; .endef\nspeed_pb_zone_head:\n"
        "  mov %rsp, %rax\n  push %rbp\n  push %rdi\n  push %r12\n  jmp *g_speed_pb_zone_cont(%rip)\n");

static void speed_pb_zone(void *trigger)
{
    const unsigned char *tail = g_speed_pb_zone_tail;
    if (g_speed_pb_active && !*((unsigned char *)trigger + 0x3b8)) speed_pb_capture(trigger, 0x318, 0x1f8, 0xd0);
    if (tail && g_speed_pb_zone_cont && (tail[0] != 0x41 || tail[1] != 0x54)) {
        if (!g_speed_pb_head_used) InterlockedExchange(&g_speed_pb_head_used, (LONG)(tail[0] | tail[1] << 8 | 0x10000));
        speed_pb_zone_head(trigger);
        return;
    }
    g_speed_pb_zone(trigger);
}

/* This getter is hooked at its one call in the comparison, not globally.
 * The scope is thread-local and restored for nested comparisons. The getter
 * runs before the native NewPersonalBest flag and UI event are produced.
 */
static float speed_pb_score(void *request)
{
    float online = g_speed_pb_score(request);
    if (!g_speed_pb_active) return online;
    speed_pb_scope *scope = TlsGetValue(g_speed_pb_tls);
    if (scope && scope->request == request && speed_pb_finite_nonnegative(online) && scope->previous > online)
        return scope->previous;
    return online;
}

static void speed_pb_compare(void *comparison, speed_pb_ref *request)
{
    speed_pb_scope scope = {request->object, 0};
    void *outer;
    float speed = *(float *)((unsigned char *)comparison + 0x11c);
    int found;
    if (!g_speed_pb_active) return g_speed_pb_compare(comparison, request);
    outer = TlsGetValue(g_speed_pb_tls);
    found = speed_pb_previous(comparison, speed, &scope.previous);
    if (!TlsSetValue(g_speed_pb_tls, found ? &scope : NULL)) {
        /* Disable the getter override too: an outer scope may still be set. */
        InterlockedExchange(&g_speed_pb_fault, (LONG)GetLastError() + 1);
        InterlockedExchange(&g_speed_pb_active, 0);
        return g_speed_pb_compare(comparison, request);
    }
    g_speed_pb_compare(comparison, request);
    if (!TlsSetValue(g_speed_pb_tls, outer)) {
        InterlockedExchange(&g_speed_pb_fault, (LONG)GetLastError() + 1);
        InterlockedExchange(&g_speed_pb_active, 0);
    }
}
