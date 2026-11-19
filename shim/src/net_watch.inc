/* Net watch (log only; FH2MP_NO_NETWATCH=1 turns it off): each time the game closes its connection to our game server, one "netwatch:"
 * line says why, with the message that caused it.
 *
 * FH2 1.0.0.64 (docs/re/netv2.md section 3, read 2026-11-19): Connection::ProcessReceivedPackets (game+0x1766100) hands every message to
 * MessageDispatcher::Dispatch (game+0x1761d60). On an established connection any negative result except 0x8060000e (a keyed event for an
 * object this client does not have, skipped) logs "MP Debugging Disconnect due to bad packet." and calls Connection::Disconnect
 * (game+0x1761c40) with that result as the reason. Disconnect itself sends no CloseConnection (only Connection::Close, game+0x1768690, does);
 * whether a listener of the disconnect closes the transport afterwards is not read, so the server may see only silence, the way a crash
 * looks. Hooks: Dispatch remembers the last refused message of its thread; Disconnect logs the reason and, when the reason is that refusal,
 * the message (id, where it starts, its first bytes).
 *
 * The newest close that the game did not make itself also rides the game's next request to the LSP once, as "X-FH2MP-Net" (xhr2.inc), so the
 * server log has it without a crash report (native/internal/lsp netclose.go). */

#define NW_SKIPPED_OBJECT 0x8060000eu /* keyed event for an unknown object: the handler skipped its body, the datagram goes on */
#define NW_GAME_CLOSED 0x8060000fu    /* the game closed the connection itself (Connection::Close, or Update while closing) */

enum { NW_OK, NW_SKIPPED, NW_DATAGRAM_DROPPED, NW_DISCONNECT };

/* What ProcessReceivedPackets does with Dispatch's result (connect_only: Dispatch's 4th argument, 1 while the connection is not established). */
static int net_watch_outcome(int result, int connect_only)
{
    if (result >= 0) return NW_OK;
    if ((unsigned)result == NW_SKIPPED_OBJECT) return NW_SKIPPED;
    return connect_only ? NW_DATAGRAM_DROPPED : NW_DISCONNECT;
}

/* Disconnect reasons the notes name (docs/re/netv2.md, disconnect table). */
static const char *net_watch_reason(unsigned reason)
{
    switch (reason) {
    case 0x80600006u: return "a message handler refused a message";
    case 0x80600007u: return "a message ran past the end of its packet";
    case 0x80600008u: return "nothing arrived from the server within the time limit";
    case 0x8060000au: return "a message id with no handler, or a message not allowed yet";
    case 0x8060000cu: return "the connect attempt failed";
    case 0x8060000fu: return "the game closed it";
    }
    return "a reason the notes do not name";
}

/* `count` bits (at most 32), least significant first, from bit `pos` of a stream; `buf` holds `have` bytes of it from byte pos / 8 on. -> 0 when
 * the bits are not all in `buf`. */
static int net_watch_bits(const unsigned char *buf, unsigned have, unsigned pos, unsigned count, unsigned *out)
{
    unsigned i, v = 0, at = pos & 7;
    if (count > 32 || ((at + count + 7) >> 3) > have) return 0;
    for (i = 0; i < count; i++) {
        v |= (unsigned)((buf[(at + i) >> 3] >> ((at + i) & 7)) & 1) << i;
    }
    *out = v;
    return 1;
}

/* One close as the header carries it. msg < 0: the close followed no refused message. */
typedef struct {
    unsigned seq, reason, result, at, size;
    int msg;
} net_watch_close;

static char *nw_put(char *p, char *end, const char *s)
{
    while (*s && p < end) *p++ = *s++;
    return p;
}

static char *nw_put_dec(char *p, char *end, unsigned v)
{
    char d[10];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n && p < end) *p++ = d[--n];
    return p;
}

static char *nw_put_hex8(char *p, char *end, unsigned v)
{
    int i;
    for (i = 28; i >= 0 && p < end; i -= 4) *p++ = "0123456789abcdef"[(v >> i) & 15];
    return p;
}

/* The X-FH2MP-Net value: "v1 seq=3 reason=80600006 msg=32 result=80600006 bits=118/600", without the message part when there is none.
 * -> its length; `out` is always terminated. */
