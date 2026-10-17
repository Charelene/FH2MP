/*
 * dbg.inc - live in-process debugger for the FH2 client (included by azimuth_tls_proxy.c).
 *
 * A localhost TCP command server runs inside the game process (port is written to fh2mp-dbg.port next
 * to the shim; 47001.. first free). Use shim/fh2dbg.py. Nothing needs arming at launch or re-building:
 *
 *   rd <addr> <len>              hex dump (addr = expression, or a bare RVA)
 *   wr <addr> <hexbytes>         write memory
 *   bp <addr> [name=N] [log=e1,e2] [mem=expr:len]... [bt[=N]] [ret] [once|count=N] [cond=a==b|a!=b] [off]
 *   bl | bd <id|all> | be <id>   list / delete / enable breakpoints
 *   watch <addr> <1|2|4|8> [w|rw] [name=N] [bt[=N]]   hardware watchpoint ("who writes this?")
 *   wl | wd <id|all>             list / delete watchpoints
 *   thr                          threads
 *   ev [since]                   fetch events (breakpoint hits, watch hits, crash reports)
 *   selftest                     checks that breakpoints and hardware watchpoints work on this system
 *
 * Expressions: numbers (0x.. or decimal), registers (rax..r15, rip, rsp), img (image base), + - &,
 * [e] = qword at e, d[e] dword, w[e] word, b[e] byte, parentheses. A value below 0x4000000 used as an
 * address means "RVA in the game image". Access violations are reported automatically (registers,
 * what pointer registers point at, call-site backtrace) to the event log and fh2mp-crash-N.txt.
 * Disable everything with env FH2MP_NO_DBG.
 */

#include <tlhelp32.h>
#include <winsock2.h>

#define DBG_MAX_BP 96
#define DBG_MAX_WATCH 8   /* slots 0-3 can be hardware (debug registers), any slot can be page-guard */
#define DBG_RING 2048
#define DBG_LINE 400
#define DBG_OUT 60000

typedef struct {
    char *p;
    int n;
    int cap;
} sb_t;

/* ---- tiny libc replacements (this DLL links no C runtime) ---- */
static int d_strncmp(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
        if (!a[i]) {
            return 0;
        }
    }
    return 0;
}

static char *d_strchr(char *s, char c)
{
    for (; *s; s++) {
        if (*s == c) {
            return s;
        }
    }
    return NULL;
}

static char *d_strrchr(char *s, char c)
{
    char *r = NULL;
    for (; *s; s++) {
        if (*s == c) {
            r = s;
        }
    }
    return r;
}

static long d_atol(const char *s)
{
    long v = 0;
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s++ - '0');
    }
    return neg ? -v : v;
}

static void d_zero(void *p, size_t n)
{
    volatile unsigned char *b = (volatile unsigned char *)p;
    while (n--) {
        *b++ = 0;
    }
}

static void d_move(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
}

static void dbg_selftest(sb_t *o);



static void sb_str(sb_t *b, const char *s)
{
    while (*s && b->n < b->cap - 1) {
        b->p[b->n++] = *s++;
    }
    b->p[b->n] = 0;
}

static void sb_hex(sb_t *b, unsigned long long v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    char t[17];
    int i = 0;

    if (digits <= 0) {
        digits = 1;
        {
            unsigned long long w = v;
            while (w >> 4) {
                digits++;
                w >>= 4;
            }
        }
    }
    for (i = 0; i < digits; i++) {
        t[digits - 1 - i] = hx[(v >> (4 * i)) & 15];
    }
    t[digits] = 0;
    sb_str(b, t);
}

