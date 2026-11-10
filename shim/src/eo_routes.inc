/* FH2 1.0.0.64 with FH2 Extra Options 0.6.0: online, no Free Roam event shows on the map and none can be started (reported 2026-11-10).
 *
 * The event starts that online Free Roam puts on the map are the game's route activation entities. Each one listens for a FeatureToggle name,
 * online_route_activations (mainland) or online_island_route_activations (Storm Island), and the game's feature states switch those two names
 * on and off with FeatureEnabled/FeatureDisabled script messages [C: decompile, strings at 0x1e86430/0x1e86450, used by 0x7cab60, 0x865690,
 * 0x8991b0, 0x8a4390, 0x8bb0b0]:
 *   ApplyFeatureStateToGame (0x8991b0, offline) through SetFeature 0x899010, calls at 0x899335 (mainland) and 0x899378 (island), value 0;
 *   NetworkApplyFeatureStateToGame (0x8bb0b0, online) through its own helper 0x8ba180, value 1 when online events are allowed;
 *   ApplyFeatureStateToGameForEvent (0x865690) through helper 0x864680, value 0;
 *   IslandIEApplyFeatureStateToGame (0x8a4390) through helper 0x8a3a80, value 0.
 * Extra Options 0.6.0 ("Show All Events", source fh2/offline_events.c at RokkuDayo/ForzaHorizon2_ExtraOptions 2dc7bc9) renames every route
 * activation as it is built (call 0x7cacea -> its route_feature) to a gate of its own, EO_MainlandRoute_Activation_<route> /
 * EO_IslandRoute_Activation_<route>, and switches those gates itself each tick. Without Show All Events, and during a network session, a gate
 * follows what the game last set for its map's feature; Extra Options learns that only from the two offline calls, which it points at its
 * set_event_feature (thunk at 0x816f04). The network state's helper never reaches it, so online the gates stay off: no marker, no start.
 * [C: the source and the decompile; that this is the whole cause is inferred, not yet seen in a game.]
 *
 * The fix: after each of the other three states' helpers sets one of the two names, the same name and value also go to whatever the offline
 * call site calls now. When Extra Options redirected it, that is its set_event_feature, which records the value and passes it on to SetFeature
 * itself, exactly as on the offline path; its next tick switches its gates to match. When nothing redirected the call (no Extra Options, or a
 * build that does not rename the routes) the site still calls SetFeature and nothing more is done, so vanilla behaviour is untouched.
 * FH2MP_NO_EOROUTES=1 (or the session switch NO_EOROUTES) turns it off. */
#define EOR_SET_FEATURE_RVA 0x899010u        /* SetFeature(const std::string *name, bool on): the game event bus */
#define EOR_MAINLAND_CALL_RVA 0x899335u      /* ApplyFeatureStateToGame: SetFeature("online_route_activations", 0) */
#define EOR_ISLAND_CALL_RVA 0x899378u        /* ApplyFeatureStateToGame: SetFeature("online_island_route_activations", 0) */
#define EOR_NETWORK_HELPER_RVA 0x8ba180u     /* NetworkApplyFeatureStateToGame's FeatureEnabled/FeatureDisabled helper */
#define EOR_EVENT_HELPER_RVA 0x864680u       /* ApplyFeatureStateToGameForEvent's */
#define EOR_ISLAND_IE_HELPER_RVA 0x8a3a80u   /* IslandIEApplyFeatureStateToGame's */

static const char EOR_MAINLAND[] = "online_route_activations";
static const char EOR_ISLAND[] = "online_island_route_activations";

/* The game's std::string (MSVC x64): the text inline while capacity < 16, else a pointer; then length and capacity. */
struct eor_str {
    union {
        char text[16];
        const char *ptr;
    } u;
    unsigned long long length, capacity;
};

static const char *eor_str_text(const struct eor_str *s)
{
    return s->capacity < 16 ? s->u.text : s->u.ptr;
}

static int eor_text_is(const char *text, unsigned long long length, const char *name, unsigned long long name_length)
{
    unsigned long long i;
    if (!text || length != name_length) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        if (text[i] != name[i]) {
            return 0;
        }
    }
    return 1;
}

/* Which route feature `s` names: 0 mainland, 1 Storm Island, -1 another feature. */
static int eor_route_group(const struct eor_str *s)
{
    const char *text;
    if (!s || s->length > 64) {
        return -1;
    }
    text = eor_str_text(s);
    if (eor_text_is(text, s->length, EOR_MAINLAND, sizeof EOR_MAINLAND - 1)) {
        return 0;
    }
    if (eor_text_is(text, s->length, EOR_ISLAND, sizeof EOR_ISLAND - 1)) {
        return 1;
    }
    return -1;
}

/* A string the game reads but never frees or changes (Extra Options passes its gate names to SetFeature the same way). */
static void eor_borrow(struct eor_str *s, int group)
{
    const char *name = group ? EOR_ISLAND : EOR_MAINLAND;
    unsigned long long n = group ? sizeof EOR_ISLAND - 1 : sizeof EOR_MAINLAND - 1;
    unsigned i;
    for (i = 0; i < sizeof s->u.text; i++) {
        s->u.text[i] = 0;
    }
    s->u.ptr = name;
    s->length = n;
    s->capacity = n < 16 ? 16 : n;
}

/* Where the call rel32 at `site` (its five bytes in `code`) goes, or 0 when the bytes are not one. */
static unsigned long long eor_call_target(const unsigned char code[5], unsigned long long site)
{
    unsigned rel;
    if (code[0] != 0xE8) {
        return 0;
    }
    rel = (unsigned)code[1] | (unsigned)code[2] << 8 | (unsigned)code[3] << 16 | (unsigned)code[4] << 24;
    return site + 5 + (unsigned long long)(long long)(int)rel;
}

/* What to call with (name, value) for route group `group`, given the bytes at the offline call site: its target when someone redirected it,
 * 0 when it still calls SetFeature (nothing to pass on) or is not a call any more (unknown change: leave it). */
static unsigned long long eor_forward_target(const unsigned char code[5], unsigned long long site, unsigned long long set_feature)
{
    unsigned long long to = eor_call_target(code, site);
    return to && to != set_feature ? to : 0;
}