static unsigned net_watch_header_text(char *out, unsigned cap, const net_watch_close *c)
{
    char *p = out, *end = out + (cap ? cap - 1 : 0);
    if (!cap) return 0;
    p = nw_put(p, end, "v1 seq=");
    p = nw_put_dec(p, end, c->seq);
    p = nw_put(p, end, " reason=");
    p = nw_put_hex8(p, end, c->reason);
    if (c->msg >= 0) {
        p = nw_put(p, end, " msg=");
        p = nw_put_dec(p, end, (unsigned)c->msg);
        p = nw_put(p, end, " result=");
        p = nw_put_hex8(p, end, c->result);
        p = nw_put(p, end, " bits=");
        p = nw_put_dec(p, end, c->at);
        p = nw_put(p, end, "/");
        p = nw_put_dec(p, end, c->size);
    }
    *p = 0;
    return (unsigned)(p - out);
}

#ifndef NET_WATCH_LOGIC_ONLY

typedef int (*nw_dispatch_fn)(void *dispatcher, void *context, void *stream, char connect_only);
typedef void (*nw_disconnect_fn)(void *connection, unsigned reason);
static nw_dispatch_fn g_nw_dispatch_orig;
static nw_disconnect_fn g_nw_disconnect_orig;

/* the last refused message (written on the network thread that dispatched it, read by Disconnect on the same thread) */
static struct {
    DWORD thread, tick;
    int msg, connect_only;
    unsigned result, at, size, head_len;
    unsigned char head[16];
} g_nw_last;
static volatile LONG g_nw_closes, g_nw_dropped;

/* the newest close for the next request's header (net_watch_header); g_nw_text_lock guards g_nw_text */
static volatile LONG g_nw_text_lock, g_nw_text_seq;
static LONG g_nw_text_sent;
static char g_nw_text[96];

/* Reads the stream the dispatcher was given (+4 size in bits, +0x18 buffer holder whose +0x10 is the byte storage, as net_string_guard.inc)
 * and keeps up to 16 bytes from bit `at` on. */
static void nw_refused(void *stream, unsigned at, int result, int connect_only)
{
    unsigned char header[32];
    UINT_PTR holder = 0, data = 0;
    unsigned size = 0, id = 0;
    g_nw_last.thread = GetCurrentThreadId();
    g_nw_last.tick = GetTickCount();
    g_nw_last.msg = -1;
    g_nw_last.result = (unsigned)result;
    g_nw_last.at = at;
    g_nw_last.size = 0;
    g_nw_last.head_len = 0;
    g_nw_last.connect_only = connect_only;
    if (!stream || !read_game_memory(stream, header, sizeof header)) return;
    memcpy(&size, header + 4, 4);
    memcpy(&holder, header + 24, sizeof holder);
    g_nw_last.size = size;
    if (!holder || at >= size || !read_game_memory((const void *)(holder + 16), &data, sizeof data) || !data) return;
    {
        unsigned have = ((size + 7) >> 3) - (at >> 3);
        if (have > sizeof g_nw_last.head) have = sizeof g_nw_last.head;
        if (!read_game_memory((const void *)(data + (at >> 3)), g_nw_last.head, have)) return;
        g_nw_last.head_len = have;
        if (net_watch_bits(g_nw_last.head, have, at, 7, &id)) g_nw_last.msg = (int)id;
    }
    if (net_watch_outcome(result, connect_only) == NW_DATAGRAM_DROPPED) {
        LONG n = InterlockedIncrement(&g_nw_dropped);
        if (n <= 4 || n % 256 == 0) {
            log_begin();
            log_raw("netwatch: a message from the server was refused before the connection was up, so the rest of its datagram was dropped (#");
            log_dec((unsigned long)n);
            log_raw("): message ");
            if (g_nw_last.msg >= 0) log_dec((unsigned long)g_nw_last.msg); else log_raw("unreadable");
            log_raw(", result 0x");
            log_hex((unsigned)result, 8);
            log_raw("\n");
        }
    }
}

static int nw_dispatch(void *dispatcher, void *context, void *stream, char connect_only)
{
    unsigned at = stream ? *(volatile unsigned *)((char *)stream + 0xc) : 0;   /* bit cursor before the 7-bit id */
    int r = g_nw_dispatch_orig(dispatcher, context, stream, connect_only);
    if (net_watch_outcome(r, connect_only != 0) >= NW_DATAGRAM_DROPPED) nw_refused(stream, at, r, connect_only != 0);
    return r;
}

