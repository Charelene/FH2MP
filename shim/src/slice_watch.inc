/* Slice watch (memory-growth-oom, #91/#468/#477/#480/#495, 2026-11-19). The heap census of shim 1.4.1 found the runaway heap full of Azimuth's buffer slices
 * (report ba111120035cb727: 962 segments, 15 GB, mostly std::_Ref_count_obj2<azimuth::gfx::Slice>, vtable azimuth+0x6f3f18). How Azimuth 1.0.4 (sha256
 * 56d2c9cb...b3b092) keeps them, read from its code [C: the disassembly; the meaning of each field is inferred from how the code uses it]:
 *   - A dynamic buffer the game maps with WRITE_DISCARD (ID3D11DeviceContext::Map, azimuth+0x1dae20, map type 4 on a resource with +0x94 == 2) gets a fresh
 *     Slice of memory each time: the current one (+0x108) is pushed onto the resource's retire queue (a std::deque<Slice*> at +0x110: map +0x118, map size
 *     +0x120, offset +0x128, size +0x130), then azimuth+0x1c95c0 (owner, resource) finds the next one.
 *   - azimuth+0x1c95c0 first runs the retire pass azimuth+0x1c6780: while the slice at the FRONT of the queue has a fence value (+0x40) the GPU has passed
 *     (owner +0x14c0), it is moved to the resource's free pool (+0x138) for reuse. Then it takes a free slice from the pool, or makes a new one with its own
 *     arena block (azimuth+0x1c64f0, the resource's size at +0x90) and keeps it in the resource's slice list (+0x150, a vector of shared_ptr<Slice>).
 *     A new current slice's fence is set to ~0 (not yet used) until the GPU work that reads it is stamped.
 *   - Deferred command lists install their recorded slices the same way (azimuth+0x1c97f0, also behind the retire pass).
 * So the queue drains in order only: a slice whose fence never gets a value stays at the front and every slice behind it is kept, and every later discard of
 * that buffer makes a new arena block (a guess at the runaway's mechanism, not seen yet). The watch measures it on the player's PC: while it runs it samples
 * the retire queue, the slice list, the front slice's fence and the GPU's passed fence of every buffer the game discards, with the game function that asked
 * (the first return address in the game's image), and logs the buffers with the longest queues. It reads only; Azimuth's calls run unchanged.
 * The heap censuses of 7 runaway reports (prod, 2026-11-15..18) do not all lead with Slice: others lead with BufferArena::Block (azimuth+0x6f40f8), the NVIDIA
 * driver's nvwgf2umx.dll+0x1d2980, BundlePlan or title_storage [C: the census lines]. So the watch also counts every arena block Azimuth allocates in the
 * window (BufferArena::allocate, azimuth+0x1ced10(arena, out, size, alignment); called from +0x1c60b7, +0x1c654d (a buffer's new slice), +0x1c8ea1,
 * +0x1c8f7d, +0x1c92e0 and +0x55fce1 [C]), with the bytes asked for, by its Azimuth call site and by the game function on the stack: what allocates the
 * 1 GB every 3 s of a runaway shows there even when no buffer queue grows.
 * It runs SLICE_WATCH_S seconds when the heap census does (commit over 1.25 x and 2.5 x RAM, a runaway) and, for a baseline on a test rig only, once at
 * FH2MP_SLICEWATCH_AT=<seconds> into the game. FH2MP_NO_SLICEWATCH (or switches=NO_SLICEWATCH from the hotfix document) turns it off.
 *
 * The part above the #ifndef has no Windows calls: shim/tests/test_slice_watch.sh runs it natively. */
#define SLICE_WATCH_S 5u
#define SLICE_WATCH_SLOTS 512u
#define SLICE_WATCH_TOP 6u
#define SLICE_WATCH_UNKNOWN (~0ull)

