// FH2MPGuide.dll, the in-game Guide as a FH2 Mod Platform mod, drawn into the game's own frames the way the Steam and Discord overlays
// do: it hooks DXGI's Present/Present1 and ResizeBuffers, and on each present that has something to show it records one command list with
// Dear ImGui's DX12 backend onto the back buffer, on the swap chain's own queue, before the frame goes out. That queue is learnt when the
// swap chain is made (DXGI's CreateSwapChain* are hooked from the mod's start, before Azimuth makes its own; GetDevice does not name it).
// Never on another queue: drawing on the first direct queue the game used removed the device under Azimuth (2026-11-11). Nothing is
// drawn or recorded while the Guide is closed and no toast plays.
// Opening: the pad's Guide button held for half a second (XInputGetStateEx, a reader thread) or Home in the game window. While open the
// game gets an idle pad (Windows.Gaming.Input's IGamepad::GetCurrentReading, which Azimuth reads, is hooked) and the Guide takes the
// keyboard's arrows, Enter, Escape, Q and E from the game window. FH2MP_NO_GUIDE=1 keeps it off. Log: the platform's log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <hstring.h>
#include <roapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx12.h"
#include "fh2mod.h"
#include "gfx.h"
#include "guide.h"
#include "imgui.h"
#include "join.h"
#include "net.h"
#include "sysui.h"
#include "voice.h"

// probe.cpp
void party_probe_start(const FH2ModApi* api, FH2Mod* mod, unsigned char* game, void (*log)(const char*));

namespace {

const FH2ModApi* g_api;
FH2Mod* g_mod;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_api && g_mod) g_api->log(g_mod, buf);
}

// ---- events from the pad thread and the window procedure, used on the render thread
struct Ev {
    PadEvent e;
    uint32_t ch;  // a typed character when not 0 (the Guide's keyboard screen is up)
};
std::mutex g_ev_mu;
std::deque<Ev> g_events;
std::atomic<bool> g_open{false}, g_typing{false}, g_join_ok{false};

void push(PadEvent e) {
    std::lock_guard<std::mutex> l(g_ev_mu);
    if (g_events.size() < 64) g_events.push_back({e, 0});
}

void push_char(uint32_t ch) {
    std::lock_guard<std::mutex> l(g_ev_mu);
    if (g_events.size() < 64) g_events.push_back({PadEvent::A, ch});
}

double now_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

// ---- the game's pad: Azimuth reads Windows.Gaming.Input.Gamepad. While the Guide is open the game gets an idle reading; the buttons held
// when it closes stay hidden from the game until they are let go (so the B that closed the Guide does not also reach the game).
struct WgiReading {
    UINT64 timestamp;
    UINT32 buttons;
    double lt, rt, lx, ly, rx, ry;
};
static_assert(sizeof(WgiReading) == 64, "Windows.Gaming.Input.GamepadReading");
typedef HRESULT(STDMETHODCALLTYPE* GetReadingFn)(void*, WgiReading*);
GetReadingFn o_get_reading;
std::atomic<UINT32> g_hidden{0};

HRESULT STDMETHODCALLTYPE hk_get_reading(void* self, WgiReading* r) {
    HRESULT hr = o_get_reading(self, r);
    if (FAILED(hr) || !r) return hr;
    if (g_open.load()) {
        g_hidden.fetch_or(r->buttons);
        UINT64 ts = r->timestamp;
        *r = WgiReading{};
        r->timestamp = ts;
    } else if (UINT32 h = g_hidden.load()) {
        g_hidden.fetch_and(r->buttons);  // released ones reach the game again
        r->buttons &= ~h;
    }
    return hr;
}

// The IGamepad of a connected pad, from Windows.Gaming.Input's Gamepad statics (the list fills in shortly after first use, so the pad
// thread calls this until it works). Hooks GetCurrentReading (IGamepad slot 8) once; true when hooked.
bool hook_wgi() {
    static const GUID iid_statics = {0x8bbce529, 0xd49c, 0x39e9, {0x95, 0x60, 0xe4, 0x7d, 0xde, 0x96, 0xb7, 0xc8}};
    typedef HRESULT(WINAPI * RoGetFn)(HSTRING, REFIID, void**);
    typedef HRESULT(WINAPI * MakeRefFn)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);
    static HMODULE combase = LoadLibraryW(L"combase.dll");
    static RoGetFn ro_get = combase ? (RoGetFn)(void*)GetProcAddress(combase, "RoGetActivationFactory") : nullptr;
    static MakeRefFn make_ref = combase ? (MakeRefFn)(void*)GetProcAddress(combase, "WindowsCreateStringReference") : nullptr;
    if (!ro_get || !make_ref) return false;
    static const wchar_t cls[] = L"Windows.Gaming.Input.Gamepad";
    HSTRING_HEADER hh;
    HSTRING name = nullptr;
    void** statics = nullptr;
    if (FAILED(make_ref(cls, UINT32(wcslen(cls)), &hh, &name)) || FAILED(ro_get(name, iid_statics, (void**)&statics)) || !statics) return false;
    typedef HRESULT(STDMETHODCALLTYPE * GetPadsFn)(void*, void**);
    typedef HRESULT(STDMETHODCALLTYPE * SizeFn)(void*, UINT32*);
    typedef HRESULT(STDMETHODCALLTYPE * GetAtFn)(void*, UINT32, void**);
    typedef ULONG(STDMETHODCALLTYPE * ReleaseFn)(void*);
    auto vt = [](void* o) { return *(void***)o; };
    void* pads = nullptr;
    void* pad = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(((GetPadsFn)vt(statics)[10])(statics, &pads)) && pads && SUCCEEDED(((SizeFn)vt(pads)[7])(pads, &n)) && n > 0)
        ((GetAtFn)vt(pads)[6])(pads, 0, &pad);
    bool ok = false;
    if (pad) {
        FH2Hook* h = g_api->hook_function(g_mod, "guide: IGamepad::GetCurrentReading", vt(pad)[8], (void*)hk_get_reading, (void**)&o_get_reading);
        ok = h && g_api->enable_hooks(g_mod, h);
        logf(ok ? "guide: the game's pad reading is hooked: the game gets an idle pad while the Guide is open"
                : "guide: the game's pad reading could not be hooked: the game still reads the pad while the Guide is open");
        ((ReleaseFn)vt(pad)[2])(pad);
    }
    if (pads) ((ReleaseFn)vt(pads)[2])(pads);
    ((ReleaseFn)vt(statics)[2])(statics);
    return pad != nullptr;  // tried: do not try again, hooked or not
}

