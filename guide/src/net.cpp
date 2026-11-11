#define WIN32_LEAN_AND_MEAN
#include "net.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <set>

#include "gfx.h"
#include "json.h"

namespace {

std::wstring g_dir;
void (*g_log)(const char*);
std::mutex g_mu;
std::condition_variable g_cv;
Social g_social;
bool g_open, g_wake;
struct Op {
    NetOp op;
    uint64_t xuid;
    int64_t id;
    std::string name;  // the other player, or the chat id for ReadChat
    std::string text;  // Invite, SendText: the words; Join: the lobby's id
    std::string kind;  // Join: the lobby's kind
};
std::deque<Op> g_ops;
std::deque<NetResult> g_results;
std::deque<PicReady> g_pics;
std::set<int64_t> g_hidden;  // messages deleted in the Message Center (data/bin/fh2mp-guide-hidden.txt)
bool g_hidden_dirty;

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

std::string narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += (c > 0 && c < 0x80) ? char(c) : '?';
    return s;
}

bool read_file(const std::wstring& path, std::string* out, FILETIME* written) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out->assign(size < (1u << 20) ? size : 0, '\0');
    bool ok = ReadFile(f, &(*out)[0], DWORD(out->size()), &got, nullptr) != 0;
    out->resize(got);
    if (written) GetFileTime(f, nullptr, nullptr, written);
    CloseHandle(f);
    return ok;
}

// A JSON string literal of UTF-8 text.
std::string quoted(const std::string& t) {
    std::string o = "\"";
    for (unsigned char c : t) {
        if (c == '"' || c == '\\') o += '\\', o += char(c);
        else if (c == '\n') o += "\\n";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) o += ' ';
        else o += char(c);
    }
    return o + "\"";
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '/')) s.pop_back();
    size_t a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a);
}

// The session the launcher wrote for this game.
struct Conn {
    std::wstring host;
    INTERNET_PORT port = 0;
    bool https = false;
    std::string token, base;
    uint64_t xuid = 0;
    FILETIME written{};
};

bool load_conn(Conn* c, std::string* why) {
    std::wstring bin = g_dir + L"\\data\\bin\\";
    std::string session, url;
    FILETIME w{};
    if (!read_file(bin + L"fh2mp-session.txt", &session, &w)) return *why = "no session file (the game was not started with Play)", false;
    if (!read_file(bin + L"fh2mp-lsp.txt", &url, nullptr)) return *why = "no fh2mp-lsp.txt", false;
    Conn n;
    n.written = w;
    size_t at = 0;
    while (at < session.size()) {
        size_t end = session.find('\n', at);
        std::string line = trim(session.substr(at, end == std::string::npos ? std::string::npos : end - at));
        at = end == std::string::npos ? session.size() : end + 1;
        if (line.rfind("token=", 0) == 0) n.token = line.substr(6);
        if (line.rfind("xuid=", 0) == 0) n.xuid = strtoull(line.c_str() + 5, nullptr, 16);
    }
    url = trim(url);
    n.base = url;
    std::string rest;
    if (url.rfind("https://", 0) == 0) n.https = true, rest = url.substr(8);
    else if (url.rfind("http://", 0) == 0) rest = url.substr(7);
    else return *why = "fh2mp-lsp.txt holds no http(s) URL", false;
    std::string hostport = rest.substr(0, rest.find('/'));
    size_t colon = hostport.rfind(':');
    std::string host = colon == std::string::npos ? hostport : hostport.substr(0, colon);
    n.port = colon == std::string::npos ? (n.https ? 443 : 80) : INTERNET_PORT(atoi(hostport.c_str() + colon + 1));
    n.host.assign(host.begin(), host.end());
    if (n.token.empty() || n.host.empty()) return *why = "the session file has no token", false;
    *c = n;
    return true;
}

HINTERNET g_http;

