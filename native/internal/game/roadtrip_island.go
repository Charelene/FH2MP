package game

import "fmt"

// Storm Island Road Trip (session type 4). The game's own server code has a generator of its own for it,
// Network::RoadTrip::NetworkIslandRoadTripGenerator (vtable game+0x2d43398), next to the mainland one (game+0x2d433c8) [C]:
//   - the vote's three options are championships, not regions (draw game+0xa45230; the mainland's game+0xa457d0 draws regions 3..8):
//     option 0 is championship 0, options 1 and 2 two different ones of 1..4 other than the current one (generator +0x120, 0xff before
//     the first vote, game+0xa3c3a0); classes like the mainland's: a random one, the current one, a random one;
//   - championship 1..4 means race mode 32 RallyX, 33 RallyRush, 34 CrossCountryCircuit, 35 ExtremeCrossCountry (game+0xa41030; 37 = none);
//   - its events are Road Trip location 9 events (game+0xa40b30: RoadTripLocation == 9 and the championship's mode, King 25 and Infection 26
//     always allowed, championship 0 any mode), four of them: two of the championship's mode, then King or Infection, then one more of the
//     championship's mode (recipe game+0xa45f20); championship 0 uses the mainland recipe (game+0xa46180) on the island's events.
// The draw's output fills the same EventSelection LocationIds the mainland fills with regions, so the vote carries championship ids
// [I: same synced array, read by the same vote screen]; CurrentLocationId then holds the winning championship [I].

// RTIslandLocation is Storm Island's Road Trip location (AnthemOnlineEvents.RoadTripLocationId).
const RTIslandLocation = 9

// rtIslandChampionshipMode is the race mode of island championship 1..4 (game+0xa41030), 0 for championship 0 (any mode).
var rtIslandChampionshipMode = map[int64]int64{1: 32, 2: 33, 3: 34, 4: 35}

// rtIslandArenaModes are King and Infection, the one arena event of a championship.
var rtIslandArenaModes = []int64{RaceModeKing, RaceModeInfection}

// PickIslandRoadTripCandidates draws Storm Island's vote: three championships with four location 9 events each. current is the
// championship the group just played (-1 before the first vote).
func PickIslandRoadTripCandidates(rng Rand, gamedbPath string, players int, current int64) (*RtCand, error) {
	db, err := openRoadTripDB(gamedbPath)
	if err != nil {
		return nil, err
	}
	defer db.Close()
	rows, err := db.Query("SELECT e.AnthemOnlineEventId, e.RaceModeId, m.MinimumPlayers FROM AnthemOnlineEvents e JOIN RaceModes m ON m.id = e.RaceModeId "+
		"WHERE e.RoadTripLocationId = ? AND m.MinimumPlayers <= ? AND e.RaceModeId NOT IN (-1) ORDER BY e.AnthemOnlineEventId", RTIslandLocation, maxInt(1, players))
	if err != nil {
		return nil, err
	}
	byMode := map[int64][]int64{}
	var modes []int64
	minPlayers := map[int64]int64{}
	for rows.Next() {
		var ev, mode, minp int64
		if err := rows.Scan(&ev, &mode, &minp); err != nil {
			rows.Close()
			return nil, err
		}
		if _, ok := byMode[mode]; !ok {
			modes = append(modes, mode)
		}
		byMode[mode] = append(byMode[mode], ev)
		minPlayers[ev] = minp
	}
	rows.Close()
	if err := rows.Err(); err != nil {
		return nil, err
	}

	var champs []int64
	for c := int64(1); c <= 4; c++ {
		if c != current && len(byMode[rtIslandChampionshipMode[c]]) >= 3 {
			champs = append(champs, c)
		}
	}
	if len(champs) < 2 {
		return nil, fmt.Errorf("island road trip: %d championship(s) with 3 events at location %d (current %d), need 2", len(champs), RTIslandLocation, current)
	}
	all := 0
	for _, m := range modes {
		all += len(byMode[m])
	}
	if all < 4 {
		return nil, fmt.Errorf("island road trip: %d event(s) at location %d, need 4", all, RTIslandLocation)
	}
	picked := sample(rng, champs, 2)
	out := &RtCand{Locs: [3]int64{0, picked[0], picked[1]}}
	for i, c := range out.Locs {
		var evs []int64
		if c == 0 {
			evs = islandMixedEvents(rng, modes, byMode)
		} else {
			evs = islandChampionshipEvents(rng, rtIslandChampionshipMode[c], byMode)
		}
		for k := 0; k < 4; k++ {
			out.Events[i][k] = evs[k]
			out.Mins[i][k] = int(minPlayers[evs[k]])
		}
	}
	classes := sample(rng, []int{0, 1, 2, 3, 4, 5}, 3)
	copy(out.Classes[:], classes)
	return out, nil
}

// islandChampionshipEvents: two of the championship's mode, King or Infection, one more of its mode (game+0xa45f20). Without an arena
// event for this many players the third is one more of the mode as well.
func islandChampionshipEvents(rng Rand, mode int64, byMode map[int64][]int64) []int64 {
	pool := sample(rng, byMode[mode], len(byMode[mode]))
	out := []int64{pool[0], pool[1]}
	var arena []int64
	for _, m := range rtIslandArenaModes {
		if len(byMode[m]) > 0 {
			arena = append(arena, m)
		}
	}
	if len(arena) > 0 && len(pool) >= 3 {
		m := arena[rng.Intn(len(arena))]
		out = append(out, byMode[m][rng.Intn(len(byMode[m]))], pool[2])
	} else if len(pool) >= 4 {
		out = append(out, pool[2], pool[3])
	} else {
		out = append(out, pool[2], pool[0]) // only three events of this mode for this many players: one is driven twice
	}
	return out
}

// islandMixedEvents is championship 0: like the mainland draw, one event of each mode in a random mode order until there are four.
func islandMixedEvents(rng Rand, modes []int64, byMode map[int64][]int64) []int64 {
	pools := map[int64][]int64{}
	for _, m := range modes {
		pools[m] = sample(rng, byMode[m], len(byMode[m]))
	}
	var out []int64
	for len(out) < 4 {
		for _, m := range sample(rng, modes, len(modes)) {
			if v := pools[m]; len(v) > 0 && len(out) < 4 {
				out = append(out, v[len(v)-1])
				pools[m] = v[:len(v)-1]
			}
		}
	}
	return out
}

// rtPickCandidates draws the next vote: regions on the mainland, championships on Storm Island.
func (w *World) rtPickCandidates(rng Rand, players int) (*RtCand, error) {
	if w.Island {
		current := int64(-1)
		if w.RtLastLoc != nil {
			current = *w.RtLastLoc
		}
		return PickIslandRoadTripCandidates(rng, w.RtGamedb, players, current)
	}
	return PickRoadTripCandidates(rng, w.RtGamedb, players)
}

// rtOptionsWord names what the vote offers in the log: regions ("locations") or the island's championships.
func (w *World) rtOptionsWord() string {
	if w.Island {
		return "championships"
	}
	return "locations"
}

// rtOptionWord is one option of the vote in the log: a region ("location") or an island championship.
func (w *World) rtOptionWord() string {
	if w.Island {
		return "championship"
	}
	return "location"
}

// rtDriveMs is how long the group gets to drive to the next event. Storm Island is one region: every drive is a region leg.
func (w *World) rtDriveMs(from, to *int64) int64 {
	if w.Island {
		leg := int64(RTIslandLocation)
		return RoadTripDriveMs(&leg, &leg)
	}
	return RoadTripDriveMs(from, to)
}
