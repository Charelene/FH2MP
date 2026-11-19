/* Heap watch (memory-growth-oom, 2026-11-19). The heap censuses of 7 runaways show the growing heap is the process heap and that it holds almost no
 * objects: in ~129,000 sampled qwords the most common pointer into a module shows up 2 to 103 times, so the 13-32 GB are raw data [C: the census lines on
 * prod]. A pointer census cannot say whose data that is. The heap watch can: during the same window as the slice watch it follows every allocation of
 * HEAP_WATCH_MIN bytes or more on the process heap (RtlAllocateHeap, RtlReAllocateHeap) and every free of one of them (RtlFreeHeap), and at the end
 * logs the bytes still held, by the first caller outside the C runtime and the system DLLs (ntdll, KERNELBASE, kernel32, ucrtbase, msvcrt, vcruntime,
 * msvcp): the module and offset that kept the memory. It only counts; every call goes on unchanged. Off with FH2MP_NO_SLICEWATCH, like the slice watch.
 *
 * The part above the #ifndef has no Windows calls: shim/tests/test_heap_watch.sh runs it natively. */
#define HEAP_WATCH_MIN 4096u
#define HEAP_WATCH_SLOTS 65536u        /* live allocations followed at once (a power of two) */
#define HEAP_WATCH_TOMB (~0ull)
#define HEAP_WATCH_TOP 8u

typedef struct {
    unsigned long long ptr, size, caller;       /* ptr 0 = never used, HEAP_WATCH_TOMB = freed */
} heap_watch_ent;

typedef struct {
    heap_watch_ent e[HEAP_WATCH_SLOTS];
    unsigned long long alloc_bytes, freed_bytes;  /* of the followed allocations, in the window */
    unsigned long allocs, frees, dropped;         /* dropped: no free slot found (the table was full there) */
} heap_watch_table;

static unsigned heap_watch_hash(unsigned long long p)
{
    return (unsigned)((p * 0x9e3779b97f4a7c15ull) >> 40) & (HEAP_WATCH_SLOTS - 1);
}

/* Not thread-safe by itself: the Windows part takes its lock around these. */
static void heap_watch_add(heap_watch_table *t, unsigned long long ptr, unsigned long long size, unsigned long long caller)
{
    unsigned i = heap_watch_hash(ptr), n;

    t->allocs++;
    t->alloc_bytes += size;
    for (n = 0; n < 64; n++, i = (i + 1) & (HEAP_WATCH_SLOTS - 1)) {
        if (t->e[i].ptr == 0 || t->e[i].ptr == HEAP_WATCH_TOMB) {
            t->e[i].ptr = ptr;
            t->e[i].size = size;
            t->e[i].caller = caller;
            return;
        }
    }
    t->dropped++;
}

/* -> 1 when ptr is followed. Read without the lock by the free hook, so the lock is taken only for the frees of followed blocks; a block is followed
 * before its own free can come, so a racy read never misses it. */
static int heap_watch_has(const heap_watch_table *t, unsigned long long ptr)
{
    unsigned i = heap_watch_hash(ptr), n;

    for (n = 0; n < 64; n++, i = (i + 1) & (HEAP_WATCH_SLOTS - 1)) {
        unsigned long long p = t->e[i].ptr;
        if (p == 0) {
            return 0;
        }
        if (p == ptr) {
            return 1;
        }
    }
    return 0;
}

