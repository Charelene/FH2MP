package game

import (
	"database/sql"
	"os"
	"path/filepath"
	"testing"

	"fh2mp/internal/pyrand"
)

// islandEventDB is the island part of the game's own event table (Road Trip location 9, build 1.0.0.64), plus a mainland row that must
// never be drawn.
func islandEventDB(t *testing.T) string {
	t.Helper()
	db := filepath.Join(t.TempDir(), "gamedb.slt")
	conn, err := sql.Open("sqlite", "file:"+filepath.ToSlash(db))
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	for _, q := range []string{
		"CREATE TABLE AnthemOnlineEvents (AnthemOnlineEventId INTEGER, RoadTripLocationId INTEGER, TrackId INTEGER, NumLaps INTEGER, RaceModeId INTEGER)",
		"CREATE TABLE RaceModes (id INTEGER, Name TEXT, Enum TEXT, RaceModeId INTEGER, MinimumPlayers INTEGER)",
		"INSERT INTO RaceModes VALUES (0, '', '', 0, 1), (25, '', '', 0, 2), (26, '', '', 0, 2), (32, '', '', 0, 1), (33, '', '', 0, 1), (34, '', '', 0, 1), (35, '', '', 0, 1), (36, '', '', 0, 1)",
		"INSERT INTO AnthemOnlineEvents VALUES (177, 9, 1030, 1, 25), (200, 9, 1056, 1, 25), (202, 9, 1057, 1, 25), (208, 9, 1060, 1, 25), " +
			"(176, 9, 1030, 1, 26), (199, 9, 1056, 1, 26), (201, 9, 1057, 1, 26), (207, 9, 1060, 1, 26), " +
			"(170, 9, 1027, 3, 32), (174, 9, 1034, 2, 32), (178, 9, 1033, 2, 32), (179, 9, 1035, 3, 32), (180, 9, 1028, 3, 32), " +
			"(171, 9, 1044, 1, 33), (175, 9, 1042, 1, 33), (184, 9, 1040, 1, 33), (185, 9, 1041, 1, 33), (186, 9, 1029, 1, 33), (187, 9, 1043, 1, 33), " +
			"(172, 9, 1050, 3, 34), (188, 9, 1051, 3, 34), (189, 9, 1052, 3, 34), (190, 9, 1053, 3, 34), (191, 9, 1054, 3, 34), " +
			"(173, 9, 1038, 1, 35), (181, 9, 1039, 1, 35), (182, 9, 1036, 1, 35), (183, 9, 1037, 1, 35), " +
			"(192, 9, 1032, 1, 36), (193, 9, 1045, 1, 36), (194, 9, 1046, 1, 36), (195, 9, 1047, 1, 36), (196, 9, 1048, 1, 36), (197, 9, 1049, 1, 36), " +
			"(34, 4, 862, 3, 0)",
	} {
		if _, err := conn.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	return db
}

func islandEventModes(t *testing.T, db string) map[int64]int64 {
	t.Helper()
	conn, err := sql.Open("sqlite", "file:"+filepath.ToSlash(db)+"?mode=ro")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	rows, err := conn.Query("SELECT AnthemOnlineEventId, RaceModeId FROM AnthemOnlineEvents WHERE RoadTripLocationId = 9")
	if err != nil {
		t.Fatal(err)
	}
	defer rows.Close()
	out := map[int64]int64{}
	for rows.Next() {
		var ev, m int64
		rows.Scan(&ev, &m)
		out[ev] = m
	}
	return out
}

func TestIslandRoadTripVoteIsChampionships(t *testing.T) {
	db := islandEventDB(t)
	mode := islandEventModes(t, db)
	for seed := uint64(1); seed <= 200; seed++ {
		for _, c := range []struct {
			players int
			current int64
		}{{1, -1}, {4, -1}, {4, 2}} {
			cand, err := PickIslandRoadTripCandidates(pyrand.New(seed), db, c.players, c.current)
			if err != nil {
				t.Fatalf("seed %d: %v", seed, err)
			}
			if cand.Locs[0] != 0 {
				t.Fatalf("seed %d: option 0 is championship %d, want 0", seed, cand.Locs[0])
			}
			if cand.Locs[1] == cand.Locs[2] || cand.Locs[1] < 1 || cand.Locs[1] > 4 || cand.Locs[2] < 1 || cand.Locs[2] > 4 {
				t.Fatalf("seed %d: options %v, want two different championships of 1..4", seed, cand.Locs)
			}
			if cand.Locs[1] == c.current || cand.Locs[2] == c.current {
				t.Fatalf("seed %d: the current championship %d is offered again: %v", seed, c.current, cand.Locs)
			}
			for i := 0; i < 3; i++ {
				seen := map[int64]bool{}
				arena := 0
				for k, ev := range cand.Events[i] {
					m, ok := mode[ev]
					if !ok {
						t.Fatalf("seed %d: event %d is not a Storm Island event", seed, ev)
					}
					if seen[ev] {
						t.Fatalf("seed %d: option %d has event %d twice: %v", seed, i, ev, cand.Events[i])
					}
					seen[ev] = true
					if m == RaceModeKing || m == RaceModeInfection {
						arena++
						if c.players < 2 {
							t.Fatalf("seed %d: arena event %d drawn for %d player", seed, ev, c.players)
						}
						if i > 0 && k != 2 {
							t.Fatalf("seed %d: championship %d has its arena event at %d, want the third", seed, cand.Locs[i], k)
						}
						continue
					}
					if i > 0 && m != rtIslandChampionshipMode[cand.Locs[i]] {
						t.Fatalf("seed %d: championship %d holds event %d of mode %d", seed, cand.Locs[i], ev, m)
					}
				}
				if i > 0 && c.players >= 2 && arena != 1 {
					t.Fatalf("seed %d: championship %d has %d arena events, want 1", seed, cand.Locs[i], arena)
				}
			}
		}
	}
}

// An island Road Trip world draws championships at creation and drives island legs; a mainland one is unchanged.
func TestIslandRoadTripWorld(t *testing.T) {
	db := islandEventDB(t)
	t.Setenv("FH2MP_GAMEDB", db)
	hub := newIslandHub(t)
	w := hub.WorldFor(LobbyTypeIslandRoadTrip, "rt-isl")
	if !w.Roadtrip || !w.Island || w.RtCandidates == nil {
		t.Fatalf("island Road Trip world: roadtrip %v island %v candidates %v", w.Roadtrip, w.Island, w.RtCandidates)
	}
	if w.RtCandidates.Locs[0] != 0 {
		t.Fatalf("first island vote %v, want championship 0 first", w.RtCandidates.Locs)
	}
	three, four := int64(3), int64(4)
	if got, want := w.rtDriveMs(&three, &four), RoadTripDriveMs(&three, &three); got != want {
		t.Fatalf("island drive %d ms, want one region leg %d ms", got, want)
	}
	if _, err := os.Stat(db); err != nil {
		t.Fatal(err)
	}
}
