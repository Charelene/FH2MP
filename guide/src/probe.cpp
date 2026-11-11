// The Party probe (FH2MP_GUIDE_PROBE=1): which Windows.Xbox.Multiplayer.Party statics methods the game calls, how often and from where,
// so the call behind Pause > Game Party > Invite Friends can be found by pressing it. The game gets the statics through its activation
// helper (game+0xeda14c in 1.0.0.64: PCWSTR class name, IID, out), with three IIDs (e6110c4c-... from game+0x17cd00d and game+0x179a15d,
// 87521843-... from game+0x17a67e5, 15946429-... from game+0x17b5776). The probe asks for the same three objects and puts a pass-through
// on every method slot (6 on; IUnknown and IInspectable are left alone), up to the first slot that is not code in the same module.
// Each slot logs its first call, then a count when it is called again after 3 s of quiet. Arguments pass through untouched (up to five
// after `this`, enough for every Party statics method).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "fh2mod.h"

namespace {

const FH2ModApi* p_api;
FH2Mod* p_mod;
void (*p_log)(const char*);
unsigned char* p_game;
size_t p_game_size;

const int IFACES = 3, SLOTS = 24;
const char* IFACE_NAME[IFACES] = {"e6110c4c", "87521843", "15946429"};
const GUID IFACE_IID[IFACES] = {{0xe6110c4c, 0x3660, 0x4230, {0x98, 0x80, 0x20, 0x8b, 0x14, 0x9a, 0x3c, 0xa4}},
                                {0x87521843, 0x2298, 0x43c4, {0x81, 0xf1, 0xc7, 0xc5, 0x0a, 0xb7, 0x94, 0xe1}},
                                {0x15946429, 0x17a1, 0x4fdf, {0x9d, 0x67, 0x76, 0x6e, 0xd0, 0xf4, 0xb8, 0x68}}};

typedef uint64_t (*SlotFn)(void*, void*, void*, void*, void*, void*);
SlotFn p_orig[IFACES][SLOTS];
std::atomic<ULONGLONG> p_last[IFACES][SLOTS];
std::atomic<unsigned> p_count[IFACES][SLOTS];

void logf(const char* fmt, ...) {
    char buf[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (p_log) p_log(buf);
}

// Call sites seen so far (the game's return addresses): one line when a site is first seen (with the stack above it), then one with its
// count when it calls again after 3 s of quiet.
struct Site {
    std::atomic<uintptr_t> ret{0};
    std::atomic<ULONGLONG> last{0};
    std::atomic<unsigned> n{0};
};
Site p_sites[1024];

Site* site_of(uintptr_t ret) {
    size_t i = (ret >> 2) * 2654435761u % 1024;
    for (size_t k = 0; k < 1024; k++, i = (i + 1) % 1024) {
        uintptr_t cur = p_sites[i].ret.load();
        if (cur == ret) return &p_sites[i];
        if (cur == 0) {
            uintptr_t expect = 0;
            if (p_sites[i].ret.compare_exchange_strong(expect, ret) || expect == ret) return &p_sites[i];
        }
    }
    return nullptr;
}

void where(void* p, char* out, size_t n) {
    size_t off = size_t((unsigned char*)p - p_game);
    if (off < p_game_size) snprintf(out, n, "game+0x%zx", off);
    else snprintf(out, n, "%p", p);
}

template <int I, int S>
uint64_t probe_slot(void* self, void* a, void* b, void* c, void* d, void* e) {
    void* ret = __builtin_return_address(0);
    if (Site* site = site_of(uintptr_t(ret))) {
        ULONGLONG now = GetTickCount64(), last = site->last.exchange(now);
        unsigned n = ++site->n;
        if (n == 1 || now - last > 3000) {
            void* frames[8] = {};
            USHORT got = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
            char stack[400] = "", one[40];
            for (USHORT f = 0; f < got; f++) {
                where(frames[f], one, sizeof one);
                strncat(stack, f ? " < " : "", sizeof stack - strlen(stack) - 1);
                strncat(stack, one, sizeof stack - strlen(stack) - 1);
            }
            char from[40];
            where(ret, from, sizeof from);
            logf("guide probe: slot %d from %s (%s, call %u; args %p %p %p) stack: %s", S, from, n == 1 ? "first" : "again", n, a, b, c, stack);
        }
    }
    return p_orig[I][S](self, a, b, c, d, e);
}

template <int I, int... S>
constexpr std::array<SlotFn, sizeof...(S)> slot_table(std::integer_sequence<int, S...>) {
    return {&probe_slot<I, S>...};
}

const std::array<SlotFn, SLOTS> DETOURS[IFACES] = {slot_table<0>(std::make_integer_sequence<int, SLOTS>{}),
                                                    slot_table<1>(std::make_integer_sequence<int, SLOTS>{}),
                                                    slot_table<2>(std::make_integer_sequence<int, SLOTS>{})};

bool is_code_of(void* p, HMODULE mod) {
    MEMORY_BASIC_INFORMATION mi{};
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    if (!(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m);
    return m == mod;
}
}  // namespace

void party_probe_start(const FH2ModApi* api, FH2Mod* mod, unsigned char* game, void (*log)(const char*)) {
    p_api = api, p_mod = mod, p_game = game, p_log = log;
    p_game_size = 0x2400000;  // the mapped image of 1.0.0.64 is about 36 MB
    if (game[0xeda14c] != 0xe9) return logf("guide probe: game+0xeda14c is not the activation helper this probe knows: no probe");
    typedef HRESULT (*ActivateFn)(const wchar_t*, const GUID*, void**);
    ActivateFn activate = (ActivateFn)(game + 0xeda14c);
    typedef HRESULT(WINAPI * RoInitFn)(int);
    if (HMODULE cb = LoadLibraryW(L"combase.dll"))
        if (auto ro_init = (RoInitFn)(void*)GetProcAddress(cb, "RoInitialize")) ro_init(1);
    void** hooked_vt[IFACES] = {};
    for (int i = 0; i < IFACES; i++) {
        void** obj = nullptr;
        HRESULT hr = activate(L"Windows.Xbox.Multiplayer.Party", &IFACE_IID[i], (void**)&obj);
        if (FAILED(hr) || !obj) {
            logf("guide probe: Party %s: activation failed (0x%08lx)", IFACE_NAME[i], (unsigned long)hr);
            continue;
        }
        void** vt = *(void***)obj;
        bool again = false;
        for (int j = 0; j < i; j++) again = again || hooked_vt[j] == vt;
        hooked_vt[i] = vt;
        if (again) {
            logf("guide probe: Party %s shares the vtable %p already watched", IFACE_NAME[i], (void*)vt);
            continue;
        }
        HMODULE owner = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)vt[6], &owner);
        int hooked = 0;
        for (int s = 6; s < SLOTS && is_code_of(vt[s], owner); s++) {
            char name[64];
            snprintf(name, sizeof name, "guide probe: Party %s slot %d", IFACE_NAME[i], s);
            FH2Hook* h = api->hook_pointer(mod, name, &vt[s], (void*)DETOURS[i][s], (void**)&p_orig[i][s]);
            if (h && api->enable_hooks(mod, h)) hooked++;
        }
        logf("guide probe: Party %s: %d method slots watched (vtable %p)", IFACE_NAME[i], hooked, (void*)vt);
    }
}
