// The Guide's drawing and navigation, from the design canvas: artboard 1 (Guide home: blades), 4 (the friend list: Friends and Invite to
// Game), 5 and the 360's Message screen (one message of the Message Center), 7 (the notification toast). Everything is laid out in the
// canvas' 1280x720 units, scaled to the game's frame and centred; the Guide itself is drawn at 0.84 of the canvas and a little higher, so
// a toast at the canvas' place (y 600) shows under it. Motion follows the canvas' CSS: cubic-bezier(.1,.9,.2,1) for the open (scale
// .92 -> 1, 0.26 s), the blade widths (0.3 s), the focus bars (0.14 s), the rows sliding in after a blade switch (150 units, 0.32 s,
// 40 ms + 28 ms per row), a page or tab coming in (110 units, 0.26 s) and the title fading (0.16 s); the toast plays its 7.5 s keyframes
// once.
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

// The Guide's size against the canvas and the centre it is drawn around (canvas units).
const float GUIDE_K = 0.84f, GUIDE_CY = 330;

enum : uint32_t {
    I_PERSON = 0xe7fd, I_GROUPS = 0xf233, I_MAIL = 0xe158, I_SENSORS = 0xe51e, I_CHAT = 0xe0ca, I_GROUP_ADD = 0xe7f0, I_GAMES = 0xea28,
    I_TROPHY = 0xea23, I_PALETTE = 0xe40a, I_MIC = 0xe029, I_BELL = 0xe7f4, I_SETTINGS = 0xe8b8, I_FLAG = 0xe153, I_SCHEDULE = 0xe8b5,
    I_EXPAND_LESS = 0xe5ce, I_EXPAND_MORE = 0xe5cf, I_APPS = 0xe5c3, I_WIFI = 0xe63e, I_PERSON_ADD = 0xe7fe
};

struct Row {
    const char* label;
    uint32_t icon;
    GuideAction action;
    Page page;  // where A goes when action is None
};

// The blades' rows as the canvas has them (artboard 1).
const Row GAMES[] = {{"Forza Horizon 2", I_GAMES, GuideAction::Resume, Page::Home},
                     {"Achievements", I_TROPHY, GuideAction::Soon, Page::Home},
                     {"Game Library", I_APPS, GuideAction::Soon, Page::Home}};
const Row ME[] = {{"Leave Game", 0, GuideAction::Soon, Page::Home},
                  {"Friends", I_PERSON, GuideAction::None, Page::Friends},
                  {"Party", I_GROUPS, GuideAction::Soon, Page::Home},
                  {"Messages", I_MAIL, GuideAction::None, Page::Messages},
                  {"Beacons & Activity", I_SENSORS, GuideAction::Soon, Page::Home},
                  {"Chat", I_CHAT, GuideAction::Soon, Page::Home},
                  {"Lobbies", I_FLAG, GuideAction::Soon, Page::Home}};
const Row SETTINGS[] = {{"Profile", I_PERSON, GuideAction::Soon, Page::Home},       {"Theme", I_PALETTE, GuideAction::Soon, Page::Home},
                        {"Voice", I_MIC, GuideAction::Soon, Page::Home},            {"Notifications", I_BELL, GuideAction::Soon, Page::Home},
                        {"Network", I_WIFI, GuideAction::Soon, Page::Home},         {"Controller", I_GAMES, GuideAction::Soon, Page::Home},
                        {"System", I_SETTINGS, GuideAction::Soon, Page::Home}};
const Row* ROWS[3] = {GAMES, ME, SETTINGS};
const int ROW_COUNT[3] = {3, 7, 7};
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

// Where AddText must start so that the capital letters are centred on cy (a word next to a round key looks centred on it).
float caps_top(const FontMetrics& m, float size, float cy) { return cy + size * m.cap * 0.5f - size * m.asc / (m.asc + m.desc); }

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

