/* What the game is doing when nothing fails. The release log only has failures, so a game that stops asking the server for anything, a request that never
 * comes back, a game that stops simulating and a process that stopped running all look the same: no lines. The reports that came down to this: Xls calls
 * ending mid-session (dev 2026-11-01), 111 s after being matched (dev 2026-11-01), the freeze in issue #354.
 *
 * One thread wakes once a second and reads what the hooks already record: the time of the last own-car prediction (lighting_send) and the request counters of
 * xhr2.inc. No hook gets slower. It writes a line when one of these changes:
 *   health: process          the thread itself did not run for seconds: the whole process stood still (a debugger, swapping, the machine suspended, Wine)
 *   health: predictions      the game stopped or resumed sending its car; this also happens in loading screens, so it only says when, not why
 * and every HEALTH_SUMMARY_S seconds a summary with the requests of that period, if there were any or some are in flight. FH2MP_NO_HEALTH turns it off.
 *
 * The part above the #ifndef has no Windows calls: shim/tests/test_health.sh runs it natively; shim/tests/test_health_thread.sh runs the thread under Wine. */
#define HEALTH_PROCESS_GAP_MS 3000u     /* a wake this late means the process did not run */
#define HEALTH_FRESH_MS 2000u           /* predictions counted as flowing */
#define HEALTH_SILENT_MS 5000u          /* flowing before, nothing for this long: stopped */
#ifndef HEALTH_SUMMARY_S
#define HEALTH_SUMMARY_S 300u
#endif
#define HEALTH_LINES_MAX 120u           /* stop/resume lines per game; after that every 25th */

/* Memory (a player's PC froze with RAM going from 55% to 100% in seconds, 1.2.0, 2026-11): a jump within HEALTH_MEM_WINDOW s writes one line saying what grew,
 * and every HEALTH_SUMMARY_S s a line with the levels when they moved by HEALTH_MEM_MOVED since the last one. The process's private commit includes its D3D12
 * memory, in VRAM or not (measured on an RTX 3080 Ti and an RX 6600, 2026-11-09); free RAM falls once VRAM overflows into system memory. */
#define HEALTH_MEM_WINDOW 3u
#define HEALTH_MEM_COMMIT_JUMP (1ull << 30)
#define HEALTH_MEM_AVAIL_DROP (2ull << 30)
#define HEALTH_MEM_GPU_JUMP (1ull << 30)
#define HEALTH_MEM_MOVED (256ull << 20)
#define HEALTH_MEM_LINES_MAX 20u        /* jump lines per game; after that every 25th */
#define HEALTH_MEM_UNKNOWN (~0ull)

enum { HEALTH_NOTHING = 0, HEALTH_PROCESS_PAUSED = 1, HEALTH_PREDICTIONS_STOPPED = 2, HEALTH_PREDICTIONS_RESUMED = 4 };
enum { HEALTH_MEM_COMMIT = 1, HEALTH_MEM_AVAIL = 2, HEALTH_MEM_GPU = 4 };

typedef struct {
    unsigned long long commit[HEALTH_MEM_WINDOW + 1], avail[HEALTH_MEM_WINDOW + 1], gpu[HEALTH_MEM_WINDOW + 1];
    unsigned n, lines;
} health_mem;

/* One sample a second: commit and gpu (non-local GPU memory, HEALTH_MEM_UNKNOWN when not readable) of this process, avail = the system's free physical memory, all
 * in bytes. -> HEALTH_MEM_ bits of what jumped since the oldest sample of the window, with the amounts in d[0..2] (commit, avail, gpu); *write = the line is written
 * (HEALTH_MEM_LINES_MAX per game, then every 25th). A jump starts the window again at this sample, so one jump is one line. */
