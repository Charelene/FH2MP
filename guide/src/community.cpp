// Community, the 360's tabbed screen (Party, Friends, Messages, Players; the canvas' artboards 2 and 4), and a player's profile (Gamer
// Profile and Personal Profile, artboards 8 and 9) with the 360's option list: a fixed order per relationship, entries that do not apply
// left out, entries that cannot work right now greyed.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "guide.h"
#include "ui.h"

using namespace ui;

namespace {

const int FRIEND_ROWS = 6;  // 69-unit rows under the tabs
const int MSG_ROWS = 8;     // 56-unit rows
const uint32_t COMM_ICON[4] = {I_GROUPS, I_PERSON, I_MAIL, I_GAMES};

enum ProfileCmd { PInvite, PJoin, PMessage, PAddFriend, PAccept, PDecline, PCancel, PRemove, PBio, PEdit, PCompare };

// The profile texts a player can set, as the server names them, and how long each may be.
const char* FIELD_NAME[4] = {"Motto", "Name", "Location", "Bio"};

uint32_t kind_icon(const MessageItem& m) {
    if (m.kind == MessageItem::GameInvite) return I_GAMES;
    if (m.kind == MessageItem::FriendRequest) return I_PERSON_ADD;
    return m.unread ? I_MAIL : I_DRAFTS;
}

// The 360's one-line summaries of the Messages tab.
const char* summary(const MessageItem& m) {
    if (m.kind == MessageItem::GameInvite) return "Wants to play with you";
    if (m.kind == MessageItem::FriendRequest) return "Wants to be your friend";
    return "Sent you a message";
}

int rank(const Friend& f, Relation rel) {
    if (rel == Relation::RequestReceived) return 0;
    if (rel == Relation::RequestSent) return 3;
    return f.state == Friend::Playing ? 1 : f.state == Friend::Online ? 2 : 4;
}

std::string status_words(const Friend& f) {
    if (f.state == Friend::Playing) return "Forza Horizon 2";
    if (f.state == Friend::Online) return "Online";
    return f.last_seen >= 0 ? ago(f.last_seen, "Last online ") : "Offline";
}

}  // namespace

// The Friends tab's players: who asked to be a friend, friends in a game, friends online, requests this player sent, friends offline;
// by gamertag within each.
std::vector<Guide::FriendRow> Guide::friend_rows() const {
    std::vector<FriendRow> out;
    for (const Friend& f : social_.requests) out.push_back({&f, Relation::RequestReceived});
    for (const Friend& f : social_.friends) out.push_back({&f, Relation::Friend});
    for (const Friend& f : social_.outgoing) out.push_back({&f, Relation::RequestSent});
    std::stable_sort(out.begin(), out.end(), [](const FriendRow& x, const FriendRow& y) {
        int rx = rank(*x.f, x.rel), ry = rank(*y.f, y.rel);
        if (rx != ry) return rx < ry;
        return _stricmp(x.f->tag.c_str(), y.f->tag.c_str()) < 0;
    });
    return out;
}

// The Players tab: who is in this player's game right now, by gamertag (the 360 listed the players of the current game first; players
// met earlier come when the server keeps them).
std::vector<const Friend*> Guide::player_rows() const {
    std::vector<const Friend*> out;
    for (const Friend& f : social_.players) out.push_back(&f);
    std::stable_sort(out.begin(), out.end(), [](const Friend* x, const Friend* y) { return _stricmp(x->tag.c_str(), y->tag.c_str()) < 0; });
    return out;
}

// The Messages tab: friend requests, then game invites and friends' messages, newest first.
std::vector<MessageItem> Guide::messages() const {
    std::vector<MessageItem> out, dated;
    for (const Friend& f : social_.requests) {
        MessageItem m;
        m.kind = MessageItem::FriendRequest, m.from = f, m.unread = true;
        out.push_back(m);
    }
    for (const ReceivedInvite& iv : social_.invites) {
        MessageItem m;
        m.kind = MessageItem::GameInvite, m.from = iv.from, m.id = iv.id, m.text = iv.text, m.age = iv.age, m.expires_in = iv.expires_in;
        m.unread = true;
        dated.push_back(m);
    }
    double wall = double(time(nullptr));
    for (const TextMessage& t : social_.texts) {
        MessageItem m;
        m.kind = MessageItem::Text, m.id = t.id, m.text = t.body, m.chat = t.chat, m.unread = t.unread;
        m.age = t.created > 0 ? int(std::max(0.0, wall - t.created)) : -1;
        m.from.xuid = t.from, m.from.tag = t.tag;
        if (const Friend* f = find(t.from)) m.from = *f;
        dated.push_back(m);
    }
    std::stable_sort(dated.begin(), dated.end(), [](const MessageItem& x, const MessageItem& y) { return unsigned(x.age) < unsigned(y.age); });
    out.insert(out.end(), dated.begin(), dated.end());
    return out;
}