// A key of the hint bar: a 20-unit circle (A, B, X, Y) or a 34x22 rounded box (LB, RB) with a 2-unit ring inside its edge, centred on cy,
// its letters centred by their capital height. Returns its width in pixels.
float hint_key(ImDrawList* dl, const GuideAssets& a, const char* key, ImU32 ring, float x, float cy, float s, float op) {
    bool wide = strlen(key) > 1;
    float kw = (wide ? 34 : 20) * s, kh = (wide ? 22 : 20) * s, rr = (wide ? 6 : 10) * s;
    ImVec2 k0(x, cy - kh / 2), k1(x + kw, cy + kh / 2);
    dl->AddRectFilled(k0, k1, alpha(BLACK, op), rr);
    dl->AddRect(ImVec2(k0.x + s, k0.y + s), ImVec2(k1.x - s, k1.y - s), alpha(ring, op), rr - s, 0, 2 * s);
    ImFont* kf = a.text_bold ? a.text_bold : a.text;
    float size = (wide ? 11 : 12) * s;
    float tw = kf->CalcTextSizeA(size, FLT_MAX, 0, key).x;
    dl->AddText(kf, size, ImVec2(x + (kw - tw) * 0.5f, caps_top(a.bold_m, size, cy)), alpha(ring, op), key);
    return kw;
}

// Toasts take the theme's accent; only a failure is red.
ImU32 toast_color(ToastKind k) { return k == ToastKind::Warning ? IM_COL32(0xee, 0x3b, 0x23, 255) : ACCENT; }

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
const ImU32 RING_A = IM_COL32(0x5f, 0xd1, 0x1c, 255), RING_B = IM_COL32(0xff, 0x4a, 0x4f, 255);

// The hint bar: a flex row 10 units under the panel, 27 in, 19-unit text, gaps 22 between hints and 7 inside one, a hint without a label
// sits 8 from the next (LB RB). The keys and the words share one centre line (the words by their capitals).
void hint_bar(ImDrawList* dl, const GuideAssets& a, const View& v, float x, float top, const Hint* hints, int n, float op) {
    float cy = top + 12.5f;
    for (int i = 0; i < n; i++) {
        ImVec2 p = v.pt(x, cy);
        x += hint_key(dl, a, hints[i].key, hints[i].ring, p.x, p.y, v.sz(1), op) / v.sz(1);
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
    if (secs < 0) return "";
    if (secs < 60) return "Just now";
    if (secs < 120) return "1 minute ago";
    if (secs < 3600) return std::to_string(secs / 60) + " minutes ago";
    if (secs < 7200) return "1 hour ago";
    return std::to_string(secs / 3600) + " hours ago";
}

std::string expires(int secs) {
    if (secs < 60) return "Expires in under a minute";
    if (secs < 120) return "Expires in 1 minute";
    return "Expires in " + std::to_string(secs / 60) + " minutes";
}

const char* kind_word(MessageItem::Kind k) {
    return k == MessageItem::GameInvite ? "Game Invite" : k == MessageItem::FriendRequest ? "Friend Request" : "Message";
}

uint32_t kind_icon(MessageItem::Kind k) { return k == MessageItem::GameInvite ? I_GAMES : k == MessageItem::FriendRequest ? I_PERSON_ADD : I_MAIL; }

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
        focus_[1] = 1, bar_[1].from = bar_[1].to = 1;  // canvas: the player's blade opens on Friends
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
    if (p == Page::Messages) {
        int n = int(messages().size());
        msg_row_ = std::clamp(msg_row_, 0, std::max(0, n - 1));
        msg_top_ = std::clamp(msg_top_, 0, std::max(0, n - LIST_ROWS));
        msg_bar_.go(float(msg_row_ - msg_top_), now, 0);
    }
    if (p == Page::Message) msg_opt_ = 0, opt_bar_.go(0, now, 0);
}