static void nw_disconnect(void *connection, unsigned reason)
{
    int state = 0;
    if (connection && read_game_memory((const char *)connection + 0x18, &state, sizeof state) && state != 0) {
        net_watch_close c;
        LONG n = InterlockedIncrement(&g_nw_closes);
        int with_msg = g_nw_last.thread == GetCurrentThreadId() && g_nw_last.result == reason && GetTickCount() - g_nw_last.tick < 1000;
        c.seq = (unsigned)n;
        c.reason = reason;
        c.msg = with_msg ? g_nw_last.msg : -1;
        c.result = g_nw_last.result;
        c.at = g_nw_last.at;
        c.size = g_nw_last.size;
        if (n <= 10 || n % 100 == 0) {
            log_begin();
            log_raw("netwatch: the game closed its connection to the game server (close #");
            log_dec((unsigned long)n);
            log_raw(", connection state ");
            log_dec((unsigned long)state);
            log_raw("): reason 0x");
            log_hex(reason, 8);
            log_raw(", ");
            log_raw(net_watch_reason(reason));
            if (with_msg) {
                unsigned i;
                log_raw("; the refused message: id ");
                if (g_nw_last.msg >= 0) log_dec((unsigned long)g_nw_last.msg); else log_raw("unreadable");
                log_raw(", starting at bit ");
                log_dec(g_nw_last.at);
                log_raw(" of ");
                log_dec(g_nw_last.size);
                log_raw(", bytes from there:");
                for (i = 0; i < g_nw_last.head_len; i++) {
                    log_raw(" ");
                    log_hex(g_nw_last.head[i], 2);
                }
            }
            log_raw("\n");
        }
        if (reason != NW_GAME_CLOSED) { /* the game's own close of a session it leaves is routine: only the log has it */
            while (InterlockedCompareExchange(&g_nw_text_lock, 1, 0) != 0) SwitchToThread();
            net_watch_header_text(g_nw_text, sizeof g_nw_text, &c);
            InterlockedExchange(&g_nw_text_lock, 0);
            InterlockedIncrement(&g_nw_text_seq);
        }
    }
    g_nw_disconnect_orig(connection, reason);
}

/* For xhr2.inc: "X-FH2MP-Net: <value>" in `line` (600 wide chars) once per new close. -> 1 when there is one. */
static int net_watch_header(wchar_t *line)
{
    static const wchar_t name[] = L"X-FH2MP-Net: ";
    LONG seq = g_nw_text_seq;
    int i, k;
    if (seq == g_nw_text_sent) return 0;
    for (k = 0; name[k]; k++) line[k] = name[k];
    while (InterlockedCompareExchange(&g_nw_text_lock, 1, 0) != 0) SwitchToThread();
    for (i = 0; g_nw_text[i] && i < (int)sizeof g_nw_text - 1; i++) line[k + i] = (wchar_t)(unsigned char)g_nw_text[i];
    InterlockedExchange(&g_nw_text_lock, 0);
    line[k + i] = 0;
    g_nw_text_sent = seq;
    return 1;
}

static int patch_net_watch(void)
{
    static const unsigned char dispatch[16] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57};
    static const unsigned char disconnect[16] = {0x40,0x57,0x48,0x83,0xec,0x30,0x48,0xc7,0x44,0x24,0x20,0xfe,0xff,0xff,0xff,0x48};
    /* the bad-packet path itself: mov edx,r14d; mov rcx,rdi; call Connection::Disconnect (game+0x17662d4) */
    static const unsigned char bad_packet[11] = {0x41,0x8b,0xd6,0x48,0x8b,0xcf,0xe8,0x61,0xb9,0xff,0xff};
    unsigned char *img = find_fh2_image();
    if (!img) return 0;
    if (!code_is("netwatch", "MessageDispatcher::Dispatch", img, 0x1761d60, dispatch, sizeof dispatch) ||
        !code_is("netwatch", "Connection::Disconnect", img, 0x1761c40, disconnect, sizeof disconnect) ||
        !code_is("netwatch", "the bad-packet disconnect in ProcessReceivedPackets", img, 0x17662d4, bad_packet, sizeof bad_packet)) {
        return 0;
    }
    if (!plat_hook("netwatch: connection close reasons", img + 0x1761c40, (void *)nw_disconnect, (void **)&g_nw_disconnect_orig)) return 0;
    if (!plat_hook("netwatch: refused messages", img + 0x1761d60, (void *)nw_dispatch, (void **)&g_nw_dispatch_orig)) return 0;
    log_begin();
    log_raw("hook: netwatch: each close of the game's connection to the game server is logged with its reason and the message that caused it "
            "(game+0x1761c40, game+0x1761d60)\n");
    return 1;
}

#endif
