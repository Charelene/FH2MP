// sfx-test.exe [dir [play]]: the Xbox 360 theme's Guide sounds (guide.cpp, sounds_for) for a walk through the Guide, and none in another
// theme. dir: a data\bin\guide-360 folder whose 16 files must all load (sfx.cpp); play: then they are heard through the mixer, the opening
// with focus ticks over it. Exit code 0 when every step gave what was expected.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

#include "guide.h"

namespace {

const char* name(Sfx s) {
    static const char* n[] = {"HudOpen", "HudClose", "BladeOpen", "BladeSwitch1", "BladeSwitch2", "BladeSwitch3", "BladeSwitch4", "Focus",
                              "InactiveFocus", "Select", "InactiveSelect", "Back", "TabSwitch", "KeyFocus", "KeySelect", "Notify"};
    return n[int(s)];
}

std::string list(const std::vector<Sfx>& v) {
    std::string s;
    for (Sfx x : v) s += (s.empty() ? "" : " ") + std::string(name(x));
    return "[" + s + "]";
}

int failed = 0;

void expect(const char* step, const std::vector<Sfx>& got, std::initializer_list<Sfx> want) {
    std::vector<Sfx> w(want);
    bool ok = got == w;
    if (!ok) failed++;
    printf("%s %-34s got %s%s\n", ok ? "ok  " : "FAIL", step, list(got).c_str(), ok ? "" : (" want " + list(w)).c_str());
}

}  // namespace

void print_line(const char* s) { printf("log: %s\n", s); }

int main(int argc, char** argv) {
    if (argc > 1) {
        std::string d = std::string(argv[1]) + "\\";
        int n = sfx_load(std::wstring(d.begin(), d.end()), print_line);
        bool ok = n == int(Sfx::Count);
        failed += !ok;
        printf("%s loaded %d of %d from %s\n", ok ? "ok  " : "FAIL", n, int(Sfx::Count), argv[1]);
        if (argc > 2 && std::string(argv[2]) == "play") {
            sfx_play(Sfx::HudOpen), sfx_play(Sfx::BladeOpen);
            for (Sfx s : {Sfx::Focus, Sfx::Focus, Sfx::BladeSwitch4, Sfx::Select, Sfx::Back, Sfx::HudClose, Sfx::Notify}) Sleep(450), sfx_play(s);
            Sleep(4000);  // the stream closes after three quiet seconds
            sfx_play(Sfx::Notify);
            Sleep(900);
        }
    }
    theme_apply("xbox360");
    Guide g;
    double t = 1;
    auto ev = [&](PadEvent e) {
        g.handle(e, t += 1);
        return g.take_sounds();
    };
    g.toggle(t);
    expect("open", g.take_sounds(), {Sfx::HudOpen, Sfx::BladeOpen});
    expect("down on Friends", ev(PadEvent::Down), {Sfx::Focus});
    expect("up again", ev(PadEvent::Up), {Sfx::Focus});
    expect("right to Settings", ev(PadEvent::Right), {Sfx::BladeSwitch4});
    expect("right at the last blade", ev(PadEvent::Right), {});
    expect("lb to the player's blade", ev(PadEvent::Lb), {Sfx::BladeSwitch2});
    expect("left to Games", ev(PadEvent::Left), {Sfx::BladeSwitch1});
    expect("down past greyed rows", ev(PadEvent::Down), {});
    expect("right back", ev(PadEvent::Right), {Sfx::BladeSwitch2});
    expect("A on Friends", ev(PadEvent::A), {Sfx::Select});
    expect("rb to Messages", ev(PadEvent::Rb), {Sfx::TabSwitch});
    expect("B back home", ev(PadEvent::B), {Sfx::Back});
    expect("B closes", ev(PadEvent::B), {Sfx::HudClose});

    g.toggle(t += 1);
    g.take_sounds();
    ev(PadEvent::Right);
    expect("A on Theme", ev(PadEvent::A), {Sfx::Select});
    expect("B keeps Xbox 360", ev(PadEvent::B), {Sfx::Back});
    ev(PadEvent::B);

    theme_apply("xbox");
    g.toggle(t += 1);
    expect("open in the Xbox theme", g.take_sounds(), {});
    expect("down in the Xbox theme", ev(PadEvent::Down), {});
    expect("B in the Xbox theme", ev(PadEvent::B), {});
    printf("%s\n", failed ? "FAILED" : "all ok");
    return failed ? 1 : 0;
}