// One request; the status (0 when the server could not be reached, with err set).
int request(const Conn& c, const wchar_t* method, const std::string& path, const std::string& body, std::string* out, std::string* err) {
    out->clear();
    if (!g_http) {
        g_http = WinHttpOpen(L"fh2mp-guide", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (g_http) WinHttpSetTimeouts(g_http, 5000, 5000, 6000, 8000);
    }
    if (!g_http) return *err = "WinHTTP is not available", 0;
    HINTERNET con = WinHttpConnect(g_http, c.host.c_str(), c.port, 0);
    if (!con) return *err = "could not open a connection (" + std::to_string(GetLastError()) + ")", 0;
    std::wstring wpath(path.begin(), path.end());
    HINTERNET req = WinHttpOpenRequest(con, method, wpath.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, c.https ? WINHTTP_FLAG_SECURE : 0);
    int status = 0;
    if (req) {
        std::wstring hdr = L"Authorization: Bearer " + std::wstring(c.token.begin(), c.token.end()) + L"\r\n";
        if (!body.empty()) hdr += L"Content-Type: application/json\r\n";
        if (WinHttpSendRequest(req, hdr.c_str(), DWORD(-1), (void*)(body.empty() ? nullptr : body.data()), DWORD(body.size()), DWORD(body.size()), 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
            status = int(code);
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!WinHttpQueryDataAvailable(req, &avail) || !avail || out->size() > (4u << 20)) break;
                size_t at = out->size();
                out->resize(at + avail);
                if (!WinHttpReadData(req, &(*out)[at], avail, &got)) break;
                out->resize(at + got);
            }
        } else {
            *err = "the server did not answer (" + std::to_string(GetLastError()) + ")";
        }
        WinHttpCloseHandle(req);
    } else {
        *err = "could not make the request (" + std::to_string(GetLastError()) + ")";
    }
    WinHttpCloseHandle(con);
    return status;
}

// The server's own words for a refusal, else the status. code: the server's error code when it gave one.
std::string refusal(int status, const std::string& body, const std::string& err, std::string* code = nullptr) {
    if (!status) return err;
    JVal v;
    if (json_parse(body, &v)) {
        std::string c = v["error"].str();
        if (code) *code = c;
        std::string m = v["message"].str();
        if (!m.empty()) return m;
        if (!c.empty()) return c + " (" + std::to_string(status) + ")";
    }
    if (status == 404) return "This FH2MP server does not have that yet";
    return "the server refused (" + std::to_string(status) + ")";
}

Friend card(const JVal& c) {
    Friend f;
    f.xuid = uint64_t(c["xuid"].i64());
    f.tag = c["gamertag"].str();
    f.level = int(c["level"].i64());
    f.pic_version = c["gamerpicVersion"].num();
    std::string st = c["state"].str();
    f.state = st == "playing" ? Friend::Playing : st == "online" ? Friend::Online : Friend::Offline;
    const JVal& club = c["club"];
    f.club = club.type == JVal::Obj ? (club["tag"].str().empty() ? club["name"].str() : club["tag"].str()) : club.str();
    const JVal& lobby = c["lobby"];
    if (lobby.type == JVal::Obj) {
        f.lobby_id = lobby["id"].str(), f.lobby_kind = lobby["kind"].str(), f.lobby_status = lobby["status"].str();
        f.lobby_players = int(lobby["players"].i64()), f.lobby_capacity = int(lobby["capacity"].i64());
    }
    const JVal& seen = c["lastSeenAgo"];
    if (seen.type == JVal::Num) f.last_seen = int(seen.i64());
    f.motto = c["motto"].str(), f.name = c["name"].str(), f.location = c["location"].str(), f.bio = c["bio"].str();
    f.has_profile = c["motto"].type == JVal::Str;
    return f;
}

void push_result(NetResult r) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_results.size() < 32) g_results.push_back(std::move(r));
}

// The messages deleted in the Message Center, one id per line.
std::wstring hidden_path() { return g_dir + L"\\data\\bin\\fh2mp-guide-hidden.txt"; }

void load_hidden() {
    std::string text;
    if (!read_file(hidden_path(), &text, nullptr)) return;
    size_t at = 0;
    while (at < text.size()) {
        int64_t id = strtoll(text.c_str() + at, nullptr, 10);
        if (id > 0) g_hidden.insert(id);
        size_t end = text.find('\n', at);
        at = end == std::string::npos ? text.size() : end + 1;
    }
}

// Writes the newest 500 ids (the server drops week-old messages, so older ids mean nothing).
void save_hidden(const std::set<int64_t>& ids) {
    std::string text;
    size_t skip = ids.size() > 500 ? ids.size() - 500 : 0;
    for (int64_t id : ids) {
        if (skip) { skip--; continue; }
        text += std::to_string(id) + "\n";
    }
    HANDLE f = CreateFileW(hidden_path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        logf("guide: deleted messages not saved: %ls could not be written (%lu); they show again after a restart", hidden_path().c_str(), GetLastError());
        return;
    }
    DWORD put = 0;
    WriteFile(f, text.data(), DWORD(text.size()), &put, nullptr);
    CloseHandle(f);
}