typedef struct {
    unsigned long long res;          /* the resource object (0 = free slot) */
    unsigned long long caller;       /* the first game return address seen for it, as an offset from the game's base (SLICE_WATCH_UNKNOWN: none on the stack) */
    unsigned long long queue_max;    /* the longest retire queue seen after a discard */
    unsigned long long list_max;     /* the most slices it held */
    unsigned long long front_fence;  /* the fence of the slice at the queue's front, last seen (~0: Azimuth's mark for a slice no GPU work has used yet) */
    unsigned discards, size, from_list;
    int front_ok;                    /* front_fence was read (0: the queue was empty or not readable) */
} slice_watch_res;

#define SLICE_WATCH_SITES 128u

/* Arena allocations by one key (an Azimuth call site or a game frame, as an offset from its module; 0 = free slot, SLICE_WATCH_UNKNOWN = no game frame). */
typedef struct {
    unsigned long long key, bytes;
    unsigned long count;
} slice_watch_site;

typedef struct {
    slice_watch_res r[SLICE_WATCH_SLOTS];
    unsigned long long passed;       /* the GPU fence value Azimuth last saw passed */
    unsigned long discards, dropped; /* discards sampled; resources not counted because the table was full */
    slice_watch_site by_site[SLICE_WATCH_SITES], by_game[SLICE_WATCH_SITES];
    unsigned long long alloc_bytes;  /* arena bytes asked for in the window */
    unsigned long allocs, alloc_dropped;
} slice_watch_table;

/* `bytes` more for `key` in t; a full table counts it in *dropped instead. */
static void slice_watch_site_add(slice_watch_site *t, unsigned long long key, unsigned long long bytes, unsigned long *dropped)
{
    unsigned i = (unsigned)((key * 0x9e3779b97f4a7c15ull) >> 57) & (SLICE_WATCH_SITES - 1), n;

    for (n = 0; n < SLICE_WATCH_SITES; n++, i = (i + 1) & (SLICE_WATCH_SITES - 1)) {
        if (t[i].count && t[i].key == key) {
            t[i].count++;
            t[i].bytes += bytes;
            return;
        }
        if (!t[i].count) {
            t[i].key = key;
            t[i].count = 1;
            t[i].bytes = bytes;
            return;
        }
    }
    (*dropped)++;
}

/* One arena allocation of `bytes` from Azimuth call site `site` with game frame `game`; the totals grow even when a table is full. */
static void slice_watch_alloc(slice_watch_table *t, unsigned long long site, unsigned long long game, unsigned long long bytes)
{
    t->allocs++;
    t->alloc_bytes += bytes;
    slice_watch_site_add(t->by_site, site, bytes, &t->alloc_dropped);
    slice_watch_site_add(t->by_game, game, bytes, &t->alloc_dropped);
}

/* The n keys with the most bytes (ties: more allocations), most first, into out (their slots); -> how many. */
static unsigned slice_watch_site_top(const slice_watch_site *t, unsigned n, unsigned *out)
{
    unsigned got = 0, i, k;

    for (i = 0; i < SLICE_WATCH_SITES; i++) {
        if (!t[i].count) {
            continue;
        }
        for (k = got; k > 0 && (t[i].bytes > t[out[k - 1]].bytes || (t[i].bytes == t[out[k - 1]].bytes && t[i].count > t[out[k - 1]].count)); k--) {
            if (k < n) {
                out[k] = out[k - 1];
            }
        }
        if (k < n) {
            out[k] = i;
            if (got < n) {
                got++;
            }
        }
    }
    return got;
}

/* One sample, taken after Azimuth gave `res` its next slice. */
static void slice_watch_note(slice_watch_table *t, unsigned long long res, unsigned long long caller, unsigned size, unsigned long long queue,
                             unsigned long long list, int front_ok, unsigned long long front_fence, unsigned long long passed, int from_list)
{
    unsigned i = (unsigned)((res * 0x9e3779b97f4a7c15ull) >> 55) & (SLICE_WATCH_SLOTS - 1), n;
    slice_watch_res *r = NULL;

    t->discards++;
    t->passed = passed;
    for (n = 0; n < SLICE_WATCH_SLOTS; n++, i = (i + 1) & (SLICE_WATCH_SLOTS - 1)) {
        if (t->r[i].res == res || !t->r[i].res) {
            r = &t->r[i];
            break;
        }
    }
    if (!r) {
        t->dropped++;
        return;
    }
    if (!r->res) {
        r->res = res;
        r->caller = caller;
    } else if (r->caller == SLICE_WATCH_UNKNOWN) {
        r->caller = caller;
    }
    r->discards++;
    r->size = size;
    r->front_ok = front_ok;
    r->front_fence = front_ok ? front_fence : 0;
    r->from_list |= from_list;
    if (queue > r->queue_max) {
        r->queue_max = queue;
    }
    if (list > r->list_max) {
        r->list_max = list;
    }
}

