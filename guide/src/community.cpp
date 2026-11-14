// Community, the 360's tabbed screen (Party, Friends, Messages, Players; the canvas' artboards 2 and 4), and a player's profile (Gamer
// Profile and Personal Profile, artboards 8 and 9) with the 360's option list: a fixed order per relationship, entries that do not apply
// left out, entries that cannot work right now greyed.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <ctime>

#include "guide.h"
#include "one.h"
#include "ui.h"

using namespace ui;

namespace {

const int FRIEND_ROWS = 6;  // 69-unit rows under the tabs
const int MSG_ROWS = 8;     // 56-unit rows
const int ONE_MSG_ROWS = 6; // the Xbox One layout's cards, 139 apart
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

// The Party tab: the players of this player's private game (an invite-only game is the party: its leader starts the events and can
// remove players), this player first, then the others by gamertag. Empty when the player is in no private game.
std::vector<const Friend*> Guide::party_rows() const {
    std::vector<const Friend*> out;
    if (!social_.me.lobby_private || social_.me.lobby_id.empty()) return out;
    std::vector<const Friend*> others = player_rows();
    out.push_back(&social_.me);
    out.insert(out.end(), others.begin(), others.end());
    return out;
}

// The Messages tab: friend requests, then game invites and friends' messages, newest first.
int Guide::msg_rows() const { return guide_style_ == 1 ? ONE_MSG_ROWS : MSG_ROWS; }

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
    messages_.clamp(1 + int(messages().size()), msg_rows());
    (void)now;
}

void Guide::handle_community(PadEvent e, std::vector<GuideRequest>& out, double now) {
    if (guide_style_ == 1) {
        // the Xbox One layout: Social and Messages are tabs of the guide. LB and RB go from tab to tab; left and right go through
        // Social's lists (Party, Friends, Players) and on to the next tab at their ends; B closes the guide, as on any tab
        static const int ORDER[3] = {TabParty, TabFriends, TabPlayers};
        const int d = (e == PadEvent::Left || e == PadEvent::Lb) ? -1 : 1, i = comm_tab_ == TabParty ? 0 : comm_tab_ == TabFriends ? 1 : 2;
        if (e == PadEvent::Lb || e == PadEvent::Rb) return one_tab_to(one_tab() + d, now);
        if (e == PadEvent::Left || e == PadEvent::Right) {
            if (comm_tab_ == TabMessages || i + d < 0 || i + d > 2) return one_tab_to(one_tab() + d, now);
            comm_dir_ = d, comm_tab_ = ORDER[i + d], comm_at_ = now;
            return;
        }
        if (e == PadEvent::B) return toggle(now);
    }
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
    } else if (comm_tab_ == TabParty) {
        std::vector<const Friend*> rows = party_rows();
        int n = int(rows.size());
        if (dy && n) {
            party_.move(dy, n, FRIEND_ROWS, now);
        } else if (e == PadEvent::A && party_.row < n) {
            prof_ = *rows[party_.row];
            if (prof_.tag.empty()) prof_.tag = gamertag;
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
            messages_.move(dy, n, msg_rows(), now);
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
    ImU32 fg = greyed ? DISABLED : focused ? INK : off ? MUTED2 : TEXT, sub = greyed ? DISABLED : focused ? INK : MUTED2;
    // name column: 22 and 20 units, line-height 1.15, 3 apart, centred: the lines' centres 13 above and 14 below the row's
    std::string second = !f.motto.empty() ? f.motto : !f.club.empty() ? "[" + f.club + "]" : f.level > 0 ? "Level " + std::to_string(f.level) : "";
    ImVec2 n0 = v.pt(x + 78, second.empty() ? cy : cy - 13), n1 = v.pt(x + 78, cy + 14.15f);
    if (fg == TEXT) row_text(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(305), alpha(fg, op), f.tag, v.sz(uu(1)), op);
    else text_fit(dl, a.text, a.text_m, v.sz(22), n0.x, n0.y, v.sz(305), alpha(fg, op), f.tag);
    if (!second.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), n1.x, n1.y, v.sz(305), alpha(sub, op), second);
    ImU32 ic = greyed ? DISABLED : focused ? INK : (f.state == Friend::Playing || here) ? BRIGHT : f.state == Friend::Online ? ONLINE : OFFLINE;
    ImVec2 pi = v.pt(x + 399 + 14, cy);
    icon_mid(dl, a, v.sz(28), pi.x, pi.y, alpha(ic, op), I_PERSON);
    std::string waiting = waiting_words(f, rel);
    std::string detail = !waiting.empty() ? waiting : sent ? "Invite sent" : here ? "In Your Game" : f.state == Friend::Playing ? lobby_words(f) : "";
    bool lit = !greyed && !focused && (!waiting.empty() || sent);
    float sw = W - 445 - (check >= 0 ? 70 : 20);
    ImVec2 s0 = v.pt(x + 445, detail.empty() ? cy : cy - 13), s1 = v.pt(x + 445, cy + 14.15f);
    if (fg == TEXT) row_text(dl, a.text, a.text_m, v.sz(22), s0.x, s0.y, v.sz(sw), alpha(fg, op), status_words(f), v.sz(uu(1)), op);
    else text_fit(dl, a.text, a.text_m, v.sz(22), s0.x, s0.y, v.sz(sw), alpha(fg, op), status_words(f));
    if (!detail.empty()) text_fit(dl, a.text, a.text_m, v.sz(20), s1.x, s1.y, v.sz(sw), alpha(lit ? BRIGHT : sub, op), detail);
    if (check >= 0) {
        ImVec2 cp = v.pt(x + W - 20 - 16, cy);
        icon_mid(dl, a, v.sz(32), cp.x, cp.y, alpha(greyed ? DISABLED : focused ? INK : TEXT, op), check ? I_CHECK_BOX : I_CHECK_BLANK);
    }
}

