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

#include "achievements.h"
#include "imgui.h"
#include "net.h"
#include "theme.h"

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
    bool was_open = false, dir_blocked = false;
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

// A picture that holds an animation's frames in a grid, row by row.
struct SpriteSheet {
    ImTextureID tex = 0;
    int cols = 0, rows = 0, frames = 0;
};

enum class AchTier { Normal, Rare, Mythic };

// The Xbox app's art for achievement toasts, read from the player's own install of that app when there is one (achievements.cpp,
// ach_art_set; a texture of 0 is not there and the toast is drawn without it). By tier: the trophy, the rare diamond, the mythic star.
struct AchToastArt {
    SpriteSheet intro[3], idle[3], outro[3];
    SpriteSheet intro_hi, idle_hi, outro_hi;  // the mythic star's highlight layer
    SpriteSheet aurora;                       // the haze around the mythic toast
    ImTextureID gleam = 0, sparkle = 0, ripple = 0, glow_a = 0, glow_b = 0, pulse = 0;
};
// Puts a loaded picture where it belongs, by its file name; and the file names: where 0 = the Xbox app's SocialPublicAssets\Notifications,
// 1 = Game Bar's Assets (null ended).
void ach_art_set(AchToastArt* art, const char* file, ImTextureID tex);
const char* const* ach_art_files(int where);
// How long an achievement toast of a tier is on screen.
float ach_toast_seconds(AchTier tier, int style = 0);  // style: 0 the Guide's toast, 1 the Xbox 360's notification, 2 the Xbox One's toast

