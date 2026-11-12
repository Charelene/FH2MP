// The Guide's core: the pad reader, the stack of screens, the blades (artboard 1), the header, the 360's message box and toasts
// (artboard 7), and what the server's answers turn into. The other screens are in community.cpp (Community, a player's profile) and
// messages.cpp (a message, Create New, recipients, compose, the keyboard). Motion follows the canvas' CSS: cubic-bezier(.1,.9,.2,1) for
// the open (scale .92 -> 1, 0.26 s), the blade widths (0.3 s), the focus bars (0.14 s), the rows sliding in after a blade switch
// (150 units, 0.32 s, 40 ms + 28 ms per row), a page or tab coming in (110 units, 0.26 s) and the title fading (0.16 s); the toast plays
// its 7.5 s keyframes once.
#include "guide.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui.h"

using namespace ui;

namespace {

enum HomeCmd { HomeNone, HomeResume, HomeFriends, HomeParty, HomeMessages, HomeProfile, HomeAchievements, HomeTheme, HomeVoice };
const char* BLADE[3] = {"Games", nullptr, "Settings"};

// Toasts take the theme's accent; only a warning is red.
ImU32 toast_color(ToastKind k) { return k == ToastKind::Warning ? IM_COL32(0xee, 0x3b, 0x23, 255) : ACCENT; }

// The 360's words when the player's game cannot take others (its "Can't invite party. Start a new game, or set up your current game so
// others can join."): the player is in no lobby, or in the game server's fallback world after it lost the lobby.
const char* NO_GAME = "Can't send game invite. Start a new game, or set up your current game so others can join.";

// The 360's words for a refusal, by the server's error code; fallback when the code has none of its own.
std::string failure_text(const NetResult& r, const char* fallback) {
    if (r.code.empty()) return "Connection lost. Please reconnect to FH2MP.";
    if (r.code == "player_not_found") return "The gamertag you entered does not exist on FH2MP.";
    if (r.code == "already_friends") return "This person is already on your friends list.";
    if (r.code == "self_request") return "You can't add yourself as a friend.";
    if (r.code == "request_limit") return r.message;
    if (r.code == "not_in_lobby" || r.code == "not_in_game") return NO_GAME;
    return fallback;
}

const char* game_words(const std::string& kind) { return kind == "roadtrip" ? "Road Trip" : "Free Roam"; }

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

int PadReader::feed(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, uint64_t now_ms, bool open, PadEvent* out) {
    int n = 0;
    if (buttons & padbits::Guide) {
        if (!guide_down) guide_down = true, guide_since = now_ms;
        if (!guide_fired && now_ms - guide_since >= GUIDE_HOLD_MS) guide_fired = true, out[n++] = PadEvent::Toggle;
    } else {
        if (open && guide_down && !guide_fired) out[n++] = PadEvent::Toggle;  // a short press closes an open Guide
        guide_down = guide_fired = false;
    }
    bool ltd = lt >= 128, rtd = rt >= 128;
    if (open && !was_open) {
        // the Guide just opened: what is held now was pressed for the game (the A that chose the game's Invite Friends went on to pick
        // the first friend, win1 2026-11-11), so it counts only after it was let go
        prev = 0xffff, prev_lt = prev_rt = true, dir_blocked = true;
    }
    was_open = open;
    if (open) {
        uint16_t pressed = buttons & ~prev;
        const struct { uint16_t b; PadEvent e; } map[] = {{padbits::A, PadEvent::A},   {padbits::B, PadEvent::B},   {padbits::X, PadEvent::X},
                                                          {padbits::Y, PadEvent::Y},   {padbits::Lb, PadEvent::Lb}, {padbits::Rb, PadEvent::Rb},
                                                          {padbits::L3, PadEvent::L3}, {padbits::Start, PadEvent::Start}};
        for (auto& m : map)
            if (pressed & m.b) out[n++] = m.e;
        if (ltd && !prev_lt) out[n++] = PadEvent::Lt;
        if (rtd && !prev_rt) out[n++] = PadEvent::Rt;
        int d = -1;
        if (buttons & padbits::Up) d = int(PadEvent::Up);
        else if (buttons & padbits::Down) d = int(PadEvent::Down);
        else if (buttons & padbits::Left) d = int(PadEvent::Left);
        else if (buttons & padbits::Right) d = int(PadEvent::Right);
        else if (std::max(std::abs(int(lx)), std::abs(int(ly))) >= 18000)
            d = std::abs(int(lx)) >= std::abs(int(ly)) ? int(lx > 0 ? PadEvent::Right : PadEvent::Left) : int(ly > 0 ? PadEvent::Up : PadEvent::Down);
        if (dir_blocked) {
            if (d < 0) dir_blocked = false;
            d = -1;
        }
        if (d != dir) {
            dir = d;
            if (d >= 0) out[n++] = PadEvent(d), next_at = now_ms + REPEAT_DELAY_MS;
        } else if (d >= 0 && now_ms >= next_at) {
            out[n++] = PadEvent(d), next_at = now_ms + REPEAT_EVERY_MS;
        }
    } else {
        dir = -1;
    }
    prev = buttons, prev_lt = ltd, prev_rt = rtd;
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

void Guide::ListFocus::clamp(int n, int visible) {
    row = std::clamp(row, 0, std::max(0, n - 1));
    top = std::clamp(top, 0, std::max(0, n - visible));
    if (row < top) top = row;
    if (row >= top + visible) top = row - visible + 1;
    bar.set(float(row - top));
}

void Guide::ListFocus::move(int dy, int n, int visible, double now) {
    if (n <= 0) return;
    row = std::clamp(row + dy, 0, n - 1);
    if (row < top) top = row;
    if (row >= top + visible) top = row - visible + 1;
    bar.go(float(row - top), now, 0.14f);
}

// ---- the stack of screens

void Guide::toggle(double now) {
    open = !open;
    if (open) {
        opened_ = now, stack_ = {Page::Home}, page_at_ = -10, flow_base_ = -1;
        if (box_waiting_.on) box_ = box_waiting_, box_.at = now, box_waiting_.on = false;
        else open_on_toast(now);
    } else {
        closed_ = now, sent_.clear(), box_.on = false, draft_ = Draft(), waiting_xuid_ = 0, held_ = 0;
    }
    if (!widths_set_) {
        for (int i = 0; i < 3; i++) width_[i].set(i == blade_ ? 832.0f : 64.0f);
        bar_[1].set(1);  // canvas: the player's blade opens on Friends
        widths_set_ = true;
    }
    fix_home_focus();
}

// The Guide was opened while a toast about something plays: it opens on that thing (the 360's Guide button on a notification).
void Guide::open_on_toast(double now) {
    if (ach_toast_showing(now) && ach_now_.index >= 0 && ach_.known) {
        // "Hold (the Guide button) to open": the achievements, on the one the toast is about
        ach_focus_ = std::min(ach_now_.index, int(ach_defs().size()) - 1), ach_top_ = std::max(0, ach_focus_ / 11 - 1);
        ach_now_.index = -1;
        stack_ = {Page::Home, Page::Achievements};
        entered(Page::Achievements, now);
        return;
    }
    if (now - toast_at_ > 6.3 || toast_.target == NoTarget) return;
    ToastItem t = toast_;
    toast_.target = NoTarget;
    if (t.target == ToPlayer) {
        const Friend* f = find(t.xuid);
        comm_tab_ = TabFriends, comm_at_ = -10;
        if (!f) {
            stack_ = {Page::Home, Page::Community};
        } else {
            prof_ = *f;
            prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
            stack_ = {Page::Home, Page::Community, Page::Profile};
        }
        entered(page(), now);
        return;
    }
    comm_tab_ = TabMessages, comm_at_ = -10;
    stack_ = {Page::Home, Page::Community};
    for (const MessageItem& m : messages()) {
        bool it = (t.target == ToInvite && m.kind == MessageItem::GameInvite && m.id == t.id) ||
                  (t.target == ToFriendRequest && m.kind == MessageItem::FriendRequest && m.from.xuid == t.xuid) ||
                  (t.target == ToText && m.kind == MessageItem::Text && m.id == t.id);
        if (!it) continue;
        open_msg_ = m, msg_opt_ = -1;
        stack_.push_back(Page::Message);
        if (m.kind == MessageItem::Text && m.unread) {
            GuideRequest r;
            r.action = GuideAction::ReadChat, r.id = m.id, r.name = m.chat;
            wanted_.push_back(r);
            for (TextMessage& x : social_.texts)
                if (x.chat == m.chat && x.id <= m.id) x.unread = false;
        }
        break;
    }
    entered(Page::Community, now);
    entered(page(), now);
}

void Guide::push(Page p, double now) {
    stack_.push_back(p), page_dir_ = 1, page_at_ = now;
    entered(p, now);
}

void Guide::pop(double now) {
    if (stack_.size() <= 1) return toggle(now);
    stack_.pop_back(), page_dir_ = -1, page_at_ = now;
    entered(page(), now);
}

void Guide::pop_to(size_t depth, double now) {
    if (depth == 0) {
        if (open) toggle(now);
        return;
    }
    if (stack_.size() <= depth) return;
    stack_.resize(depth), page_dir_ = -1, page_at_ = now;
    entered(page(), now);
}

// A screen is on top again: its lists fit what the server has now.
void Guide::entered(Page p, double now) {
    (void)now;
    if (p == Page::Community) {
        friends_.clamp(1 + int(friend_rows().size()), 6);
        messages_.clamp(1 + int(messages().size()), 8);
        players_.clamp(int(social_.players.size()), 6);
        party_.clamp(int(party_rows().size()), 6);
    } else if (p == Page::Profile) {
        for (int t = 0; t < 2; t++) {
            std::vector<Opt> o = profile_options(t);
            if (prof_row_[t] < 0 || prof_row_[t] >= int(o.size()) || !o[prof_row_[t]].enabled) prof_row_[t] = first_enabled(o);
        }
        prof_bar_.set(float(std::max(0, prof_row_[prof_tab_])));
    } else if (p == Page::EditProfile) {
        std::vector<Opt> o = edit_options();
        if (edit_row_ < 0 || edit_row_ >= int(o.size()) || !o[edit_row_].enabled) edit_row_ = first_enabled(o);
        edit_bar_.set(float(std::max(0, edit_row_)));
    } else if (p == Page::Message) {
        std::vector<Opt> o = message_options(open_msg_);
        if (msg_opt_ < 0 || msg_opt_ >= int(o.size()) || !o[msg_opt_].enabled) msg_opt_ = first_enabled(o);
        opt_bar_.set(float(std::max(0, msg_opt_)));
    } else if (p == Page::CreateNew) {
        std::vector<Opt> o = create_options();
        if (new_row_ < 0 || new_row_ >= int(o.size()) || !o[new_row_].enabled) new_row_ = first_enabled(o);
        new_bar_.set(float(std::max(0, new_row_)));
    } else if (p == Page::Recipients) {
        std::vector<const Friend*> rows = pick_rows();
        pick_.clamp(int(rows.size()), 6);
        // a game invite: the focus starts on the first friend who is not already in the game
        if (draft_.kind == Draft::GameInvite && pick_.row < int(rows.size()) && same_game(*rows[pick_.row], social_.me))
            for (int i = 0; i < int(rows.size()); i++)
                if (!same_game(*rows[i], social_.me)) {
                    pick_.row = i, pick_.clamp(int(rows.size()), 6);
                    break;
                }
    } else if (p == Page::Compose) {
        std::vector<Opt> o = draft_options();
        if (draft_row_ < 0 || draft_row_ >= int(o.size()) || !o[draft_row_].enabled) draft_row_ = first_enabled(o);
        draft_bar_.set(float(std::max(0, draft_row_)));
    }
}

void Guide::open_community(int tab, double now) {
    if (!open) toggle(now);
    stack_ = {Page::Home, Page::Community}, comm_tab_ = tab, comm_at_ = -10, page_at_ = -10;
    entered(Page::Community, now);
}

void Guide::open_profile_for_game(uint64_t xuid, double now) {
    if (!open) toggle(now);
    const Friend* f = find(xuid);
    prof_ = f ? *f : Friend();
    prof_.xuid = xuid;
    prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
    stack_ = {Page::Profile}, page_at_ = -10, box_.on = false, flow_base_ = -1;
    entered(Page::Profile, now);
    if (!f) {
        // not someone the friend list has: the server is asked for their card
        GuideRequest r;
        r.action = GuideAction::Lookup, r.xuid = xuid;
        wanted_.push_back(r);
    }
}

void Guide::open_keyboard_for_game(const std::string& text, const std::string& title, const std::string& description, double now) {
    if (!open) toggle(now);
    stack_.clear(), box_.on = false, flow_base_ = -1;
    game_title_ = title, game_description_ = description, game_text_.clear(), game_done_ = false;
    open_keyboard(Keys::Game, text, now);
    page_at_ = -10;
}

bool Guide::take_game_text(std::string* text) {
    bool done = game_done_;
    if (done && text) *text = game_text_;
    game_done_ = false, game_text_.clear();
    return done;
}

void Guide::open_people_for_game(const std::string& prompt, const std::vector<uint64_t>& people, const std::vector<uint64_t>& chosen, unsigned min,
                                 unsigned max, double now) {
    if (!open) toggle(now);
    game_pick_ = GamePick();
    game_pick_.on = true, game_pick_.prompt = prompt, game_pick_.people = people, game_pick_.min = min, game_pick_.max = std::max(1u, max);
    for (uint64_t x : chosen)
        if (std::find(people.begin(), people.end(), x) != people.end() && game_pick_.chosen.size() < game_pick_.max) game_pick_.chosen.push_back(x);
    draft_ = Draft(), pick_many_ = false, pick_ = ListFocus();
    stack_ = {Page::Recipients}, page_at_ = -10, box_.on = false, flow_base_ = -1;
    entered(Page::Recipients, now);
    if (game_pick_.max > 1 && game_pick_.chosen.size() >= game_pick_.max) {
        // a list that starts full: the focus goes to a ticked player (the others are greyed)
        std::vector<const Friend*> rows = pick_rows();
        for (int i = 0; i < int(rows.size()); i++)
            if (std::find(game_pick_.chosen.begin(), game_pick_.chosen.end(), rows[i]->xuid) != game_pick_.chosen.end()) {
                pick_.row = i, pick_.clamp(int(rows.size()), 6);
                break;
            }
    }
    // players the friend list does not have: the server is asked who they are (a row shows once it answered)
    int asked = 0;
    for (uint64_t x : people)
        if (!find(x) && asked++ < 20) {
            GuideRequest r;
            r.action = GuideAction::Lookup, r.xuid = x;
            wanted_.push_back(r);
        }
}

bool Guide::take_game_people(std::vector<uint64_t>* chosen) {
    bool done = game_pick_.on && game_pick_.done;
    if (done && chosen) *chosen = game_pick_.chosen;
    game_pick_ = GamePick();
    return done;
}

void Guide::open_invite(double now) {
    bool was = open;
    if (!open) toggle(now);
    flow_base_ = was ? int(stack_.size()) : 0;
    draft_ = Draft();
    draft_.kind = Draft::GameInvite, draft_.by_game = true;
    pick_many_ = false, pick_ = ListFocus();
    if (was) {
        push(Page::Recipients, now);
    } else {
        stack_ = {Page::Home, Page::Recipients}, page_at_ = -10;
        entered(Page::Recipients, now);
    }
}

void Guide::set_social(const Social& s) {
    social_ = s;
    fix_home_focus();
    if (!open) return;
    Page p = page();
    if (p == Page::Community || p == Page::Recipients || p == Page::Profile || p == Page::Compose) {
        friends_.clamp(1 + int(friend_rows().size()), 6);
        messages_.clamp(1 + int(messages().size()), 8);
        players_.clamp(int(social_.players.size()), 6);
        party_.clamp(int(party_rows().size()), 6);
        pick_.clamp(int(pick_rows().size()), 6);
        // an option list can lose or grey the focused entry when the server's state changes
        if (p == Page::Profile) {
            std::vector<Opt> o = profile_options(prof_tab_);
            int& row = prof_row_[prof_tab_];
            if (row < 0 || row >= int(o.size()) || !o[row].enabled) row = first_enabled(o), prof_bar_.set(float(std::max(0, row)));
        }
        if (p == Page::Compose) {
            std::vector<Opt> o = draft_options();
            if (draft_row_ < 0 || draft_row_ >= int(o.size()) || !o[draft_row_].enabled) draft_row_ = first_enabled(o), draft_bar_.set(float(std::max(0, draft_row_)));
        }
    }
}

// ---- who is who

const Friend* Guide::find(uint64_t xuid) const {
    if (xuid && xuid == social_.me.xuid) return &social_.me;
    for (const Friend& f : social_.friends)
        if (f.xuid == xuid) return &f;
    for (const Friend& f : social_.requests)
        if (f.xuid == xuid) return &f;
    for (const Friend& f : social_.outgoing)
        if (f.xuid == xuid) return &f;
    for (const Friend& f : social_.players)
        if (f.xuid == xuid) return &f;
    for (const Friend& f : social_.others)
        if (f.xuid == xuid) return &f;
    return nullptr;
}

Relation Guide::relation(uint64_t xuid) const {
    for (const Friend& f : social_.friends)
        if (f.xuid == xuid) return Relation::Friend;
    for (const Friend& f : social_.requests)
        if (f.xuid == xuid) return Relation::RequestReceived;
    for (const Friend& f : social_.outgoing)
        if (f.xuid == xuid) return Relation::RequestSent;
    return Relation::Stranger;
}

// This player is in a lobby the server can seat others in (not the game server's fallback world, where nobody can be seated).
bool Guide::my_game_takes_invites() const { return !social_.me.lobby_id.empty() && social_.me.lobby_id != "default"; }

// The 360's rule for Invite to Game: this player is in a game session the other one is not in. FH2MP sends invites to friends only.
bool Guide::can_invite(const Friend& f) const {
    return relation(f.xuid) == Relation::Friend && my_game_takes_invites() && !same_game(f, social_.me);
}

// Join Session In Progress: their game is one others can be seated in (a private Free Roam is not listed by the server at all), it is
// not this player's, and it has room.
bool Guide::can_join(const Friend& f) const {
    if (f.lobby_id.empty() || f.lobby_id == "default" || same_game(f, social_.me)) return false;
    return f.lobby_capacity <= 0 || f.lobby_players < f.lobby_capacity;
}

// What is waiting from this player, as the 360 showed it in place of their status.
std::string Guide::waiting_words(const Friend& f, Relation rel) const {
    if (rel == Relation::RequestReceived) return "Wants to be your friend";
    if (rel == Relation::RequestSent) return "Friend request pending";
    for (const ReceivedInvite& iv : social_.invites)
        if (iv.from.xuid == f.xuid) return "Wants to play with you";
    for (const TextMessage& m : social_.texts)
        if (m.from == f.xuid && m.unread) return "Sent you a message";
    return "";
}

// ---- the message box

void Guide::show_box(int tag, const char* title, const std::string& body, std::vector<std::string> buttons, double now) {
    Box b;
    b.on = true, b.title = title, b.body = body, b.buttons = std::move(buttons), b.tag = tag, b.at = now;
    b.bar.set(0);
    if (open) box_ = b;
    else box_waiting_ = b;
}

// The 360's question before a join: shown when the player is in a game (the join takes them out of it). True when it was asked.
bool Guide::ask_exit_session(double now) {
    if (social_.me.lobby_id.empty()) return false;
    show_box(BoxExitSession, "Exit Session",
             "Joining this session will exit your current one. If you are playing a game, you will lose any unsaved progress.\n\nAre you sure you want to exit?",
             {"Yes, exit", "No, don't exit"}, now);
    return true;
}

std::vector<GuideRequest> Guide::take_requests() {
    std::vector<GuideRequest> out;
    out.swap(wanted_);
    return out;
}

void Guide::box_done(int tag, int button, std::vector<GuideRequest>& out, double now) {
    box_.on = false;
    if (tag == BoxExitSession) {
        int held = held_;
        held_ = 0;
        if (button == 0 && held == 1) accept_invite(held_msg_, out, now);
        if (button == 0 && held == 2) join_player(held_player_, out, now);
        return;
    }
    if (tag == BoxDiscard && button == 0) end_flow(now);  // "Yes, discard message"
    if (tag == BoxThenClose && open) toggle(now);
}

// ---- events

std::vector<GuideRequest> Guide::handle(PadEvent e, double now) {
    std::vector<GuideRequest> out;
    if (e == PadEvent::Toggle) {
        toggle(now);
        return out;
    }
    if (box_.on) {
        int n = int(box_.buttons.size());
        if (e == PadEvent::Up || e == PadEvent::Down) {
            box_.focus = std::clamp(box_.focus + (e == PadEvent::Up ? -1 : 1), 0, n - 1);
            box_.bar.go(float(box_.focus), now, 0.14f);
        } else if (e == PadEvent::A) {
            box_done(box_.tag, box_.focus, out, now);
        } else if (e == PadEvent::B) {
            box_done(box_.tag, n - 1, out, now);  // B is the last button: OK, or the "No" of a question
        }
        return out;
    }
    switch (page()) {
    case Page::Home: handle_home(e, out, now); break;
    case Page::Community: handle_community(e, out, now); break;
    case Page::Profile: handle_profile(e, out, now); break;
    case Page::EditProfile: handle_edit(e, out, now); break;
    case Page::Message: handle_message(e, out, now); break;
    case Page::CreateNew: handle_create(e, out, now); break;
    case Page::Recipients: handle_pick(e, out, now); break;
    case Page::Compose: handle_compose(e, out, now); break;
    case Page::Keyboard: handle_keyboard(e, out, now); break;
    case Page::Achievements: handle_achievements(e, now); break;
    case Page::Compare: handle_compare(e, now); break;
    case Page::AchDetail: handle_ach_detail(e, now); break;
    case Page::Theme: handle_theme(e, out, now); break;
    case Page::Voice: handle_voice(e, out, now); break;
    }
    return out;
}

// The blades' rows as the canvas has them (artboard 1). A row with no screen behind it yet is greyed.
std::vector<Opt> Guide::home_rows(int blade) const {
    if (blade == 0)
        return {{"Forza Horizon 2", HomeResume, true, I_GAMES}, {"Achievements", HomeAchievements, ach_.known, I_TROPHY}, {"Game Library", HomeNone, false, I_APPS}};
    if (blade == 1)
        return {{"Leave Game", HomeNone, false, 0},          {"Friends", HomeFriends, true, I_PERSON},  {"Party", HomeParty, true, I_GROUPS},
                {"Messages", HomeMessages, true, I_MAIL},    {"Beacons & Activity", HomeNone, false, I_SENSORS},
                {"Chat", HomeNone, false, I_CHAT},           {"Lobbies", HomeNone, false, I_FLAG}};
    return {{"Profile", HomeProfile, social_.loaded, I_PERSON}, {"Theme", HomeTheme, true, I_PALETTE},  {"Voice", HomeVoice, true, I_MIC},
            {"Notifications", HomeNone, false, I_BELL},   {"Network", HomeNone, false, I_WIFI},   {"Controller", HomeNone, false, I_GAMES},
            {"System", HomeNone, false, I_SETTINGS}};
}

void Guide::handle_home(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<Opt> rows = home_rows(blade_);
    int& f = focus_[blade_];
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        f = step_focus(rows, f, e == PadEvent::Up ? -1 : 1);
        if (f >= 0) bar_[blade_].go(float(f), now, 0.14f);
        break;
    case PadEvent::Left:
    case PadEvent::Lb: switch_to(blade_ - 1, now); break;
    case PadEvent::Right:
    case PadEvent::Rb: switch_to(blade_ + 1, now); break;
    case PadEvent::B: toggle(now); break;
    case PadEvent::A: {
        if (f < 0 || f >= int(rows.size()) || !rows[f].enabled) break;
        int cmd = rows[f].cmd;
        if (cmd == HomeResume) {
            toggle(now);
            GuideRequest r;
            r.action = GuideAction::Resume;
            out.push_back(r);
        } else if (cmd == HomeAchievements) {
            ach_focus_ = ach_top_ = 0;
            push(Page::Achievements, now);
        } else if (cmd == HomeTheme) {
            open_theme(now);
        } else if (cmd == HomeVoice) {
            open_voice(now);
        } else if (cmd == HomeProfile) {
            prof_ = social_.me;
            if (prof_.tag.empty()) prof_.tag = gamertag;
            prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
            push(Page::Profile, now);
        } else {
            comm_tab_ = cmd == HomeParty ? TabParty : cmd == HomeMessages ? TabMessages : TabFriends, comm_at_ = -10;
            push(Page::Community, now);
        }
        break;
    }
    default: break;
    }
}

// Each blade's focus rests on a row that can be chosen (a row becomes usable when the server answers), or on none.
void Guide::fix_home_focus() {
    for (int b = 0; b < 3; b++) {
        std::vector<Opt> rows = home_rows(b);
        int& f = focus_[b];
        if (f >= 0 && f < int(rows.size()) && rows[f].enabled) continue;
        f = first_enabled(rows);
        if (f >= 0) bar_[b].set(float(f));
    }
}

void Guide::switch_to(int b, double now) {
    if (b == blade_ || b < 0 || b > 2) return;
    switch_dir_ = b > blade_ ? 1 : -1;
    blade_ = b, switched_ = now;
    for (int i = 0; i < 3; i++) width_[i].go(i == b ? 832.0f : 64.0f, now, 0.3f);
}

// ---- what the server's answers turn into: the 360's toasts (things that happened) and message boxes (things that could not)

void Guide::on_result(const NetResult& r, const GuideAssets& a, double now) {
    ImTextureID them = a.pic(r.xuid);
    bool mine = draft_.sending && ((r.op == NetOp::Invite && draft_.kind == Draft::GameInvite) || (r.op == NetOp::SendText && draft_.kind == Draft::Message) ||
                                   (r.op == NetOp::FriendRequest && draft_.kind == Draft::FriendRequest));
    if (mine) {
        // one answer per recipient of the message being sent
        if (r.ok && r.op == NetOp::Invite) sent_.insert(r.xuid);
        if (!r.ok) draft_.failed++, draft_.fail_code = r.code.empty() ? "-" : r.code;
        if (--draft_.pending > 0) return;
        draft_.sending = false;
        if (draft_.failed) {
            NetResult worst = r;
            worst.code = draft_.fail_code == "-" ? "" : draft_.fail_code;
            const char* fallback = draft_.kind == Draft::FriendRequest ? "Can't complete this process. Please try again." : "Your message could not be sent.";
            show_box(BoxInfo, draft_.kind == Draft::GameInvite ? "Game Invite" : draft_.kind == Draft::FriendRequest ? "Friend Request" : "Message",
                     failure_text(worst, fallback), {"OK"}, now);
            draft_.failed = 0;
            return;
        }
        if (draft_.kind == Draft::GameInvite) {
            bool one = draft_.to.size() == 1;
            toast(ToastKind::Invite, "Game invite sent", one ? ("To " + draft_.to[0].tag).c_str() : "", I_GAMES, one ? a.pic(draft_.to[0].xuid) : 0, now);
        }
        end_flow(now);
        return;
    }
    switch (r.op) {
    case NetOp::Invite:
        if (waiting_xuid_ == r.xuid) waiting_xuid_ = 0;
        if (r.ok) {
            sent_.insert(r.xuid);
            toast(ToastKind::Invite, "Game invite sent", ("To " + r.name).c_str(), I_GAMES, them, now);
        } else {
            show_box(BoxInfo, "Game Invite", failure_text(r, "Your message could not be sent."), {"OK"}, now);
        }
        break;
    case NetOp::Accept:
    case NetOp::Join:
        if (waiting_xuid_ == r.xuid) waiting_xuid_ = 0;
        if (r.ok && game_joins) {
            // the seat is held: the game is handed the session and joins it by its own flow (its own prompt, its own errors); the
            // Guide gets out of the way, as the 360's did after an accepted invite
            GuideRequest j;
            j.action = GuideAction::JoinGame, j.xuid = r.xuid, j.name = r.name, j.text = r.session, j.kind = r.tmpl, j.scid = r.scid;
            wanted_.push_back(j);
            if (open) toggle(now);
        } else if (r.ok) {
            // a game this Guide cannot hand a session to: the place is held and the player goes online by hand
            show_box(BoxInfo, r.op == NetOp::Accept ? "Game Invite" : "Join Session In Progress",
                     "A place in " + r.name + "'s game is held for you. Go online in " + game_words(r.kind) + " to take it.", {"OK"}, now);
        } else if (r.op == NetOp::Accept) {
            bool gone = r.code == "invite_gone" || r.code == "lobby_gone";
            show_box(BoxInfo, "Game Invite", gone ? "Couldn't accept game invite. The invitation is no longer available." : failure_text(r, "Couldn't accept game invite."),
                     {"OK"}, now);
        } else {
            show_box(BoxInfo, "Join Session In Progress", failure_text(r, "Couldn't join the session in progress."), {"OK"}, now);
        }
        break;
    case NetOp::Decline:
        if (!r.ok) show_box(BoxInfo, "Game Invite", failure_text(r, "Couldn't decline game invite."), {"OK"}, now);
        break;
    case NetOp::FriendRequest:
        if (!r.ok) show_box(BoxInfo, "Friend Request", failure_text(r, "Can't complete this process. Please try again."), {"OK"}, now);
        break;
    case NetOp::AcceptFriend:
        if (!r.ok) show_box(BoxInfo, "Friend Request", failure_text(r, "Couldn't accept friend request."), {"OK"}, now);
        break;
    case NetOp::DeclineFriend:
        if (!r.ok) show_box(BoxInfo, "Friend Request", failure_text(r, "Couldn't delete friend request."), {"OK"}, now);
        break;
    case NetOp::RemoveFriend:
        if (!r.ok) show_box(BoxInfo, "Friends", failure_text(r, "Can't complete this process. Please try again."), {"OK"}, now);
        break;
    case NetOp::SendText:
        if (!r.ok) show_box(BoxInfo, "Message", failure_text(r, "Your message could not be sent."), {"OK"}, now);
        break;
    case NetOp::SetProfile:
        // the server words its refusal as the 360 did ("Your motto contains a word or phrase that isn't allowed. Please try again.")
        if (!r.ok) show_box(BoxInfo, "Edit Profile", r.code == "bad_profile_text" ? r.message : failure_text(r, "Can't complete this process. Please try again."), {"OK"}, now);
        break;
    case NetOp::ReadChat: break;
    case NetOp::Achievements:
        // Compare Games without the other player's achievements: said in a box, back on their profile
        if (!r.ok && open && cmp_.xuid == r.xuid && (page() == Page::Compare || page() == Page::AchDetail)) {
            while (page() != Page::Compare) stack_.pop_back();
            pop(now);
            show_box(BoxInfo, "Compare Games", "Can't retrieve information from FH2MP. Please try again later.", {"OK"}, now);
        }
        break;
    case NetOp::Lookup:
        // the game's gamercard for a player the server could not give: said in a box, and the game gets its screen back
        if (!r.ok && open && stack_.size() == 1 && page() == Page::Profile && prof_.xuid == r.xuid && !find(r.xuid))
            show_box(BoxThenClose, "Profile", "Can't retrieve information from FH2MP. Please try again later.", {"OK"}, now);
        break;
    case NetOp::Received: toast_to(ToInvite, r.xuid, r.id, ToastKind::Invite, r.name.c_str(), "wants to play Forza Horizon 2", I_GAMES, them, now); break;
    case NetOp::GotRequest: toast_to(ToFriendRequest, r.xuid, 0, ToastKind::Info, r.name.c_str(), "wants to be your friend", I_PERSON_ADD, 0, now); break;
    case NetOp::GotText: toast_to(ToText, r.xuid, r.id, ToastKind::Info, r.name.c_str(), "sent you a message", I_MAIL, 0, now); break;
    case NetOp::Online: toast_to(ToPlayer, r.xuid, 0, ToastKind::Info, r.name.c_str(), "is online", I_PERSON, 0, now); break;
    }
}

// A toast that arrives while another plays waits for it (four at most; the oldest waiting one gives way).
void Guide::toast(ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic, double now) {
    toast_to(NoTarget, 0, 0, kind, title, detail, icon, pic, now);
}

void Guide::toast_to(ToastTarget target, uint64_t xuid, int64_t id, ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic,
                     double now) {
    ToastItem t;
    t.kind = kind, t.title = title, t.detail = detail, t.icon = icon, t.pic = pic, t.target = target, t.xuid = xuid, t.id = id;
    for (auto& c : t.title) c = char(toupper((unsigned char)c));
    for (auto& c : t.detail) c = char(toupper((unsigned char)c));
    if (now - toast_at_ < 6.3) {
        if (toasts_.size() >= 4) toasts_.pop_front();
        toasts_.push_back(t);
        return;
    }
    toast_ = t, toast_at_ = now;
    toast_sound();
}

// Every toast plays the achievement toast's regular sound as it starts (user 2026-11-12), unless an achievement's own is waiting.
void Guide::toast_sound() {
    if (!ach_sound_) ach_sound_ = int(AchTier::Normal) + 1;
}

bool Guide::busy(double now) const { return open || now - toast_at_ < 6.4 || !toasts_.empty() || ach_toast_showing(now) || !ach_toasts_.empty(); }

// ---- drawing

void Guide::page_motion(double now, float op, float* slide, float* pop, float* title_op) const {
    float pe = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.26), 0.0f, 1.0f)) : 1;
    *slide = (1 - pe) * 110 * page_dir_, *pop = op * pe;
    *title_op = page_at_ > 0 ? ease_guide(std::clamp(float((now - page_at_) / 0.16), 0.0f, 1.0f)) : 1;
}

