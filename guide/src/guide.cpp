// The Guide's drawing and navigation, from the design canvas (artboard 1 "Guide", artboard 7 "Notification"). Everything is laid out in the
// canvas' 1280x720 units, scaled to the game's frame and centred. Motion follows the canvas' CSS: cubic-bezier(.1,.9,.2,1) for the open
// (scale .92 -> 1, 0.26 s), the blade widths (0.3 s), the focus bar (0.14 s) and the rows sliding in after a blade switch (150 units,
// 0.32 s, 40 ms + 28 ms per row); the toast plays its 7.5 s keyframes once.
#include "guide.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Theme "xbox" of the canvas.
const ImU32 ACCENT = IM_COL32(0x10, 0x8a, 0x10, 255), DARK = IM_COL32(0x0a, 0x4f, 0x0a, 255), PANEL = IM_COL32(0x2a, 0x2b, 0x2e, 255),
            LINE = IM_COL32(0x3a, 0x3b, 0x3f, 255), TAB = IM_COL32(0x45, 0x47, 0x4c, 255), RAISED = IM_COL32(0x34, 0x36, 0x3a, 255),
            MUTED = IM_COL32(0xb4, 0xb6, 0xbb, 255), TAB_TEXT = IM_COL32(0xd4, 0xd6, 0xda, 255), MUTED3 = IM_COL32(0xc9, 0xcc, 0xd2, 255),
            WHITE = IM_COL32(255, 255, 255, 255), BLACK = IM_COL32(0, 0, 0, 255);

enum : uint32_t {
    I_PERSON = 0xe7fd, I_GROUPS = 0xf233, I_MAIL = 0xe158, I_SENSORS = 0xe51e, I_CHAT = 0xe0ca, I_GROUP_ADD = 0xe7f0, I_GAMES = 0xea28,
    I_TROPHY = 0xea23, I_PALETTE = 0xe40a, I_MIC = 0xe029, I_BELL = 0xe7f4, I_SETTINGS = 0xe8b8, I_INFO = 0xe88e
};

struct Row {
    const char* label;
    const char* meta;
    uint32_t icon;
    GuideAction action;
};

const Row GAMES[] = {{"Forza Horizon 2", "Now playing", I_GAMES, GuideAction::Resume}, {"Achievements", "", I_TROPHY, GuideAction::Soon}};
const Row ME[] = {{"Friends", "", I_PERSON, GuideAction::Soon},          {"Party", "", I_GROUPS, GuideAction::Soon},
                  {"Messages", "", I_MAIL, GuideAction::Soon},           {"Beacons & Activity", "", I_SENSORS, GuideAction::Soon},
                  {"Private Chat", "", I_CHAT, GuideAction::Soon},       {"Invite to Game", "", I_GROUP_ADD, GuideAction::Soon}};
const Row SETTINGS[] = {{"Profile", "", I_PERSON, GuideAction::Soon},    {"Theme", "Xbox", I_PALETTE, GuideAction::Soon},
                        {"Voice", "", I_MIC, GuideAction::Soon},         {"Notifications", "", I_BELL, GuideAction::Soon},
                        {"Controller", "", I_GAMES, GuideAction::Soon},  {"System", "", I_SETTINGS, GuideAction::Soon}};
const Row* ROWS[3] = {GAMES, ME, SETTINGS};
const int ROW_COUNT[3] = {2, 6, 6};
const char* BLADE[3] = {"Games", nullptr, "Settings"};

void utf8(uint32_t cp, char out[5]) {
    if (cp < 0x80) {
        out[0] = char(cp), out[1] = 0;
    } else if (cp < 0x800) {
        out[0] = char(0xc0 | (cp >> 6)), out[1] = char(0x80 | (cp & 0x3f)), out[2] = 0;
    } else {
        out[0] = char(0xe0 | (cp >> 12)), out[1] = char(0x80 | ((cp >> 6) & 0x3f)), out[2] = char(0x80 | (cp & 0x3f)), out[3] = 0;
    }
}

