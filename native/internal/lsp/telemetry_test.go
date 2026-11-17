package lsp

import (
	"fmt"
	"strings"
	"testing"

	"fh2mp/internal/dbtest"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/scoreboard"
)

// telemHub answers the game server's telemetry query with a fixed reply and remembers what it was asked.
type telemHub struct {
	asked []string
	reply pyjson.Obj
}

func (h *telemHub) Populations() (map[string][]string, bool) { return nil, false }
func (h *telemHub) Control(string)                           {}
func (h *telemHub) Query(cmd string) pyjson.Obj              { h.asked = append(h.asked, cmd); return h.reply }
func (h *telemHub) Tools(string) string                      { return "" }

func TestTelemetryCheckStoresTheGameServersVerdict(t *testing.T) {
	t.Setenv("FH2MP_ENV", "dev")
	c := New()
	var lines []string
	c.Out = func(s string) { lines = append(lines, s) }
	if err := c.Setup(dbtest.New(t, "fh2mp_cat")); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { c.DB.Close() })
	xuid := int64(0x900000000000abc) // the shape of a real xuid (0x9000...)
	insert := func(st int64, score float64) *scoreboard.Stored {
		t.Helper()
		row := must(c.DB.One("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, abs, submitted) "+
			"VALUES (?, 9, ?, 17, 65535, 1062, 3, 0.677, ?, 0, 0, 0, 0, 1) RETURNING id", xuid, st, score))
		return &scoreboard.Stored{ID: row.Int("id"), Xuid: xuid, Sb: 9, St: st, Score: score}
	}
	verdictOf := func(id int64) (string, int64, string, string) {
		t.Helper()
		row := must(c.DB.One("SELECT verdict, version, reasons, measurements FROM score_verdicts WHERE submission_id=?", id))
		if row == nil {
			return "", 0, "", ""
		}
		return row.Str("verdict"), row.Int("version"), row.Str("reasons"), row.Str("measurements")
	}
	hub := &telemHub{}
	c.Xbl.Hub = hub
	r := &Req{Seq: 1, Xuid: xuid, Authed: true, Tag: "Tester"}

	// a trap result above everything the car did: stored as the game server judged it, next to ghost verdicts, and logged as a warning
	hub.reply = pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "verdict", V: "hard"},
		{K: "reasons", V: []any{"speed trap 360 km/h, the car never went over 288 km/h in the last 60 s"}},
		{K: "measures", V: pyjson.Obj{{K: "source", V: "telemetry"}, {K: "max_state_kmh", V: 288.0}}}}
	trap := insert(5, 100)
	c.telemetryCheck(r, trap)
	if want := fmt.Sprintf("telemetry %x trap 100.0000", uint64(xuid)); len(hub.asked) != 1 || hub.asked[0] != want {
		t.Fatalf("asked %q, want [%q]", hub.asked, want)
	}
	v, version, reasons, measures := verdictOf(trap.ID)
	if v != "hard" || version != telemVersion || !strings.Contains(reasons, "never went over 288 km/h") || !strings.Contains(measures, "telemetry") {
		t.Fatalf("stored %q v%d %s %s", v, version, reasons, measures)
	}
	if !strings.Contains(strings.Join(lines, ""), "speed trap result") || !strings.Contains(strings.Join(lines, ""), "-> hard") {
		t.Fatalf("no log line for the verdict: %q", lines)
	}

	// a zone result of a player who was not on the game server: nothing stored
	hub.reply = pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "verdict", V: "none"}, {K: "reasons", V: []any{"the player was not connected to this server at the time"}}}
	zone := insert(6, 50)
	c.telemetryCheck(r, zone)
	if !strings.HasSuffix(hub.asked[len(hub.asked)-1], " zone 50.0000") {
		t.Fatalf("zone asked as %q", hub.asked[len(hub.asked)-1])
	}
	if v, _, _, _ := verdictOf(zone.ID); v != "" {
		t.Fatalf("a no-data answer stored verdict %q", v)
	}

	// the game server not answering: nothing stored, the result stands
	hub.reply = nil
	quiet := insert(5, 70)
	c.telemetryCheck(r, quiet)
	if v, _, _, _ := verdictOf(quiet.ID); v != "" {
		t.Fatalf("no answer stored verdict %q", v)
	}

	// switched off: the game server is not asked
	t.Setenv("FH2MP_TELEMETRY", "off")
	before := len(hub.asked)
	c.telemetryCheck(r, insert(5, 60))
	if len(hub.asked) != before {
		t.Fatalf("asked %q with FH2MP_TELEMETRY=off", hub.asked[before:])
	}
}
