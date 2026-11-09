package game

import (
	"database/sql"
	"path/filepath"
	"strings"
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/gameopts"
)

// optionsWith builds a GameOptions payload with an end condition and a lap count (the stock game's default: laps end condition, 3 laps).
func optionsWith(t *testing.T, end, laps uint64) (int, bits.Int) {
	t.Helper()
	size, payload, err := gameopts.EncodeFields(map[int]gameopts.Field{
		30:                      {Width: 8, Value: bits.FromUint64(255)},
		gameopts.EndConditionID: {Width: 3, Value: bits.FromUint64(end)},
		gameopts.NumLapsID:      {Width: 32, Value: bits.FromUint64(laps)},
	})
	if err != nil {
		t.Fatal(err)
	}
	return size, payload
}

func lapsOf(t *testing.T, size int, payload bits.Int) (end, laps uint64) {
	t.Helper()
	f, err := gameopts.Fields(size, payload)
	if err != nil {
		t.Fatal(err)
	}
	return f[gameopts.EndConditionID].Value.Uint64(), f[gameopts.NumLapsID].Value.Uint64()
}

// The host's game writes 3 laps into every Road Trip event; the game database's lap count is what the event has (1 for a sprint). Tag modes keep theirs.
func TestRoadTripLapsFromTheGameDatabase(t *testing.T) {
	r := newLoadRig(t, 1)
	db := filepath.Join(t.TempDir(), "gamedb.slt")
	conn, err := sql.Open("sqlite", "file:"+filepath.ToSlash(db))
	if err != nil {
		t.Fatal(err)
	}
	for _, q := range []string{
		"CREATE TABLE AnthemOnlineEvents (AnthemOnlineEventId INTEGER, RoadTripLocationId INTEGER, TrackId INTEGER, NumLaps INTEGER, RaceModeId INTEGER)",
		"INSERT INTO AnthemOnlineEvents VALUES (34, 4, 862, 3, 0), (51, 4, 895, 1, 31), (60, 4, 900, 2, 0), (70, 4, 901, 1, 25)",
	} {
		if _, err := conn.Exec(q); err != nil {
			t.Fatal(err)
		}
	}
	conn.Close()
	w := r.w
	w.Roadtrip, w.RtGamedb, w.RtWinner = true, db, 0
	w.RtCandidates = &RtCand{}
	set := func(race int64) { w.RtCandidates.Events[0][0] = race }
	port := 40000

	set(34) // a 3-lap circuit: already right, nothing to do
	size, payload := optionsWith(t, 1, 3)
	if s, p, c := w.rtFixLaps(port, size, payload); c != nil || s != size || !p.Equal(payload) {
		t.Fatal("a 3-lap event with 3 laps was changed")
	}
	set(51) // a sprint: 1 lap
	size, payload = optionsWith(t, 1, 3)
	s, p, c := w.rtFixLaps(port, size, payload)
	if c == nil {
		t.Fatal("a sprint with the default 3 laps was not corrected")
	}
	if end, laps := lapsOf(t, s, p); end != 1 || laps != 1 {
		t.Fatalf("sprint options: end condition %d, laps %d (want 1, 1)", end, laps)
	}
	if f, err := gameopts.Fields(c.Size, c.Payload); err != nil || f[gameopts.NumLapsID].Value.Uint64() != 1 {
		t.Fatalf("the delta for the host does not carry the 1 lap: %v", err)
	}
	if r.count("event 51 said end condition 1 and NumLaps 3; the game database says 1 lap(s)") != 1 {
		t.Fatalf("no correction line: %q", r.log)
	}
	set(60) // a 2-lap event with no end condition and a zero count (what an unset lap total looks like)
	size, payload = optionsWith(t, 0, 0)
	s, p, c = w.rtFixLaps(port, size, payload)
	if end, laps := lapsOf(t, s, p); c == nil || end != 1 || laps != 2 {
		t.Fatalf("None end condition / 0 laps became end %d, laps %d (want 1, 2)", end, laps)
	}
	set(70) // a tag mode: left alone
	size, payload = optionsWith(t, 2, 0)
	if _, _, c := w.rtFixLaps(port, size, payload); c != nil {
		t.Fatal("a tag event's options were changed")
	}
	set(999) // not in the database: the road trip code assumes 1 lap; the mode is unknown, so nothing is touched
	size, payload = optionsWith(t, 1, 3)
	if _, _, c := w.rtFixLaps(port, size, payload); c != nil {
		t.Fatal("an event that is not in the game database was changed")
	}
	w.RtGamedb = "" // no game database: nothing to compare
	set(51)
	if _, _, c := w.rtFixLaps(port, size, payload); c != nil {
		t.Fatal("options were changed without a game database")
	}
	if strings.Contains(strings.Join(r.log, "\n"), "cannot be read") {
		t.Fatalf("readable options were reported as unreadable: %q", r.log)
	}
}
