// What the Guide needs of the game's swap chain when another mod or the start's timing hides it (header only, so
// tests/swapchain_test.cpp runs the same code against a real DXGI without a game). Player logs 2026-11-13 (ShowFPS 1.1.1, ANNA Drone
// Mode): ShowFPS copies a swap chain's vtable at its creation and points Present/Present1 of the copy at its own function, which calls on
// only to the detours enabled before ShowFPS's (fh2mod.h: the most recently enabled detour runs first), so a later hook on DXGI's Present
// never runs for that swap chain; and on a fast start Azimuth makes its swap chain before the Guide's CreateSwapChain* hooks are on, so
// the swap chain's queue was never seen.
//   - find_dxgi_vtable: DXGI's own swap chain vtable from a copy of it (a probe swap chain another mod copied)
//   - cloned_vtable: a swap chain that calls through another mod's copy of DXGI's vtable (its Present, Present1 or ResizeBuffers differ)
//   - queue_offsets / queue_in_chain: where DXGI keeps a swap chain's queue, learnt on a swap chain made with a known queue, then read
//     from the game's (accepted only as a DIRECT queue object on the swap chain's own device)
//   - Once: one draw per frame when a present passes two of the Guide's hooks
// Every read of memory that is not ours goes through ReadProcessMemory on this process: a bad address fails, it does not fault.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <string>
#include <vector>

namespace chainfind {

const int VT_SLOTS = 23;  // IUnknown (0-2), IDXGIObject (3-6), IDXGIDeviceSubObject (7), IDXGISwapChain (8-17), IDXGISwapChain1 (18-22)
const int PRESENT = 8, RESIZE = 13, PRESENT1 = 22;
const int FIRST_OWN = 8;  // entries 0-7 (QueryInterface .. GetDevice) are left as DXGI's in a copied vtable

inline bool read(const void* at, void* out, size_t n) {
    SIZE_T got = 0;
    return at && ReadProcessMemory(GetCurrentProcess(), at, out, n, &got) && got == n;
}

inline HMODULE module_of(const void* p) {
    HMODULE m = nullptr;
    if (!p || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m)) return nullptr;
    return m;
}

// "ShowFPS.dll+0x6030", or the bare address when no module holds it.
inline std::string where(const void* p) {
    char buf[MAX_PATH + 32];
    HMODULE m = module_of(p);
    char path[MAX_PATH];
    DWORD n = m ? GetModuleFileNameA(m, path, MAX_PATH) : 0;
    if (!n) {
        snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)(uintptr_t)p);
        return buf;
    }
    const char* base = path;
    for (const char* c = path; *c; c++)
        if (*c == '\\' || *c == '/') base = c + 1;
    snprintf(buf, sizeof buf, "%s+0x%llx", base, (unsigned long long)((uintptr_t)p - (uintptr_t)m));
    return buf;
}

// The index of the first of entries 0..VT_SLOTS-1 that is not in `dxgi`, or -1 when they all are.
inline int first_foreign(void* const* vt, HMODULE dxgi) {
    for (int i = 0; i < VT_SLOTS; i++)
        if (module_of(vt[i]) != dxgi) return i;
    return -1;
}

// DXGI's own swap chain vtable, found in dxgi.dll's read-only data from `copy` (another mod's copy of it, VT_SLOTS entries read): the
// place whose entries all lie in dxgi.dll and equal the copy's wherever the copy's lie in dxgi.dll too (at least 16 of them). -> nullptr
// when there is no such place.
inline void* const* find_dxgi_vtable(void* const* copy, HMODULE dxgi) {
    if (!dxgi) return nullptr;
    unsigned char* base = (unsigned char*)dxgi;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int known = 0;
    for (int i = 0; i < VT_SLOTS; i++) known += module_of(copy[i]) == dxgi;
    if (known < 16) return nullptr;
    for (int s = 0; s < nt->FileHeader.NumberOfSections; s++) {
        DWORD ch = sec[s].Characteristics;
        if (!(ch & IMAGE_SCN_MEM_READ) || (ch & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_WRITE))) continue;
        void* const* p = (void* const*)(base + sec[s].VirtualAddress);
        size_t n = sec[s].Misc.VirtualSize / sizeof(void*);
        for (size_t at = 0; at + VT_SLOTS <= n; at++) {
            if (p[at] != copy[0]) continue;  // QueryInterface is always DXGI's own in a copy
            bool same = true;
            for (int i = 0; i < VT_SLOTS && same; i++)
                same = module_of(p[at + i]) == dxgi && (module_of(copy[i]) != dxgi || p[at + i] == copy[i]);
            if (same) return p + at;
        }
    }
    return nullptr;
}

