// The Xbox system UI the game calls (Windows.Xbox.UI.SystemUI), answered by the Guide (sysui.cpp).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FH2ModApi;
struct FH2Mod;

// What the game asked the system UI for. op is the async object the game holds and waits on; the overlay ends it with sysui_complete.
struct SysAsk {
    enum Kind { ProfileCard, Keyboard, People } kind = ProfileCard;
    uint64_t xuid = 0;                     // ProfileCard: whose gamercard
    std::string text, title, description;  // Keyboard: the default text, the title and the description the game gave; People: title = the prompt
    std::vector<uint64_t> people, chosen;  // People: who can be chosen, and who starts chosen
    unsigned min = 0, max = 0;             // People: how many must and may be chosen
    void* op = nullptr;
};

// Hooks the game's own wrappers of the system UI calls the Guide has a screen for. on_invite runs when the game asks for the invite UI
// (any thread). False when none could be hooked.
bool sysui_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*), void (*on_invite)());
// The next thing the game asked for, if any (the render thread).
bool sysui_take(SysAsk* out);
// Ends what the game is waiting on and releases the overlay's hold on op. done = it has a result (the keyboard's text, the chosen
// players), else it was dismissed: the keyboard is cancelled, the people picker gives no players.
void sysui_complete(void* op, bool done, const std::string& text, const std::vector<uint64_t>& people);
