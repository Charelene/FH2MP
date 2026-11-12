// Forza Horizon 2's achievements for the Guide: the 88 definitions, and what the Achievements mod has unlocked (achievements.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct AchDef {
    const char* id;    // the key in the Achievements mod's unlock file
    const char* name;
    const char* desc;
    int g;             // Gamerscore
    int group;         // 0 the game, 1 Storm Island, 2 the Porsche Expansion
};
// All 88, in the order of the list (the game's 50, Storm Island's 23, the Porsche Expansion's 15; 2000G).
const std::vector<AchDef>& ach_defs();

// A player's unlocked achievements: id -> when ("YYYY-MM-DD HH:MM:SS UTC", the mod's own form).
struct AchUnlocks {
    bool known = false;  // there is a source for them (this player: the mod's file was read; another player: the server answered)
    std::map<std::string, std::string> when;
    bool has(const char* id) const { return when.count(id) > 0; }
    int count() const;       // of the 88
    int gamerscore() const;  // of the 88
};

// This player's unlocks, from the newest fh2-mods\Achievements\User_<id>.ini (the profile played last, as the mod picks it). Reads the
// file when it changed since *stamp (0 at first); false when nothing changed. Without a file, out->known is false.
bool ach_refresh(const std::wstring& azimuth_dir, uint64_t* stamp, AchUnlocks* out);
// The [unlocked] section of the mod's file.
void ach_parse(const std::string& text, AchUnlocks* out);
// When an achievement was unlocked, as the user's short date in local time ("" when it cannot be read).
std::string ach_date(const std::string& when);
