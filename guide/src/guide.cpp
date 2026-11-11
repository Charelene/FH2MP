// The Guide's drawing and navigation, from the design canvas: artboard 1 (Guide home: blades), 4 (the friend list: Friends and Invite to
// Game), 5 (a received game invite), 7 (the notification toast). Everything is laid out in the canvas' 1280x720 units, scaled to the game's
// frame and centred. Motion follows the canvas' CSS: cubic-bezier(.1,.9,.2,1) for the open (scale .92 -> 1, 0.26 s), the blade widths
// (0.3 s), the focus bars (0.14 s), the rows sliding in after a blade switch (150 units, 0.32 s, 40 ms + 28 ms per row), a page or tab
// coming in (110 units, 0.26 s) and the title fading (0.16 s); the toast plays its 7.5 s keyframes once.
#include "guide.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Theme "xbox" of the canvas.
const ImU32 ACCENT = IM_COL32(0x10, 0x8a, 0x10, 255), DARK = IM_COL32(0x0a, 0x4f, 0x0a, 255), BRIGHT = IM_COL32(0x8e, 0xd6, 0x1e, 255),
            PANEL = IM_COL32(0x2a, 0x2b, 0x2e, 255), PANE = IM_COL32(0x1e, 0x1f, 0x22, 255), LINE = IM_COL32(0x3a, 0x3b, 0x3f, 255),
            TAB = IM_COL32(0x45, 0x47, 0x4c, 255), RAISED = IM_COL32(0x34, 0x36, 0x3a, 255), MUTED = IM_COL32(0xb4, 0xb6, 0xbb, 255),
            MUTED2 = IM_COL32(0x9c, 0x9f, 0xa5, 255), TAB_TEXT = IM_COL32(0xd4, 0xd6, 0xda, 255), MUTED3 = IM_COL32(0xc9, 0xcc, 0xd2, 255),
            WHITE = IM_COL32(255, 255, 255, 255), BLACK = IM_COL32(0, 0, 0, 255), ONLINE = IM_COL32(0xe8, 0xe9, 0xeb, 255),
            OFFLINE = IM_COL32(0xe0, 0x55, 0x2f, 255);
const ImU32 PIC_COLORS[] = {IM_COL32(0xc9, 0x66, 0x2f, 255), IM_COL32(0x3f, 0x6f, 0xc4, 255), IM_COL32(0xc4, 0x9a, 0x2b, 255),
                            IM_COL32(0x3e, 0x9a, 0x5a, 255), IM_COL32(0x2f, 0x8f, 0xa8, 255), IM_COL32(0x9a, 0x4f, 0xb8, 255),
                            IM_COL32(0xa8, 0x3a, 0x3a, 255)};

enum : uint32_t {
    I_PERSON = 0xe7fd, I_GROUPS = 0xf233, I_MAIL = 0xe158, I_SENSORS = 0xe51e, I_CHAT = 0xe0ca, I_GROUP_ADD = 0xe7f0, I_GAMES = 0xea28,
    I_TROPHY = 0xea23, I_PALETTE = 0xe40a, I_MIC = 0xe029, I_BELL = 0xe7f4, I_SETTINGS = 0xe8b8, I_FLAG = 0xe153, I_SCHEDULE = 0xe8b5,
    I_EXPAND_LESS = 0xe5ce, I_EXPAND_MORE = 0xe5cf
};

struct Row {
    const char* label;
    uint32_t icon;
    GuideAction action;
    Page page;  // where A goes when action is None
};

const Row GAMES[] = {{"Forza Horizon 2", I_GAMES, GuideAction::Resume, Page::Home}, {"Achievements", I_TROPHY, GuideAction::Soon, Page::Home}};
const Row ME[] = {{"Friends", I_PERSON, GuideAction::None, Page::Friends},          {"Party", I_GROUPS, GuideAction::Soon, Page::Home},
                  {"Messages", I_MAIL, GuideAction::None, Page::Received},          {"Beacons & Activity", I_SENSORS, GuideAction::Soon, Page::Home},
                  {"Private Chat", I_CHAT, GuideAction::Soon, Page::Home},          {"Invite to Game", I_GROUP_ADD, GuideAction::None, Page::Invite}};
const Row SETTINGS[] = {{"Profile", I_PERSON, GuideAction::Soon, Page::Home},       {"Theme", I_PALETTE, GuideAction::Soon, Page::Home},
                        {"Voice", I_MIC, GuideAction::Soon, Page::Home},            {"Notifications", I_BELL, GuideAction::Soon, Page::Home},
                        {"Controller", I_GAMES, GuideAction::Soon, Page::Home},     {"System", I_SETTINGS, GuideAction::Soon, Page::Home}};
