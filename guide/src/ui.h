// Drawing helpers shared by the Guide's screens (guide.cpp, community.cpp, messages.cpp): the canvas' colours and icons, CSS-like text
// placement, the hint bar, option lists. Everything is laid out in the canvas' 1280x720 units, scaled to the game's frame and centred;
// the Guide is drawn at 0.84 of the canvas and a little higher, so a toast at the canvas' place (y 600) shows under it.
#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "guide.h"

namespace ui {

// The theme's colours (theme.cpp sets them; these are Xbox's, the 360 Guide's own skin): the accent, a bright and a dark of it, the ink for
// text on the accent, and the surfaces. TEXT is the text on a panel, SPINE_TEXT the open blade's name, PANEL_HI and PANEL_LO the panel's
// gradient top and bottom, TOAST the toasts' surface (they stay dark in every theme). White, black and the offline colour stay.
inline ImU32 ACCENT = IM_COL32(0x00, 0x8a, 0x00, 255), DARK = IM_COL32(0x0a, 0x4f, 0x0a, 255), BRIGHT = IM_COL32(0x10, 0x7c, 0x10, 255),
             INK = IM_COL32(235, 235, 235, 255), PANEL = IM_COL32(216, 221, 222, 255), PANE = IM_COL32(201, 207, 209, 255),
             LINE = IM_COL32(194, 196, 200, 255), TAB = IM_COL32(91, 104, 117, 255), RAISED = IM_COL32(195, 199, 203, 255),
             MUTED = IM_COL32(101, 109, 114, 255), MUTED2 = IM_COL32(101, 109, 114, 255), TAB_TEXT = IM_COL32(235, 235, 235, 255),
             MUTED3 = IM_COL32(0xc9, 0xcc, 0xd2, 255), DISABLED = IM_COL32(185, 189, 194, 255), KEY = IM_COL32(0x2f, 0x30, 0x34, 255),
             KEY_LINE = IM_COL32(0x48, 0x49, 0x4e, 255), TEXT = IM_COL32(51, 58, 64, 255), SPINE_TEXT = IM_COL32(88, 96, 102, 255),
             PANEL_HI = IM_COL32(232, 235, 236, 255), PANEL_LO = IM_COL32(201, 207, 209, 255), TOAST = IM_COL32(0x2a, 0x2b, 0x2e, 255),
             ONLINE = IM_COL32(51, 58, 64, 255);
const ImU32 WHITE = IM_COL32(255, 255, 255, 255), BLACK = IM_COL32(0, 0, 0, 255), OFFLINE = IM_COL32(0xe0, 0x55, 0x2f, 255);
const ImU32 PIC_COLORS[] = {IM_COL32(0xc9, 0x66, 0x2f, 255), IM_COL32(0x3f, 0x6f, 0xc4, 255), IM_COL32(0xc4, 0x9a, 0x2b, 255),
                            IM_COL32(0x3e, 0x9a, 0x5a, 255), IM_COL32(0x2f, 0x8f, 0xa8, 255), IM_COL32(0x9a, 0x4f, 0xb8, 255),
                            IM_COL32(0xa8, 0x3a, 0x3a, 255)};
const ImU32 RING_A = IM_COL32(0x5f, 0xd1, 0x1c, 255), RING_B = IM_COL32(0xff, 0x4a, 0x4f, 255), RING_X = IM_COL32(0x3c, 0x8c, 0xff, 255),
            RING_Y = IM_COL32(0xff, 0xb4, 0x1f, 255);

// The Guide's size against the canvas and the centre it is drawn around (canvas units); a page's left edge and width.
const float GUIDE_K = 0.84f, GUIDE_CY = 330, PAGE_L = 205, PAGE_W = 862;

// Material Icons (filled).
enum : uint32_t {
    I_PERSON = 0xe7fd, I_GROUPS = 0xf233, I_MAIL = 0xe158, I_DRAFTS = 0xe151, I_SENSORS = 0xe51e, I_CHAT = 0xe0ca, I_GROUP_ADD = 0xe7f0,
    I_GAMES = 0xea28, I_TROPHY = 0xea23, I_PALETTE = 0xe40a, I_MIC = 0xe029, I_BELL = 0xe7f4, I_SETTINGS = 0xe8b8, I_FLAG = 0xe153,
    I_SCHEDULE = 0xe8b5, I_EXPAND_LESS = 0xe5ce, I_EXPAND_MORE = 0xe5cf, I_APPS = 0xe5c3, I_WIFI = 0xe63e, I_PERSON_ADD = 0xe7fe,
    I_STAR = 0xe838, I_VOLUME = 0xe050, I_SEND = 0xe163, I_EDIT = 0xe3c9, I_CHECK_BOX = 0xe834, I_CHECK_BLANK = 0xe835, I_PLAY_CIRCLE = 0xe1c4, I_CAR = 0xe531
};

inline void utf8(uint32_t cp, char out[5]) {
    if (cp < 0x80) {
        out[0] = char(cp), out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = char(0xc0 | (cp >> 6)), out[1] = char(0x80 | (cp & 0x3f)), out[2] = 0;
    } else if (cp < 0x10000) {
        out[0] = char(0xe0 | (cp >> 12)), out[1] = char(0x80 | ((cp >> 6) & 0x3f)), out[2] = char(0x80 | (cp & 0x3f)), out[3] = 0;
    } else {
        out[0] = char(0xf0 | (cp >> 18)), out[1] = char(0x80 | ((cp >> 12) & 0x3f)), out[2] = char(0x80 | ((cp >> 6) & 0x3f)), out[3] = char(0x80 | (cp & 0x3f)), out[4] = 0;
    }
}

inline std::string utf8(const std::u32string& t) {
    std::string o;
    char b[5];
    for (char32_t c : t) utf8(uint32_t(c), b), o += b;
    return o;
}

inline std::u32string utf32(const std::string& t) {
    std::u32string o;
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
        uint32_t cp = n == 1 ? c : c & (0xff >> (n + 1));
        for (int k = 1; k < n && i + k < t.size(); k++) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3f);
        o += char32_t(cp), i += n;
    }
    return o;
}