// ---- the pad: XInputGetStateEx (ordinal 100: it reports the Guide button) on the first connected pad, every 33 ms
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
typedef DWORD(WINAPI* GetStateFn)(DWORD, XState*);

DWORD WINAPI pad_thread(LPVOID) {
    GetStateFn get = nullptr;
    bool ex = false;
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll"}) {
        HMODULE m = LoadLibraryW(dll);
        if (!m) continue;
        if ((get = (GetStateFn)(void*)GetProcAddress(m, (LPCSTR)100))) { ex = true; break; }
        if ((get = (GetStateFn)(void*)GetProcAddress(m, "XInputGetState"))) break;
    }
    if (!get) {
        logf("guide: no XInput: the pad cannot open the Guide (Home still does)");
        return 0;
    }
    logf("guide: pad reader started (Guide button %s; hold it 0.5 s, or press Home in the game)", ex ? "seen" : "not reported by this XInput");
    typedef HRESULT(WINAPI * RoInitFn)(int);
    if (HMODULE cb = LoadLibraryW(L"combase.dll"))
        if (auto ro_init = (RoInitFn)(void*)GetProcAddress(cb, "RoInitialize")) ro_init(1);  // RO_INIT_MULTITHREADED, for hook_wgi
    bool wgi = false;
    ULONGLONG next_wgi = 0;
    PadReader reader;
    int slot = -1;
    ULONGLONG next_scan = 0;
    bool seen = false;
    for (;;) {
        Sleep(33);
        ULONGLONG now = GetTickCount64();
        XState st{};
        if (slot >= 0 && get(DWORD(slot), &st) != 0) {
            logf("guide: the pad in XInput slot %d is gone", slot);
            slot = -1;
        }
        if (slot < 0 && now >= next_scan) {
            next_scan = now + 2000;
            for (int i = 0; i < 4 && slot < 0; i++)
                if (get(DWORD(i), &st) == 0) slot = i, logf("guide: pad found in XInput slot %d", i);
        }
        if (slot < 0) continue;
        if (!wgi && now >= next_wgi) {
            next_wgi = now + 2000;
            wgi = hook_wgi();
        }
        if (!seen && (st.buttons & padbits::Guide)) seen = true, logf("guide: the pad's Guide button reaches the Guide");
        PadEvent out[16];
        int n = reader.feed(st.buttons, st.lt, st.rt, st.lx, st.ly, now, g_open.load(), out);
        for (int i = 0; i < n; i++) {
            if (out[i] == PadEvent::Toggle)
                logf("guide: the Guide button %s (held %llu ms)", reader.guide_fired ? "was held long enough: the Guide toggles" : "was tapped: the open Guide closes",
                     (unsigned long long)(now - reader.guide_since));
            push(out[i]);
        }
    }
}

// ---- the game window: Home toggles; while open the Guide takes its navigation keys
HWND g_hwnd;
WNDPROC g_old_proc;

LRESULT CALLBACK guide_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        bool repeat = (lp >> 30) & 1;
        if (wp == VK_HOME && !repeat) {
            push(PadEvent::Toggle);
            return 0;
        }
        if (g_open.load() && g_typing.load()) {
            // the keyboard screen: the PC's keyboard types into it (the layout's own characters, whatever the game's message loop does)
            switch (wp) {
            case VK_ESCAPE: if (!repeat) push(PadEvent::B); return 0;
            case VK_RETURN: if (!repeat) push_char('\r'); return 0;
            case VK_BACK: push_char('\b'); return 0;
            case VK_LEFT: push(PadEvent::Lb); return 0;
            case VK_RIGHT: push(PadEvent::Rb); return 0;
            default: break;
            }
            BYTE state[256];
            wchar_t out[4];
            if (GetKeyboardState(state)) {
                int n = ToUnicode(UINT(wp), UINT((lp >> 16) & 0xff), state, out, 4, 0);
                for (int i = 0; i < n; i++)
                    if (out[i] >= 0x20 && out[i] != 0x7f && (out[i] < 0xd800 || out[i] > 0xdfff)) push_char(out[i]);
            }
            return 0;
        }
        if (g_open.load()) {
            switch (wp) {
            case VK_UP: push(PadEvent::Up); return 0;
            case VK_DOWN: push(PadEvent::Down); return 0;
            case VK_LEFT: push(PadEvent::Left); return 0;
            case VK_RIGHT: push(PadEvent::Right); return 0;
            case VK_RETURN: case VK_SPACE: if (!repeat) push(PadEvent::A); return 0;
            case VK_ESCAPE: case VK_BACK: if (!repeat) push(PadEvent::B); return 0;
            case 'Q': if (!repeat) push(PadEvent::Lb); return 0;
            case 'E': if (!repeat) push(PadEvent::Rb); return 0;
            default: break;
            }
        }
    }
    if (g_open.load() && g_typing.load() && (msg == WM_KEYUP || msg == WM_CHAR || msg == WM_SYSKEYUP)) return 0;
    if (g_open.load() && (msg == WM_KEYUP || msg == WM_CHAR) &&
        (wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT || wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_BACK))
        return 0;
    return CallWindowProcW(g_old_proc, h, msg, wp, lp);
}

// ---- Direct3D 12 state of the overlay
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain3*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* Present1Fn)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffersFn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn o_present;
Present1Fn o_present1;
ResizeBuffersFn o_resize;

ID3D12CommandQueue* g_queue;  // the swap chain's own queue