void Guide::set_social(const Social& s) {
    social_ = s;
    for (int t = 0; t < 4; t++) {
        int n = int(tab_list(t).size());
        row_[t] = std::clamp(row_[t], 0, std::max(0, n - 1));
        top_[t] = std::clamp(top_[t], 0, std::max(0, n - LIST_ROWS));
    }
    int n = int(messages().size());
    msg_row_ = std::clamp(msg_row_, 0, std::max(0, n - 1));
    msg_top_ = std::clamp(msg_top_, 0, std::max(0, n - LIST_ROWS));
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

// The Message Center: game invites (newest first), friend requests, then friends' messages (unread first).
std::vector<MessageItem> Guide::messages() const {
    std::vector<MessageItem> out;
    for (const ReceivedInvite& iv : social_.invites) {
        MessageItem m;
        m.kind = MessageItem::GameInvite, m.from = iv.from, m.id = iv.id, m.text = iv.text, m.age = iv.age, m.expires_in = iv.expires_in;
        m.unread = true;
        out.push_back(m);
    }
    for (const Friend& f : social_.requests) {
        MessageItem m;
        m.kind = MessageItem::FriendRequest, m.from = f, m.unread = true;
        out.push_back(m);
    }
    std::vector<MessageItem> texts;
    for (const ChatThread& c : social_.chats) {
        if (c.last_incoming <= 0 || c.id == "club") continue;
        MessageItem m;
        m.kind = MessageItem::Text, m.chat = c.id, m.id = c.last_incoming, m.text = c.last, m.unread = c.unread > 0;
        m.from.xuid = strtoull(c.id.c_str(), nullptr, 10), m.from.tag = c.title;
        for (const Friend& f : social_.friends)
            if (f.xuid == m.from.xuid) m.from = f;
        texts.push_back(m);
    }
    std::stable_sort(texts.begin(), texts.end(), [](const MessageItem& x, const MessageItem& y) { return x.unread > y.unread; });
    out.insert(out.end(), texts.begin(), texts.end());
    return out;
}

std::vector<const char*> Guide::message_options(const MessageItem& m) const {
    switch (m.kind) {
    case MessageItem::GameInvite: return {"Accept Invite", "Reply", "View Profile", "Delete", "Block Communications"};
    case MessageItem::FriendRequest: return {"Accept Friend Request", "Decline", "View Profile", "Block Communications"};
    case MessageItem::Text: return {"Reply", "View Profile", "Block Communications"};
    }
    return {};
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
    case Page::Messages: return handle_messages(e, now);
    case Page::Message: return handle_message(e, now);
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
            toast(ToastKind::Info, row.label, "Coming in a later build", row.icon ? row.icon : I_GAMES, 0, now);
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
        // any friend but one already in your game (canvas: that row does nothing); an offline friend finds it in Messages later
        const Friend& f = *list[row_[tab_]];
        if (!in_my_game(f, social_.me)) r.action = GuideAction::Invite, r.xuid = f.xuid, r.name = f.tag;
        break;
    }
    default: break;
    }
    return r;
}

GuideRequest Guide::handle_messages(PadEvent e, double now) {
    GuideRequest r;
    std::vector<MessageItem> list = messages();
    int n = int(list.size());
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        if (!n) break;
        int f = std::clamp(msg_row_ + (e == PadEvent::Up ? -1 : 1), 0, n - 1);
        msg_row_ = f;
        if (f < msg_top_) msg_top_ = f;
        if (f >= msg_top_ + LIST_ROWS) msg_top_ = f - LIST_ROWS + 1;
        msg_bar_.go(float(f - msg_top_), now, 0.14f);
        break;
    }
    case PadEvent::B: go_page(Page::Home, -1, now); break;
    case PadEvent::A: {
        if (!n) break;
        open_msg_ = list[msg_row_];
        go_page(Page::Message, 1, now);
        if (open_msg_.kind == MessageItem::Text && open_msg_.unread) {
            r.action = GuideAction::ReadChat, r.id = open_msg_.id, r.name = open_msg_.chat;
            for (ChatThread& c : social_.chats)
                if (c.id == open_msg_.chat) c.unread = 0;
        }
        break;
    }
    default: break;
    }
    return r;
}