static int health_memory_step(health_mem *m, unsigned long long commit, unsigned long long avail, unsigned long long gpu, unsigned long long *d, int *write)
{
    int out = 0;
    unsigned i;

    if (m->n == HEALTH_MEM_WINDOW + 1) {
        for (i = 0; i < HEALTH_MEM_WINDOW; i++) {
            m->commit[i] = m->commit[i + 1];
            m->avail[i] = m->avail[i + 1];
            m->gpu[i] = m->gpu[i + 1];
        }
        m->n--;
    }
    m->commit[m->n] = commit;
    m->avail[m->n] = avail;
    m->gpu[m->n] = gpu;
    m->n++;
    d[0] = commit > m->commit[0] ? commit - m->commit[0] : 0;
    d[1] = m->avail[0] > avail ? m->avail[0] - avail : 0;
    d[2] = gpu != HEALTH_MEM_UNKNOWN && m->gpu[0] != HEALTH_MEM_UNKNOWN && gpu > m->gpu[0] ? gpu - m->gpu[0] : 0;
    out |= d[0] >= HEALTH_MEM_COMMIT_JUMP ? HEALTH_MEM_COMMIT : 0;
    out |= d[1] >= HEALTH_MEM_AVAIL_DROP ? HEALTH_MEM_AVAIL : 0;
    out |= d[2] >= HEALTH_MEM_GPU_JUMP ? HEALTH_MEM_GPU : 0;
    *write = 0;
    if (out) {
        m->commit[0] = commit;
        m->avail[0] = avail;
        m->gpu[0] = gpu;
        m->n = 1;
        m->lines++;
        *write = m->lines <= HEALTH_MEM_LINES_MAX || m->lines % 25 == 0;
    }
    return out;
}

/* The periodic level line: written when nothing was written yet (last_commit 0) or commit or GPU memory moved by HEALTH_MEM_MOVED since the last one. */
static int health_memory_moved(unsigned long long last_commit, unsigned long long last_gpu, unsigned long long commit, unsigned long long gpu)
{
    unsigned long long dc = commit > last_commit ? commit - last_commit : last_commit - commit;
    unsigned long long dg = gpu == HEALTH_MEM_UNKNOWN || last_gpu == HEALTH_MEM_UNKNOWN ? 0 : gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;

    return !last_commit || dc >= HEALTH_MEM_MOVED || dg >= HEALTH_MEM_MOVED;
}

typedef struct {
    unsigned long long woke, silent_from;
    int flowing, silent;
} health_state;

/* now and last_prediction are GetTickCount64 values; last_prediction 0 = none yet (or the lighting hooks are off). -> HEALTH_ bits; *ms = the length of what ended
 * or, for a stop, how long it has been silent. A process pause reports through *pause_ms. */
static int health_step(health_state *h, unsigned long long now, unsigned long long last_prediction, unsigned long long *ms, unsigned long long *pause_ms)
{
    int out = HEALTH_NOTHING;
    unsigned long long age = last_prediction && now >= last_prediction ? now - last_prediction : ~0ULL;

    if (h->woke && now - h->woke >= HEALTH_PROCESS_GAP_MS) {
        out |= HEALTH_PROCESS_PAUSED;
        *pause_ms = now - h->woke;
    }
    h->woke = now;
    if (h->silent) {
        if (last_prediction != h->silent_from && age < HEALTH_FRESH_MS) {
            h->silent = 0;
            h->flowing = 1;
            out |= HEALTH_PREDICTIONS_RESUMED;
            *ms = last_prediction - h->silent_from;
        }
    } else if (age < HEALTH_FRESH_MS) {
        h->flowing = 1;
    } else if (h->flowing && age != ~0ULL && age >= HEALTH_SILENT_MS) {
        h->silent = 1;
        h->flowing = 0;
        h->silent_from = last_prediction;
        out |= HEALTH_PREDICTIONS_STOPPED;
        *ms = age;
    }
    return out;
}

/* Exactly 8 lowercase hex digits (what the launcher writes and native/internal/diag RunID accepts). */
static int run_id_ok(const char *s)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return s[8] == 0;
}

