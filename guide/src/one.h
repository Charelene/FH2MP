// The Xbox One guide's look (Settings > System > Xbox One), shared by the screens that have it: a panel down the left of the screen
// with a row of tabs at its top, the open tab's name and a list under them, the clock and status in the screen's top right corner and
// the hints beside the panel. The layout and colours are measured from screenshots of the real guide, in 1080ths of the screen's height
// (there is no layout data for it): the panel 540 x 1016 at (78, 32) in 1E1E1E with 13-unit corners; the tabs 84 apart from 63 in, a
// bar 83 x 5 in the accent under the open one; the name 35 high, bold, 32 in; a list from 170 down, its entries 515 wide from 13 in:
// 74 high for one line (an icon 50 in, the words 90 in at 26), 102 for two (the second line 23, in 9E9E9E); the focus a lighter fill
// (333333) with a 3-unit outline and a glow in the accent, gliding from entry to entry. The words are Game Bar's Segoe Sans Text and
// the icons its Xbox Fluent font where Game Bar is installed (assets.cpp), Segoe UI and the Guide's Material Icons where it is not.
// The accent is the theme's.
#pragma once

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

#include "ui.h"

namespace one {

using namespace ui;

const float PX = 78, PY = 32, PW = 540, PH = 1016, LIST = 170, LINE_H = 74, TWO_H = 102, SEG_H = 48;
const ImU32 PANEL1 = IM_COL32(0x1e, 0x1e, 0x1e, 247), CARD1 = IM_COL32(0x2e, 0x2e, 0x2e, 255), FOCUS1 = IM_COL32(0x33, 0x33, 0x33, 255),
            LINE1 = IM_COL32(0x30, 0x30, 0x30, 255), GREY1 = IM_COL32(0x9e, 0x9e, 0x9e, 255), OFF1 = IM_COL32(0x5c, 0x5c, 0x5c, 255);

// The Xbox Fluent glyph for one of the Guide's icons, or 0 where the Guide's own stays (the font's other glyphs are not known).
inline uint32_t fluent(uint32_t icon) {
    switch (icon) {
    case I_GAMES: return 0xeb9e;     // a controller
    case I_PERSON:
    case I_GROUPS: return 0xe939;    // people
    case I_MAIL:
    case I_CHAT: return 0xeab2;      // a speech bubble
    case I_SETTINGS: return 0xe9bf;  // a gear
    case I_BELL: return 0xea7c;
    case I_VOLUME: return 0xea90;    // a speaker
    case I_APPS: return 0xea88;      // a shelf of games
    case I_PALETTE: return 0xe9a7;   // "customize"
    }
    return 0;
}

struct Panel {
    ImDrawList* dl;
    const GuideAssets& a;
    ImVec2 o, size;        // the screen's top left corner and size, in pixels
    float U, op, dx;       // pixels per unit; the guide's opacity and its slide in from the left
    float cop = 1, cdx = 0;  // the list's own share of the opacity and its slide (a tab or a page coming in)
    ImFont *tf, *bf;
    FontMetrics tm, bm;

    Panel(ImDrawList* dl_, const GuideAssets& a_, ImVec2 origin, float s, float op_, float in) : dl(dl_), a(a_), op(op_) {
        size = ImGui::GetIO().DisplaySize;
        o = ImVec2(origin.x + 640 * s - size.x * 0.5f, origin.y);
        U = s * 720 / 1080, dx = -70 * (1 - in);
        tf = a.text_one ? a.text_one : a.text, tm = a.text_one ? a.one_m : a.text_m;
        bf = a.text_one_bold ? a.text_one_bold : a.text_bold ? a.text_bold : a.text, bm = a.text_one_bold ? a.one_bold_m : a.bold_m;
    }
    ImVec2 P(float x, float y) const { return ImVec2(o.x + (x + dx) * U, o.y + y * U); }
    ImVec2 L(float x, float y) const { return P(x + cdx, y); }  // a point of the list
    ImU32 col(ImU32 c, float k = 1) const { return alpha(c, op * k); }
    ImU32 lcol(ImU32 c, float k = 1) const { return alpha(c, op * cop * k); }
    ImU32 outline() const { return blend(ACCENT, WHITE, 0.22f); }