GuideRequest Guide::handle_message(PadEvent e, double now) {
    GuideRequest r;
    std::vector<const char*> opts = message_options(open_msg_);
    int n = int(opts.size());
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        msg_opt_ = std::clamp(msg_opt_ + (e == PadEvent::Up ? -1 : 1), 0, n - 1);
        opt_bar_.go(float(msg_opt_), now, 0.14f);
        break;
    case PadEvent::B: go_page(Page::Messages, -1, now); break;
    case PadEvent::A: {
        std::string opt = opts[msg_opt_];
        const MessageItem& m = open_msg_;
        r.xuid = m.from.xuid, r.name = m.from.tag, r.id = m.id;
        if (m.kind == MessageItem::GameInvite && (opt == "Accept Invite" || opt == "Delete")) {
            r.action = opt == "Accept Invite" ? GuideAction::Accept : GuideAction::Decline;
            for (size_t i = 0; i < social_.invites.size(); i++)
                if (social_.invites[i].id == m.id) social_.invites.erase(social_.invites.begin() + i);  // until the server's list says so
        } else if (m.kind == MessageItem::FriendRequest && (opt == "Accept Friend Request" || opt == "Decline")) {
            r.action = opt == "Decline" ? GuideAction::DeclineFriend : GuideAction::AcceptFriend;
            for (size_t i = 0; i < social_.requests.size(); i++)
                if (social_.requests[i].xuid == m.from.xuid) social_.requests.erase(social_.requests.begin() + i);
        } else {
            toast(ToastKind::Info, opt.c_str(), "Coming in a later build", kind_icon(m.kind), 0, now);
            r.action = GuideAction::Soon;
            break;
        }
        go_page(Page::Messages, -1, now);
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
        case Page::Messages: draw_messages(dl, a, origin, s, zoom, op, now); break;
        case Page::Message: draw_message(dl, a, origin, s, zoom, op, now); break;
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
    // coming back from a page: the blades slide in like a page does
    float pe = ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f));
    float pslide = (1 - pe) * 110 * page_dir_, pop = op * (page_at_ > 0 ? pe : 1);
    const char* title = blade_ == 1 ? gamertag.c_str() : BLADE[blade_];
    draw_header(dl, a, origin, s, zoom, op, 160, 960, title, page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1);

    int online = 0, waiting = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    for (const MessageItem& m : messages()) waiting += m.unread;
    char friends_meta[16] = "", messages_meta[16] = "";
    if (social_.loaded) snprintf(friends_meta, sizeof friends_meta, "%d", online);
    if (waiting) snprintf(messages_meta, sizeof messages_meta, "%d", waiting);

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
                if (r.icon) icon_mid(dl, a, v.sz(32), right - v.sz(16), cy, alpha(WHITE, pop * rop), r.icon);
                right -= v.sz(32 + 16);
                const char* meta = bi == 1 && i == 1 ? friends_meta : bi == 1 && i == 3 ? messages_meta : bi == 2 && i == 1 ? "Xbox" : bi == 0 && i == 0 ? "Now playing" : "";
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

