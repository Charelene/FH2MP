/* FH2 Extra Options' Weather Control plugin (EO_WeatherControl.dll 1.0.4, EO 0.4.4) holds the free-roam clock and weather. The 0.4.4 zip the launcher
 * pins has [TimeLock] Enabled=1, Hour=20:00 in its ini. The plugin's TimeOfDay::Update hook has no online check: while enabled it re-locks any running clock through
 * game+0x197aec0, which clears the network-sync flag +0x361, and turning it off starts the clock again without setting that flag back.
 * So a client that joins with the lock on stays at its own time, whatever the session says (issue 71).
 * While a session exists the two settings are parked here in the plugin's memory, not in its ini, so the player's choice is back after
 * leaving, and a clock the plugin holds is released to the network clock: TimeOfDayComponent::Update (0x6f4ab0) applies it from then on. */
#define EO_WC_WEATHER 0xa6e4u
#define EO_WC_ENABLED 0xa6dcu
#define EO_WC_GENERATION 0xa6e0u
#define EO_WC_HELD_CLOCK 0xa708u
#define FH2_NET_GAME_MANAGER_RVA 0x34d6348u

struct eo_lock {
    int parked, enabled, weather, clock_done;
};

enum { EO_LOCK_NONE, EO_LOCK_PARKED = 1, EO_LOCK_CLOCK_RELEASED = 2, EO_LOCK_RESTORED = 4 };

/* One step: wc = the plugin's image, session = the game has a session object, clock = the world's time-of-day manager or NULL. -> EO_LOCK_* */
static int eo_lock_step(struct eo_lock *s, unsigned char *wc, int session, unsigned char *clock)
{
    int *enabled = (int *)(wc + EO_WC_ENABLED), *weather = (int *)(wc + EO_WC_WEATHER), *generation = (int *)(wc + EO_WC_GENERATION);
    unsigned char **held = (unsigned char **)(wc + EO_WC_HELD_CLOCK);
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
