// The Xbox system UI the game calls (Windows.Xbox.UI.SystemUI), answered by the Guide (sysui.cpp).
#pragma once

#include <cstdint>
#include <string>

struct FH2ModApi;
struct FH2Mod;

// What the game asked the system UI for. op is the async object the game holds and polls; the overlay ends it with sysui_complete.
struct SysAsk {
    enum Kind { ProfileCard, Keyboard } kind = ProfileCard;
    uint64_t xuid = 0;                     // ProfileCard: whose gamercard
    std::string text, title, description;  // Keyboard: the default text, the title and the description the game gave
    void* op = nullptr;
};

// Hooks the game's own wrappers of the system UI calls the Guide has a screen for. on_invite runs when the game asks for the invite UI
// (any thread). False when none could be hooked.
bool sysui_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*), void (*on_invite)());
// The next thing the game asked for, if any (the render thread).
bool sysui_take(SysAsk* out);
// Ends what the game is waiting on: done = it has a result (the keyboard's text), else it was dismissed. Releases the overlay's hold on op.
void sysui_complete(void* op, bool done, const std::string& text);