// Fonts and textures the Guide draws with (made by the overlay once Direct3D is ready).
struct GuideAssets {
    ImFont* text = nullptr;       // Segoe UI, or ImGui's own font when Windows has none
    ImFont* text_bold = nullptr;  // Segoe UI Bold (toast titles), or the same as text
    ImFont* text_one = nullptr;   // the Xbox One toast's words: Game Bar's Segoe Sans Text where it is installed, else Segoe UI Semibold
    ImFont* icons = nullptr;      // Material Icons (filled)
    FontMetrics text_m, bold_m, one_m, icons_m;
    ImTextureID gamerpic = 0;     // the player's picture (Azimuth's data/player.png), 0 when there is none
    std::unordered_map<uint64_t, ImTextureID> pics;  // other players' pictures by xuid, as they arrive from the server
    AchToastArt ach;              // the achievement toast's art, when the player has the Xbox app

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
    HideText, Join, SetProfile, JoinGame, Lookup, Achievements, SetTheme, SetVoice, SetNotify
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

enum class Page { Home, Community, Profile, EditProfile, Message, CreateNew, Recipients, Compose, Keyboard, Achievements, Compare, AchDetail, Theme, Voice, Notifications };
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
    // The game's own Invite Friends: a game invite's recipients. The game offers it only in a session of its own, so nothing is refused
    // here from the Guide's picture of the player's game, which can be half a minute old (win1 2026-11-11: refused twice right after a
    // private Free Roam started); the server refuses at Send when nobody can be seated.
    void open_invite(double now);
    // Opens straight on Community's tab (the preview tool).
    void open_community(int tab, double now);
    // The game asked the Xbox system UI for a player's gamercard: the Guide opens on that player's profile alone (B closes the Guide).
    void open_profile_for_game(uint64_t xuid, double now);
    // The game asked for the system keyboard: the Guide opens on its keyboard alone, with the game's title, description and text.
    void open_keyboard_for_game(const std::string& text, const std::string& title, const std::string& description, double now);
    // After that keyboard closed: true and the text when the player chose Done, false when they backed out.
    bool take_game_text(std::string* text);
    // The game asked for the system people picker: the Guide opens on its player list alone, with the game's prompt and the players the
    // game named. max 1: A chooses the player; more: A ticks, X finishes once at least min (and at least one) are ticked.
    void open_people_for_game(const std::string& prompt, const std::vector<uint64_t>& people, const std::vector<uint64_t>& chosen, unsigned min,
                              unsigned max, double now);
    // After that list closed: true and the chosen players when the player finished, false when they backed out.
    bool take_game_people(std::vector<uint64_t>* chosen);
    // One pad or key event while open; what the overlay must ask of the server.
    std::vector<GuideRequest> handle(PadEvent e, double now);
    // The keyboard screen is up: typed characters go to type_char ('\b' backspace, '\r' done).
    bool typing() const { return open && page() == Page::Keyboard && !box_.on; }
    std::vector<GuideRequest> type_char(uint32_t cp, double now);
    // The friend list, invites and messages as the server last gave them.
    void set_social(const Social& s);
    // Settings > Voice as the overlay keeps it (voice_page.cpp): microphone, voice chat, volume 0..4. A change comes back as SetVoice
    // (id: bit 0 mic, bit 1 chat, the volume from bit 2).
    void set_voice(bool mic, bool chat, int volume) { voice_mic_ = mic, voice_chat_ = chat, voice_volume_ = volume; }
    void open_voice(double now);
    // Settings > Notifications as the overlay keeps it (notify_page.cpp): how an achievement is shown, 0 the Guide's toast, 1 the
    // Xbox 360's notification, 2 the Xbox One's toast. A change comes back as SetNotify (id: the style).
    void set_notify(int style) { ach_style_ = style == 1 || style == 2 ? style : 0; }
    int ach_style() const { return ach_style_; }
    void open_notify(double now);
    // Settings > Theme, opened on the theme in use (theme_page.cpp).
    void open_theme(double now);
    // Who is talking right now (the voice engine's list: xuids, 0 for this player): the Players tab marks them.
    void set_talking(const uint64_t* ids, int n) { talking_.assign(ids, ids + n); }
    // This player's unlocked achievements as the Achievements mod's file has them (not known: the Achievements row is greyed).
    void set_achievements(const AchUnlocks& u);
    // An achievement was unlocked: its toast (they queue). gamerscore -1: none to show (the Mythic Achievement, whose name is the game's);
    // percent -1: not known (a rare one's share of players); index: its place in the list, for opening the Guide on it (-1: none).
    void ach_toast(AchTier tier, int gamerscore, const std::string& name, const std::string& desc, float percent, int index, double now);
    // A toast started just now: the sound that goes with it, once.
    bool take_ach_sound(AchTier* tier);
    bool ach_toast_showing(double now) const;
    // What came of a request, or what arrived: the 360's toasts and message boxes.
    void on_result(const NetResult& r, const GuideAssets& a, double now);
    // What on_result wants done (JoinGame), for the overlay to take after it.
    std::vector<GuideRequest> take_requests();
    // Draws into the game's frame (w x h pixels). Returns true while something is on screen (the Guide or a toast).
    bool draw(ImDrawList* dl, const GuideAssets& a, float w, float h, double now);
    // A 360 toast at the bottom centre (7.5 s cycle of the canvas, played once). pic: the other player's picture on an invite toast (it
    // glides in over the icon at 2.0 s), 0 for none.
    void toast(ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic, double now);
    // The same, for something the player can go to: opening the Guide while the toast plays opens that item, as the 360 did (an
    // invite or a message: its Message screen; a friend who came online: their profile).
    enum ToastTarget { NoTarget, ToInvite, ToFriendRequest, ToText, ToPlayer };
    void toast_to(ToastTarget target, uint64_t xuid, int64_t id, ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic,
                  double now);
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
        bool by_game = false;    // GameInvite the game's own Invite Friends asked for
        bool sending = false;
        int pending = 0, failed = 0;
        std::string fail_code;
    };
    // The on-screen keyboard (artboard 6).
    struct Keys {
        enum Purpose { Text, Gamertag, Field, Game } purpose = Text;
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
    ListFocus friends_, messages_, players_;
    std::vector<uint64_t> talking_;  // who is heard right now (voice), by xuid; 0 = this player
    // profile
    Friend prof_;
    int prof_tab_ = 0, prof_dir_ = 1;
    double prof_at_ = -10;
    int prof_row_[2] = {0, 0};
    Tween prof_bar_;
    // Settings > Theme (theme_page.cpp): the tab (0 Themes, 1 Plastic), the focus in each, and the theme kept when the page opened
    int theme_tab_ = 0, theme_dir_ = 1, theme_row_[2] = {0, 0};
    double theme_at_ = -10;
    Tween theme_bar_;
    std::string theme_kept_;
    bool voice_mic_ = true, voice_chat_ = true;
    int voice_volume_ = 2, voice_row_ = 0;
    Tween voice_bar_;
    int ach_style_ = 0, notify_row_ = 0;
    Tween notify_bar_;
    ListFocus party_;
    void handle_voice(PadEvent e, std::vector<GuideRequest>& out, double now);
    void draw_voice(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void handle_notify(PadEvent e, std::vector<GuideRequest>& out, double now);
    void draw_notify(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    std::vector<const Friend*> party_rows() const;     // the Party tab: this player's private game, this player first; empty when in none
    void handle_theme(PadEvent e, std::vector<GuideRequest>& out, double now);
    void draw_theme(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
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
    // achievements: this player's, the focused tile and the first row of tiles on screen
    AchUnlocks ach_;
    int ach_focus_ = 0, ach_top_ = 0;
    // the achievement toast: the one showing, the ones waiting
    struct AchToast {
        AchTier tier = AchTier::Normal;
        int g = -1, index = -1;
        float percent = -1;
        std::string name, desc;
    };
    AchToast ach_now_;
    std::deque<AchToast> ach_toasts_;
    double ach_at_ = -100;
    float ach_lift_ = 0;
    int ach_sound_ = 0;  // 1 + the tier whose sound is to be played
    void toast_sound();
    // Compare Games: the other player, the list's focus, and the achievement whose details are open
    Friend cmp_;
    ListFocus cmp_list_;
    int ach_detail_ = 0;
    std::string game_title_, game_description_, game_text_;  // the game's keyboard request and its answer
    bool game_done_ = false;
    // the game's people picker request and its answer
    struct GamePick {
        bool on = false, done = false;
        std::string prompt;
        std::vector<uint64_t> people, chosen;
        unsigned min = 0, max = 0;
    };
    GamePick game_pick_;
    mutable std::vector<Friend> game_people_;  // the rows of that list (pick_rows)
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
        int target = 0;  // ToastTarget
        uint64_t xuid = 0;
        int64_t id = 0;
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
    void open_on_toast(double now);
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
    std::vector<const Friend*> player_rows() const;    // the Players tab: who is in this player's game, by gamertag
    std::vector<const Friend*> pick_rows() const;      // the recipients list
    void handle_game_pick(PadEvent e, double now);
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
    void handle_achievements(PadEvent e, double now);
    void open_compare(const Friend& them, std::vector<GuideRequest>& out, double now);
    void handle_compare(PadEvent e, double now);
    void handle_ach_detail(PadEvent e, double now);

    void draw_home(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_community(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_profile(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_message(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_create(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_pick(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_compose(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_keyboard(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_achievements(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_compare(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_ach_detail(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, double now);
    void draw_box(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float w, float h, double now);
    void draw_header(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float op, const char* title, float title_op,
                     bool half = false, float slide = 0);

    // One player's row of a list (artboard 4): picture, name, state and what they are doing. check: -1 none, 0 an empty box, 1 ticked.
    void player_row(ImDrawList* dl, const GuideAssets& a, ImVec2 origin, float s, float zoom, float x, float y, const Friend& f, Relation rel,
                    bool focused, bool greyed, int check, float op) const;
    void draw_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now);
    void draw_ach_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now);
    void draw_ach_toast_360(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, float t);
    void draw_ach_toast_one(ImDrawList* dl, const GuideAssets& g, float s, ImVec2 origin, float a);
    // The slide and fade of the page that just came in: x offset (canvas units) and opacity; title_op for the header's title.
    void page_motion(double now, float op, float* slide, float* pop, float* title_op) const;
};

// Cubic-bezier easing as CSS defines it (x1, y1, x2, y2), for t in 0..1.
float css_bezier(float x1, float y1, float x2, float y2, float t);
