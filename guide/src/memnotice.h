// When to tell the player that memory is running out (crash family memory-growth-oom, #91 / #468): the game's commit grows in a long
// Free Roam (report 25aff8e573c2f3b0: 9 GB at 5 min, 14.6 GB at 52 min, 21 GB on a 15 GB PC) until Windows' commit limit is reached and a
// small allocation stops Azimuth ("out of memory: 864 bytes at azimuth+0x493f21"). The launcher's memory guard only watches since 1.3.2
// (it ended healthy games in 1.3.1), so nothing told the player.
//
// The notice is a toast; it ends and limits nothing. It needs both: the game's own commit at 85% of the PC's RAM or more (normal play is
// 11-16 GB on 32 GB PCs, 12 GB on a 16 GB laptop early in a boot, so a big PC playing normally never gets there) and the system's commit
// charge at 90% of its limit or more, for SUSTAIN samples in a row (loading jumps 1-8 GB within seconds and settles). Once shown it waits
// until the charge is under 80% again, and shows at most MAX_SHOWN times a game. No Windows calls here: guide/tests/memnotice_test.cpp.
#pragma once

#include <cstdint>

struct MemNotice {
    static constexpr int SUSTAIN = 6;       // samples (MemNotice::PERIOD apart): 30 s
    static constexpr double PERIOD = 5.0;   // seconds between samples
    static constexpr int MAX_SHOWN = 2;
    int over = 0, shown = 0;
    bool armed = true;

    // One sample: the game's commit, the PC's RAM, the system's commit charge and limit (bytes). -> true when the toast is due now.
    bool sample(uint64_t game_commit, uint64_t ram, uint64_t sys_used, uint64_t sys_limit) {
        if (!ram || !sys_limit) return false;
        double sys = double(sys_used) / double(sys_limit);
        if (sys < 0.80) armed = true;
        bool high = double(game_commit) >= 0.85 * double(ram) && sys >= 0.90;
        over = high ? over + 1 : 0;
        if (!high || over < SUSTAIN || !armed || shown >= MAX_SHOWN) return false;
        armed = false, over = 0, ++shown;
        return true;
    }
};
