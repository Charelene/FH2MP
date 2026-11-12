// D3DCompile for Dear ImGui's DX12 backend, taken from System32's d3dcompiler_47.dll. Next to azimuth.exe a file of that name is the
// FH2 Mod Platform's loader, so the overlay never loads it by name from the search path.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

typedef HRESULT(WINAPI* D3DCompileFn)(const void*, SIZE_T, const char*, const D3D_SHADER_MACRO*, ID3DInclude*, const char*, const char*, UINT,
                                      UINT, ID3DBlob**, ID3DBlob**);

extern "C" HRESULT WINAPI D3DCompile(const void* data, SIZE_T size, const char* name, const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                                     const char* entry, const char* target, UINT flags1, UINT flags2, ID3DBlob** code, ID3DBlob** errors) {
    static D3DCompileFn real = [] {
        HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? (D3DCompileFn)(void*)GetProcAddress(m, "D3DCompile") : nullptr;
    }();
    if (!real) return E_NOTIMPL;
    return real(data, size, name, defines, include, entry, target, flags1, flags2, code, errors);
}