/* "ab12cd34.57": the game (run) and the request (the xhr# of fh2mp.log), for X-FH2MP-Run. out holds 20 characters. */
static void run_request_text(char *out, const char *run, unsigned long seq)
{
    char digits[11];
    int n = 0, i = 0;

    do {
        digits[n++] = (char)('0' + seq % 10);
        seq /= 10;
    } while (seq);
    while (run[i]) {
        out[i] = run[i];
        i++;
    }
    out[i++] = '.';
    while (n) {
        out[i++] = digits[--n];
    }
    out[i] = 0;
}

/* Heap census (memory-growth-oom, #91/#468/#477/#480, 2026-11-14). In the runaways the game's commit climbs about 1 GB every 3-5 s from the first seconds of a
 * boot to 51-61 GB (16 GB PCs, AMD and NVIDIA, launchers 1.3.3 and 1.4.0) while GPU memory stays flat. The dumps (7d03d5bf1d83e9d3, f0aec613ae84fa07) show where:
 * 2860-3290 private allocations of 15.9-16.2 MB, each a segment of one NT heap (signature 0xffeeffee at +0x10, the heap at +0x28), 46-51 GB in all, against
 * about 85 such segments in a healthy session. The few heap bytes a minidump keeps point mostly at Azimuth's own objects: std::shared_ptr control blocks of
 * azimuth::gfx::BufferArena::Block (vtable azimuth+0x6f40f8, 167 and 221 hits), then its ordinary_block, Slice and title_storage blocks [C: RTTI of Azimuth
 * 1.0.4; that these fill the heap is inferred, the sample is what the threads' stacks pointed at]. A dump cannot say what keeps them, so the watcher counts
 * it on the player's PC: when the game's commit passes 1.25 x RAM (healthy play stays under RAM: 11-16 GB on 32 GB PCs, 12 GB on 16 GB, Notion 2026-11-09)
 * and again at 2.5 x RAM, it reads pages spread over the largest heap's segments and logs the pointers into loaded modules found there most often, with the
 * class names Azimuth's and the game's RTTI give them. It reads only (no heap lock: the game never waits for it) and changes nothing. */
#define HEALTH_CENSUS_SEGMENTS 64u      /* segments sampled */
#define HEALTH_CENSUS_PAGES 4u          /* pages read in each */
#define HEALTH_CENSUS_TOP 10u           /* pointers logged */
#define HEALTH_TALLY_SLOTS 2048u

/* -> 1 when a census is due at this commit (stage 0: commit >= 1.25 x RAM, stage 1: >= 2.5 x RAM); *stage moves on. total 0 = RAM unknown: never. */
static int health_census_due(unsigned long long commit, unsigned long long total, int *stage)
{
    if (!total) {
        return 0;
    }
    if (*stage == 0 && commit / 5 >= total / 4) {
        *stage = 1;
        return 1;
    }
    if (*stage == 1 && commit / 5 >= total / 2) {
        *stage = 2;
        return 1;
    }
    return 0;
}

/* The first 0x40 bytes of a private allocation at base: an NT heap segment (_HEAP_SEGMENT: SegmentSignature 0xffeeffee at +0x10, Heap at +0x28, BaseAddress
 * at +0x30). -> its heap, or 0. */
static unsigned long long health_heap_segment(const unsigned char *hdr, unsigned long long base)
{
    unsigned sig, i;
    unsigned long long heap = 0, self = 0;

    sig = (unsigned)hdr[0x10] | (unsigned)hdr[0x11] << 8 | (unsigned)hdr[0x12] << 16 | (unsigned)hdr[0x13] << 24;
    for (i = 0; i < 8; i++) {
        heap |= (unsigned long long)hdr[0x28 + i] << (8 * i);
        self |= (unsigned long long)hdr[0x30 + i] << (8 * i);
    }
    return sig == 0xffeeffeeu && self == base && heap ? heap : 0;
}

/* Counts of 64-bit values (open addressing, HEALTH_TALLY_SLOTS; a full table drops new values and counts them in dropped). */
typedef struct {
    unsigned long long key[HEALTH_TALLY_SLOTS];
    unsigned count[HEALTH_TALLY_SLOTS];
    unsigned long dropped;
} health_tally;