ImU32 alpha(ImU32 c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    unsigned int al = (c >> IM_COL32_A_SHIFT) & 0xff;
    return (c & ~IM_COL32_A_MASK) | (ImU32(al * a + 0.5f) << IM_COL32_A_SHIFT);
}

float ease_guide(float t) { return css_bezier(0.1f, 0.9f, 0.2f, 1.0f, t); }

// Where AddText must start (y) so that a line box centred on cy has its baseline where CSS puts it: cy + (ascent - descent) / 2 em,
// whatever the line-height (the half-leading is shared out evenly). ImGui puts the baseline ascent / (ascent + descent) of the size down.
float css_top(const FontMetrics& m, float size, float cy) { return cy + size * (m.asc - m.desc) * 0.5f - size * m.asc / (m.asc + m.desc); }

// Text in a line box centred on cy (CSS align-items: center), starting at x or, with right, ending there.
void text_mid(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float x, float cy, ImU32 col, const char* text, bool right = false) {
    float w = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    dl->AddText(f, size, ImVec2(right ? x - w : x, css_top(m, size, cy)), col, text);
}

// CSS writing-mode: vertical-rl: the text runs down from top, turned a quarter clockwise, its column centred on cx. Laid out without a
// clip rect (ImGui drops glyphs outside the clip rect before they are turned, which cut the blade names short).
void vertical_text(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float cx, float top, ImU32 col, const char* text) {
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

// A key of the hint bar: a 20-unit circle (A, B, X, Y) or a 34x22 rounded box (LB, RB) with a 2-unit ring inside its edge, centred on cy.
// Returns its width in pixels.
float hint_key(ImDrawList* dl, const GuideAssets& a, const char* key, ImU32 ring, float x, float cy, float s, float op) {
    bool wide = strlen(key) > 1;
    float kw = (wide ? 34 : 20) * s, kh = (wide ? 22 : 20) * s, rr = (wide ? 6 : 10) * s;
    ImVec2 k0(x, cy - kh / 2), k1(x + kw, cy + kh / 2);
    dl->AddRectFilled(k0, k1, alpha(BLACK, op), rr);
    dl->AddRect(ImVec2(k0.x + s, k0.y + s), ImVec2(k1.x - s, k1.y - s), alpha(ring, op), rr - s, 0, 2 * s);
    ImFont* kf = a.text_bold ? a.text_bold : a.text;
    float size = (wide ? 11 : 12) * s;
    float tw = kf->CalcTextSizeA(size, FLT_MAX, 0, key).x;
    dl->AddText(kf, size, ImVec2(x + (kw - tw) * 0.5f, css_top(a.bold_m, size, cy)), alpha(ring, op), key);
    return kw;
}

ImU32 toast_color(ToastKind k) {
    switch (k) {
    case ToastKind::Invite: return ACCENT;
    case ToastKind::Info: return IM_COL32(0xf7, 0x8c, 0x00, 255);
    case ToastKind::Saving: return IM_COL32(0xf2, 0xc2, 0x00, 255);
    case ToastKind::Warning: return IM_COL32(0xee, 0x3b, 0x23, 255);
    }
    return ACCENT;
}

}  // namespace

float css_bezier(float x1, float y1, float x2, float y2, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    auto bx = [&](float u) { return 3 * (1 - u) * (1 - u) * u * x1 + 3 * (1 - u) * u * u * x2 + u * u * u; };
    auto by = [&](float u) { return 3 * (1 - u) * (1 - u) * u * y1 + 3 * (1 - u) * u * u * y2 + u * u * u; };
    float lo = 0, hi = 1, u = t;
    for (int i = 0; i < 30; i++) {
        u = (lo + hi) * 0.5f;
        if (bx(u) < t) lo = u; else hi = u;
    }
    return by(u);
}