// Which queue each swap chain was made with (DXGI's CreateSwapChain* detours fill it).
std::mutex g_chains_mu;
std::vector<std::pair<IUnknown*, ID3D12CommandQueue*>> g_chains;  // keyed by the swap chain's IUnknown (its COM identity)

IUnknown* identity(IUnknown* p) {
    IUnknown* u = nullptr;
    if (!p || FAILED(p->QueryInterface(IID_PPV_ARGS(&u))) || !u) return nullptr;
    u->Release();  // only compared, never used
    return u;
}

void note_chain(IUnknown* device, IUnknown* made) {
    ID3D12CommandQueue* q = nullptr;
    IUnknown* chain = identity(made);
    if (!device || !chain || FAILED(device->QueryInterface(IID_PPV_ARGS(&q))) || !q) return;  // not Direct3D 12
    std::lock_guard<std::mutex> l(g_chains_mu);
    for (auto& c : g_chains)
        if (c.first == chain) {
            if (c.second) c.second->Release();
            c.second = q;
            return;
        }
    g_chains.emplace_back(chain, q);
}

ID3D12CommandQueue* queue_of(IUnknown* presented) {
    IUnknown* chain = identity(presented);
    std::lock_guard<std::mutex> l(g_chains_mu);
    for (auto& c : g_chains)
        if (c.first == chain) return c.second;
    return nullptr;
}

typedef HRESULT(STDMETHODCALLTYPE* CreateSCFn)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSCHwndFn)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*,
                                                    IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSCCoreFn)(IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE* CreateSCCompFn)(IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
CreateSCFn o_create_sc;
CreateSCHwndFn o_create_hwnd;
CreateSCCoreFn o_create_core;
CreateSCCompFn o_create_comp;

HRESULT STDMETHODCALLTYPE hk_create_sc(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** out) {
    HRESULT hr = o_create_sc(f, dev, d, out);
    if (SUCCEEDED(hr) && out) note_chain(dev, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_create_hwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                         IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_create_hwnd(f, dev, w, d, fs, o, out);
    if (SUCCEEDED(hr) && out) note_chain(dev, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_create_core(IDXGIFactory2* f, IUnknown* dev, IUnknown* win, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_create_core(f, dev, win, d, o, out);
    if (SUCCEEDED(hr) && out) note_chain(dev, *out);
    return hr;
}
HRESULT STDMETHODCALLTYPE hk_create_comp(IDXGIFactory2* f, IUnknown* dev, const DXGI_SWAP_CHAIN_DESC1* d, IDXGIOutput* o, IDXGISwapChain1** out) {
    HRESULT hr = o_create_comp(f, dev, d, o, out);
    if (SUCCEEDED(hr) && out) note_chain(dev, *out);
    return hr;
}
ID3D12Device* g_device;
IDXGISwapChain3* g_swapchain;  // the swap chain the overlay is set up for
ID3D12DescriptorHeap *g_rtv_heap, *g_srv_heap;
ID3D12GraphicsCommandList* g_list;
ID3D12Fence* g_fence;
HANDLE g_fence_event;
UINT64 g_fence_value;
struct Frame { ID3D12CommandAllocator* alloc = nullptr; ID3D12Resource* buffer = nullptr; D3D12_CPU_DESCRIPTOR_HANDLE rtv{}; UINT64 fence = 0; };
std::vector<Frame> g_frames;
UINT g_rtv_size, g_srv_size;
const UINT SRV_COUNT = 256;  // ImGui's font atlas and the pictures (the player's, friends')
std::vector<UINT> g_srv_free;
bool g_ready, g_failed, g_buffers_ok;
DXGI_FORMAT g_format;
GuideAssets g_assets;
Guide g_guide;
double g_last_frame;
thread_local bool t_in_present;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    if (g_srv_free.empty()) {  // all in use: share the last one (a picture shows wrong; nothing breaks)
        cpu->ptr = g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(SRV_COUNT - 1) * g_srv_size;
        gpu->ptr = g_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(SRV_COUNT - 1) * g_srv_size;
        return;
    }
    UINT i = g_srv_free.back();
    g_srv_free.pop_back();
    cpu->ptr = g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g_srv_size;
    gpu->ptr = g_srv_heap->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g_srv_size;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
    UINT i = UINT((cpu.ptr - g_srv_heap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srv_size);
    if (i < SRV_COUNT - 1) g_srv_free.push_back(i);
}

void wait_gpu() {
    if (!g_fence || !g_queue) return;
    UINT64 v = ++g_fence_value;
    g_queue->Signal(g_fence, v);
    if (g_fence->GetCompletedValue() < v) {
        g_fence->SetEventOnCompletion(v, g_fence_event);
        WaitForSingleObject(g_fence_event, 2000);
    }
}

void release_buffers() {
    wait_gpu();
    for (Frame& f : g_frames)
        if (f.buffer) f.buffer->Release(), f.buffer = nullptr;
    g_buffers_ok = false;
}

bool make_buffers() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_frames.size(); i++) {
        if (FAILED(g_swapchain->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].buffer)))) return false;
        g_frames[i].rtv.ptr = h.ptr + SIZE_T(i) * g_rtv_size;
        g_device->CreateRenderTargetView(g_frames[i].buffer, nullptr, g_frames[i].rtv);
    }
    g_buffers_ok = true;
    return true;
}

// The achievement toast's pictures and sounds, which the launcher puts into data/bin/achievement-toast (constants.ACH_TOAST): the
// pictures become textures by their names (ach_art_set), the sounds stay in memory for PlaySound. What is missing is drawn without.
std::vector<unsigned char> g_ach_wav[3], g_ach_wav_360[3];  // by tier: the Xbox app's sounds, the Xbox 360 style's own

