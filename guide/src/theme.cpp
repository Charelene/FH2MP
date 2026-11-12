// The Guide's themes (theme.h): the canvas' presets and the plastic tint, worked out as its Themes board does (HSL of the accent, the
// saturation capped at 0.62, each surface at its own lightness).
#include "theme.h"

#include <algorithm>
#include <cmath>

#include "ui.h"

namespace {

ImU32 hex(unsigned v) { return IM_COL32((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff, 255); }

const std::vector<ThemeDef> g_presets = {
    {"xbox", "Xbox", hex(0x108A10), hex(0x8ED61E), hex(0x0A4F0A), hex(0xFFFFFF), false},
    {"fh2mp", "FH2MP", hex(0xC9A0F7), hex(0xFF9F6E), hex(0x2E2A48), hex(0x1A1630), false},
    {"red", "Red", hex(0xC4262E), hex(0xFF6B6B), hex(0x5C0F13), hex(0xFFFFFF), true},
    {"orange", "Orange", hex(0xC2500A), hex(0xFFA347), hex(0x5E2604), hex(0xFFFFFF), true},
    {"yellow", "Yellow", hex(0xE0A100), hex(0xFFD84A), hex(0x5C4300), hex(0x14120A), true},
    {"green", "Green", hex(0x00875A), hex(0x47E0A0), hex(0x00402B), hex(0xFFFFFF), true},
    {"blue", "Blue", hex(0x1E6FD9), hex(0x5CB8FF), hex(0x0B3366), hex(0xFFFFFF), true},
    {"pink", "Pink", hex(0xC22872), hex(0xFF7DB8), hex(0x5E0D35), hex(0xFFFFFF), true},
};

// The surfaces: panel, pane, line, tab, raised, muted, muted2, muted3, tab text, disabled, key, key line.
struct Neutrals {
    ImU32 panel, pane, line, tab, raised, muted, muted2, muted3, tab_text, disabled, key, key_line;
};
const Neutrals BASE = {hex(0x2a2b2e), hex(0x1e1f22), hex(0x3a3b3f), hex(0x45474c), hex(0x34363a), hex(0xb4b6bb),
                       hex(0x9c9fa5), hex(0xc9ccd2), hex(0xd4d6da), hex(0x6e7076), hex(0x2f3034), hex(0x48494e)};
const Neutrals FH2MP = {hex(0x242740), hex(0x1f2134), hex(0x31344e), hex(0x363a54), hex(0x2a2e4a), hex(0xb5bad8),
                        hex(0xa3a8c8), hex(0xc9cdea), hex(0xd5d9f5), hex(0x6e7394), hex(0x2f3350), hex(0x3a3e5c)};

// The canvas' plastic neutrals: the accent's hue, its saturation capped at 0.62 (or less where a surface asks for less).
Neutrals plastic_of(ImU32 accent) {
    float r = ((accent >> IM_COL32_R_SHIFT) & 0xff) / 255.0f, g = ((accent >> IM_COL32_G_SHIFT) & 0xff) / 255.0f,
          b = ((accent >> IM_COL32_B_SHIFT) & 0xff) / 255.0f;
    float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn, h = 0, l = (mx + mn) / 2;
    float s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1));
    if (d != 0) {
        h = mx == r ? std::fmod((g - b) / d, 6.0f) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        h *= 60;
        if (h < 0) h += 360;
    }
    float S = std::min(s, 0.62f);
    auto c = [&](float L, float sat) {
        float ss = sat < 0 ? S : std::min(sat, s);
        float C = (1 - std::fabs(2 * L - 1)) * ss, X = C * (1 - std::fabs(std::fmod(h / 60, 2.0f) - 1)), m = L - C / 2;
        float rgb[3];
        if (h < 60) rgb[0] = C, rgb[1] = X, rgb[2] = 0;
        else if (h < 120) rgb[0] = X, rgb[1] = C, rgb[2] = 0;
        else if (h < 180) rgb[0] = 0, rgb[1] = C, rgb[2] = X;
        else if (h < 240) rgb[0] = 0, rgb[1] = X, rgb[2] = C;
        else if (h < 300) rgb[0] = X, rgb[1] = 0, rgb[2] = C;
        else rgb[0] = C, rgb[1] = 0, rgb[2] = X;
        auto q = [&](float v) { return int(std::lround(std::clamp(v + m, 0.0f, 1.0f) * 255)); };
        return IM_COL32(q(rgb[0]), q(rgb[1]), q(rgb[2]), 255);
    };
    return {c(0.17f, -1), c(0.11f, -1), c(0.26f, -1), c(0.31f, -1), c(0.22f, -1), c(0.8f, 0.45f),
            c(0.72f, 0.4f), c(0.85f, 0.45f), c(0.88f, 0.5f), c(0.5f, 0.3f), c(0.2f, -1), c(0.35f, -1)};
}

std::string g_current = "xbox";

}  // namespace

const std::vector<ThemeDef>& theme_presets() { return g_presets; }

bool theme_plastic(const std::string& key) {
    const std::string tail = "-plastic";
    return key.size() > tail.size() && key.compare(key.size() - tail.size(), tail.size(), tail) == 0;
}

const ThemeDef& theme_preset(const std::string& key) {
    std::string base = theme_plastic(key) ? key.substr(0, key.size() - 8) : key;
    for (const ThemeDef& t : g_presets)
        if (base == t.key && (!theme_plastic(key) || t.plastic_too)) return t;
    return g_presets[0];
}

std::string theme_name(const std::string& key) {
    const ThemeDef& t = theme_preset(key);
    return std::string(t.name) + (theme_plastic(key) && t.plastic_too ? " Plastic" : "");
}

void theme_apply(const std::string& key) {
    const ThemeDef& t = theme_preset(key);
    bool plastic = theme_plastic(key) && t.plastic_too;
    Neutrals n = plastic ? plastic_of(t.accent) : std::string(t.key) == "fh2mp" ? FH2MP : BASE;
    using namespace ui;
    ACCENT = t.accent, BRIGHT = t.bright, DARK = t.dark, INK = t.ink;
    PANEL = n.panel, PANE = n.pane, LINE = n.line, TAB = n.tab, RAISED = n.raised, MUTED = n.muted, MUTED2 = n.muted2, MUTED3 = n.muted3;
    TAB_TEXT = n.tab_text, DISABLED = n.disabled, KEY = n.key, KEY_LINE = n.key_line;
    g_current = plastic ? std::string(t.key) + "-plastic" : t.key;
}

const std::string& theme_current() { return g_current; }