/* The n resources with the longest retire queues (ties: more slices held, then more discards), most first, into out (their slots); -> how many. */
static int slice_watch_before(const slice_watch_res *a, const slice_watch_res *b)
{
    if (a->queue_max != b->queue_max) {
        return a->queue_max > b->queue_max;
    }
    if (a->list_max != b->list_max) {
        return a->list_max > b->list_max;
    }
    return a->discards > b->discards;
}

static unsigned slice_watch_top(const slice_watch_table *t, unsigned n, unsigned *out)
{
    unsigned got = 0, i, k;

    for (i = 0; i < SLICE_WATCH_SLOTS; i++) {
        if (!t->r[i].res) {
            continue;
        }
        for (k = got; k > 0 && slice_watch_before(&t->r[i], &t->r[out[k - 1]]); k--) {
            if (k < n) {
                out[k] = out[k - 1];
            }
        }
        if (k < n) {
            out[k] = i;
            if (got < n) {
                got++;
            }
        }
    }
    return got;
}

/* The front of an MSVC std::deque<T*> (blocks of 2 pointers): the block index is (offset / 2) & (map size - 1), the place in it offset & 1. map_size must be a
 * power of two (MSVC keeps it so); -> 0 when it is not. */
static int slice_watch_front_slot(unsigned long long map_size, unsigned long long offset, unsigned long long *block, unsigned *place)
{
    if (!map_size || (map_size & (map_size - 1))) {
        return 0;
    }
    *block = (offset >> 1) & (map_size - 1);
    *place = (unsigned)(offset & 1);
    return 1;
}

#ifndef SLICE_WATCH_LOGIC_ONLY
/* azimuth.exe 1.0.4: the three functions hooked, and the retire pass whose bytes fix the field offsets read above (its first 0x60 bytes carry +0x14c0,
 * +0x130, +0x128, +0x120 and +0x118). Every byte compared: a different Azimuth build is left alone. */
#define SLICE_WATCH_RVA_RENAME 0x1c95c0u
#define SLICE_WATCH_RVA_INSTALL 0x1c97f0u
#define SLICE_WATCH_RVA_RETIRE 0x1c6780u
#define SLICE_WATCH_RVA_ALLOCATE 0x1ced10u
static const unsigned char g_sw_allocate_code[29] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18,
                                                      0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xc9};
static const unsigned char g_sw_rename_code[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57,
                                                    0x41, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xfa, 0x4c, 0x8b, 0xf1};
static const unsigned char g_sw_install_code[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89,
                                                     0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30};
static const unsigned char g_sw_retire_code[0x60] = {
    0x48, 0x8b, 0xc4, 0x53, 0x48, 0x83, 0xec, 0x50, 0x4c, 0x8b, 0x81, 0xc0, 0x14, 0x00, 0x00, 0x48, 0x8b, 0xda, 0x90, 0x4c, 0x89, 0x44, 0x24, 0x60, 0x48,
    0x83, 0xba, 0x30, 0x01, 0x00, 0x00, 0x00, 0x0f, 0x84, 0xdf, 0x01, 0x00, 0x00, 0x4c, 0x89, 0x60, 0xe0, 0x49, 0xbc, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x1f, 0x4c, 0x89, 0x68, 0xd8, 0x48, 0x89, 0x68, 0x18, 0x48, 0x89, 0x70, 0xf0, 0x48, 0x89, 0x78, 0xe8, 0x4c, 0x89, 0x70, 0xd0, 0x4c, 0x89, 0x78,
    0xc8, 0x0f, 0x1f, 0x40, 0x00, 0x48, 0x8b, 0x93, 0x28, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x83, 0x20, 0x01, 0x00, 0x00, 0x48, 0x8b};