int PadReader::feed(uint16_t buttons, int16_t lx, int16_t ly, uint64_t now_ms, bool open, PadEvent* out) {
    int n = 0;
    if (buttons & padbits::Guide) {
        if (!guide_down) guide_down = true, guide_since = now_ms;
        if (!guide_fired && now_ms - guide_since >= GUIDE_HOLD_MS) guide_fired = true, out[n++] = PadEvent::Toggle;
    } else {
        if (open && guide_down && !guide_fired) out[n++] = PadEvent::Toggle;  // a short press closes an open Guide
        guide_down = guide_fired = false;
    }
    if (open) {
        uint16_t pressed = buttons & ~prev;
        const struct { uint16_t b; PadEvent e; } map[] = {{padbits::A, PadEvent::A},   {padbits::B, PadEvent::B},   {padbits::X, PadEvent::X},
                                                          {padbits::Y, PadEvent::Y},   {padbits::Lb, PadEvent::Lb}, {padbits::Rb, PadEvent::Rb},
                                                          {padbits::Start, PadEvent::Start}};
        for (auto& m : map)
            if (pressed & m.b) out[n++] = m.e;
        int d = -1;
        if (buttons & padbits::Up) d = int(PadEvent::Up);
        else if (buttons & padbits::Down) d = int(PadEvent::Down);
        else if (buttons & padbits::Left) d = int(PadEvent::Left);
        else if (buttons & padbits::Right) d = int(PadEvent::Right);
        else if (std::max(std::abs(int(lx)), std::abs(int(ly))) >= 18000)
            d = std::abs(int(lx)) >= std::abs(int(ly)) ? int(lx > 0 ? PadEvent::Right : PadEvent::Left) : int(ly > 0 ? PadEvent::Up : PadEvent::Down);
        if (d != dir) {
            dir = d;
            if (d >= 0) out[n++] = PadEvent(d), next_at = now_ms + REPEAT_DELAY_MS;
        } else if (d >= 0 && now_ms >= next_at) {
            out[n++] = PadEvent(d), next_at = now_ms + REPEAT_EVERY_MS;
        }
    } else {
        dir = -1;
    }
    prev = buttons;
    return n;
}

void Guide::Tween::go(float v, double now, float s) {
    if (std::fabs(v - to) < 1e-4f) return;
    from = value(now), to = v, start = now, secs = s;
}

float Guide::Tween::value(double now) const {
    if (secs <= 0) return to;
    float t = float((now - start) / secs);
    return from + (to - from) * ease_guide(std::clamp(t, 0.0f, 1.0f));
}

void Guide::toggle(double now) {
    open = !open;
    if (open) opened_ = now; else closed_ = now;
    if (!widths_set_) {
        for (int i = 0; i < 3; i++) width_[i].from = width_[i].to = (i == blade_ ? 832.0f : 64.0f);
        widths_set_ = true;
    }
}

void Guide::switch_to(int b, double now) {
    if (b == blade_ || b < 0 || b > 2) return;
    switch_dir_ = b > blade_ ? 1 : -1;
    blade_ = b, switched_ = now;
    for (int i = 0; i < 3; i++) width_[i].go(i == b ? 832.0f : 64.0f, now, 0.3f);
}

GuideAction Guide::handle(PadEvent e, double now) {
    int n = ROW_COUNT[blade_];
    switch (e) {
    case PadEvent::Toggle: toggle(now); break;
    case PadEvent::Up:
    case PadEvent::Down: {
        int f = std::clamp(focus_[blade_] + (e == PadEvent::Up ? -1 : 1), 0, n - 1);
        focus_[blade_] = f;
        bar_[blade_].go(float(f), now, 0.14f);
        break;
    }
    case PadEvent::Left:
    case PadEvent::Lb: switch_to(blade_ - 1, now); break;
    case PadEvent::Right:
    case PadEvent::Rb: switch_to(blade_ + 1, now); break;
    case PadEvent::B: open = false, closed_ = now; break;
    case PadEvent::A: {
        const Row& r = ROWS[blade_][focus_[blade_]];
        if (r.action == GuideAction::Resume) {
            open = false, closed_ = now;
            return GuideAction::Resume;
        }
        toast(ToastKind::Info, r.label, "Coming in a later build", r.icon, 0, now);
        return GuideAction::Soon;
    }
    default: break;
    }
    return GuideAction::None;
}