const Row* ROWS[3] = {GAMES, ME, SETTINGS};
const int ROW_COUNT[3] = {2, 6, 6};
const char* BLADE[3] = {"Games", nullptr, "Settings"};

// The friend list's tabs (artboard 4): widths and icons.
const float TAB_W[4] = {158, 388, 158, 158};
const uint32_t TAB_ICON[4] = {I_GROUPS, I_PERSON, I_FLAG, I_SCHEDULE};
const int LIST_ROWS = 6;

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

// The same, cut with an ellipsis to fit max_w (CSS text-overflow: ellipsis).
void text_fit(ImDrawList* dl, ImFont* f, const FontMetrics& m, float size, float x, float cy, float max_w, ImU32 col, const std::string& text) {
    if (f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str()).x <= max_w) return text_mid(dl, f, m, size, x, cy, col, text.c_str());
    std::string t = text;
    while (!t.empty() && f->CalcTextSizeA(size, FLT_MAX, 0.0f, (t + "...").c_str()).x > max_w) {
        t.pop_back();
        while (!t.empty() && (t.back() & 0xc0) == 0x80) t.pop_back();
    }
    text_mid(dl, f, m, size, x, cy, col, (t + "...").c_str());
}

void icon_mid(ImDrawList* dl, const GuideAssets& a, float size, float cx, float cy, ImU32 col, uint32_t icon) {
    if (!a.icons || !icon) return;
    char ic[5];
    utf8(icon, ic);
    float w = a.icons->CalcTextSizeA(size, FLT_MAX, 0, ic).x;
    dl->AddText(a.icons, size, ImVec2(cx - w * 0.5f, css_top(a.icons_m, size, cy)), col, ic);
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

// Canvas units to pixels: the frame's scale, then the Guide's open zoom around the centre of the screen.
struct View {
    ImVec2 origin;
    float s, zoom;
    ImVec2 pt(float x, float y) const {
        ImVec2 c(origin.x + 640 * s, origin.y + 360 * s), p(origin.x + x * s, origin.y + y * s);
        return ImVec2(c.x + (p.x - c.x) * zoom, c.y + (p.y - c.y) * zoom);
    }
    float sz(float v) const { return v * s * zoom; }
};

struct Hint {
    const char* key;
    const char* label;
    ImU32 ring;
};
const ImU32 RING_A = IM_COL32(0x5f, 0xd1, 0x1c, 255), RING_B = IM_COL32(0xff, 0x4a, 0x4f, 255);

// The hint bar: a flex row 10 units under the panel, 27 in, 19-unit text, gaps 22 between hints and 7 inside one, a hint without a label
// sits 8 from the next (LB RB). Every item is centred on the row's centre: top + half the 19-unit text's line box (the tallest item).
void hint_bar(ImDrawList* dl, const GuideAssets& a, const View& v, float x, float top, const Hint* hints, int n, float op) {
    float cy = top + 19 * (a.text_m.asc + a.text_m.desc) * 0.5f;
    for (int i = 0; i < n; i++) {
        ImVec2 p = v.pt(x, cy);
        x += hint_key(dl, a, hints[i].key, hints[i].ring, p.x, p.y, v.sz(1), op) / v.sz(1);
        if (hints[i].label) {
            x += 7;
            ImVec2 q = v.pt(x, cy);
            text_mid(dl, a.text, a.text_m, v.sz(19), q.x, q.y, alpha(WHITE, op), hints[i].label);
            x += a.text->CalcTextSizeA(19, FLT_MAX, 0, hints[i].label).x + 22;
        } else {
            x += 8;
        }
    }
}

// A player's picture, or the canvas' stand-in: a coloured square with a person's shape rising from its bottom edge.
void player_pic(ImDrawList* dl, const GuideAssets& a, ImTextureID pic, uint64_t xuid, ImVec2 p0, ImVec2 p1, float op) {
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

bool in_my_game(const Friend& f, const Friend& me) { return !me.lobby_id.empty() && f.lobby_id == me.lobby_id; }

std::string lobby_words(const Friend& f) {
    if (!f.lobby_status.empty()) return f.lobby_status;
    if (f.lobby_kind == "freeroam") return "Free Roam";
    if (f.lobby_kind == "roadtrip") return "Road Trip";
    return f.lobby_kind;
}

std::string ago(int secs) {
    if (secs < 60) return "Just now";
    if (secs < 120) return "1 minute ago";
    return std::to_string(secs / 60) + " minutes ago";
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
    if (open) {
        opened_ = now, page_ = Page::Home, page_at_ = -10;
    } else {
        closed_ = now, sent_.clear();
    }
    if (!widths_set_) {
        for (int i = 0; i < 3; i++) width_[i].from = width_[i].to = (i == blade_ ? 832.0f : 64.0f);
        widths_set_ = true;
    }
}

void Guide::open_on(Page p, double now) {
    if (p == Page::Invite || p == Page::Friends) tab_ = 1, list_bar_.go(float(row_[1] - top_[1]), now, 0);
    if (!open) {
        toggle(now);
        page_ = p, page_at_ = -10;
    } else if (page_ != p) {
        go_page(p, 1, now);
    }
}

void Guide::go_page(Page p, int dir, double now) {
    page_ = p, page_dir_ = dir, page_at_ = now;
    if (p == Page::Received) {
        received_ = std::min(received_, std::max(0, int(social_.invites.size()) - 1)), received_opt_ = 0;
        received_bar_.go(0, now, 0);
    }
}

void Guide::set_social(const Social& s) {
    social_ = s;
    for (int t = 0; t < 4; t++) {
        int n = int(tab_list(t).size());
        row_[t] = std::clamp(row_[t], 0, std::max(0, n - 1));
        top_[t] = std::clamp(top_[t], 0, std::max(0, n - LIST_ROWS));
    }
    received_ = std::clamp(received_, 0, std::max(0, int(s.invites.size()) - 1));
}

// The rows of a tab of the friend list, sorted by online status: in a game, online, offline; by name within each.
std::vector<const Friend*> Guide::tab_list(int tab) const {
    std::vector<const Friend*> out;
    for (const Friend& f : social_.friends) {
        bool in = tab == 1 || (tab == 0 && in_my_game(f, social_.me)) ||
                  (tab == 2 && !social_.me.club.empty() && f.club == social_.me.club);  // tab 3 (recent players) has no source yet
        if (in) out.push_back(&f);
    }
    std::stable_sort(out.begin(), out.end(), [](const Friend* x, const Friend* y) {
        if (x->state != y->state) return x->state > y->state;
        return _stricmp(x->tag.c_str(), y->tag.c_str()) < 0;
    });
    return out;
}

GuideRequest Guide::handle(PadEvent e, double now) {
    if (e == PadEvent::Toggle) {
        toggle(now);
        return {};
    }
    switch (page_) {
    case Page::Home: return handle_home(e, now);
    case Page::Friends:
    case Page::Invite: return handle_list(e, now);
    case Page::Received: return handle_received(e, now);
    }
    return {};
}

GuideRequest Guide::handle_home(PadEvent e, double now) {
    GuideRequest r;
    int n = ROW_COUNT[blade_];
    switch (e) {
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
    case PadEvent::B: toggle(now); break;
    case PadEvent::A: {
        const Row& row = ROWS[blade_][focus_[blade_]];
        if (row.action == GuideAction::Resume) {
            toggle(now);
            r.action = GuideAction::Resume;
        } else if (row.action == GuideAction::None) {
            if (row.page == Page::Invite || row.page == Page::Friends) tab_ = 1, list_bar_.go(float(row_[1] - top_[1]), now, 0);
            go_page(row.page, 1, now);
        } else {
            toast(ToastKind::Info, row.label, "Coming in a later build", row.icon, 0, now);
            r.action = GuideAction::Soon;
        }
        break;
    }
    default: break;
    }
    return r;
}

GuideRequest Guide::handle_list(PadEvent e, double now) {
    GuideRequest r;
    std::vector<const Friend*> list = tab_list(tab_);
    int n = int(list.size());
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        if (!n) break;
        int f = std::clamp(row_[tab_] + (e == PadEvent::Up ? -1 : 1), 0, n - 1);
        row_[tab_] = f;
        if (f < top_[tab_]) top_[tab_] = f;
        if (f >= top_[tab_] + LIST_ROWS) top_[tab_] = f - LIST_ROWS + 1;
        list_bar_.go(float(f - top_[tab_]), now, 0.14f);
        break;
    }
    case PadEvent::Left:
    case PadEvent::Lb:
    case PadEvent::Right:
    case PadEvent::Rb: {
        int d = (e == PadEvent::Left || e == PadEvent::Lb) ? -1 : 1;
        int t = std::clamp(tab_ + d, 0, 3);
        if (t != tab_) tab_dir_ = d, tab_ = t, tab_at_ = now, list_bar_.go(float(row_[t] - top_[t]), now, 0);
        break;
    }
    case PadEvent::B: go_page(Page::Home, -1, now); break;
    case PadEvent::A: {
        if (!n) break;
        const Friend& f = *list[row_[tab_]];
        if (in_my_game(f, social_.me)) {
            toast(ToastKind::Info, "Invite to Game", (f.tag + " is already in your game").c_str(), I_GROUP_ADD, 0, now);
        } else if (f.state == Friend::Offline) {
            toast(ToastKind::Info, "Invite to Game", (f.tag + " is offline").c_str(), I_GROUP_ADD, 0, now);
        } else {
            r.action = GuideAction::Invite, r.xuid = f.xuid, r.name = f.tag;
        }
        break;
    }
    default: break;
    }
    return r;
}

