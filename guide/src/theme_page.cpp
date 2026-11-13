// Settings > Theme: the canvas' themes in two tabs (Themes: its eight and Xbox 360; Plastic: the six colours tinted all over), the player's own card
// beside them. The focused theme is shown at once on the whole Guide; A keeps it (the player's profile, so others see the card in it),
// B goes back to the one kept.
#include <algorithm>

#include "ui.h"

using namespace ui;

namespace {

std::vector<std::string> theme_keys(int tab) {
    std::vector<std::string> out;
    for (const ThemeDef& t : theme_presets())
        if (tab == 0) out.push_back(t.key);
        else if (t.plastic_too) out.push_back(std::string(t.key) + "-plastic");
    return out;
}

}  // namespace

void Guide::open_theme(double now) {
    theme_kept_ = theme_current();
    theme_tab_ = theme_plastic(theme_kept_) ? 1 : 0;
    std::vector<std::string> keys = theme_keys(theme_tab_);
    auto at = std::find(keys.begin(), keys.end(), theme_kept_);
    theme_row_[theme_tab_] = at == keys.end() ? 0 : int(at - keys.begin());
    theme_row_[1 - theme_tab_] = 0;
    theme_bar_.set(float(theme_row_[theme_tab_]));
    theme_at_ = -10;
    push(Page::Theme, now);
}

void Guide::handle_theme(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<std::string> keys = theme_keys(theme_tab_);
    int& row = theme_row_[theme_tab_];
    switch (e) {
    case PadEvent::Left:
    case PadEvent::Lb:
    case PadEvent::Right:
    case PadEvent::Rb: {
        int d = (e == PadEvent::Left || e == PadEvent::Lb) ? -1 : 1;
        int t = std::clamp(theme_tab_ + d, 0, 1);
        if (t == theme_tab_) break;
        theme_dir_ = d, theme_tab_ = t, theme_at_ = now;
        std::vector<std::string> k = theme_keys(t);
        theme_row_[t] = std::clamp(theme_row_[t], 0, int(k.size()) - 1);
        theme_bar_.set(float(theme_row_[t]));
        theme_apply(k[theme_row_[t]]);
        break;
    }
    case PadEvent::Up:
    case PadEvent::Down: {
        int r = std::clamp(row + (e == PadEvent::Up ? -1 : 1), 0, int(keys.size()) - 1);
        if (r == row) break;
        row = r;
        theme_bar_.go(float(row), now, 0.14f);
        theme_apply(keys[row]);
        break;
    }
    case PadEvent::A: {
        theme_kept_ = keys[row];
        theme_apply(theme_kept_);
        GuideRequest r;
        r.action = GuideAction::SetTheme, r.name = theme_kept_;
        out.push_back(r);
        pop(now);
        toast(ToastKind::Info, "Theme", theme_name(theme_kept_).c_str(), I_PALETTE, 0, now);
        break;
    }
    case PadEvent::B:
        theme_apply(theme_kept_);
        pop(now);
        break;
    default: break;
    }
}

void Guide::draw_theme(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Theme", title_op);
    float x = L + slide;

    const char* names[2] = {"Themes", "Plastic"};
    for (int t = 0; t < 2; t++) {
        bool on = t == theme_tab_;
        float tx = x + t * 431;
        if (on) panel_fill(dl, v.pt(tx, 96), v.pt(tx + 431, 138), pop);
        else dl->AddRectFilled(v.pt(tx, 96), v.pt(tx + 431, 138), alpha(TAB, pop));
        if (!on) tab_shadow(dl, v, t ? tx : tx + 431, 96, 138, t ? 1 : -1, pop);
        float lw = a.text->CalcTextSizeA(24, FLT_MAX, 0, names[t]).x;
        ImVec2 lp = v.pt(on ? tx + 27 : tx + (431 - lw) / 2, 117);
        text_mid(dl, a.text, a.text_m, v.sz(24), lp.x, lp.y, alpha(on ? TEXT : TAB_TEXT, pop), names[t]);
    }
    ImVec2 p0 = v.pt(x, 138), p1 = v.pt(x + W, PAGE_B);
    panel_fill(dl, p0, p1, pop);
    dl->PushClipRect(p0, p1, true);

    // the themes (they slide in when the tab changes), each with its saturated, bright and dark squares at the row's end
    float te = ease_guide(std::clamp(float((now - theme_at_) / 0.26), 0.0f, 1.0f));
    float ox = x + (1 - te) * 110 * theme_dir_, oop = pop * te;
    std::vector<std::string> keys = theme_keys(theme_tab_);
    std::vector<Opt> opts;
    for (const std::string& k : keys) opts.push_back({theme_name(k), 0, true, 0});
    const float RH = 42, TOP = 138;  // the 360's tab strip and rows, as on a profile
    dl->PushClipRect(v.pt(x, 138), v.pt(x + 431, PAGE_B), true);
    option_list(dl, a, v, ox, TOP, 431, RH, 24, 27, opts, theme_row_[theme_tab_], theme_bar_.value(now), oop);
    for (size_t i = 0; i < keys.size(); i++) {
        const ThemeDef& t = theme_preset(keys[i]);
        float cy = TOP + i * RH + RH * 0.5f, sx = ox + 431 - 27 - 3 * 22 - 2 * 6;
        ImU32 sw[3] = {t.accent, t.bright, t.dark};
        for (int k = 0; k < 3; k++) {
            ImVec2 q0 = v.pt(sx + k * 28, cy - 11), q1 = v.pt(sx + k * 28 + 22, cy + 11);
            dl->AddRectFilled(q0, q1, alpha(sw[k], oop));
            if (k == 2) dl->AddRect(q0, q1, alpha(LINE, oop), 0, 0, v.sz(1));
        }
    }
    dl->PopClipRect();

    // the player's own card, in the theme in focus
    Friend me = social_.me;
    if (me.tag.empty()) me.tag = gamertag;
    profile_side(dl, a, origin, s, zoom, x, 138, me, false, pop, pop);

    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 4, op);
}
