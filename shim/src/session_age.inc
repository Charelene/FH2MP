/* How old a launcher session file may be when a game process starts (FILETIME ticks, 100 ns).
 * The launcher writes fh2mp-session.txt at Play and removes it when the game exits, but a launcher that was closed or crashed leaves it, and
 * a token stays valid on the server for 12 hours after its last use: prod 2026-10-29, 256 of 530 players refused for an outdated shim ran a
 * current launcher and had started the game without Play (directly, or restarted from in-game), on the session an earlier Play left behind.
 * A game process now only goes online on a session written shortly before it started; anything older is ignored (the player is offline,
 * with their own car database), removed, and the notice tells them to press Play. */
#define SESSION_MAX_AGE_TICKS (30ULL * 60ULL * 10000000ULL)

/* written/now are FILETIME values as 64-bit; max_age 0 = no limit (rigs, FH2MP_SESSION_MAX_AGE_S=0). An unreadable time (0) is not stale:
 * a file the shim cannot date is the launcher's business. A file dated in the future is as suspect as an old one. */
static int session_is_stale(unsigned long long written, unsigned long long now, unsigned long long max_age)
{
    unsigned long long age;
    if (!max_age || !written || !now) {
        return 0;
    }
    age = now >= written ? now - written : written - now;
    return age > max_age;
}