inline ImU32 alpha(ImU32 c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    unsigned int al = (c >> IM_COL32_A_SHIFT) & 0xff;
    return (c & ~IM_COL32_A_MASK) | (ImU32(al * a + 0.5f) << IM_COL32_A_SHIFT);
}

// A panel: the theme's gradient from PANEL_HI at the top to PANEL_LO at the bottom (one colour in the dark themes).
inline void panel_fill(ImDrawList* dl, ImVec2 p0, ImVec2 p1, float op) {
    ImU32 hi = alpha(PANEL_HI, op), lo = alpha(PANEL_LO, op);
    dl->AddRectFilledMultiColor(p0, p1, hi, hi, lo, lo);
}

inline float ease_guide(float t) { return css_bezier(0.1f, 0.9f, 0.2f, 1.0f, t); }

// Where AddText must start (y) so that a line box centred on cy has its baseline where CSS puts it: cy + (ascent - descent) / 2 em,
// whatever the line-height (the half-leading is shared out evenly). ImGui puts the baseline ascent / (ascent + descent) of the size down.
inline float css_top(const FontMetrics& m, float size, float cy) { return cy + size * (m.asc - m.desc) * 0.5f - size * m.asc / (m.asc + m.desc); }

// Where AddText must start so that the capital letters are centred on cy (a word next to a round key looks centred on it).
inline float caps_top(const FontMetrics& m, float size, float cy) { return cy + size * m.cap * 0.5f - size * m.asc / (m.asc + m.desc); }

// Text in a line box centred on cy (CSS align-items: center), starting at x or, with right, ending there.
inline void text_mid(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float x, float cy, ImU32 col, const char* text, bool right = false) {
    float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    dl->AddText(f, size, ImVec2(right ? x - w : x, css_top(m, size, cy)), col, text);
}

