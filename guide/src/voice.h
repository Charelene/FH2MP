// Voice chat: the Guide's own Microsoft.Xbox.GameChat.ChatManager (voice.cpp).
#pragma once

#include <cstdint>

struct FH2ModApi;
struct FH2Mod;

// Hooks the game's activation of the ChatManager (game+0x363a70) and its chat buffer (game+0x36d830), so that the game's voice goes
// through our engine. FH2MP_NO_VOICE=1 leaves both to Azimuth. False when nothing was hooked.
bool voice_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*));

// Who is talking now (heard in the last half second), for the Guide: up to max console ids, the count returned. The local player is
// the id 0 entry when the microphone is open.
int voice_talking(uint64_t* ids, int max);

// The player's voice settings (the Guide's Settings > Voice): mic off = muted, and the game shows this player without a microphone; chat
// off = nothing is heard and nothing is sent; volume 0..4 = how loud the others are (2 is as they come).
void voice_settings(bool mic, bool chat, int volume);
