// The in-game Guide (FH2MPGuide.dll): shared declarations between the overlay (overlay.cpp: hooks, Direct3D 12, input) and the Guide
// itself (guide.cpp, community.cpp, messages.cpp). The look is the design canvas https://claude.ai/artifact/GYsZgmPY8hSP7A4iGQaAyH; what
// each screen offers, in which order and when, is the Xbox 360's (the spec: Notion "Xbox 360 Live UX spec for the Guide").
#pragma once

#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "net.h"

enum class PadEvent { Toggle, Up, Down, Left, Right, A, B, X, Y, Lb, Rb, Lt, Rt, L3, Start, OpenInvite };

// XInput button bits (0x0400 is the Guide button, reported by XInputGetStateEx only).
namespace padbits {
constexpr uint16_t Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008, Start = 0x0010, L3 = 0x0040, Lb = 0x0100, Rb = 0x0200,
                   Guide = 0x0400, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
}

// Readings (buttons, triggers, left stick, time) into events: the Guide button held for GUIDE_HOLD_MS (half a second) toggles, a short
// press closes an open Guide, navigation only while open, a held direction repeats after 400 ms then every 80 ms.
struct PadReader {
    static constexpr uint64_t GUIDE_HOLD_MS = 500, REPEAT_DELAY_MS = 400, REPEAT_EVERY_MS = 80;
    uint16_t prev = 0;
    bool prev_lt = false, prev_rt = false;
    bool guide_down = false, guide_fired = false;
    uint64_t guide_since = 0;
    int dir = -1;  // PadEvent of the held direction, or -1
    uint64_t next_at = 0;
    // Appends the events of one reading to out (at most 16); returns how many. lt, rt: the triggers, 0..255.
    int feed(uint16_t buttons, uint8_t lt, uint8_t rt, int16_t lx, int16_t ly, uint64_t now_ms, bool open, PadEvent* out);
};

// A font's hhea ascender and descender in em. The fonts are loaded so that a size of N draws N-pixel em glyphs, as CSS's font-size does
// (Dear ImGui by default makes ascender minus descender N pixels, which drew Segoe UI a quarter smaller than the canvas).
struct FontMetrics {
    float asc = 0.8f, desc = 0.2f;
    float cap = 0.7f;  // capital letter height (OS/2 sCapHeight), for centring a word on an icon by its letters
};

// Fonts and textures the Guide draws with (made by the overlay once Direct3D is ready).
struct GuideAssets {
    ImFont* text = nullptr;       // Segoe UI, or ImGui's own font when Windows has none
    ImFont* text_bold = nullptr;  // Segoe UI Bold (toast titles), or the same as text
    ImFont* icons = nullptr;      // Material Icons (filled)
    FontMetrics text_m, bold_m, icons_m;
    ImTextureID gamerpic = 0;     // the player's picture (Azimuth's data/player.png), 0 when there is none
    std::unordered_map<uint64_t, ImTextureID> pics;  // other players' pictures by xuid, as they arrive from the server

    ImTextureID pic(uint64_t xuid) const {
        auto it = pics.find(xuid);
        return it == pics.end() ? 0 : it->second;
    }
};

// Loads Segoe UI, Segoe UI Bold and the embedded Material Icons into ImGui's atlas with CSS sizing (assets.cpp). Needs an ImGui context.
void load_fonts(GuideAssets& a);

// The canvas' toast kinds (artboard 7): the square's colour, the title's colour, and whether a player's picture glides in (invites only).
enum class ToastKind { Invite, Info, Saving, Warning };

enum class GuideAction {
    None, Resume, Invite, AcceptInvite, DeclineInvite, FriendRequest, AcceptFriend, DeclineFriend, RemoveFriend, SendText, ReadChat,
    HideText, Join, SetProfile, JoinGame
};

// What the overlay must ask of the server after an event.
struct GuideRequest {
    GuideAction action = GuideAction::None;
    uint64_t xuid = 0;
    int64_t id = 0;
    std::string name;  // the other player (the chat's id for ReadChat; the gamertag asked for, for FriendRequest; the field for SetProfile)
    std::string text;  // Invite, SendText, SetProfile: the words; Join: the lobby's id
    std::string kind;  // Join: the lobby's kind; JoinGame: the session's template
    std::string scid;  // JoinGame: the session's service configuration (text holds the session's name, name whose game it is)
};

// One entry of the Messages tab: a game invite, a friend request, or a message a friend sent.
struct MessageItem {
    enum Kind { GameInvite, FriendRequest, Text } kind = Text;
    Friend from;
    int64_t id = 0;  // the invite's id, or the message's id
    std::string text, chat;
    int age = -1, expires_in = -1;  // seconds when known
    bool unread = false;
};

// What a player is to this one; it picks the option list of their profile (the 360's four lists).
enum class Relation { Friend, Stranger, RequestReceived, RequestSent };

// One entry of an option list. Entries that do not apply are not in the list; one that cannot work right now is in it, greyed, and the
// focus passes over it (the 360's rule).
struct Opt {
    std::string label;
    int cmd = 0;
    bool enabled = true;
    uint32_t icon = 0;
};