// The same, cut with an ellipsis to fit max_w (CSS text-overflow: ellipsis).
inline void text_fit(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float x, float cy, float max_w, ImU32 col, const std::string& text) {
    if (f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_w) return text_mid(dl, f, m, size, x, cy, col, text.c_str());
    std::string t = text;
    while (!t.empty() && f->CalcTextSizeA(size, FLT_MAX, 0.0f, (t + "...").c_str()).x > max_w) {
        t.pop_back();
        while (!t.empty() && (t.back() & 0xc0) == 0x80) t.pop_back();
    }
    text_mid(dl, f, m, size, x, cy, col, (t + "...").c_str());
}

inline void icon_mid(ImDrawList* dl, const GuideAssets& a, float size, float cx, float cy, ImU32 col, uint32_t icon) {
    if (!a.icons || !icon) return;
    char ic[5];
    utf8(icon, ic);
    float w = a.icons->CalcTextSizeA(size, FLT_MAX, 0, ic).x;
    dl->AddText(a.icons, size, ImVec2(cx - w * 0.5f, css_top(a.icons_m, size, cy)), col, ic);
}

// CSS writing-mode: vertical-rl: the text runs down from top, turned a quarter clockwise, its column centred on cx. Laid out without a
// clip rect (ImGui drops glyphs outside the clip rect before they are turned, which cut the blade names short).
inline void vertical_text(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float cx, float top, ImU32 col, const char* text) {
    float baseline_x = cx - size * (m.asc - m.desc) * 0.5f;  // the ascent side faces right
    ImVec2 at(baseline_x + size * m.asc / (m.asc + m.desc), top);
    int from = dl->VtxBuffer.Size;
    dl->PushClipRectFullScreen();
    dl->AddText(f, size, at, col, text);
    dl->PopClipRect();
    for (int i = from; i < dl->VtxBuffer.Size; i++) {
        ImVec2 p = dl->VtxBuffer[i].pos;
        float dx = p.x - at.x, dy = p.y - at.y;
        dl->VtxBuffer[i].pos = ImVec2(at.x - dy, at.y + dx);
    }
}

// A pad key's badge: a circle (A, B, X, Y: 20 units) or a rounded box (LB, RB, LT, RT, L: w x 22) with a 2-unit ring inside its edge,
// centred on cy, its letters centred by their capital height. s: pixels per unit. Returns its width in pixels.
inline float badge(ImDrawList* dl, const GuideAssets& a, const char* key, ImU32 ring, float x, float cy, float s, float op, float w = 0, float r = -1) {
    bool wide = strlen(key) > 1;
    float kw = (w > 0 ? w : wide ? 34 : 20) * s, kh = (wide || w > 0 ? 22 : 20) * s, rr = (r >= 0 ? std::min(r, 11.0f) : wide ? 6 : 10) * s;
    ImVec2 k0(x, cy - kh / 2), k1(x + kw, cy + kh / 2);
    bool disc = !wide && w <= 0;  // A, B, X, Y: the 360's legend draws a disc filled with the button's colour and a light letter
    dl->AddRectFilled(k0, k1, alpha(disc ? ring : BLACK, op), rr);
    if (!disc) dl->AddRect(ImVec2(k0.x + s, k0.y + s), ImVec2(k1.x - s, k1.y - s), alpha(ring, op), std::max(0.0f, rr - s), 0, 2 * s);
    ImFont* kf = a.text_bold ? a.text_bold : a.text;
    float size = (wide || w > 0 ? 11 : 12) * s;
    float tw = kf->CalcTextSizeA(size, FLT_MAX, 0, key).x;
    dl->AddText(kf, size, ImVec2(x + (kw - tw) * 0.5f, caps_top(a.bold_m, size, cy)), alpha(disc ? IM_COL32(245, 245, 245, 255) : ring, op), key);
    return kw;
}

// Canvas units to pixels: the frame's scale, the Guide's size (GUIDE_K around (640, GUIDE_CY)), then its open zoom around that centre.
struct View {
    ImVec2 origin;
    float s, zoom;
    ImVec2 pt(float x, float y) const {
        float k = GUIDE_K * zoom;
        return ImVec2(origin.x + (640 + (x - 640) * k) * s, origin.y + (GUIDE_CY + (y - 360) * k) * s);
    }
    float sz(float v) const { return v * s * zoom * GUIDE_K; }
};

