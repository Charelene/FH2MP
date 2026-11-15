// The memory notice's rule (src/memnotice.h). build.sh memtest, then build/memnotice-test.exe: exit code 0 = every case holds.
#include <cstdio>

#include "memnotice.h"

static int failures;
#define CHECK(c) ((c) ? (void)0 : (void)(std::printf("FAIL line %d: %s\n", __LINE__, #c), ++failures))

static const uint64_t GB = 1ull << 30;

int main() {
    {   // report 25aff8e573c2f3b0's shape: 15 GB RAM, the game at 14 GB, the system at 95% of its limit: due after 30 s, once
        MemNotice n;
        int due = 0, at = -1;
        for (int i = 0; i < 20; i++)
            if (n.sample(14 * GB, 15 * GB, 38 * GB, 40 * GB)) due++, at = i;
        CHECK(due == 1);
        CHECK(at == MemNotice::SUSTAIN - 1);
    }
    {   // a 32 GB PC at 1440p ultra, the game at 16 GB and the system busy: normal play, never
        MemNotice n;
        bool any = false;
        for (int i = 0; i < 100; i++) any |= n.sample(16 * GB, 32 * GB, 46 * GB, 50 * GB);
        CHECK(!any);
    }
    {   // a big game commit with plenty of commit left (a large page file): never
        MemNotice n;
        bool any = false;
        for (int i = 0; i < 100; i++) any |= n.sample(14 * GB, 15 * GB, 30 * GB, 60 * GB);
        CHECK(!any);
    }
    {   // loading: a few samples over the line, then back: never
        MemNotice n;
        bool any = false;
        for (int r = 0; r < 10; r++) {
            for (int i = 0; i < MemNotice::SUSTAIN - 1; i++) any |= n.sample(14 * GB, 15 * GB, 38 * GB, 40 * GB);
            any |= n.sample(9 * GB, 15 * GB, 30 * GB, 40 * GB);
        }
        CHECK(!any);
    }
    {   // shown, stays high: not again; the charge falls under 80% and climbs back: again; never more than MAX_SHOWN
        MemNotice n;
        int due = 0;
        for (int r = 0; r < 5; r++) {
            for (int i = 0; i < 40; i++) due += n.sample(14 * GB, 15 * GB, 38 * GB, 40 * GB);
            n.sample(8 * GB, 15 * GB, 20 * GB, 40 * GB);
        }
        CHECK(due == MemNotice::MAX_SHOWN);
    }
    {   // between 80% and 90% after a notice: not re-armed
        MemNotice n;
        int due = 0;
        for (int i = 0; i < 10; i++) due += n.sample(14 * GB, 15 * GB, 38 * GB, 40 * GB);
        for (int i = 0; i < 10; i++) due += n.sample(14 * GB, 15 * GB, 34 * GB, 40 * GB);   // 85%
        for (int i = 0; i < 10; i++) due += n.sample(14 * GB, 15 * GB, 38 * GB, 40 * GB);
        CHECK(due == 1);
    }
    {   // unknown numbers: nothing
        MemNotice n;
        bool any = false;
        for (int i = 0; i < 20; i++) any |= n.sample(14 * GB, 0, 38 * GB, 40 * GB) || n.sample(14 * GB, 15 * GB, 38 * GB, 0);
        CHECK(!any);
    }
    std::printf(failures ? "memnotice: %d failure(s)\n" : "memnotice: all cases hold\n", failures);
    return failures ? 1 : 0;
}
