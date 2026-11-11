// Direct3D 12 helpers shared by the overlay and the preview tool: pictures (PNG) into shader-visible textures.
#pragma once

#include <d3d12.h>

#include <vector>

// Decodes a PNG into RGBA (stb_image). False when it is not a picture stb_image reads.
bool decode_png(const unsigned char* data, size_t size, std::vector<unsigned char>* rgba, int* w, int* h);

// Copies RGBA pixels into a new texture through `queue` and waits for the copy; the texture's view goes to `srv`. The texture, or null.
ID3D12Resource* upload_texture(ID3D12Device* dev, ID3D12CommandQueue* queue, const unsigned char* rgba, int w, int h, D3D12_CPU_DESCRIPTOR_HANDLE srv);