GuideRequest Guide::handle_received(PadEvent e, double now) {
    GuideRequest r;
    int n = int(social_.invites.size());
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        if (!n) break;
        received_opt_ = std::clamp(received_opt_ + (e == PadEvent::Up ? -1 : 1), 0, 1);
        received_bar_.go(float(received_opt_), now, 0.14f);
        break;
    case PadEvent::Lb:
    case PadEvent::Left:
        if (received_ > 0) received_--, page_dir_ = -1, page_at_ = now;
        break;
    case PadEvent::Rb:
    case PadEvent::Right:
        if (received_ + 1 < n) received_++, page_dir_ = 1, page_at_ = now;
        break;
    case PadEvent::B: go_page(Page::Home, -1, now); break;
    case PadEvent::A: {
        if (!n) break;
        const ReceivedInvite& iv = social_.invites[received_];
        r.action = received_opt_ == 0 ? GuideAction::Accept : GuideAction::Decline;
        r.id = iv.id, r.xuid = iv.from.xuid, r.name = iv.from.tag;
        social_.invites.erase(social_.invites.begin() + received_);  // until the server's list comes back without it
        received_ = std::clamp(received_, 0, std::max(0, n - 2));
        if (r.action == GuideAction::Accept || social_.invites.empty()) go_page(Page::Home, -1, now);
        break;
    }
    default: break;
    }
    return r;
}

