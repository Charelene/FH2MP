// Joining a friend's game the way the game itself does it (FH2 1.0.0.64; docs/re/invites.md, 2026-11-11).
//
// On Xbox One an accepted invite reached the game as a protocol activation, `ms-xbl-<title>://gameSessionReady?sessionName=..&
// serviceConfigurationId=..&sessionTemplateName=..` (CForza::HandleProtocolActivation, game+0x222550), which raises the event
// ProtocolActivationGameSessionReady with a Nerd::SessionDetails. The matchmaking module's handler for it (game+0x815bd0) notes the
// session; its update (game+0x808b60) then runs the game's own flow "join a game session found by our game party"
// (network_matchmaking.matchmake_show_invite_to_session): it leaves the current session if there is one, reads the session from the
// session directory, shows the game's join prompt or joins at once, and connects.
//
// The Guide does what the activation did: it builds the SessionDetails with the game's own constructor and calls the module's handler,
// on the game's thread (from the module's update, which is hooked for that), so no game input is made up and no state is forced: the
// game decides everything from there, as it did for an invite on Xbox.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "fh2mod.h"
#include "join.h"

namespace {

const FH2ModApi* g_api;
FH2Mod* g_mod;
void (*g_log)(const char*);

void logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_log) g_log(buf);
}

// The game's functions (RVAs of build 1.0.0.64, checked by their first bytes before anything is hooked).
typedef void (*UpdateFn)(void* module, float dt);                                                   // NetworkMatchmakingModule update
typedef void (*SessionReadyFn)(void* module, void** details_ref, void*, void*);                     // its ProtocolActivationGameSessionReady handler
typedef void* (*DetailsCtorFn)(void* mem, const wchar_t* scid, const wchar_t* name, const wchar_t* tmpl);  // Nerd::SessionDetails
typedef void* (*AllocFn)(size_t);                                                                   // the game's operator new
UpdateFn o_update;
SessionReadyFn g_session_ready;
DetailsCtorFn g_details_ctor;
AllocFn g_alloc;
bool g_ready;

std::mutex g_mu;
bool g_pending;
std::wstring g_scid, g_tmpl, g_name;
std::string g_who;
ULONGLONG g_asked_at;

std::wstring wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

// The module's update, on the game's thread: a join asked for by the Guide is handed to the module here, then the update runs and acts
// on it.
void hk_update(void* module, float dt) {
    std::wstring scid, tmpl, name;
    std::string who;
    bool take = false;
    {
        std::lock_guard<std::mutex> l(g_mu);
        if (g_pending) take = true, g_pending = false, scid = g_scid, tmpl = g_tmpl, name = g_name, who = g_who;
    }
    if (take) {
        // a TReference<Nerd::SessionDetails>: the object is counted through its vtable (slot 2 adds, slot 3 releases)
        void* mem = g_alloc(0x70);
        if (!mem) {
            logf("guide: join not started: the game's allocator gave no memory for the session details (%s's game)", who.c_str());
        } else {
            void** obj = (void**)g_details_ctor(mem, scid.c_str(), name.c_str(), tmpl.c_str());
            void** vt = *(void***)obj;
            ((void (*)(void*))vt[2])(obj);
            void* ref = obj;
            g_session_ready(module, &ref, nullptr, nullptr);
            ((void (*)(void*))vt[3])(obj);
            logf("guide: join handed to the game: session %ls of %s (template %ls); the game's own flow takes it from here", name.c_str(), who.c_str(),
                 tmpl.c_str());
        }
    }
    o_update(module, dt);
}

bool starts_with(const unsigned char* at, const unsigned char* want, size_t n) { return memcmp(at, want, n) == 0; }

}  // namespace

bool join_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*)) {
    g_api = api, g_mod = mod, g_log = log;
    static const unsigned char update[] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xd9};  // game+0x808b60
    static const unsigned char ready[] = {0x40, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff};       // game+0x815bd0
    static const unsigned char ctor[] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x48, 0x08, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};   // game+0x2168b0
    static const unsigned char alloc[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0x15};                         // game+0x3229c0
    if (!starts_with(base + 0x808b60, update, sizeof update) || !starts_with(base + 0x815bd0, ready, sizeof ready) ||
        !starts_with(base + 0x2168b0, ctor, sizeof ctor) || !starts_with(base + 0x3229c0, alloc, sizeof alloc)) {
        logf("guide: the game's matchmaking code is not the one this Guide knows: an accepted invite holds the seat, the player goes online by hand");
        return false;
    }
    g_session_ready = (SessionReadyFn)(void*)(base + 0x815bd0);
    g_details_ctor = (DetailsCtorFn)(void*)(base + 0x2168b0);
    g_alloc = (AllocFn)(void*)(base + 0x3229c0);
    FH2Hook* h = api->hook_function(mod, "guide: the matchmaking module's update (game+0x808b60)", base + 0x808b60, (void*)hk_update, (void**)&o_update);
    if (!h || !api->enable_hooks(mod, h)) {
        logf("guide: the matchmaking module's update could not be hooked: an accepted invite holds the seat, the player goes online by hand");
        return false;
    }
    g_ready = true;
    logf("guide: an accepted invite joins through the game's own session-ready flow");
    return true;
}

bool join_available() { return g_ready; }

void join_request(const std::string& scid, const std::string& tmpl, const std::string& name, const std::string& who) {
    std::lock_guard<std::mutex> l(g_mu);
    g_scid = wide(scid), g_tmpl = wide(tmpl), g_name = wide(name), g_who = who, g_pending = true, g_asked_at = GetTickCount64();
}

bool join_waiting(unsigned long long* for_ms) {
    std::lock_guard<std::mutex> l(g_mu);
    if (for_ms) *for_ms = g_pending ? GetTickCount64() - g_asked_at : 0;
    return g_pending;
}

void join_cancel() {
    std::lock_guard<std::mutex> l(g_mu);
    g_pending = false;
}