// The Message Center's list (laid out like artboard 4's rows): the sender's picture, name and what it is; on the right the kind's icon,
// the kind and when.
void Guide::draw_messages(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = 205, W = 862;
    float te = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1;
    draw_header(dl, a, origin, s, zoom, op, L, W, "Messages", te);
    float pe = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f)) : 1;
    float x = L + (1 - pe) * 110 * page_dir_, pop = op * pe;
    std::vector<MessageItem> list = messages();
    int unread = 0;
    for (auto& m : list) unread += m.unread;
    // one tab across the top, like the friend list's
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + W, 138), alpha(PANEL, pop));
    char label[48];
    snprintf(label, sizeof label, "Messages (%d New)", unread);
    float lw = a.text->CalcTextSizeA(22, FLT_MAX, 0, label).x, cw = 26 + 12 + lw, cx = x + (W - cw) / 2;
    ImVec2 ic = v.pt(cx + 13, 117), lp = v.pt(cx + 38, 117);
    icon_mid(dl, a, v.sz(26), ic.x, ic.y, alpha(WHITE, pop), I_MAIL);
    text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(WHITE, pop), label);
    ImVec2 p0 = v.pt(x, 138), p1 = v.pt(x + W, 552);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->PushClipRect(p0, p1, true);
    if (list.empty()) {
        const char* why = !social_.session ? "Not signed in: start the game with Play in the FH2MP launcher" : "No messages";
        float ww = a.text->CalcTextSizeA(22, FLT_MAX, 0, why).x;
        ImVec2 c = v.pt(x + (W - ww) / 2, 345);
        text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(MUTED, pop), why);
    } else {
        float by = 138 + msg_bar_.value(now) * 69;
        ImVec2 b0 = v.pt(x, by), b1 = v.pt(x + W, by + 69);
        dl->AddRectFilled(b0, b1, alpha(ACCENT, pop));
        dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
        ImFont* bf = a.text_bold ? a.text_bold : a.text;
        for (int i = msg_top_; i < std::min(int(list.size()), msg_top_ + LIST_ROWS); i++) {
            const MessageItem& m = list[i];
            float y = 138 + (i - msg_top_) * 69, cy = y + 34.5f;
            bool foc = i == msg_row_;
            if (!foc) dl->AddLine(v.pt(x, y + 68.5f), v.pt(x + W, y + 68.5f), alpha(LINE, pop), v.sz(1));
            player_pic(dl, a, a.pic(m.from.xuid), m.from.xuid, v.pt(x + 9, y + 4.5f), v.pt(x + 69, y + 64.5f), pop);
            ImU32 sub = foc ? WHITE : MUTED2;
            ImVec2 n0 = v.pt(x + 78, cy - 13), n1 = v.pt(x + 78, cy + 14.15f);
            text_fit(dl, m.unread ? bf : a.text, m.unread ? a.bold_m : a.text_m, v.sz(22), n0.x, n0.y, v.sz(305), alpha(WHITE, pop), m.from.tag);
            std::string second = m.kind == MessageItem::Text ? m.text : m.kind == MessageItem::GameInvite ? "Forza Horizon 2" : "Wants to be your friend";
            text_fit(dl, a.text, a.text_m, v.sz(20), n1.x, n1.y, v.sz(305), alpha(sub, pop), second);
            ImVec2 pi = v.pt(x + 399 + 14, cy);
            icon_mid(dl, a, v.sz(28), pi.x, pi.y, alpha(foc ? WHITE : m.unread ? BRIGHT : ONLINE, pop), kind_icon(m.kind));
            ImVec2 s0 = v.pt(x + 445, cy - 13), s1 = v.pt(x + 445, cy + 14.15f);
            text_mid(dl, a.text, a.text_m, v.sz(22), s0.x, s0.y, alpha(WHITE, pop), kind_word(m.kind));
            std::string when = m.kind == MessageItem::GameInvite ? ago(m.age) : m.kind == MessageItem::Text ? (m.unread ? "Unread" : "Read") : "";
            if (!when.empty()) text_mid(dl, a.text, a.text_m, v.sz(20), s1.x, s1.y, alpha(sub, pop), when.c_str());
        }
    }
    dl->PopClipRect();
    ImVec2 f0 = v.pt(x, 552), f1 = v.pt(x + W, 607);
    dl->AddRectFilled(f0, f1, alpha(PANEL, pop));
    dl->AddLine(ImVec2(f0.x, f0.y + v.sz(0.5f)), ImVec2(f1.x, f0.y + v.sz(0.5f)), alpha(LINE, pop), v.sz(1));
    ImVec2 fl = v.pt(x + 27, 579.5f);
    text_mid(dl, a.text, a.text_m, v.sz(20), fl.x, fl.y, alpha(MUTED, pop), "Invites and requests first");
    char count[32];
    snprintf(count, sizeof count, "%d of %d", list.empty() ? 0 : msg_row_ + 1, int(list.size()));
    ImVec2 fr = v.pt(x + W - 30, 579.5f);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(14), fr.y, alpha(MUTED, pop), I_EXPAND_MORE);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(42), fr.y, alpha(MUTED, pop), I_EXPAND_LESS);
    text_mid(dl, a.text, a.text_m, v.sz(20), fr.x - v.sz(64), fr.y, alpha(MUTED, pop), count, true);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}