static void health_tally_add(health_tally *t, unsigned long long v)
{
    unsigned i = (unsigned)((v * 0x9e3779b97f4a7c15ull) >> 53) & (HEALTH_TALLY_SLOTS - 1), n;

    for (n = 0; n < HEALTH_TALLY_SLOTS; n++, i = (i + 1) & (HEALTH_TALLY_SLOTS - 1)) {
        if (t->count[i] && t->key[i] == v) {
            t->count[i]++;
            return;
        }
        if (!t->count[i]) {
            t->key[i] = v;
            t->count[i] = 1;
            return;
        }
    }
    t->dropped++;
}

/* The n most counted values, most first, into out (their slots); -> how many. Ties keep the lower slot first. */
static unsigned health_tally_top(const health_tally *t, unsigned n, unsigned *out)
{
    unsigned got = 0, i, k;

    for (i = 0; i < HEALTH_TALLY_SLOTS; i++) {
        if (!t->count[i]) {
            continue;
        }
        for (k = got; k > 0 && t->count[out[k - 1]] < t->count[i]; k--) {
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

#ifndef HEALTH_LOGIC_ONLY
#include <psapi.h>

/* The game's own GPU memory: DXGI's QueryVideoMemoryInfo answers for the calling process. Windows only (under Wine dxgi is DXVK's, which would start Vulkan in
 * this thread), opened 30 s into the game, up to 4 adapters summed (the game uses one). Vtable slots: IDXGIFactory1::EnumAdapters1 12, IDXGIAdapter3::
 * QueryVideoMemoryInfo 14 (segment group 0 local, 1 non-local). */
#define HEALTH_GPU_AFTER_S 30u
typedef struct { unsigned long long budget, usage, available_for_reservation, reservation; } health_vmi;
typedef HRESULT (WINAPI *health_factory_fn)(const GUID *, void **);
typedef HRESULT (STDMETHODCALLTYPE *health_enum_fn)(void *, UINT, void **);
typedef HRESULT (STDMETHODCALLTYPE *health_qi_fn)(void *, const GUID *, void **);
typedef ULONG (STDMETHODCALLTYPE *health_release_fn)(void *);
typedef HRESULT (STDMETHODCALLTYPE *health_vmi_fn)(void *, UINT, int, health_vmi *);
static const GUID g_health_iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
static const GUID g_health_iid_adapter3 = {0x645967a4, 0x1392, 0x4310, {0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd}};
static void *g_health_adapters[4];
static int g_health_gpu_state;                 /* 0 not opened yet, 1 readable, -1 not readable */

static void health_gpu_open(void)
{
    HMODULE dxgi;
    health_factory_fn create = NULL;
    void *factory = NULL, *adapter;
    HRESULT hr = E_FAIL;
    UINT i;
    int n = 0;

    g_health_gpu_state = -1;
    if (GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
        return;
    }
    dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        create = (health_factory_fn)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
    }
    if (create) {
        hr = create(&g_health_iid_factory1, &factory);
    }
    if (FAILED(hr) || !factory) {
        log_begin();
        log_raw("health: the game's GPU memory is not readable (CreateDXGIFactory1 ");
        log_raw(create ? "failed" : "not found");
        log_raw("), memory lines carry no GPU numbers\n");
        return;
    }
    for (i = 0; i < 8 && n < 4; i++) {
        adapter = NULL;
        if (FAILED(((health_enum_fn)(*(void ***)factory)[12])(factory, i, &adapter)) || !adapter) {
            break;
        }
        if (SUCCEEDED(((health_qi_fn)(*(void ***)adapter)[0])(adapter, &g_health_iid_adapter3, &g_health_adapters[n])) && g_health_adapters[n]) {
            n++;
        }
        ((health_release_fn)(*(void ***)adapter)[2])(adapter);
    }
    ((health_release_fn)(*(void ***)factory)[2])(factory);
    g_health_gpu_state = n ? 1 : -1;
}

static void health_gpu_read(unsigned long long *local, unsigned long long *nonlocal)
{
    health_vmi v;
    int i;

    *local = *nonlocal = HEALTH_MEM_UNKNOWN;
    if (g_health_gpu_state != 1) {
        return;
    }
    *local = *nonlocal = 0;
    for (i = 0; i < 4 && g_health_adapters[i]; i++) {
        health_vmi_fn query = (health_vmi_fn)(*(void ***)g_health_adapters[i])[14];
        if (SUCCEEDED(query(g_health_adapters[i], 0, 0, &v))) {
            *local += v.usage;
        }
        if (SUCCEEDED(query(g_health_adapters[i], 0, 1, &v))) {
            *nonlocal += v.usage;
        }
    }
}

typedef struct { unsigned long long commit, peak, avail, total, gpu_local, gpu_shared; } health_mem_now;

static int health_mem_read(health_mem_now *s)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    MEMORYSTATUSEX ms;

    pmc.cb = sizeof pmc;
    ms.dwLength = sizeof ms;
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc) || !GlobalMemoryStatusEx(&ms)) {
        return 0;
    }
    s->commit = pmc.PrivateUsage;
    s->peak = pmc.PeakPagefileUsage;
    s->avail = ms.ullAvailPhys;
    s->total = ms.ullTotalPhys;
    health_gpu_read(&s->gpu_local, &s->gpu_shared);
    return 1;
}