typedef void *(*slice_watch_rename_fn)(void *owner, void *res);
typedef void *(*slice_watch_install_fn)(void *owner, void *res, void *recorded);
typedef void *(*slice_watch_allocate_fn)(void *arena, void *out, unsigned long long size, unsigned alignment);
static slice_watch_rename_fn g_sw_rename_orig;
static slice_watch_install_fn g_sw_install_orig;
static slice_watch_allocate_fn g_sw_allocate_orig;
static unsigned long long g_sw_az_lo;
static slice_watch_table g_sw;                 /* a global: the platform's crash dump carries it */
static SRWLOCK g_sw_lock = SRWLOCK_INIT;
static volatile LONG g_sw_on;
static int g_sw_state;                         /* 0 not tried, 1 hooked, -1 not available (the line saying why is written) */
static unsigned long long g_sw_game_lo, g_sw_game_hi;

static unsigned long long slice_watch_caller(void)
{
    void *frames[40];
    USHORT n = RtlCaptureStackBackTrace(2, 40, frames, NULL), i;

    for (i = 0; i < n; i++) {
        unsigned long long a = (unsigned long long)(ULONG_PTR)frames[i];
        if (a >= g_sw_game_lo && a < g_sw_game_hi) {
            return a - g_sw_game_lo;
        }
    }
    return SLICE_WATCH_UNKNOWN;
}

static void slice_watch_sample(void *owner, void *res, int from_list)
{
    unsigned long long r = (unsigned long long)(ULONG_PTR)res, o = (unsigned long long)(ULONG_PTR)owner;
    unsigned long long queue = 0, map = 0, map_size = 0, offset = 0, block = 0, front = 0, fence = 0, passed = 0, lb = 0, le = 0;
    unsigned long long caller = slice_watch_caller();
    unsigned size = 0, place = 0;
    int front_ok = 0;

    health_read(r + 0x90, &size, 4);
    health_read(r + 0x150, &lb, 8);
    health_read(r + 0x158, &le, 8);
    health_read(o + 0x14c0, &passed, 8);
    if (health_read(r + 0x130, &queue, 8) && queue && health_read(r + 0x118, &map, 8) && health_read(r + 0x120, &map_size, 8) &&
        health_read(r + 0x128, &offset, 8) && slice_watch_front_slot(map_size, offset, &block, &place) && health_read(map + block * 8, &front, 8) &&
        health_read(front + place * 8, &front, 8)) {
        front_ok = health_read(front + 0x40, &fence, 8);
    }
    AcquireSRWLockExclusive(&g_sw_lock);
    slice_watch_note(&g_sw, r, caller, size, queue, le > lb ? (le - lb) / 16 : 0, front_ok, fence, passed, from_list);
    ReleaseSRWLockExclusive(&g_sw_lock);
}

static void *slice_watch_rename_hook(void *owner, void *res)
{
    void *out = g_sw_rename_orig(owner, res);
    if (g_sw_on) {
        slice_watch_sample(owner, res, 0);
    }
    return out;
}

static void *slice_watch_install_hook(void *owner, void *res, void *recorded)
{
    void *out = g_sw_install_orig(owner, res, recorded);
    if (g_sw_on) {
        slice_watch_sample(owner, res, 1);
    }
    return out;
}

__attribute__((noinline)) static void *slice_watch_allocate_hook(void *arena, void *out, unsigned long long size, unsigned alignment)
{
    if (g_sw_on) {
        unsigned long long site = (unsigned long long)(ULONG_PTR)__builtin_return_address(0) - g_sw_az_lo, game = slice_watch_caller();
        AcquireSRWLockExclusive(&g_sw_lock);
        slice_watch_alloc(&g_sw, site, game, size);
        ReleaseSRWLockExclusive(&g_sw_lock);
    }
    return g_sw_allocate_orig(arena, out, size, alignment);
}

/* Hooks the three functions once, when the first window starts (the platform pauses the game's threads for each, about 50 ms, in a game that is already
 * running away). -> 1 when they are in place. */