void Guide::toast(ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic, double now) {
    toast_kind_ = kind, toast_title_ = title, toast_detail_ = detail, toast_icon_ = icon, toast_pic_ = pic, toast_at_ = now;
    for (auto& c : toast_title_) c = char(toupper((unsigned char)c));
    for (auto& c : toast_detail_) c = char(toupper((unsigned char)c));
}

bool Guide::busy(double now) const { return open || now - toast_at_ < 6.4; }

bool Guide::draw(ImDrawList* dl, const GuideAssets& a, float w, float h, double now) {
    float s = std::min(w / 1280.0f, h / 720.0f);
    ImVec2 origin(w * 0.5f - 640 * s, h * 0.5f - 360 * s);
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
    ImFont* tf = a.text;
    if (open) {
        float k = ease_guide(std::clamp(float((now - opened_) / 0.26), 0.0f, 1.0f));
        float zoom = 0.92f + 0.08f * k, op = k;
        ImVec2 c = at(640, 360);
        auto z = [&](ImVec2 p) { return ImVec2(c.x + (p.x - c.x) * zoom, c.y + (p.y - c.y) * zoom); };
        auto zs = [&](float v) { return v * s * zoom; };
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, int(153 * op)));

        // header: blade name (the player on the middle blade), the player's picture, the clock
        // canvas: h1 at (27, 20) of the section (left 160, top 41), 22 px, line-height 1: its line box is centred on y 72
        const char* title = blade_ == 1 ? gamertag.c_str() : BLADE[blade_];
        text_mid(dl, tf, a.text_m, zs(22), z(at(187, 72)).x, z(at(187, 72)).y, alpha(WHITE, op), title);
        ImVec2 pic0 = z(at(616, 41)), pic1 = z(at(664, 89));
        if (a.gamerpic) {
            dl->AddImage(a.gamerpic, pic0, pic1, ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, op));
        } else {
            dl->AddRectFilled(pic0, pic1, alpha(IM_COL32(0xc9, 0x66, 0x2f, 255), op));
            char ic[5];
            utf8(I_PERSON, ic);
            if (a.icons)
                text_mid(dl, a.icons, a.icons_m, zs(52), (pic0.x + pic1.x) * 0.5f - zs(26), (pic0.y + pic1.y) * 0.5f + zs(6), alpha(IM_COL32(0, 0, 0, 140), op), ic);
        }
        SYSTEMTIME st;
        GetLocalTime(&st);
        char clock[32];
        int h12 = st.wHour % 12 == 0 ? 12 : st.wHour % 12;
        snprintf(clock, sizeof clock, "%d:%02d  %s", h12, st.wMinute, st.wHour < 12 ? "AM" : "PM");
        text_mid(dl, tf, a.text_m, zs(22), z(at(1090, 72)).x, z(at(1090, 72)).y, alpha(WHITE, op), clock, true);

        // blades
        float x = 160;
        float since = float(now - switched_);
        for (int bi = 0; bi < 3; bi++) {
            float bw = width_[bi].value(now);
            bool on = bi == blade_;
            float bh = on ? 500 : 452, top = 96 + (500 - bh) / 2, lw = on ? 96.0f : 64.0f;
            ImVec2 l0 = z(at(x, top)), l1 = z(at(x + std::min(lw, bw), top + bh));
            dl->AddRectFilled(l0, l1, alpha(on ? RAISED : TAB, op));
            // canvas: 26 px, vertical-rl, centred across the tab, 24 px from its top
            const char* name = bi == 1 ? gamertag.c_str() : BLADE[bi];
            vertical_text(dl, tf, a.text_m, zs(26), (l0.x + l1.x) * 0.5f, l0.y + zs(24), alpha(on ? WHITE : TAB_TEXT, op), name);
            if (bw > lw + 1) {
                ImVec2 b0 = z(at(x + lw, top)), b1 = z(at(x + bw, top + bh));
                dl->AddRectFilled(b0, b1, alpha(PANEL, op));
                dl->PushClipRect(b0, b1, true);
                if (on) {
                    float by = top + bar_[bi].value(now) * 66;
                    ImVec2 r0 = z(at(x + lw, by)), r1 = z(at(x + lw + 736, by + 66));
                    dl->AddRectFilled(r0, r1, alpha(ACCENT, op));
                    dl->AddLine(ImVec2(r0.x, r1.y - zs(0.5f)), ImVec2(r1.x, r1.y - zs(0.5f)), alpha(DARK, op), zs(1));
                }
                for (int i = 0; i < ROW_COUNT[bi]; i++) {
                    const Row& r = ROWS[bi][i];
                    float slide = 0, rop = 1;
                    if (on && since < 0.7f) {
                        float d = std::clamp((since - 0.04f - 0.028f * i) / 0.32f, 0.0f, 1.0f);
                        float e = ease_guide(d);
                        slide = (1 - e) * 150 * switch_dir_, rop = e;
                    }
                    float y = top + i * 66;
                    bool hi = on && i == focus_[bi];
                    ImVec2 r0 = z(at(x + lw + slide, y)), r1 = z(at(x + lw + 736 + slide, y + 66));
                    if (!hi) dl->AddLine(ImVec2(r0.x, r1.y - zs(0.5f)), ImVec2(r1.x, r1.y - zs(0.5f)), alpha(LINE, op * rop), zs(1));
                    float cy = (r0.y + r1.y) * 0.5f;
                    text_mid(dl, tf, a.text_m, zs(26), r0.x + zs(30), cy, alpha(WHITE, op * rop), r.label);
                    float right = r1.x - zs(22);
                    if (r.icon && a.icons) {
                        char ic[5];
                        utf8(r.icon, ic);
                        float iw = a.icons->CalcTextSizeA(zs(32), FLT_MAX, 0, ic).x;
                        dl->AddText(a.icons, zs(32), ImVec2(right - zs(16) - iw * 0.5f, css_top(a.icons_m, zs(32), cy)), alpha(WHITE, op * rop), ic);
                        right -= zs(32 + 16);
                    }
                    if (r.meta && *r.meta) text_mid(dl, tf, a.text_m, zs(20), right, cy, alpha(hi ? WHITE : MUTED, op * rop), r.meta, true);
                }
                dl->PopClipRect();
            }
            x += bw;
        }

        // hint bar (canvas: a flex row 10 px under the blades, 27 px in, 19 px text, gaps 22 between hints and 7 inside one, LB and RB
        // 8 apart): every item's box is centred on the row's centre, 596 + 10 + half the 19 px text's line box (the tallest item)
        float hx = 187, hy = 596 + 10 + 19 * (a.text_m.asc + a.text_m.desc) * 0.5f;
        struct Hint { const char* key; const char* label; ImU32 ring; };
        const Hint hints[] = {{"A", "Select", IM_COL32(0x5f, 0xd1, 0x1c, 255)}, {"B", "Back", IM_COL32(0xff, 0x4a, 0x4f, 255)},
                              {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
        for (const Hint& hnt : hints) {
            ImVec2 p = z(at(hx, hy));
            hx += hint_key(dl, a, hnt.key, hnt.ring, p.x, p.y, zs(1), op) / zs(1);
            if (hnt.label) {
                hx += 7;
                ImVec2 q = z(at(hx, hy));
                text_mid(dl, tf, a.text_m, zs(19), q.x, q.y, alpha(WHITE, op), hnt.label);
                hx += tf->CalcTextSizeA(19, FLT_MAX, 0, hnt.label).x + 22;
            } else {
                hx += 8;
            }
        }
    }
    draw_toast(dl, a, s, origin, now);
    return busy(now);
}