// The vtable `chain` calls through when it is a copy of DXGI's (`dxgi_vt`: DXGI's entries 0..VT_SLOTS-1, at `dxgi_vt_addr`) whose
// Present, Present1 or ResizeBuffers is another function; nullptr when it calls through DXGI's own, or the memory is not such a swap chain
// (entries 0-7 not DXGI's, or unreadable).
inline void** cloned_vtable(const void* chain, void* const* dxgi_vt, const void* dxgi_vt_addr) {
    void** vt = nullptr;
    void* e[VT_SLOTS];
    if (!read(chain, &vt, sizeof vt) || !vt || vt == dxgi_vt_addr || !read(vt, e, sizeof e)) return nullptr;
    for (int i = 0; i < FIRST_OWN; i++)
        if (e[i] != dxgi_vt[i]) return nullptr;
    if (e[PRESENT] == dxgi_vt[PRESENT] && e[PRESENT1] == dxgi_vt[PRESENT1] && e[RESIZE] == dxgi_vt[RESIZE]) return nullptr;
    return vt;
}

// Where DXGI keeps a swap chain's queue: the offsets from the swap chain's interface pointer (IDXGISwapChain1, which is also the
// IDXGISwapChain the game calls Present on) of the fields that hold `queue`, read on a swap chain made with that queue.
inline std::vector<long> queue_offsets(IDXGISwapChain1* chain, ID3D12CommandQueue* queue, long from = -0x200, long to = 0x800) {
    std::vector<long> out;
    for (long off = from; off < to; off += long(sizeof(void*))) {
        void* p = nullptr;
        if (read((const char*)chain + off, &p, sizeof p) && p == queue) out.push_back(off);
    }
    return out;
}

// The queue of `chain`, read at `offsets`: only a field that holds an object with the command queue vtable `queue_vtable` (the probe
// queue's: every queue of the process' Direct3D 12 runtime shares it), whose queue is of type DIRECT on the swap chain's own device.
// -> that queue, AddRef'd, and its offset in *at; nullptr when no field holds one (the Guide then draws on no queue at all: drawing on
// another one than the swap chain's removed the device under Azimuth, 2026-11-11).
inline ID3D12CommandQueue* queue_in_chain(IDXGISwapChain1* chain, const std::vector<long>& offsets, const void* queue_vtable, long* at) {
    ID3D12Device* dev = nullptr;
    if (!queue_vtable || FAILED(chain->GetDevice(IID_PPV_ARGS(&dev))) || !dev) return nullptr;
    ID3D12CommandQueue* out = nullptr;
    for (long off : offsets) {
        void* p = nullptr;
        void* vt = nullptr;
        if (!read((const char*)chain + off, &p, sizeof p) || !p || !read(p, &vt, sizeof vt) || vt != queue_vtable) continue;
        ID3D12CommandQueue* q = nullptr;
        if (FAILED(((IUnknown*)p)->QueryInterface(IID_PPV_ARGS(&q))) || !q) continue;
        ID3D12Device* qdev = nullptr;
        bool ok = q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qdev))) && qdev == dev;
        if (qdev) qdev->Release();
        if (ok) {
            out = q;
            if (at) *at = off;
            break;
        }
        q->Release();
    }
    dev->Release();
    return out;
}

// One draw per frame: a present that comes through another mod's copied vtable to the Guide and then, through that mod, to the Guide's
// hook on DXGI's own Present is drawn on at the outer one only. Per thread; held across the call to the original.
struct Once {
    static int& depth() {
        thread_local int d = 0;
        return d;
    }
    bool first;
    Once() : first(depth()++ == 0) {}
    ~Once() { depth()--; }
    Once(const Once&) = delete;
    Once& operator=(const Once&) = delete;
};

}  // namespace chainfind
