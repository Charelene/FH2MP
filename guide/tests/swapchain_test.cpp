// The swap chain finder (src/chainfind.h) against this PC's own DXGI and Direct3D 12, without a game. build.sh chaintest, then
// build/swapchain-test.exe: exit code 0 = every case holds, 2 = no Direct3D 12 device on this PC (nothing tested).
// The cases are the three runs of the player logs of 2026-11-13: a swap chain whose queue was never seen (the queue is read from it),
// a swap chain another mod gave a copied vtable (found, and its Present hooked there), and a probe swap chain that is such a copy (DXGI's
// own vtable found again).
#include <cstdio>
#include <cstring>

#include "chainfind.h"

static int failures;
#define CHECK(c) ((c) ? (void)0 : (void)(std::printf("FAIL line %d: %s\n", __LINE__, #c), ++failures))

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

static HWND window() {
    static bool registered;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = proc, wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"FH2MPGuideChainTest";
    if (!registered) registered = RegisterClassExW(&wc) != 0;
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
}

static IDXGISwapChain1* make_chain(IDXGIFactory2* fac, ID3D12CommandQueue* q) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 64, sd.Height = 64, sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, sd.BufferCount = 2, sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    if (FAILED(fac->CreateSwapChainForHwnd(q, window(), &sd, nullptr, nullptr, &sc))) return nullptr;
    return sc;
}