std::vector<unsigned char> read_file(const std::wstring& path) {
    std::vector<unsigned char> out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    if (size != INVALID_FILE_SIZE && size < (16u << 20)) {
        out.resize(size);
        if (!ReadFile(f, out.data(), size, &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(f);
    return out;
}

void load_ach_art() {
    std::wstring dir = std::wstring(g_api->azimuth_dir) + L"\\data\\bin\\achievement-toast\\";
    int pictures = 0, missing = 0;
    for (int where = 0; where < 2; where++)
        for (const char* const* f = ach_art_files(where); *f; f++) {
            std::wstring name(*f, *f + strlen(*f));
            std::vector<unsigned char> png = read_file(dir + name), rgba;
            int w = 0, h = 0;
            if (png.empty() || !decode_png(png.data(), png.size(), &rgba, &w, &h)) {
                missing++;
                continue;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu;
            srv_alloc(nullptr, &cpu, &gpu);
            if (!upload_texture(g_device, g_queue, rgba.data(), w, h, cpu)) {
                srv_free(nullptr, cpu, gpu);
                missing++;
                continue;
            }
            ach_art_set(&g_assets.ach, *f, (ImTextureID)gpu.ptr);
            pictures++;
        }
    static const wchar_t* sounds[3] = {L"ach-sound-normal.wav", L"ach-sound-rare.wav", L"ach-sound-mythic.wav"};
    int wavs = 0;
    for (int i = 0; i < 3; i++) wavs += !(g_ach_wav[i] = read_file(dir + sounds[i])).empty();
    // the Xbox 360 style's own sounds, when the folder has them (without one, that tier keeps the sound above)
    static const wchar_t* sounds_360[3] = {L"ach-sound-360-normal.wav", L"ach-sound-360-rare.wav", L"ach-sound-360-mythic.wav"};
    for (int i = 0; i < 3; i++) wavs += !(g_ach_wav_360[i] = read_file(dir + sounds_360[i])).empty();
    logf("guide: achievement toast: %d pictures and %d sounds loaded from data\\bin\\achievement-toast (%d pictures missing: drawn without)", pictures, wavs, missing);
}

// The toast's sound, from the real winmm.dll in the system folder (the one next to azimuth.exe is Extra Options').
void play_ach_sound(AchTier tier) {
    typedef BOOL(WINAPI * PlaySoundWFn)(LPCWSTR, HMODULE, DWORD);
    static PlaySoundWFn play = [] {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE m = n ? LoadLibraryW((std::wstring(sys, n) + L"\\winmm.dll").c_str()) : nullptr;
        return m ? (PlaySoundWFn)(void*)GetProcAddress(m, "PlaySoundW") : nullptr;
    }();
    const std::vector<unsigned char>& own = g_ach_wav_360[int(tier)];
    const std::vector<unsigned char>& wav = g_guide.ach_style() == 1 && !own.empty() ? own : g_ach_wav[int(tier)];
    if (play && !wav.empty()) play((LPCWSTR)wav.data(), nullptr, 0x0004 /* SND_MEMORY */ | 0x0001 /* SND_ASYNC */ | 0x0002 /* SND_NODEFAULT */);
}

// The player's Guide theme: data\bin\fh2mp-guide-theme.txt (its key), so the Guide has it from the first frame; the server keeps it too,
// for the player's gamercard as others see it. A Guide with no file takes the server's when the profile arrives (social_and_toasts).
std::wstring theme_file() { return std::wstring(g_api->azimuth_dir) + L"\\data\\bin\\fh2mp-guide-theme.txt"; }
bool g_theme_from_file;

void load_theme() {
    std::vector<unsigned char> b = read_file(theme_file());
    std::string key(b.begin(), b.end());
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' ')) key.pop_back();
    if (key.empty()) return;
    theme_apply(key);
    g_theme_from_file = true;
    logf("guide: theme %s (data\\bin\\fh2mp-guide-theme.txt)", theme_name(theme_current()).c_str());
}

void save_theme(const std::string& key) {
    HANDLE f = CreateFileW(theme_file().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        std::string line = key + "\n";
        WriteFile(f, line.data(), DWORD(line.size()), &wrote, nullptr);
        CloseHandle(f);
    }
    g_theme_from_file = true;
}

// The player's voice settings: data\bin\fh2mp-guide-voice.txt, three numbers (microphone 0/1, voice chat 0/1, volume 0..4). Without the
// file: microphone on, voice chat on, volume 2.
std::wstring voice_file() { return std::wstring(g_api->azimuth_dir) + L"\\data\\bin\\fh2mp-guide-voice.txt"; }

void apply_voice(bool mic, bool chat, int volume) {
    voice_settings(mic, chat, volume);
    g_guide.set_voice(mic, chat, volume);
}

// How an achievement is shown: data\bin\fh2mp-guide-notify.txt, one number (0 the Guide's toast, 1 the Xbox 360's notification, 2 the
// Xbox One's toast).
// Without the file: 0.
std::wstring notify_file() { return std::wstring(g_api->azimuth_dir) + L"\\data\\bin\\fh2mp-guide-notify.txt"; }

void load_notify() {
    std::vector<unsigned char> b = read_file(notify_file());
    g_guide.set_notify(b.empty() ? 0 : b[0] - '0');
}

void save_notify(int style) {
    HANDLE f = CreateFileW(notify_file().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    const char line[2] = {char('0' + style), '\n'};
    WriteFile(f, line, 2, &wrote, nullptr);
    CloseHandle(f);
}

void load_voice() {
    std::vector<unsigned char> b = read_file(voice_file());
    std::string text(b.begin(), b.end());
    int mic = 1, chat = 1, volume = 2;
    if (sscanf(text.c_str(), "%d %d %d", &mic, &chat, &volume) != 3) mic = 1, chat = 1, volume = 2;
    apply_voice(mic != 0, chat != 0, std::clamp(volume, 0, 4));
}

void save_voice(bool mic, bool chat, int volume) {
    HANDLE f = CreateFileW(voice_file().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[32];
    int n = snprintf(line, sizeof line, "%d %d %d\n", mic ? 1 : 0, chat ? 1 : 0, volume);
    DWORD wrote = 0;
    WriteFile(f, line, DWORD(n), &wrote, nullptr);
    CloseHandle(f);
}

// The player's picture: Azimuth's data/player.png (256x256 RGBA, written by the launcher), uploaded once into a shader-visible texture.
ImTextureID load_gamerpic() {
    std::wstring path = std::wstring(g_api->azimuth_dir) + L"\\data\\player.png";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 0;
    std::vector<unsigned char> png(GetFileSize(f, nullptr));
    DWORD got = 0;
    if (!ReadFile(f, png.data(), DWORD(png.size()), &got, nullptr)) got = 0;
    CloseHandle(f);
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!decode_png(png.data(), got, &rgba, &w, &h)) return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    srv_alloc(nullptr, &cpu, &gpu);
    if (!upload_texture(g_device, g_queue, rgba.data(), w, h, cpu)) {
        srv_free(nullptr, cpu, gpu);
        return 0;
    }
    logf("guide: player picture loaded (%dx%d)", w, h);
    return (ImTextureID)gpu.ptr;
}

// The player's name: player_name in Azimuth's data/azimuth.ini (UTF-16), which the launcher writes.
std::string read_gamertag() {
    std::wstring path = std::wstring(g_api->azimuth_dir) + L"\\data\\azimuth.ini";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "FH2MP";
    std::vector<char> raw(GetFileSize(f, nullptr) + 2, 0);
    DWORD got = 0;
    ReadFile(f, raw.data(), DWORD(raw.size() - 2), &got, nullptr);
    CloseHandle(f);
    std::wstring text((const wchar_t*)raw.data(), got / 2);
    size_t at = text.find(L"player_name=");
    if (at == std::wstring::npos) return "FH2MP";
    size_t end = text.find_first_of(L"\r\n", at);
    std::wstring name = text.substr(at + 12, end == std::wstring::npos ? std::wstring::npos : end - at - 12);
    std::string out;
    for (wchar_t c : name)
        if (c > 0 && c < 0x80) out += char(c);
    return out.empty() ? "FH2MP" : out;
}

bool setup(IDXGISwapChain3* sc) {
    g_queue = queue_of(sc);
    if (!g_queue) {
        logf("guide: this swap chain was made before the Guide could see it, so its command queue is unknown: no Guide this run");
        return false;
    }
    if (g_queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        logf("guide: the swap chain's queue is not a direct queue (%d): no Guide", int(g_queue->GetDesc().Type));
        return false;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) return false;
    g_swapchain = sc;
    g_format = desc.BufferDesc.Format;
    g_frames.assign(desc.BufferCount, Frame{});
    D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRV_COUNT, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(g_device->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&g_rtv_heap))) || FAILED(g_device->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srv_heap))))
        return false;
    g_rtv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srv_size = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = SRV_COUNT - 1; i-- > 0;) g_srv_free.push_back(i);
    for (Frame& f : g_frames)
        if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].alloc, nullptr, IID_PPV_ARGS(&g_list)))) return false;
    g_list->Close();
    if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) return false;
    g_fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!make_buffers()) return false;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr, io.LogFilename = nullptr;
    load_fonts(g_assets);

    ImGui_ImplDX12_InitInfo info;
    info.Device = g_device, info.CommandQueue = g_queue, info.NumFramesInFlight = int(desc.BufferCount);
    info.RTVFormat = g_format, info.SrvDescriptorHeap = g_srv_heap;
    info.SrvDescriptorAllocFn = srv_alloc, info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return false;

    g_assets.gamerpic = load_gamerpic();
    load_ach_art();
    load_theme();
    load_voice();
    load_notify();
    g_guide.gamertag = read_gamertag();
    g_hwnd = desc.OutputWindow;
    if (g_hwnd && !g_old_proc) g_old_proc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)guide_wndproc);
    logf("guide: ready on the game's swap chain (%u buffers, %ux%u, format %d)", desc.BufferCount, desc.BufferDesc.Width, desc.BufferDesc.Height,
         int(g_format));
    return true;
}

