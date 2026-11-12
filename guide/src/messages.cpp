// Messages, as the 360 had them: one message with its options (artboard 5), Create New (artboard 3), the recipients list and the
// compose screen (artboard 4's flow), and the on-screen keyboard (artboard 6).
#include <windows.h>

#include <algorithm>
#include <cstdio>

#include "guide.h"
#include "ui.h"

using namespace ui;

namespace {

const int PICK_ROWS = 6;

enum MessageCmd { MAccept, MDecline, MReply, MProfile, MDelete, MAcceptFriend, MDeclineFriend };
enum CreateCmd { CMessage, CGameInvite, CFriendRequest };
enum DraftCmd { DSend, DRecipients, DText };
enum KeyAct { KChar, KLeft, KRight, KSymbols, KAccents, KCaps, KBackspace, KSpace, KDone };

const char* KEY_ROWS[3][4] = {{"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_@."},
                              {"!?#$%&*()/", "+-=_[]{}<>", "'\":;,.~^|\\", "\xE2\x82\xAC\xC2\xA3\xC2\xA5\xC2\xA9\xC2\xAE\xC2\xB0\xE2\x80\xA2\xE2\x80\xA6@`"},
                              {"\xC3\xA0\xC3\xA1\xC3\xA2\xC3\xA4\xC3\xA3\xC3\xA5\xC3\xA6\xC3\xA7\xC3\xA8\xC3\xA9", "\xC3\xAA\xC3\xAB\xC3\xAC\xC3\xAD\xC3\xAE\xC3\xAF\xC3\xB1\xC3\xB2\xC3\xB3\xC3\xB4",
                               "\xC3\xB6\xC3\xB5\xC3\xB8\xC5\x93\xC3\xB9\xC3\xBA\xC3\xBB\xC3\xBC\xC3\xBD\xC3\xBF", "\xC3\x9F\xC3\xB0\xC3\xBE\xC2\xA1\xC2\xBF\xC2\xAB\xC2\xBB\xC3\xA7\xC3\xB1."}};

uint32_t upper(uint32_t c) {
    if (c == 0xdf || c == 0xff) return c;  // no single capital in these tables' range
    if (c < 0x10000) {
        wchar_t w = wchar_t(c);
        CharUpperBuffW(&w, 1);
        return w;
    }
    return c;
}

const char* type_name(const MessageItem& m) {
    return m.kind == MessageItem::GameInvite ? "Game Invite" : m.kind == MessageItem::FriendRequest ? "Friend Request" : "Message";
}

// The keyboard's grid (artboard 6): a 116-unit column, ten letter columns, a 116-unit column; five 54-unit rows; 5 apart.
struct KeyGrid {
    float x0, y0, letter;
    ImVec2 at(int col) const {  // a column's left edge and width
        if (col == 0) return ImVec2(x0, 116);
        if (col == 11) return ImVec2(x0 + 116 + 5 + 10 * (letter + 5), 116);
        return ImVec2(x0 + 116 + 5 + (col - 1) * (letter + 5), letter);
    }
};

}  // namespace

// ---- one message

// The 360's options for an open message, by its type. Block Communications comes with a block list on the server.
std::vector<Opt> Guide::message_options(const MessageItem& m) const {
    if (m.kind == MessageItem::GameInvite) return {{"Accept Game Invite", MAccept, true, 0}, {"Decline Game Invite", MDecline, true, 0}};
    if (m.kind == MessageItem::FriendRequest) return {{"Accept Friend Request", MAcceptFriend, true, 0}, {"Decline Friend Request", MDeclineFriend, true, 0}};
    return {{"Reply", MReply, relation(m.from.xuid) == Relation::Friend, 0}, {"View Profile", MProfile, true, 0}, {"Delete", MDelete, true, 0}};
}

void Guide::handle_message(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<Opt> opts = message_options(open_msg_);
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        msg_opt_ = step_focus(opts, msg_opt_, e == PadEvent::Up ? -1 : 1);
        if (msg_opt_ >= 0) opt_bar_.go(float(msg_opt_), now, 0.14f);
        break;
    case PadEvent::B: pop(now); break;
    case PadEvent::A: {
        if (msg_opt_ < 0 || msg_opt_ >= int(opts.size()) || !opts[msg_opt_].enabled) break;
        const MessageItem m = open_msg_;
        GuideRequest r;
        r.xuid = m.from.xuid, r.name = m.from.tag, r.id = m.id;
        switch (opts[msg_opt_].cmd) {
        case MAccept:
            held_ = 1, held_msg_ = m;
            if (!ask_exit_session(now)) held_ = 0, accept_invite(m, out, now);
            break;
        case MAcceptFriend:
            r.action = GuideAction::AcceptFriend;
            out.push_back(r);
            for (size_t i = 0; i < social_.requests.size(); i++)
                if (social_.requests[i].xuid == m.from.xuid) social_.requests.erase(social_.requests.begin() + i--);
            social_.friends.push_back(m.from);
            pop(now);
            break;
        case MDecline:
        case MDeclineFriend:
        case MDelete:
            delete_message(m, out, now);
            pop(now);
            break;
        case MReply:
            flow_base_ = int(stack_.size());
            draft_ = Draft();
            draft_.kind = Draft::Message, draft_.to = {m.from};
            draft_row_ = -1;
            push(Page::Compose, now);
            break;
        case MProfile:
            prof_ = m.from;
            prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
            push(Page::Profile, now);
            break;
        }
        break;
    }
    default: break;
    }
}