void Guide::delete_message(const MessageItem& m, std::vector<GuideRequest>& out, double now) {
    GuideRequest r;
    r.xuid = m.from.xuid, r.name = m.from.tag, r.id = m.id;
    if (m.kind == MessageItem::GameInvite) {
        r.action = GuideAction::DeclineInvite;
        for (size_t i = 0; i < social_.invites.size(); i++)
            if (social_.invites[i].id == m.id) social_.invites.erase(social_.invites.begin() + i--);  // until the server's list says so
    } else if (m.kind == MessageItem::FriendRequest) {
        r.action = GuideAction::DeclineFriend;
        for (size_t i = 0; i < social_.requests.size(); i++)
            if (social_.requests[i].xuid == m.from.xuid) social_.requests.erase(social_.requests.begin() + i--);
    } else {
        r.action = GuideAction::HideText;  // the chat keeps the line; the Messages tab stops listing it
        for (size_t i = 0; i < social_.texts.size(); i++)
            if (social_.texts[i].id == m.id) social_.texts.erase(social_.texts.begin() + i--);
    }
    out.push_back(r);
    messages_.clamp(1 + int(messages().size()), MSG_ROWS);
    (void)now;
}

void Guide::handle_community(PadEvent e, std::vector<GuideRequest>& out, double now) {
    if (e == PadEvent::Left || e == PadEvent::Lb || e == PadEvent::Right || e == PadEvent::Rb) {
        int d = (e == PadEvent::Left || e == PadEvent::Lb) ? -1 : 1;
        int t = std::clamp(comm_tab_ + d, 0, 3);
        if (t != comm_tab_) comm_dir_ = d, comm_tab_ = t, comm_at_ = now;
        return;
    }
    if (e == PadEvent::B) return pop(now);
    int dy = e == PadEvent::Up ? -1 : e == PadEvent::Down ? 1 : 0;
    if (comm_tab_ == TabFriends) {
        std::vector<FriendRow> rows = friend_rows();
        int n = 1 + int(rows.size());
        if (dy) {
            friends_.move(dy, n, FRIEND_ROWS, now);
        } else if (e == PadEvent::A && friends_.row == 0) {
            // Add Friend: the gamertag, then the request
            flow_base_ = int(stack_.size());
            draft_ = Draft();
            draft_.kind = Draft::FriendRequest;
            open_keyboard(Keys::Gamertag, "", now);
        } else if (e == PadEvent::A && friends_.row < n) {
            prof_ = *rows[friends_.row - 1].f;
            prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
            push(Page::Profile, now);
        }
    } else if (comm_tab_ == TabPlayers) {
        std::vector<const Friend*> rows = player_rows();
        int n = int(rows.size());
        if (dy && n) {
            players_.move(dy, n, FRIEND_ROWS, now);
        } else if (e == PadEvent::A && players_.row < n) {
            prof_ = *rows[players_.row];
            prof_tab_ = 0, prof_at_ = -10, prof_row_[0] = prof_row_[1] = -1;
            push(Page::Profile, now);
        }
    } else if (comm_tab_ == TabMessages) {
        std::vector<MessageItem> list = messages();
        int n = 1 + int(list.size());
        if (dy) {
            messages_.move(dy, n, MSG_ROWS, now);
        } else if ((e == PadEvent::A && messages_.row == 0) || e == PadEvent::Y) {
            flow_base_ = int(stack_.size());
            new_row_ = -1;
            push(Page::CreateNew, now);
        } else if (e == PadEvent::A && messages_.row < n) {
            open_msg_ = list[messages_.row - 1];
            msg_opt_ = -1;
            push(Page::Message, now);
            if (open_msg_.kind == MessageItem::Text && open_msg_.unread) {
                GuideRequest r;
                r.action = GuideAction::ReadChat, r.id = open_msg_.id, r.name = open_msg_.chat;
                out.push_back(r);
                for (TextMessage& t : social_.texts)
                    if (t.chat == open_msg_.chat && t.id <= open_msg_.id) t.unread = false;
            }
        } else if (e == PadEvent::X && messages_.row > 0 && messages_.row < n) {
            delete_message(list[messages_.row - 1], out, now);
        }
    }
}