// One message (artboard 5, the 360's Message screen): left, when it came and the options (56-unit rows); right, the sender's card and the
// message text.
void Guide::draw_message(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = 205, W = 862;
    const MessageItem& m = open_msg_;
    float te = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1;
    draw_header(dl, a, origin, s, zoom, op, L, W, "Message", te);
    float pe = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f)) : 1;
    float x = L + (1 - pe) * 110 * page_dir_, pop = op * pe;
    // left: when (22 units, line-height 1.2, from 26 down)
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + 431, 607), alpha(PANEL, pop));
    std::vector<std::string> lines = {"Message Received"};
    if (m.age >= 0) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILETIME ft;
        SystemTimeToFileTime(&st, &ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime, u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= ULONGLONG(m.age) * 10000000ULL;
        ft.dwLowDateTime = u.LowPart, ft.dwHighDateTime = u.HighPart;
        FileTimeToSystemTime(&ft, &st);
        char date[32], clock[32];
        snprintf(date, sizeof date, "%d/%d/%d", st.wMonth, st.wDay, st.wYear);
        snprintf(clock, sizeof clock, "%d:%02d  %s", st.wHour % 12 == 0 ? 12 : st.wHour % 12, st.wMinute, st.wHour < 12 ? "AM" : "PM");
        lines.push_back(date), lines.push_back(clock);
    }
    if (m.expires_in >= 0) lines.push_back(expires(m.expires_in));
    else if (m.kind == MessageItem::Text) lines.push_back(m.unread ? "Unread" : "Read");
    for (size_t i = 0; i < lines.size(); i++) {
        ImVec2 p = v.pt(x + 27, 96 + 26 + 13.2f + i * 32.4f);
        text_mid(dl, a.text, a.text_m, v.sz(22), p.x, p.y, alpha(i == 0 ? MUTED : WHITE, pop), lines[i].c_str());
    }
    std::vector<const char*> opts = message_options(m);
    float my = 96 + 231;
    dl->AddLine(v.pt(x, my - 0.5f), v.pt(x + 431, my - 0.5f), alpha(LINE, pop), v.sz(1));
    float by = my + opt_bar_.value(now) * 56;
    ImVec2 b0 = v.pt(x, by), b1 = v.pt(x + 431, by + 56);
    dl->AddRectFilled(b0, b1, alpha(ACCENT, pop));
    dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
    for (size_t i = 0; i < opts.size(); i++) {
        float y = my + i * 56;
        if (int(i) != msg_opt_) dl->AddLine(v.pt(x, y + 55.5f), v.pt(x + 431, y + 55.5f), alpha(LINE, pop), v.sz(1));
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
    text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(cw - 32), alpha(WHITE, pop), m.from.tag);
    player_pic(dl, a, a.pic(m.from.xuid), m.from.xuid, v.pt(cx, cy0 + 46), v.pt(cx + 128, cy0 + 46 + 128), pop);
    std::string zone = m.kind == MessageItem::GameInvite ? "Free Roam"
                       : m.from.state == Friend::Playing ? lobby_words(m.from) : m.from.state == Friend::Online ? "Online" : "Offline";
    std::string facts[3][2] = {{"Club", m.from.club.empty() ? "-" : m.from.club},
                               {"Level", m.from.level > 0 ? std::to_string(m.from.level) : "-"},
                               {"Zone", zone}};
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
    std::string body = m.kind == MessageItem::GameInvite      ? m.from.tag + " invites you to play Forza Horizon 2."
                       : m.kind == MessageItem::FriendRequest ? m.from.tag + " wants to be your friend."
                                                              : m.text;
    ImVec2 bp = v.pt(rx + 32, my2 + 32 + 8);
    dl->AddText(a.text, v.sz(24), bp, alpha(WHITE, pop), body.c_str(), nullptr, v.sz(431 - 64));
    if (m.kind == MessageItem::GameInvite && !m.text.empty()) {
        float bh = a.text->CalcTextSizeA(24, FLT_MAX, 431 - 64, body.c_str()).y;
        ImVec2 tp = v.pt(rx + 32, my2 + 40 + bh + 8);
        dl->AddText(a.text, v.sz(22), tp, alpha(MUTED, pop), m.text.c_str(), nullptr, v.sz(431 - 64));
    }
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
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
    float bx = 640 - (64 + pw) * 0.5f, by = 600;  // canvas: y 600 (the Guide is drawn small enough to end above it)
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