// The server's side: the friend list (re-read twice a second at most), pictures that arrived, and what came of the player's actions.
void social_and_toasts(double now) {
    static double last_read;
    static uint64_t version = ~0ull;
    if (now - last_read > 0.5) {
        last_read = now;
        Social so = net_social();
        if (so.version != version) {
            version = so.version, g_guide.set_social(so);
            // no theme file on this PC (a new install): the one the server keeps for the player
            if (!g_theme_from_file && so.loaded && !so.me.theme.empty() && so.me.theme != theme_current()) {
                theme_apply(so.me.theme);
                save_theme(so.me.theme);
                logf("guide: theme %s, as the player's profile on the server has it", theme_name(so.me.theme).c_str());
            }
        }
    }
    uint64_t talkers[16];
    g_guide.set_talking(talkers, voice_talking(talkers, 16));  // every frame: a talker's mark follows the voice
    // this player's achievements: the Achievements mod's unlock file, looked at every 3 s and read when it changed
    static double last_ach = -10;
    static uint64_t ach_stamp;
    if (now - last_ach > 3) {
        last_ach = now;
        AchUnlocks u;
        if (ach_refresh(g_api->azimuth_dir, &ach_stamp, &u)) {
            if (u.known) logf("guide: achievements read from the Achievements mod's unlock file (%d of %d unlocked, %dG)", u.count(), int(ach_defs().size()), u.gamerscore());
            else logf("guide: the Achievements mod has no unlock file (fh2-mods\\Achievements\\User_*.ini): the Guide's Achievements stay off");
            // what is new since the last read: a toast each, in the list's order (none at the first read: that is what was already
            // unlocked); the Mythic Achievement when the last of the game's own 50 comes
            static AchUnlocks before;
            static bool read_once;
            if (read_once && before.known && u.known) {
                const std::vector<AchDef>& defs = ach_defs();
                int base_before = 0, base_after = 0, base_all = 0;
                // The mod's first look at a save unlocks everything the save already earned, all at one second (win1 2026-11-12: 18 at
                // 06:05:50 UTC, 18 toasts): three or more new ones with one time are that catch-up, not achievements earned now.
                std::vector<int> fresh;
                std::map<std::string, int> per_time;
                for (int i = 0; i < int(defs.size()); i++)
                    if (u.has(defs[i].id) && !before.has(defs[i].id)) fresh.push_back(i), per_time[u.when.at(defs[i].id)]++;
                bool catch_up = per_time.size() == 1 && fresh.size() >= 3;
                if (catch_up)
                    logf("guide: %d achievements unlocked at once (%s): the Achievements mod caught up with the save, no toasts",
                         int(fresh.size()), per_time.begin()->first.c_str());
                for (int i = 0; i < int(defs.size()); i++) {
                    const AchDef& d = defs[i];
                    if (d.group == 0) base_all++, base_before += before.has(d.id), base_after += u.has(d.id);
                }
                for (int i : catch_up ? std::vector<int>() : fresh) {
                    const AchDef& d = defs[i];
                    float rare = net_ach_rare_percent(d.id);
                    logf("guide: achievement unlocked: %s (%dG)%s: its toast", d.name, d.g, rare >= 0 ? ", a rare one" : "");
                    g_guide.ach_toast(rare >= 0 ? AchTier::Rare : AchTier::Normal, d.g, d.name, d.desc, rare, i, now);
                }
                if (base_after == base_all && base_before < base_all && !catch_up) {
                    char words[48];
                    snprintf(words, sizeof words, "%d of %d Achievements", base_all, base_all);
                    logf("guide: all %d of the game's own achievements unlocked: the Mythic Achievement's toast", base_all);
                    g_guide.ach_toast(AchTier::Mythic, -1, "Forza Horizon 2", words, -1, -1, now);
                }
            }
            if (u.known) before = u;
            read_once = true;
            g_guide.set_achievements(u);
            net_report_achievements(u);
        }
    }
    PicReady pic;
    while (net_take_pic(&pic)) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu;
        D3D12_GPU_DESCRIPTOR_HANDLE gpu;
        srv_alloc(nullptr, &cpu, &gpu);
        if (upload_texture(g_device, g_queue, pic.rgba.data(), pic.w, pic.h, cpu)) g_assets.pics[pic.xuid] = (ImTextureID)gpu.ptr;
        else srv_free(nullptr, cpu, gpu);
    }
    NetResult r;
    while (net_take_result(&r)) g_guide.on_result(r, g_assets, now);
    // a join the game has not taken for 20 s: its online part is not running (the title screen, say); it is dropped and said
    unsigned long long waited = 0;
    if (join_waiting(&waited) && waited > 20000) {
        join_cancel();
        logf("guide: join dropped: the game's matchmaking module did not run for 20 s after the seat was held");
        NetResult late;
        late.op = NetOp::Join, late.ok = false, late.code = "game_not_ready";
        g_guide.on_result(late, g_assets, now);
    }
}