static void sb_dec(sb_t *b, long long v)
{
    char t[24];
    int i = 0, k;

    if (v < 0) {
        sb_str(b, "-");
        v = -v;
    }
    if (v == 0) {
        t[i++] = '0';
    }
    while (v > 0) {
        t[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (k = 0; k < i / 2; k++) {
        char c = t[k];
        t[k] = t[i - 1 - k];
        t[i - 1 - k] = c;
    }
    t[i] = 0;
    sb_str(b, t);
}

/* ------------------------------------------------------------------ event ring */

static char g_ev[DBG_RING][DBG_LINE];
static volatile LONG g_ev_seq;
static volatile LONG g_dbg_lock;

static void dbg_lock(void)
{
    while (InterlockedCompareExchange(&g_dbg_lock, 1, 0) != 0) {
        SwitchToThread();
    }
}

static void dbg_unlock(void)
{
    InterlockedExchange(&g_dbg_lock, 0);
}

static void ev_add(const char *line)
{
    int seq, i = 0;

    dbg_lock();
    seq = (int)InterlockedIncrement(&g_ev_seq);
    while (line[i] && i < DBG_LINE - 1) {
        g_ev[seq % DBG_RING][i] = line[i];
        i++;
    }
    g_ev[seq % DBG_RING][i] = 0;
    dbg_unlock();
    log_begin();
    log_raw("dbg: ");
    log_raw(g_ev[seq % DBG_RING]);
    log_raw("\n");
}

/* ------------------------------------------------------------------ expressions */

typedef struct {
    const char *s;
    CONTEXT *c;
    int err;
} ev_t;

static unsigned char *g_dbg_img;

static unsigned long long ev_expr(ev_t *e);

static void ev_ws(ev_t *e)
{
    while (*e->s == ' ') {
        e->s++;
    }
}

static unsigned long long ev_reg(ev_t *e, const char *name, int len)
{
    static const struct {
        const char *n;
        int off;
    } regs[] = {
        {"rax", 0}, {"rcx", 1}, {"rdx", 2}, {"rbx", 3}, {"rsp", 4}, {"rbp", 5}, {"rsi", 6}, {"rdi", 7},
        {"r8", 8},  {"r9", 9},  {"r10", 10}, {"r11", 11}, {"r12", 12}, {"r13", 13}, {"r14", 14}, {"r15", 15},
        {"rip", 16},
    };
    int i, k;

    if (len == 3 && name[0] == 'i' && name[1] == 'm' && name[2] == 'g') {
        return (unsigned long long)(ULONG_PTR)g_dbg_img;
    }
    for (i = 0; i < (int)(sizeof regs / sizeof regs[0]); i++) {
        const char *rn = regs[i].n;
        for (k = 0; k < len && rn[k] && rn[k] == name[k]; k++) {
        }
        if (k == len && rn[k] == 0) {
            if (!e->c) {
                return 0;
            }
            switch (regs[i].off) {
            case 0: return e->c->Rax;
            case 1: return e->c->Rcx;
            case 2: return e->c->Rdx;
            case 3: return e->c->Rbx;
            case 4: return e->c->Rsp;
            case 5: return e->c->Rbp;
            case 6: return e->c->Rsi;
            case 7: return e->c->Rdi;
            case 8: return e->c->R8;
            case 9: return e->c->R9;
            case 10: return e->c->R10;
            case 11: return e->c->R11;
            case 12: return e->c->R12;
            case 13: return e->c->R13;
            case 14: return e->c->R14;
            case 15: return e->c->R15;
            default: return e->c->Rip;
            }
        }
    }
    e->err = 1;
    return 0;
}

static unsigned long long ev_term(ev_t *e)
{
    unsigned long long v = 0;
    int size = 8;

    ev_ws(e);
    if ((e->s[0] == 'd' || e->s[0] == 'w' || e->s[0] == 'b') && e->s[1] == '[') {
        size = e->s[0] == 'd' ? 4 : (e->s[0] == 'w' ? 2 : 1);
        e->s++;
    }
    if (*e->s == '[') {
        unsigned long long a;
        e->s++;
        a = ev_expr(e);
        ev_ws(e);
        if (*e->s == ']') {
            e->s++;
        } else {
            e->err = 1;
        }
        if (e->err || a < 0x10000 || IsBadReadPtr((const void *)(ULONG_PTR)a, size)) {
            e->err = 1;
            return 0;
        }
        mem_copy(&v, (const void *)(ULONG_PTR)a, size);
        return v;
    }
    if (*e->s == '(') {
        e->s++;
        v = ev_expr(e);
        ev_ws(e);
        if (*e->s == ')') {
            e->s++;
        } else {
            e->err = 1;
        }
        return v;
    }
    if (e->s[0] == '0' && (e->s[1] == 'x' || e->s[1] == 'X')) {
        e->s += 2;
        while ((*e->s >= '0' && *e->s <= '9') || (*e->s >= 'a' && *e->s <= 'f') || (*e->s >= 'A' && *e->s <= 'F')) {
            char c = *e->s++;
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        return v;
    }
    if (*e->s >= '0' && *e->s <= '9') {
        while (*e->s >= '0' && *e->s <= '9') {
            v = v * 10 + (unsigned)(*e->s++ - '0');
        }
        return v;
    }
    if ((*e->s >= 'a' && *e->s <= 'z') || (*e->s >= 'A' && *e->s <= 'Z')) {
        const char *st = e->s;
        while ((*e->s >= 'a' && *e->s <= 'z') || (*e->s >= 'A' && *e->s <= 'Z') || (*e->s >= '0' && *e->s <= '9')) {
            e->s++;
        }
        return ev_reg(e, st, (int)(e->s - st));
    }
    e->err = 1;
    return 0;
}

static unsigned long long ev_expr(ev_t *e)
{
    unsigned long long v = ev_term(e);

    for (;;) {
        ev_ws(e);
        if (*e->s == '+') {
            e->s++;
            v += ev_term(e);
        } else if (*e->s == '-') {
            e->s++;
            v -= ev_term(e);
        } else if (*e->s == '&') {
            e->s++;
            v &= ev_term(e);
        } else {
            break;
        }
    }
    return v;
}

static int dbg_eval(const char *s, CONTEXT *c, unsigned long long *out)
{
    ev_t e;

    e.s = s;
    e.c = c;
    e.err = 0;
    *out = ev_expr(&e);
    ev_ws(&e);
    return !e.err && *e.s == 0;
}

/* address argument: expression; small values are RVAs in the game image */
static int dbg_addr(const char *s, unsigned long long *out)
{
    if (!dbg_eval(s, NULL, out)) {
        return 0;
    }
    if (*out < 0x4000000ull && g_dbg_img) {
        *out += (unsigned long long)(ULONG_PTR)g_dbg_img;
    }
    return 1;
}

/* ------------------------------------------------------------------ backtrace / reports */

static unsigned long long g_img_lo, g_img_hi;

static void dbg_img_range(void)
{
    if (!g_dbg_img) {
        g_dbg_img = find_fh2_image();
    }
    if (g_dbg_img) {
        g_img_lo = (unsigned long long)(ULONG_PTR)g_dbg_img + 0x1000;
        g_img_hi = (unsigned long long)(ULONG_PTR)g_dbg_img + 0x1db4000; /* .text */
    }
}

static int dbg_is_retaddr(unsigned long long v)
{
    const unsigned char *p;

    if (v < g_img_lo + 8 || v >= g_img_hi || IsBadReadPtr((const void *)(ULONG_PTR)(v - 7), 7)) {
        return 0;
    }
    p = (const unsigned char *)(ULONG_PTR)v;
    return p[-5] == 0xE8 || (p[-6] == 0xFF && p[-5] == 0x15) || (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) ||
           (p[-3] == 0xFF && (p[-2] & 0xF8) == 0xD0) || (p[-3] == 0xFF && p[-2] >= 0x50 && p[-2] <= 0x57) ||
           (p[-2] == 0xFF && p[-1] >= 0x10 && p[-1] <= 0x17);
}

/* appends "  bt +RVA ..." for plausible return addresses on the stack of context c */
static void dbg_bt(sb_t *o, CONTEXT *c, int max)
{
    unsigned long long sp = c->Rsp, v;
    int off, found = 0;

    sb_str(o, "  bt");
    for (off = 0; off < 0x800 && found < max; off += 8) {
        if (IsBadReadPtr((const void *)(ULONG_PTR)(sp + off), 8)) {
            break;
        }
        v = *(const unsigned long long *)(ULONG_PTR)(sp + off);
        if (dbg_is_retaddr(v)) {
            sb_str(o, " +0x");
            sb_hex(o, v - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
            found++;
        }
    }
}

static void dbg_hexbytes(sb_t *o, unsigned long long a, int n)
{
    int i;

    if (a < 0x10000 || IsBadReadPtr((const void *)(ULONG_PTR)a, n)) {
        sb_str(o, "<unreadable>");
        return;
    }
    for (i = 0; i < n; i++) {
        sb_hex(o, ((const unsigned char *)(ULONG_PTR)a)[i], 2);
    }
}

static void dbg_regs_line(sb_t *o, CONTEXT *c)
{
    sb_str(o, "rax="); sb_hex(o, c->Rax, 0);
    sb_str(o, " rcx="); sb_hex(o, c->Rcx, 0);
    sb_str(o, " rdx="); sb_hex(o, c->Rdx, 0);
    sb_str(o, " rbx="); sb_hex(o, c->Rbx, 0);
    sb_str(o, " rsp="); sb_hex(o, c->Rsp, 0);
    sb_str(o, " rbp="); sb_hex(o, c->Rbp, 0);
    sb_str(o, " rsi="); sb_hex(o, c->Rsi, 0);
    sb_str(o, " rdi="); sb_hex(o, c->Rdi, 0);
    sb_str(o, " r8="); sb_hex(o, c->R8, 0);
    sb_str(o, " r9="); sb_hex(o, c->R9, 0);
    sb_str(o, " r10="); sb_hex(o, c->R10, 0);
    sb_str(o, " r11="); sb_hex(o, c->R11, 0);
    sb_str(o, " r12="); sb_hex(o, c->R12, 0);
    sb_str(o, " r13="); sb_hex(o, c->R13, 0);
    sb_str(o, " r14="); sb_hex(o, c->R14, 0);
    sb_str(o, " r15="); sb_hex(o, c->R15, 0);
}

static volatile LONG g_crash_n;

static void dbg_crash_report(EXCEPTION_POINTERS *ep)
{
    static unsigned long long seen[16];
    static int nseen;
    CONTEXT *c = ep->ContextRecord;
    char buf[3000];
    sb_t o;
    int i, n;
    unsigned long long rip = c->Rip;
    const unsigned long long regv[] = {c->Rax, c->Rcx, c->Rdx, c->Rbx, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9, c->R10,
                                      c->R11, c->R12, c->R13, c->R14, c->R15};
    const char *regn[] = {"rax", "rcx", "rdx", "rbx", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13",
                          "r14", "r15"};

    for (i = 0; i < nseen; i++) {
        if (seen[i] == rip) {
            return;
        }
    }
    if (nseen < 16) {
        seen[nseen++] = rip;
    }
    dbg_img_range();
    o.p = buf;
    o.n = 0;
    o.cap = sizeof buf;
    buf[0] = 0;
    sb_str(&o, "CRASH code=0x");
    sb_hex(&o, ep->ExceptionRecord->ExceptionCode, 8);
    sb_str(&o, " tid=");
    sb_dec(&o, GetCurrentThreadId());
    sb_str(&o, " rip=");
    if (rip >= (unsigned long long)(ULONG_PTR)g_dbg_img && rip < g_img_hi) {
        sb_str(&o, "+0x");
        sb_hex(&o, rip - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
    } else {
        sb_str(&o, "0x");
        sb_hex(&o, rip, 0);
    }
    if (ep->ExceptionRecord->NumberParameters >= 2) {
        sb_str(&o, ep->ExceptionRecord->ExceptionInformation[0] ? " writing 0x" : " reading 0x");
        sb_hex(&o, ep->ExceptionRecord->ExceptionInformation[1], 0);
    }
    sb_str(&o, "\n  ");
    dbg_regs_line(&o, c);
    sb_str(&o, "\n");
    for (i = 0; i < 15; i++) {
        unsigned long long v = regv[i];
        if (v > 0x10000 && v < 0x7fffffffffffull && !IsBadReadPtr((const void *)(ULONG_PTR)v, 32)) {
            sb_str(&o, "  ");
            sb_str(&o, regn[i]);
            sb_str(&o, " -> ");
            dbg_hexbytes(&o, v, 32);
            sb_str(&o, "\n");
        }
    }
    dbg_bt(&o, c, 24);
    sb_str(&o, "\n");

    n = (int)InterlockedIncrement(&g_crash_n);
    {
        wchar_t path[MAX_PATH];
        wchar_t num[8];
        HANDLE f;
        DWORD w;
        int k = 0, v = n;
        char tmp[4];

        lstrcpyW(path, g_dir);
        lstrcatW(path, L"\\fh2mp-crash-");
        if (v >= 10) {
            num[k++] = (wchar_t)(L'0' + v / 10);
        }
        num[k++] = (wchar_t)(L'0' + v % 10);
        num[k] = 0;
        lstrcatW(path, num);
        lstrcatW(path, L".txt");
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, o.p, (DWORD)o.n, &w, NULL);
            CloseHandle(f);
        }
        (void)tmp;
    }
    /* the ring holds one event per line */
    {
        char *ln = o.p;
        char *q;
        for (q = o.p; ; q++) {
            if (*q == '\n' || *q == 0) {
                char sv = *q;
                *q = 0;
                if (ln[0]) {
                    ev_add(ln);
                }
                if (!sv) {
                    break;
                }
                ln = q + 1;
            }
        }
    }
}

/* ------------------------------------------------------------------ breakpoints */

typedef struct {
    int used, enabled, armed, is_ret, owner;
    unsigned char *addr;
    unsigned char orig;
    char name[32];
    long hits, limit;
    int once, bt, want_ret;
    char logs[6][80];
    int nlogs;
    char mem[3][80];
    int memlen[3];
    int nmem;
    char cond[96];
    DWORD ret_tid;
} dbg_bp;

static dbg_bp g_dbp[DBG_MAX_BP];
static struct {
    DWORD tid;
    int bp;
} g_pend[32];

static int dbg_arm(dbg_bp *b)
{
    DWORD old;

    if (b->armed || IsBadReadPtr(b->addr, 1)) {
        return b->armed;
    }
    if (!VirtualProtect(b->addr, 1, PAGE_EXECUTE_READWRITE, &old)) {
        return 0;
    }
    b->orig = *b->addr;
    *(volatile unsigned char *)b->addr = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), b->addr, 1);
    b->armed = 1;
    return 1;
}

static void dbg_disarm(dbg_bp *b)
{
    if (b->armed) {
        *(volatile unsigned char *)b->addr = b->orig;
        FlushInstructionCache(GetCurrentProcess(), b->addr, 1);
        b->armed = 0;
    }
}

static int dbg_cond_ok(const char *cond, CONTEXT *c)
{
    char l[96];
    int i, ne = 0, pos = -1;
    unsigned long long a = 0, b = 0;

    if (!cond[0]) {
        return 1;
    }
    for (i = 0; cond[i]; i++) {
        if ((cond[i] == '=' || cond[i] == '!') && cond[i + 1] == '=') {
            pos = i;
            ne = cond[i] == '!';
            break;
        }
    }
    if (pos < 0) {
        return dbg_eval(cond, c, &a) && a != 0;
    }
    for (i = 0; i < pos && i < 95; i++) {
        l[i] = cond[i];
    }
    l[i] = 0;
    if (!dbg_eval(l, c, &a) || !dbg_eval(cond + pos + 2, c, &b)) {
        return 1;
    }
    return ne ? a != b : a == b;
}

static void dbg_bp_hit(int idx, EXCEPTION_POINTERS *ep)
{
    dbg_bp *b = &g_dbp[idx];
    CONTEXT *c = ep->ContextRecord;
    char buf[1800];
    sb_t o;
    int i;

    if (!dbg_cond_ok(b->cond, c)) {
        return;
    }
    b->hits++;
    o.p = buf;
    o.n = 0;
    o.cap = sizeof buf;
    buf[0] = 0;
    if (b->is_ret) {
        sb_str(&o, "ret ");
        sb_str(&o, g_dbp[b->owner].name);
        sb_str(&o, " tid=");
        sb_dec(&o, GetCurrentThreadId());
        sb_str(&o, " rax=0x");
        sb_hex(&o, c->Rax, 0);
        if (g_dbp[b->owner].nmem > 0) { /* mem= on a ret breakpoint dumps memory at the returned pointer */
            sb_str(&o, " mem[rax]=");
            dbg_hexbytes(&o, c->Rax, g_dbp[b->owner].memlen[0]);
        }
        ev_add(o.p);
        b->enabled = 0;
        return;
    }
    sb_str(&o, "bp ");
    sb_str(&o, b->name);
    sb_str(&o, " #");
    sb_dec(&o, b->hits);
    sb_str(&o, " tid=");
    sb_dec(&o, GetCurrentThreadId());
    for (i = 0; i < b->nlogs; i++) {
        unsigned long long v = 0;
        sb_str(&o, " ");
        sb_str(&o, b->logs[i]);
        sb_str(&o, "=");
        if (dbg_eval(b->logs[i], c, &v)) {
            sb_str(&o, "0x");
            sb_hex(&o, v, 0);
        } else {
            sb_str(&o, "?");
        }
    }
    for (i = 0; i < b->nmem; i++) {
        unsigned long long a = 0;
        sb_str(&o, " mem[");
        sb_str(&o, b->mem[i]);
        sb_str(&o, "]=");
        if (dbg_eval(b->mem[i], c, &a)) {
            dbg_hexbytes(&o, a, b->memlen[i]);
        } else {
            sb_str(&o, "?");
        }
    }
    if (b->bt) {
        sb_str(&o, "\n");
        dbg_bt(&o, c, b->bt);
    }
    /* one event per line */
    {
        char *ln = o.p;
        char *q;
        for (q = o.p; ; q++) {
            if (*q == '\n' || *q == 0) {
                char sv = *q;
                *q = 0;
                if (ln[0]) {
                    ev_add(ln);
                }
                if (!sv) {
                    break;
                }
                ln = q + 1;
            }
        }
    }
    if (b->want_ret && c->Rsp > 0x10000 && !IsBadReadPtr((const void *)(ULONG_PTR)c->Rsp, 8)) {
        unsigned long long ra = *(const unsigned long long *)(ULONG_PTR)c->Rsp;
        for (i = 0; i < DBG_MAX_BP; i++) {
            if (!g_dbp[i].used) {
                dbg_bp *r = &g_dbp[i];
                r->used = 1;
                r->enabled = 1;
                r->is_ret = 1;
                r->owner = idx;
                r->addr = (unsigned char *)(ULONG_PTR)ra;
                r->ret_tid = GetCurrentThreadId();
                dbg_arm(r);
                break;
            }
        }
    }
    if ((b->once && b->hits >= 1) || (b->limit && b->hits >= b->limit)) {
        b->enabled = 0;
    }
}

/* ------------------------------------------------------------------ hardware watchpoints */

typedef struct {
    int used;
    unsigned long long addr;
    int len, rw, bt, pg;
    char name[32];
    long hits;
} dbg_watch;

static dbg_watch g_dw[DBG_MAX_WATCH];
static DWORD g_hw_done[512];
static int g_hw_ndone;

static unsigned long long dbg_dr7(void)
{
    unsigned long long d = 0;
    int i;

    for (i = 0; i < 4; i++) {
        if (g_dw[i].used && !g_dw[i].pg) {
            unsigned long long lenbits = g_dw[i].len == 1 ? 0 : (g_dw[i].len == 2 ? 1 : (g_dw[i].len == 8 ? 2 : 3));
            d |= (1ull << (i * 2)) | ((unsigned long long)(g_dw[i].rw ? 3 : 1) << (16 + i * 4)) |
                 (lenbits << (18 + i * 4));
        }
    }
    return d;
}

static void dbg_hw_apply_thread(DWORD tid)
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, tid);
    CONTEXT c;
    int i;

    if (!h) {
        return;
    }
    if (SuspendThread(h) != (DWORD)-1) {
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(h, &c)) {
            c.Dr0 = (g_dw[0].used && !g_dw[0].pg) ? g_dw[0].addr : 0;
            c.Dr1 = (g_dw[1].used && !g_dw[1].pg) ? g_dw[1].addr : 0;
            c.Dr2 = (g_dw[2].used && !g_dw[2].pg) ? g_dw[2].addr : 0;
            c.Dr3 = (g_dw[3].used && !g_dw[3].pg) ? g_dw[3].addr : 0;
            c.Dr6 = 0;
            c.Dr7 = dbg_dr7();
            SetThreadContext(h, &c);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    for (i = 0; i < g_hw_ndone; i++) {
        if (g_hw_done[i] == tid) {
            return;
        }
    }
    if (g_hw_ndone < 512) {
        g_hw_done[g_hw_ndone++] = tid;
    }
}