// The chosen tab is 388 wide and named with its count; the others show their icon and count (artboard 2). Under them, the chosen tab's
// rows down to the page's foot. The tabs are the 360's tab strip: the chosen one the panel itself, the others the blades' colour, each
// under the shadow of its neighbour on the chosen tab's side.
void Guide::draw_community(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    if (guide_style_ == 1) return draw_community_one(dl, a, origin, s, op, now);
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Community", title_op);
    float x = L + slide;

    std::vector<FriendRow> friends = friend_rows();
    std::vector<MessageItem> msgs = messages();
    int online = 0, unread = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    for (const MessageItem& m : msgs) unread += m.unread;
    std::vector<const Friend*> players = player_rows();
    std::vector<const Friend*> party = party_rows();
    int counts[4] = {int(party.size()), online, int(msgs.size()), int(players.size())};
    float tx = x;
    for (int t = 0; t < 4; t++) {
        bool on = t == comm_tab_;
        float tw = on ? 388 : 158;
        if (on) panel_fill(dl, v.pt(tx, 96), v.pt(tx + tw, 138), pop);
        else dl->AddRectFilled(v.pt(tx, 96), v.pt(tx + tw, 138), alpha(TAB, pop));
        if (t > comm_tab_) tab_shadow(dl, v, tx, 96, 138, 1, pop);
        if (t < comm_tab_) tab_shadow(dl, v, tx + tw, 96, 138, -1, pop);
        char label[48];
        if (!on) snprintf(label, sizeof label, "%d", counts[t]);
        else if (t == TabParty) snprintf(label, sizeof label, "Party (%d)", counts[t]);
        else if (t == TabFriends) snprintf(label, sizeof label, "Friends (%d Online)", online);
        else if (t == TabMessages) snprintf(label, sizeof label, "Messages (%d New)", unread);
        else snprintf(label, sizeof label, "Players (%d Online)", counts[t]);
        ImU32 fg = on ? TEXT : TAB_TEXT;
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
    ImVec2 p0 = v.pt(x, 138), p1 = v.pt(x + W, PAGE_B);
    panel_fill(dl, p0, p1, pop);
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
                icon_mid(dl, a, v.sz(36), ip.x, ip.y, alpha(foc ? INK : TEXT, cop), I_PERSON_ADD);
                if (foc) text_mid(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, alpha(INK, cop), "Add Friend");
                else row_text(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, FLT_MAX, alpha(TEXT, cop), "Add Friend", v.sz(uu(1)), cop);
                continue;
            }
            const FriendRow& r = friends[i - 1];
            player_row(dl, a, origin, s, zoom, cxs, y, *r.f, r.rel, foc, false, -1, cop);
        }
        scroll_marks(dl, a, v, cxs + W - 30, PAGE_B - 27.5f, friends_.top > 0, friends_.top + FRIEND_ROWS < n, cop);
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
                if (foc) text_mid(dl, a.text, a.text_m, v.sz(24), np.x, np.y, alpha(INK, cop), "Create New");
                else row_text(dl, a.text, a.text_m, v.sz(24), np.x, np.y, FLT_MAX, alpha(TEXT, cop), "Create New", v.sz(uu(1)), cop);
                continue;
            }
            const MessageItem& m = msgs[i - 1];
            ImU32 fg = foc ? INK : m.unread ? TEXT : MUTED;
            if (fg == TEXT) row_text(dl, a.text, a.text_m, v.sz(24), np.x, np.y, v.sz(380), alpha(fg, cop), m.from.tag, v.sz(uu(1)), cop);
            else text_fit(dl, a.text, a.text_m, v.sz(24), np.x, np.y, v.sz(380), alpha(fg, cop), m.from.tag);
            ImVec2 ip = v.pt(cxs + 431 + 18, cy);
            icon_mid(dl, a, v.sz(30), ip.x, ip.y, alpha(fg, cop), kind_icon(m));
            ImVec2 sp = v.pt(cxs + 431 + 46, cy);
            text_fit(dl, a.text, a.text_m, v.sz(24), sp.x, sp.y, v.sz(W - 431 - 46 - 27), alpha(fg, cop), summary(m));
        }
        scroll_marks(dl, a, v, cxs + W - 30, 138 + MSG_ROWS * 56 + 10.5f, messages_.top > 0, messages_.top + MSG_ROWS < n, cop);
        break;
    }
    case TabParty: {
        int n = int(party.size());
        if (!n) {
            // not in a party: the 360's one entry (a party is started by inviting friends to a private game, so it is greyed here)
            std::vector<Opt> opts = {{"Start a Party", 0, false, 0}};
            option_list(dl, a, v, cxs, 138, W, 42, 24, 27, opts, -1, 0, cop);
            break;
        }
        bar(party_.bar.value(now), 69);
        for (int i = party_.top; i < std::min(n, party_.top + FRIEND_ROWS); i++) {
            float y = 138 + (i - party_.top) * 69;
            Friend f = *party[i];
            if (f.tag.empty()) f.tag = gamertag;
            bool foc = i == party_.row, mine = f.xuid == social_.me.xuid;
            player_row(dl, a, origin, s, zoom, cxs, y, f, mine ? Relation::Friend : relation(f.xuid), foc, false, -1, cop);
            float ex = cxs + W - 20 - 16;
            bool talks = std::find(talking_.begin(), talking_.end(), mine ? 0 : f.xuid) != talking_.end();
            if (talks) {
                ImVec2 mp = v.pt(ex, y + 34.5f);
                icon_mid(dl, a, v.sz(30), mp.x, mp.y, alpha(foc ? INK : BRIGHT, cop), I_MIC);
                ex -= 38;
            }
            if (social_.me.lobby_leader && f.xuid == social_.me.lobby_leader) {
                // the party's leader
                ImVec2 sp = v.pt(ex, y + 34.5f);
                icon_mid(dl, a, v.sz(30), sp.x, sp.y, alpha(foc ? INK : BRIGHT, cop), I_STAR);
            }
        }
        scroll_marks(dl, a, v, cxs + W - 30, PAGE_B - 27.5f, party_.top > 0, party_.top + FRIEND_ROWS < n, cop);
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
        scroll_marks(dl, a, v, cxs + W - 30, PAGE_B - 27.5f, players_.top > 0, players_.top + FRIEND_ROWS < n, cop);
        break;
    }
    }
    dl->PopClipRect();
    if (comm_tab_ == TabMessages) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"X", "Delete", RING_X}, {"Y", "Create New", RING_Y}};
        hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 4, op);
    } else if (comm_tab_ == TabFriends || (comm_tab_ == TabPlayers && !social_.players.empty()) || (comm_tab_ == TabParty && !party.empty())) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
        hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
    } else {
        const Hint hints[] = {{"B", "Back", RING_B}};
        hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 1, op);
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