enum class Page { Home, Community, Profile, EditProfile, Message, CreateNew, Recipients, Compose, Keyboard };
enum CommunityTab { TabParty = 0, TabFriends = 1, TabMessages = 2, TabPlayers = 3 };
// What a message box was shown for: just to tell (OK), or the question before a written message is dropped.
// BoxThenClose: told from a closed Guide (the game's own Invite Friends), so OK closes it again.
// BoxExitSession: the 360's question before a join takes the player out of the game they are in.
enum BoxTag { BoxInfo = 0, BoxDiscard = 1, BoxThenClose = 2, BoxExitSession = 3 };

class Guide {
public:
    bool open = false;
    std::string gamertag = "FH2MP";
    // The game itself joins a session it is handed (join.cpp found the game's code): an accepted invite then needs no words. When false
    // the seat is held and a message box tells the player to go online by hand.
    bool game_joins = false;

    void toggle(double now);
    // The game's own Invite Friends: the Guide opens on a game invite's recipients; sending it closes the Guide again.
    void open_invite(double now);
    // Opens straight on Community's tab (the preview tool).
    void open_community(int tab, double now);
    // One pad or key event while open; what the overlay must ask of the server.
    std::vector<GuideRequest> handle(PadEvent e, double now);
    // The keyboard screen is up: typed characters go to type_char ('\b' backspace, '\r' done).
    bool typing() const { return open && page() == Page::Keyboard && !box_.on; }
    std::vector<GuideRequest> type_char(uint32_t cp, double now);
    // The friend list, invites and messages as the server last gave them.
    void set_social(const Social& s);
    // What came of a request, or what arrived: the 360's toasts and message boxes.
    void on_result(const NetResult& r, const GuideAssets& a, double now);
    // What on_result wants done (JoinGame), for the overlay to take after it.
    std::vector<GuideRequest> take_requests();
    // Draws into the game's frame (w x h pixels). Returns true while something is on screen (the Guide or a toast).
    bool draw(ImDrawList* dl, const GuideAssets& a, float w, float h, double now);
    // A 360 toast at the bottom centre (7.5 s cycle of the canvas, played once). pic: the other player's picture on an invite toast (it
    // glides in over the icon at 2.0 s), 0 for none.
    void toast(ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic, double now);
    bool busy(double now) const;

private:
    struct Tween {
        float from = 0, to = 0;
        double start = -10;
        float secs = 0;
        void go(float v, double now, float s);
        void set(float v) { from = to = v, secs = 0; }
        float value(double now) const;
    };
    // A scrolling list's focus: the row, the first row on screen, and the bar gliding to the row.
    struct ListFocus {
        int row = 0, top = 0;
        Tween bar;
        void clamp(int n, int visible);
        void move(int dy, int n, int visible, double now);
    };
    // The 360's message box: heading, text, buttons one under another. tag says what asked.
    struct Box {
        bool on = false;
        std::string title, body;
        std::vector<std::string> buttons;
        int focus = 0, tag = 0;
        Tween bar;
        double at = -10;
    };
    // A message, game invite or friend request being written (the 360's New Message screen).
    struct Draft {
        enum Kind { Message, GameInvite, FriendRequest } kind = Message;
        std::vector<Friend> to;  // Message, GameInvite
        std::string gamertag;    // FriendRequest: the gamertag asked for
        uint64_t xuid = 0;       // FriendRequest from a profile: the player
        std::string text;
        bool sending = false;
        int pending = 0, failed = 0;
        std::string fail_code;
    };
    // The on-screen keyboard (artboard 6).
    struct Keys {
        enum Purpose { Text, Gamertag, Field } purpose = Text;
        int field = 0;  // Field: which profile text (0 motto, 1 name, 2 location, 3 bio)
        std::u32string text;
        size_t caret = 0;
        bool caps = false;
        int mode = 0;  // 0 letters, 1 symbols, 2 accents
        int focus = 1;
        size_t max = 120;
    };
    struct Key {
        std::string label, badge;
        uint32_t icon = 0, ch = 0;
        int col = 0, row = 0, cols = 1, rows = 1;  // grid cell (12 columns, 5 rows) and span
        int act = 0;                                // 0 a character, else KeyAct
        bool lit = false, side = false, wide = false;
        ImU32 badge_col = 0;
        float badge_w = 20, badge_r = 999, size = 22;
    };