// One friend's conversation as last read: the messages they sent (the newest 20).
struct ThreadCache {
    int64_t fetched = -1;  // the thread's lastIncomingId when it was read
    std::vector<TextMessage> msgs;
};

DWORD WINAPI worker(LPVOID) {
    Conn conn;
    bool have = false, reachable = true;
    std::string why_none;
    ULONGLONG next_comm = 0, next_inv = 0, next_session = 0;
    std::set<int64_t> seen_invites;
    bool first_invites = true, first_comm = true;
    std::map<uint64_t, double> pic_done;  // xuid -> picture version fetched (-1: none on the server)
    std::map<std::string, ThreadCache> threads;
    std::map<uint64_t, Friend::State> was_state;
    std::map<uint64_t, ULONGLONG> online_told;
    std::set<uint64_t> seen_requests;
    load_hidden();
    for (;;) {
        Op op{};
        bool has_op = false, open = false;
        {
            std::unique_lock<std::mutex> l(g_mu);
            ULONGLONG now = GetTickCount64();
            ULONGLONG due = std::min(next_comm, next_inv);
            if (g_ops.empty() && !g_wake && due > now) g_cv.wait_for(l, std::chrono::milliseconds(std::min<ULONGLONG>(due - now, 2000)));
            if (g_wake) next_comm = next_inv = 0, g_wake = false;
            if (!g_ops.empty()) op = g_ops.front(), g_ops.pop_front(), has_op = true;
            open = g_open;
        }
        ULONGLONG now = GetTickCount64();
        if (!have && now >= next_session) {
            next_session = now + 10000;
            std::string why;
            have = load_conn(&conn, &why);
            if (have) {
                logf("guide: server %s, player %llx: friends and invites on", conn.base.c_str(), (unsigned long long)conn.xuid);
                next_comm = next_inv = 0;
            } else if (why != why_none) {
                logf("guide: no friends or invites: %s", why.c_str());
                why_none = why;
            }
            std::lock_guard<std::mutex> l(g_mu);
            if (g_social.session != have) g_social.session = have, g_social.version++;
        }
        if (has_op) {
            NetResult r;
            r.op = op.op, r.xuid = op.xuid, r.name = op.name, r.id = op.id;
            if (!have) {
                r.message = "Not signed in";
            } else {
                std::string body, out, err;
                int status = 0;
                std::string them = std::to_string(op.xuid);
                if (op.op == NetOp::Invite) {
                    body = "{\"player\":\"" + them + "\"" + (op.text.empty() ? "" : ",\"text\":" + quoted(op.text)) + "}";
                    status = request(conn, L"POST", "/v1/community/invites", body, &out, &err);
                } else if (op.op == NetOp::FriendRequest) {
                    body = "{\"player\":" + quoted(op.name.empty() ? them : op.name) + "}";
                    status = request(conn, L"POST", "/v1/community/friends", body, &out, &err);
                } else if (op.op == NetOp::RemoveFriend) {
                    status = request(conn, L"DELETE", "/v1/community/friends/" + them, "", &out, &err);
                } else if (op.op == NetOp::SendText) {
                    status = request(conn, L"POST", "/v1/community/chat/" + them, "{\"text\":" + quoted(op.text) + "}", &out, &err);
                } else if (op.op == NetOp::Join) {
                    body = "{\"lobby\":" + quoted(op.text) + ",\"kind\":" + quoted(op.kind) + "}";
                    status = request(conn, L"POST", "/v1/community/reservation", body, &out, &err);
                } else if (op.op == NetOp::SetProfile) {
                    status = request(conn, L"POST", "/v1/community/profile", "{" + quoted(op.name) + ":" + quoted(op.text) + "}", &out, &err);
                } else if (op.op == NetOp::Accept) {
                    status = request(conn, L"POST", "/v1/community/invites/" + std::to_string(op.id) + "/accept", "{}", &out, &err);
                } else if (op.op == NetOp::Decline) {
                    status = request(conn, L"DELETE", "/v1/community/invites/" + std::to_string(op.id), "", &out, &err);
                } else if (op.op == NetOp::AcceptFriend) {
                    status = request(conn, L"POST", "/v1/community/friends/" + std::to_string(op.xuid) + "/accept", "{}", &out, &err);
                } else if (op.op == NetOp::DeclineFriend) {
                    status = request(conn, L"DELETE", "/v1/community/friends/" + std::to_string(op.xuid) + "/request", "", &out, &err);
                } else if (op.op == NetOp::ReadChat) {
                    status = request(conn, L"PATCH", "/v1/community/chat/" + op.name, "{\"lastId\":" + std::to_string(op.id) + "}", &out, &err);
                }
                r.ok = status >= 200 && status < 300;
                if (!r.ok) r.message = refusal(status, out, err, &r.code);
                if (r.ok && (op.op == NetOp::Accept || op.op == NetOp::Join)) {
                    JVal v;
                    if (json_parse(out, &v)) r.kind = v["reservation"]["kind"].str();
                }
                const char* what = op.op == NetOp::Invite ? "game invite to" : op.op == NetOp::Accept ? "accepting the invite from"
                                   : op.op == NetOp::Decline ? "declining the invite from" : op.op == NetOp::AcceptFriend ? "accepting the friend request from"
                                   : op.op == NetOp::DeclineFriend ? "declining or cancelling the friend request with"
                                   : op.op == NetOp::FriendRequest ? "friend request to" : op.op == NetOp::RemoveFriend ? "removing the friend"
                                   : op.op == NetOp::SendText ? "message to" : op.op == NetOp::Join ? "holding a seat in the game of"
                                   : op.op == NetOp::SetProfile ? "setting the profile text" : "marking read the chat";
                if (r.ok) logf("guide: %s %s (%llx) done", what, op.name.c_str(), (unsigned long long)op.xuid);
                else logf("guide: %s %s (%llx) failed: %s", what, op.name.c_str(), (unsigned long long)op.xuid, r.message.c_str());
                if (op.op != NetOp::Invite) next_inv = 0;
                next_comm = 0;
            }
            push_result(r);
        }
        if (!have) continue;
        now = GetTickCount64();
        if (now >= next_comm) {
            next_comm = now + (open ? 5000 : 30000);
            std::string out, err;
            int status = request(conn, L"GET", "/v1/community", "", &out, &err);
            JVal v;
            if (status == 200 && json_parse(out, &v)) {
                Friend me = card(v["me"]);
                std::vector<Friend> friends, requests, outgoing;
                std::vector<ChatThread> chats;
                for (auto& f : v["friends"].a) friends.push_back(card(f));
                for (auto& f : v["incoming"].a) requests.push_back(card(f));
                for (auto& f : v["outgoing"].a) outgoing.push_back(card(f));
                for (auto& c : v["chats"].a) {
                    ChatThread t;
                    const JVal& id = c["id"];
                    t.id = id.type == JVal::Str ? id.s : std::to_string(id.i64());
                    t.title = c["title"].str(), t.last = c["last"].str(), t.unread = int(c["unread"].i64()), t.last_incoming = c["lastIncomingId"].i64();
                    chats.push_back(t);
                }
                // what is new since the last read gets a toast (nothing on the first read: it was all there before the game started)
                for (const Friend& f : friends) {
                    auto it = was_state.find(f.xuid);
                    // "is online" once per ten minutes a friend: a launcher that polls late flips between online and offline
                    if (!first_comm && it != was_state.end() && it->second == Friend::Offline && f.state != Friend::Offline &&
                        (!online_told.count(f.xuid) || now - online_told[f.xuid] > 600000)) {
                        online_told[f.xuid] = now;
                        NetResult r;
                        r.op = NetOp::Online, r.ok = true, r.xuid = f.xuid, r.name = f.tag;
                        push_result(r);
                    }
                    was_state[f.xuid] = f.state;
                }
                for (const Friend& f : requests) {
                    if (!seen_requests.insert(f.xuid).second || first_comm) continue;
                    logf("guide: friend request from %s (%llx)", f.tag.c_str(), (unsigned long long)f.xuid);
                    NetResult r;
                    r.op = NetOp::GotRequest, r.ok = true, r.xuid = f.xuid, r.name = f.tag;
                    push_result(r);
                }
                // friends' messages: a conversation is read again when its last incoming message changed (four conversations a pass)
                int fetched = 0;
                for (const ChatThread& t : chats) {
                    if (t.id == "club") continue;
                    ThreadCache& tc = threads[t.id];
                    if (tc.fetched != t.last_incoming && t.last_incoming > 0 && fetched < (first_comm ? 64 : 4)) {
                        fetched++;
                        std::string mo, me2;
                        JVal mv;
                        if (request(conn, L"GET", "/v1/community/chat/" + t.id, "", &mo, &me2) == 200 && json_parse(mo, &mv)) {
                            bool had = tc.fetched >= 0;
                            int64_t before = tc.msgs.empty() ? 0 : tc.msgs.back().id;
                            tc.msgs.clear();
                            for (auto& m : mv["messages"].a) {
                                if (uint64_t(m["sender"].i64()) == conn.xuid) continue;
                                TextMessage x;
                                x.id = m["id"].i64(), x.from = uint64_t(m["sender"].i64()), x.tag = m["gamertag"].str(), x.body = m["body"].str();
                                x.created = m["created"].num(), x.chat = t.id;
                                tc.msgs.push_back(x);
                            }
                            if (tc.msgs.size() > 20) tc.msgs.erase(tc.msgs.begin(), tc.msgs.end() - 20);
                            tc.fetched = t.last_incoming;
                            if ((had || !first_comm) && !tc.msgs.empty() && tc.msgs.back().id > before) {
                                logf("guide: message %lld from %s (%llx)", (long long)tc.msgs.back().id, tc.msgs.back().tag.c_str(), (unsigned long long)tc.msgs.back().from);
                                NetResult r;
                                r.op = NetOp::GotText, r.ok = true, r.xuid = tc.msgs.back().from, r.name = tc.msgs.back().tag, r.id = tc.msgs.back().id;
                                push_result(r);
                            }
                        }
                    }
                    int unread = t.unread;  // the newest `unread` of them are unread
                    for (size_t i = tc.msgs.size(); i-- > 0;) tc.msgs[i].unread = unread-- > 0;
                }
                first_comm = false;
                std::lock_guard<std::mutex> l(g_mu);
                g_social.me = me, g_social.friends = friends, g_social.requests = requests, g_social.outgoing = outgoing, g_social.chats = chats;
                g_social.texts.clear();
                for (const ChatThread& t : chats) {
                    auto it = threads.find(t.id);
                    if (it == threads.end()) continue;
                    for (const TextMessage& m : it->second.msgs)
                        if (!g_hidden.count(m.id)) g_social.texts.push_back(m);
                }
                std::sort(g_social.texts.begin(), g_social.texts.end(), [](const TextMessage& a, const TextMessage& b) { return a.id > b.id; });
                g_social.loaded = true, g_social.error.clear();
                if (!reachable) logf("guide: the server answers again"), reachable = true;
            } else {
                std::lock_guard<std::mutex> l(g_mu);
                g_social.error = refusal(status, out, err);
                if (reachable) logf("guide: friends could not be read: %s", g_social.error.c_str()), reachable = false;
                if (status == 401) have = false, next_session = 0;  // the launcher may have written a new session
            }
            std::set<int64_t> to_save;
            {
                std::lock_guard<std::mutex> l(g_mu);
                g_social.version++;
                if (g_hidden_dirty) to_save = g_hidden, g_hidden_dirty = false;
            }
            if (!to_save.empty()) save_hidden(to_save);
        }
        now = GetTickCount64();
        if (have && now >= next_inv) {
            next_inv = now + (open ? 5000 : 10000);
            std::string out, err;
            int status = request(conn, L"GET", "/v1/community/invites", "", &out, &err);
            JVal v;
            if (status == 200 && json_parse(out, &v)) {
                std::vector<ReceivedInvite> list;
                for (auto& iv : v["invites"].a) {
                    ReceivedInvite r;
                    r.id = iv["id"].i64(), r.from = card(iv["from"]), r.text = iv["text"].str(), r.kind = iv["kind"].str();
                    r.age = int(iv["age"].i64()), r.expires_in = int(iv["expiresIn"].i64());
                    list.push_back(r);
                }
                // new invites get a toast: on the first read only the newest (the others were waiting before the game started)
                for (size_t i = 0; i < list.size(); i++) {
                    if (seen_invites.count(list[i].id)) continue;
                    seen_invites.insert(list[i].id);
                    if (first_invites && i > 0) continue;
                    logf("guide: game invite %lld from %s (%llx), %s, %d s left", (long long)list[i].id, list[i].from.tag.c_str(),
                         (unsigned long long)list[i].from.xuid, list[i].kind.c_str(), list[i].expires_in);
                    NetResult r;
                    r.op = NetOp::Received, r.ok = true, r.xuid = list[i].from.xuid, r.name = list[i].from.tag, r.id = list[i].id;
                    push_result(r);
                }
                first_invites = false;
                std::lock_guard<std::mutex> l(g_mu);
                g_social.invites = list;
                g_social.version++;
            } else if (status == 404) {
                next_inv = now + 600000;  // a server without invites: ask again in ten minutes
            }
        }
        // pictures: friends and inviters, one per pass so actions stay quick
        std::vector<std::pair<uint64_t, double>> want;
        {
            std::lock_guard<std::mutex> l(g_mu);
            for (auto& f : g_social.friends) want.emplace_back(f.xuid, f.pic_version);
            for (auto& iv : g_social.invites) want.emplace_back(iv.from.xuid, iv.from.pic_version);
            for (auto& f : g_social.requests) want.emplace_back(f.xuid, f.pic_version);
            for (auto& f : g_social.outgoing) want.emplace_back(f.xuid, f.pic_version);
        }
        for (auto& w : want) {
            auto it = pic_done.find(w.first);
            if (it != pic_done.end() && (it->second == w.second || it->second < 0)) continue;
            std::string out, err;
            int status = request(conn, L"GET", "/v1/gamerpic/" + std::to_string(w.first) + ".png", "", &out, &err);
            if (status == 0) break;  // unreachable: try on a later pass
            PicReady p;
            p.xuid = w.first;
            if (status == 200 && decode_png((const unsigned char*)out.data(), out.size(), &p.rgba, &p.w, &p.h) && p.w <= 512 && p.h <= 512) {
                pic_done[w.first] = w.second;
                std::lock_guard<std::mutex> l(g_mu);
                if (g_pics.size() < 64) g_pics.push_back(std::move(p));
            } else {
                pic_done[w.first] = -1;
            }
            break;
        }
    }
}

}  // namespace

