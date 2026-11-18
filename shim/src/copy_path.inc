/* Another player's car follows its owner's path, pure part (no game memory; native tests in tests/test_copy_path.sh).
 *
 * The copy of another player's car (RemoteCarPrediction, vtable 0x2d3b360) is a full physics body in this game's world. Each frame
 * RemoteCarPrediction::helper_9effa0 (0x9effa0) blends it from a start state A (+0xa0, time t0 at +0x2c) to the owner's latest
 * sample B (+0x1d0, time t1 at +0x30) with the body's Hermite blend (vslot 0xb90, 0x12bbb90), applies the result (vslot 0xb50) and
 * adds the contact offset (+0x320). Between frames the world step moves the body like any other: contacts with this game's own car
 * push it, lift it and spin it. When the next sample arrives, RemoteCarPrediction::ApplyUnpackedState (0x9ebc20) restarts the
 * blend from the body as it is then (CaptureState, vslot 0xb48, into A, t0 = the last frame's time at +0x330). Every shove of a
 * frame is kept and only a fraction of it is pulled back before the next sample keeps it again, so a car its owner has pinned on the
 * ground climbs onto this game's car (measured 2026-11-18: the Fiat drawn 1.42 m above its owner's position for 4 s on the
 * truck's game, with the contact offset at most 0.29 m), and this game's car meets no resistance and drives into the other's
 * real position (the two owners' cars 1.25 m apart, centre to centre, where touching is 3 m).
 *
 * With the switch on, the blend restarts from the old blend at the last frame's time instead: the state the game itself applied
 * that frame, before the world step moved it. The copy then stays on its owner's path; contacts still push this game's own car.
 * (Letting the copy keep up to 15 cm of the world step's displacement was tried 2026-11-18 and dropped: without contact the body
 * also drifts from the path between frames, and that made free driving jitter.)
 */

#define CP_TYPE_REMOTE_PLAYER 3 /* Car+0x4d08 */
#define CP_MAX_BLEND_MS 2000    /* helper_9effa0 applies no blend over a longer gap */

/* The fraction of the old blend (t0..t1) at the last frame's time tlast, as helper_9effa0 computed it for that frame, clamped to
 * 0..1; -1 when the old blend was not applied (too short or too long). */
static float cp_frac(int t0, int t1, int tlast)
{
    int d = t1 - t0, e = tlast - t0;
    if (d < 2 || d > CP_MAX_BLEND_MS) {
        return -1.0f;
    }
    if (e <= 0) {
        return 0.0f;
    }
    if (e >= d) {
        return 1.0f;
    }
    return (float)e / (float)d;
}

/* 1 when ApplyUnpackedState restarted the blend from the body in its usual branch: before the call the copy was fresh (stale state
 * +0x354 == 0); after it the snap branch was not taken (+0x1a == 0) and t0 is the last frame's time. */
static int cp_restarted_from_body(int stale_before, int snapped_after, int t0_after, int tlast)
{
    return stale_before == 0 && !snapped_after && t0_after == tlast;
}