    float width(const std::string& t, float sz, bool bold = false) const { return (bold ? bf : tf)->CalcTextSizeA(sz, FLT_MAX, 0, t.c_str()).x; }
    // Words centred on the line cy. align: -1 from x, 0 centred on x, 1 ending at x; max_w > 0 cuts them with an ellipsis.
    void text(ImVec2 p, float sz, ImU32 c, const std::string& t, bool bold = false, int align = -1, float max_w = 0) const {
        ImFont* f = bold ? bf : tf;
        const FontMetrics& m = bold ? bm : tm;
        float w = f->CalcTextSizeA(sz * U, FLT_MAX, 0, t.c_str()).x;
        float x = align < 0 ? p.x : align > 0 ? p.x - std::min(w, max_w > 0 ? max_w * U : w) : p.x - w / 2;
        if (max_w > 0) text_fit(dl, f, m, sz * U, x, p.y, max_w * U, c, t);
        else text_mid(dl, f, m, sz * U, x, p.y, c, t.c_str());
    }
    void icon(uint32_t ic, ImVec2 c, float sz, ImU32 colr) const {
        uint32_t fl = a.icons_one ? fluent(ic) : 0;
        if (!fl) return icon_mid(dl, a, sz * U, c.x, c.y, colr, ic);
        char g[5];
        utf8(fl, g);
        float w = a.icons_one->CalcTextSizeA(sz * U, FLT_MAX, 0, g).x;
        dl->AddText(a.icons_one, sz * U, ImVec2(c.x - w * 0.5f, css_top(a.icons_one_m, sz * U, c.y)), colr, g);
    }
    // A round picture of a player, or the Guide's stand-in in a disc.
    void avatar(ImTextureID pic, uint64_t xuid, ImVec2 c, float r, ImU32 tint) const {
        if (pic) return dl->AddImageRounded(pic, ImVec2(c.x - r * U, c.y - r * U), ImVec2(c.x + r * U, c.y + r * U), ImVec2(0, 0), ImVec2(1, 1), tint, r * U);
        dl->AddCircleFilled(c, r * U, alpha(PIC_COLORS[xuid % (sizeof PIC_COLORS / sizeof PIC_COLORS[0])], ((tint >> IM_COL32_A_SHIFT) & 0xff) / 255.0f), 48);
        icon_mid(dl, a, r * 1.3f * U, c.x, c.y, alpha(IM_COL32(0, 0, 0, 150), ((tint >> IM_COL32_A_SHIFT) & 0xff) / 255.0f), I_PERSON);
    }