void Guide::player_row(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float y, const Friend& f, Relation rel,
                       bool focused, bool greyed, int check, float op) const {
    View v{origin, s, zoom};
    const float W = PAGE_W;
    float cy = y + 34.5f;
    bool here = same_game(f, social_.me), off = f.state == Friend::Offline, sent = sent_.count(f.xuid) > 0;
    if (!focused) dl->AddLine(v.pt(x, y + 68.5f), v.pt(x + W, y + 68.5f), alpha(LINE, op), v.sz(1));
    player_pic(dl, a, a.pic(f.xuid), f.xuid, v.pt(x + 9, y + 4.5f), v.pt(x + 69, y + 64.5f), greyed ? op * 0.45f : op);
    ImU32 fg = greyed ? DISABLED : focused ? INK : off ? MUTED2 : WHITE, sub = greyed ? DISABLED : focused ? INK : MUTED2;
    // name column: 22 and 20 units, line-height 1.15, 3 apart, centred: the lines' centres 13 above and 14 below the row's
    std::string second = !f.motto.empty() ? f.motto : !f.club.empty() ? "[" + f.club + "]" : f.level > 0 ? "Level " + std::to_string(f.level) : "";
    ImVec2 n0 = v.pt(x + 78, second.empty() ? cy : cy - 13), n1 = v.pt(x + 78, cy + 14.15f);
    text_fit(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(305), alpha(fg, op), f.tag);
    if (!second.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), n1.x, n1.y, v.sz(305), alpha(sub, op), second);
    ImU32 ic = greyed ? DISABLED : focused ? INK : (f.state == Friend::Playing || here) ? BRIGHT : f.state == Friend::Online ? ONLINE : OFFLINE;
    ImVec2 pi = v.pt(x + 399 + 14, cy);
    icon_mid(dl, a, v.sz(28), pi.x, pi.y, alpha(ic, op), I_PERSON);
    std::string waiting = waiting_words(f, rel);
    std::string detail = !waiting.empty() ? waiting : sent ? "Invite sent" : here ? "In Your Game" : f.state == Friend::Playing ? lobby_words(f) : "";
    bool lit = !greyed && !focused && (!waiting.empty() || sent);
    float sw = W - 445 - (check >= 0 ? 70 : 20);
    ImVec2 s0 = v.pt(x + 445, detail.empty() ? cy : cy - 13), s1 = v.pt(x + 445, cy + 14.15f);
    text_fit(dl, a.text, a.text_m, v.sz(22), s0.x, s0.y, v.sz(sw), alpha(fg, op), status_words(f));
    if (!detail.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), s1.x, s1.y, v.sz(sw), alpha(lit ? BRIGHT : sub, op), detail);
    if (check >= 0) {
        ImVec2 cp = v.pt(x + W - 20 - 16, cy);
        icon_mid(dl, a, v.sz(32), cp.x, cp.y, alpha(greyed ? DISABLED : WHITE, op), check ? I_CHECK_BOX : I_CHECK_BLANK);
    }
}