static void health_mb(unsigned long long bytes)
{
    log_dec((unsigned long)(bytes >> 20));
    log_raw(" MB");
}

/* "commit A MB (peak B MB), free RAM C of D MB, GPU local E MB, shared F MB, N texture fallbacks" */
static void health_mem_levels(const health_mem_now *s)
{
    log_raw("commit ");
    health_mb(s->commit);
    log_raw(" (peak ");
    health_mb(s->peak);
    log_raw("), free RAM ");
    health_mb(s->avail);
    log_raw(" of ");
    health_mb(s->total);
    if (s->gpu_local != HEALTH_MEM_UNKNOWN) {
        log_raw(", GPU local ");
        health_mb(s->gpu_local);
        log_raw(", shared ");
        health_mb(s->gpu_shared);
    }
    log_raw(", ");
    log_dec((unsigned long)g_place_logged);
    log_raw(" texture fallbacks");
}

/* ---- the heap census (see health_census_due) */
typedef struct { unsigned long long base, size; char name[40]; } health_module;
static health_module g_census_mods[384];
static unsigned g_census_nmods;
static health_tally g_census_tally;
static unsigned char g_census_page[4096];

static void health_hex(unsigned long long v)        /* as few digits as v needs */
{
    int digits = 1;
    while (digits < 16 && (v >> (4 * digits))) {
        digits++;
    }
    log_hex(v, digits);
}

static int health_read(unsigned long long at, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(ULONG_PTR)at, out, n, &got) && got == n;
}

static void health_census_modules(void)
{
    HMODULE mods[384];
    DWORD need = 0;
    unsigned i, n;

    g_census_nmods = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) {
        return;
    }
    n = need / sizeof mods[0];
    for (i = 0; i < n && i < 384; i++) {
        MODULEINFO mi;
        health_module *m = &g_census_mods[g_census_nmods];
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) {
            continue;
        }
        m->base = (unsigned long long)(ULONG_PTR)mi.lpBaseOfDll;
        m->size = mi.SizeOfImage;
        if (!K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], m->name, sizeof m->name)) {
            m->name[0] = 0;
        }
        g_census_nmods++;
    }
}

static const health_module *health_module_of(unsigned long long v)
{
    unsigned i;
    for (i = 0; i < g_census_nmods; i++) {
        if (v >= g_census_mods[i].base && v < g_census_mods[i].base + g_census_mods[i].size) {
            return &g_census_mods[i];
        }
    }
    return NULL;
}

/* An MSVC vtable's class: the Complete Object Locator before it (signature 1, image-relative type descriptor at +12), the decorated name 16 bytes into the type
 * descriptor (".?AV..."). Written without ".?AV"/".?AU", up to 90 characters. -> 0 when v is not a vtable with RTTI. */