// Accept Game Invite: the seat is asked of the server (the answer starts the join); the invite leaves the list.
void Guide::accept_invite(const MessageItem& m, std::vector<GuideRequest>& out, double now) {
    GuideRequest r;
    r.action = GuideAction::AcceptInvite, r.xuid = m.from.xuid, r.name = m.from.tag, r.id = m.id;
    waiting_xuid_ = m.from.xuid;
    out.push_back(r);
    for (size_t i = 0; i < social_.invites.size(); i++)
        if (social_.invites[i].id == m.id) social_.invites.erase(social_.invites.begin() + i--);
    if (page() == Page::Message) pop(now);
}

// Left: when it came and the options (56-unit rows); right: the sender's card and the message text.
void Guide::draw_message(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    const MessageItem& m = open_msg_;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, type_name(m), title_op);
    float x = L + slide;
    panel_fill(dl, v.pt(x, 96), v.pt(x + 431, 607), pop);
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
        if (m.expires_in < 0) lines.push_back(ago(m.age));
    }
    if (m.expires_in >= 0) lines.push_back(expires(m.expires_in));
    for (size_t i = 0; i < lines.size(); i++) {
        ImVec2 p = v.pt(x + 27, 96 + 26 + 13.2f + i * 32.4f);
        text_mid(dl, a.text, a.text_m, v.sz(22), p.x, p.y, alpha(i == 0 ? MUTED : TEXT, pop), lines[i].c_str());
    }
    float my = 96 + 231;
    dl->AddLine(v.pt(x, my - 0.5f), v.pt(x + 431, my - 0.5f), alpha(LINE, pop), v.sz(1));
    option_list(dl, a, v, x, my, 431, 56, 24, 27, message_options(m), msg_opt_, opt_bar_.value(now), pop);

    float rx = x + 431;
    dl->AddRectFilled(v.pt(rx, 96), v.pt(rx + 431, 607), alpha(PANE, pop));
    float cx = rx + 22, cw = 431 - 44, cy0 = 96 + 22;
    panel_fill(dl, v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46 + 128), pop);
    // the sender's card: its title bar in the sender's theme
    const Friend* sender = find(m.from.xuid);
    const ThemeDef& own = theme_preset(sender ? sender->theme : m.from.theme);
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46), alpha(own.accent, pop));
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    ImVec2 np = v.pt(cx + 16, cy0 + 23);
    text_fit(dl, bf, a.bold_m, v.sz(26), np.x, np.y, v.sz(cw - 32), alpha(own.ink, pop), m.from.tag);
    player_pic(dl, a, a.pic(m.from.xuid), m.from.xuid, v.pt(cx, cy0 + 46), v.pt(cx + 128, cy0 + 46 + 128), pop);
    const Friend* live = find(m.from.xuid);
    const Friend& from = live ? *live : m.from;
    std::string zone = from.state == Friend::Playing ? (lobby_words(from).empty() ? "Forza Horizon 2" : lobby_words(from)) : from.state == Friend::Online ? "Online" : "Offline";
    std::string facts[3][2] = {{"Club", from.club.empty() ? "-" : from.club}, {"Level", from.level > 0 ? std::to_string(from.level) : "-"}, {"Zone", zone}};
    for (int i = 0; i < 3; i++) {
        float fy = cy0 + 46 + 64 + (i - 1) * 31.2f;
        ImVec2 l = v.pt(cx + 128 + 16, fy), r = v.pt(cx + cw - 16, fy);
        text_mid(dl, a.text, a.text_m, v.sz(21), l.x, l.y, alpha(TEXT, pop), facts[i][0].c_str());
        text_mid(dl, a.text, a.text_m, v.sz(21), r.x, r.y, alpha(TEXT, pop), facts[i][1].c_str(), true);
    }
    float my2 = cy0 + 46 + 128 + 30;
    ImVec2 mi = v.pt(rx + 32 + 15, my2 + 16);
    icon_mid(dl, a, v.sz(30), mi.x, mi.y, alpha(MUTED, pop), I_MAIL);
    ImVec2 mt = v.pt(rx + 32 + 40, my2 + 16);
    text_mid(dl, a.text, a.text_m, v.sz(24), mt.x, mt.y, alpha(MUTED, pop), "Message Text");
    // the 360's wording: "%1 invites you to play %2. %3" with what the sender added after it
    std::string body = m.kind == MessageItem::GameInvite      ? m.from.tag + " invites you to play Forza Horizon 2."
                       : m.kind == MessageItem::FriendRequest ? m.from.tag + " wants to be your friend."
                                                              : m.text;
    dl->PushClipRect(v.pt(rx, 96), v.pt(rx + 431, 607), true);
    float used = wrapped(dl, a, v, 24, 1.35f, rx + 32, my2 + 40, 431 - 64, alpha(TEXT, pop), body);
    if (m.kind == MessageItem::GameInvite && !m.text.empty()) wrapped(dl, a, v, 22, 1.35f, rx + 32, my2 + 40 + used + 8, 431 - 64, alpha(MUTED, pop), m.text);
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}

