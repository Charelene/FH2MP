/* Contact look-ahead pause, pure part (no game memory; native tests in tests/test_contact_forecast.sh).
 *
 * Each game sends its own car not as it is but as a look-ahead: LocalCarPrediction::helper_9b9350 (0x9b9350) runs the car's own
 * dynamics (CCarDynamics::StepSimulation 0x12eb090, through the fast/full predict slots 0x12d6fa0/0x12d6eb0) about a third of a
 * second ahead and sends that state, stamped with the future time. Car-to-car contacts are only solved in the world step
 * (CPhysicsWorld::StepWorld 0x12eb450 -> helper_12d7b30), which the look-ahead never runs, so while two players' cars touch, each
 * one's look-ahead drives straight through the other car and projects the contact's velocity spikes a third of a second ahead.
 * The other game shows that (a car on the other's bonnet, shaking), its own car is shoved by it, and the loop feeds itself.
 *
 * While the local car touches another player's car (and for CF_HOLD_MS after), the look-ahead is skipped the way the game itself
 * skips it for a car standing still: its gate reads the car's speed (CCarDynamics::GetSpeedMps, field +0xe28 of the interface
 * the prediction holds) and below 0.001 m/s it sends the car's real state with the usual stamp.
 */

#ifndef CONTACT_FORECAST_INC
#define CONTACT_FORECAST_INC

#define CF_SLOTS 8
#define CF_HOLD_MS 500u
#define CF_TYPE_REMOTE_PLAYER 3 /* Car+0x4d08: 0 local player, 2/5 traffic (ours/theirs), 3 another player's car */

typedef struct {
    const void *who; /* the LocalCarPrediction that had the contact */
    unsigned long long at;
} cf_slot;

/* Notes a contact of `who` at `now`: its own slot, else an empty one, else the oldest. */
static void cf_touch(cf_slot *t, const void *who, unsigned long long now)
{
    int i, pick = -1;
    for (i = 0; i < CF_SLOTS; i++) {
        if (t[i].who == who) {
            t[i].at = now;
            return;
        }
    }
    for (i = 0; i < CF_SLOTS; i++) {
        if (!t[i].who) {
            pick = i;
            break;
        }
    }
    if (pick < 0) {
        pick = 0;
        for (i = 1; i < CF_SLOTS; i++) {
            if (t[i].at < t[pick].at) {
                pick = i;
            }
        }
    }
    t[pick].who = who;
    t[pick].at = now;
}

/* 1 when `who` touched another player's car within `hold` ms before `now`. */
static int cf_recent(const cf_slot *t, const void *who, unsigned long long now, unsigned hold)
{
    int i;
    if (!who) {
        return 0;
    }
    for (i = 0; i < CF_SLOTS; i++) {
        if (t[i].who == who) {
            return now >= t[i].at && now - t[i].at <= hold;
        }
    }
    return 0;
}

/* Who drives into whom in a contact between the local car and another player's car, from the velocities the game's collision
 * limiter (LocalCarPrediction::vfunc_14, 0x9c6900) works with: each car's velocity before the contact's impulse J (mine - J/m_mine,
 * theirs + J/m_theirs, J and the normal n from the contact record +0x50 / +0x20). The closing speed is -(mine - theirs).n; 1 when
 * the local car's part of it (-mine.n) is at least the other car's part (theirs.n) and at least CF_RAM_MIN_MPS, so the local car
 * is the one ramming. A rammer takes the full contact (it stops against the other car instead of sinking into it); a car being
 * rammed, or two cars pressed together standing still, keep the limiter, which caps the change of speed and calms the ping-pong
 * of being squeezed against the copy each frame (measured 2026-11-18: without it the rammed car jumped 8.7 cm back and forth
 * every frame for seconds). */
#define CF_RAM_MIN_MPS 0.5f

/* The pause also starts before a contact: a state sent up to a third of a second ahead reaches the other game while the cars hit,
 * and shows this car driving on through the other one; the car being hit was squeezed against it (measured 2026-11-18: 8.7 cm back
 * and forth every frame for half a second to a second after each hit, while the samples sent before the contact played out).
 * 1 when the two cars (positions p, velocities v, metres and m/s) come within CF_TOUCH_M of each other, centre to centre, within
 * CF_AHEAD_S, closing faster than CF_APPROACH_MIN_MPS. Cars side by side at the same speed, or passing each other further apart
 * than CF_TOUCH_M at their closest, do not count. */
#define CF_TOUCH_M 3.0f  /* closer than a car passing in the next lane (about 3.5 m), well inside a nose-to-tail hit */
#define CF_AHEAD_S 0.7f  /* the look-ahead (about 0.34 s at a 100 ms round trip) plus the time from 3 m apart to touching */
#define CF_APPROACH_MIN_MPS 0.5f

static int cf_will_touch(const float *pm, const float *vm, const float *po, const float *vo)
{
    float d[3], w[3], dd, dw, ww, t, m[3], mm;
    int i;
    for (i = 0; i < 3; i++) {
        d[i] = po[i] - pm[i]; /* from the local car to the other */
        w[i] = vo[i] - vm[i]; /* the other's velocity relative to the local car */
    }
    dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    dw = d[0] * w[0] + d[1] * w[1] + d[2] * w[2];
    ww = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    if (!(dd == dd) || !(ww == ww) || dd > 1e8f || ww > 1e6f) {
        return 0;
    }
    /* closing speed along the line between them: -dw/|d|; compare squared to avoid a square root */
    if (dw >= 0.0f || dw * dw < CF_APPROACH_MIN_MPS * CF_APPROACH_MIN_MPS * dd) {
        return 0;
    }
    /* closest approach at t = -dw/ww; within the window, else at its end */
    t = -dw / ww;
    if (t > CF_AHEAD_S) {
        t = CF_AHEAD_S;
    }
    for (i = 0; i < 3; i++) {
        m[i] = d[i] + w[i] * t;
    }
    mm = m[0] * m[0] + m[1] * m[1] + m[2] * m[2];
    return mm < CF_TOUCH_M * CF_TOUCH_M;
}

static int cf_local_rams(const float *vm, float im, const float *vo, float io, const float *n, const float *j)
{
    float mine_n = (vm[0] - im * j[0]) * n[0] + (vm[1] - im * j[1]) * n[1] + (vm[2] - im * j[2]) * n[2];
    float theirs_n = (vo[0] + io * j[0]) * n[0] + (vo[1] + io * j[1]) * n[1] + (vo[2] + io * j[2]) * n[2];
    return -mine_n >= theirs_n && -mine_n >= CF_RAM_MIN_MPS;
}

#endif