struct Hint {
    const char* key;
    const char* label;
    ImU32 ring;
};

// The hint bar: a flex row 10 units under the panel, 27 in, 19-unit text, gaps 22 between hints and 7 inside one, a hint without a label
// sits 8 from the next (LB RB). The keys and the words share one centre line (the words by their capitals).
inline void hint_bar(ImDrawList* dl, const GuideAssets& a, const View& v, float x, float top, const Hint* hints, int n, float op) {
    float cy = top + 12.5f;
    for (int i = 0; i < n; i++) {
        ImVec2 p = v.pt(x, cy);
        x += badge(dl, a, hints[i].key, hints[i].ring, p.x, p.y, v.sz(1), op) / v.sz(1);
        if (hints[i].label) {
            x += 7;
            ImVec2 q = v.pt(x, cy);
            dl->AddText(a.text, v.sz(19), ImVec2(q.x, caps_top(a.text_m, v.sz(19), q.y)), alpha(WHITE, op), hints[i].label);
            x += a.text->CalcTextSizeA(19, FLT_MAX, 0, hints[i].label).x + 22;
        } else {
            x += 8;
        }
    }
}

// A player's picture, or the canvas' stand-in: a coloured square with a person's shape rising from its bottom edge.
inline void player_pic(ImDrawList* dl, const GuideAssets& a, ImTextureID pic, uint64_t xuid, ImVec2 p0, ImVec2 p1, float op) {
    if (pic) {
        dl->AddImage(pic, p0, p1, ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, op));
        return;
    }
    dl->AddRectFilled(p0, p1, alpha(PIC_COLORS[xuid % (sizeof PIC_COLORS / sizeof PIC_COLORS[0])], op));
    float side = p1.x - p0.x;
    dl->PushClipRect(p0, p1, true);
    icon_mid(dl, a, side * 1.03f, (p0.x + p1.x) * 0.5f, p1.y - side * 0.38f, alpha(IM_COL32(0, 0, 0, 140), op), I_PERSON);
    dl->PopClipRect();
}

// The scroll arrows at the bottom right of a list: up when rows are above, down when rows are below.
inline void scroll_marks(ImDrawList* dl, const GuideAssets& a, const View& v, float right, float cy, bool up, bool down, float op) {
    ImVec2 p = v.pt(right, cy);
    if (down) icon_mid(dl, a, v.sz(28), p.x - v.sz(14), p.y, alpha(MUTED, op), I_EXPAND_MORE);
    if (up) icon_mid(dl, a, v.sz(28), p.x - v.sz(42), p.y, alpha(MUTED, op), I_EXPAND_LESS);
}

// The first entry the focus can rest on, or -1 when every entry is greyed.
inline int first_enabled(const std::vector<Opt>& o) {
    for (size_t i = 0; i < o.size(); i++)
        if (o[i].enabled) return int(i);
    return -1;
}

// The focus one step up or down from cur, passing over greyed entries; cur when there is none that way.
inline int step_focus(const std::vector<Opt>& o, int cur, int dir) {
    for (int i = cur + dir; i >= 0 && i < int(o.size()); i += dir)
        if (o[i].enabled) return i;
    return cur >= 0 && cur < int(o.size()) && o[cur].enabled ? cur : first_enabled(o);
}

