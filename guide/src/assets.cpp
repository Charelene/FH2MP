// The Guide's fonts: Segoe UI (regular and bold) from Windows, the Xbox One toast's font from Game Bar or Windows, Material Icons (filled) embedded in the DLL. Sized like CSS: a font size of
// N gives N-pixel em glyphs (ImFontConfig::ExtraSizeScale = (ascender - descender) / em), so the canvas' pixel sizes can be used as they are.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "guide.h"

extern "C" const unsigned char _binary_MaterialIcons_Regular_ttf_start[], _binary_MaterialIcons_Regular_ttf_end[];

namespace {

// The hhea ascender and descender of a TrueType font, in em (head.unitsPerEm). False when the tables are missing.
bool ttf_metrics(const unsigned char* d, size_t n, FontMetrics* m) {
    auto u16 = [&](size_t o) -> unsigned { return o + 2 <= n ? unsigned(d[o]) << 8 | d[o + 1] : 0u; };
    auto u32 = [&](size_t o) -> size_t { return o + 4 <= n ? size_t(d[o]) << 24 | size_t(d[o + 1]) << 16 | size_t(d[o + 2]) << 8 | d[o + 3] : 0; };
    if (n < 12) return false;
    size_t head = 0, hhea = 0, os2 = 0;
    for (unsigned i = 0, tables = u16(4); i < tables; i++) {
        size_t r = 12 + 16 * size_t(i);
        if (r + 16 > n) break;
        if (!memcmp(d + r, "head", 4)) head = u32(r + 8);
        if (!memcmp(d + r, "hhea", 4)) hhea = u32(r + 8);
        if (!memcmp(d + r, "OS/2", 4)) os2 = u32(r + 8);
    }
    if (!head || !hhea) return false;
    unsigned em = u16(head + 18);
    int asc = int16_t(u16(hhea + 4)), desc = int16_t(u16(hhea + 6));
    if (!em || asc - desc <= 0) return false;
    m->asc = float(asc) / float(em), m->desc = float(-desc) / float(em);
    if (os2 && u16(os2) >= 2 && int16_t(u16(os2 + 88)) > 0) m->cap = float(int16_t(u16(os2 + 88))) / float(em);
    return true;
}

std::vector<unsigned char> read_file(const std::string& path) {
    std::vector<unsigned char> out;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    out.resize(GetFileSize(f, nullptr));
    DWORD got = 0;
    if (!ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr)) got = 0;
    out.resize(got);
    CloseHandle(f);
    return out;
}

ImFont* load_ttf(const std::string& path, FontMetrics* m) {
    std::vector<unsigned char> data = read_file(path);
    if (data.empty() || !ttf_metrics(data.data(), data.size(), m)) return nullptr;
    ImFontConfig cfg;
    cfg.ExtraSizeScale = m->asc + m->desc;
    // ImGui keeps its own copy (IM_ALLOC), so the file's bytes can go
    void* copy = IM_ALLOC(data.size());
    memcpy(copy, data.data(), data.size());
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(copy, int(data.size()), 26.0f, &cfg);
}

// Game Bar's folder of pictures and fonts on this PC, with its trailing backslash; "" where Game Bar is not installed.
std::string gamebar_assets() {
    const wchar_t* family = L"Microsoft.XboxGamingOverlay_8wekyb3d8bbwe";
    UINT32 count = 0, len = 0, path_len = MAX_PATH;
    GetPackagesByPackageFamily(family, &count, nullptr, &len, nullptr);  // the first call only says how much room is needed
    if (!count || !len) return "";
    std::vector<PWSTR> names(count);
    std::vector<wchar_t> buf(len);
    wchar_t path[MAX_PATH];
    if (GetPackagesByPackageFamily(family, &count, names.data(), &len, buf.data()) != ERROR_SUCCESS || !count ||
        GetPackagePathByFullName(names[0], &path_len, path) != ERROR_SUCCESS)
        return "";
    char ansi[MAX_PATH * 2];
    int n = WideCharToMultiByte(CP_ACP, 0, path, -1, ansi, sizeof ansi, nullptr, nullptr);
    return n > 1 ? std::string(ansi) + "\\Assets\\" : "";
}

}  // namespace

void load_fonts(GuideAssets& a) {
    char win[MAX_PATH];
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string fonts = std::string(win) + "\\Fonts\\";
    a.text = load_ttf(fonts + "segoeui.ttf", &a.text_m);
    if (!a.text) a.text = ImGui::GetIO().Fonts->AddFontDefault(), a.text_m = FontMetrics{};
    a.text_bold = load_ttf(fonts + "segoeuib.ttf", &a.bold_m);
    if (!a.text_bold) a.text_bold = a.text, a.bold_m = a.text_m;
    // The Xbox One toast's words: Game Bar draws its toasts in its own Segoe Sans Text, which is read from Game Bar's folder where
    // Game Bar is installed (it is never copied); Segoe UI Semibold is the nearest Windows itself has. GUIDE_GAMEBAR_ASSETS names the
    // folder for guide-preview.exe, as it does for the toast's pictures.
    const char* named = getenv("GUIDE_GAMEBAR_ASSETS");
    std::string bar = named ? std::string(named) + "\\" : gamebar_assets();
    if (!bar.empty()) a.text_one = load_ttf(bar + "Segoe-Sans-Text.ttf", &a.one_m);
    if (!a.text_one) a.text_one = load_ttf(fonts + "seguisb.ttf", &a.one_m);
    if (!a.text_one) a.text_one = a.text, a.one_m = a.text_m;
    const unsigned char* icons = _binary_MaterialIcons_Regular_ttf_start;
    size_t n = size_t(_binary_MaterialIcons_Regular_ttf_end - _binary_MaterialIcons_Regular_ttf_start);
    ImFontConfig icfg;
    icfg.FontDataOwnedByAtlas = false;
    if (ttf_metrics(icons, n, &a.icons_m)) icfg.ExtraSizeScale = a.icons_m.asc + a.icons_m.desc;
    a.icons = ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)icons, int(n), 32.0f, &icfg);
}