void ask(const GuideRequest& r);

// What the Guide asked of the server after an event.
void ask(const GuideRequest& r) {
    switch (r.action) {
    case GuideAction::Invite: net_invite(r.xuid, r.name, r.text); break;
    case GuideAction::AcceptInvite: net_accept(r.id, r.xuid, r.name); break;
    case GuideAction::DeclineInvite: net_decline(r.id, r.xuid, r.name); break;
    case GuideAction::FriendRequest: net_friend_request(r.name, r.xuid); break;
    case GuideAction::AcceptFriend: net_accept_friend(r.xuid, r.name); break;
    case GuideAction::DeclineFriend: net_decline_friend(r.xuid, r.name); break;
    case GuideAction::RemoveFriend: net_remove_friend(r.xuid, r.name); break;
    case GuideAction::SendText: net_send_text(r.xuid, r.name, r.text); break;
    case GuideAction::ReadChat: net_read_chat(r.name, r.id); break;
    case GuideAction::HideText: net_hide_text(r.id); break;
    case GuideAction::Join: net_join(r.xuid, r.name, r.text, r.kind); break;
    case GuideAction::SetProfile: net_set_profile(r.name, r.text); break;
    case GuideAction::SetVoice: {
        bool mic = r.id & 1, chat = r.id & 2;
        int volume = int(r.id >> 2);
        apply_voice(mic, chat, volume);
        save_voice(mic, chat, volume);
        break;
    }
    case GuideAction::SetNotify:
        save_notify(int(r.id));
        logf("guide: achievements are shown as %s: kept in data\\bin\\fh2mp-guide-notify.txt", r.id == 1 ? "the Xbox 360's notification" : r.id == 2 ? "the Xbox One's toast" : "the Guide's toast");
        break;
    case GuideAction::SetTheme:
        save_theme(r.name);
        net_set_profile("theme", r.name);
        logf("guide: theme set to %s: kept in data\\bin\\fh2mp-guide-theme.txt and on the player's profile", theme_name(r.name).c_str());
        break;
    case GuideAction::JoinGame: join_request(r.scid, r.kind, r.text, r.name); break;
    case GuideAction::Lookup: net_lookup(r.xuid); break;
    case GuideAction::Achievements: net_achievements(r.xuid, r.name); break;
    case GuideAction::None:
    case GuideAction::Resume: break;
    }
}