bool Guide::draw(ImDrawList* dl, const GuideAssets& a, float w, float h, double now) {
    float s = std::min(w / 1280.0f, h / 720.0f);
    ImVec2 origin(w * 0.5f - 640 * s, h * 0.5f - 360 * s);
    if (open) {
        float k = ease_guide(std::clamp(float((now - opened_) / 0.26), 0.0f, 1.0f));
        float zoom = 0.92f + 0.08f * k, op = k;
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, int(153 * op)));
        switch (page()) {
        case Page::Home: draw_home(dl, a, origin, s, zoom, op, now); break;
        case Page::Community: draw_community(dl, a, origin, s, zoom, op, now); break;
        case Page::Profile: draw_profile(dl, a, origin, s, zoom, op, now); break;
        case Page::EditProfile: draw_edit(dl, a, origin, s, zoom, op, now); break;
        case Page::Message: draw_message(dl, a, origin, s, zoom, op, now); break;
        case Page::CreateNew: draw_create(dl, a, origin, s, zoom, op, now); break;
        case Page::Recipients: draw_pick(dl, a, origin, s, zoom, op, now); break;
        case Page::Compose: draw_compose(dl, a, origin, s, zoom, op, now); break;
        case Page::Keyboard: draw_keyboard(dl, a, origin, s, zoom, op, now); break;
        case Page::Achievements: draw_achievements(dl, a, origin, s, zoom, op, now); break;
        case Page::Compare: draw_compare(dl, a, origin, s, zoom, op, now); break;
        case Page::AchDetail: draw_ach_detail(dl, a, origin, s, zoom, op, now); break;
        case Page::Theme: draw_theme(dl, a, origin, s, zoom, op, now); break;
        case Page::Voice: draw_voice(dl, a, origin, s, zoom, op, now); break;
        }
        if (box_.on) draw_box(dl, a, origin, s, w, h, now);
    }
    draw_toast(dl, a, s, origin, now);
    draw_ach_toast(dl, a, s, origin, now);
    return busy(now);
}