void Guide::switch_to(int b, double now) {
    if (b == blade_ || b < 0 || b > 2) return;
    switch_dir_ = b > blade_ ? 1 : -1;
    blade_ = b, switched_ = now;
    for (int i = 0; i < 3; i++) width_[i].go(i == b ? 832.0f : 64.0f, now, 0.3f);
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
    if (open) {
        float k = ease_guide(std::clamp(float((now - opened_) / 0.26), 0.0f, 1.0f));
        float zoom = 0.92f + 0.08f * k, op = k;
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, int(153 * op)));
        switch (page_) {
        case Page::Home: draw_home(dl, a, origin, s, zoom, op, now); break;
        case Page::Friends:
        case Page::Invite: draw_list(dl, a, origin, s, zoom, op, now); break;
        case Page::Received: draw_received(dl, a, origin, s, zoom, op, now); break;
        }
    }
    draw_toast(dl, a, s, origin, now);
    return busy(now);
}

// The header above a panel (canvas: the title at (27, 20), 22 units, line-height 1, so centred on y 72; the player's picture 48x48 in the
// middle at the top; the clock 30 in from the right).
void Guide::draw_header(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, float left, float width, const char* title,
                        float title_op) {
    View v{origin, s, zoom};
    ImVec2 t = v.pt(left + 27, 72);
    text_mid(dl, a.text, a.text_m, v.sz(22), t.x, t.y, alpha(WHITE, op * title_op), title);
    ImVec2 pic0 = v.pt(left + width / 2 - 24, 41), pic1 = v.pt(left + width / 2 + 24, 89);
    player_pic(dl, a, a.gamerpic, social_.me.xuid, pic0, pic1, op);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char clock[32];
    int h12 = st.wHour % 12 == 0 ? 12 : st.wHour % 12;
    snprintf(clock, sizeof clock, "%d:%02d  %s", h12, st.wMinute, st.wHour < 12 ? "AM" : "PM");
    ImVec2 c = v.pt(left + width - 30, 72);
    text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(WHITE, op), clock, true);
}