// The chosen tab is 388 wide and named with its count; the others show their icon and count (artboard 2). Under them, 469 units of the
// chosen tab's rows.
void Guide::draw_community(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Community", title_op);
    float x = L + slide;

    std::vector<FriendRow> friends = friend_rows();
    std::vector<MessageItem> msgs = messages();
    int online = 0, unread = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    for (const MessageItem& m : msgs) unread += m.unread;
    std::vector<const Friend*> players = player_rows();
    int counts[4] = {0, online, int(msgs.size()), int(players.size())};
    float tx = x;
    for (int t = 0; t < 4; t++) {
        bool on = t == comm_tab_;
        float tw = on ? 388 : 158;
        dl->AddRectFilled(v.pt(tx, 96), v.pt(tx + tw, 138), alpha(on ? PANEL : TAB, pop));
        char label[48];
        if (!on) snprintf(label, sizeof label, "%d", counts[t]);
        else if (t == TabParty) snprintf(label, sizeof label, "Party (%d)", counts[t]);
        else if (t == TabFriends) snprintf(label, sizeof label, "Friends (%d Online)", online);
        else if (t == TabMessages) snprintf(label, sizeof label, "Messages (%d New)", unread);
        else snprintf(label, sizeof label, "Players (%d Online)", counts[t]);
        ImU32 fg = on ? WHITE : TAB_TEXT;
        if (on) {
            // canvas: the chosen tab's icon and name start 27 in, 14 apart
            ImVec2 ic = v.pt(tx + 27 + 14, 117), lp = v.pt(tx + 27 + 28 + 14, 117);
            icon_mid(dl, a, v.sz(28), ic.x, ic.y, alpha(fg, pop), COMM_ICON[t]);
            text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(fg, pop), label);
        } else {
            float lw = a.text->CalcTextSizeA(22, FLT_MAX, 0, label).x, cw = 26 + 12 + lw;
            float cx = tx + (tw - cw) / 2;
            ImVec2 ic = v.pt(cx + 13, 117), lp = v.pt(cx + 38, 117);
            icon_mid(dl, a, v.sz(26), ic.x, ic.y, alpha(fg, pop), COMM_ICON[t]);
            text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(fg, pop), label);
        }
        tx += tw;
    }
    ImVec2 p0 = v.pt(x, 138), p1 = v.pt(x + W, 607);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->PushClipRect(p0, p1, true);
    float ce = ease_guide(std::clamp(float((now - comm_at_) / 0.26), 0.0f, 1.0f));
    float cxs = x + (1 - ce) * 110 * comm_dir_, cop = pop * ce;
    auto empty = [&](const char* why) {
        float ww = a.text->CalcTextSizeA(22, FLT_MAX, 0, why).x;
        ImVec2 c = v.pt(cxs + (W - ww) / 2, 372);
        text_mid(dl, a.text, a.text_m, v.sz(22), c.x, c.y, alpha(MUTED, cop), why);
    };
    auto bar = [&](float row, float row_h) {
        float by = 138 + row * row_h;
        ImVec2 b0 = v.pt(cxs, by), b1 = v.pt(cxs + W, by + row_h);
        dl->AddRectFilled(b0, b1, alpha(ACCENT, cop));
        dl->AddLine(ImVec2(b0.x, b1.y - v.sz(0.5f)), ImVec2(b1.x, b1.y - v.sz(0.5f)), alpha(DARK, cop), v.sz(1));
    };
    switch (comm_tab_) {
    case TabFriends: {
        if (!social_.session) { empty("Not signed in"); break; }
        if (!social_.loaded && !social_.error.empty()) { empty("Server unavailable"); break; }
        int n = 1 + int(friends.size());
        bar(friends_.bar.value(now), 69);
        for (int i = friends_.top; i < std::min(n, friends_.top + FRIEND_ROWS); i++) {
            float y = 138 + (i - friends_.top) * 69;
            bool foc = i == friends_.row;
            if (i == 0) {
                // Add Friend, the list's first row
                if (!foc) dl->AddLine(v.pt(cxs, y + 68.5f), v.pt(cxs + W, y + 68.5f), alpha(LINE, cop), v.sz(1));
                dl->AddRectFilled(v.pt(cxs + 9, y + 4.5f), v.pt(cxs + 69, y + 64.5f), alpha(foc ? DARK : RAISED, cop));
                ImVec2 ip = v.pt(cxs + 39, y + 34.5f), lp = v.pt(cxs + 78, y + 34.5f);
                icon_mid(dl, a, v.sz(36), ip.x, ip.y, alpha(WHITE, cop), I_PERSON_ADD);
                text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(WHITE, cop), "Add Friend");
                continue;
            }
            const FriendRow& r = friends[i - 1];
            player_row(dl, a, origin, s, zoom, cxs, y, *r.f, r.rel, foc, false, -1, cop);
        }
        scroll_marks(dl, a, v, cxs + W - 30, 579.5f, friends_.top > 0, friends_.top + FRIEND_ROWS < n, cop);
        break;
    }
    case TabMessages: {
        // Create New, then the sender (left) and what it is (right: its icon and the 360's summary)
        bar(messages_.bar.value(now), 56);
        int n = 1 + int(msgs.size());
        for (int i = messages_.top; i < std::min(n, messages_.top + MSG_ROWS); i++) {
            float y = 138 + (i - messages_.top) * 56, cy = y + 28;
            bool foc = i == messages_.row;
            if (!foc) dl->AddLine(v.pt(cxs, y + 55.5f), v.pt(cxs + W, y + 55.5f), alpha(LINE, cop), v.sz(1));
            ImVec2 np = v.pt(cxs + 27, cy);
            if (i == 0) {
                text_mid(dl, a.text, a.text_m, v.sz(24), np.x, np.y, alpha(WHITE, cop), "Create New");
                continue;
            }
            const MessageItem& m = msgs[i - 1];
            ImU32 fg = foc || m.unread ? WHITE : MUTED;
            text_fit(dl, a.text, a.text_m, v.sz(24), np.x, np.y, v.sz(380), alpha(fg, cop), m.from.tag);
            ImVec2 ip = v.pt(cxs + 431 + 18, cy);
            icon_mid(dl, a, v.sz(30), ip.x, ip.y, alpha(fg, cop), kind_icon(m));
            ImVec2 sp = v.pt(cxs + 431 + 46, cy);
            text_fit(dl, a.text, a.text_m, v.sz(24), sp.x, sp.y, v.sz(W - 431 - 46 - 27), alpha(fg, cop), summary(m));
        }
        scroll_marks(dl, a, v, cxs + W - 30, 138 + MSG_ROWS * 56 + 10.5f, messages_.top > 0, messages_.top + MSG_ROWS < n, cop);
        break;
    }
    case TabParty: {
        // not in a party: the 360's one entry (parties come later, so it is greyed)
        std::vector<Opt> opts = {{"Start a Party", 0, false, 0}};
        option_list(dl, a, v, cxs, 138, W, 46, 22, 27, opts, -1, 0, cop);
        break;
    }
    default: {
        int n = int(players.size());
        if (!n) { empty("No recent players"); break; }
        bar(players_.bar.value(now), 69);
        for (int i = players_.top; i < std::min(n, players_.top + FRIEND_ROWS); i++) {
            float y = 138 + (i - players_.top) * 69;
            const Friend& f = *players[i];
            player_row(dl, a, origin, s, zoom, cxs, y, f, relation(f.xuid), i == players_.row, false, -1, cop);
            if (std::find(talking_.begin(), talking_.end(), f.xuid) != talking_.end()) {
                // heard right now: a microphone at the row's end
                ImVec2 mp = v.pt(cxs + W - 20 - 16, y + 34.5f);
                icon_mid(dl, a, v.sz(30), mp.x, mp.y, alpha(i == players_.row ? INK : BRIGHT, cop), I_MIC);
            }
        }
        scroll_marks(dl, a, v, cxs + W - 30, 579.5f, players_.top > 0, players_.top + FRIEND_ROWS < n, cop);
        break;
    }
    }
    dl->PopClipRect();
    if (comm_tab_ == TabMessages) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"X", "Delete", RING_X}, {"Y", "Create New", RING_Y}};
        hint_bar(dl, a, v, L + 27, 617, hints, 4, op);
    } else if (comm_tab_ == TabFriends || (comm_tab_ == TabPlayers && !social_.players.empty())) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
    } else {
        const Hint hints[] = {{"B", "Back", RING_B}};
        hint_bar(dl, a, v, L + 27, 617, hints, 1, op);
    }
}