// The header above a panel (canvas: the title at (27, 20), 22 units, line-height 1, so centred on y 72; the player's picture 48x48 in the
// middle at the top; the clock 30 in from the right).
void Guide::draw_header(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, float left, float width, const char* title,
                        float title_op) {
    View v{origin, s, zoom};
    ImVec2 t = v.pt(left + 27, 72);
    text_fit(dl, a.text, a.text_m, v.sz(22), t.x, t.y, v.sz(width / 2 - 24 - 27 - 14), alpha(WHITE, op * title_op), title);  // up to the picture
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
    float pslide, pop, title_op;
    page_motion(now, op, &pslide, &pop, &title_op);  // coming back from a page: the blades slide in like a page does
    const char* title = blade_ == 1 ? gamertag.c_str() : BLADE[blade_];
    draw_header(dl, a, origin, s, zoom, op, 160, 960, title, title_op);

    int online = 0, waiting = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    for (const MessageItem& m : messages()) waiting += m.unread;
    char friends_meta[16] = "", messages_meta[16] = "";
    if (social_.loaded) snprintf(friends_meta, sizeof friends_meta, "%d", online);
    if (waiting) snprintf(messages_meta, sizeof messages_meta, "%d", waiting);
    char ach_meta[32] = "";
    if (ach_.known) snprintf(ach_meta, sizeof ach_meta, "%d of %d", ach_.count(), int(ach_defs().size()));

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
            std::vector<Opt> rows = home_rows(bi);
            ImVec2 b0 = v.pt(x + lw, top), b1 = v.pt(x + bw, top + bh);
            dl->AddRectFilled(b0, b1, alpha(PANEL, pop));
            dl->PushClipRect(b0, b1, true);
            bool barred = on && focus_[bi] >= 0;
            if (barred) {
                float by = top + bar_[bi].value(now) * 66;
                ImVec2 r0 = v.pt(x + lw, by), r1 = v.pt(x + lw + 736, by + 66);
                dl->AddRectFilled(r0, r1, alpha(ACCENT, pop));
                dl->AddLine(ImVec2(r0.x, r1.y - v.sz(0.5f)), ImVec2(r1.x, r1.y - v.sz(0.5f)), alpha(DARK, pop), v.sz(1));
            }
            for (int i = 0; i < int(rows.size()); i++) {
                const Opt& r = rows[i];
                float slide = 0, rop = 1;
                if (on && since < 0.7f) {
                    float d = std::clamp((since - 0.04f - 0.028f * i) / 0.32f, 0.0f, 1.0f);
                    float e = ease_guide(d);
                    slide = (1 - e) * 150 * switch_dir_, rop = e;
                }
                float y = top + i * 66;
                bool hi = barred && i == focus_[bi];
                ImVec2 r0 = v.pt(x + lw + slide, y), r1 = v.pt(x + lw + 736 + slide, y + 66);
                if (!hi) dl->AddLine(ImVec2(r0.x, r1.y - v.sz(0.5f)), ImVec2(r1.x, r1.y - v.sz(0.5f)), alpha(LINE, pop * rop), v.sz(1));
                float cy = (r0.y + r1.y) * 0.5f;
                ImU32 fg = alpha(r.enabled ? WHITE : DISABLED, pop * rop);
                text_mid(dl, tf, a.text_m, v.sz(26), r0.x + v.sz(30), cy, fg, r.label.c_str());
                float right = r1.x - v.sz(22);
                if (r.icon) icon_mid(dl, a, v.sz(32), right - v.sz(16), cy, fg, r.icon);
                right -= v.sz(32 + 16);
                const char* meta = r.cmd == HomeFriends ? friends_meta : r.cmd == HomeMessages ? messages_meta : r.cmd == HomeResume ? "Now playing"
                                   : r.cmd == HomeAchievements ? ach_meta : "";
                if (*meta) text_mid(dl, tf, a.text_m, v.sz(20), right, cy, alpha(hi ? WHITE : MUTED, pop * rop), meta, true);
            }
            dl->PopClipRect();
        }
        x += bw;
    }
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, 187, 606, hints, 4, op);
}