void Guide::draw_home(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    ImFont* tf = a.text;
    // coming back from a page: the blades slide in from the left like a page does
    float pe = ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f));
    float pslide = (1 - pe) * 110 * page_dir_, pop = op * (page_at_ > 0 ? pe : 1);
    const char* title = blade_ == 1 ? gamertag.c_str() : BLADE[blade_];
    draw_header(dl, a, origin, s, zoom, op, 160, 960, title, page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1);

    int online = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    char friends_meta[32] = "", messages_meta[16] = "";
    if (social_.loaded) snprintf(friends_meta, sizeof friends_meta, "%d online", online);
    if (!social_.invites.empty()) snprintf(messages_meta, sizeof messages_meta, "%d", int(social_.invites.size()));

    float x = 160 + pslide;
    float since = float(now - switched_);
    for (int bi = 0; bi < 3; bi++) {
        float bw = width_[bi].value(now);
        bool on = bi == blade_;
        float bh = on ? 500 : 452, top = 96 + (500 - bh) / 2, lw = on ? 96.0f : 64.0f;
        ImVec2 l0 = v.pt(x, top), l1 = v.pt(x + std::min(lw, bw), top + bh);
        dl->AddRectFilled(l0, l1, alpha(on ? RAISED : TAB, pop));
        // canvas: 26 units, vertical-rl, centred across the tab, 24 from its top
        const char* name = bi == 1 ? gamertag.c_str() : BLADE[bi];
        vertical_text(dl, tf, a.text_m, v.sz(26), (l0.x + l1.x) * 0.5f, l0.y + v.sz(24), alpha(on ? WHITE : TAB_TEXT, pop), name);
        if (bw > lw + 1) {
            ImVec2 b0 = v.pt(x + lw, top), b1 = v.pt(x + bw, top + bh);
            dl->AddRectFilled(b0, b1, alpha(PANEL, pop));
            dl->PushClipRect(b0, b1, true);
            if (on) {
                float by = top + bar_[bi].value(now) * 66;
                ImVec2 r0 = v.pt(x + lw, by), r1 = v.pt(x + lw + 736, by + 66);
                dl->AddRectFilled(r0, r1, alpha(ACCENT, pop));
                dl->AddLine(ImVec2(r0.x, r1.y - v.sz(0.5f)), ImVec2(r1.x, r1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
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
                ImVec2 r0 = v.pt(x + lw + slide, y), r1 = v.pt(x + lw + 736 + slide, y + 66);
                if (!hi) dl->AddLine(ImVec2(r0.x, r1.y - v.sz(0.5f)), ImVec2(r1.x, r1.y - v.sz(0.5f)), alpha(LINE, pop * rop), v.sz(1));
                float cy = (r0.y + r1.y) * 0.5f;
                text_mid(dl, tf, a.text_m, v.sz(26), r0.x + v.sz(30), cy, alpha(WHITE, pop * rop), r.label);
                float right = r1.x - v.sz(22);
                if (r.icon) {
                    icon_mid(dl, a, v.sz(32), right - v.sz(16), cy, alpha(WHITE, pop * rop), r.icon);
                    right -= v.sz(32 + 16);
                }
                const char* meta = bi == 1 && i == 0 ? friends_meta : bi == 1 && i == 2 ? messages_meta : bi == 2 && i == 1 ? "Xbox" : "";
                if (*meta) text_mid(dl, tf, a.text_m, v.sz(20), right, cy, alpha(hi ? WHITE : MUTED, pop * rop), meta, true);
            }
            dl->PopClipRect();
        }
        x += bw;
    }
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, 187, 606, hints, 4, op);
}

