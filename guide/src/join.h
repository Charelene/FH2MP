// Joining a friend's game through the game's own "game session ready" flow (join.cpp).
#pragma once

#include <string>

struct FH2ModApi;
struct FH2Mod;

// Checks the game's code and hooks the matchmaking module's update. False when this build of the game is not the known one: the Guide
// then falls back to holding the seat and telling the player to go online by hand.
bool join_install(const FH2ModApi* api, FH2Mod* mod, unsigned char* base, void (*log)(const char*));
bool join_available();
// Asks the game to join the session (scid, template, name: the lobby's session in the session directory). who: whose game, for the log.
// It is handed over on the game's thread at the module's next update.
void join_request(const std::string& scid, const std::string& tmpl, const std::string& name, const std::string& who);
// A request the game has not taken yet (the module is not running, e.g. before the game's online part started), and for how long.
bool join_waiting(unsigned long long* for_ms);
void join_cancel();
