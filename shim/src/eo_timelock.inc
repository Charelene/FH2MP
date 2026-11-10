/* FH2 Extra Options' Weather Control plugin holds the free-roam clock and weather. The plugin's TimeOfDay::Update hook has no online check: while
 * enabled it re-locks any running clock through game+0x197aec0, which clears the network-sync flag +0x361, and turning it off releases the clock
 * without setting that flag back. So a client that joins with the lock on stays at its own time, whatever the session says (issue 71).
 * While a session exists the two settings are parked here in the plugin's memory, not in its ini, so the player's choice is back after
 * leaving, and a clock the plugin holds is released to the network clock: TimeOfDayComponent::Update (0x6f4ab0) applies it from then on.
 *
 * Two builds are known, with the same four globals at other offsets (eo_wc_layout):
 *   1.0.4, in Extra Options 0.4.4: the 0.4.4 zip has [TimeLock] Enabled=1, Hour=20:00; holding sets the clock's stop flag +0x24.
 *   1.1.0, in Extra Options 0.6.0 (EO_WeatherControl.dll sha256 8f8628c7...be67, read statically 2026-11-09): the zip has Enabled=0, and the
 *     lock can still be turned on from the free-roam pause menu (offered when the event context type is 2, online Free Roam included). It holds
 *     by setting the time again through game+0x197aec0 whenever it drifts (no +0x24), and when the lock goes off its hook drops the clock
 *     itself ("clock hold released; native timing retained"), +0x361 still cleared. Globals [C: its getters and TimeOfDay hook]: enabled 0xa6f0
 *     (getter 0x30c0), change counter 0xa6f4 (getter 0x3130, read by the hook each tick), weather mode 0xa6f8 (getter 0x3590, 0 = Dynamic),
 *     held clock 0xa718/0xa720 (last clock seen / the one it holds, hook 0x3890). */
#define FH2_NET_GAME_MANAGER_RVA 0x34d6348u

/* Where one build of the plugin keeps its settings (offsets in its image) */
struct eo_wc_layout {
    unsigned enabled, weather, generation, held_clock;
};
static const struct eo_wc_layout EO_WC_1_0_4 = {0xa6dcu, 0xa6e4u, 0xa6e0u, 0xa708u};
static const struct eo_wc_layout EO_WC_1_1_0 = {0xa6f0u, 0xa6f8u, 0xa6f4u, 0xa720u};

struct eo_lock {
    int parked, enabled, weather, clock_done;
};

enum { EO_LOCK_NONE, EO_LOCK_PARKED = 1, EO_LOCK_CLOCK_RELEASED = 2, EO_LOCK_RESTORED = 4 };

/* One step: wc = the plugin's image, at = its build's layout, session = the game has a session object, clock = the world's time-of-day manager
 * or NULL. -> EO_LOCK_* */
static int eo_lock_step(struct eo_lock *s, unsigned char *wc, const struct eo_wc_layout *at, int session, unsigned char *clock)
{
    int *enabled = (int *)(wc + at->enabled), *weather = (int *)(wc + at->weather), *generation = (int *)(wc + at->generation);
    unsigned char **held = (unsigned char **)(wc + at->held_clock);
    int e, w, did = EO_LOCK_NONE;

    if (!session) {
        if (s->parked) {
            if (s->enabled) __atomic_store_n(enabled, s->enabled, __ATOMIC_SEQ_CST);
            if (s->weather) __atomic_store_n(weather, s->weather, __ATOMIC_SEQ_CST);
            if (s->enabled || s->weather) {
                __atomic_add_fetch(generation, 1, __ATOMIC_SEQ_CST);
                did = EO_LOCK_RESTORED;
            }
            s->parked = s->enabled = s->weather = s->clock_done = 0;
        }
        return did;
    }
    s->parked = 1;
    e = __atomic_exchange_n(enabled, 0, __ATOMIC_SEQ_CST);        /* every step: the settings page can still turn them on */
    w = __atomic_exchange_n(weather, 0, __ATOMIC_SEQ_CST);
    if (e || w) {
        if (e) s->enabled = e;
        if (w) s->weather = w;
        __atomic_add_fetch(generation, 1, __ATOMIC_SEQ_CST);
        did = EO_LOCK_PARKED;
    }
    if (clock && !s->clock_done) {
        if (*held != clock) {
            s->clock_done = 1;
        } else if (!clock[0x25] && !clock[0x27] && !clock[0x28]) {    /* the game's own fixed time wins; SetTimeOfDay restores sync after it */
            *held = NULL;
            clock[0x24] = 0;
            clock[0x361] = 1;
            s->clock_done = 1;
            did |= EO_LOCK_CLOCK_RELEASED;
        }
    }
    return did;
}