// Artboards 8 and 9: two tabs, then the options on the left and the player's card and a pane on the right. The tabs are the 360's tab
// strip (42 canvas units, the chosen one the panel itself), the options its rows (28 of its units high, their text 18 in).
void Guide::draw_profile(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now) {
    if (guide_style_ == 1) return draw_profile_one(dl, a, origin, s, op, now);
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Profile", title_op);
    float x = L + slide;
    const Friend* live = find(prof_.xuid);
    const Friend& p = live ? *live : prof_;

    const char* names[2] = {"Gamer Profile", "Personal Profile"};
    for (int t = 0; t < 2; t++) {
        bool on = t == prof_tab_;
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

    // the options (they slide in when the tab changes)
    float te = ease_guide(std::clamp(float((now - prof_at_) / 0.26), 0.0f, 1.0f));
    float ox = x + (1 - te) * 110 * prof_dir_, oop = pop * te;
    std::vector<Opt> opts = profile_options(prof_tab_);
    int row = prof_row_[prof_tab_];
    dl->PushClipRect(v.pt(x, 138), v.pt(x + 431, PAGE_B), true);
    option_list(dl, a, v, ox, 138, 431, 42, 24, 27, opts, row, prof_bar_.value(now), oop);
    dl->PopClipRect();

    float fade = pop * std::clamp(float((now - prof_at_) / 0.16), 0.0f, 1.0f);  // the pane fades when the tab changes
    profile_side(dl, a, origin, s, zoom, x, 138, p, prof_tab_ == 1, pop, fade);
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 4, op);
}