// ---- a player's profile

// The 360's option list for a player, in its order. FH2MP sends invites and messages to friends only, so for anyone else those two are
// greyed; options whose feature is not built yet (party, private chat, player review) are not listed.
std::vector<Opt> Guide::profile_options(int tab) const {
    const Friend* live = find(prof_.xuid);
    const Friend& p = live ? *live : prof_;
    Relation rel = relation(p.xuid);
    std::vector<Opt> o;
    if (p.tag.empty()) return o;  // a player the server is still being asked about
    if (tab == 1) {
        o.push_back({"View Bio", PBio, !p.bio.empty(), 0});
        return o;
    }
    // the player's own profile (Settings, Profile): the 360's Edit Profile; greyed on a server that keeps no profile texts
    if (p.xuid && p.xuid == social_.me.xuid) return {{"Edit Profile", PEdit, p.has_profile, 0}};
    bool idle = waiting_xuid_ != p.xuid;
    if (rel == Relation::RequestReceived) {
        o.push_back({"Accept Friend Request", PAccept, true, 0});
        o.push_back({"Decline Friend Request", PDecline, true, 0});
    }
    if (rel == Relation::Stranger) o.push_back({"Send Friend Request", PAddFriend, true, 0});
    o.push_back({"Invite to Game", PInvite, can_invite(p) && idle, 0});
    if (can_join(p)) o.push_back({"Join Session In Progress", PJoin, idle, 0});
    o.push_back({"Send Message", PMessage, rel == Relation::Friend, 0});
    // the 360's Compare Games: their achievements beside this player's; greyed while neither side has any to show
    o.push_back({"Compare Games", PCompare, p.gamerscore >= 0 || ach_.known, 0});
    if (rel == Relation::Friend) o.push_back({"Remove Friend", PRemove, true, 0});
    if (rel == Relation::RequestSent) o.push_back({"Cancel Friend Request", PCancel, true, 0});
    return o;
}