// The 360's message box over the page: a heading, the text, the buttons one under another (56-unit rows as the canvas' option lists).
void Guide::draw_box(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float w, float h, double now) {
    float k = ease_guide(std::clamp(float((now - box_.at) / 0.2), 0.0f, 1.0f));
    View v{origin, s, 0.96f + 0.04f * k};
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, int(140 * k)));
    const float W = 560, X = 640 - W / 2, PAD = 27;
    float text_h = 0;
    {
        const char* p = box_.body.c_str();
        const char* end = p + box_.body.size();
        while (p < end) {
            const char* brk = a.text->CalcWordWrapPosition(22, p, end, W - 2 * PAD);
            if (brk == p) brk = p + 1;
            for (const char* q = p; q < brk; q++)
                if (*q == '\n') { brk = q + 1; break; }
            text_h += 22 * 1.35f, p = brk;
            while (p < end && *p == ' ') p++;
        }
    }
    float head = 62, body_h = text_h + 2 * 22, rows = 56.0f * box_.buttons.size();
    float H = head + body_h + rows, Y = 360 - H / 2;
    dl->AddRectFilled(v.pt(X, Y), v.pt(X + W, Y + head), alpha(RAISED, k));
    ImVec2 t = v.pt(X + PAD, Y + head / 2);
    text_mid(dl, a.text, a.text_m, v.sz(24), t.x, t.y, alpha(WHITE, k), box_.title.c_str());
    dl->AddRectFilled(v.pt(X, Y + head), v.pt(X + W, Y + head + body_h), alpha(PANE, k));
    wrapped(dl, a, v, 22, 1.35f, X + PAD, Y + head + 22, W - 2 * PAD, alpha(WHITE, k), box_.body);
    float by = Y + head + body_h;
    dl->AddRectFilled(v.pt(X, by), v.pt(X + W, by + rows), alpha(PANEL, k));
    std::vector<Opt> opts;
    for (const std::string& b : box_.buttons) opts.push_back({b, 0, true, 0});
    option_list(dl, a, v, X, by, W, 56, 24, PAD, opts, box_.focus, box_.bar.value(now), k);
}