void Guide::draw_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now) {
    float t = float(now - toast_at_);
    if (t < 0 || t > 6.3f) return;
    auto seg = [&](float t0, float t1, float x1, float y1, float x2, float y2) {
        return css_bezier(x1, y1, x2, y2, std::clamp((t - t0) / (t1 - t0), 0.0f, 1.0f));
    };
    // nZoom: 0 -> 1.08 (0.3 s, cubic-bezier(.2,.8,.3,1.2)) -> 1 (0.45 s, ease-out) ... 5.85 s -> 0 (6.3 s, cubic-bezier(.6,0,.9,.4))
    float zoom, zop;
    if (t < 0.3f) {
        float e = seg(0, 0.3f, 0.2f, 0.8f, 0.3f, 1.2f);
        zoom = 1.08f * e, zop = std::min(1.0f, e);
    } else if (t < 0.45f) {
        zoom = 1.08f - 0.08f * seg(0.3f, 0.45f, 0.0f, 0.0f, 0.58f, 1.0f), zop = 1;
    } else if (t < 5.85f) {
        zoom = 1, zop = 1;
    } else {
        float e = seg(5.85f, 6.3f, 0.6f, 0.0f, 0.9f, 0.4f);
        zoom = 1 - e, zop = 1 - e;
    }
    // nSlide / nUnfold: 0.6 -> 1.05 s open, 5.4 -> 5.85 s close, cubic-bezier(.7,0,.25,1). The panel is as wide as its text (canvas: 269
    // with 22 / 12 padding for its sample text), the whole toast centred; it unfolds from its centre while the square slides half its width.
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    float tw = std::max(bf->CalcTextSizeA(17, FLT_MAX, 0, toast_title_.c_str()).x, a.text->CalcTextSizeA(17, FLT_MAX, 0, toast_detail_.c_str()).x);
    float pw = std::ceil(tw) + 22 + 12, half = pw * 0.5f;
    float unfold = t < 5.4f ? seg(0.6f, 1.05f, 0.7f, 0.0f, 0.25f, 1.0f) : 1 - seg(5.4f, 5.85f, 0.7f, 0.0f, 0.25f, 1.0f);
    float shift = half * (1 - unfold);
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
    float bx = 640 - (64 + pw) * 0.5f, by = 600;
    ImU32 kc = toast_color(toast_kind_);
    if (unfold > 0.001f) {
        ImVec2 p0 = at(bx + 64 + shift, by), p1 = at(bx + 64 + pw - shift, by + 64);
        dl->AddRectFilled(p0, p1, PANEL);
        dl->PushClipRect(p0, p1, true);
        float x = at(bx + 64 + 22, 0).x;
        // two 22 px lines centred in the 64 px panel: their line boxes are centred on by + 21 and by + 43
        dl->AddText(bf, 17 * s, ImVec2(x, css_top(a.bold_m, 17 * s, at(0, by + 21).y)), kc, toast_title_.c_str());
        dl->AddText(a.text, 17 * s, ImVec2(x, css_top(a.text_m, 17 * s, at(0, by + 43).y)), WHITE, toast_detail_.c_str());
        dl->PopClipRect();
    }
    // the square: slides with the panel, zooms in and out; on an invite its icon slides up at 2.0-2.4 s and the other player's picture
    // comes up in its place
    float side = 64 * s * zoom;
    ImVec2 c = at(bx + 32 + shift, by + 32);
    ImVec2 q0(c.x - side / 2, c.y - side / 2), q1(c.x + side / 2, c.y + side / 2);
    if (side > 0.5f) {
        dl->AddRectFilled(q0, q1, alpha(kc, zop));
        float up = toast_pic_ ? side * seg(2.0f, 2.4f, 0.3f, 0.0f, 0.2f, 1.0f) : 0;
        dl->PushClipRect(q0, q1, true);
        if (a.icons && toast_icon_) {
            char ic[5];
            utf8(toast_icon_, ic);
            float size = 46 * s * zoom;
            float iw = a.icons->CalcTextSizeA(size, FLT_MAX, 0, ic).x;
            dl->AddText(a.icons, size, ImVec2(c.x - iw / 2, css_top(a.icons_m, size, c.y) - up), alpha(WHITE, zop), ic);
        }
        if (toast_pic_)
            dl->AddImage(toast_pic_, ImVec2(q0.x, q0.y + side - up), ImVec2(q1.x, q1.y + side - up), ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, zop));
        dl->PopClipRect();
    }
}