/* A free (or the old block of a realloc): -> 1 when ptr was followed. */
static int heap_watch_remove(heap_watch_table *t, unsigned long long ptr)
{
    unsigned i = heap_watch_hash(ptr), n;

    for (n = 0; n < 64; n++, i = (i + 1) & (HEAP_WATCH_SLOTS - 1)) {
        if (t->e[i].ptr == 0) {
            return 0;
        }
        if (t->e[i].ptr == ptr) {
            t->e[i].ptr = HEAP_WATCH_TOMB;
            t->frees++;
            t->freed_bytes += t->e[i].size;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    unsigned long long caller, bytes;
    unsigned long count;
} heap_watch_sum;

/* The callers holding the most bytes at the end, most first: -> how many (up to n), and the totals of what is still held. */
static unsigned heap_watch_top(const heap_watch_table *t, heap_watch_sum *out, unsigned n, unsigned long long *held_bytes, unsigned long *held_count)
{
    static heap_watch_sum sums[1024];
    unsigned nsum = 0, i, k, got = 0;

    *held_bytes = 0;
    *held_count = 0;
    for (i = 0; i < HEAP_WATCH_SLOTS; i++) {
        const heap_watch_ent *e = &t->e[i];
        if (e->ptr == 0 || e->ptr == HEAP_WATCH_TOMB) {
            continue;
        }
        *held_bytes += e->size;
        (*held_count)++;
        for (k = 0; k < nsum && sums[k].caller != e->caller; k++) {
        }
        if (k == nsum) {
            if (nsum == 1024) {
                continue;               /* more callers than this: they are in the totals only */
            }
            sums[nsum].caller = e->caller, sums[nsum].bytes = 0, sums[nsum].count = 0;
            nsum++;
        }
        sums[k].bytes += e->size;
        sums[k].count++;
    }
    for (i = 0; i < nsum; i++) {
        for (k = got; k > 0 && sums[i].bytes > out[k - 1].bytes; k--) {
            if (k < n) {
                out[k] = out[k - 1];
            }
        }
        if (k < n) {
            out[k] = sums[i];
            if (got < n) {
                got++;
            }
        }
    }
    return got;
}

#ifndef HEAP_WATCH_LOGIC_ONLY
typedef PVOID (NTAPI *heap_watch_alloc_fn)(PVOID heap, ULONG flags, SIZE_T size);
typedef PVOID (NTAPI *heap_watch_realloc_fn)(PVOID heap, ULONG flags, PVOID p, SIZE_T size);
typedef BOOLEAN (NTAPI *heap_watch_free_fn)(PVOID heap, ULONG flags, PVOID p);
static heap_watch_alloc_fn g_hw_alloc_orig;
static heap_watch_realloc_fn g_hw_realloc_orig;
static heap_watch_free_fn g_hw_free_orig;
static heap_watch_table *g_hw;                 /* 1.5 MB, made at the first window only, so a crash dump of a normal game stays as it was */
static SRWLOCK g_hw_lock = SRWLOCK_INIT;
static volatile LONG g_hw_on;
static int g_hw_state;                         /* 0 not tried, 1 hooked, -1 not available */
static PVOID g_hw_heap;
static struct { unsigned long long lo, hi; } g_hw_skip[8];
static unsigned g_hw_nskip;

static int heap_watch_skipped(unsigned long long a)
{
    unsigned i;
    for (i = 0; i < g_hw_nskip; i++) {
        if (a >= g_hw_skip[i].lo && a < g_hw_skip[i].hi) {
            return 1;
        }
    }
    return 0;
}

static unsigned long long heap_watch_caller(void)
{
    void *frames[24];
    USHORT n = RtlCaptureStackBackTrace(2, 24, frames, NULL), i;

    for (i = 0; i < n; i++) {
        unsigned long long a = (unsigned long long)(ULONG_PTR)frames[i];
        if (!heap_watch_skipped(a)) {
            return a;
        }
    }
    return n ? (unsigned long long)(ULONG_PTR)frames[n - 1] : 0;
}

static PVOID NTAPI heap_watch_alloc_hook(PVOID heap, ULONG flags, SIZE_T size)
{
    PVOID p = g_hw_alloc_orig(heap, flags, size);
    if (g_hw_on && p && heap == g_hw_heap && size >= HEAP_WATCH_MIN) {
        unsigned long long caller = heap_watch_caller();
        AcquireSRWLockExclusive(&g_hw_lock);
        heap_watch_add(g_hw, (unsigned long long)(ULONG_PTR)p, size, caller);
        ReleaseSRWLockExclusive(&g_hw_lock);
    }
    return p;
}

static PVOID NTAPI heap_watch_realloc_hook(PVOID heap, ULONG flags, PVOID old, SIZE_T size)
{
    PVOID p = g_hw_realloc_orig(heap, flags, old, size);
    if (g_hw_on && p && heap == g_hw_heap) {
        unsigned long long caller = size >= HEAP_WATCH_MIN ? heap_watch_caller() : 0;
        AcquireSRWLockExclusive(&g_hw_lock);
        if (old) {
            heap_watch_remove(g_hw, (unsigned long long)(ULONG_PTR)old);
        }
        if (size >= HEAP_WATCH_MIN) {
            heap_watch_add(g_hw, (unsigned long long)(ULONG_PTR)p, size, caller);
        }
        ReleaseSRWLockExclusive(&g_hw_lock);
    }
    return p;
}

static BOOLEAN NTAPI heap_watch_free_hook(PVOID heap, ULONG flags, PVOID p)
{
    if (g_hw_on && p && heap == g_hw_heap && heap_watch_has(g_hw, (unsigned long long)(ULONG_PTR)p)) {
        AcquireSRWLockExclusive(&g_hw_lock);
        heap_watch_remove(g_hw, (unsigned long long)(ULONG_PTR)p);
        ReleaseSRWLockExclusive(&g_hw_lock);
    }
    return g_hw_free_orig(heap, flags, p);
}

static void heap_watch_skip_module(const wchar_t *name)
{
    HMODULE m = GetModuleHandleW(name);
    MODULEINFO mi;
    if (m && g_hw_nskip < 8 && K32GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi)) {
        g_hw_skip[g_hw_nskip].lo = (unsigned long long)(ULONG_PTR)mi.lpBaseOfDll;
        g_hw_skip[g_hw_nskip].hi = g_hw_skip[g_hw_nskip].lo + mi.SizeOfImage;
        g_hw_nskip++;
    }
}

/* Hooks the three heap calls once (three thread pauses of the platform, in a game that is already running away). -> 1 when they are in place. */
static int heap_watch_hook(void)
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    void *a_fn = ntdll ? (void *)GetProcAddress(ntdll, "RtlAllocateHeap") : NULL, *r_fn = ntdll ? (void *)GetProcAddress(ntdll, "RtlReAllocateHeap") : NULL,
         *f_fn = ntdll ? (void *)GetProcAddress(ntdll, "RtlFreeHeap") : NULL;
    FH2Hook *a, *r, *f;
    wchar_t env[8];

    if (g_hw_state) {
        return g_hw_state > 0;
    }
    g_hw_state = -1;
    if (GetEnvironmentVariableW(L"FH2MP_NO_SLICEWATCH", env, 8) != 0 || GetEnvironmentVariableW(L"FH2MP_NO_HEAPWATCH", env, 8) != 0) {
        return 0;
    }
    if (!a_fn || !r_fn || !f_fn || !g_api || !g_mod) {
        log_begin();
        log_raw("health: heap watch: ntdll's heap calls not found, the watch does not run\n");
        return 0;
    }
    g_hw = (heap_watch_table *)VirtualAlloc(NULL, sizeof *g_hw, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_hw) {
        log_begin();
        log_raw("health: heap watch: no memory for its table (VirtualAlloc error ");
        log_dec(GetLastError());
        log_raw("), the watch does not run\n");
        return 0;
    }
    g_hw_heap = GetProcessHeap();
    heap_watch_skip_module(L"ntdll.dll");
    heap_watch_skip_module(L"KERNELBASE.dll");
    heap_watch_skip_module(L"kernel32.dll");
    heap_watch_skip_module(L"ucrtbase.dll");
    heap_watch_skip_module(L"msvcrt.dll");
    heap_watch_skip_module(L"vcruntime140.dll");
    heap_watch_skip_module(L"msvcp140.dll");
    heap_watch_skip_module(L"msvcp_win.dll");
    a = g_api->hook_function(g_mod, plat_tag("heapwatch: RtlAllocateHeap"), a_fn, (void *)heap_watch_alloc_hook, (void **)&g_hw_alloc_orig);
    r = a ? g_api->hook_function(g_mod, plat_tag("heapwatch: RtlReAllocateHeap"), r_fn, (void *)heap_watch_realloc_hook, (void **)&g_hw_realloc_orig) : NULL;
    f = r ? g_api->hook_function(g_mod, plat_tag("heapwatch: RtlFreeHeap"), f_fn, (void *)heap_watch_free_hook, (void **)&g_hw_free_orig) : NULL;
    if (!a || !r || !f) {
        plat_failed("heapwatch: ntdll heap call", !a ? a_fn : !r ? r_fn : f_fn, "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, a) || !g_api->enable_hooks(g_mod, r) || !g_api->enable_hooks(g_mod, f)) {
        plat_failed("heapwatch: ntdll heap call", a_fn, "created but could not be switched on");
        return 0;
    }
    g_hw_state = 1;
    return 1;
}

static int heap_watch_start(void)
{
    if (g_hw_on || !heap_watch_hook()) {
        return 0;
    }
    AcquireSRWLockExclusive(&g_hw_lock);
    memset(g_hw, 0, sizeof *g_hw);
    ReleaseSRWLockExclusive(&g_hw_lock);
    InterlockedExchange(&g_hw_on, 1);
    return 1;
}

static void heap_watch_finish(void)
{
    heap_watch_sum top[HEAP_WATCH_TOP];
    heap_watch_table *t = g_hw;
    unsigned long long held_bytes;
    unsigned long held_count;
    unsigned n, i;

    if (!g_hw_on) {
        return;
    }
    InterlockedExchange(&g_hw_on, 0);
    health_census_modules();
    AcquireSRWLockExclusive(&g_hw_lock);          /* a hook that saw the watch on before this waits here, then changes nothing that matters */
    n = heap_watch_top(t, top, HEAP_WATCH_TOP, &held_bytes, &held_count);
    ReleaseSRWLockExclusive(&g_hw_lock);
    log_begin();
    log_raw("health: heap watch over ");
    log_dec(SLICE_WATCH_S);
    log_raw(" s: ");
    slice_watch_u64(t->allocs);
    log_raw(" process-heap allocations of 4 KB or more, ");
    health_mb(t->alloc_bytes);
    log_raw(", ");
    health_mb(t->freed_bytes);
    log_raw(" of it freed again; still held ");
    health_mb(held_bytes);
    log_raw(" in ");
    slice_watch_u64(held_count);
    if (t->dropped) {
        log_raw(" (");
        log_dec(t->dropped);
        log_raw(" not followed: table full)");
    }
    log_raw(n ? "; held by:" : "\n");
    for (i = 0; i < n; i++) {
        const health_module *m = health_module_of(top[i].caller);
        log_raw(i ? ", " : " ");
        log_raw(m && m->name[0] ? m->name : "?");
        log_raw("+0x");
        health_hex(m ? top[i].caller - m->base : top[i].caller);
        log_raw(" ");
        health_mb(top[i].bytes);
        log_raw(" (");
        slice_watch_u64(top[i].count);
        log_raw(")");
    }
    if (n) {
        log_raw("\n");
    }
}
#endif