// The right half of a profile screen, from y top down to the page's foot: the 360's description pane with the player's card at its top,
// laid out as the 360's gamer card is (gamercard.xur, in its units): a lighter band 103 high across the pane, the card 270 x 85 nine in
// from the band's corner, its title bar 24 high (the name 9 in), the picture 61 square under it, the facts 16 high from 70 in and 18
// apart, their values ending 9 from the card's edge. Under the band, on the pane itself: what they play and their presence, or the
// personal texts (Motto, Name, Location, Bio).
void Guide::profile_side(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float top, const Friend& p, bool personal,
                         float pop, float fade) const {
    View v{origin, s, zoom};
    ImFont* bf = a.text_bold ? a.text_bold : a.text;
    bool me = p.xuid && p.xuid == social_.me.xuid;
    const float px = x + 431, pw = PAGE_W - 431, band = uu(103), bar = uu(24), pic = uu(61);
    side_fill(dl, v.pt(px, top), v.pt(px + pw, PAGE_B), pop);
    raised_fill(dl, v.pt(px, top), v.pt(px + pw, top + band), pop);
    float cx = px + uu(9), cw = uu(270), cy0 = top + uu(9);
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + uu(85)), alpha(CARD, pop));
    // the title bar in the card owner's theme (the player's own: the one in use, which the Theme screen previews)
    const ThemeDef& own = theme_preset(p.theme);
    ImU32 bar_col = me ? ACCENT : own.accent, ink = me ? INK : own.ink;
    dl->AddRectFilled(v.pt(cx, cy0), v.pt(cx + cw, cy0 + bar), alpha(bar_col, pop));
    float name_w = cw - uu(18);
    if (p.level > 0) {
        std::string lv = std::to_string(p.level);
        float lw = bf->CalcTextSizeA(27, FLT_MAX, 0, lv.c_str()).x, tw = bf->CalcTextSizeA(12, FLT_MAX, 0, "LVL").x;
        ImVec2 lp = v.pt(cx + cw - uu(9), cy0 + bar / 2), tp = v.pt(cx + cw - uu(9) - lw - 10, cy0 + bar / 2);
        text_mid(dl, bf, a.bold_m, v.sz(27), lp.x, lp.y, alpha(ink, pop), lv.c_str(), true);
        text_mid(dl, bf, a.bold_m, v.sz(12), tp.x, tp.y, alpha(ink, pop * 0.85f), "LVL", true);
        name_w -= lw + 10 + tw + 12;
    }
    ImVec2 np = v.pt(cx + uu(9), cy0 + bar / 2);
    text_fit(dl, a.text, a.text_m, v.sz(uu(16)), np.x, np.y, v.sz(name_w), alpha(ink, pop), p.tag);
    player_pic(dl, a, me ? a.gamerpic : a.pic(p.xuid), p.xuid, v.pt(cx, cy0 + bar), v.pt(cx + pic, cy0 + bar + pic), pop);
    std::vector<std::pair<std::string, std::string>> facts = {{"Club", p.club.empty() ? "-" : p.club}};
    // Gamerscore, as on the 360's gamercard: this player's from the Achievements mod's file, another player's from the server
    int score = me && ach_.known ? ach_.gamerscore() : p.gamerscore;
    if (score >= 0) facts.push_back({"Gamerscore", std::to_string(score)});
    if (!p.name.empty() && !personal) facts.push_back({"Name", p.name});
    const float fs = uu(16), fl = cx + uu(70), fr = cx + cw - uu(9);
    for (size_t i = 0; i < facts.size(); i++) {
        float fy = cy0 + bar + uu(18.0f * i + 9.9f);
        ImVec2 l = v.pt(fl, fy), r = v.pt(fr, fy);
        text_mid(dl, a.text, a.text_m, v.sz(fs), l.x, l.y, alpha(TEXT, pop), facts[i].first.c_str());
        float lw = a.text->CalcTextSizeA(fs, FLT_MAX, 0, facts[i].first.c_str()).x;
        std::string val = facts[i].second;
        float vw = a.text->CalcTextSizeA(fs, FLT_MAX, 0, val.c_str()).x, room = fr - fl - lw - 12;
        if (vw <= room) text_mid(dl, a.text, a.text_m, v.sz(fs), r.x, r.y, alpha(TEXT, pop), val.c_str(), true);
        else text_fit(dl, a.text, a.text_m, v.sz(fs), l.x + v.sz(lw + 12), l.y, v.sz(room), alpha(TEXT, pop), val);
    }

    float py = top + band, pb = PAGE_B - uu(9);
    dl->PushClipRect(v.pt(px, py), v.pt(px + pw, PAGE_B), true);
    float ix = px + uu(16), iw = pw - uu(32);
    if (p.tag.empty()) {
        // a player the server is still being asked about: nothing is claimed about them yet
    } else if (!personal) {
        float y = py + 16;
        if (!p.location.empty()) {
            ImVec2 lp = v.pt(ix, y + 14);
            text_fit(dl, a.text, a.text_m, v.sz(22), lp.x, lp.y, v.sz(iw), alpha(SIDE_TEXT, fade), p.location);
            y += 28 + 16;
        }
        if (p.state == Friend::Playing) {
            // what they are playing: the accent circle, the game, where in it
            ImVec2 c = v.pt(ix + 26, y + 26);
            dl->AddCircleFilled(c, v.sz(26), alpha(ACCENT, fade), 48);
            icon_mid(dl, a, v.sz(30), c.x, c.y, alpha(INK, fade), I_CAR);
            std::string where = !me && same_game(p, social_.me) ? "In Your Game" : lobby_words(p);
            ImVec2 g0 = v.pt(ix + 52 + 14, where.empty() ? y + 26 : y + 14), g1 = v.pt(ix + 52 + 14, y + 39);
            text_fit(dl, a.text, a.text_m, v.sz(21), g0.x, g0.y, v.sz(iw - 66), alpha(SIDE_TEXT, fade), "Forza Horizon 2");
            if (!where.empty()) text_fit(dl, a.text, a.text_m, v.sz(17), g1.x, g1.y, v.sz(iw - 66), alpha(SIDE_MUTED, fade), where);
        }
        // presence, at the pane's foot
        bool on = p.state != Friend::Offline || me;
        std::string pres = on ? "Online" : p.last_seen >= 0 ? ago(p.last_seen, "Last online ") : "Offline";
        std::string waiting = me ? "" : waiting_words(p, relation(p.xuid));
        if (!waiting.empty()) pres += " \xC2\xB7 " + waiting;
        ImVec2 ip = v.pt(ix + 12, pb - 13), tp = v.pt(ix + 32, pb - 13);
        icon_mid(dl, a, v.sz(24), ip.x, ip.y, alpha(on ? SIDE_TEXT : OFFLINE, fade), I_PERSON);
        text_fit(dl, a.text, a.text_m, v.sz(22), tp.x, tp.y, v.sz(iw - 32), alpha(SIDE_TEXT, fade), pres);
    } else {
        // a 15-unit label over 19-unit text; the 360's words when there is none
        const std::string* vals[4] = {&p.motto, &p.name, &p.location, &p.bio};
        const char* none[4] = {"(No motto)", "(No name)", "(No location)", "(No bio)"};
        float y = py + 14;
        for (int i = 0; i < 4; i++) {
            ImVec2 lp = v.pt(ix, y + 10);
            text_mid(dl, a.text, a.text_m, v.sz(15), lp.x, lp.y, alpha(SIDE_MUTED, fade), FIELD_NAME[i]);
            y += 20;
            bool has = !vals[i]->empty();
            y += wrapped(dl, a, v, 19, 1.25f, ix, y, iw, alpha(has ? SIDE_TEXT : SIDE_MUTED, fade), has ? *vals[i] : none[i]);
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
    if (guide_style_ == 1) return draw_edit_one(dl, a, origin, s, op, now);
    View v{origin, s, zoom};
    const float L = PAGE_L, W = PAGE_W;
    float slide, pop, title_op;
    page_motion(now, op, &slide, &pop, &title_op);
    draw_header(dl, a, origin, s, zoom, op, "Edit Profile", title_op);
    float x = L + slide;
    ImVec2 p0 = v.pt(x, 96), p1 = v.pt(x + W, PAGE_B);
    panel_fill(dl, p0, p1, pop);
    dl->PushClipRect(p0, p1, true);
    option_list(dl, a, v, x, 96, 431, 42, 24, 27, edit_options(), edit_row_, edit_bar_.value(now), pop);

    Friend me = social_.me;
    if (me.tag.empty()) me.tag = gamertag;
    profile_side(dl, a, origin, s, zoom, x, 96, me, true, pop, pop);
    dl->PopClipRect();
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    hint_bar(dl, a, v, HINT_X, HINT_Y, hints, 2, op);
}

// ---- Community in the Xbox One layout (one.h)

// A player's entry, two lines: their picture (a dot on it when they are online), their gamertag, and under it what they are doing and
// what waits between them and this player.
void Guide::one_player(const one::Panel& p, float y, const Friend& f, Relation rel, bool greyed) const {
    using namespace one;
    bool here = same_game(f, social_.me), off = f.state == Friend::Offline, sent = sent_.count(f.xuid) > 0;
    ImVec2 c = p.L(PX + 60, y + TWO_H / 2);
    p.avatar(p.a.pic(f.xuid), f.xuid, f.tag, c, 34, p.lcol(WHITE, greyed ? 0.45f : 1));
    if (!off && !greyed) {
        ImVec2 d(c.x + 24 * p.U, c.y + 24 * p.U);
        p.dl->AddCircleFilled(d, 11 * p.U, p.lcol(PANEL1), 24);
        p.dl->AddCircleFilled(d, 8 * p.U, p.lcol(IM_COL32(0x10, 0x7c, 0x10, 255)), 24);
    }
    std::string waiting = waiting_words(f, rel);
    std::string detail = !waiting.empty() ? waiting : sent ? "Invite sent" : here ? "In Your Game" : f.state == Friend::Playing ? lobby_words(f) : "";
    std::string second = status_words(f) + (detail.empty() ? "" : "  -  " + detail);
    bool lit = !greyed && (!waiting.empty() || sent);
    p.two(y, PX + 112, f.tag, second, !greyed && !off, 380, lit ? blend(ACCENT, WHITE, 0.4f) : GREY1);
    if (off && !greyed) p.text(p.L(PX + 112, y + 33), 26, p.lcol(GREY1), f.tag, false, -1, 380);
}

static const float NOTE_H = 124, NOTE_P = 139;  // a notification card, and the cards' pitch

// How old a message is, as the console's notifications say it: "now", "5m", "3h", "2d", "1w".
static std::string brief(int secs) {
    if (secs < 60) return "now";
    static const int PER[4] = {60, 3600, 86400, 604800};
    int i = secs < PER[1] ? 0 : secs < PER[2] ? 1 : secs < PER[3] ? 2 : 3;
    return std::to_string(secs / PER[i]) + "mhdw"[i];
}

// A message on one of the console's notification cards (the card under it is the caller's: one::Panel::card): the sender's picture
// with what it is in a small disc at its foot, their gamertag in bold over the 360's summary, how old it is at the right end, and a
// mark in the accent down its left edge while it is unread.
void Guide::one_note(const one::Panel& p, float y, const MessageItem& m) const {
    using namespace one;
    const float x = PX + 9, cy = y + NOTE_H / 2;
    if (m.unread) p.dl->AddRectFilled(p.L(x, y), p.L(x + 7, y + NOTE_H), p.lcol(ACCENT), 3 * p.U);
    p.avatar(p.a.pic(m.from.xuid), m.from.xuid, m.from.tag, p.L(x + 59, cy), 34, p.lcol(WHITE));
    ImVec2 b = p.L(x + 84, cy + 24);
    p.dl->AddCircleFilled(b, 17 * p.U, p.lcol(PANEL1), 24);
    p.dl->AddCircleFilled(b, 15 * p.U, p.lcol(IM_COL32(0x10, 0x7c, 0x10, 255)), 24);
    p.icon(kind_icon(m), b, 17, p.lcol(WHITE));
    std::string when = m.age >= 0 ? brief(m.age) : "";
    p.text(p.L(x + 111, cy - 18), 25, p.lcol(WHITE), m.from.tag, true, -1, 380 - p.width(when, 23));
    p.text(p.L(x + 111, cy + 18), 23, p.lcol(m.unread ? WHITE : GREY1), summary(m), false, -1, 390);
    if (!when.empty()) p.text(p.L(x + 500, cy - 18), 23, p.lcol(GREY1), when, false, 1);
}

// The guide's Social and Messages tabs. Social: the chosen list's name with its count, its three lists' names under it, then the
// players two lines high. Messages: its name with how many are new, then the messages on the console's notification cards (six
// fit). The focus glides over either.
void Guide::draw_community_one(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float op, double now) {
    using namespace one;
    std::vector<FriendRow> friends = friend_rows();
    std::vector<MessageItem> msgs = messages();
    std::vector<const Friend*> players = player_rows(), party = party_rows();
    int online = 0, unread = 0;
    for (const Friend& f : social_.friends) online += f.state != Friend::Offline;
    for (const MessageItem& m : msgs) unread += m.unread;
    char title[48];
    if (comm_tab_ == TabParty) snprintf(title, sizeof title, "Party  -  %d", int(party.size()));
    else if (comm_tab_ == TabFriends) snprintf(title, sizeof title, "Friends  -  %d online", online);
    else if (comm_tab_ == TabMessages) snprintf(title, sizeof title, "Messages  -  %d new", unread);
    else snprintf(title, sizeof title, "Players  -  %d", int(players.size()));
    Panel p = one_frame(dl, a, origin, s, op, now, title);
    const bool social = comm_tab_ != TabMessages;
    static const char* const NAMES[3] = {"Party", "Friends", "Players"};
    if (social) p.segments(NAMES, 3, comm_tab_ == TabParty ? 0 : comm_tab_ == TabFriends ? 1 : 2);
    float ce = ease_guide(std::clamp(float((now - comm_at_) / 0.26), 0.0f, 1.0f));
    p.cop *= ce, p.cdx += (1 - ce) * 40 * comm_dir_;
    const float top = PY + LIST + (social ? SEG_H : 0);
    auto empty = [&](const char* why) { p.text(p.L(PX + 32, top + 40), 23, p.lcol(GREY1), why); };
    dl->PushClipRect(p.P(PX, top - 6), p.P(PX + PW, PY + PH - 6), true);
    switch (comm_tab_) {
    case TabFriends: {
        if (!social_.session) { empty("Not signed in"); break; }
        if (!social_.loaded && !social_.error.empty()) { empty("Server unavailable"); break; }
        int n = 1 + int(friends.size());
        p.focus(top + friends_.bar.value(now) * TWO_H, TWO_H);
        for (int i = friends_.top; i < std::min(n, friends_.top + FRIEND_ROWS); i++) {
            float y = top + (i - friends_.top) * TWO_H;
            if (i == 0) {
                // Add Friend, the list's first entry
                ImVec2 c = p.L(PX + 60, y + TWO_H / 2);
                dl->AddCircleFilled(c, 34 * p.U, p.lcol(CARD1), 48);
                p.icon(I_PERSON_ADD, c, 36, p.lcol(WHITE));
                p.text(p.L(PX + 112, y + TWO_H / 2), 26, p.lcol(WHITE), "Add friend");
                continue;
            }
            one_player(p, y, *friends[i - 1].f, friends[i - 1].rel, false);
        }
        p.more(friends_.top > 0, friends_.top + FRIEND_ROWS < n);
        break;
    }
    case TabMessages: {
        int n = 1 + int(msgs.size());
        // the console's notification cards: their fills first, the focus over the one it stands on, then what they say
        const int last = std::min(n, messages_.top + msg_rows());
        for (int i = messages_.top; i < last; i++) p.card(top + (i - messages_.top) * NOTE_P, NOTE_H);
        p.focus(top + messages_.bar.value(now) * NOTE_P, NOTE_H, PX + 9, 522);
        for (int i = messages_.top; i < last; i++) {
            float y = top + (i - messages_.top) * NOTE_P;
            if (i > 0) {
                one_note(p, y, msgs[i - 1]);
                continue;
            }
            ImVec2 c = p.L(PX + 68, y + NOTE_H / 2);
            dl->AddCircleFilled(c, 34 * p.U, p.lcol(IM_COL32(0x45, 0x45, 0x45, 255)), 48);
            p.icon(I_EDIT, c, 34, p.lcol(WHITE));
            p.text(p.L(PX + 120, y + NOTE_H / 2), 26, p.lcol(WHITE), "Create new");
        }
        p.more(messages_.top > 0, last < n, PY + PH - 14);  // under the sixth card
        break;
    }
    case TabParty: {
        int n = int(party.size());
        if (!n) {
            // not in a party: the console's one entry (a party is started by inviting friends to a private game, so it is greyed here)
            p.line(top, I_GROUPS, "Start a party", "", false);
            break;
        }
        p.focus(top + party_.bar.value(now) * TWO_H, TWO_H);
        for (int i = party_.top; i < std::min(n, party_.top + FRIEND_ROWS); i++) {
            float y = top + (i - party_.top) * TWO_H;
            Friend f = *party[i];
            if (f.tag.empty()) f.tag = gamertag;
            bool mine = f.xuid == social_.me.xuid;
            one_player(p, y, f, mine ? Relation::Friend : relation(f.xuid), false);
            float ex = PX + 490;
            if (std::find(talking_.begin(), talking_.end(), mine ? 0 : f.xuid) != talking_.end()) {
                ImVec2 mp = p.L(ex, y + TWO_H / 2);
                p.icon(I_MIC, mp, 30, p.lcol(WHITE));
                ex -= 40;
            }
            if (social_.me.lobby_leader && f.xuid == social_.me.lobby_leader) {
                ImVec2 sp = p.L(ex, y + TWO_H / 2);
                p.icon(I_STAR, sp, 30, p.lcol(WHITE));  // the party's leader
            }
        }
        p.more(party_.top > 0, party_.top + FRIEND_ROWS < n);
        break;
    }
    default: {
        int n = int(players.size());
        if (!n) { empty("No recent players"); break; }
        p.focus(top + players_.bar.value(now) * TWO_H, TWO_H);
        for (int i = players_.top; i < std::min(n, players_.top + FRIEND_ROWS); i++) {
            float y = top + (i - players_.top) * TWO_H;
            const Friend& f = *players[i];
            one_player(p, y, f, relation(f.xuid), false);
            if (std::find(talking_.begin(), talking_.end(), f.xuid) != talking_.end()) {
                ImVec2 mp = p.L(PX + 490, y + TWO_H / 2);
                p.icon(I_MIC, mp, 30, p.lcol(WHITE));  // heard right now
            }
        }
        p.more(players_.top > 0, players_.top + FRIEND_ROWS < n);
        break;
    }
    }
    dl->PopClipRect();
    const Hint lb = {"LB", nullptr, MUTED3}, rb = {"RB", "Switch", MUTED3};
    if (comm_tab_ == TabMessages) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"X", "Delete", RING_X}, {"Y", "Create new", RING_Y}, lb, rb};
        p.hints(hints, 6);
    } else if (comm_tab_ == TabFriends || (comm_tab_ == TabPlayers && !social_.players.empty()) || (comm_tab_ == TabParty && !party.empty())) {
        const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, lb, rb};
        p.hints(hints, 4);
    } else {
        const Hint hints[] = {{"B", "Back", RING_B}, lb, rb};
        p.hints(hints, 3);
    }
}