static int slice_watch_hook(void)
{
    unsigned char *az = (unsigned char *)GetModuleHandleW(NULL), *game = find_fh2_image();
    FH2Hook *a, *b, *c;
    wchar_t env[8];

    if (g_sw_state) {
        return g_sw_state > 0;
    }
    g_sw_state = -1;
    if (GetEnvironmentVariableW(L"FH2MP_NO_SLICEWATCH", env, 8) != 0) {
        return 0;
    }
    if (!game || !g_api || !g_mod || IsBadReadPtr(az + SLICE_WATCH_RVA_RETIRE, sizeof g_sw_retire_code) ||
        bytes_first_diff(az + SLICE_WATCH_RVA_RETIRE, g_sw_retire_code, sizeof g_sw_retire_code) >= 0 ||
        bytes_first_diff(az + SLICE_WATCH_RVA_RENAME, g_sw_rename_code, sizeof g_sw_rename_code) >= 0 ||
        bytes_first_diff(az + SLICE_WATCH_RVA_INSTALL, g_sw_install_code, sizeof g_sw_install_code) >= 0 ||
        bytes_first_diff(az + SLICE_WATCH_RVA_ALLOCATE, g_sw_allocate_code, sizeof g_sw_allocate_code) >= 0) {
        log_begin();
        log_raw(game ? "health: slice watch: not azimuth.exe 1.0.4's buffer code at azimuth+0x1c95c0/0x1c97f0/0x1c6780/0x1ced10 (another Azimuth build or another "
                       "mod's hook), the watch does not run\n"
                     : "health: slice watch: the game's image is not known, the watch does not run\n");
        return 0;
    }
    g_sw_game_lo = (unsigned long long)(ULONG_PTR)game;
    g_sw_game_hi = g_sw_game_lo + FH2_SIZE_OF_IMAGE;
    g_sw_az_lo = (unsigned long long)(ULONG_PTR)az;
    a = g_api->hook_function(g_mod, plat_tag("slicewatch: Azimuth buffer discard"), az + SLICE_WATCH_RVA_RENAME, (void *)slice_watch_rename_hook,
                             (void **)&g_sw_rename_orig);
    b = a ? g_api->hook_function(g_mod, plat_tag("slicewatch: Azimuth command list buffer"), az + SLICE_WATCH_RVA_INSTALL, (void *)slice_watch_install_hook,
                                 (void **)&g_sw_install_orig)
          : NULL;
    c = b ? g_api->hook_function(g_mod, plat_tag("slicewatch: Azimuth buffer arena allocate"), az + SLICE_WATCH_RVA_ALLOCATE, (void *)slice_watch_allocate_hook,
                                 (void **)&g_sw_allocate_orig)
          : NULL;
    if (!a || !b || !c) {
        plat_failed("slicewatch: Azimuth buffer code", az + (!a ? SLICE_WATCH_RVA_RENAME : !b ? SLICE_WATCH_RVA_INSTALL : SLICE_WATCH_RVA_ALLOCATE),
                    "refused (overlaps another hook, or the code is not hookable)");
        return 0;
    }
    if (!g_api->enable_hooks(g_mod, a) || !g_api->enable_hooks(g_mod, b) || !g_api->enable_hooks(g_mod, c)) {
        plat_failed("slicewatch: Azimuth buffer discard", az + SLICE_WATCH_RVA_RENAME, "created but could not be switched on");
        return 0;
    }
    g_sw_state = 1;
    return 1;
}

/* From the health thread: start a window. -> 1 when one runs. */
static int slice_watch_start(void)
{
    if (g_sw_on || !slice_watch_hook()) {
        return 0;
    }
    AcquireSRWLockExclusive(&g_sw_lock);
    memset(&g_sw, 0, sizeof g_sw);
    ReleaseSRWLockExclusive(&g_sw_lock);
    InterlockedExchange(&g_sw_on, 1);
    return 1;
}

static void slice_watch_u64(unsigned long long v)
{
    char b[24];
    int n = 0;

    do {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        char c[2] = {b[--n], 0};
        log_raw(c);
    }
}