// An option list as the canvas draws them: rows row_h high from (x, top), w wide, the accent bar on the focused row (bar: its row,
// gliding), a line under every other row, greyed entries in the disabled colour. icon_size > 0 puts each row's icon in a column icon_w
// wide before its label (Create New, compose).
inline void option_list(ImDrawList* dl, const GuideAssets& a, const View& v, float x, float top, float w, float row_h, float text_size, float pad,
                        const std::vector<Opt>& opts, int focus, float bar, float op, float icon_size = 0, float icon_w = 0, float gap = 0) {
    if (focus >= 0 && focus < int(opts.size()) && opts[focus].enabled) {
        float by = top + bar * row_h;
        ImVec2 b0 = v.pt(x, by), b1 = v.pt(x + w, by + row_h);
        dl->AddRectFilled(b0, b1, alpha(ACCENT, op));
        dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, op), v.sz(1));
    }
    for (size_t i = 0; i < opts.size(); i++) {
        float y = top + i * row_h, cy = y + row_h * 0.5f;
        if (int(i) != focus) dl->AddLine(v.pt(x, y + row_h - 0.5f), v.pt(x + w, y + row_h - 0.5f), alpha(LINE, op), v.sz(1));
        ImU32 fg = alpha(!opts[i].enabled ? DISABLED : int(i) == focus ? INK : TEXT, op);

        float tx = x + pad;
        if (icon_size > 0) {
            ImVec2 ip = v.pt(tx + icon_w * 0.5f, cy);
            icon_mid(dl, a, v.sz(icon_size), ip.x, ip.y, fg, opts[i].icon);
            tx += icon_w + gap;
        }
        ImVec2 p = v.pt(tx, cy);
        text_fit(dl, a.text, a.text_m, v.sz(text_size), p.x, p.y, v.sz(x + w - pad - tx), fg, opts[i].label);
    }
}

// Text wrapped to max_w (canvas units) from (x, y), line-height lh; returns the height it took.
inline float wrapped(ImDrawList* dl, const GuideAssets& a, const View& v, float size, float lh, float x, float y, float max_w, ImU32 col, const std::string& text) {
    // ImGui lays lines out size apart; draw line by line for the canvas' line-height
    const char* p = text.c_str();
    const char* end = p + text.size();
    float used = 0;
    while (p < end) {
        const char* brk = a.text->CalcWordWrapPosition(size, p, end, max_w);
        if (brk == p) brk = p + 1;
        const char* stop = brk;
        for (const char* q = p; q < brk; q++)
            if (*q == '\n') { stop = q; brk = q + 1; break; }
        ImVec2 at = v.pt(x, y + used + size * lh * 0.5f);
        dl->AddText(a.text, v.sz(size), ImVec2(at.x, css_top(a.text_m, v.sz(size), at.y)), col, p, stop);
        used += size * lh;
        p = brk;
        while (p < end && *p == ' ') p++;
    }
    return used;
}

inline bool same_game(const Friend& f, const Friend& me) { return !me.lobby_id.empty() && f.lobby_id == me.lobby_id; }

inline std::string lobby_words(const Friend& f) {
    if (!f.lobby_status.empty()) return f.lobby_status;
    if (f.lobby_kind == "freeroam") return "Free Roam";
    if (f.lobby_kind == "roadtrip") return "Road Trip";
    return f.lobby_kind;
}

// The 360's ages: "5 minutes ago", "1 hour ago", "yesterday", "3 days ago".
inline std::string ago(int secs, const char* lead = "") {
    std::string l = lead;
    if (secs < 0) return "";
    if (secs < 120) return l + "1 minute ago";
    if (secs < 3600) return l + std::to_string(secs / 60) + " minutes ago";
    if (secs < 7200) return l + "1 hour ago";
    if (secs < 86400) return l + std::to_string(secs / 3600) + " hours ago";
    if (secs < 2 * 86400) return l + "yesterday";
    return l + std::to_string(secs / 86400) + " days ago";
}

// The 360's expiry: "expires in 8 minutes".
inline std::string expires(int secs) {
    if (secs <= 0) return "expired";
    if (secs < 120) return "expires in 1 minute";
    if (secs < 3600) return "expires in " + std::to_string(secs / 60) + " minutes";
    if (secs < 7200) return "expires in 1 hour";
    if (secs < 86400) return "expires in " + std::to_string(secs / 3600) + " hours";
    if (secs < 2 * 86400) return "expires tomorrow";
    return "expires in " + std::to_string(secs / 86400) + " days";
}

}  // namespace ui
