package lsp

import (
	"fmt"
	"os"
	"strings"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/scoreboard"
)

// telemVersion marks a telemetry verdict in score_verdicts (game.TelemVersion; ghost verdicts carry ghost.CheckVersion).
const telemVersion = 1001

// telemetryCheck asks the game server to judge a speed trap or speed zone result against the player's own live car stream
// (native/internal/game/telem.go) and stores the verdict where the ghost check keeps its verdicts (score_verdicts, version telemVersion),
// so the operator panel lists it. Log only: nothing is hidden. FH2MP_TELEMETRY=off skips it. Runs after the reply has gone out.
func (c *Core) telemetryCheck(r *Req, s *scoreboard.Stored) {
	defer func() {
		if p := recover(); p != nil {
			c.LogExc(r, fmt.Sprintf("  telemetry check of result %d failed after it was stored; the result stands unchecked", s.ID), fmt.Errorf("%v", p), diag.Warn)
		}
	}()
	if os.Getenv("FH2MP_TELEMETRY") == "off" || c.Xbl == nil {
		return
	}
	kind := "trap"
	if s.St == 6 {
		kind = "zone"
	}
	o := c.Xbl.HubTelemetry(s.Xuid, kind, s.Score)
	if o == nil {
		c.Log(r, fmt.Sprintf("  telemetry: speed %s result %d (%.0f km/h) of %#x not checked: the game server did not answer", kind, s.ID, s.Score*3.6, s.Xuid))
		return
	}
	verdict, _ := o.Get("verdict")
	v, _ := verdict.(string)
	var reasons []string
	if l, ok := o.Get("reasons"); ok {
		if list, ok := l.([]any); ok {
			for _, x := range list {
				if t, ok := x.(string); ok {
					reasons = append(reasons, t)
				}
			}
		}
	}
	why := "consistent with the car"
	if len(reasons) > 0 {
		why = strings.Join(reasons, "; ")
	}
	if v != "pass" && v != "flag" && v != "hard" {
		c.Log(r, fmt.Sprintf("  telemetry: speed %s result %d (%.0f km/h) of %#x not checked: %s", kind, s.ID, s.Score*3.6, s.Xuid, why))
		return
	}
	reasonsAny := make([]any, len(reasons))
	for i, t := range reasons {
		reasonsAny[i] = t
	}
	rj, _ := pyjson.Dumps(reasonsAny, pyjson.Opts{})
	measures := pyjson.Obj{}
	if m, ok := o.Get("measures"); ok {
		if mo, ok := m.(pyjson.Obj); ok {
			measures = mo
		}
	}
	mj, _ := pyjson.Dumps(measures, pyjson.Opts{})
	err := c.DB.Do(func(t *db.Tx) error {
		_, err := t.Exec("INSERT INTO score_verdicts (submission_id, version, verdict, reasons, measurements, decided_at) VALUES (?,?,?,?,?,?) "+
			"ON CONFLICT (submission_id) DO UPDATE SET version=excluded.version, verdict=excluded.verdict, reasons=excluded.reasons, "+
			"measurements=excluded.measurements, decided_at=excluded.decided_at", s.ID, telemVersion, v, rj, mj, nowF())
		return err
	})
	if err != nil {
		c.LogExc(r, fmt.Sprintf("  telemetry verdict %s of result %d not stored", v, s.ID), err, diag.Warn)
		return
	}
	level := diag.Info
	if v != "pass" {
		level = diag.Warn
	}
	c.LogAt(r, level, "", fmt.Sprintf("  telemetry: speed %s result %d (%.0f km/h) of %#x -> %s: %s", kind, s.ID, s.Score*3.6, s.Xuid, v, why))
}