/* From the health thread at the end of a window: stops sampling and writes the lines. commit = the game's commit now. */
static void slice_watch_finish(const char *why, unsigned long long commit)
{
    static slice_watch_table t;
    unsigned top[SLICE_WATCH_TOP], n, i, resources = 0;

    InterlockedExchange(&g_sw_on, 0);
    AcquireSRWLockExclusive(&g_sw_lock);
    t = g_sw;
    ReleaseSRWLockExclusive(&g_sw_lock);
    for (i = 0; i < SLICE_WATCH_SLOTS; i++) {
        resources += t.r[i].res != 0;
    }
    n = slice_watch_top(&t, SLICE_WATCH_TOP, top);
    log_begin();
    log_raw("health: slice watch (");
    log_raw(why);
    log_raw(") over ");
    log_dec(SLICE_WATCH_S);
    log_raw(" s at commit ");
    health_mb(commit);
    log_raw(": ");
    log_dec(t.discards);
    log_raw(" buffer discards on ");
    log_dec(resources);
    log_raw(" buffers");
    if (t.dropped) {
        log_raw(" (");
        log_dec(t.dropped);
        log_raw(" on buffers past the table, not counted)");
    }
    log_raw(", GPU fence passed ");
    slice_watch_u64(t.passed);
    log_raw(n ? "; longest retire queues:" : "\n");
    for (i = 0; i < n; i++) {
        const slice_watch_res *r = &t.r[top[i]];
        log_raw(i ? "; " : " ");
        log_raw("0x");
        health_hex(r->res);
        log_raw(" (");
        log_dec(r->size);
        log_raw(" bytes");
        log_raw(r->from_list ? ", from command lists" : "");
        log_raw(") queue ");
        slice_watch_u64(r->queue_max);
        log_raw(", ");
        slice_watch_u64(r->list_max);
        log_raw(" slices held, front fence ");
        if (!r->front_ok) {
            log_raw("-");
        } else if (r->front_fence == ~0ull) {
            log_raw("unset");
        } else {
            slice_watch_u64(r->front_fence);
        }
        log_raw(", ");
        log_dec(r->discards);
        log_raw(" discards from ");
        if (r->caller == SLICE_WATCH_UNKNOWN) {
            log_raw("no game frame");
        } else {
            log_raw("game+0x");
            health_hex(r->caller);
        }
    }
    if (n) {
        log_raw("\n");
    }
    /* the arena: who asked for blocks in the same window */
    log_begin();
    log_raw("health: slice watch: ");
    slice_watch_u64(t.allocs);
    log_raw(" buffer arena blocks, ");
    health_mb(t.alloc_bytes);
    log_raw(" asked for");
    if (t.alloc_dropped) {
        log_raw(" (");
        log_dec(t.alloc_dropped);
        log_raw(" not placed in the tables)");
    }
    if (!t.allocs) {
        log_raw("\n");
        return;
    }
    log_raw("; by Azimuth call:");
    n = slice_watch_site_top(t.by_site, SLICE_WATCH_TOP, top);
    for (i = 0; i < n; i++) {
        log_raw(i ? ", " : " ");
        log_raw("azimuth+0x");
        health_hex(t.by_site[top[i]].key);
        log_raw(" (");
        slice_watch_u64(t.by_site[top[i]].count);
        log_raw(" blocks, ");
        slice_watch_u64(t.by_site[top[i]].bytes >> 10);
        log_raw(" KB)");
    }
    log_raw("; by game call:");
    n = slice_watch_site_top(t.by_game, SLICE_WATCH_TOP, top);
    for (i = 0; i < n; i++) {
        log_raw(i ? ", " : " ");
        if (t.by_game[top[i]].key == SLICE_WATCH_UNKNOWN) {
            log_raw("no game frame");
        } else {
            log_raw("game+0x");
            health_hex(t.by_game[top[i]].key);
        }
        log_raw(" (");
        slice_watch_u64(t.by_game[top[i]].count);
        log_raw(" blocks, ");
        slice_watch_u64(t.by_game[top[i]].bytes >> 10);
        log_raw(" KB)");
    }
    log_raw("\n");
}
#endif