// ---- Create New

// The 360's Create New, in its order; the types whose feature is not built yet (private chat, party) are not listed. A game invite needs
// a game others can be seated in.
std::vector<Opt> Guide::create_options() const {
    return {{"Message", CMessage, true, I_MAIL}, {"Game Invite", CGameInvite, my_game_takes_invites(), I_GAMES}, {"Friend Request", CFriendRequest, true, I_PERSON_ADD}};
}

void Guide::start_draft(Draft::Kind kind, double now) {
    draft_ = Draft();
    draft_.kind = kind, draft_row_ = -1;
    if (kind == Draft::FriendRequest) return open_keyboard(Keys::Gamertag, "", now);
    pick_many_ = false, pick_ = ListFocus();
    push(Page::Recipients, now);
}

void Guide::handle_create(PadEvent e, std::vector<GuideRequest>& out, double now) {
    (void)out;
    std::vector<Opt> opts = create_options();
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        new_row_ = step_focus(opts, new_row_, e == PadEvent::Up ? -1 : 1);
        if (new_row_ >= 0) new_bar_.go(float(new_row_), now, 0.14f);
        break;
    case PadEvent::B: end_flow(now); break;
    case PadEvent::A:
        if (new_row_ < 0 || !opts[new_row_].enabled) break;
        start_draft(opts[new_row_].cmd == CMessage ? Draft::Message : opts[new_row_].cmd == CGameInvite ? Draft::GameInvite : Draft::FriendRequest, now);
        break;
    default: break;
    }
}

// Artboard 3: the types on the left (88-unit rows with their icons), what the highlighted one is on the right (the 360's words).
void Guide::draw_create(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Create New", title_op);
    float x = L + slide;
    panel_fill(dl, v.pt(x, 96), v.pt(x + 431, 607), pop);
    std::vector<Opt> opts = create_options();
    option_list(dl, a, v, x, 96, 431, 88, 24, 22, opts, new_row_, new_bar_.value(now), pop, 48, 52, 20);
    dl->AddRectFilled(v.pt(x + 431, 96), v.pt(x + W, 607), alpha(PANE, pop));
    const char* words[3] = {"Send a message to one or more people.", "Invite others to play a game.", "Ask another player to join your friends list."};
    if (new_row_ >= 0 && new_row_ < int(opts.size()))
        wrapped(dl, a, v, 24, 1.4f, x + 431 + 34, 96 + 30, 431 - 68, alpha(TEXT, pop), words[opts[new_row_].cmd]);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}

// ---- recipients

// Who a message or a game invite can go to: friends (the server's rule), in the Friends tab's order.
std::vector<const Friend*> Guide::pick_rows() const {
    std::vector<const Friend*> out;
    if (game_pick_.on) {
        // the game's people picker: the players the game named, in its order, each once the Guide knows who they are
        game_people_.clear();
        for (uint64_t x : game_pick_.people)
            if (const Friend* f = find(x))
                if (!f->tag.empty()) game_people_.push_back(*f);
        for (const Friend& f : game_people_) out.push_back(&f);
        return out;
    }
    for (const FriendRow& r : friend_rows())
        if (r.rel == Relation::Friend) out.push_back(r.f);
    return out;
}

// The game's people picker. One player wanted: A chooses and the list closes. More: A ticks (a full list greys the rest), X finishes.
// B gives the game no players.
void Guide::handle_game_pick(PadEvent e, double now) {
    std::vector<const Friend*> rows = pick_rows();
    int n = int(rows.size());
    GamePick& g = game_pick_;
    auto ticked = [&](uint64_t x) { return std::find(g.chosen.begin(), g.chosen.end(), x) != g.chosen.end(); };
    bool one = g.max <= 1, full = !one && g.chosen.size() >= g.max;
    auto ok = [&](int i) { return !full || ticked(rows[i]->xuid); };
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        int d = e == PadEvent::Up ? -1 : 1;
        for (int i = pick_.row + d; i >= 0 && i < n; i += d)
            if (ok(i)) {
                pick_.move(i - pick_.row, n, PICK_ROWS, now);
                break;
            }
        break;
    }
    case PadEvent::B:
        g.chosen.clear(), g.done = false;
        toggle(now);
        break;
    case PadEvent::A: {
        if (pick_.row >= n || !ok(pick_.row)) break;
        uint64_t x = rows[pick_.row]->xuid;
        if (one) {
            g.chosen = {x}, g.done = true;
            toggle(now);
            break;
        }
        auto it = std::find(g.chosen.begin(), g.chosen.end(), x);
        if (it != g.chosen.end()) g.chosen.erase(it);
        else g.chosen.push_back(x);
        break;
    }
    case PadEvent::X:
        if (one || g.chosen.size() < std::max(1u, g.min)) break;
        g.done = true;
        toggle(now);
        break;
    default: break;
    }
}

