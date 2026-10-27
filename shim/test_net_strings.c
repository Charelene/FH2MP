#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define FH2_TIMESTAMP 0x565501edu
#define FH2_SIZE_OF_IMAGE 0x3a18000u
#include "src/game_image.inc"
static void log_begin(void) {}
static void log_raw(const char *s) { (void)s; }
#include "src/net_string_guard.inc"

/* Offline codec fixtures only. No sockets, game process or client traffic. */
typedef struct { unsigned char data[512]; unsigned bits; } fixture;
typedef struct { unsigned char pad[16]; void *data; } storage;
typedef struct { unsigned strict, size, byte_pos, bit_pos; void *pad; storage *buffer; } stream;
static unsigned calls;
static unsigned long long delegate(void *peer, void *s, void *listener, ULONG_PTR extra)
{
    assert(peer == (void *)1 && listener == (void *)2 && extra == 0x12345678);
    assert(s);
    ++calls;
    return 0x1234;
}
static void put(fixture *f, unsigned value, unsigned n)
{
    for (unsigned i = 0; i < n; ++i, ++f->bits)
        if ((value >> i) & 1) f->data[f->bits >> 3] |= 1u << (f->bits & 7);
}
static void text(fixture *f, unsigned n, unsigned kind)
{
    put(f, kind, 2); put(f, n, kind * 8 + 6);
    for (unsigned i = 0; i < n; ++i) put(f, 'A' + i % 26, 8);
}
static fixture message(unsigned handler, unsigned alignment, unsigned kind, unsigned flags)
{
    fixture f = {{0}, 0};
    put(&f, 0, alignment);
    if (handler == 0) {
        text(&f, 38, kind); text(&f, 16, kind);
        for (unsigned i = 0; i < 162; ++i) put(&f, i & 1, 1);
    } else if (handler == 1) {
        /* 64 requires at least the 14-bit length form. */
        text(&f, kind ? 64 : 63, kind); text(&f, 0, kind);
    } else {
        put(&f, flags, 2); put(&f, 7, 7);
        if (flags == 2) put(&f, 91, 8);
        put(&f, 9, 5); put(&f, 1, 1); text(&f, 36, kind);
    }
    return f;
}
static void check(fixture *f, unsigned start, unsigned handler, int accepted)
{
    storage data = {{0}, f->data};
    stream s = {0, f->bits, start >> 3, start, NULL, &data}, before = s;
    unsigned old = calls;
    unsigned long long result = net_string_receive(handler, delegate, (void *)1, &s, (void *)2, 0x12345678);
    assert(result == (accepted ? 0x1234u : 0x80600006u));
    assert(calls == old + accepted);
    assert(!memcmp(&s, &before, sizeof s));
}
int main(void)
{
    assert(sizeof(stream) == 32 && offsetof(stream, buffer) == 24 && offsetof(storage, data) == 16);
    unsigned cases = 0;
    for (unsigned handler = 0; handler < 3; ++handler)
        for (unsigned align = 0; align < 8; ++align)
            for (unsigned kind = 0; kind < 4; ++kind)
                for (unsigned flags = 0; flags < 4; ++flags) {
                    fixture f = message(handler, align, kind, flags);
                    check(&f, align, handler, 1); ++cases;
                    /* Every truncated prefix/body/tail is rejected without native side effects. */
                    unsigned full = f.bits;
                    for (unsigned size = align; size < full; ++size) {
                        f.bits = size; check(&f, align, handler, 0); ++cases;
                    }
                    f.bits = full; put(&f, 0x65, 7); /* packed next message is permitted */
                    check(&f, align, handler, 1); ++cases;
                }
    fixture f = {{0x09, 0x11}, 16}; /* Literal wire length 1090: cls1, 14-bit length. */
    check(&f, 0, 0, 0); ++cases;
    for (unsigned handler = 0; handler < 3; ++handler) {
        fixture bad = {{0}, 0};
        if (handler == 2) { put(&bad, 0, 2); put(&bad, 0, 7); put(&bad, 0, 6); }
        text(&bad, handler == 0 ? 39 : handler == 1 ? 65 : 37, 1);
        check(&bad, 0, handler, 0); ++cases;
    }
    for (unsigned align = 0; align < 8; ++align) {
        for (unsigned handler = 0; handler < 2; ++handler) {
            fixture bad = {{0}, 0};
            put(&bad, 0, align); text(&bad, 0, 0);
            text(&bad, handler == 0 ? 17 : 65, 1);
            if (handler == 0)
                for (unsigned i = 0; i < 162; ++i) put(&bad, 0, 1);
            check(&bad, align, handler, 0); ++cases;
        }
    }
    f = message(0, 0, 0, 0);
    storage data = {{0}, f.data};
    stream s = {0, f.bits, 0, 0, NULL, &data};
    s.bit_pos = f.bits + 1; assert(!net_string_preflight(&s, 0));
    s.bit_pos = 0; s.byte_pos = 1; assert(!net_string_preflight(&s, 0));
    s.byte_pos = 0; s.buffer = NULL; assert(!net_string_preflight(&s, 0));
    s.buffer = &data; data.data = NULL; assert(!net_string_preflight(&s, 0));
    assert(!net_string_preflight(NULL, 0));
    unsigned char *page = VirtualAlloc(NULL, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(page);
    memcpy(page + 4096 - 2, "\x09\x11", 2);
    DWORD old;
    assert(VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old));
    data.data = page + 4096 - 2;
    s.size = 16;
    assert(!net_string_preflight(&s, 0)); /* Oversize must not read its alleged payload. */
    assert(VirtualFree(page, 0, MEM_RELEASE));
    assert(!net_string_preflight(&s, 0));
    /* Actual wrappers preserve delegation and each use the correct handler schema. */
    g_string_open_orig = g_string_meet_orig = g_string_bucket_orig = delegate;
    for (unsigned h = 0; h < 3; ++h) {
        f = message(h, 3, 1, 2); data.data = f.data;
        s.size = f.bits; s.bit_pos = 3; s.byte_pos = 0;
        net_string_receive_fn wrapper = h == 0 ? net_string_open : h == 1 ? net_string_meet : net_string_bucket;
        assert(wrapper((void *)1, &s, (void *)2, 0x12345678) == 0x1234);
    }
    printf("netstrings: %u codec boundary/truncation cases; invalid memory and all receive wrappers checked\n", cases);
    return 0;
}