void Guide::handle_profile(PadEvent e, std::vector<GuideRequest>& out, double now) {
    std::vector<Opt> opts = profile_options(prof_tab_);
    int& row = prof_row_[prof_tab_];
    const Friend* live = find(prof_.xuid);
    if (live) prof_ = *live;
    const Friend p = prof_;
    switch (e) {
    case PadEvent::Left:
    case PadEvent::Lb:
    case PadEvent::Right:
    case PadEvent::Rb: {
        int d = (e == PadEvent::Left || e == PadEvent::Lb) ? -1 : 1;
        int t = std::clamp(prof_tab_ + d, 0, 1);
        if (t == prof_tab_) break;
        prof_dir_ = d, prof_tab_ = t, prof_at_ = now;
        std::vector<Opt> o = profile_options(t);
        if (prof_row_[t] < 0 || prof_row_[t] >= int(o.size()) || !o[prof_row_[t]].enabled) prof_row_[t] = first_enabled(o);
        prof_bar_.set(float(std::max(0, prof_row_[t])));
        break;
    }
    case PadEvent::Up:
    case PadEvent::Down:
        row = step_focus(opts, row, e == PadEvent::Up ? -1 : 1);
        if (row >= 0) prof_bar_.go(float(row), now, 0.14f);
        break;
    case PadEvent::B: pop(now); break;
    case PadEvent::A: {
        if (row < 0 || row >= int(opts.size()) || !opts[row].enabled) break;
        GuideRequest r;
        r.xuid = p.xuid, r.name = p.tag;
        auto drop = [&](std::vector<Friend>& list) {
            for (size_t i = 0; i < list.size(); i++)
                if (list[i].xuid == p.xuid) list.erase(list.begin() + i--);
        };
        switch (opts[row].cmd) {
        case PInvite:
            r.action = GuideAction::Invite, waiting_xuid_ = p.xuid;
            out.push_back(r);
            break;
        case PJoin:
            held_ = 2, held_player_ = p;
            if (!ask_exit_session(now)) held_ = 0, join_player(p, out, now);
            break;
        case PMessage:
            flow_base_ = int(stack_.size());
            draft_ = Draft();
            draft_.kind = Draft::Message, draft_.to = {p};
            draft_row_ = -1;
            push(Page::Compose, now);
            return;
        case PAddFriend:
            flow_base_ = int(stack_.size());
            draft_ = Draft();
            draft_.kind = Draft::FriendRequest, draft_.gamertag = p.tag, draft_.xuid = p.xuid;
            draft_row_ = -1;
            push(Page::Compose, now);
            return;
        case PAccept:
            r.action = GuideAction::AcceptFriend;
            out.push_back(r);
            drop(social_.requests), social_.friends.push_back(p);  // until the server's list says so
            break;
        case PDecline:
            r.action = GuideAction::DeclineFriend;
            out.push_back(r);
            drop(social_.requests);
            break;
        case PCancel:
            r.action = GuideAction::DeclineFriend;
            out.push_back(r);
            drop(social_.outgoing);
            break;
        case PRemove:
            r.action = GuideAction::RemoveFriend;
            out.push_back(r);
            drop(social_.friends);
            break;
        case PBio: show_box(0, (p.tag + "'s bio").c_str(), p.bio, {"Back"}, now); break;
        case PCompare: open_compare(p, out, now); return;
        case PEdit:
            edit_row_ = -1;
            push(Page::EditProfile, now);
            return;
        }
        entered(Page::Profile, now);  // the list is another one now
        break;
    }
    default: break;
    }
}

// Join Session In Progress: a seat in their game is asked of the server; with it the game joins.
void Guide::join_player(const Friend& p, std::vector<GuideRequest>& out, double now) {
    (void)now;
    GuideRequest r;
    r.action = GuideAction::Join, r.xuid = p.xuid, r.name = p.tag, r.text = p.lobby_id, r.kind = p.lobby_kind;
    waiting_xuid_ = p.xuid;
    out.push_back(r);
}

// Artboards 8 and 9: two tabs (56 units), then 455 units: the options on the left (50-unit rows), the player's card and a pane on the right.
void Guide::draw_profile(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Profile", title_op);
    float x = L + slide;
    const Friend* live = find(prof_.xuid);
    const Friend& p = live ? *live : prof_;

    const char* names[2] = {"Gamer Profile", "Personal Profile"};
    for (int t = 0; t < 2; t++) {
        bool on = t == prof_tab_;
        float tx = x + t * 431;
        dl->AddRectFilled(v.pt(tx, 96), v.pt(tx + 431, 152), alpha(on ? PANEL : TAB, pop));
        float lw = a.text->CalcTextSizeA(24, FLT_MAX, 0, names[t]).x;
        ImVec2 lp = v.pt(on ? tx + 30 : tx + (431 - lw) / 2, 124);
        text_mid(dl, a.text, a.text_m, v.sz(24), lp.x, lp.y, alpha(on ? WHITE : TAB_TEXT, pop), names[t]);
    }
    ImVec2 p0 = v.pt(x, 152), p1 = v.pt(x + W, 607);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->PushClipRect(p0, p1, true);

    // the options (they slide in when the tab changes)
    float te = ease_guide(std::clamp(float((now - prof_at_) / 0.26), 0.0f, 1.0f));
    float ox = x + (1 - te) * 110 * prof_dir_, oop = pop * te;
    std::vector<Opt> opts = profile_options(prof_tab_);
    int row = prof_row_[prof_tab_];
    dl->PushClipRect(v.pt(x, 152), v.pt(x + 431, 607), true);
    option_list(dl, a, v, ox, 156, 431, 50, 23, 30, opts, row, prof_bar_.value(now), oop);
    dl->PopClipRect();

    float fade = pop * std::clamp(float((now - prof_at_) / 0.16), 0.0f, 1.0f);  // the pane fades when the tab changes
    profile_side(dl, a, origin, s, zoom, x, 152, p, prof_tab_ == 1, pop, fade);
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, L + 27, 617, hints, 4, op);
}

