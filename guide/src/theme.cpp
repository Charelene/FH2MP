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
    {"xbox360", "Xbox 360", hex(0x008A00), hex(0x107C10), hex(0x0A4F0A), hex(0xEBEBEB), false},
    {"fh2mp", "FH2MP", hex(0xC9A0F7), hex(0xFF9F6E), hex(0x2E2A48), hex(0x1A1630), false},
    {"red", "Red", hex(0xC4262E), hex(0xFF6B6B), hex(0x5C0F13), hex(0xFFFFFF), true},
    {"orange", "Orange", hex(0xC2500A), hex(0xFFA347), hex(0x5E2604), hex(0xFFFFFF), true},
    {"yellow", "Yellow", hex(0xE0A100), hex(0xFFD84A), hex(0x5C4300), hex(0x14120A), true},
    {"green", "Green", hex(0x00875A), hex(0x47E0A0), hex(0x00402B), hex(0xFFFFFF), true},
    {"blue", "Blue", hex(0x1E6FD9), hex(0x5CB8FF), hex(0x0B3366), hex(0xFFFFFF), true},
    {"pink", "Pink", hex(0xC22872), hex(0xFF7DB8), hex(0x5E0D35), hex(0xFFFFFF), true},
};

// The surfaces: panel, pane, line, tab, raised, muted, muted2, muted3, tab text, disabled, key, key line; then the text on a panel, the open
// blade's name, the panel's gradient (0: flat), the toasts' surface (0: the panel) and the online mark; then a page's right-hand pane (its
// gradient, 0: the pane's colour; its text and secondary text, 0: the panel's), a gamer card's body (0: raised) and the light copy under a
// row's text (0: none). The dark themes leave all of those; Xbox 360 sets them.
struct Neutrals {
    ImU32 panel, pane, line, tab, raised, muted, muted2, muted3, tab_text, disabled, key, key_line;
    ImU32 text = IM_COL32(255, 255, 255, 255), spine_text = IM_COL32(255, 255, 255, 255), panel_hi = 0, panel_lo = 0, toast = 0,
          online = IM_COL32(0xe8, 0xe9, 0xeb, 255), side_hi = 0, side_lo = 0, side_text = 0, side_muted = 0, card = 0, emboss = 0;
};
// Xbox 360: the 360 Guide's own skin (huduiskin.xex): the blade's gradient 232,235,236 -> 201,207,209, the row hairlines 194,196,200, the
// side blades 91,104,117 with 235 text, XuiButtonGuide's Normal text 51,58,64 and Focus bar 0,138,0 with 235 text, its NormalDisable
// 185,189,194 and the 101,109,114 secondary text, the open blade's spine 205,208,211 -> 185,189,194 with its name in 88,96,102. The
// keyboard and the toasts stay dark. A page's right-hand pane is the Options pages' description pane (151,157,161 -> 105,110,115, its
// text 235), a card's body gamercard.xur's 227, and a row's text lies on XuiButtonGuide's 235 copy. Its bright green is dark enough to
// read on the light panels. Xbox is the dark theme it always was.
const Neutrals XBOX360 = {hex(0xd8ddde), hex(0xc9cfd1), hex(0xc2c4c8), hex(0x5b6875), hex(0xc3c7cb), hex(0x656d72),
                          hex(0x656d72), hex(0xc9ccd2), hex(0xebebeb), hex(0xb9bdc2), hex(0x2f3034), hex(0x48494e),
                          hex(0x333a40), hex(0x586066), hex(0xe8ebec), hex(0xc9cfd1), hex(0x2a2b2e), hex(0x333a40),
                          hex(0x979da1), hex(0x696e73), hex(0xebebeb), hex(0xc4c8cc), hex(0xe3e3e3), hex(0xebebeb)};
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
int g_layout = 0;

// The Xbox One guide's surfaces (measured from screenshots: the panel 1E1E1E, cards 2E2E2E, lines 303030, secondary text 9E9E9E).
const Neutrals ONE = {hex(0x1e1e1e), hex(0x262626), hex(0x303030), hex(0x2e2e2e), hex(0x2e2e2e), hex(0x9e9e9e),
                      hex(0x8c8c8c), hex(0xc9ccd2), hex(0xd4d6da), hex(0x5c5c5c), hex(0x2f3034), hex(0x48494e)};

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
    bool x360 = std::string(t.key) == "xbox360" && g_layout != 1;
    Neutrals n = g_layout == 1 ? ONE : plastic ? plastic_of(t.accent) : std::string(t.key) == "fh2mp" ? FH2MP : x360 ? XBOX360 : BASE;
    using namespace ui;
    ACCENT = t.accent, BRIGHT = t.bright, DARK = t.dark, INK = t.ink;
    PANEL = n.panel, PANE = n.pane, LINE = n.line, TAB = n.tab, RAISED = n.raised, MUTED = n.muted, MUTED2 = n.muted2, MUTED3 = n.muted3;
    TAB_TEXT = n.tab_text, DISABLED = n.disabled, KEY = n.key, KEY_LINE = n.key_line;
    TEXT = n.text, SPINE_TEXT = n.spine_text, PANEL_HI = n.panel_hi ? n.panel_hi : n.panel, PANEL_LO = n.panel_lo ? n.panel_lo : n.panel;
    TOAST = n.toast ? n.toast : n.panel, ONLINE = n.online;
    SIDE_HI = n.side_hi ? n.side_hi : n.pane, SIDE_LO = n.side_lo ? n.side_lo : n.pane, SIDE_TEXT = n.side_text ? n.side_text : n.text;
    SIDE_MUTED = n.side_muted ? n.side_muted : n.muted, CARD = n.card ? n.card : n.raised, EMBOSS = n.emboss;
    // the pad's keys: the 360 legend's discs and colours in its own theme, the ringed circles everywhere else
    KEY_DISCS = x360;
    RING_A = x360 ? IM_COL32(91, 169, 41, 255) : IM_COL32(0x5f, 0xd1, 0x1c, 255), RING_B = x360 ? IM_COL32(183, 51, 51, 255) : IM_COL32(0xff, 0x4a, 0x4f, 255);
    RING_X = x360 ? IM_COL32(35, 105, 160, 255) : IM_COL32(0x3c, 0x8c, 0xff, 255), RING_Y = x360 ? IM_COL32(227, 148, 2, 255) : IM_COL32(0xff, 0xb4, 0x1f, 255);



    g_current = plastic ? std::string(t.key) + "-plastic" : t.key;
}

const std::string& theme_current() { return g_current; }

void theme_layout(int layout) {
    g_layout = layout == 1 ? 1 : 0;
    theme_apply(std::string(g_current));
}
