/* Guard counts (Black Box, "guards as data"): how often each of the shim's game-bug guards turned a crash away in this game, sent to the LSP on
 * the game's next request after a count changed, as "X-FH2MP-Guards: v1 camanim=3 dtconv=1" (xhr2.inc; native/internal/lsp guardcounts.go logs
 * it). Each guard keeps its own counter; this only reads them. Only counts above 0 are named, so a game whose guards never fired sends nothing.
 *
 * The part above the #ifndef has no Windows calls: shim/tests/test_guard_counts.sh runs it natively. */

typedef struct {
    const char *name;
    long count;
} guard_count;

static char *gc_put(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

/* The header value for n counts; "" when none is above 0. -> its length; `out` is always terminated. */
static unsigned guard_counts_text(char *out, unsigned cap, const guard_count *c, unsigned n)
{
    char *p = out, *end = out + (cap ? cap - 1 : 0);
    unsigned i, named = 0;
    if (!cap) return 0;
    for (i = 0; i < n; i++) {
        char d[12];
        int k = 0;
        unsigned long v;
        if (c[i].count <= 0) continue;
        p = gc_put(p, end, named++ ? " " : "v1 ");
        p = gc_put(p, end, c[i].name);
        p = gc_put(p, end, "=");
        v = (unsigned long)c[i].count;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k && p < end) *p++ = d[--k];
    }
    *p = 0;
    return (unsigned)(p - out);
}

/* -> 1 when any count differs from the last one sent (and remembers these as sent). */
static int guard_counts_changed(const guard_count *c, unsigned n, long *sent)
{
    unsigned i;
    int changed = 0;
    for (i = 0; i < n; i++) {
        if (c[i].count != sent[i]) {
            sent[i] = c[i].count;
            changed = 1;
        }
    }
    return changed;
}

#ifndef GUARD_COUNTS_LOGIC_ONLY

#define GUARD_COUNTS_N 12

/* The guards' own counters, read as they are (a racy read only delays a count to the next request). */
static unsigned guard_counts_read(guard_count *c)
{
    unsigned n = 0;
    c[n].name = "camanim", c[n++].count = g_cag_hits;
    c[n].name = "complookup", c[n++].count = g_cl_hits;
    c[n].name = "dtconv", c[n++].count = g_dtc_hits;
    c[n].name = "modelguard", c[n++].count = g_mu_hits;
    c[n].name = "garagerating", c[n++].count = g_gr_hits;
    c[n].name = "travelxp", c[n++].count = g_txg_hits;
    c[n].name = "predguard", c[n++].count = g_rp_hits;
    c[n].name = "logguard", c[n++].count = g_gl_dropped;
    c[n].name = "speedometer", c[n++].count = g_hg_logged;
    c[n].name = "torqueload", c[n++].count = g_torque_rejects;
    c[n].name = "placement", c[n++].count = g_place_logged;
    c[n].name = "netstrings", c[n++].count = g_string_rejections[0] + g_string_rejections[1] + g_string_rejections[2];
    return n;
}

static volatile LONG g_gc_lock;
static long g_gc_sent[GUARD_COUNTS_N];

/* For xhr2.inc: "X-FH2MP-Guards: <value>" in `line` (600 wide chars) when a count changed since the last request that carried it. */
static int guard_counts_header(wchar_t *line)
{
    static const wchar_t name[] = L"X-FH2MP-Guards: ";
    guard_count c[GUARD_COUNTS_N];
    char text[300];
    unsigned n, i, k;
    int changed;
    if (InterlockedCompareExchange(&g_gc_lock, 1, 0) != 0) return 0;   /* another request is building it: this one goes without */
    n = guard_counts_read(c);
    changed = guard_counts_changed(c, n, g_gc_sent);
    InterlockedExchange(&g_gc_lock, 0);
    if (!changed || !guard_counts_text(text, sizeof text, c, n)) return 0;
    for (k = 0; name[k]; k++) line[k] = name[k];
    for (i = 0; text[i]; i++) line[k + i] = (wchar_t)(unsigned char)text[i];
    line[k + i] = 0;
    return 1;
}

#endif