    // home
    int blade_ = 1;
    int focus_[3] = {0, 1, -1};
    double opened_ = -10, closed_ = -10, switched_ = -10;
    int switch_dir_ = 1;
    Tween bar_[3], width_[3];
    bool widths_set_ = false;
    // pages
    std::vector<Page> stack_ = {Page::Home};
    double page_at_ = -10;
    int page_dir_ = 1;
    int flow_base_ = -1;  // the stack's depth a compose flow returns to when it is sent or dropped; 0 closes the Guide; -1 no flow
    // community
    int comm_tab_ = TabFriends, comm_dir_ = 1;
    double comm_at_ = -10;
    ListFocus friends_, messages_;
    // profile
    Friend prof_;
    int prof_tab_ = 0, prof_dir_ = 1;
    double prof_at_ = -10;
    int prof_row_[2] = {0, 0};
    Tween prof_bar_;
    uint64_t waiting_xuid_ = 0;  // an option of this profile is on its way to the server
    int edit_row_ = 0;           // Edit Profile's focus
    Tween edit_bar_;
    // one message
    MessageItem open_msg_;
    int msg_opt_ = 0;
    Tween opt_bar_;
    // create new, recipients, compose, keyboard
    int new_row_ = 0;
    Tween new_bar_;
    bool pick_many_ = false;
    ListFocus pick_;
    Draft draft_;
    int draft_row_ = 0;
    Tween draft_bar_;
    Keys keys_;
    // the message box (shown over the page; one that arrives while the Guide is closed waits for the next opening)
    Box box_, box_waiting_;
    // a join held back by the Exit Session question: the invite to accept, or the player whose game to join
    int held_ = 0;  // 0 none, 1 an invite, 2 a player's game
    MessageItem held_msg_;
    Friend held_player_;
    std::vector<GuideRequest> wanted_;
    std::set<uint64_t> sent_;
    Social social_;
    // toast: the one playing and the ones waiting for it
    struct ToastItem {
        ToastKind kind = ToastKind::Info;
        std::string title, detail;
        uint32_t icon = 0;
        ImTextureID pic = 0;
    };
    ToastItem toast_;
    std::deque<ToastItem> toasts_;
    double toast_at_ = -100;

    Page page() const { return stack_.back(); }
    void push(Page p, double now);
    void pop(double now);
    void pop_to(size_t depth, double now);
    void entered(Page p, double now);
    void switch_to(int b, double now);
    void fix_home_focus();
    void show_box(int tag, const char* title, const std::string& body, std::vector<std::string> buttons, double now);
    void box_done(int tag, int button, std::vector<GuideRequest>& out, double now);

    // who is who
    Relation relation(uint64_t xuid) const;
    const Friend* find(uint64_t xuid) const;
    bool my_game_takes_invites() const;
    bool can_invite(const Friend& f) const;
    bool can_join(const Friend& f) const;
    std::string waiting_words(const Friend& f, Relation rel) const;
    struct FriendRow {
        const Friend* f;
        Relation rel;
    };
    std::vector<FriendRow> friend_rows() const;        // the Friends tab under its Add Friend row
    std::vector<const Friend*> pick_rows() const;      // the recipients list
    std::vector<MessageItem> messages() const;
    std::vector<Opt> home_rows(int blade) const;
    std::vector<Opt> profile_options(int tab) const;
    std::vector<Opt> message_options(const MessageItem& m) const;
    std::vector<Opt> create_options() const;
    std::vector<Opt> draft_options() const;
    std::vector<Key> keyboard() const;

    void start_draft(Draft::Kind kind, double now);
    void open_keyboard(Keys::Purpose purpose, const std::string& text, double now);
    void keyboard_done(std::vector<GuideRequest>& out, double now);
    void press_key(const Key& k, std::vector<GuideRequest>& out, double now);
    std::vector<Opt> edit_options() const;
    void handle_edit(PadEvent e, std::vector<GuideRequest>& out, double now);
    void draw_edit(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    // The right half of a profile screen (artboards 8 and 9): the player's card, and under it the gamer or the personal pane.
    void profile_side(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float top, const Friend& p, bool personal,
                      float pop, float fade) const;
    void send_draft(std::vector<GuideRequest>& out, double now);
    void end_flow(double now);
    void delete_message(const MessageItem& m, std::vector<GuideRequest>& out, double now);
    bool ask_exit_session(double now);
    void accept_invite(const MessageItem& m, std::vector<GuideRequest>& out, double now);
    void join_player(const Friend& p, std::vector<GuideRequest>& out, double now);

    void handle_home(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_community(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_profile(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_message(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_create(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_pick(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_compose(PadEvent e, std::vector<GuideRequest>& out, double now);
    void handle_keyboard(PadEvent e, std::vector<GuideRequest>& out, double now);

    void draw_home(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_community(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_profile(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_message(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_create(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_pick(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_compose(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_keyboard(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_box(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float w, float h, double now);
    void draw_header(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, float left, float width, const char* title,
                     float title_op);
    // One player's row of a list (artboard 4): picture, name, state and what they are doing. check: -1 none, 0 an empty box, 1 ticked.
    void player_row(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float y, const Friend& f, Relation rel,
                    bool focused, bool greyed, int check, float op) const;
    void draw_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now);
    // The slide and fade of the page that just came in: x offset (canvas units) and opacity; title_op for the header's title.
    void page_motion(double now, float op, float* slide, float* pop, float* title_op) const;
};

// Cubic-bezier easing as CSS defines it (x1, y1, x2, y2), for t in 0..1.
float css_bezier(float x1, float y1, float x2, float y2, float t);