// Artboard 4: the friend list. Tabs Party / Friends / Club / Recent (42 units), six 69-unit rows, a 55-unit footer; the panel is 862 wide
// at x 205. A row: the picture (60), the name and a second line (330 wide), the presence icon and two lines of where they are.
void Guide::draw_list(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = 205, W = 862;
    float te = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1;
    draw_header(dl, a, origin, s, zoom, op, L, W, page_ == Page::Invite ? "Invite to Game" : "Friends", te);
    float pe = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f)) : 1;
    float slide = (1 - pe) * 110 * page_dir_, pop = op * pe;

    std::vector<const Friend*> lists[4] = {tab_list(0), tab_list(1), tab_list(2), tab_list(3)};
    int online = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    // tabs
    float tx = L + slide;
    for (int t = 0; t < 4; t++) {
        bool on = t == tab_;
        ImVec2 t0 = v.pt(tx, 96), t1 = v.pt(tx + TAB_W[t], 138);
        dl->AddRectFilled(t0, t1, alpha(on ? PANEL : TAB, pop));
        char label[48];
        if (t == 1) snprintf(label, sizeof label, "Friends (%d Online)", online);
        else snprintf(label, sizeof label, "%d", int(lists[t].size()));
        float lw = a.text->CalcTextSizeA(22, FLT_MAX, 0, label).x, cw = 26 + 12 + lw;
        float cx = tx + (TAB_W[t] - cw) / 2, cy = 117;
        ImU32 fg = on ? WHITE : TAB_TEXT;
        ImVec2 ic = v.pt(cx + 13, cy);
        icon_mid(dl, a, v.sz(26), ic.x, ic.y, alpha(fg, pop), TAB_ICON[t]);
        ImVec2 lp = v.pt(cx + 38, cy);
        text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(fg, pop), label);
        tx += TAB_W[t];
    }
    // list
    ImVec2 p0 = v.pt(L + slide, 138), p1 = v.pt(L + W + slide, 552);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->PushClipRect(p0, p1, true);
    const std::vector<const Friend*>& list = lists[tab_];
    float le = ease_guide(std::clamp(float((now - tab_at_) / 0.26), 0.0f, 1.0f));
    float ls = slide + (1 - le) * 110 * tab_dir_, lop = pop * le;
    if (!social_.session || (!social_.loaded && !social_.error.empty()) || list.empty()) {
        const char* why = !social_.session ? "Not signed in: start the game with Play in the FH2MP launcher"
                          : !social_.loaded && !social_.error.empty() ? "The FH2MP server can't be reached right now"
                          : !social_.loaded ? "Loading your friends..."
                          : tab_ == 0 ? "None of your friends are in your game"
                          : tab_ == 2 ? (social_.me.club.empty() ? "You are not in a club" : "No friends in your club")
                          : tab_ == 3 ? "No recent players yet"
                                      : "No friends yet: add them in the FH2MP launcher";
        float ww = a.text->CalcTextSizeA(22, FLT_MAX, 0, why).x;
        ImVec2 c = v.pt(L + ls + (W - ww) / 2, 345);
        text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(MUTED, lop), why);
    } else {
        float by = 138 + list_bar_.value(now) * 69;
        ImVec2 b0 = v.pt(L + ls, by), b1 = v.pt(L + W + ls, by + 69);
        dl->AddRectFilled(b0, b1, alpha(ACCENT, lop));
        dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, lop), v.sz(1));
        for (int i = top_[tab_]; i < std::min(int(list.size()), top_[tab_] + LIST_ROWS); i++) {
            const Friend& f = *list[i];
            float y = 138 + (i - top_[tab_]) * 69, cy = y + 34.5f;
            bool foc = i == row_[tab_];
            bool here = in_my_game(f, social_.me), off = f.state == Friend::Offline, sent = sent_.count(f.xuid) > 0;
            if (!foc) {
                ImVec2 l0 = v.pt(L + ls, y + 69), l1 = v.pt(L + W + ls, y + 69);
                dl->AddLine(ImVec2(l0.x, l0.y - v.sz(0.5f)), ImVec2(l1.x, l1.y - v.sz(0.5f)), alpha(LINE, lop), v.sz(1));
            }
            player_pic(dl, a, a.pic(f.xuid), f.xuid, v.pt(L + ls + 9, y + 4.5f), v.pt(L + ls + 69, y + 64.5f), lop);
            ImU32 fg = foc ? WHITE : off ? MUTED2 : WHITE, sub = foc ? WHITE : MUTED2;
            // name column: 22 and 20 units, line-height 1.15, 3 apart, centred: the lines' centres 13 above and 14 below the row's
            ImVec2 n0 = v.pt(L + ls + 78, cy - 13), n1 = v.pt(L + ls + 78, cy + 14.15f);
            text_fit(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(305), alpha(fg, lop), f.tag);
            std::string second = !f.club.empty() ? "[" + f.club + "]" : f.level > 0 ? "Level " + std::to_string(f.level) : "";
            if (!second.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), n1.x, n1.y, v.sz(305), alpha(sub, lop), second);
            // presence
            ImU32 ic = foc ? WHITE : (f.state == Friend::Playing || here) ? BRIGHT : f.state == Friend::Online ? ONLINE : OFFLINE;
            ImVec2 pi = v.pt(L + ls + 399 + 14, cy);
            icon_mid(dl, a, v.sz(28), pi.x, pi.y, alpha(ic, lop), I_PERSON);
            std::string status = f.state == Friend::Playing ? "Forza Horizon 2" : f.state == Friend::Online ? "Online" : "Offline";
            std::string detail = sent ? "Invite sent" : here ? "In your game" : f.state == Friend::Playing ? lobby_words(f) : f.state == Friend::Online ? "In the FH2MP launcher" : "";
            float sw = W - 445 - 20;
            ImVec2 s0 = v.pt(L + ls + 445, cy - 13), s1 = v.pt(L + ls + 445, cy + 14.15f);
            text_fit(dl, a.text, a.text_m, v.sz(22), s0.x, s0.y, v.sz(sw), alpha(fg, lop), status);
            if (!detail.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), s1.x, s1.y, v.sz(sw), alpha(foc ? WHITE : sent ? BRIGHT : MUTED2, lop), detail);
        }
    }
    dl->PopClipRect();
    // footer
    ImVec2 f0 = v.pt(L + slide, 552), f1 = v.pt(L + W + slide, 607);
    dl->AddRectFilled(f0, f1, alpha(PANEL, pop));
    dl->AddLine(ImVec2(f0.x, f0.y + v.sz(0.5f)), ImVec2(f1.x, f0.y + v.sz(0.5f)), alpha(LINE, pop), v.sz(1));
    ImVec2 fl = v.pt(L + slide + 27, 579.5f);
    text_mid(dl, a.text, a.text_m, v.sz(20), fl.x, fl.y, alpha(MUTED, pop), "Sorted by online status");
    ImVec2 fr = v.pt(L + slide + W - 30, 579.5f);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(14), fr.y, alpha(MUTED, pop), I_EXPAND_MORE);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(42), fr.y, alpha(MUTED, pop), I_EXPAND_LESS);
    char count[32];
    snprintf(count, sizeof count, "%d of %d", list.empty() ? 0 : row_[tab_] + 1, int(list.size()));
    text_mid(dl, a.text, a.text_m, v.sz(20), fr.x - v.sz(64), fr.y, alpha(MUTED, pop), count, true);
    const Hint hints[] = {{"A", page_ == Page::Invite ? "Invite" : "Invite to Game", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, L + 27, 617, hints, 4, op);
}

