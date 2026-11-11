// guide-preview.exe: draws the Guide off screen with the same code the overlay uses (guide.cpp, assets.cpp, Dear ImGui's DX12 backend) and
// writes a PNG, so the look can be checked against the design canvas without a game.
//   guide-preview.exe <out.png> <scene> [seconds] [gamerpic.png] [background.png]
// Scenes: home (the Guide, the player's blade), games, settings, down (focus moved down one row), toast-info, toast-invite.
// seconds: time since the Guide opened or the toast started (default 1.5 and 3.0).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backends/imgui_impl_dx12.h"
#include "gfx.h"
#include "guide.h"
#include "imgui.h"

namespace {

const UINT W = 1920, H = 1080, SRVS = 64;
ID3D12Device* g_dev;
ID3D12DescriptorHeap* g_srv;
UINT g_srv_size, g_srv_next;

void srv_alloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
    UINT i = g_srv_next++;
    cpu->ptr = g_srv->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(i) * g_srv_size;
    gpu->ptr = g_srv->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(i) * g_srv_size;
}
void srv_free(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {}

std::vector<unsigned char> read_all(const char* path) {
    std::vector<unsigned char> out;
    FILE* f = fopen(path, "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    out.resize(size_t(ftell(f)));
    fseek(f, 0, SEEK_SET);
    out.resize(fread(out.data(), 1, out.size(), f));
    fclose(f);
    return out;
}

ImTextureID picture(ID3D12CommandQueue* q, const char* path) {
    std::vector<unsigned char> png = read_all(path), rgba;
    int w, h;
    if (png.empty() || !decode_png(png.data(), png.size(), &rgba, &w, &h)) return 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    srv_alloc(nullptr, &cpu, &gpu);
    return upload_texture(g_dev, q, rgba.data(), w, h, cpu) ? (ImTextureID)gpu.ptr : 0;
}

bool save_png(const char* path, const unsigned char* bgra, UINT stride) {
    IWICImagingFactory* f = nullptr;
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    wchar_t wpath[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) &&
              SUCCEEDED(st->InitializeFromFilename(wpath, GENERIC_WRITE)) && SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) &&
              SUCCEEDED(fr->SetSize(W, H)) && SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
              SUCCEEDED(fr->WritePixels(H, stride, stride * H, (BYTE*)bgra)) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (f) f->Release();
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: guide-preview <out.png> <scene> [seconds] [gamerpic.png] [background.png]\n");
        return 2;
    }
    std::string scene = argv[2];
    bool is_toast = scene.rfind("toast", 0) == 0;
    double secs = argc > 3 ? atof(argv[3]) : (is_toast ? 3.0 : 1.5);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dev))) || FAILED(g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)))) {
        fprintf(stderr, "no Direct3D 12 device\n");
        return 1;
    }
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, rb{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, td.Width = W, td.Height = H, td.DepthOrArraySize = 1, td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1, td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{DXGI_FORMAT_B8G8R8A8_UNORM, {0.16f, 0.2f, 0.22f, 1}};
    ID3D12Resource *rt = nullptr, *readback = nullptr;
    UINT pitch = (W * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = UINT64(pitch) * H, bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1;
    bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    g_dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt));
    g_dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC rd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    D3D12_DESCRIPTOR_HEAP_DESC sd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SRVS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    g_dev->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtv_heap));
    g_dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&g_srv));
    g_srv_size = g_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    g_dev->CreateRenderTargetView(rt, nullptr, rtv);
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto wait = [&] {
        q->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) fence->SetEventOnCompletion(fv, ev), WaitForSingleObject(ev, 5000);
    };

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    GuideAssets assets;
    load_fonts(assets);
    ImGui_ImplDX12_InitInfo info;
    info.Device = g_dev, info.CommandQueue = q, info.NumFramesInFlight = 1, info.RTVFormat = DXGI_FORMAT_B8G8R8A8_UNORM, info.SrvDescriptorHeap = g_srv;
    info.SrvDescriptorAllocFn = srv_alloc, info.SrvDescriptorFreeFn = srv_free;
    if (!ImGui_ImplDX12_Init(&info)) return 1;
    if (argc > 4) assets.gamerpic = picture(q, argv[4]);
    ImTextureID bg = argc > 5 ? picture(q, argv[5]) : 0;

    Guide g;
    g.gamertag = "DebugFox";
    const double t0 = 100.0;
    if (is_toast) {
        if (scene == "toast-invite") g.toast(ToastKind::Invite, "Game invite sent", "To DebugWolf", 0xea28, assets.gamerpic, t0);
        else g.toast(ToastKind::Info, "Notifications", "Coming in a later build", 0xe7f4, 0, t0);
    } else {
        g.toggle(t0 - 10);
        if (scene == "games") g.handle(PadEvent::Lb, t0 - 5);
        if (scene == "settings") g.handle(PadEvent::Rb, t0 - 5);
        if (scene == "down") g.handle(PadEvent::Down, t0 - 5);
        g.toggle(t0 - 1), g.toggle(t0);  // closed and opened again: the open animation starts at t0
    }

    for (int frame = 0; frame < 2; frame++) {
        io.DisplaySize = ImVec2(float(W), float(H));
        io.DeltaTime = 1.0f / 60;
        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        if (bg) ImGui::GetBackgroundDrawList()->AddImage(bg, ImVec2(0, 0), ImVec2(float(W), float(H)));
        g.draw(ImGui::GetForegroundDrawList(), assets, float(W), float(H), t0 + secs);
        ImGui::Render();
        alloc->Reset();
        list->Reset(alloc, nullptr);
        list->ClearRenderTargetView(rtv, cv.Color, 0, nullptr);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetDescriptorHeaps(1, &g_srv);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), list);
        if (frame == 1) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = rt, b.Transition.Subresource = 0;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET, b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            list->ResourceBarrier(1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}, src{rt, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            dst.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, W, H, 1, pitch};
            src.SubresourceIndex = 0;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        list->Close();
        ID3D12CommandList* lists[] = {list};
        q->ExecuteCommandLists(1, lists);
        wait();
    }
    void* map = nullptr;
    D3D12_RANGE all{0, SIZE_T(pitch) * H};
    readback->Map(0, &all, &map);
    std::vector<unsigned char> px((size_t)pitch * H);
    memcpy(px.data(), map, px.size());
    readback->Unmap(0, nullptr);
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (!save_png(argv[1], px.data(), pitch)) {
        fprintf(stderr, "could not write %s\n", argv[1]);
        return 1;
    }
    printf("wrote %s (%s at %.2f s)\n", argv[1], scene.c_str(), secs);
    ImGui_ImplDX12_Shutdown();
    ImGui::DestroyContext();
    return 0;
}