void render(IDXGISwapChain3* sc) {
    if (g_failed) return;
    if (!g_ready) {
        g_ready = setup(sc);
        if (!g_ready) {
            g_failed = true;
            logf("guide: could not set up on the game's swap chain: the Guide stays off for this run");
            return;
        }
        sysui_showing(true);
    }
    if (sc != g_swapchain) return;  // a second swap chain (a video, say): the Guide only draws on the game's
    double now = now_s();
    std::deque<Ev> evs;
    {
        std::lock_guard<std::mutex> l(g_ev_mu);
        evs.swap(g_events);
    }
    g_guide.game_joins = g_join_ok.load();
    for (const Ev& ev : evs) {
        bool was = g_guide.open;
        if (ev.ch) {
            if (was)
                for (const GuideRequest& r : g_guide.type_char(ev.ch, now)) ask(r);
            continue;
        }
        PadEvent e = ev.e;
        if (e == PadEvent::OpenInvite) {
            g_guide.open_invite(now);
            if (!was) logf("guide: opened by the game's Invite Friends");
            continue;
        }
        if (!was) {
            if (e == PadEvent::Toggle) g_guide.toggle(now), logf("guide: opened");
            continue;
        }
        for (const GuideRequest& r : g_guide.handle(e, now)) ask(r);
        if (!g_guide.open) logf("guide: closed");
    }
    if (g_open.load() != g_guide.open) net_guide_open(g_guide.open);
    g_open.store(g_guide.open);
    g_typing.store(g_guide.typing());
    social_and_toasts(now);
    // what the game asked the Xbox system UI for: the Guide shows it, and the game's wait ends when the Guide is closed again
    static void* sys_op;
    static int sys_kind;
    SysAsk sys;
    while (sysui_take(&sys)) {
        if (sys_op) sysui_complete(sys_op, false, "", {});
        if (sys.kind == SysAsk::ProfileCard) g_guide.open_profile_for_game(sys.xuid, now);
        else if (sys.kind == SysAsk::People) g_guide.open_people_for_game(sys.title, sys.people, sys.chosen, sys.min, sys.max, now);
        else g_guide.open_keyboard_for_game(sys.text, sys.title, sys.description, now);
        sys_op = sys.op, sys_kind = sys.kind;
    }
    if (sys_op && !g_guide.open) {
        std::string text;
        std::vector<uint64_t> people;
        bool done = sys_kind == SysAsk::Keyboard ? g_guide.take_game_text(&text) : sys_kind == SysAsk::People ? g_guide.take_game_people(&people) : true;
        if (sys_kind == SysAsk::People)
            logf(done ? "guide: the people picker's choice goes back to the game (%d players)" : "guide: the people picker was dismissed: the game gets no players (%d)",
                 int(people.size()));
        if (sys_kind == SysAsk::Keyboard)
            logf(done ? "guide: the keyboard's text goes back to the game (%d bytes)" : "guide: the keyboard was dismissed: the game keeps its own text (%d bytes typed)",
                 int(text.size()));
        sysui_complete(sys_op, done, text, people);
        sys_op = nullptr;
    }
    for (const GuideRequest& r : g_guide.take_requests()) ask(r);
    if (g_open.load() != g_guide.open) net_guide_open(g_guide.open), g_open.store(g_guide.open);
    g_typing.store(g_guide.typing());
    if (!g_guide.busy(now)) return;
    if (!g_buffers_ok && !make_buffers()) return;

    UINT bi = sc->GetCurrentBackBufferIndex();
    Frame& f = g_frames[bi];
    if (g_fence->GetCompletedValue() < f.fence) {
        g_fence->SetEventOnCompletion(f.fence, g_fence_event);
        WaitForSingleObject(g_fence_event, 1000);
    }
    D3D12_RESOURCE_DESC bd = f.buffer->GetDesc();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(bd.Width), float(bd.Height));
    io.DeltaTime = g_last_frame > 0 ? float(std::min(0.25, std::max(1e-4, now - g_last_frame))) : 1.0f / 60;
    g_last_frame = now;
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    g_guide.draw(ImGui::GetForegroundDrawList(), g_assets, io.DisplaySize.x, io.DisplaySize.y, now);
    AchTier sound_tier;
    if (g_guide.take_ach_sound(&sound_tier)) play_ach_sound(sound_tier);
    ImGui::Render();

    f.alloc->Reset();
    g_list->Reset(f.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = f.buffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT, b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srv_heap);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_list);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_queue->ExecuteCommandLists(1, lists);
    g_queue->Signal(g_fence, f.fence = ++g_fence_value);
}

HRESULT STDMETHODCALLTYPE hk_present(IDXGISwapChain3* sc, UINT sync, UINT flags) {
    if (!t_in_present && !(flags & DXGI_PRESENT_TEST)) {
        t_in_present = true;
        render(sc);
        t_in_present = false;
    }
    return o_present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE hk_present1(IDXGISwapChain3* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!t_in_present && !(flags & DXGI_PRESENT_TEST)) {
        t_in_present = true;
        render(sc);
        t_in_present = false;
    }
    return o_present1(sc, sync, flags, p);
}

HRESULT STDMETHODCALLTYPE hk_resize(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    if (g_ready && sc == g_swapchain) release_buffers();
    HRESULT hr = o_resize(sc, n, w, h, fmt, flags);
    if (g_ready && sc == g_swapchain && SUCCEEDED(hr)) {
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        if (d.BufferCount != g_frames.size() || d.BufferDesc.Format != g_format) {
            logf("guide: the game changed its swap chain's buffers (%u, format %d): the Guide stays off for this run", d.BufferCount, int(d.BufferDesc.Format));
            g_failed = true;
            sysui_showing(false);
        }
    }
    return hr;
}

LRESULT CALLBACK dummy_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

// The game's own Pause > Game Party > Invite Friends. The popup (built in game+0xdddf30, build 1.0.0.64) gives its items ids (Invite
// Friends is 0) and calls a std::function on a choice whose body is game+0xdaefe0: it switches on the chosen item's id (mov ecx,
// [rdi+0x68] at game+0xdaf04b); case 0 starts at game+0xdaf226 and asks game+0x17b9700 for the shell's invite UI, which Azimuth does not
// have, so nothing showed (2026-11-11: no call reached Azimuth's stub objects on the press). An instruction hook in case 0
// (game+0xdaf232, mov [rsp+0x38], r14d: no rip-relative operand) opens the Guide on Invite to Game; the game's code goes on as before.
void on_invite_friends(FH2Context*, void*) {
    logf("guide: the game's Invite Friends was chosen: the Guide opens Invite to Game");
    push(PadEvent::OpenInvite);
}

// The game asked for the system invite UI (sysui.cpp; any thread).
void on_game_invite_ui() { push(PadEvent::OpenInvite); }

