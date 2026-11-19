/* The net watch of src/net_watch.inc with built objects laid out the way FH2 1.0.0.64 lays them out: the dispatcher's bit stream (+4 size in bits,
 * +0xc bit cursor, +0x18 buffer holder whose +0x10 points at the bytes; as in net_string_guard.inc) and a connection with its state at +0x18.
 * No game: the stand-in Dispatch returns the result each case sets, the stand-in Disconnect records its call. Expected values are the ones put
 * into the objects. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static char g_lines[16384];
static char g_cur[2048];
static int g_n;
static CRITICAL_SECTION g_cs;

static void log_begin(void) { EnterCriticalSection(&g_cs); }
static void log_raw(const char *s)
{
    size_t n = strlen(s);
    if (g_n + n >= sizeof g_cur) n = sizeof g_cur - 1 - g_n;
    memcpy(g_cur + g_n, s, n);
    g_n += (int)n;
    g_cur[g_n] = 0;
    if (n && s[n - 1] == '\n') {
        strcat(g_lines, g_cur);
        fputs(g_cur, stdout);
        g_n = 0;
        LeaveCriticalSection(&g_cs);
    }
}
static void log_dec(unsigned long v) { char b[16]; sprintf(b, "%lu", v); log_raw(b); }
static void log_hex(unsigned long long v, int digits)
{
    char buf[17];
    int i;
    for (i = digits - 1; i >= 0; i--) {
        buf[i] = "0123456789abcdef"[v & 0xf];
        v >>= 4;
    }
    buf[digits] = 0;
    log_raw(buf);
}
static int read_game_memory(const void *address, void *out, SIZE_T size)
{
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
/* what net_watch.inc takes from fh2mp_mod.c; the hooks themselves are not made in this test */
static unsigned char *find_fh2_image(void) { return NULL; }
static int code_is(const char *tag, const char *what, const unsigned char *img, unsigned rva, const void *expect, unsigned n)
{ (void)tag, (void)what, (void)img, (void)rva, (void)expect, (void)n; return 0; }
static int plat_hook(const char *name, void *target, void *detour, void **original) { (void)name, (void)target, (void)detour, (void)original; return 0; }

#include "src/net_watch.inc"

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); g_failed++; } } while (0)

static int g_result;                      /* what the stand-in Dispatch answers */
static int g_disconnects;
static unsigned g_disconnect_reason;
static void *g_disconnect_conn;
static int fake_dispatch(void *d, void *c, void *s, char only) { (void)d, (void)c, (void)s, (void)only; return g_result; }
static void fake_disconnect(void *conn, unsigned reason) { g_disconnects++; g_disconnect_conn = conn; g_disconnect_reason = reason; }

static unsigned char g_stream[0x40], g_holder[0x20], g_data[80], g_conn[0x300];

static void stream_at(unsigned size_bits, unsigned bit)
{
    unsigned char *data = g_data;
    unsigned char *holder = g_holder;
    unsigned byte = bit >> 3;
    memset(g_stream, 0, sizeof g_stream);
    memcpy(g_stream + 4, &size_bits, 4);
    memcpy(g_stream + 8, &byte, 4);
    memcpy(g_stream + 0xc, &bit, 4);
    memcpy(g_stream + 0x18, &holder, sizeof holder);
    memcpy(g_holder + 0x10, &data, sizeof data);
}

static void conn_state(int state) { memcpy(g_conn + 0x18, &state, 4); }

int main(void)
{
    wchar_t line[600];
    int r;
    InitializeCriticalSection(&g_cs);
    g_nw_dispatch_orig = fake_dispatch;
    g_nw_disconnect_orig = fake_disconnect;

    /* a datagram of 75 bytes (600 bits): after the 88-bit header a message with id 32 (CarMeetGameModeSessionReady) whose Receive refuses */
    memset(g_data, 0, sizeof g_data);
    memcpy(g_data, "\xff\xff\x00\x00\x00\x00\x00\x00\x01\x4b\x00", 11);
    g_data[11] = 32 | 0x80;                /* id 32 in the low 7 bits, the message's first payload bit set */
    g_data[12] = 0xab;
    stream_at(600, 88);
    conn_state(6);
    g_result = (int)0x80600006u;
    r = nw_dispatch((void *)0x1, (void *)0x2, g_stream, 0);
    CHECK(r == (int)0x80600006u);          /* the game gets the result unchanged */
    nw_disconnect(g_conn, 0x80600006u);
    CHECK(g_disconnects == 1 && g_disconnect_conn == g_conn && g_disconnect_reason == 0x80600006u);
    CHECK(strstr(g_lines, "netwatch: the game closed its connection to the game server (close #1, connection state 6): reason 0x80600006, a message handler refused a message") != NULL);
    CHECK(strstr(g_lines, "the refused message: id 32, starting at bit 88 of 600, bytes from there: a0 ab 00") != NULL);
    CHECK(net_watch_header(line) == 1);
    CHECK(!lstrcmpW(line, L"X-FH2MP-Net: v1 seq=1 reason=80600006 msg=32 result=80600006 bits=88/600"));
    CHECK(net_watch_header(line) == 0);    /* once per close */

    /* a keyed event for an unknown object is tolerated: nothing is remembered; a later timeout names no message */
    g_lines[0] = 0;
    g_result = (int)0x8060000eu;
    CHECK(nw_dispatch(NULL, NULL, g_stream, 0) == (int)0x8060000eu);
    g_nw_last.thread = 0;                  /* the earlier refusal is long gone */
    nw_disconnect(g_conn, 0x80600008u);
    CHECK(g_disconnects == 2);
    CHECK(strstr(g_lines, "(close #2, connection state 6): reason 0x80600008, nothing arrived from the server within the time limit\n") != NULL);
    CHECK(strstr(g_lines, "refused message") == NULL);
    CHECK(net_watch_header(line) == 1 && !lstrcmpW(line, L"X-FH2MP-Net: v1 seq=2 reason=80600008"));

    /* a connection already closed (state 0): Disconnect does nothing in the game, so no line and no header; the game's function still runs */
    g_lines[0] = 0;
    conn_state(0);
    nw_disconnect(g_conn, 0x8060000fu);
    CHECK(g_disconnects == 3 && g_lines[0] == 0 && net_watch_header(line) == 0);

    /* before the connection is up a refusal only drops the rest of the datagram, and says so */
    g_result = (int)0x8060000au;
    g_data[11] = 9;
    stream_at(600, 88);
    CHECK(nw_dispatch(NULL, NULL, g_stream, 1) == (int)0x8060000au);
    CHECK(strstr(g_lines, "netwatch: a message from the server was refused before the connection was up, so the rest of its datagram was dropped (#1): "
                          "message 9, result 0x8060000a\n") != NULL);

    /* a stream whose cursor is past its end: the id is unreadable, nothing is read outside it */
    g_lines[0] = 0;
    conn_state(4);
    g_result = (int)0x80600007u;
    stream_at(96, 120);
    nw_dispatch(NULL, NULL, g_stream, 0);
    nw_disconnect(g_conn, 0x80600007u);
    CHECK(strstr(g_lines, "the refused message: id unreadable, starting at bit 120 of 96, bytes from there:\n") != NULL);

    printf(g_failed ? "net watch read: FAIL\n" : "net watch read: all checks pass\n");
    return g_failed != 0;
}