static ID3D12CommandQueue* make_queue(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type) {
    D3D12_COMMAND_QUEUE_DESC qd{type, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue* q = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    return q;
}

// The chain of one frame in run 1's shape: the game calls Present through the other mod's copied vtable, where the Guide's slot hook
// sits; the other mod's function calls on to its own original, which (in this test) reaches a Guide hook behind it and then DXGI.
typedef HRESULT(STDMETHODCALLTYPE* PresentFn)(IDXGISwapChain1*, UINT, UINT);
static PresentFn g_dxgi_present, g_slot_original;
static int g_other_mod, g_guide_slot, g_guide_inline, g_draws;

static HRESULT STDMETHODCALLTYPE guide_inline(IDXGISwapChain1* sc, UINT a, UINT b) {
    chainfind::Once once;
    g_guide_inline++;
    if (once.first) g_draws++;
    return g_dxgi_present(sc, a, b);
}
static HRESULT STDMETHODCALLTYPE other_mod_present(IDXGISwapChain1* sc, UINT a, UINT b) {
    g_other_mod++;
    return guide_inline(sc, a, b);
}
static HRESULT STDMETHODCALLTYPE guide_slot(IDXGISwapChain1* sc, UINT a, UINT b) {
    chainfind::Once once;
    g_guide_slot++;
    if (once.first) g_draws++;
    return g_slot_original(sc, a, b);
}

int main() {
    ID3D12Device* dev = nullptr;
    IDXGIFactory4* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
        std::printf("swapchain: no Direct3D 12 device on this PC: nothing tested\n");
        return 2;
    }
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    ID3D12CommandQueue* q1 = make_queue(dev, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ID3D12CommandQueue* q2 = make_queue(dev, D3D12_COMMAND_LIST_TYPE_DIRECT);
    ID3D12CommandQueue* qc = make_queue(dev, D3D12_COMMAND_LIST_TYPE_COMPUTE);
    IDXGISwapChain1* probe = make_chain(fac, q1);
    IDXGISwapChain1* game = make_chain(fac, q2);
    CHECK(q1 && q2 && qc && probe && game);
    if (!probe || !game) return 1;
    const void* queue_vt = *(void* const*)q1;

    {   // run 3: where DXGI keeps the queue, learnt on the probe, finds the game's own queue (not the probe's)
        std::vector<long> offs = chainfind::queue_offsets(probe, q1);
        std::printf("swapchain: DXGI keeps the queue at");
        for (long o : offs) std::printf(" %s0x%lx", o < 0 ? "-" : "+", o < 0 ? -o : o);
        std::printf("%s\n", offs.empty() ? " no place found" : "");
        CHECK(!offs.empty());
        long at = -1;
        ID3D12CommandQueue* got = chainfind::queue_in_chain(game, offs, queue_vt, &at);
        CHECK(got == q2);
        if (got) got->Release();
        // the same on a WARP device (another device of the same runtime): its own queue, never the hardware one
        IDXGIAdapter* warp = nullptr;
        ID3D12Device* wdev = nullptr;
        if (SUCCEEDED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) && SUCCEEDED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&wdev)))) {
            ID3D12CommandQueue* qw = make_queue(wdev, D3D12_COMMAND_LIST_TYPE_DIRECT);
            IDXGISwapChain1* wchain = qw ? make_chain(fac, qw) : nullptr;
            CHECK(wchain);
            if (wchain) {
                ID3D12CommandQueue* wgot = chainfind::queue_in_chain(wchain, offs, queue_vt, &at);
                CHECK(wgot == qw);
                if (wgot) wgot->Release();
                wchain->Release();
            }
            if (qw) qw->Release();
            wdev->Release();
        }
        if (warp) warp->Release();
        // refused: a field that holds no queue object, and a vtable that is not a queue's
        CHECK(chainfind::queue_in_chain(game, std::vector<long>{0}, queue_vt, &at) == nullptr);
        CHECK(chainfind::queue_in_chain(game, offs, *(void* const*)dev, &at) == nullptr);
        CHECK(chainfind::queue_in_chain(game, offs, nullptr, &at) == nullptr);
        CHECK(chainfind::queue_offsets(probe, qc).empty());
    }

    void** real_vt = *(void***)game;
    void* dxgi_vt[chainfind::VT_SLOTS];
    memcpy(dxgi_vt, real_vt, sizeof dxgi_vt);
    CHECK(chainfind::first_foreign(real_vt, dxgi) == -1);
    CHECK(chainfind::cloned_vtable(game, dxgi_vt, real_vt) == nullptr);  // DXGI's own vtable: nothing to do
    CHECK(chainfind::cloned_vtable(&dxgi_vt[3], dxgi_vt, real_vt) == nullptr);  // not a swap chain

    {   // run 1 and run 2: another mod's copy of the vtable (like ShowFPS's), with its own Present
        void* clone[64];
        CHECK(chainfind::read(real_vt, clone, sizeof clone));
        clone[chainfind::PRESENT] = (void*)other_mod_present;
        *(void***)game = clone;
        CHECK(chainfind::cloned_vtable(game, dxgi_vt, real_vt) == clone);
        CHECK(chainfind::first_foreign(clone, dxgi) == chainfind::PRESENT);
        CHECK(chainfind::find_dxgi_vtable(clone, dxgi) == (void* const*)real_vt);  // run 2: DXGI's own Present found from the copy
        CHECK(chainfind::find_dxgi_vtable(clone, nullptr) == nullptr);

        // the Guide's hook in the copy's slot; one frame draws once although it passes two Guide hooks
        g_dxgi_present = (PresentFn)real_vt[chainfind::PRESENT];
        g_slot_original = (PresentFn)clone[chainfind::PRESENT];
        clone[chainfind::PRESENT] = (void*)guide_slot;
        HRESULT hr = game->Present(0, 0);
        CHECK(SUCCEEDED(hr));
        CHECK(g_guide_slot == 1 && g_other_mod == 1 && g_guide_inline == 1);
        CHECK(g_draws == 1);
        CHECK(chainfind::Once::depth() == 0);
        game->Present(0, 0);
        CHECK(g_draws == 2);
        *(void***)game = real_vt;
    }
    {   // a nested Once is not the first; the depth comes back to 0
        chainfind::Once a;
        {
            chainfind::Once b;
            CHECK(a.first && !b.first);
        }
        chainfind::Once c;
        CHECK(!c.first);
    }
    CHECK(chainfind::where(real_vt[chainfind::PRESENT]).rfind("dxgi.dll+0x", 0) == 0);

    game->Release(), probe->Release();
    qc->Release(), q2->Release(), q1->Release();
    dev->Release(), fac->Release();
    std::printf(failures ? "swapchain: %d failure(s)\n" : "swapchain: all cases hold\n", failures);
    return failures ? 1 : 0;
}