void Guide::draw_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now) {
    float t = float(now - toast_at_);
    if ((t < 0 || t > 6.3f) && !toasts_.empty()) {
        toast_ = toasts_.front(), toasts_.pop_front(), toast_at_ = now, t = 0;
        toast_sound();
    }
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
    float tw = std::max(bf->CalcTextSizeA(17, FLT_MAX, 0, toast_.title.c_str()).x, a.text->CalcTextSizeA(17, FLT_MAX, 0, toast_.detail.c_str()).x);
    float pw = std::ceil(tw) + 22 + 12, half = pw * 0.5f;
    float unfold = t < 5.4f ? seg(0.6f, 1.05f, 0.7f, 0.0f, 0.25f, 1.0f) : 1 - seg(5.4f, 5.85f, 0.7f, 0.0f, 0.25f, 1.0f);
    float shift = half * (1 - unfold);
    auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };
    float bx = 640 - (64 + pw) * 0.5f, by = 600;  // canvas: y 600 (the Guide is drawn small enough to end above it)
    ImU32 kc = toast_color(toast_.kind);
    if (unfold > 0.001f) {
        ImVec2 p0 = at(bx + 64 + shift, by), p1 = at(bx + 64 + pw - shift, by + 64);
        dl->AddRectFilled(p0, p1, PANEL);
        dl->PushClipRect(p0, p1, true);
        float x = at(bx + 64 + 22, 0).x;
        if (toast_.detail.empty()) {
            dl->AddText(bf, 17 * s, ImVec2(x, css_top(a.bold_m, 17 * s, at(0, by + 32).y)), kc, toast_.title.c_str());
        } else {
            // two 22-unit lines centred in the 64-unit panel: their line boxes are centred on by + 21 and by + 43
            dl->AddText(bf, 17 * s, ImVec2(x, css_top(a.bold_m, 17 * s, at(0, by + 21).y)), kc, toast_.title.c_str());
            dl->AddText(a.text, 17 * s, ImVec2(x, css_top(a.text_m, 17 * s, at(0, by + 43).y)), WHITE, toast_.detail.c_str());
        }
        dl->PopClipRect();
    }
    // the square: slides with the panel, zooms in and out; on an invite its icon slides up at 2.0-2.4 s and the other player's picture
    // comes up in its place
    float side = 64 * s * zoom;
    ImVec2 c = at(bx + 32 + shift, by + 32);
    ImVec2 q0(c.x - side / 2, c.y - side / 2), q1(c.x + side / 2, c.y + side / 2);
    if (side > 0.5f) {
        dl->AddRectFilled(q0, q1, alpha(kc, zop));
        float up = toast_.pic ? side * seg(2.0f, 2.4f, 0.3f, 0.0f, 0.2f, 1.0f) : 0;
        dl->PushClipRect(q0, q1, true);
        icon_mid(dl, a, 46 * s * zoom, c.x, c.y - up, alpha(WHITE, zop), toast_.icon);
        if (toast_.pic)
            dl->AddImage(toast_.pic, ImVec2(q0.x, q0.y + side - up), ImVec2(q1.x, q1.y + side - up), ImVec2(0, 0), ImVec2(1, 1), alpha(WHITE, zop));
        dl->PopClipRect();
    }
}
