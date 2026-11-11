// The Guide's link to the FH2MP server: the same /v1/community API the launcher uses, with the session the launcher (or a rig's
// dev_client.py) wrote for the game: data/bin/fh2mp-session.txt (xuid, token) and data/bin/fh2mp-lsp.txt (server URL). One worker thread
// does all requests; the render thread only takes copies and results.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Friend {
    enum State { Offline, Online, Playing };
    uint64_t xuid = 0;
    std::string tag, club, lobby_id, lobby_kind, lobby_status;
    int level = 0;
    State state = Offline;
    double pic_version = 0;
};

struct ReceivedInvite {
    int64_t id = 0;
    Friend from;
    std::string text, kind;
    int age = 0, expires_in = 0;  // seconds, when the list was read
};

// A conversation in the launcher's chat with one friend: its last line and how many are unread.
struct ChatThread {
    std::string id, title, last;  // id: the friend's xuid in decimal (or "club")
    int unread = 0;
    int64_t last_incoming = 0;    // id of the last message the friend sent, 0 when they never wrote
};

struct Social {
    bool session = false;  // a session file was found
    bool loaded = false;   // the server answered at least once
    std::string error;     // why the last refresh failed, empty when it worked
    Friend me;
    std::vector<Friend> friends;
    std::vector<ReceivedInvite> invites;
    std::vector<Friend> requests;  // friend requests waiting for this player
    std::vector<ChatThread> chats;
    uint64_t version = 0;  // goes up with every change
};

enum class NetOp { Invite, Accept, Decline, Received, AcceptFriend, DeclineFriend, ReadChat };

// What came of an action, or an invite that just arrived (Received).
struct NetResult {
    NetOp op;
    bool ok = false;
    std::string message;  // the server's words when it refused, or ours when it could not be reached
    uint64_t xuid = 0;    // the other player
    std::string name;
    int64_t id = 0;
};

// A player's picture (/v1/gamerpic/<xuid>.png) decoded to RGBA, for the render thread to upload.
struct PicReady {
    uint64_t xuid = 0;
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
};

void net_start(const std::wstring& azimuth_dir, void (*log)(const char*));
Social net_social();
void net_guide_open(bool open);  // refreshes at once on open, and every 5 s while open (every 30 s and invites every 10 s while closed)
void net_invite(uint64_t xuid, const std::string& name);
void net_accept(int64_t id, uint64_t from, const std::string& name);
void net_decline(int64_t id, uint64_t from, const std::string& name);
void net_accept_friend(uint64_t xuid, const std::string& name);
void net_decline_friend(uint64_t xuid, const std::string& name);
void net_read_chat(const std::string& chat, int64_t last_id);
bool net_take_result(NetResult* out);
bool net_take_pic(PicReady* out);
