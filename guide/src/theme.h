// The Guide's themes, as the design canvas draws them (Themes and ThemesPlastic boards): eight presets, each a saturated accent, a bright
// and a dark of it and the ink for text on the accent; the six colours also come as "plastic", every surface tinted in the colour's hue.
// Xbox is the default. The picked theme is the player's own (kept on their profile); their gamercard's title bar shows it to others.
#pragma once

#include <string>
#include <vector>

#include "imgui.h"

struct ThemeDef {
    const char* key;   // "xbox", "fh2mp", "red", ... (a plastic one is key + "-plastic")
    const char* name;  // "Xbox", "FH2MP", "Red", ...
    ImU32 accent, bright, dark, ink;
    bool plastic_too;  // the colours have a plastic version, Xbox and FH2MP do not
};

// The eight presets, in the canvas' order.
const std::vector<ThemeDef>& theme_presets();
// The preset of a theme key ("red-plastic" -> Red); Xbox for "" or a key it does not know.
const ThemeDef& theme_preset(const std::string& key);
bool theme_plastic(const std::string& key);
// What the Guide calls it: "Red", "Red Plastic".
std::string theme_name(const std::string& key);
// Sets the Guide's colours (ui.h) to this theme. An unknown key is Xbox.
void theme_apply(const std::string& key);
// The key now in use.
const std::string& theme_current();