void Guide::handle_pick(PadEvent e, std::vector<GuideRequest>& out, double now) {
    (void)out;
    if (game_pick_.on) return handle_game_pick(e, now);
    std::vector<const Friend*> rows = pick_rows();
    int n = int(rows.size());
    // a game invite cannot go to someone already in the game: the row is greyed and the focus passes over it
    auto ok = [&](int i) { return draft_.kind != Draft::GameInvite || !same_game(*rows[i], social_.me); };
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down: {
        int d = e == PadEvent::Up ? -1 : 1;
        for (int i = pick_.row + d; i >= 0 && i < n; i += d)
            if (ok(i)) {
                pick_.move(i - pick_.row, n, PICK_ROWS, now);
                break;
            }
        break;
    }
    case PadEvent::B:
        if (pick_many_) pop(now);   // back to the message with the ticked players
        else end_flow(now);
        break;
    case PadEvent::A: {
        if (pick_.row >= n || !ok(pick_.row)) break;
        const Friend& f = *rows[pick_.row];
        if (!pick_many_) {
            draft_.to = {f};
            stack_.pop_back();  // the message takes this screen's place
            draft_row_ = -1;
            push(Page::Compose, now);
            break;
        }
        bool had = false;
        for (size_t i = 0; i < draft_.to.size(); i++)
            if (draft_.to[i].xuid == f.xuid) draft_.to.erase(draft_.to.begin() + i--), had = true;
        if (!had) {
            if (draft_.to.size() >= 20) show_box(0, "Add Recipients", "You have selected the maximum number of recipients for this message. To add a recipient, you must first clear one of your previously-selected recipients.", {"OK"}, now);
            else draft_.to.push_back(f);
        }
        break;
    }
    default: break;
    }
}

// Artboard 4's list: six 69-unit rows, a 55-unit foot with the place in the list. One player (Select Gamertag), or ticks (Edit Recipients).
void Guide::draw_pick(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    const GamePick& g = game_pick_;
    bool one = g.max <= 1, full = g.on && !one && g.chosen.size() >= g.max;
    auto ticked = [&](uint64_t xuid) { return std::find(g.chosen.begin(), g.chosen.end(), xuid) != g.chosen.end(); };
    // a row that cannot be chosen: a game invite to someone already in the game; the game's list once it is full
    auto grey = [&](const Friend& f) { return g.on ? full && !ticked(f.xuid) : draft_.kind == Draft::GameInvite && same_game(f, social_.me); };
    bool ticks = g.on ? !one : pick_many_;
    draw_header(dl, a, origin, s, zoom, op, L, W, g.on && !g.prompt.empty() ? g.prompt.c_str() : ticks && !g.on ? "Edit Recipients" : "Select Gamertag", title_op);
    float x = L + slide;
    std::vector<const Friend*> rows = pick_rows();
    int n = int(rows.size());
    ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + W, 552);
    panel_fill(dl, p0, p1, pop);
    dl->PushClipRect(p0, p1, true);
    if (!n) {
        const char* why = !social_.session ? "Not signed in" : !social_.loaded && !social_.error.empty() ? "Server unavailable" : g.on ? "" : social_.loaded ? "No friends" : "";
        float ww = a.text->CalcTextSizeA(22, FLT_MAX, 0, why).x;
        ImVec2 c = v.pt(x + (W - ww) / 2, 324);
        text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(MUTED, pop), why);
    } else {
        bool grey_focus = pick_.row < n && grey(*rows[pick_.row]);
        if (!grey_focus) {
            float by = 96 + pick_.bar.value(now) * 69;
            ImVec2 b0 = v.pt(x, by), b1 = v.pt(x + W, by + 69);
            dl->AddRectFilled(b0, b1, alpha(ACCENT, pop));
            dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
        }
        for (int i = pick_.top; i < std::min(n, pick_.top + PICK_ROWS); i++) {
            const Friend& f = *rows[i];
            bool greyed = grey(f);
            int check = -1;
            if (ticks) {
                check = g.on && ticked(f.xuid);
                for (const Friend& t : draft_.to) check |= t.xuid == f.xuid;
            }
            player_row(dl, a, origin, s, zoom, x, 96 + (i - pick_.top) * 69, f, g.on ? relation(f.xuid) : Relation::Friend, i == pick_.row && !greyed, greyed,
                       check, pop);
        }
    }
    dl->PopClipRect();
    ImVec2 f0 = v.pt(x, 552), f1 = v.pt(x + W, 607);
    panel_fill(dl, f0, f1, pop);
    dl->AddLine(ImVec2(f0.x, f0.y + v.sz(0.5f)), ImVec2(f1.x, f0.y + v.sz(0.5f)), alpha(LINE, pop), v.sz(1));
    ImVec2 fr = v.pt(x + W - 30, 579.5f);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(14), fr.y, alpha(pick_.top + PICK_ROWS < n ? MUTED : DISABLED, pop), I_EXPAND_MORE);
    icon_mid(dl, a, v.sz(28), fr.x - v.sz(42), fr.y, alpha(pick_.top > 0 ? MUTED : DISABLED, pop), I_EXPAND_LESS);
    char count[32];
    snprintf(count, sizeof count, "%d of %d", n ? pick_.row + 1 : 0, n);
    text_mid(dl, a.text, a.text_m, v.sz(20), fr.x - v.sz(64), fr.y, alpha(MUTED, pop), count, true);
    if (g.on && !one && g.chosen.size() >= std::max(1u, g.min)) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"X", "Done", RING_X}, {"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 3, op);
    } else {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
    }
}