// The right half of a profile screen, from y top down to the panel's foot: the card (name and level on the accent, the picture, the
// facts) and under it the gamer pane (what they play, their presence) or the personal pane (Motto, Name, Location, Bio).
void Guide::profile_side(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float top, const Friend& p, bool personal,
                         float pop, float fade) const {
    View v{origin, s, zoom};
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    bool me = p.xuid && p.xuid == social_.me.xuid;
    float cx = x + 431 + 14, cw = 431 - 28, cy0 = top + 14;
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46 + 112), alpha(RAISED, pop));
    // the title bar in the card owner's theme (the player's own: the one in use, which the Theme screen previews)
    const ThemeDef& own = theme_preset(p.theme);
    ImU32 bar_col = me ? ACCENT : own.accent, ink = me ? INK : own.ink;
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + 46), alpha(bar_col, pop));
    float name_w = cw - 16 - 14;
    if (p.level > 0) {
        std::string lv = std::to_string(p.level);
        float lw = bf->CalcTextSizeA(32, FLT_MAX, 0, lv.c_str()).x, tw = bf->CalcTextSizeA(13, FLT_MAX, 0, "LVL").x;
        ImVec2 lp = v.pt(cx + cw - 14, cy0 + 23), tp = v.pt(cx + cw - 14 - lw - 12, cy0 + 23);
        text_mid(dl, bf, a.bold_m, v.sz(32), lp.x, lp.y, alpha(ink, pop), lv.c_str(), true);
        text_mid(dl, bf, a.bold_m, v.sz(13), tp.x, tp.y, alpha(ink, pop * 0.85f), "LVL", true);
        name_w -= lw + 12 + tw + 12;
    }
    ImVec2 np = v.pt(cx + 16, cy0 + 23);
    text_fit(dl, bf, a.bold_m, v.sz(25), np.x, np.y, v.sz(name_w), alpha(ink, pop), p.tag);
    player_pic(dl, a, me ? a.gamerpic : a.pic(p.xuid), p.xuid, v.pt(cx, cy0 + 46), v.pt(cx + 112, cy0 + 46 + 112), pop);
    std::vector<std::pair<std::string, std::string>> facts = {{"Club", p.club.empty() ? "-" : p.club}};
    // Gamerscore, as on the 360's gamercard: this player's from the Achievements mod's file, another player's from the server
    int score = me && ach_.known ? ach_.gamerscore() : p.gamerscore;
    if (score >= 0) facts.push_back({"Gamerscore", std::to_string(score)});
    if (!p.name.empty() && !personal) facts.push_back({"Name", p.name});
    float fh = 28.0f * facts.size() + 4.0f * (facts.size() - 1), fy = cy0 + 46 + (112 - fh) / 2 + 14;
    for (size_t i = 0; i < facts.size(); i++) {
        ImVec2 l = v.pt(cx + 112 + 16, fy + i * 32), r = v.pt(cx + cw - 16, fy + i * 32);
        text_mid(dl, a.text, a.text_m, v.sz(21), l.x, l.y, alpha(WHITE, pop), facts[i].first.c_str());
        float lw = a.text->CalcTextSizeA(21, FLT_MAX, 0, facts[i].first.c_str()).x;
        std::string val = facts[i].second;
        float vw = a.text->CalcTextSizeA(21, FLT_MAX, 0, val.c_str()).x, room = cw - 112 - 32 - lw - 12;
        if (vw <= room) text_mid(dl, a.text, a.text_m, v.sz(21), r.x, r.y, alpha(WHITE, pop), val.c_str(), true);
        else text_fit(dl, a.text, a.text_m, v.sz(21), l.x + v.sz(lw + 12), l.y, v.sz(room), alpha(WHITE, pop), val);
    }

    float py = cy0 + 46 + 112 + 14, pb = 607 - 14;
    dl->AddRectFilled(v.pt(cx, py), v.pt(cx + cw, pb), alpha(PANE, pop));
    dl->PushClipRect(v.pt(cx, py), v.pt(cx + cw, pb), true);
    float ix = cx + 20, iw = cw - 40;
    if (p.tag.empty()) {
        // a player the server is still being asked about: nothing is claimed about them yet
    } else if (!personal) {
        float y = py + 16;
        if (!p.location.empty()) {
            ImVec2 lp = v.pt(ix, y + 14);
            text_fit(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, v.sz(iw), alpha(WHITE, fade), p.location);
            y += 28 + 16;
        }
        if (p.state == Friend::Playing) {
            // what they are playing: the accent circle, the game, where in it
            ImVec2 c = v.pt(ix + 26, y + 26);
            dl->AddCircleFilled(c, v.sz(26), alpha(ACCENT, fade), 48);
            icon_mid(dl, a, v.sz(30), c.x, c.y, alpha(WHITE, fade), I_CAR);
            std::string where = !me && same_game(p, social_.me) ? "In Your Game" : lobby_words(p);
            ImVec2 g0 = v.pt(ix + 52 + 14, where.empty() ? y + 26 : y + 14), g1 = v.pt(ix + 52 + 14, y + 39);
            text_fit(dl, a.text, a.text_m, v.sz(21), g0.x, g0.y, v.sz(iw - 66), alpha(WHITE, fade), "Forza Horizon 2");
            if (!where.empty()) text_fit(dl, a.text, a.text_m, v.sz(17), g1.x, g1.y, v.sz(iw - 66), alpha(MUTED, fade), where);
        }
        // presence, at the pane's foot
        bool on = p.state != Friend::Offline || me;
        std::string pres = on ? "Online" : p.last_seen >= 0 ? ago(p.last_seen, "Last online ") : "Offline";
        std::string waiting = me ? "" : waiting_words(p, relation(p.xuid));
        if (!waiting.empty()) pres += " \xC2\xB7 " + waiting;
        ImVec2 ip = v.pt(ix + 12, pb - 16 - 13), tp = v.pt(ix + 32, pb - 16 - 13);
        icon_mid(dl, a, v.sz(24), ip.x, ip.y, alpha(on ? BRIGHT : OFFLINE, fade), I_PERSON);
        text_fit(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, v.sz(iw - 32), alpha(WHITE, fade), pres);
    } else {
        // a 15-unit label over 19-unit text; the 360's words when there is none
        const std::string* vals[4] = {&p.motto, &p.name, &p.location, &p.bio};
        const char* none[4] = {"(No motto)", "(No name)", "(No location)", "(No bio)"};
        float y = py + 14;
        for (int i = 0; i < 4; i++) {
            ImVec2 lp = v.pt(ix, y + 10);
            text_mid(dl, a.text, a.text_m, v.sz(15), lp.x, lp.y, alpha(MUTED, fade), FIELD_NAME[i]);
            y += 20;
            bool has = !vals[i]->empty();
            y += wrapped(dl, a, v, 19, 1.25f, ix, y, iw, alpha(has ? WHITE : MUTED2, fade), has ? *vals[i] : none[i]);
            y += 8;
        }
    }
    dl->PopClipRect();
}

