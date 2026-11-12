#define WIN32_LEAN_AND_MEAN
#include "gfx.h"

#include <windows.h>

#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

bool decode_png(const unsigned char* data, size_t size, std::vector<unsigned char>* rgba, int* w, int* h) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory(data, int(size), w, h, &n, 4);
    if (!px) return false;
    rgba->assign(px, px + size_t(*w) * size_t(*h) * 4);
    stbi_image_free(px);
    return true;
}

ID3D12Resource* upload_texture(ID3D12Device* dev, ID3D12CommandQueue* queue, const unsigned char* rgba, int w, int h, D3D12_CPU_DESCRIPTOR_HANDLE srv) {
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT}, up{D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, td.Width = UINT64(w), td.Height = UINT(h), td.DepthOrArraySize = 1, td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
    UINT pitch = (UINT(w) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, bd.Width = UINT64(pitch) * UINT64(h), bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1;
    bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *tex = nullptr, *upload = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    bool ok = SUCCEEDED(dev->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))) &&
              SUCCEEDED(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))) &&
              SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) &&
              SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    if (ok) {
        void* map = nullptr;
        D3D12_RANGE none{0, 0};
        ok = SUCCEEDED(upload->Map(0, &none, &map));
        if (ok) {
            for (int y = 0; y < h; y++) memcpy((char*)map + size_t(y) * pitch, rgba + size_t(y) * size_t(w) * 4, size_t(w) * 4);
            upload->Unmap(0, nullptr);
        }
    }
    if (ok) {
        D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, src{upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        dst.SubresourceIndex = 0;
        src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, UINT(w), UINT(h), 1, pitch};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &b);
        list->Close();
        ID3D12CommandList* lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        queue->Signal(fence, 1);
        if (fence->GetCompletedValue() < 1) {
            fence->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, 2000);
        }
        CloseHandle(ev);
        ok = fence->GetCompletedValue() >= 1;
    }
    if (ok) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM, sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dev->CreateShaderResourceView(tex, &sd, srv);
    }
    if (fence) fence->Release();
    if (list) list->Release();
    if (alloc) alloc->Release();
    if (upload) upload->Release();
    if (!ok && tex) tex->Release(), tex = nullptr;
    return tex;
}
