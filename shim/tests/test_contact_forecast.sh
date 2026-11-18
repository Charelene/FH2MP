#!/usr/bin/env bash
# The contact look-ahead pause (src/contact_forecast.inc): the contact table and the hold window, natively. With FH2_EXE set it also
# checks the exe (1.0.0.64) has the hooked functions' first bytes and the speed getter whose field the pause zeroes.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" <<'EOT'
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "contact_forecast.inc"
static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL line %d: %s\n", __LINE__, #cond); failed++; } } while (0)

static long file_off(const unsigned char *pe, unsigned rva)
{
    unsigned lfanew = *(const uint32_t *)(pe + 0x3c), nsec = *(const uint16_t *)(pe + lfanew + 6), opt = *(const uint16_t *)(pe + lfanew + 0x14), i;
    for (i = 0; i < nsec; i++) {
        const unsigned char *s = pe + lfanew + 0x18 + opt + i * 40;
        uint32_t va = *(const uint32_t *)(s + 12), vs = *(const uint32_t *)(s + 8), raw = *(const uint32_t *)(s + 20);
        if (rva >= va && rva < va + vs) return (long)(raw + (rva - va));
    }
    return -1;
}

int main(int argc, char **argv)
{
    cf_slot t[CF_SLOTS];
    int a, b, c[CF_SLOTS + 1], i;
    memset(t, 0, sizeof t);

    CHECK(!cf_recent(t, &a, 1000, CF_HOLD_MS));          /* never touched */
    CHECK(!cf_recent(t, NULL, 1000, CF_HOLD_MS));
    cf_touch(t, &a, 1000);
    CHECK(cf_recent(t, &a, 1000, CF_HOLD_MS));           /* the same frame */
    CHECK(cf_recent(t, &a, 1500, CF_HOLD_MS));           /* exactly at the end of the hold */
    CHECK(!cf_recent(t, &a, 1501, CF_HOLD_MS));          /* after it */
    CHECK(!cf_recent(t, &b, 1000, CF_HOLD_MS));          /* another car's prediction is not paused */
    CHECK(!cf_recent(t, &a, 999, CF_HOLD_MS));           /* a clock behind the contact is not "recent" */
    cf_touch(t, &a, 3000);                                /* a new contact renews the hold, no second slot */
    CHECK(cf_recent(t, &a, 3400, CF_HOLD_MS));
    for (i = 0; i < CF_SLOTS; i++) CHECK(i == 0 ? t[i].who == &a : t[i].who == NULL);

    memset(t, 0, sizeof t);                               /* full table: the oldest contact gives way */
    for (i = 0; i < CF_SLOTS; i++) cf_touch(t, &c[i], 100 + (unsigned)i);
    cf_touch(t, &c[CF_SLOTS], 5000);
    CHECK(!cf_recent(t, &c[0], 5000, 1u << 30));          /* c[0] (oldest, at 100) was replaced */
    CHECK(cf_recent(t, &c[1], 5000, 1u << 30));
    CHECK(cf_recent(t, &c[CF_SLOTS], 5000, CF_HOLD_MS));

    /* about to hit (positions in m, velocities in m/s; x along the road, z across) */
    {
        const float o[3] = {0, 0, 0}, v20[3] = {20, 0, 0}, v10[3] = {10, 0, 0}, v0[3] = {0, 0, 0};
        const float ahead9[3] = {9, 0, 0}, ahead20[3] = {20, 0, 0}, beside[3] = {0, 0, 2}, lane[3] = {15, 0, 3.5f};
        const float oncoming[3] = {-20, 0, 0};
        CHECK(cf_will_touch(o, v20, ahead9, v10));          /* rear-ending a car 9 m ahead, 10 m/s faster: yes */
        CHECK(!cf_will_touch(o, v20, ahead20, v10));        /* 20 m ahead: not within the window yet */
        CHECK(cf_will_touch(o, v20, ahead9, v0));           /* into a parked car */
        CHECK(!cf_will_touch(o, v20, beside, v20));         /* side by side at the same speed */
        CHECK(!cf_will_touch(o, v20, lane, oncoming));      /* passing an oncoming car in the next lane, 3.5 m apart */
        {
            const float swipe[3] = {20, 0, -3};             /* the car beside steers into this one at 3 m/s */
            CHECK(cf_will_touch(o, v20, beside, swipe));
        }
        CHECK(!cf_will_touch(o, v10, ahead9, v20));         /* the car ahead pulls away */
        CHECK(!cf_will_touch(o, v0, ahead9, v0));           /* both parked */
        {
            const float nan3[3] = {0.0f / 0.0f, 0, 0};
            CHECK(!cf_will_touch(o, v20, nan3, v0));        /* garbage in: no pause */
        }
    }

    /* who rams: normal n points at the local car (closing = -(mine - theirs).n), impulse J already applied */
    {
        const float n[3] = {1, 0, 0}, nj[3] = {0, 0, 0}, still[3] = {0, 0, 0};
        const float toward[3] = {-6, 0, 0};     /* the local car drives at the other at 6 m/s */
        const float comes[3] = {6, 0, 0};       /* the other car drives at the local car */
        const float away[3] = {-6, 0, 0};       /* (for the other car) moving away from the local car */
        const float j[3] = {2000, 0, 0};        /* an impulse that pushed the local car back */
        CHECK(cf_local_rams(toward, 1.0f / 2500, still, 1.0f / 1000, n, nj));    /* truck rams a parked Fiat */
        CHECK(!cf_local_rams(still, 1.0f / 1000, comes, 1.0f / 2500, n, nj));    /* parked Fiat rammed by the truck */
        CHECK(!cf_local_rams(still, 1.0f / 1000, still, 1.0f / 2500, n, nj));    /* pressed together standing: the limiter */
        CHECK(cf_local_rams(toward, 1.0f / 2500, away, 1.0f / 1000, n, nj));     /* pushing a car along: the pusher */
        {
            /* the car being pushed along, in its own game: n points at it, it moves away along n, the pusher follows along n */
            const float along[3] = {6, 0, 0};
            CHECK(!cf_local_rams(along, 1.0f / 1000, along, 1.0f / 2500, n, nj));
        }
        {
            const float slow[3] = {-0.3f, 0, 0};
            CHECK(!cf_local_rams(slow, 1.0f / 2500, still, 1.0f / 1000, n, nj)); /* creeping in under 0.5 m/s: the limiter */
        }
        {
            /* velocities after a fully soft hit (both -30/7 m/s, J = 30000/7): the truck went 6 m/s before it, the Fiat stood */
            const float after[3] = {-30.0f / 7, 0, 0}, jsoft[3] = {30000.0f / 7, 0, 0};
            CHECK(cf_local_rams(after, 1.0f / 2500, after, 1.0f / 1000, n, jsoft));
            CHECK(!cf_local_rams(still, 1.0f / 1000, comes, 1.0f / 2500, n, j));      /* rammed, with an impulse too */
        }
    }

    if (argc > 1) {
        static unsigned char pe[64 << 20];
        static const unsigned char limiter_head[13] = {0x40, 0x56, 0x41, 0x54, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xE0, 0x00, 0x00, 0x00};
        static const unsigned char send_head[11] = {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70};
        static const unsigned char speed_get[9] = {0xC5, 0xFA, 0x10, 0x81, 0x28, 0x0E, 0x00, 0x00, 0xC3};
        /* the look-ahead gate in 0x9b9350: call [rax+0x2a0] (GetSpeedMps) */
        static const unsigned char gate_call[6] = {0xFF, 0x90, 0xA0, 0x02, 0x00, 0x00};
        FILE *f = fopen(argv[1], "rb");
        long n = f ? (long)fread(pe, 1, sizeof pe, f) : 0, o, k;
        int gate = 0;
        if (f) fclose(f);
        CHECK(n > 0x40);
        o = file_off(pe, 0x9c6900); CHECK(o > 0 && memcmp(pe + o, limiter_head, sizeof limiter_head) == 0);
        o = file_off(pe, 0x9b9350); CHECK(o > 0 && memcmp(pe + o, send_head, sizeof send_head) == 0);
        for (k = 0; o > 0 && k < 0x363 - 6; k++) gate |= memcmp(pe + o + k, gate_call, sizeof gate_call) == 0;
        CHECK(gate);
        o = file_off(pe, 0x12cd5e0); CHECK(o > 0 && memcmp(pe + o, speed_get, sizeof speed_get) == 0);
        {
            /* what cf_rams_now and the slow-contact watch rely on, as the limiter itself does it */
            static const unsigned char settings_read[7] = {0x48, 0x8B, 0x05, 0xD4, 0x40, 0xC9, 0x02}; /* mov rax,[0x365a9e8] at 0x9c690d */
            static const unsigned char other_body[8] = {0x48, 0x8B, 0x81, 0x08, 0x4C, 0x00, 0x00, 0xC3}; /* FUN_140f605d0: [rcx+0x4c08] */
            static const unsigned char inv_mass[6] = {0xFF, 0x90, 0x00, 0x03, 0x00, 0x00};              /* call [rax+0x300] at 0x9c6995 */
            static const unsigned char velocity[6] = {0xFF, 0x90, 0x88, 0x02, 0x00, 0x00};              /* call [rax+0x288] at 0x9c69b2 */
            o = file_off(pe, 0x9c690d); CHECK(o > 0 && memcmp(pe + o, settings_read, sizeof settings_read) == 0);
            o = file_off(pe, 0xf605d0); CHECK(o > 0 && memcmp(pe + o, other_body, sizeof other_body) == 0);
            o = file_off(pe, 0x9c6995); CHECK(o > 0 && memcmp(pe + o, inv_mass, sizeof inv_mass) == 0);
            o = file_off(pe, 0x9c69b2); CHECK(o > 0 && memcmp(pe + o, velocity, sizeof velocity) == 0);
        }
        {
            /* the stamp the pause rewrites: lea eax,[r14+rbp]; mov [rbx+0x44],eax at 0x9b966d (now + lead + 186 ms) */
            static const unsigned char stamp_store[7] = {0x41, 0x8D, 0x04, 0x2E, 0x89, 0x43, 0x44};
            o = file_off(pe, 0x9b966d); CHECK(o > 0 && memcmp(pe + o, stamp_store, sizeof stamp_store) == 0);
        }
        if (!failed) puts("contact forecast: hooked functions, the speed gate and the speed getter found in the exe");
    }
    if (!failed) puts("contact forecast: all checks pass");
    return failed != 0;
}
EOT
${HOSTCC:-cc} -O2 -Wall -Wextra -I src -o "$tmp/t" "$tmp/t.c"
if [ -n "${FH2_EXE:-}" ]; then "$tmp/t" "$FH2_EXE"; else "$tmp/t"; fi