// ---- Edit Profile (the player's own): the four texts on the left, the card and the personal pane on the right

std::vector<Opt> Guide::edit_options() const {
    std::vector<Opt> o;
    for (int i = 0; i < 4; i++) o.push_back({FIELD_NAME[i], i, true, 0});
    return o;
}

void Guide::handle_edit(PadEvent e, std::vector<GuideRequest>& out, double now) {
    (void)out;
    std::vector<Opt> opts = edit_options();
    switch (e) {
    case PadEvent::Up:
    case PadEvent::Down:
        edit_row_ = step_focus(opts, edit_row_, e == PadEvent::Up ? -1 : 1);
        if (edit_row_ >= 0) edit_bar_.go(float(edit_row_), now, 0.14f);
        break;
    case PadEvent::B: pop(now); break;
    case PadEvent::A: {
        if (edit_row_ < 0 || edit_row_ >= 4) break;
        const Friend& me = social_.me;
        const std::string* vals[4] = {&me.motto, &me.name, &me.location, &me.bio};
        const size_t max[4] = {21, 40, 40, 500};  // the server's limits
        int field = edit_row_;
        open_keyboard(Keys::Field, *vals[field], now);
        keys_.field = field, keys_.max = max[field];
        break;
    }
    default: break;
    }
}

void Guide::draw_edit(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, L, W, "Edit Profile", title_op);
    float x = L + slide;
    ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + W, 607);
    dl->AddRectFilled(p0, p1, alpha(PANEL, pop));
    dl->PushClipRect(p0, p1, true);
    option_list(dl, a, v, x, 100, 431, 50, 23, 30, edit_options(), edit_row_, edit_bar_.value(now), pop);
    Friend me = social_.me;
    if (me.tag.empty()) me.tag = gamertag;
    profile_side(dl, a, origin, s, zoom, x, 96, me, true, pop, pop);
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, L + 27, 617, hints, 2, op);
}
