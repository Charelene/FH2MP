// The Xbox 360 Guide's own sounds, played in the Xbox 360 theme. The files are the 360's (system update 2.0.17559, decoded from XMA to
// 16-bit PCM WAV): the launcher puts them into data/bin/guide-360 (constants.GUIDE_360), never the repo. Which 360 scene plays which:
// xam's hudbkgnd.xur HUD_open / HUD_close (the Guide's background), hud's GuideMain.xur BladeOpen and BladeSwitch_1..4 (its blades),
// QuickLaunch.xur tab_Switch, xam's UI skin btn_* (buttons, the "G" ones are the Guide's) and vk_* (the keyboard), notify NotifyPopup
// (every toast, achievements too).
#pragma once

#include <string>

enum class Sfx {
    HudOpen, HudClose, BladeOpen, BladeSwitch1, BladeSwitch2, BladeSwitch3, BladeSwitch4, Focus, InactiveFocus, Select, InactiveSelect, Back,
    TabSwitch, KeyFocus, KeySelect, Notify, Count
};

// The file of a sound in data/bin/guide-360 ("HUD_open.wav", ...).
const char* sfx_file(Sfx s);

// The mod's side (sfx.cpp): reads the files of dir (16-bit PCM WAV, mono or stereo, 44.1 kHz) -> how many it has; a missing one stays
// silent. log gets the playback thread's lines (a device that cannot be opened).
int sfx_load(const std::wstring& dir, void (*log)(const char*));
bool sfx_has(Sfx s);
// Starts a sound over whatever plays (eight at once; the oldest gives way). Returns at once: a thread of its own mixes and plays.
void sfx_play(Sfx s);