// ---- compose

// The 360's New Message: the send button first (named by what is sent), then the recipients and the text. Send is greyed until there is
// something to send.
std::vector<Opt> Guide::draft_options() const {
    const Draft& d = draft_;
    std::vector<Opt> o;
    if (d.kind == Draft::FriendRequest) {
        o.push_back({d.sending ? "Sending" : "Send Request", DSend, !d.sending && !d.gamertag.empty(), I_SEND});
        if (!d.xuid) o.push_back({"Edit Recipient", DRecipients, !d.sending, I_PERSON});
        return o;
    }
    // a game invite needs a game others can be seated in (it can stop being one while the invite is written); one the game itself
    // asked for is the server's to refuse
    bool ready = !d.to.empty() && (d.kind == Draft::GameInvite ? d.by_game || my_game_takes_invites() : !d.text.empty());
    o.push_back({d.sending ? "Sending" : d.kind == Draft::GameInvite ? "Send Invite" : "Send Message", DSend, ready && !d.sending, I_SEND});
    o.push_back({"Edit Recipients", DRecipients, !d.sending, I_PERSON});
    o.push_back({d.text.empty() ? "Add Text" : "Edit Text", DText, !d.sending, I_EDIT});
    return o;
}

void Guide::send_draft(std::vector<GuideRequest>& out, double now) {
    (void)now;
    Draft& d = draft_;
    d.sending = true, d.failed = 0, d.pending = 0, d.fail_code.clear();
    if (d.kind == Draft::FriendRequest) {
        GuideRequest r;
        r.action = GuideAction::FriendRequest, r.xuid = d.xuid, r.name = d.xuid ? "" : d.gamertag;
        out.push_back(r), d.pending = 1;
        return;
    }
    for (const Friend& f : d.to) {
        GuideRequest r;
        r.action = d.kind == Draft::GameInvite ? GuideAction::Invite : GuideAction::SendText;
        r.xuid = f.xuid, r.name = f.tag, r.text = d.text;
        out.push_back(r), d.pending++;
    }
}

// The message was sent or dropped: back to where it began (out of the Guide when the game's own Invite Friends opened it).
void Guide::end_flow(double now) {
    int base = flow_base_;
    flow_base_ = -1, draft_ = Draft();
    if (base < 0) return pop(now);
    pop_to(size_t(base), now);
}

void Guide::handle_compose(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<Opt> opts = draft_options();
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        draft_row_ = step_focus(opts, draft_row_, e == PadEvent::Up ? -1 : 1);
        if (draft_row_ >= 0) draft_bar_.go(float(draft_row_), now, 0.14f);
        break;
    case PadEvent::B:
        if (draft_.sending) break;
        if (!draft_.text.empty())
            show_box(BoxDiscard, "Message Not Sent", "Your message has not been sent. Are you sure you want to discard the message?",
                     {"Yes, discard message", "No, edit message"}, now);
        else end_flow(now);
        break;
    case PadEvent::A:
        if (draft_row_ < 0 || draft_row_ >= int(opts.size()) || !opts[draft_row_].enabled) break;
        switch (opts[draft_row_].cmd) {
        case DSend: send_draft(out, now); break;
        case DRecipients:
            if (draft_.kind == Draft::FriendRequest) {
                open_keyboard(Keys::Gamertag, draft_.gamertag, now);
            } else {
                pick_many_ = true, pick_ = ListFocus();
                push(Page::Recipients, now);
            }
            break;
        case DText: open_keyboard(Keys::Text, draft_.text, now); break;
        }
        break;
    default: break;
    }
}

// Artboard 4's compose: the options on the left (76-unit rows with their icons), who it goes to and what it says on the right.
void Guide::draw_compose(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    const Draft& d = draft_;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, d.kind == Draft::GameInvite ? "Game Invite" : d.kind == Draft::FriendRequest ? "Friend Request" : "Message", title_op);
    float x = L + slide;
    panel_fill(dl, v.pt(x, 96), v.pt(x + 431, 607), pop);
    option_list(dl, a, v, x, 96, 431, 76, 24, 24, draft_options(), draft_row_, draft_bar_.value(now), pop, 40, 44, 20);
    float rx = x + 431;
    dl->AddRectFilled(v.pt(rx, 96), v.pt(rx + 431, 607), alpha(PANE, pop));
    dl->PushClipRect(v.pt(rx, 96), v.pt(rx + 431, 607), true);
    ImVec2 tp = v.pt(rx + 32, 96 + 30 + 13);
    text_mid(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, alpha(MUTED, pop), "To:");
    std::string to = d.gamertag;
    if (d.kind != Draft::FriendRequest) {
        to.clear();
        for (size_t i = 0; i < d.to.size(); i++) to += (i ? ", " : "") + d.to[i].tag;
    }
    float y = 96 + 30 + 26 + 6;
    y += wrapped(dl, a, v, 26, 1.2f, rx + 32, y, 431 - 64, alpha(TEXT, pop), to);
    if (d.kind != Draft::FriendRequest) {
        y += 40;
        ImVec2 mi = v.pt(rx + 32 + 14, y + 14), ml = v.pt(rx + 32 + 38, y + 14);
        icon_mid(dl, a, v.sz(28), mi.x, mi.y, alpha(MUTED, pop), I_MAIL);
        text_mid(dl, a.text, a.text_m, v.sz(22), ml.x, ml.y, alpha(MUTED, pop), "Message Text:");
        y += 28 + 6;
        // a game invite reads as the 360's did, with the player's own words after it
        if (d.kind == Draft::GameInvite) y += wrapped(dl, a, v, 24, 1.35f, rx + 32, y, 431 - 64, alpha(TEXT, pop), gamertag + " invites you to play Forza Horizon 2.") + 8;
        if (!d.text.empty()) wrapped(dl, a, v, d.kind == Draft::GameInvite ? 22 : 24, 1.35f, rx + 32, y, 431 - 64, alpha(d.kind == Draft::GameInvite ? MUTED : TEXT, pop), d.text);
    }
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}