static int health_log_class(const health_module *m, unsigned long long v)
{
    unsigned long long col = 0;
    unsigned sig = 0, td = 0;
    char name[96];
    unsigned i;

    if (!health_read(v - 8, &col, 8) || col < m->base || col >= m->base + m->size || !health_read(col, &sig, 4) || sig != 1 ||
        !health_read(col + 12, &td, 4) || td >= m->size || !health_read(m->base + td + 16, name, sizeof name - 1)) {
        return 0;
    }
    name[sizeof name - 1] = 0;
    if (name[0] != '.' || name[1] != '?' || name[2] != 'A') {
        return 0;
    }
    for (i = 4; name[i] && i < 94; i++) {
        if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e) {
            break;
        }
    }
    name[i] = 0;
    log_raw(" (");
    log_raw(name + 4);
    log_raw(")");
    return 1;
}

static void health_census(const health_mem_now *s)
{
    static unsigned long long seg_base[8192], seg_size[8192];
    struct { unsigned long long heap, bytes; unsigned segs; } heaps[16];
    unsigned nheaps = 0, nseg = 0, i, k, pages = 0, top[HEALTH_CENSUS_TOP], ntop, biggest = 0;
    unsigned long long at = 0x10000, other = 0;
    MEMORY_BASIC_INFORMATION mbi;
    unsigned long long process_heap = (unsigned long long)(ULONG_PTR)GetProcessHeap();

    health_census_modules();
    memset(&g_census_tally, 0, sizeof g_census_tally);
    /* every private allocation that starts with a heap segment header, grouped by its heap */
    while (VirtualQuery((LPCVOID)(ULONG_PTR)at, &mbi, sizeof mbi) == sizeof mbi) {
        unsigned long long base = (unsigned long long)(ULONG_PTR)mbi.BaseAddress, size = mbi.RegionSize;
        unsigned char hdr[0x40];
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.BaseAddress == mbi.AllocationBase && size >= 0x10000 &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && health_read(base, hdr, sizeof hdr)) {
            unsigned long long heap = health_heap_segment(hdr, base);
            if (heap) {
                unsigned long long total = 0;
                MEMORY_BASIC_INFORMATION r;
                unsigned long long p = base;
                /* the segment's committed bytes: its regions up to the next allocation */
                while (VirtualQuery((LPCVOID)(ULONG_PTR)p, &r, sizeof r) == sizeof r && r.AllocationBase == mbi.AllocationBase) {
                    if (r.State == MEM_COMMIT) {
                        total += r.RegionSize;
                    }
                    p = (unsigned long long)(ULONG_PTR)r.BaseAddress + r.RegionSize;
                }
                for (k = 0; k < nheaps && heaps[k].heap != heap; k++) {
                }
                if (k == nheaps && nheaps < 16) {
                    heaps[nheaps].heap = heap, heaps[nheaps].bytes = 0, heaps[nheaps].segs = 0;
                    nheaps++;
                }
                if (k < nheaps) {
                    heaps[k].bytes += total;
                    heaps[k].segs++;
                }
                if (k < nheaps && nseg < 8192) {
                    seg_base[nseg] = base | (k & 15);     /* the heap's index in the low bits (bases are 64 KB aligned) */
                    seg_size[nseg] = total;
                    nseg++;
                }
                size = p > base ? p - base : size;
            }
        }
        if (base + size <= at) {
            break;
        }
        at = base + size;
    }
    for (k = 1; k < nheaps; k++) {
        if (heaps[k].bytes > heaps[biggest].bytes) {
            biggest = k;
        }
    }
    for (k = 0; k < nheaps; k++) {
        if (k != biggest) {
            other += heaps[k].bytes;
        }
    }
    log_begin();
    log_raw("health: heap census at commit ");
    health_mb(s->commit);
    log_raw(" (over ");
    log_raw(s->commit / 5 >= s->total / 2 ? "2.5" : "1.25");
    log_raw(" x RAM): ");
    if (!nheaps) {
        log_raw("no NT heap segment found (another heap kind), nothing sampled\n");
        return;
    }
    log_raw("heap 0x");
    health_hex(heaps[biggest].heap);
    log_raw(heaps[biggest].heap == process_heap ? " (the process heap) has " : " (not the process heap) has ");
    log_dec(heaps[biggest].segs);
    log_raw(" segments, ");
    health_mb(heaps[biggest].bytes);
    log_raw("; ");
    log_dec(nheaps - 1);
    log_raw(" other heaps ");
    health_mb(other);
    log_raw("\n");
    /* HEALTH_CENSUS_PAGES pages at even offsets in HEALTH_CENSUS_SEGMENTS segments of that heap spread over all of them */
    {
        unsigned mine = 0, want, step_n, picked = 0, idx = 0;
        for (i = 0; i < nseg; i++) {
            mine += (seg_base[i] & 15) == biggest;
        }
        want = mine < HEALTH_CENSUS_SEGMENTS ? mine : HEALTH_CENSUS_SEGMENTS;
        step_n = want ? mine / want : 1;
        for (i = 0; i < nseg && picked < want; i++) {
            unsigned long long b = seg_base[i] & ~15ull;
            if ((seg_base[i] & 15) != biggest) {
                continue;
            }
            if (idx++ % step_n) {
                continue;
            }
            picked++;
            for (k = 0; k < HEALTH_CENSUS_PAGES; k++) {
                unsigned long long off = (seg_size[i] / HEALTH_CENSUS_PAGES * k + 0x1000) & ~0xfffull, q;
                if (off + sizeof g_census_page > seg_size[i] || !health_read(b + off, g_census_page, sizeof g_census_page)) {
                    continue;
                }
                pages++;
                for (q = 0; q < sizeof g_census_page; q += 8) {
                    unsigned long long v;
                    memcpy(&v, g_census_page + q, 8);
                    if (v >= 0x10000 && health_module_of(v)) {
                        health_tally_add(&g_census_tally, v);
                    }
                }
            }
        }
    }
    ntop = health_tally_top(&g_census_tally, HEALTH_CENSUS_TOP, top);
    log_begin();
    log_raw("health: heap census: in ");
    log_dec(pages);
    log_raw(" pages of that heap the pointers into modules found most:");
    if (!ntop) {
        log_raw(" none");
    }
    for (i = 0; i < ntop; i++) {
        unsigned long long v = g_census_tally.key[top[i]];
        const health_module *m = health_module_of(v);
        log_raw(i ? "; " : " ");
        log_dec(g_census_tally.count[top[i]]);
        log_raw(" x ");
        log_raw(m && m->name[0] ? m->name : "?");
        log_raw("+0x");
        health_hex(m ? v - m->base : v);
        if (m) {
            health_log_class(m, v);
        }
    }
    log_raw("\n");
}