// Artboard 5: a received game invite. Left (431): when it came, then Accept / Decline (56-unit rows); right (431): the sender's card and the
// message.
void Guide::draw_received(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = 205, W = 862;
    int n = int(social_.invites.size());
    char title[48];
    if (n > 1) snprintf(title, sizeof title, "Game Invite (%d of %d)", received_ + 1, n);
    else snprintf(title, sizeof title, "%s", n ? "Game Invite" : "Messages");
    float te = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1;
    draw_header(dl, a, origin, s, zoom, op, L, W, title, te);
    float pe = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f)) : 1;
    float x = L + (1 - pe) * 110 * page_dir_, pop = op * pe;
    if (!n) {
        ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + W, 607);
        dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
        const char* why = !social_.session ? "Not signed in: start the game with Play in the FH2MP launcher" : "No game invites";
        float ww = a.text->CalcTextSizeA(22, FLT_MAX, 0, why).x;
        ImVec2 c = v.pt(x + (W - ww) / 2, 351);
        text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(MUTED, pop), why);
        const Hint hints[] = {{"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 1, op);
        return;
    }
    const ReceivedInvite& iv = social_.invites[received_];
    // left: when (22 units, line-height 1.2, 6 apart, from 26 down)
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + 431, 607), alpha(PANEL, pop));
    SYSTEMTIME st;
    GetLocalTime(&st);
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
    u.QuadPart -= ULONGLONG(iv.age) * 10000000ULL;
    ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
    FileTimeToSystemTime(&ft, &st);
    char date[32], clock[32];
    snprintf(date, sizeof date, "%d/%d/%d", st.wMonth, st.wDay, st.wYear);
    snprintf(clock, sizeof clock, "%d:%02d  %s", st.wHour % 12 == 0 ? 12 : st.wHour % 12, st.wMinute, st.wHour < 12 ? "AM" : "PM");
    std::string lines[4] = {"Message Received", date, clock, ago(iv.age)};
    for (int i = 0; i < 4; i++) {
        ImVec2 p = v.pt(x + 27, 96 + 26 + 13.2f + i * 32.4f);
        text_mid(dl, a.text, a.text_m, v.sz(22), p.x, p.y, alpha(i == 0 ? MUTED : WHITE, pop), lines[i].c_str());
    }
    const char* opts[2] = {"Accept", "Decline"};
    float my = 96 + 231;
    float by = my + received_bar_.value(now) * 56;
    ImVec2 b0 = v.pt(x, by), b1 = v.pt(x + 431, by + 56);
    dl->AddRectFilled(b0, b1, alpha(ACCENT, pop));
    dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
    for (int i = 0; i < 2; i++) {
        float y = my + i * 56;
        if (i != received_opt_) dl->AddLine(v.pt(x, y + 55.5f), v.pt(x + 431, y + 55.5f), alpha(LINE, pop), v.sz(1));
        ImVec2 p = v.pt(x + 27, y + 28);
        text_mid(dl, a.text, a.text_m, v.sz(24), p.x, p.y, alpha(WHITE, pop), opts[i]);
    }
    // right: the sender's card (22 in): a 46-unit name bar, the 128-unit picture, the facts; then the message
    float rx = x + 431;
    dl->AddRectFilled(v.pt(rx, 96), v.pt(rx + 431, 607), alpha(PANE, pop));
    float cx = rx + 22, cw = 431 - 44, cy0 = 96 + 22;
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46 + 128), alpha(PANEL, pop));
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46), alpha(ACCENT, pop));
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    ImVec2 np = v.pt(cx + 16, cy0 + 23);
    text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(cw - 32), alpha(WHITE, pop), iv.from.tag);
    player_pic(dl, a, a.pic(iv.from.xuid), iv.from.xuid, v.pt(cx, cy0 + 46), v.pt(cx + 128, cy0 + 46 + 128), pop);
    std::string facts[3][2] = {{"Club", iv.from.club.empty() ? "-" : iv.from.club},
                               {"Level", iv.from.level > 0 ? std::to_string(iv.from.level) : "-"},
                               {"Zone", iv.kind == "roadtrip" ? "Road Trip" : "Free Roam"}};
    for (int i = 0; i < 3; i++) {
        float fy = cy0 + 46 + 64 + (i - 1) * 31.2f;
        ImVec2 l = v.pt(cx + 128 + 16, fy), r = v.pt(cx + cw - 16, fy);
        text_mid(dl, a.text, a.text_m, v.sz(21), l.x, l.y, alpha(WHITE, pop), facts[i][0].c_str());
        text_mid(dl, a.text, a.text_m, v.sz(21), r.x, r.y, alpha(WHITE, pop), facts[i][1].c_str(), true);
    }
    float my2 = cy0 + 46 + 128 + 30;
    ImVec2 mi = v.pt(rx + 32 + 15, my2 + 16);
    icon_mid(dl, a, v.sz(30), mi.x, mi.y, alpha(MUTED, pop), I_MAIL);
    ImVec2 mt = v.pt(rx + 32 + 40, my2 + 16);
    text_mid(dl, a.text, a.text_m, v.sz(24), mt.x, mt.y, alpha(MUTED, pop), "Message Text");
    std::string body = iv.from.tag + " invites you to play Forza Horizon 2.";
    ImVec2 bp = v.pt(rx + 32, my2 + 32 + 8);
    dl->AddText(a.text, v.sz(24), bp, alpha(WHITE, pop), body.c_str(), nullptr, v.sz(431 - 64));
    if (!iv.text.empty()) {
        float bh = a.text->CalcTextSizeA(24, FLT_MAX, 431 - 64, body.c_str()).y;
        ImVec2 tp = v.pt(rx + 32, my2 + 40 + bh + 8);
        dl->AddText(a.text, v.sz(22), tp, alpha(MUTED, pop), iv.text.c_str(), nullptr, v.sz(431 - 64));
    }
    if (n > 1) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
        hint_bar(dl, a, v, L + 27, 617, hints, 4, op);
    } else {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
    }
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
        // two 22-unit lines centred in the 64-unit panel: their line boxes are centred on by + 21 and by + 43
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
        icon_mid(dl, a, 46 * s * zoom, c.x, c.y - up, alpha(WHITE, zop), toast_icon_);
        if (toast_pic_)
            dl->AddImage(toast_pic_, ImVec2(q0.x, q0.y + side - up), ImVec2(q1.x, q1.y + side - up), ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, zop));
        dl->PopClipRect();
    }
}