/* applies the current watch set to every thread (all = re-apply to threads already done too) */
static void dbg_hw_apply(int all)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();

    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    if (all) {
        g_hw_ndone = 0;
    }
    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid && te.th32ThreadID != me) {
                int i, done = 0;
                for (i = 0; i < g_hw_ndone; i++) {
                    if (g_hw_done[i] == te.th32ThreadID) {
                        done = 1;
                    }
                }
                if (!done) {
                    dbg_hw_apply_thread(te.th32ThreadID);
                }
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

static DWORD WINAPI dbg_hw_thread(LPVOID unused)
{
    (void)unused;
    for (;;) {
        Sleep(1000);
        if (dbg_dr7()) {
            dbg_hw_apply(0); /* picks up threads created since the last pass */
        }
    }
    return 0;
}

static void dbg_watch_hit(int slot, EXCEPTION_POINTERS *ep)
{
    dbg_watch *w = &g_dw[slot];
    CONTEXT *c = ep->ContextRecord;
    char buf[1400];
    sb_t o;
    unsigned long long v = 0;

    w->hits++;
    o.p = buf;
    o.n = 0;
    o.cap = sizeof buf;
    buf[0] = 0;
    sb_str(&o, "watch ");
    sb_str(&o, w->name);
    sb_str(&o, " #");
    sb_dec(&o, w->hits);
    sb_str(&o, " tid=");
    sb_dec(&o, GetCurrentThreadId());
    sb_str(&o, " rip=+0x");
    sb_hex(&o, c->Rip - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
    if (!IsBadReadPtr((const void *)(ULONG_PTR)w->addr, w->len)) {
        mem_copy(&v, (const void *)(ULONG_PTR)w->addr, w->len);
    }
    sb_str(&o, " new=0x");
    sb_hex(&o, v, 0);
    ev_add(o.p);
    if (w->bt) {
        o.n = 0;
        o.p[0] = 0;
        dbg_bt(&o, c, w->bt);
        ev_add(o.p);
    }
}

/* ---- page-guard watchpoints (work where debug registers are not delivered, e.g. Wine/Proton) ---- */

static struct {
    DWORD tid;
    int hit;
    unsigned long long addr;
    int len;
} g_pgpend[32];

static void dbg_pg_arm_page(unsigned long long addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;

    if (VirtualQuery((const void *)(ULONG_PTR)addr, &mbi, sizeof mbi) && !(mbi.Protect & PAGE_GUARD) &&
        mbi.State == MEM_COMMIT) {
        VirtualProtect((void *)(ULONG_PTR)(addr & ~0xFFFull), 1, mbi.Protect | PAGE_GUARD, &old);
    }
}

static void dbg_pg_arm_all(void)
{
    int i;

    for (i = 0; i < DBG_MAX_WATCH; i++) {
        if (g_dw[i].used && g_dw[i].pg) {
            dbg_pg_arm_page(g_dw[i].addr);
        }
    }
}

static void dbg_pg_disarm_page(unsigned long long addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    DWORD old;

    if (VirtualQuery((const void *)(ULONG_PTR)addr, &mbi, sizeof mbi) && (mbi.Protect & PAGE_GUARD)) {
        VirtualProtect((void *)(ULONG_PTR)(addr & ~0xFFFull), 1, mbi.Protect & ~(DWORD)PAGE_GUARD, &old);
    }
}

/* a guarded page was touched: log if it is one of our ranges, then single-step and re-guard */
static int dbg_pg_violation(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    unsigned long long a = ep->ExceptionRecord->NumberParameters >= 2 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    int is_write = ep->ExceptionRecord->NumberParameters >= 1 && ep->ExceptionRecord->ExceptionInformation[0] == 1;
    int i, on_page = 0, hit = -1;

    for (i = 0; i < DBG_MAX_WATCH; i++) {
        if (g_dw[i].used && g_dw[i].pg) {
            if ((a >> 12) == (g_dw[i].addr >> 12)) {
                on_page = 1;
                if (a + 8 > g_dw[i].addr && a < g_dw[i].addr + (unsigned)g_dw[i].len && (is_write || g_dw[i].rw)) {
                    hit = i;
                }
            }
        }
    }
    if (!on_page) {
        return 0;
    }
    if (hit >= 0) {
        dbg_watch *w = &g_dw[hit];
        char buf[1400];
        sb_t o;
        unsigned long long v = 0;

        w->hits++;
        o.p = buf;
        o.n = 0;
        o.cap = sizeof buf;
        buf[0] = 0;
        sb_str(&o, "watch ");
        sb_str(&o, w->name);
        sb_str(&o, " #");
        sb_dec(&o, w->hits);
        sb_str(&o, is_write ? " WRITE" : " READ");
        sb_str(&o, " tid=");
        sb_dec(&o, GetCurrentThreadId());
        sb_str(&o, " rip=+0x");
        sb_hex(&o, c->Rip - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
        if (!IsBadReadPtr((const void *)(ULONG_PTR)w->addr, w->len)) {
            mem_copy(&v, (const void *)(ULONG_PTR)w->addr, w->len);
        }
        sb_str(&o, " old=0x");
        sb_hex(&o, v, 0);
        ev_add(o.p);
        if (w->bt) {
            o.n = 0;
            o.p[0] = 0;
            dbg_bt(&o, c, w->bt);
            ev_add(o.p);
        }
    }
    for (i = 0; i < 32; i++) {
        if (g_pgpend[i].tid == 0 || g_pgpend[i].tid == GetCurrentThreadId()) {
            g_pgpend[i].tid = GetCurrentThreadId();
            g_pgpend[i].hit = hit;
            g_pgpend[i].addr = hit >= 0 ? g_dw[hit].addr : 0;
            g_pgpend[i].len = hit >= 0 ? g_dw[hit].len : 0;
            break;
        }
    }
    c->EFlags |= 0x100u;
    return 1;
}

static int dbg_pg_step(EXCEPTION_POINTERS *ep)
{
    DWORD me = GetCurrentThreadId();
    int i;

    for (i = 0; i < 32; i++) {
        if (g_pgpend[i].tid == me) {
            if (g_pgpend[i].hit >= 0 && g_pgpend[i].len > 0 && !IsBadReadPtr((const void *)(ULONG_PTR)g_pgpend[i].addr, g_pgpend[i].len)) {
                unsigned long long v = 0;
                char buf[128];
                sb_t o;
                mem_copy(&v, (const void *)(ULONG_PTR)g_pgpend[i].addr, g_pgpend[i].len);
                o.p = buf;
                o.n = 0;
                o.cap = sizeof buf;
                buf[0] = 0;
                sb_str(&o, "watch ");
                sb_str(&o, g_dw[g_pgpend[i].hit].name);
                sb_str(&o, "   -> new=0x");
                sb_hex(&o, v, 0);
                ev_add(o.p);
            }
            g_pgpend[i].tid = 0;
            ep->ContextRecord->EFlags &= ~0x100u;
            dbg_pg_arm_all();
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ exception entry */

static void dbg_pend_set(int bp)
{
    int i;
    DWORD me = GetCurrentThreadId();

    for (i = 0; i < 32; i++) {
        if (g_pend[i].tid == 0 || g_pend[i].tid == me) {
            g_pend[i].tid = me;
            g_pend[i].bp = bp;
            return;
        }
    }
}

/* returns 1 when the exception was ours and execution should continue */
static int dbg_handle(EXCEPTION_POINTERS *ep)
{
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT *c = ep->ContextRecord;
    int i;

    if (code == EXCEPTION_BREAKPOINT) {
        for (i = 0; i < DBG_MAX_BP; i++) {
            dbg_bp *b = &g_dbp[i];
            if (b->used && b->armed && b->addr == (unsigned char *)ep->ExceptionRecord->ExceptionAddress) {
                if (b->enabled && (!b->is_ret || b->ret_tid == GetCurrentThreadId())) {
                    dbg_bp_hit(i, ep);
                }
                /* step over the original instruction, then re-arm */
                *(volatile unsigned char *)b->addr = b->orig;
                FlushInstructionCache(GetCurrentProcess(), b->addr, 1);
                c->Rip = (DWORD64)(ULONG_PTR)b->addr;
                if (b->is_ret) {
                    b->armed = 0;
                    b->used = 0;
                    return 1;
                }
                dbg_pend_set(i);
                c->EFlags |= 0x100u;
                return 1;
            }
        }
        return 0;
    }
    if (code == 0x80000001u) { /* STATUS_GUARD_PAGE_VIOLATION */
        return dbg_pg_violation(ep);
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        DWORD me = GetCurrentThreadId();
        int handled = dbg_pg_step(ep);

        for (i = 0; i < 32; i++) {
            if (g_pend[i].tid == me) {
                dbg_bp *b = &g_dbp[g_pend[i].bp];
                g_pend[i].tid = 0;
                if (b->used && b->enabled) {
                    *(volatile unsigned char *)b->addr = 0xCC;
                    FlushInstructionCache(GetCurrentProcess(), b->addr, 1);
                } else if (b->used) {
                    b->armed = 0;
                }
                c->EFlags &= ~0x100u;
                handled = 1;
                break;
            }
        }
        if (c->Dr6 & 0xF) {
            int s;
            for (s = 0; s < 4; s++) {
                if ((c->Dr6 & (1ull << s)) && g_dw[s].used && !g_dw[s].pg) {
                    dbg_watch_hit(s, ep);
                }
            }
            c->Dr6 = 0;
            handled = 1;
        }
        return handled;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION) {
        dbg_crash_report(ep);
    }
    return 0;
}

/* ------------------------------------------------------------------ command server */

static int dbg_next_tok(char **p, char *out, int cap)
{
    int n = 0;

    while (**p == ' ') {
        (*p)++;
    }
    if (!**p) {
        return 0;
    }
    while (**p && **p != ' ' && n < cap - 1) {
        out[n++] = *(*p)++;
    }
    out[n] = 0;
    return 1;
}

static void dbg_bp_list(sb_t *o)
{
    int i;

    for (i = 0; i < DBG_MAX_BP; i++) {
        dbg_bp *b = &g_dbp[i];
        if (b->used && !b->is_ret) {
            sb_str(o, "bp ");
            sb_dec(o, i);
            sb_str(o, b->enabled ? " on  +0x" : " off +0x");
            sb_hex(o, (unsigned long long)(ULONG_PTR)b->addr - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
            sb_str(o, " ");
            sb_str(o, b->name);
            sb_str(o, " hits=");
            sb_dec(o, b->hits);
            sb_str(o, b->armed ? "" : " (not armed)");
            sb_str(o, "\n");
        }
    }
}

static void dbg_cmd(char *line, sb_t *o)
{
    char cmd[16], a[200], t[200];
    char *p = line;

    if (!dbg_next_tok(&p, cmd, sizeof cmd)) {
        return;
    }
    dbg_img_range();
    if (!lstrcmpA(cmd, "help")) {
        sb_str(o, "rd wr bp bl bd be watch wl wd thr ev info selftest  (see shim/src/dbg.inc)\n");
    } else if (!lstrcmpA(cmd, "info")) {
        sb_str(o, "img=0x");
        sb_hex(o, (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
        sb_str(o, " pid=");
        sb_dec(o, GetCurrentProcessId());
        sb_str(o, " events=");
        sb_dec(o, g_ev_seq);
        sb_str(o, "\n");
    } else if (!lstrcmpA(cmd, "rd")) {
        unsigned long long addr;
        char n[40];
        int len = 64, i;

        if (!dbg_next_tok(&p, a, sizeof a) || !dbg_addr(a, &addr)) {
            sb_str(o, "error: rd <addr> [len]\n");
            return;
        }
        if (dbg_next_tok(&p, n, sizeof n)) {
            unsigned long long l;
            if (dbg_eval(n, NULL, &l)) {
                len = (int)(l > 4096 ? 4096 : l);
            }
        }
        for (i = 0; i < len; i += 16) {
            int k, m = len - i < 16 ? len - i : 16;
            sb_hex(o, addr + i, 0);
            sb_str(o, ": ");
            if (IsBadReadPtr((const void *)(ULONG_PTR)(addr + i), m)) {
                sb_str(o, "<unreadable>\n");
                break;
            }
            for (k = 0; k < m; k++) {
                sb_hex(o, ((const unsigned char *)(ULONG_PTR)(addr + i))[k], 2);
                sb_str(o, " ");
            }
            sb_str(o, "\n");
        }
    } else if (!lstrcmpA(cmd, "wr")) {
        unsigned long long addr;
        int i, n = 0;
        unsigned char bytes[256];
        DWORD old;

        if (!dbg_next_tok(&p, a, sizeof a) || !dbg_addr(a, &addr) || !dbg_next_tok(&p, t, sizeof t)) {
            sb_str(o, "error: wr <addr> <hexbytes>\n");
            return;
        }
        for (i = 0; t[i] && t[i + 1] && n < 256; i += 2) {
            unsigned v = 0;
            int k;
            for (k = 0; k < 2; k++) {
                char ch = t[i + k];
                v = v * 16 + (unsigned)(ch <= '9' ? ch - '0' : (ch | 32) - 'a' + 10);
            }
            bytes[n++] = (unsigned char)v;
        }
        if (VirtualProtect((void *)(ULONG_PTR)addr, (SIZE_T)n, PAGE_EXECUTE_READWRITE, &old)) {
            mem_copy((void *)(ULONG_PTR)addr, bytes, (SIZE_T)n);
            FlushInstructionCache(GetCurrentProcess(), (void *)(ULONG_PTR)addr, (SIZE_T)n);
            sb_str(o, "ok\n");
        } else {
            sb_str(o, "error: cannot write\n");
        }
    } else if (!lstrcmpA(cmd, "bp")) {
        unsigned long long addr;
        int idx;
        dbg_bp *b;

        if (!dbg_next_tok(&p, a, sizeof a) || !dbg_addr(a, &addr)) {
            sb_str(o, "error: bp <addr> [opts]\n");
            return;
        }
        for (idx = 0; idx < DBG_MAX_BP && g_dbp[idx].used; idx++) {
        }
        if (idx == DBG_MAX_BP) {
            sb_str(o, "error: breakpoint table full\n");
            return;
        }
        b = &g_dbp[idx];
        d_zero(b, sizeof *b);
        b->addr = (unsigned char *)(ULONG_PTR)addr;
        b->enabled = 1;
        lstrcpyA(b->name, "bp");
        while (dbg_next_tok(&p, t, sizeof t)) {
            if (!d_strncmp(t, "name=", 5)) {
                lstrcpynA(b->name, t + 5, 32);
            } else if (!d_strncmp(t, "log=", 4)) {
                char *q = t + 4;
                while (*q && b->nlogs < 6) {
                    int n = 0;
                    while (*q && *q != ',' && n < 79) {
                        b->logs[b->nlogs][n++] = *q++;
                    }
                    b->logs[b->nlogs][n] = 0;
                    b->nlogs++;
                    if (*q == ',') {
                        q++;
                    }
                }
            } else if (!d_strncmp(t, "mem=", 4) && b->nmem < 3) {
                char *colon = d_strrchr(t + 4, ':');
                int len = 32;
                if (colon) {
                    unsigned long long l;
                    if (dbg_eval(colon + 1, NULL, &l)) {
                        len = (int)(l > 256 ? 256 : l);
                    }
                    *colon = 0;
                }
                lstrcpynA(b->mem[b->nmem], t + 4, 80);
                b->memlen[b->nmem++] = len;
            } else if (!d_strncmp(t, "bt", 2)) {
                b->bt = t[2] == '=' ? d_atol(t + 3) : 8;
            } else if (!lstrcmpA(t, "ret")) {
                b->want_ret = 1;
            } else if (!lstrcmpA(t, "once")) {
                b->once = 1;
            } else if (!d_strncmp(t, "count=", 6)) {
                b->limit = d_atol(t + 6);
            } else if (!d_strncmp(t, "cond=", 5)) {
                lstrcpynA(b->cond, t + 5, 96);
            } else if (!lstrcmpA(t, "off")) {
                b->enabled = 0;
            }
        }
        b->used = 1;
        if (b->enabled && !dbg_arm(b)) {
            b->used = 0;
            sb_str(o, "error: cannot arm (address not mapped?)\n");
            return;
        }
        if (!b->enabled) {
            dbg_arm(b);
        }
        sb_str(o, "bp ");
        sb_dec(o, idx);
        sb_str(o, " set at +0x");
        sb_hex(o, addr - (unsigned long long)(ULONG_PTR)g_dbg_img, 0);
        sb_str(o, "\n");
    } else if (!lstrcmpA(cmd, "bl")) {
        dbg_bp_list(o);
    } else if (!lstrcmpA(cmd, "bd") || !lstrcmpA(cmd, "be")) {
        int i;
        int del = cmd[1] == 'd';
        if (!dbg_next_tok(&p, a, sizeof a)) {
            sb_str(o, "error: bd|be <id|all>\n");
            return;
        }
        for (i = 0; i < DBG_MAX_BP; i++) {
            dbg_bp *b = &g_dbp[i];
            if (b->used && (!lstrcmpA(a, "all") || d_atol(a) == i)) {
                if (del) {
                    b->enabled = 0;
                    /* leave a pending re-arm to the single-step handler, which checks `used` */
                    dbg_disarm(b);
                    b->used = 0;
                } else {
                    b->enabled = 1;
                    dbg_arm(b);
                }
            }
        }
        sb_str(o, "ok\n");
    } else if (!lstrcmpA(cmd, "watch")) {
        unsigned long long addr;
        int slot, len, rw = 0, bt = 0, pg = 1;
        char nm[32];

        if (!dbg_next_tok(&p, a, sizeof a) || !dbg_addr(a, &addr) || !dbg_next_tok(&p, t, sizeof t)) {
            sb_str(o, "error: watch <addr> <1|2|4|8> [w|rw] [name=N] [bt[=N]]\n");
            return;
        }
        len = d_atol(t);
        lstrcpyA(nm, "watch");
        while (dbg_next_tok(&p, t, sizeof t)) {
            if (!lstrcmpA(t, "rw")) {
                rw = 1;
            } else if (!d_strncmp(t, "name=", 5)) {
                lstrcpynA(nm, t + 5, 32);
            } else if (!d_strncmp(t, "bt", 2)) {
                bt = t[2] == '=' ? d_atol(t + 3) : 10;
            } else if (!lstrcmpA(t, "hw")) {
                pg = 0;
            } else if (!lstrcmpA(t, "pg")) {
                pg = 1;
            }
        }
        if ((len != 1 && len != 2 && len != 4 && len != 8) || (addr & (unsigned)(len - 1))) {
            sb_str(o, "error: length must be 1/2/4/8 and the address aligned to it\n");
            return;
        }
        for (slot = pg ? 4 : 0; slot < (pg ? DBG_MAX_WATCH : 4) && g_dw[slot].used; slot++) {
        }
        if (slot == (pg ? DBG_MAX_WATCH : 4)) {
            sb_str(o, "error: no free watchpoint slot\n");
            return;
        }
        g_dw[slot].pg = pg;
        g_dw[slot].used = 1;
        g_dw[slot].addr = addr;
        g_dw[slot].len = len;
        g_dw[slot].rw = rw;
        g_dw[slot].bt = bt;
        g_dw[slot].hits = 0;
        lstrcpyA(g_dw[slot].name, nm);
        if (pg) {
            dbg_pg_arm_page(addr);
        } else {
            dbg_hw_apply(1);
        }
        sb_str(o, pg ? "page-guard watch " : "hardware watch ");
        sb_dec(o, slot);
        sb_str(o, " set at 0x");
        sb_hex(o, addr, 0);
        sb_str(o, "\n");
    } else if (!lstrcmpA(cmd, "wl")) {
        int i;
        for (i = 0; i < DBG_MAX_WATCH; i++) {
            if (g_dw[i].used) {
                sb_str(o, "watch ");
                sb_dec(o, i);
                sb_str(o, " 0x");
                sb_hex(o, g_dw[i].addr, 0);
                sb_str(o, g_dw[i].pg ? " pg len=" : " hw len=");
                sb_dec(o, g_dw[i].len);
                sb_str(o, " ");
                sb_str(o, g_dw[i].name);
                sb_str(o, " hits=");
                sb_dec(o, g_dw[i].hits);
                sb_str(o, "\n");
            }
        }
    } else if (!lstrcmpA(cmd, "wd")) {
        int i;
        if (!dbg_next_tok(&p, a, sizeof a)) {
            sb_str(o, "error: wd <id|all>\n");
            return;
        }
        for (i = 0; i < DBG_MAX_WATCH; i++) {
            if (g_dw[i].used && (!lstrcmpA(a, "all") || d_atol(a) == i)) {
                if (g_dw[i].pg) {
                    dbg_pg_disarm_page(g_dw[i].addr);
                }
                g_dw[i].used = 0;
            }
        }
        dbg_hw_apply(1);
        dbg_pg_arm_all();
        sb_str(o, "ok\n");
    } else if (!lstrcmpA(cmd, "thr")) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        if (snap != INVALID_HANDLE_VALUE) {
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == GetCurrentProcessId()) {
                        sb_dec(o, te.th32ThreadID);
                        sb_str(o, " ");
                    }
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }
        sb_str(o, "\n");
    } else if (!lstrcmpA(cmd, "ev")) {
        long since = 0;
        long last = (long)g_ev_seq, s;
        if (dbg_next_tok(&p, a, sizeof a)) {
            since = d_atol(a);
        }
        if (last - since > DBG_RING - 8) {
            since = last - (DBG_RING - 8);
        }
        for (s = since + 1; s <= last; s++) {
            sb_dec(o, s);
            sb_str(o, " ");
            sb_str(o, g_ev[s % DBG_RING]);
            sb_str(o, "\n");
        }
        sb_str(o, "# ");
        sb_dec(o, last);
        sb_str(o, "\n");
    } else if (!lstrcmpA(cmd, "selftest")) {
        sb_str(o, "selftest: see events (bp on a probe function + hw watch on a variable)\n");
        dbg_selftest(o);
    } else {
        sb_str(o, "error: unknown command (help)\n");
    }
}

typedef int (WSAAPI *ws_startup_fn)(WORD, LPWSADATA);
typedef SOCKET (WSAAPI *ws_socket_fn)(int, int, int);
typedef int (WSAAPI *ws_bind_fn)(SOCKET, const struct sockaddr *, int);
typedef int (WSAAPI *ws_listen_fn)(SOCKET, int);
typedef SOCKET (WSAAPI *ws_accept_fn)(SOCKET, struct sockaddr *, int *);
typedef int (WSAAPI *ws_recv_fn)(SOCKET, char *, int, int);
typedef int (WSAAPI *ws_send_fn)(SOCKET, const char *, int, int);
typedef int (WSAAPI *ws_close_fn)(SOCKET);
typedef int (WSAAPI *ws_sockopt_fn)(SOCKET, int, int, const char *, int);

static char g_dbg_out[DBG_OUT];

static DWORD WINAPI dbg_server(LPVOID unused)
{
    HMODULE ws = LoadLibraryW(L"ws2_32.dll");
    ws_startup_fn f_startup;
    ws_socket_fn f_socket;
    ws_bind_fn f_bind;
    ws_listen_fn f_listen;
    ws_accept_fn f_accept;
    ws_recv_fn f_recv;
    ws_send_fn f_send;
    ws_close_fn f_close;
    ws_sockopt_fn f_opt;
    WSADATA wd;
    SOCKET ls;
    int port;
    struct sockaddr_in sa;

    (void)unused;
    if (!ws) {
        return 0;
    }
    f_startup = (ws_startup_fn)(void *)GetProcAddress(ws, "WSAStartup");
    f_socket = (ws_socket_fn)(void *)GetProcAddress(ws, "socket");
    f_bind = (ws_bind_fn)(void *)GetProcAddress(ws, "bind");
    f_listen = (ws_listen_fn)(void *)GetProcAddress(ws, "listen");
    f_accept = (ws_accept_fn)(void *)GetProcAddress(ws, "accept");
    f_recv = (ws_recv_fn)(void *)GetProcAddress(ws, "recv");
    f_send = (ws_send_fn)(void *)GetProcAddress(ws, "send");
    f_close = (ws_close_fn)(void *)GetProcAddress(ws, "closesocket");
    f_opt = (ws_sockopt_fn)(void *)GetProcAddress(ws, "setsockopt");
    if (!f_startup || !f_socket || !f_bind || !f_listen || !f_accept || !f_recv || !f_send || !f_close) {
        return 0;
    }
    f_startup(0x0202, &wd);
    ls = f_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) {
        return 0;
    }
    if (f_opt) {
        int one = 1;
        f_opt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    }
    for (port = 47001; port < 47040; port++) {
        d_zero(&sa, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = (u_short)(((port & 0xff) << 8) | (port >> 8));
        sa.sin_addr.s_addr = 0x0100007Fu; /* 127.0.0.1 */
        if (f_bind(ls, (const struct sockaddr *)&sa, sizeof sa) == 0) {
            break;
        }
    }
    if (port >= 47040 || f_listen(ls, 2) != 0) {
        return 0;
    }
    {
        wchar_t path[MAX_PATH];
        HANDLE f;
        DWORD w;
        char num[8];
        sb_t sb;
        sb.p = num;
        sb.n = 0;
        sb.cap = sizeof num;
        num[0] = 0;
        sb_dec(&sb, port);
        lstrcpyW(path, g_dir);
        lstrcatW(path, L"\\fh2mp-dbg.port");
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, num, (DWORD)sb.n, &w, NULL);
            CloseHandle(f);
        }
        log_begin();
        log_raw("dbg: command server on 127.0.0.1:");
        log_raw(num);
        log_raw("\n");
    }
    for (;;) {
        SOCKET cs = f_accept(ls, NULL, NULL);
        char in[4096];
        int have = 0;

        if (cs == INVALID_SOCKET) {
            Sleep(200);
            continue;
        }
        for (;;) {
            int n = f_recv(cs, in + have, (int)sizeof in - have - 1, 0);
            char *nl;
            if (n <= 0) {
                break;
            }
            have += n;
            in[have] = 0;
            while ((nl = d_strchr(in, '\n')) != NULL) {
                sb_t o;
                int rest;
                *nl = 0;
                if (nl > in && nl[-1] == '\r') {
                    nl[-1] = 0;
                }
                o.p = g_dbg_out;
                o.n = 0;
                o.cap = DBG_OUT - 8;
                g_dbg_out[0] = 0;
                dbg_cmd(in, &o);
                sb_str(&o, ".\n");
                f_send(cs, o.p, o.n, 0);
                rest = have - (int)(nl + 1 - in);
                d_move(in, nl + 1, (size_t)rest);
                have = rest;
                in[have] = 0;
            }
        }
        f_close(cs);
    }
    return 0;
}

/* ------------------------------------------------------------------ self test */

static volatile LONG *g_selftest_var;   /* VirtualAlloc'd: its own page, so guarding it cannot catch the shim's globals */
static volatile LONG g_selftest_probe_hits;

static __attribute__((noinline)) int dbg_selftest_probe(int x)
{
    g_selftest_probe_hits += 1;
    return x + 1;
}

static DWORD WINAPI dbg_selftest_writer(LPVOID unused)
{
    (void)unused;
    Sleep(300);
    InterlockedExchange(g_selftest_var, 1234);
    Sleep(300);
    return 0;
}

static void dbg_selftest(sb_t *o)
{
    int i, slot, before_watch, before_bp;
    HANDLE t;
    dbg_bp *b;
    volatile int sink = 0;

    if (!g_selftest_var) {
        g_selftest_var = (volatile LONG *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!g_selftest_var) {
            sb_str(o, "selftest: cannot allocate test page\n");
            return;
        }
    }
    /* 1. software breakpoint on a probe function in this DLL */
    for (i = 0; i < DBG_MAX_BP && g_dbp[i].used; i++) {
    }
    if (i < DBG_MAX_BP) {
        b = &g_dbp[i];
        d_zero(b, sizeof *b);
        b->used = b->enabled = 1;
        b->addr = (unsigned char *)(ULONG_PTR)&dbg_selftest_probe;
        lstrcpyA(b->name, "selftest-probe");
        lstrcpyA(b->logs[0], "rcx");
        b->nlogs = 1;
        before_bp = (int)g_ev_seq;
        if (dbg_arm(b)) {
            sink = dbg_selftest_probe(41);
            sb_str(o, (int)g_ev_seq > before_bp && sink == 42 ? "software breakpoint: OK\n"
                                                              : "software breakpoint: FAILED\n");
        } else {
            sb_str(o, "software breakpoint: cannot arm\n");
        }
        dbg_disarm(b);
        b->used = 0;
    }
    /* 2. watchpoint on a variable written from another thread: first page-guard, then hardware */
    for (slot = 4; slot < DBG_MAX_WATCH && g_dw[slot].used; slot++) {
    }
    if (slot < DBG_MAX_WATCH) {
        g_dw[slot].pg = 1;
        g_dw[slot].used = 1;
        g_dw[slot].addr = (unsigned long long)(ULONG_PTR)g_selftest_var;
        g_dw[slot].len = 4;
        g_dw[slot].rw = 0;
        g_dw[slot].bt = 4;
        g_dw[slot].hits = 0;
        lstrcpyA(g_dw[slot].name, "selftest-pg");
        *g_selftest_var = 0;
        dbg_pg_arm_page(g_dw[slot].addr);
        t = CreateThread(NULL, 0, dbg_selftest_writer, NULL, 0, NULL);
        if (t) {
            WaitForSingleObject(t, 3000);
            CloseHandle(t);
        }
        Sleep(200);
        sb_str(o, g_dw[slot].hits > 0 ? "page-guard watchpoint: OK\n" : "page-guard watchpoint: NOT TRIGGERED\n");
        dbg_pg_disarm_page(g_dw[slot].addr);
        g_dw[slot].used = 0;
    }
    for (slot = 0; slot < 4 && g_dw[slot].used; slot++) {
    }
    if (slot < 4) {
        g_dw[slot].pg = 0;
        g_dw[slot].used = 1;
        g_dw[slot].addr = (unsigned long long)(ULONG_PTR)g_selftest_var;
        g_dw[slot].len = 4;
        g_dw[slot].rw = 0;
        g_dw[slot].bt = 4;
        g_dw[slot].hits = 0;
        lstrcpyA(g_dw[slot].name, "selftest-var");
        *g_selftest_var = 0;
        before_watch = (int)g_ev_seq;
        dbg_hw_apply(1);
        t = CreateThread(NULL, 0, dbg_selftest_writer, NULL, 0, NULL);
        if (t) {
            WaitForSingleObject(t, 3000);
            CloseHandle(t);
        }
        Sleep(200);
        sb_str(o, g_dw[slot].hits > 0 || (int)g_ev_seq > before_watch
                      ? "hardware watchpoint: OK\n"
                      : "hardware watchpoint: NOT TRIGGERED (debug registers unsupported here?)\n");
        g_dw[slot].used = 0;
        dbg_hw_apply(1);
    }
    (void)sink;
}

static void dbg_start(void)
{
    wchar_t tmp[8];
    HANDLE t;

    if (GetEnvironmentVariableW(L"FH2MP_NO_DBG", tmp, 8) > 0) {
        return;
    }
    dbg_img_range();
    t = CreateThread(NULL, 0, dbg_server, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
    t = CreateThread(NULL, 0, dbg_hw_thread, NULL, 0, NULL);
    if (t) {
        CloseHandle(t);
    }
}