    // The status corner: Gamerscore (none when below 0), the microphone, and the clock 50 high, bold, ending 131 from the right edge.
    void corner(int gamerscore, bool mic) const {
        const float right = o.x + size.x - 131 * U, y = o.y + 86 * U;
        time_t t = time(nullptr);
        const std::tm* lt = std::localtime(&t);  // (tm is the text font's metrics here)
        int hour = lt ? lt->tm_hour : 0, minute = lt ? lt->tm_min : 0;
        char clock[32];
        snprintf(clock, sizeof clock, "%d:%02d %s", hour % 12 ? hour % 12 : 12, minute, hour < 12 ? "AM" : "PM");
        text_mid(dl, bf, bm, 50 * U, right, o.y + 148 * U, col(WHITE), clock, true);
        icon_mid(dl, a, 30 * U, right - 18 * U, y, col(WHITE, mic ? 1 : 0.45f), I_MIC);
        if (gamerscore < 0) return;
        std::string g = std::to_string(gamerscore);
        float gw = bf->CalcTextSizeA(20 * U, FLT_MAX, 0, g.c_str()).x, gx = right - 60 * U - gw;
        text_mid(dl, bf, bm, 20 * U, gx, y, col(WHITE), g.c_str());
        dl->AddCircleFilled(ImVec2(gx - 20 * U, y), 12 * U, col(WHITE), 32);
        float gs = 16 * U, cw = bf->CalcTextSizeA(gs, FLT_MAX, 0, "G").x;
        dl->AddText(bf, gs, ImVec2(gx - 20 * U - cw / 2, caps_top(bm, gs, y)), col(IM_COL32(16, 16, 16, 255)), "G");
    }
    // The panel over a soft shadow.
    void panel() const {
        for (int i = 5; i >= 1; i--)
            dl->AddRectFilled(P(PX - i * 3, PY - i * 3 + 6), P(PX + PW + i * 3, PY + PH + i * 3 + 6), IM_COL32(0, 0, 0, int(13 * op)), (13 + i * 3) * U);
        dl->AddRectFilled(P(PX, PY), P(PX + PW, PY + PH), col(PANEL1), 13 * U);
    }
    // The tabs: the Guide's three blades (Games, the player's picture as the console's last tab is, Settings); bar: where the accent
    // bar stands, as a tab's number (it glides between them).
    void tabs(float bar, ImTextureID pic, uint64_t xuid) const {
        static const uint32_t ICON[3] = {I_GAMES, 0, I_SETTINGS};
        for (int i = 0; i < 3; i++) {
            ImVec2 c = P(PX + 63 + 84 * i, PY + 56);
            if (ICON[i]) icon(ICON[i], c, 38, col(WHITE));
            else avatar(pic, xuid, c, 27, col(WHITE));
        }
        float x = PX + 63 + 84 * bar;
        dl->AddRectFilled(P(x - 41.5f, PY + 101), P(x + 41.5f, PY + 106), col(ACCENT), 2.5f * U);
    }
    void title(const std::string& t) const { text(L(PX + 32, PY + 133), 35, lcol(WHITE), t, true, -1, PW - 64); }
    // A page's own tabs, under its name: their names in a row, the open one white over a bar in the accent. They take SEG_H.
    void segments(const char* const* names, int n, int on) const {
        float x = PX + 32;
        for (int i = 0; i < n; i++) {
            float w = width(names[i], 23);
            text(P(x, PY + LIST + 16), 23, col(i == on ? WHITE : GREY1), names[i]);
            if (i == on) dl->AddRectFilled(P(x, PY + LIST + 34), P(x + w, PY + LIST + 38), col(ACCENT), 2 * U);
            x += w + 30;
        }
    }
    // The focus: a lighter fill, a glow in the accent and a bright outline.
    void focus(float y, float h, float x = PX + 13, float w = 515) const {
        ImVec2 f0 = L(x, y), f1 = L(x + w, y + h);
        dl->AddRectFilled(f0, f1, lcol(FOCUS1), 6 * U);
        for (int i = 4; i >= 1; i--) dl->AddRect(f0, f1, lcol(ACCENT, 0.13f), 6 * U, 0, (3 + i * 3.5f) * U);
        dl->AddRect(f0, f1, lcol(outline()), 6 * U, 0, 3 * U);
    }
    void divider(float y) const { dl->AddRectFilled(L(PX + 20, y), L(PX + PW - 20, y + 1.5f), lcol(LINE1)); }
    // A one-line entry: an icon (0: none), its words, and words in grey at its right end.
    void line(float y, uint32_t ic, const std::string& words, const std::string& meta, bool enabled, float k = 1) const {
        ImU32 fg = lcol(enabled ? WHITE : OFF1, k);
        float cy = y + LINE_H / 2, mw = meta.empty() ? 0 : width(meta, 23) + 20;
        if (ic) icon(ic, L(PX + 50, cy), 34, fg);
        text(L(PX + (ic ? 90 : 32), cy), 26, fg, words, false, -1, PX + 509 - mw - (PX + (ic ? 90 : 32)));
        if (!meta.empty()) text(L(PX + 509, cy), 23, lcol(enabled ? GREY1 : OFF1, k), meta, false, 1);
    }
    // A two-line entry from x: its words over a second line in grey.
    void two(float y, float x, const std::string& words, const std::string& sub, bool enabled, float max_w, ImU32 sub_col = GREY1) const {
        text(L(x, y + 33), 26, lcol(enabled ? WHITE : OFF1), words, false, -1, max_w);
        if (!sub.empty()) text(L(x, y + 69), 23, lcol(enabled ? sub_col : OFF1), sub, false, -1, max_w);
    }
    // Words wrapped to max_w from (x, y), in lines 1.3 of their height apart; returns the height they took.
    float para(float x, float y, float sz, ImU32 c, const std::string& t, float max_w) const {
        const char* b = t.c_str();
        const char* e = b + t.size();
        float used = 0;
        while (b < e) {
            const char* br = tf->CalcWordWrapPosition(sz, b, e, max_w);
            if (br == b) br = b + 1;
            ImVec2 p = L(x, y + used + sz * 0.65f);
            dl->AddText(tf, sz * U, ImVec2(p.x, css_top(tm, sz * U, p.y)), c, b, br);
            used += sz * 1.3f, b = br;
            while (b < e && *b == ' ') b++;
        }
        return used;
    }
    // The hints beside the panel, one under another (a key without a word shares the next one's line).
    void hints(const Hint* h, int n) const {
        float x = PX + PW + 62, y = 174;
        for (int i = 0; i < n; i++) {
            ImVec2 p = P(x, y);
            float w = badge(dl, a, h[i].key, h[i].ring, p.x, p.y, 1.3f * U, op) / U;
            if (!h[i].label) {
                x += w + 8;
                continue;
            }
            text(P(x + w + 12, y), 26, col(WHITE), h[i].label);
            x = PX + PW + 62, y += 48;
        }
    }
    // Up and down marks at the panel's foot when a list has more above or below.
    void more(bool up, bool down) const {
        if (up) icon_mid(dl, a, 30 * U, P(PX + PW - 70, 0).x, P(0, PY + PH - 26).y, col(GREY1), I_EXPAND_LESS);
        if (down) icon_mid(dl, a, 30 * U, P(PX + PW - 36, 0).x, P(0, PY + PH - 26).y, col(GREY1), I_EXPAND_MORE);
    }
};

}  // namespace one