[[maybe_unused]] void hook_invite_friends(unsigned char* base) {
    static const unsigned char sw[] = {0x8b, 0x4f, 0x68};                                     // game+0xdaf04b mov ecx, [rdi+0x68]
    static const unsigned char case0[] = {0x48, 0x8d, 0x05, 0x53, 0xbd, 0x01, 0x00,           // game+0xdaf226 lea rax, [rip+0x1bd53]
                                          0x48, 0x89, 0x44, 0x24, 0x30, 0x44, 0x89, 0x74, 0x24, 0x38};  // mov [rsp+0x30], rax; mov [rsp+0x38], r14d
    if (memcmp(base + 0xdaf04b, sw, sizeof sw) || memcmp(base + 0xdaf226, case0, sizeof case0))
        return logf("guide: the Game Party popup's code is not the one this Guide knows: Invite Friends does not open the Guide");
    FH2Hook* h = g_api->hook_instruction(g_mod, "guide: the game's Invite Friends (game+0xdaf232)", base + 0xdaf232, on_invite_friends, nullptr);
    if (!h || !g_api->enable_hooks(g_mod, h)) return logf("guide: the game's Invite Friends could not be hooked: it does not open the Guide");
    logf("guide: the game's Invite Friends opens the Guide's Invite to Game");
}

// The Party probe (probe.cpp), on with FH2MP_GUIDE_PROBE=1: logs the Party statics calls, to find the one behind the game's Invite Friends.
// (game+0x179a110 is not it: it runs when Free Roam is joined and then every frame, 2026-11-11.)

// DXGI's swap chain makers, hooked as early as possible: they tell which queue each swap chain belongs to.
bool hook_factory() {
    IDXGIFactory2* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || !fac) {
        logf("guide: no DXGI factory: no Guide");
        return false;
    }
    void** fvt = *(void***)fac;
    FH2Hook* h[4] = {g_api->hook_function(g_mod, "guide: IDXGIFactory::CreateSwapChain", fvt[10], (void*)hk_create_sc, (void**)&o_create_sc),
                     g_api->hook_function(g_mod, "guide: IDXGIFactory2::CreateSwapChainForHwnd", fvt[15], (void*)hk_create_hwnd, (void**)&o_create_hwnd),
                     g_api->hook_function(g_mod, "guide: IDXGIFactory2::CreateSwapChainForCoreWindow", fvt[16], (void*)hk_create_core, (void**)&o_create_core),
                     g_api->hook_function(g_mod, "guide: IDXGIFactory2::CreateSwapChainForComposition", fvt[24], (void*)hk_create_comp, (void**)&o_create_comp)};
    fac->Release();
    bool ok = true;
    for (FH2Hook* x : h) ok = ok && x && g_api->enable_hooks(g_mod, x);
    if (!ok) logf("guide: the swap chain makers could not be hooked: no Guide");
    return ok;
}

// The present vtable addresses come from a throw-away device, queue and swap chain on a hidden window (DXGI and Direct3D 12 share them).
void net_log(const char* line) { logf("%s", line); }

DWORD WINAPI hook_thread(LPVOID) {
    if (!hook_factory()) return 0;
    if (!g_api->wait_for_game(180000)) {
        logf("guide: the game never came up: no Guide");
        return 0;
    }
    net_start(g_api->azimuth_dir, net_log);
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = dummy_proc, wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"FH2MPGuideProbe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    IDXGIFactory2* fac = nullptr;
    IDXGISwapChain1* sc = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 64, sd.Height = 64, sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, sd.BufferCount = 2, sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bool ok = SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) && SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) &&
              SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && SUCCEEDED(fac->CreateSwapChainForHwnd(q, w, &sd, nullptr, nullptr, &sc));
    if (!ok) {
        logf("guide: could not make the probe swap chain: no Guide");
    } else {
        void** svt = *(void***)sc;
        FH2Hook* hooks[3] = {
            g_api->hook_function(g_mod, "guide: IDXGISwapChain::Present", svt[8], (void*)hk_present, (void**)&o_present),
            g_api->hook_function(g_mod, "guide: IDXGISwapChain1::Present1", svt[22], (void*)hk_present1, (void**)&o_present1),
            g_api->hook_function(g_mod, "guide: IDXGISwapChain::ResizeBuffers", svt[13], (void*)hk_resize, (void**)&o_resize)};
        bool on = hooks[0] && hooks[2] && g_api->enable_hooks(g_mod, hooks[0]) && g_api->enable_hooks(g_mod, hooks[2]) &&
                  (!hooks[1] || g_api->enable_hooks(g_mod, hooks[1]));
        if (!on)
            logf("guide: the present hooks could not be made: no Guide");
        else
            logf("guide: hooked the game's presents; waiting for its first frame");
    }
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (q) q->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    if (ok) CloseHandle(CreateThread(nullptr, 0, pad_thread, nullptr, 0, nullptr));
    if (unsigned char* base = (unsigned char*)g_api->wait_for_code(180000)) {
        sysui_install(g_api, g_mod, base, net_log, on_game_invite_ui);
        voice_install(g_api, g_mod, base, net_log);
        g_join_ok.store(join_install(g_api, g_mod, base, net_log));
        wchar_t probe[8];
        if (GetEnvironmentVariableW(L"FH2MP_GUIDE_PROBE", probe, 8) && probe[0] == L'1') party_probe_start(g_api, g_mod, base, net_log);
    }
    return 0;
}

}  // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    FH2ModInfo info{};
    info.size = sizeof info;
    info.name = "FH2MP Guide";
    info.version = "0.1.0";
    info.github_repo = nullptr;
    info.platform_version = FH2MOD_PLATFORM_VERSION;
    info.check_updates = 0;
    info.description = "The Xbox 360 style Guide over the game: hold the controller's Guide button for a second, or press Home.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 0;
    g_api = api;
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"FH2MP_NO_GUIDE", env, 8) && env[0] == L'1') {
        logf("guide: off (FH2MP_NO_GUIDE=1)");
        return 0;
    }
    CreateThread(nullptr, 0, hook_thread, nullptr, 0, nullptr);
    return 0;
}