static void health_counts(const char *label, int cls)
{
    LONG ok = InterlockedExchange(&g_xhr_ok[cls], 0), bad = InterlockedExchange(&g_xhr_bad[cls], 0);
    ULONGLONG last = g_xhr_last[cls];

    log_raw(label);
    log_raw(" ");
    log_dec((unsigned long)ok);
    log_raw(" ok ");
    log_dec((unsigned long)bad);
    log_raw(" bad, last ");
    if (last) {
        log_dec((unsigned long)((GetTickCount64() - last) / 1000));
        log_raw(" s ago");
    } else {
        log_raw("never");
    }
}

static DWORD WINAPI health_thread(LPVOID unused)
{
    health_state h = {0, 0, 0, 0};
    static health_mem mem;
    unsigned ticks = 0, lines = 0, secs = 0;
    int census_stage = 0;
    unsigned long long mem_last_commit = 0, mem_last_gpu = HEALTH_MEM_UNKNOWN;

    (void)unused;
    for (;;) {
        unsigned long long now, ms = 0, pause_ms = 0, last, d[3];
        health_mem_now s;
        int what, jump, write;

        Sleep(1000);
        if (++secs == HEALTH_GPU_AFTER_S) {
            health_gpu_open();
        }
        if (health_mem_read(&s)) {
            if (health_census_due(s.commit, s.total, &census_stage)) {
                health_census(&s);
            }
            jump = health_memory_step(&mem, s.commit, s.avail, s.gpu_shared, d, &write);
            if (jump && write) {
                log_begin();
                log_raw("health: memory jumped within ");
                log_dec(HEALTH_MEM_WINDOW);
                log_raw(" s:");
                if (jump & HEALTH_MEM_COMMIT) {
                    log_raw(" commit +");
                    health_mb(d[0]);
                }
                if (jump & HEALTH_MEM_AVAIL) {
                    log_raw(" free RAM -");
                    health_mb(d[1]);
                }
                if (jump & HEALTH_MEM_GPU) {
                    log_raw(" GPU shared +");
                    health_mb(d[2]);
                }
                log_raw("; now ");
                health_mem_levels(&s);
                log_raw(", ");
                log_dec((unsigned long)g_xhr_in_flight);
                log_raw(" requests in flight\n");
            }
            if (ticks + 1 >= HEALTH_SUMMARY_S && health_memory_moved(mem_last_commit, mem_last_gpu, s.commit, s.gpu_shared)) {
                mem_last_commit = s.commit;
                mem_last_gpu = s.gpu_shared;
                log_begin();
                log_raw("health: memory: ");
                health_mem_levels(&s);
                log_raw("\n");
            }
        }
        AcquireSRWLockShared(&g_light_lock);
        last = g_light.prediction_at;
        ReleaseSRWLockShared(&g_light_lock);
        now = GetTickCount64();
        what = health_step(&h, now, last, &ms, &pause_ms);
        if (what & HEALTH_PROCESS_PAUSED) {
            log_begin();
            log_raw("health: process did not run for ");
            log_dec((unsigned long)pause_ms);
            log_raw(" ms (a 1 s timer woke that late)\n");
        }
        if ((what & (HEALTH_PREDICTIONS_STOPPED | HEALTH_PREDICTIONS_RESUMED)) && (++lines <= HEALTH_LINES_MAX || lines % 25 == 0)) {
            log_begin();
            log_raw(what & HEALTH_PREDICTIONS_STOPPED ? "health: predictions stopped, none for " : "health: predictions resumed after ");
            log_dec((unsigned long)ms);
            log_raw(" ms, ");
            log_dec((unsigned long)g_xhr_in_flight);
            log_raw(" requests in flight\n");
        }
        if (++ticks >= HEALTH_SUMMARY_S) {
            LONG flying = g_xhr_in_flight;
            ULONGLONG oldest = xhr_oldest_start();

            ticks = 0;
            if (InterlockedCompareExchange(&g_xhr_activity, 0, 1) == 0 && !flying) {
                continue;     /* nothing asked for since the last summary and nothing waiting: the line before this still holds */
            }
            log_begin();
            log_raw("health: ");
            log_dec(HEALTH_SUMMARY_S);
            log_raw(" s: ");
            health_counts("xls", XHR_CLASS_XLS);
            log_raw("; ");
            health_counts("other", XHR_CLASS_OTHER);
            log_raw("; in flight ");
            log_dec((unsigned long)flying);
            if (oldest) {
                log_raw(", oldest ");
                log_dec((unsigned long)((GetTickCount64() - oldest) / 1000));
                log_raw(" s");
            }
            log_raw("; last prediction ");
            if (last) {
                log_dec((unsigned long)((now - last) / 1000));
                log_raw(" s ago\n");
            } else {
                log_raw("never\n");
            }
        }
    }
    return 0;
}

static void health_start(void)
{
    wchar_t off[8];
    HANDLE t;

    if (GetEnvironmentVariableW(L"FH2MP_NO_HEALTH", off, 8) != 0) {
        return;
    }
    t = CreateThread(NULL, 0, health_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    } else {
        DWORD err = GetLastError();
        log_begin();
        log_raw("health: could not start the watcher thread, error ");
        log_dec(err);
        log_raw("\n");
    }
}
#endif