// ---- the keyboard

void Guide::open_keyboard(Keys::Purpose purpose, const std::string& text, double now) {
    keys_ = Keys();
    keys_.purpose = purpose, keys_.text = utf32(text), keys_.caret = keys_.text.size();
    keys_.max = purpose == Keys::Gamertag ? 15 : 120;
    if (keys_.text.size() > keys_.max) keys_.max = keys_.text.size();
    push(Page::Keyboard, now);
}

// Done: the text goes where it was asked for. A gamertag for a new friend request leads on to the request itself.
void Guide::keyboard_done(std::vector<GuideRequest>& out, double now) {
    std::string text = utf8(keys_.text);
    while (!text.empty() && text.back() == ' ') text.pop_back();
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    if (keys_.purpose == Keys::Game) {
        // the game's own keyboard request: the text goes back to the game, the Guide closes
        game_text_ = utf8(keys_.text), game_done_ = true;
        toggle(now);
        return;
    }
    stack_.pop_back();
    if (keys_.purpose == Keys::Field) {
        // a profile text: to the server, and shown at once (the server's next answer says what it kept)
        static const char* key[4] = {"motto", "name", "location", "bio"};
        std::string* vals[4] = {&social_.me.motto, &social_.me.name, &social_.me.location, &social_.me.bio};
        int f = std::clamp(keys_.field, 0, 3);
        if (*vals[f] != text) {
            GuideRequest r;
            r.action = GuideAction::SetProfile, r.name = key[f], r.text = text;
            out.push_back(r);
            *vals[f] = text;
        }
        page_dir_ = -1, page_at_ = now;
        entered(page(), now);
        return;
    }
    if (keys_.purpose == Keys::Text) {
        draft_.text = text;
        page_dir_ = -1, page_at_ = now;
        entered(page(), now);
        return;
    }
    draft_.gamertag = text;
    if (page() == Page::Compose) {
        page_dir_ = -1, page_at_ = now;
        entered(page(), now);
    } else {
        draft_row_ = -1;
        push(Page::Compose, now);
    }
}

// The keys of the current page of the keyboard, in the canvas' order (the focus is an index into this list).
std::vector<Guide::Key> Guide::keyboard() const {
    std::vector<Key> keys;
    auto side = [&](const char* label, const char* badge, float bw, float br, int col, int row, int rows, int act, bool lit, float size) {
        Key k;
        k.label = label, k.badge = badge, k.badge_w = bw, k.badge_r = br, k.col = col, k.row = row, k.rows = rows, k.act = act, k.lit = lit, k.side = true;
        k.size = size, k.badge_col = MUTED3;
        keys.push_back(k);
    };
    side("Left", "LB", 34, 6, 0, 0, 2, KLeft, false, 20);
    for (int r = 0; r < 4; r++) {
        if (r == 2) side("Symbols", "LT", 30, 6, 0, 2, 2, KSymbols, keys_.mode == 1, 20);
        std::u32string row = utf32(KEY_ROWS[keys_.mode][r]);
        for (int c = 0; c < int(row.size()) && c < 10; c++) {
            Key k;
            k.ch = keys_.caps && keys_.mode != 1 ? upper(uint32_t(row[c])) : uint32_t(row[c]);
            char b[5];
            utf8(k.ch, b);
            k.label = b, k.col = c + 1, k.row = r;
            keys.push_back(k);
        }
        if (r == 0) side("Right", "RB", 34, 6, 11, 0, 2, KRight, false, 20);
        if (r == 2) side("Accents", "RT", 30, 6, 11, 2, 2, KAccents, keys_.mode == 2, 20);
    }
    side("Caps", "L", 22, 999, 0, 4, 1, KCaps, keys_.caps, 18);
    Key back;
    back.label = "Backspace", back.badge = "X", back.badge_col = RING_X, back.col = 1, back.row = 4, back.cols = 5, back.act = KBackspace, back.wide = true;
    keys.push_back(back);
    Key space;
    space.label = "Space", space.badge = "Y", space.badge_col = RING_Y, space.col = 6, space.row = 4, space.cols = 5, space.act = KSpace, space.wide = true;
    keys.push_back(space);
    Key done;
    done.label = "Done", done.icon = I_PLAY_CIRCLE, done.col = 11, done.row = 4, done.act = KDone, done.side = true, done.size = 18;
    keys.push_back(done);
    return keys;
}