// ---- a player's profile and Edit Profile in the Xbox One layout (one.h)

// The player's gamertag as the page's name, the two tabs' names, then their card (picture, level and club, Gamerscore, what they are
// doing, where they are), the tab's options one line each, and on Personal Profile the four texts under them.
void Guide::draw_profile_one(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float op, double now) {
    using namespace one;
    const Friend* live = find(prof_.xuid);
    const Friend& f = live ? *live : prof_;
    const bool me = f.xuid && f.xuid == social_.me.xuid;
    Panel p = one_frame(dl, a, origin, s, op, now, f.tag.empty() ? "Profile" : f.tag);
    static const char* const NAMES[2] = {"Gamer profile", "Personal profile"};
    p.segments(NAMES, 2, prof_tab_);
    const float top = PY + LIST + SEG_H;
    if (!f.tag.empty()) {
        p.avatar(me ? a.gamerpic : a.pic(f.xuid), f.xuid, f.tag, p.L(PX + 66, top + 86), 30.5f, p.lcol(WHITE));
        std::string first = f.level > 0 ? "Level " + std::to_string(f.level) : "";
        if (!f.club.empty()) first += (first.empty() ? "" : "  -  ") + f.club;
        // Gamerscore, as on the console's card: this player's from the Achievements mod's file, another player's from the server
        int score = me && ach_.known ? ach_.gamerscore() : f.gamerscore;
        bool on = f.state != Friend::Offline || me;
        std::string doing = f.state == Friend::Playing ? "Forza Horizon 2" : on ? "Online" : f.last_seen >= 0 ? ago(f.last_seen, "Last online ") : "Offline";
        if (f.state == Friend::Playing) {
            std::string where = !me && same_game(f, social_.me) ? "In Your Game" : lobby_words(f);
            if (!where.empty()) doing += "  -  " + where;
        }
        std::string waiting = me ? "" : waiting_words(f, relation(f.xuid));
        // the console's card: a line of words, then the picture (61 across) with words beside it
        p.text(p.L(PX + 32, top + 30), 26, p.lcol(on ? WHITE : GREY1), doing, false, -1, PW - 64);
        std::string gs = score >= 0 ? "Gamerscore " + std::to_string(score) : "";
        if (first.empty()) first.swap(gs);
        p.text(p.L(PX + 111, top + 86 - (gs.empty() ? 0 : 16)), 26, p.lcol(WHITE), first, false, -1, 400);
        if (!gs.empty()) p.text(p.L(PX + 111, top + 104), 23, p.lcol(GREY1), gs);
        std::string under = f.location;
        if (!waiting.empty()) under += (under.empty() ? "" : "  -  ") + waiting;
        if (!under.empty()) p.text(p.L(PX + 32, top + 148), 23, p.lcol(waiting.empty() ? GREY1 : blend(ACCENT, WHITE, 0.4f)), under, false, -1, PW - 64);
    }
    p.divider(top + 174);

    // the options (they come in when the tab changes)
    float te = ease_guide(std::clamp(float((now - prof_at_) / 0.26), 0.0f, 1.0f));
    p.cop *= te, p.cdx += (1 - te) * 40 * prof_dir_;
    std::vector<Opt> opts = profile_options(prof_tab_);
    const int row = prof_row_[prof_tab_];
    const float oy = top + 188;
    if (row >= 0 && row < int(opts.size()) && opts[row].enabled) p.focus(oy + prof_bar_.value(now) * LINE_H, LINE_H);
    for (size_t i = 0; i < opts.size(); i++) p.line(oy + i * LINE_H, 0, opts[i].label, "", opts[i].enabled);
    if (prof_tab_ == 1 && !f.tag.empty()) {
        // the four texts: a small name over each; the console's words when there is none
        const std::string* vals[4] = {&f.motto, &f.name, &f.location, &f.bio};
        const char* none[4] = {"(No motto)", "(No name)", "(No location)", "(No bio)"};
        float y = oy + opts.size() * LINE_H + 14;
        p.divider(y);
        y += 22;
        dl->PushClipRect(p.P(PX, y - 8), p.P(PX + PW, PY + PH - 14), true);
        for (int i = 0; i < 4; i++) {
            p.text(p.L(PX + 32, y + 10), 19, p.lcol(GREY1), FIELD_NAME[i]);
            y += 24;
            bool has = !vals[i]->empty();
            y += p.para(PX + 32, y, 23, p.lcol(has ? WHITE : GREY1), has ? *vals[i] : none[i], PW - 64) + 10;
        }
        dl->PopClipRect();
    }
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}, {"LB", nullptr, MUTED3}, {"RB", "Switch", MUTED3}};
    p.hints(hints, 4);
}

// The four texts, two lines each: the text's name over what it says now.
void Guide::draw_edit_one(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float op, double now) {
    using namespace one;
    Panel p = one_frame(dl, a, origin, s, op, now, "Edit profile");
    const Friend& me = social_.me;
    const std::string* vals[4] = {&me.motto, &me.name, &me.location, &me.bio};
    const char* none[4] = {"(No motto)", "(No name)", "(No location)", "(No bio)"};
    const float top = PY + LIST;
    if (edit_row_ >= 0) p.focus(top + edit_bar_.value(now) * TWO_H, TWO_H);
    for (int i = 0; i < 4; i++) p.two(top + i * TWO_H, PX + 32, FIELD_NAME[i], vals[i]->empty() ? none[i] : *vals[i], true, PW - 64);
    const Hint hints[] = {{"A", "Select", RING_A}, {"B", "Back", RING_B}};
    p.hints(hints, 2);
}