void net_start(const std::wstring& azimuth_dir, void (*log)(const char*)) {
    g_dir = azimuth_dir, g_log = log;
    CloseHandle(CreateThread(nullptr, 0, worker, nullptr, 0, nullptr));
}

Social net_social() {
    std::lock_guard<std::mutex> l(g_mu);
    return g_social;
}

void net_guide_open(bool open) {
    std::lock_guard<std::mutex> l(g_mu);
    if (open && !g_open) g_wake = true;
    g_open = open;
    g_cv.notify_all();
}

static void push_op(NetOp op, uint64_t xuid, int64_t id, const std::string& name, const std::string& text = "", const std::string& kind = "") {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_ops.size() < 48) g_ops.push_back({op, xuid, id, name, text, kind});
    g_cv.notify_all();
}

void net_invite(uint64_t xuid, const std::string& name, const std::string& text) { push_op(NetOp::Invite, xuid, 0, name, text); }
void net_accept(int64_t id, uint64_t from, const std::string& name) { push_op(NetOp::Accept, from, id, name); }
void net_decline(int64_t id, uint64_t from, const std::string& name) { push_op(NetOp::Decline, from, id, name); }
void net_friend_request(const std::string& player, uint64_t xuid) { push_op(NetOp::FriendRequest, xuid, 0, player); }
void net_accept_friend(uint64_t xuid, const std::string& name) { push_op(NetOp::AcceptFriend, xuid, 0, name); }
void net_decline_friend(uint64_t xuid, const std::string& name) { push_op(NetOp::DeclineFriend, xuid, 0, name); }
void net_remove_friend(uint64_t xuid, const std::string& name) { push_op(NetOp::RemoveFriend, xuid, 0, name); }
void net_send_text(uint64_t xuid, const std::string& name, const std::string& text) { push_op(NetOp::SendText, xuid, 0, name, text); }
void net_read_chat(const std::string& chat, int64_t last_id) { push_op(NetOp::ReadChat, 0, last_id, chat); }
void net_join(uint64_t xuid, const std::string& name, const std::string& lobby, const std::string& kind) { push_op(NetOp::Join, xuid, 0, name, lobby, kind); }

void net_set_profile(const std::string& field, const std::string& text) { push_op(NetOp::SetProfile, 0, 0, field, text); }

void net_hide_text(int64_t id) {
    std::lock_guard<std::mutex> l(g_mu);
    g_hidden.insert(id), g_hidden_dirty = true;
    for (size_t i = 0; i < g_social.texts.size(); i++)
        if (g_social.texts[i].id == id) g_social.texts.erase(g_social.texts.begin() + i--);
    g_social.version++;
    g_wake = true;
    g_cv.notify_all();
}

bool net_take_result(NetResult* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_results.empty()) return false;
    *out = std::move(g_results.front());
    g_results.pop_front();
    return true;
}

bool net_take_pic(PicReady* out) {
    std::lock_guard<std::mutex> l(g_mu);
    if (g_pics.empty()) return false;
    *out = std::move(g_pics.front());
    g_pics.pop_front();
    return true;
}