void Guide::press_key(const Key& k, std::vector<GuideRequest>& out, double now) {
    Keys& kb = keys_;
    auto insert = [&](uint32_t c) {
        if (kb.text.size() >= kb.max) return;
        if (kb.purpose == Keys::Gamertag && !(c == ' ' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return;
        kb.text.insert(kb.text.begin() + kb.caret, char32_t(c)), kb.caret++;
    };
    switch (k.act) {
    case KChar: insert(k.ch); break;
    case KLeft: if (kb.caret > 0) kb.caret--; break;
    case KRight: if (kb.caret < kb.text.size()) kb.caret++; break;
    case KSymbols: kb.mode = kb.mode == 1 ? 0 : 1; break;
    case KAccents: kb.mode = kb.mode == 2 ? 0 : 2; break;
    case KCaps: kb.caps = !kb.caps; break;
    case KBackspace: if (kb.caret > 0) kb.text.erase(kb.text.begin() + --kb.caret); break;
    case KSpace: insert(' '); break;
    case KDone: keyboard_done(out, now); break;
    }
}

std::vector<GuideRequest> Guide::type_char(uint32_t cp, double now) {
    std::vector<GuideRequest> out;
    if (!typing()) return out;
    Key k;
    if (cp == '\b') k.act = KBackspace;
    else if (cp == '\r' || cp == '\n') k.act = KDone;
    else if (cp >= 0x20 && cp != 0x7f) k.act = KChar, k.ch = cp;
    else return out;
    press_key(k, out, now);
    return out;
}

void Guide::handle_keyboard(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<Key> keys = keyboard();
    auto act = [&](int a) {
        Key k;
        k.act = a;
        press_key(k, out, now);
    };
    switch (e) {
    case PadEvent::B: pop(now); break;  // the text stays as it was
    case PadEvent::A:
        if (keys_.focus >= 0 && keys_.focus < int(keys.size())) press_key(keys[keys_.focus], out, now);
        break;
    case PadEvent::X: act(KBackspace); break;
    case PadEvent::Y: act(KSpace); break;
    case PadEvent::Lb: act(KLeft); break;
    case PadEvent::Rb: act(KRight); break;
    case PadEvent::Lt: act(KSymbols); break;
    case PadEvent::Rt: act(KAccents); break;
    case PadEvent::L3: act(KCaps); break;
    case PadEvent::Start: act(KDone); break;
    case PadEvent::Up:
    case PadEvent::Down:
    case PadEvent::Left:
    case PadEvent::Right: {
        // to the nearest key that way: among keys that overlap this one across the move, the closest; else the closest of all that way
        if (keys_.focus < 0 || keys_.focus >= int(keys.size())) { keys_.focus = 1; break; }
        const Key& c = keys[keys_.focus];
        bool horiz = e == PadEvent::Left || e == PadEvent::Right;
        int sign = (e == PadEvent::Right || e == PadEvent::Down) ? 1 : -1;
        float cx = c.col + c.cols * 0.5f, cy = c.row + c.rows * 0.5f;
        int best = -1;
        float best_score = 1e9f;
        for (int i = 0; i < int(keys.size()); i++) {
            if (i == keys_.focus) continue;
            const Key& k = keys[i];
            float kx = k.col + k.cols * 0.5f, ky = k.row + k.rows * 0.5f;
            float along = horiz ? (kx - cx) * sign : (ky - cy) * sign;
            if (along <= 0.01f) continue;
            // the gap between the two keys along the move, and how far apart they are across it
            float gap = horiz ? (sign > 0 ? k.col - (c.col + c.cols) : c.col - (k.col + k.cols)) : (sign > 0 ? k.row - (c.row + c.rows) : c.row - (k.row + k.rows));
            if (gap < -0.01f) continue;
            float lo = horiz ? std::max(c.row, k.row) : std::max(c.col, k.col);
            float hi = horiz ? std::min(c.row + c.rows, k.row + k.rows) : std::min(c.col + c.cols, k.col + k.cols);
            float across = hi > lo ? 0 : (horiz ? std::fabs(ky - cy) : std::fabs(kx - cx));
            float score = gap * 10 + across * 40 + (horiz ? std::fabs(ky - cy) : std::fabs(kx - cx));
            if (score < best_score) best_score = score, best = i;
        }
        if (best >= 0) keys_.focus = best;
        break;
    }
    default: break;
    }
}

// Artboard 6: the prompt, the count and the text field (168 units), then the keys.
void Guide::draw_keyboard(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    bool tag = keys_.purpose == Keys::Gamertag;
    static const char* field_title[4] = {"Motto", "Name", "Location", "Bio"};
    static const char* field_prompt[4] = {"Enter your motto.", "Enter your name.", "Enter your location.", "Enter your bio."};
    int field = keys_.purpose == Keys::Field ? std::clamp(keys_.field, 0, 3) : -1;
    bool game = keys_.purpose == Keys::Game;
    draw_header(dl, a, origin, s, zoom, op, L, W,
                game ? (game_title_.empty() ? "Keyboard" : game_title_.c_str()) : field >= 0 ? field_title[field] : tag ? "Enter Gamertag" : "Add Text", title_op);
    float x = L + slide;
    dl->AddRectFilled(v.pt(x, 96), v.pt(x + W, 264), alpha(PANE, pop));
    ImVec2 pp = v.pt(x + 26, 96 + 22 + 14), cp = v.pt(x + W - 26, 96 + 22 + 14);
    text_mid(dl, a.text, a.text_m, v.sz(22), pp.x, pp.y, alpha(MUTED, pop), game ? game_description_.c_str() : field >= 0 ? field_prompt[field] : tag ? "Enter a recipient's gamertag." : "Enter your message.");
    char count[32];
    snprintf(count, sizeof count, "%d / %d", int(keys_.text.size()), int(keys_.max));
    text_mid(dl, a.text, a.text_m, v.sz(22), cp.x, cp.y, alpha(MUTED, pop), count, true);
    // the field: 52 units high at the pane's foot; the caret is a 3x30 accent bar; text longer than the field keeps the caret in view
    float fy = 264 - 22 - 52, fx = x + 26, fw = W - 52;
    ImVec2 f0 = v.pt(fx, fy), f1 = v.pt(fx + fw, fy + 52);
    dl->AddRectFilled(f0, f1, alpha(LINE, pop));
    std::string before = utf8(keys_.text.substr(0, keys_.caret)), after = utf8(keys_.text.substr(keys_.caret));
    float bw = a.text->CalcTextSizeA(22, FLT_MAX, 0, before.c_str()).x, room = fw - 32 - 3;
    float shift = bw > room ? bw - room : 0;
    dl->PushClipRect(v.pt(fx + 16, fy), v.pt(fx + fw - 16, fy + 52), true);
    ImVec2 tp = v.pt(fx + 16 - shift, fy + 26);
    text_mid(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, alpha(TEXT, pop), before.c_str());
    ImVec2 c0 = v.pt(fx + 16 - shift + bw, fy + 11), c1 = v.pt(fx + 16 - shift + bw + 3, fy + 41);
    dl->AddRectFilled(c0, c1, alpha(ACCENT, pop));
    ImVec2 ap = v.pt(fx + 16 - shift + bw + 3, fy + 26);
    text_mid(dl, a.text, a.text_m, v.sz(22), ap.x, ap.y, alpha(TEXT, pop), after.c_str());
    dl->PopClipRect();

    panel_fill(dl, v.pt(x, 264), v.pt(x + W, 607), pop);
    KeyGrid g{x + 26, 264 + 20, (W - 52 - 2 * 116 - 11 * 5) / 10};
    std::vector<Key> keys = keyboard();
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    (void)bf;
    for (int i = 0; i < int(keys.size()); i++) {
        const Key& k = keys[i];
        bool on = i == keys_.focus;
        ImVec2 c0x = g.at(k.col), c1x = g.at(k.col + k.cols - 1);
        float kx = c0x.x, kw = c1x.x + c1x.y - c0x.x, ky = g.y0 + k.row * 59, kh = k.rows * 54 + (k.rows - 1) * 5;
        ImVec2 k0 = v.pt(kx, ky), k1 = v.pt(kx + kw, ky + kh);
        dl->AddRectFilled(k0, k1, alpha(on ? ACCENT : k.lit ? DARK : KEY, pop));
        dl->AddRect(ImVec2(k0.x + v.sz(0.5f), k0.y + v.sz(0.5f)), ImVec2(k1.x - v.sz(0.5f), k1.y - v.sz(0.5f)), alpha(on || k.lit ? ACCENT : KEY_LINE, pop), 0, 0, v.sz(1));
        float cx = kx + kw / 2, cy = ky + kh / 2;
        float lw = a.text->CalcTextSizeA(k.size, FLT_MAX, 0, k.label.c_str()).x;
        if (k.side) {
            // a column: the badge or the icon over the word, 6 apart, centred
            float top_h = k.icon ? 28.0f : 22.0f, total = top_h + 6 + k.size * 1.2f, ty = cy - total / 2;
            if (k.icon) {
                ImVec2 ip = v.pt(cx, ty + 14);
                icon_mid(dl, a, v.sz(28), ip.x, ip.y, alpha(WHITE, pop), k.icon);
            } else {
                ImVec2 bp = v.pt(cx - k.badge_w / 2, ty + 11);
                badge(dl, a, k.badge.c_str(), k.badge_col, bp.x, bp.y, v.sz(1), pop, k.badge_w, k.badge_r);
            }
            ImVec2 lp = v.pt(cx - lw / 2, ty + top_h + 6 + k.size * 0.6f);
            text_mid(dl, a.text, a.text_m, v.sz(k.size), lp.x, lp.y, alpha(WHITE, pop), k.label.c_str());
        } else if (k.wide) {
            // Backspace, Space: the badge then the word, from the left, 10 in and 10 apart
            ImVec2 bp = v.pt(kx + 10, cy);
            float used = badge(dl, a, k.badge.c_str(), k.badge_col, bp.x, bp.y, v.sz(1), pop) / v.sz(1);
            ImVec2 lp = v.pt(kx + 10 + used + 10, cy);
            text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(WHITE, pop), k.label.c_str());
        } else {
            ImVec2 lp = v.pt(cx - lw / 2, cy);
            text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(WHITE, pop), k.label.c_str());
        }
    }
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}
