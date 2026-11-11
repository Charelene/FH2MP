// The in-game Guide (FH2MPGuide.dll): shared declarations between the overlay (overlay.cpp: hooks, Direct3D 12, input) and the Guide's
// drawing (guide.cpp: the design canvas https://claude.ai/artifact/GYsZgmPY8hSP7A4iGQaAyH, artboards 1 and 7, in Dear ImGui).
#pragma once

#include <cstdint>
#include <string>

#include "imgui.h"

enum class PadEvent { Toggle, Up, Down, Left, Right, A, B, X, Y, Lb, Rb, Start };

// XInput button bits (0x0400 is the Guide button, reported by XInputGetStateEx only).
namespace padbits {
constexpr uint16_t Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008, Start = 0x0010, Lb = 0x0100, Rb = 0x0200, Guide = 0x0400,
                   A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000;
}

// Readings (buttons, left stick, time) into events: the Guide button held for GUIDE_HOLD_MS toggles, a short press closes an open Guide,
// navigation only while open, a held direction repeats after 400 ms then every 80 ms. Pure (tests/test_reader.cpp).
struct PadReader {
    static constexpr uint64_t GUIDE_HOLD_MS = 1000, REPEAT_DELAY_MS = 400, REPEAT_EVERY_MS = 80;
    uint16_t prev = 0;
    bool guide_down = false, guide_fired = false;
    uint64_t guide_since = 0;
    int dir = -1;  // PadEvent of the held direction, or -1
    uint64_t next_at = 0;
    // Appends the events of one reading to out (at most 16); returns how many.
    int feed(uint16_t buttons, int16_t lx, int16_t ly, uint64_t now_ms, bool open, PadEvent* out);
};

// A font's hhea ascender and descender in em. The fonts are loaded so that a size of N draws N-pixel em glyphs, as CSS's font-size does
// (Dear ImGui by default makes ascender minus descender N pixels, which drew Segoe UI a quarter smaller than the canvas).
struct FontMetrics {
    float asc = 0.8f, desc = 0.2f;
};

// Fonts and textures the Guide draws with (made by the overlay once Direct3D is ready).
struct GuideAssets {
    ImFont* text = nullptr;       // Segoe UI, or ImGui's own font when Windows has none
    ImFont* text_bold = nullptr;  // Segoe UI Bold (toast titles), or the same as text
    ImFont* icons = nullptr;      // Material Icons (filled)
    FontMetrics text_m, bold_m, icons_m;
    ImTextureID gamerpic = 0;     // the player's picture (Azimuth's data/player.png), 0 when there is none
};

// Loads Segoe UI, Segoe UI Bold and the embedded Material Icons into ImGui's atlas with CSS sizing (assets.cpp). Needs an ImGui context.
void load_fonts(GuideAssets& a);

// The canvas' toast kinds (artboard 7): the square's colour, the title's colour, and whether a player's picture glides in (invites only).
enum class ToastKind { Invite, Info, Saving, Warning };

enum class GuideAction { None, Resume, Soon };

class Guide {
public:
    bool open = false;
    std::string gamertag = "FH2MP";

    void toggle(double now);
    // One pad or key event while open; what the overlay must do.
    GuideAction handle(PadEvent e, double now);
    // Draws into the game's frame (w x h pixels). Returns true while something is on screen (the Guide or a toast).
    bool draw(ImDrawList* dl, const GuideAssets& a, float w, float h, double now);
    // A 360 toast at the bottom centre (7.5 s cycle of the canvas, played once). pic: the other player's picture on an invite toast (it
    // glides in over the icon at 2.0 s), 0 for none.
    void toast(ToastKind kind, const char* title, const char* detail, uint32_t icon, ImTextureID pic, double now);
    bool busy(double now) const;

private:
    int blade_ = 1;
    int focus_[3] = {0, 0, 0};
    double opened_ = -10, closed_ = -10, switched_ = -10;
    int switch_dir_ = 1;
    struct Tween {
        float from = 0, to = 0;
        double start = -10;
        float secs = 0;
        void go(float v, double now, float s);
        float value(double now) const;
    };
    Tween bar_[3], width_[3];
    bool widths_set_ = false;
    std::string toast_title_, toast_detail_;
    ToastKind toast_kind_ = ToastKind::Info;
    uint32_t toast_icon_ = 0;
    ImTextureID toast_pic_ = 0;
    double toast_at_ = -100;

    void switch_to(int b, double now);
    void draw_toast(ImDrawList* dl, const GuideAssets& a, float s, ImVec2 origin, double now);
};

// Cubic-bezier easing as CSS defines it (x1, y1, x2, y2), for t in 0..1.
float css_bezier(float x1, float y1, float x2, float y2, float t);
