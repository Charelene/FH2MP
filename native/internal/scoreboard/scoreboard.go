// Package scoreboard stores the scores players submit (Scoreboard.SubmitScore / SubmitXpScore) and serves them back through GetBestScore /
// GetRival / GetRowsAnthem (= server/scoreboard.py). Boards are keyed (scoreboardType, scoreType, trackId); `score` is a big-endian IEEE double
// carried as a u64 on the wire. Boards can be topped up with deterministic fake rivals when FH2MP_FAKE_ROWS > 0 (default 0: real players only).
// The ghost of a run is stored with it and judged (package ghost); what the boards show is decided by hidden_scores (package scorereports).
package scoreboard

import (
	"crypto/rand"
	"crypto/sha1"
	"encoding/binary"
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"

	"fh2mp/internal/cardb"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/ghost"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/scorereports"
	"fh2mp/internal/xpaudit"
)

var (
	log  = diag.Get("scores")
	rate = diag.NewRateLimit(nil, 0)
)

// Urandom makes the scoreId we issue (tests replace it).
var Urandom = func(n int) []byte {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return b
}

// Now is the clock (tests replace it).
var Now = func() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// Constants of the boards.
const (
	XPBudgetLogSeconds = 600.0
	MaxRouteSpeed      = SpeedCap
	BigImprovement     = 0.6 // a new best under this share of the old one, same car and PI. Honest gains are far smaller.
	// SpeedCap is the highest speed a trap can really show, m/s (285 mph).
	SpeedCap     = 127.4
	ClassPISlack = 0.002
	WRFloor      = 1.005 // made-up rival times stay at least this far above a world record
	WRSpread     = 0.12
	FakeBase     = int64(0x9100000000000)
	TargetXuid   = FakeBase + 0xFFFFFF // the empty board's world record to beat (RivalFor); outside every FakeXuid value
	TargetTag    = "World Record"
)

// Board sets.
var (
	Ascending      = map[int64]bool{1: true} // scoreTypes where a smaller score wins (lap/race times) [guess]
	ProgressLimits = map[[2]int64]int64{{4, 9}: 150, {4, 10}: 316}
	EventBoards    = map[int64]bool{3: true, 6: true}
	MaxedClasses   = []int64{6, 7} // X, R: never allowed on a Rivals board (user decision)
	ClassPivot     = map[int64]bool{1: true}
	SpeedBoards    = map[int64]bool{9: true}
	GsidBoards     = map[int64]bool{9: true}
	GhostBoards    = map[int64]bool{1: true, 3: true, 6: true}
	ClassAvgKmh    = [6]float64{88, 103, 119, 134, 150, 166}
	ClassTopPI     = [8]float64{0.4345, 0.525628, 0.6179, 0.748432, 0.846368, 0.997086, 1.0, 1.1}
	// FakeRows tops a board up with made-up rivals (env FH2MP_FAKE_ROWS; 0 = real players only).
	FakeRows        = envInt("FH2MP_FAKE_ROWS", 0)
	defaultPolicy   = envStr("FH2MP_GHOST_POLICY", "log")
	maxedClassesSQL = "6,7"
)

func envInt(name string, def int) int {
	if v := os.Getenv(name); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}

func envStr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

// DblToU64 is the wire form of a score.
func DblToU64(x float64) uint64 { return math.Float64bits(x) }

// U64ToDbl is the score of a wire value.
func U64ToDbl(u uint64) float64 { return math.Float64frombits(u) }

// Iso formats a time like the Python helper.
func Iso(t float64) string {
	return time.Unix(int64(math.Floor(t)), 0).UTC().Format("2006-01-02T15:04:05Z")
}

// EventInfo is a Monthly Rivals event: its track, laps and class.
type EventInfo struct {
	Track, Laps int64
	Cls         *int64
	CarClassID  *int64 // the event's class restriction (attrs.CarClassId)
}

// Row is a board row: a stored submission (or a made-up rival) as the rest of the server uses it.
type Row struct {
	scorereports.Flags
	ID                                                 int64
	Xuid                                               int64
	Tag                                                string
	Sb, St, Gsid, Track, Car, Cls                      int64
	Cpi, Score                                         float64
	Clean, Stm, Tcs, Abs, Autobrake, Autoshift, Clutch bool
	T                                                  float64
	Ghost                                              int64
	Fake                                               bool
	Position                                           int
}

func rowOf(r *db.Row) *Row {
	out := &Row{Flags: scorereports.FieldsOf(r), ID: r.Int("id"), Xuid: r.Int("xuid"), Tag: r.Str("tag"), Sb: r.Int("sb"), St: r.Int("st"), Gsid: r.Int("gsid"),
		Track: r.Int("track"), Car: r.Int("car"), Cls: r.Int("cls"), Cpi: r.Float("cpi"), Score: r.Float("score"), Clean: r.Int("clean") != 0,
		Stm: r.Int("stm") != 0, Tcs: r.Int("tcs") != 0, Abs: r.Int("abs") != 0, Autobrake: r.Int("autobrake") != 0, Autoshift: r.Int("autoshift") != 0,
		Clutch: r.Int("clutch") != 0, T: r.Float("submitted")}
	if r.Has("ghost") {
		out.Ghost = r.Int("ghost")
	}
	return out
}

// Store is the score store.
type Store struct {
	DB *db.DB
	// Hooks the LSP fills in.
	RouteLength func(track int64) (float64, bool) // metres
	EventInfo   func(event int64) *EventInfo      // nil = unknown
	EventCars   func(event int64) []cardb.PoolCar // the cars the event allows
	EventCarIDs func(event int64) map[int64]bool  // nil func: derived from EventCars
	Records     map[[2]int]float64                // (track, class) -> record seconds
	Names       []string
	Cars        []cardb.PoolCar // by PI
	GhostPolicy string
	fakeNames   map[int64]string
}

// New makes a store. names are the Drivatar names fake rivals borrow; cars the game's car pool.
func New(d *db.DB, names []string, cars []cardb.PoolCar) *Store {
	seen := map[string]bool{}
	var uniq []string
	for _, n := range names {
		if n != "" && !seen[n] {
			seen[n] = true
			uniq = append(uniq, n)
		}
	}
	if len(uniq) == 0 {
		uniq = []string{"Rival"}
	}
	cs := append([]cardb.PoolCar(nil), cars...)
	sort.SliceStable(cs, func(i, j int) bool { return cs[i].PI < cs[j].PI })
	return &Store{DB: d, Names: uniq, Cars: cs, GhostPolicy: defaultPolicy}
}

func (s *Store) routeLength(track int64) (float64, bool) {
	if s.RouteLength == nil {
		return 0, false
	}
	return s.RouteLength(track)
}

func (s *Store) eventInfo(event int64) *EventInfo {
	if s.EventInfo == nil {
		return nil
	}
	return s.EventInfo(event)
}

func (s *Store) eventCars(event int64) []cardb.PoolCar {
	if s.EventCars == nil {
		return nil
	}
	return s.EventCars(event)
}

// Count is the number of stored submissions.
func (s *Store) Count() (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM score_submissions")
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// ---- writes

// EventClass is the event restriction, not its illustrative card class (open events have a card class too); nil = none.
func (s *Store) EventClass(sb, eventID int64) *int64 {
	if !EventBoards[sb] {
		return nil
	}
	if e := s.eventInfo(eventID); e != nil {
		return e.CarClassID
	}
	return nil
}

// Submission is the decoded Scoreboard.SubmitScore call.
type Submission struct {
	Score                                                        uint64
	ScoreboardType, GameScoreboardID, TrackID, CarID, CarClassID int64
	CarPerformanceIndex                                          float64
	IsClean, UsedSTM, UsedTCS, UsedABS                           bool
	UsedAutoBrake, UsedAutoShifting, UsedClutch                  bool
}

func b2i(b bool) int64 {
	if b {
		return 1
	}
	return 0
}

// Stored is what Submit returns.
type Stored struct {
	ID      int64
	ScoreID []byte
	Xuid    int64
	Tag     string
	Sb, St  int64
	Score   float64
}

// Submit stores one submission and returns it, or nil if the score is not accepted (see Refusal).
func (s *Store) Submit(xuid int64, tag string, scoreType int64, d *Submission) (*Stored, error) {
	score := U64ToDbl(d.Score)
	if why := s.Refusal(scoreType, d, score); why != "" {
		log.Warn("SubmitScore refused: " + why + " " + diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)}, diag.KVP{K: "board", V: fmt.Sprintf("(%d,%d)", d.ScoreboardType, scoreType)},
			diag.KVP{K: "gsid", V: d.GameScoreboardID}, diag.KVP{K: "track", V: d.TrackID}, diag.KVP{K: "car", V: d.CarID}, diag.KVP{K: "cls", V: d.CarClassID},
			diag.KVP{K: "pi", V: fmt.Sprintf("%.4f", d.CarPerformanceIndex)}))
		return nil, nil
	}
	now := Now()
	scoreID := Urandom(16)
	expectsGhost := s.GhostPolicy == "enforce" && GhostBoards[d.ScoreboardType] && Ascending[scoreType]
	var id int64
	err := s.DB.Do(func(t *db.Tx) error {
		if err := db.TouchPlayer(t, xuid, tag, &now, nil); err != nil {
			return err
		}
		res, err := t.Exec("INSERT INTO score_submissions (xuid, sb, st, gsid, track, car, cls, cpi, score, clean, stm, tcs, "+
			"abs, autobrake, autoshift, clutch, submitted, score_id) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
			xuid, d.ScoreboardType, scoreType, d.GameScoreboardID, d.TrackID, d.CarID, d.CarClassID, d.CarPerformanceIndex, score, b2i(d.IsClean), b2i(d.UsedSTM),
			b2i(d.UsedTCS), b2i(d.UsedABS), b2i(d.UsedAutoBrake), b2i(d.UsedAutoShifting), b2i(d.UsedClutch), now, scoreID)
		if err != nil {
			return err
		}
		id = res.LastID
		if expectsGhost { // public once its ghost has passed (user decision); its owner sees it meanwhile
			_, err = t.Exec("INSERT INTO hidden_scores (submission_id, hidden_at, source, reason) VALUES (?,?,'pending','no ghost yet')", id, now)
		}
		return err
	})
	if err != nil {
		return nil, err
	}
	return &Stored{id, scoreID, xuid, tag, d.ScoreboardType, scoreType, score}, nil
}

func hexX(x int64) string { return fmt.Sprintf("%#x", x) }

// Refusal says why Submit will not store this score ("" when it will): the measured value and the limit it broke.
func (s *Store) Refusal(scoreType int64, d *Submission, score float64) string {
	if math.IsNaN(score) || math.IsInf(score, 0) {
		return fmt.Sprintf("score %s is not a finite number", pyfmt.ReprFloat(score))
	}
	if score < 0 || (Ascending[scoreType] && score == 0) {
		what := "score"
		if Ascending[scoreType] {
			what = "time"
		}
		return fmt.Sprintf("score %s is not a possible %s", pyfmt.FmtG(score), what)
	}
	if limit, ok := ProgressLimits[[2]int64{d.ScoreboardType, scoreType}]; ok {
		whole := score == math.Trunc(score)
		if float64(limit) < score || !whole {
			why := fmt.Sprintf("above the %d this board can hold", limit)
			if score <= float64(limit) {
				why = "not a whole number"
			}
			return fmt.Sprintf("progress counter %s is %s", pyfmt.FmtG(score), why)
		}
	}
	if SpeedBoards[d.ScoreboardType] && score > SpeedCap {
		return fmt.Sprintf("speed %.3f m/s is above the %s m/s cap (%.0f km/h)", score, pyfmt.ReprFloat(SpeedCap), score*3.6)
	}
	if EventBoards[d.ScoreboardType] && (d.CarClassID == 6 || d.CarClassID == 7) {
		return fmt.Sprintf("car class %d (X/R) is not allowed on a Rivals board", d.CarClassID)
	}
	if req := s.EventClass(d.ScoreboardType, d.GameScoreboardID); req != nil && d.CarClassID != *req {
		return fmt.Sprintf("car class %d does not match the event's class %d", d.CarClassID, *req)
	}
	return ""
}

// Plausibility gives (hard, flags) of a stored submission from what the client claimed, ghost aside.
func (s *Store) Plausibility(r *Row) (hard, flags []string, err error) {
	cls, cpi, score := r.Cls, r.Cpi, r.Score
	if cls < 0 || cls >= int64(len(ClassTopPI)) || !(0 <= cpi && cpi <= 1+ClassPISlack) ||
		cpi > ClassTopPI[cls]+ClassPISlack || (0 < cls && cls < 6 && cpi < ClassTopPI[cls-1]-ClassPISlack) {
		hard = append(hard, fmt.Sprintf("PI %.4f does not belong to class %d", cpi, cls))
	}
	if ClassPivot[r.Sb] {
		if length, ok := s.routeLength(r.Track); ok && length != 0 && score < length/MaxRouteSpeed {
			hard = append(hard, fmt.Sprintf("%.3f s is faster than %s m/s over the %.0f m route", score, pyfmt.FmtG(MaxRouteSpeed), length))
		}
	}
	if r.Sb == 3 {
		var allowed map[int64]bool
		if s.EventCarIDs != nil {
			allowed = s.EventCarIDs(r.Gsid)
		} else {
			allowed = map[int64]bool{}
			for _, c := range s.eventCars(r.Gsid) {
				allowed[c.ID] = true
			}
		}
		if len(allowed) > 0 && !allowed[r.Car] {
			flags = append(flags, fmt.Sprintf("car %d does not match the event's car restriction", r.Car))
		}
	}
	row, err := s.DB.One("SELECT MIN(score) FROM score_submissions WHERE xuid=? AND sb=? AND st=? AND track=? AND gsid=? AND car=? AND ABS(cpi - ?) < 0.002 AND id < ?",
		r.Xuid, r.Sb, r.St, r.Track, r.Gsid, r.Car, cpi, r.ID)
	if err != nil {
		return nil, nil, err
	}
	if row != nil && !row.IsNullAt(0) {
		if before := row.FloatAt(0); before != 0 && score < before*BigImprovement {
			flags = append(flags, fmt.Sprintf("%.3f s after %.3f s in the same car", score, before))
		}
	}
	return hard, flags, nil
}

// Verdict is the outcome of judging a run and its ghost.
type Verdict struct {
	Kind     string
	Reasons  []string
	Measured *ghost.Measures
}

// JudgeRun judges a stored submission and its ghost.
func (s *Store) JudgeRun(r *Row, data []byte) (*Verdict, error) {
	kind, reasons, measured := ghost.Check(data, r.Score, r.Car)
	hard, flags, err := s.Plausibility(r)
	if err != nil {
		return nil, err
	}
	if len(hard) > 0 {
		kind = ghost.Hard
	} else if len(flags) > 0 && kind == ghost.Pass {
		kind = ghost.Flag
	}
	all := append(append(append([]string{}, hard...), reasons...), flags...)
	return &Verdict{kind, all, measured}, nil
}

// StoreVerdict records a verdict and, when enforcing, applies it. True if the run is public afterwards; an operator's hide stays and a restored run
// stays shown.
func (s *Store) StoreVerdict(t *db.Tx, sid int64, v *Verdict, now float64) (bool, error) {
	reasons, _ := pyjson.Dumps(stringsAny(v.Reasons), pyjson.Opts{})
	measured := "{}"
	if v.Measured != nil {
		measured, _ = pyjson.Dumps(v.Measured.Obj(), pyjson.Opts{})
	}
	if _, err := t.Exec("INSERT INTO score_verdicts (submission_id, version, verdict, reasons, measurements, decided_at) VALUES (?,?,?,?,?,?) "+
		"ON CONFLICT (submission_id) DO UPDATE SET version=excluded.version, verdict=excluded.verdict, reasons=excluded.reasons, "+
		"measurements=excluded.measurements, decided_at=excluded.decided_at", sid, ghost.CheckVersion, v.Kind, reasons, measured, now); err != nil {
		return false, err
	}
	if s.GhostPolicy != "enforce" {
		return true, nil
	}
	restored, err := t.One("SELECT 1 FROM score_overrides WHERE submission_id=?", sid)
	if err != nil {
		return false, err
	}
	if v.Kind == ghost.Hard && restored == nil {
		why := strings.Join(v.Reasons, "; ")
		if r := []rune(why); len(r) > 500 {
			why = string(r[:500])
		}
		_, err := t.Exec("INSERT INTO hidden_scores (submission_id, hidden_at, source, reason) VALUES (?,?,'auto',?) ON CONFLICT (submission_id) "+
			"DO UPDATE SET source='auto', reason=excluded.reason WHERE hidden_scores.source='pending'", sid, now, why)
		return false, err
	}
	if _, err := t.Exec("DELETE FROM hidden_scores WHERE submission_id=? AND source='pending'", sid); err != nil {
		return false, err
	}
	hidden, err := t.One("SELECT 1 FROM hidden_scores WHERE submission_id=?", sid)
	return hidden == nil, err
}

func stringsAny(l []string) []any {
	out := make([]any, len(l))
	for i, s := range l {
		out[i] = s
	}
	return out
}

// AttachGhost stores the ghost `xuid` uploads for its own SubmitScore (matched by the scoreId we issued) and judges the run. The first upload stands:
// the same bytes again are a harmless retry, anything else is refused. Returns (submission id, run is newly public), or (0, false) if refused.
func (s *Store) AttachGhost(scoreID, data []byte, xuid int64) (int64, bool, error) {
	data = append([]byte(nil), data...)
	r, err := s.DB.One("SELECT s.*, g.submission_id AS stored, '' AS tag FROM score_submissions s LEFT JOIN score_ghosts g ON g.submission_id = s.id WHERE s.score_id=?", scoreID)
	if err != nil {
		return 0, false, err
	}
	sidText := fmt.Sprintf("%x", scoreID)
	if len(sidText) > 8 {
		sidText = sidText[:8]
	}
	if r == nil {
		log.Warn("ghost upload refused: no submission has this scoreId (the SubmitScore was refused, or is from before a database restore) " +
			diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)}, diag.KVP{K: "score_id", V: sidText}, diag.KVP{K: "bytes", V: len(data)}))
		return 0, false, nil
	}
	row := rowOf(r)
	if row.Xuid != xuid || len(data) > ghost.MaxBlob {
		what := fmt.Sprintf("%d bytes is above the %d byte limit", len(data), ghost.MaxBlob)
		if row.Xuid != xuid {
			what = fmt.Sprintf("the run belongs to xuid %s", hexX(row.Xuid))
		}
		log.Warn("ghost upload refused: " + what + " " + diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)}, diag.KVP{K: "submission", V: row.ID}, diag.KVP{K: "score_id", V: sidText}))
		return 0, false, nil
	}
	if !r.IsNull("stored") {
		old, err := s.Ghost(row.ID)
		if err != nil {
			return 0, false, err
		}
		if string(old) == string(data) {
			log.Debug(fmt.Sprintf("ghost upload repeated, same bytes as the stored ghost (retry) submission=%d", row.ID))
			return row.ID, false, nil
		}
		log.Warn("ghost upload refused: the run already has a different ghost and the first upload stands " + diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)},
			diag.KVP{K: "submission", V: row.ID}, diag.KVP{K: "stored_bytes", V: len(old)}, diag.KVP{K: "new_bytes", V: len(data)}))
		return 0, false, nil
	}
	if _, err := ghost.Parse(data); err != nil { // every one of 17,011 real ghosts parses; what does not is never stored or served
		ge, ok := err.(*ghost.Error)
		if !ok {
			return 0, false, err
		}
		log.Warn(fmt.Sprintf("ghost upload refused: does not parse (%s) ", ge.Msg) + diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)}, diag.KVP{K: "submission", V: row.ID},
			diag.KVP{K: "bytes", V: len(data)}, diag.KVP{K: "board", V: fmt.Sprintf("(%d,%d)", row.Sb, row.St)}))
		return 0, false, nil
	}
	judged := GhostBoards[row.Sb] && Ascending[row.St]
	var verdict *Verdict
	if judged { // outside the transaction: a parse must never hold the lock
		if verdict, err = s.JudgeRun(row, data); err != nil {
			return 0, false, err
		}
	}
	now := Now()
	public := false
	err = s.DB.Do(func(t *db.Tx) error {
		if _, err := t.Exec("INSERT INTO score_ghosts (submission_id, data, uploaded) VALUES (?,?,?) ON CONFLICT (submission_id) DO NOTHING", row.ID, data, now); err != nil {
			return err
		}
		if judged {
			var err error
			public, err = s.StoreVerdict(t, row.ID, verdict, now)
			return err
		}
		return nil
	})
	if err != nil {
		return 0, false, err
	}
	if judged {
		s.logVerdict(row, verdict, public, len(data))
	}
	return row.ID, public && verdict.Kind != ghost.Hard, nil
}

// logVerdict writes one line per judged run: the verdict, what the policy did with it, and the figures that decided it.
func (s *Store) logVerdict(r *Row, v *Verdict, public bool, size int) {
	who := diag.KV(diag.KVP{K: "submission", V: r.ID}, diag.KVP{K: "xuid", V: hexX(r.Xuid)}, diag.KVP{K: "board", V: fmt.Sprintf("(%d,%d)", r.Sb, r.St)},
		diag.KVP{K: "track", V: r.Track}, diag.KVP{K: "car", V: r.Car}, diag.KVP{K: "score", V: fmt.Sprintf("%.3f", r.Score)}, diag.KVP{K: "ghost_bytes", V: size},
		diag.KVP{K: "policy", V: s.GhostPolicy}, diag.KVP{K: "shown", V: public})
	if v.Kind == ghost.Pass {
		m := v.Measured
		var frames any
		var speed, accel, power float64
		if m != nil {
			frames, speed, accel, power = m.Frames, m.Speed, m.Accel, m.Power
		}
		log.Debug("ghost verdict pass " + who + " " + diag.KV(diag.KVP{K: "frames", V: frames}, diag.KVP{K: "speed", V: fmt.Sprintf("%.1f", speed)},
			diag.KVP{K: "accel", V: fmt.Sprintf("%.1f", accel)}, diag.KVP{K: "power", V: fmt.Sprintf("%.0f", power)}))
		return
	}
	rs := v.Reasons
	more := ""
	if len(rs) > 6 {
		rs, more = rs[:6], " ..."
	}
	tail := ""
	if s.GhostPolicy != "enforce" {
		tail = " (policy=log: stays public)"
	}
	log.Warn(fmt.Sprintf("ghost verdict %s %s reasons: ", v.Kind, who) + strings.Join(rs, "; ") + more + tail)
}

// ByScoreID is the stored submission a client refers to by the scoreId we issued (nil if unknown).
func (s *Store) ByScoreID(scoreID []byte) (*Row, error) {
	r, err := s.DB.One("SELECT s.*, COALESCE(p.gamertag, '') AS tag, 0 AS ghost FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid WHERE s.score_id=?", scoreID)
	if err != nil || r == nil {
		return nil, err
	}
	return rowOf(r), nil
}

// Ghost is the stored ghost of a submission (nil = none).
func (s *Store) Ghost(id int64) ([]byte, error) {
	r, err := s.DB.One("SELECT data FROM score_ghosts WHERE submission_id=?", id)
	if err != nil || r == nil {
		return nil, err
	}
	return r.Bytes("data"), nil
}

// PublicGhost is the ghost a player may race: never that of a hidden run, whose id is public.
func (s *Store) PublicGhost(id int64) ([]byte, error) {
	r, err := s.DB.One("SELECT g.data FROM score_ghosts g JOIN score_submissions s ON s.id = g.submission_id WHERE g.submission_id=? AND "+scorereports.Visible, id)
	if err != nil || r == nil {
		return nil, err
	}
	return r.Bytes("data"), nil
}

// IsVisible says whether a run is shown on the boards.
func (s *Store) IsVisible(id int64) (bool, error) {
	r, err := s.DB.One("SELECT 1 FROM score_submissions s WHERE s.id=? AND "+scorereports.Visible, id)
	return r != nil, err
}

// AddXP records the award in the XP ledger and, when the audit accepts it, adds it to the total (atomic increment, so concurrent submissions
// cannot lose an update). Returns (total, verdict, reason).
func (s *Store) AddXP(xuid int64, xp float64) (float64, string, string, error) {
	verdict, reason := xpaudit.Judge(xp)
	if verdict == xpaudit.OK && xp == 0 {
		total, err := s.XPOf(xuid)
		return total, verdict, reason, err
	}
	now := Now()
	if verdict == xpaudit.OK {
		r, err := s.DB.One("SELECT COALESCE(SUM(xp), 0) FROM xp_awards WHERE xuid=? AND at > ? AND verdict=?", xuid, now-3600, xpaudit.OK)
		if err != nil {
			return 0, "", "", err
		}
		if recent := r.FloatAt(0); recent+xp > xpaudit.HourBudget {
			reason = fmt.Sprintf("over the hourly budget (%s of %s)", pyfmt.FmtG(recent+xp), pyfmt.FmtG(xpaudit.HourBudget)) // counted, log first
			if ok, skipped := rate.Allow(fmt.Sprintf("xp-budget/%d", xuid), XPBudgetLogSeconds); ok {
				var sup any
				if skipped > 0 {
					sup = skipped
				}
				log.Warn("XP over the hourly budget, still counted (policy: report only) " + diag.KV(diag.KVP{K: "xuid", V: hexX(xuid)}, diag.KVP{K: "this_award", V: pyfmt.FmtG(xp)},
					diag.KVP{K: "last_hour", V: pyfmt.FmtG(recent)}, diag.KVP{K: "budget", V: pyfmt.FmtG(xpaudit.HourBudget)}, diag.KVP{K: "suppressed", V: sup}))
			}
		}
	}
	var total float64
	err := s.DB.Do(func(t *db.Tx) error {
		if err := db.TouchPlayer(t, xuid, "", &now, nil); err != nil {
			return err
		}
		stored := xp
		if math.IsNaN(xp) || math.IsInf(xp, 0) {
			stored = 0
		}
		if _, err := t.Exec("INSERT INTO xp_awards (xuid, xp, at, verdict, reason) VALUES (?,?,?,?,?)", xuid, stored, now, verdict, reason); err != nil {
			return err
		}
		if verdict != xpaudit.OK {
			r, err := t.One("SELECT xp FROM player_xp WHERE xuid=?", xuid)
			if err == nil && r != nil {
				total = r.FloatAt(0)
			}
			return err
		}
		rows, err := t.Query("INSERT INTO player_xp (xuid, xp, updated) VALUES (?,?,?) ON CONFLICT(xuid) DO UPDATE SET xp = player_xp.xp + excluded.xp, updated = excluded.updated RETURNING xp", xuid, xp, now)
		if err != nil {
			return err
		}
		total = rows[0].FloatAt(0)
		return nil
	})
	return total, verdict, reason, err
}

// XPOf is a player's XP total.
func (s *Store) XPOf(xuid int64) (float64, error) {
	r, err := s.DB.One("SELECT xp FROM player_xp WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return 0, err
	}
	return r.FloatAt(0), nil
}

// XPEntry is one line of the XP ladder.
type XPEntry struct {
	Position int
	Xuid     int64
	Tag      string
	Score    float64
}

// XPTop is the operator XP ladder, from its ledger.
func (s *Store) XPTop(limit int) ([]XPEntry, error) {
	rows, err := s.DB.Query("SELECT x.xuid, x.xp, COALESCE(p.gamertag, '') AS tag FROM player_xp x LEFT JOIN players p ON p.xuid=x.xuid ORDER BY x.xp DESC, x.xuid LIMIT ?", max(0, limit))
	if err != nil {
		return nil, err
	}
	out := make([]XPEntry, len(rows))
	for i := range rows {
		out[i] = XPEntry{i + 1, rows[i].Int("xuid"), rows[i].Str("tag"), rows[i].Float("xp")}
	}
	return out, nil
}

// ---- reads

// Better says whether a beats b on this score type.
func Better(st int64, a, b float64) bool {
	if Ascending[st] {
		return a < b
	}
	return a > b
}

// bestPerPlayer gives each player's best submission on a board (any track when track is -1/65535), keyed by xuid, in the order the database
// returned them. Progress counters use the latest valid snapshot of the active save. viewer also sees their own run awaiting its ghost.
func (s *Store) bestPerPlayer(sb, st, track int64, xuid *int64, pivot int64, viewer *int64, counts bool) ([]*Row, error) {
	asc := Ascending[st]
	shown, shownArgs := scorereports.VisibleTo(viewer)
	where, args := "s.sb=? AND s.st=? AND "+shown, append([]any{sb, st}, shownArgs...)
	limit, hasLimit := ProgressLimits[[2]int64{sb, st}]
	if hasLimit {
		where += " AND s.score BETWEEN 0 AND ? AND s.score = FLOOR(s.score)"
		args = append(args, limit)
	}
	if EventBoards[sb] || ClassPivot[sb] { // class X/R must never surface on a Rivals board, read or write (user decision)
		where += " AND s.cls NOT IN (" + maxedClassesSQL + ")"
	}
	if SpeedBoards[sb] { // impossible speeds submitted before the cap existed stay stored but never show
		where += " AND s.score <= " + pyfmt.ReprFloat(SpeedCap)
	}
	if (EventBoards[sb] || GsidBoards[sb]) && pivot > 0 { // one board per event / per trap or zone
		where += " AND s.gsid=?"
		args = append(args, pivot)
		if req := s.EventClass(sb, pivot); req != nil {
			where += " AND s.cls=?"
			args = append(args, *req)
		}
	} else if track != -1 && track != 65535 {
		where += " AND s.track=?"
		args = append(args, track)
	}
	if xuid != nil {
		where += " AND s.xuid=?"
		args = append(args, *xuid)
	}
	if ClassPivot[sb] && 0 <= pivot && pivot <= 5 { // one board per car class (the submission's carClassId)
		where += " AND s.cls=?"
		args = append(args, pivot)
	}
	// Record ties retain their first submission; save snapshots follow submission order.
	order := "s.score DESC, s.id"
	if asc {
		order = "s.score ASC, s.id"
	}
	if hasLimit {
		order = "s.id DESC"
	}
	reports, ghostFlag := "0", "FALSE"
	if counts { // the game never shows them; only the website does
		reports, ghostFlag = scorereports.Counts, scorereports.GhostFlag
	}
	sql := fmt.Sprintf("SELECT * FROM (SELECT s.*, %s AS report_count, %s AS ghost_flag, COALESCE(p.gamertag, '') AS tag, "+
		"CASE WHEN g.submission_id IS NULL OR length(g.data) > %d THEN 0 ELSE s.id END AS ghost, ROW_NUMBER() OVER "+
		"(PARTITION BY s.xuid ORDER BY %s) AS rn "+
		"FROM score_submissions s LEFT JOIN players p ON p.xuid = s.xuid "+
		"LEFT JOIN score_ghosts g ON g.submission_id = s.id WHERE %s) ranked WHERE rn = 1", reports, ghostFlag, ghost.MaxServed, order, where)
	rows, err := s.DB.Query(sql, args...)
	if err != nil {
		return nil, err
	}
	// the Python dict keeps one entry per xuid (the query returns one) in query order
	out := make([]*Row, 0, len(rows))
	for i := range rows {
		out = append(out, rowOf(&rows[i]))
	}
	return out, nil
}

// BestOf is a player's best on a board (nil = none).
func (s *Store) BestOf(xuid, sb, st, track, pivot int64) (*Row, error) {
	rows, err := s.bestPerPlayer(sb, st, track, &xuid, pivot, &xuid, false)
	if err != nil {
		return nil, err
	}
	for _, r := range rows {
		if r.Xuid == xuid {
			return r, nil
		}
	}
	return nil, nil
}

// Top lists real players only (no synthesized rivals), best first, each row with its Position. For the website's public boards.
func (s *Store) Top(sb, st, track, pivot int64, limit int) ([]*Row, error) {
	rows, err := s.bestPerPlayer(sb, st, track, nil, pivot, nil, true)
	if err != nil {
		return nil, err
	}
	asc := Ascending[st]
	sort.SliceStable(rows, func(i, j int) bool {
		a, b := rows[i], rows[j]
		ka, kb := a.Score, b.Score
		if !asc {
			ka, kb = -ka, -kb
		}
		if ka != kb {
			return ka < kb
		}
		return a.T < b.T
	})
	for i, r := range rows {
		r.Position = i + 1
	}
	return rows[:min(len(rows), max(0, limit))], nil
}

func hashInt(parts ...any) int64 {
	strs := make([]string, len(parts))
	for i, p := range parts {
		strs[i] = fmt.Sprint(p)
	}
	sum := sha1.Sum([]byte(strings.Join(strs, "|")))
	return int64(binary.BigEndian.Uint64(append([]byte{0, 0}, sum[:6]...)))
}

func hashFrac(parts ...any) float64 { return float64(hashInt(parts...)) / float64(int64(1)<<48) }

// FakeXuid is the id of the made-up rival with this name.
func FakeXuid(name string) int64 { return FakeBase + hashInt(name)%0xFFFFFF }

// FakeName is the racing-driver name behind a fake rival's id ("" if the id is not one): the profile service needs it.
func (s *Store) FakeName(xuid int64) string {
	if xuid < FakeBase || xuid > FakeBase+0xFFFFFF {
		return ""
	}
	if xuid == TargetXuid {
		return TargetTag
	}
	if s.fakeNames == nil {
		s.fakeNames = map[int64]string{}
		for _, n := range s.Names {
			s.fakeNames[FakeXuid(n)] = trunc(n, 15)
		}
	}
	return s.fakeNames[xuid]
}

func trunc(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n])
	}
	return s
}

func roundHalfEven3(x float64) float64 {
	v, _ := strconv.ParseFloat(strconv.FormatFloat(x, 'f', 3, 64), 64)
	return v
}

// fake gives deterministic fake rivals around anchor (the player's best, or 0 for a default scale).
func (s *Store) fake(sb, st, track int64, anchor float64, n int, pivot int64) []*Row {
	var out []*Row
	var evCars []cardb.PoolCar
	if EventBoards[sb] && pivot > 0 {
		evCars = s.eventCars(pivot) // rivals drive cars the event allows
	}
	// Festival Route class boards (pivot = class D..S2): no made-up time may beat the world record for that track and class
	wr := 0.0
	if Ascending[st] && ClassPivot[sb] && 0 <= pivot && pivot <= 5 {
		wr = s.Records[[2]int{int(track), int(pivot)}]
	}
	for i := 0; i < n; i++ {
		nm := s.Names[(int64(i*7)+hashInt(sb, st, track, pivot)%int64(len(s.Names)))%int64(len(s.Names))]
		r := hashFrac(sb, st, track, pivot, nm, i)
		var score float64
		switch {
		case Ascending[st]:
			var length float64
			if track != -1 && track != 0 && track != 65535 {
				length, _ = s.routeLength(track)
			}
			var ev *EventInfo
			if EventBoards[sb] && pivot > 0 {
				ev = s.eventInfo(pivot)
			}
			if ev != nil {
				l, _ := s.routeLength(ev.Track)
				if v := l * float64(ev.Laps); v != 0 {
					length = v
				}
			}
			switch {
			case anchor != 0:
				score = anchor * (0.6 + 0.9*r) // 60% .. 150% of the player's own time
			case length != 0:
				// average speed over a lap by car class (D..S2), km/h, incl. corners [guess]; the route is a polyline of waypoints, so add 10%
				cls := int64(4) // open events: assume S1-ish
				if ev != nil {
					if ev.Cls != nil {
						cls = *ev.Cls
					}
				} else if ClassPivot[sb] && 0 <= pivot && pivot <= 5 {
					cls = pivot
				}
				cls = min(max(cls, 0), int64(len(ClassAvgKmh)-1))
				score = length * 1.1 / (ClassAvgKmh[cls] / 3.6) * (0.75 + 0.6*r)
			default:
				score = 100.0 * (0.6 + 0.9*r)
			}
			if wr != 0 && score < wr*WRFloor {
				score = wr * WRFloor * (1.0 + WRSpread*hashFrac(sb, st, track, pivot, nm, i, "wr"))
			}
		case SpeedBoards[sb]:
			base := 50.0 // about 180 km/h
			if anchor != 0 {
				base = anchor
			}
			var v float64
			if anchor != 0 {
				v = base * (0.7 + 0.8*r)
			} else {
				v = 35.0 + 45.0*r
			}
			score = math.Min(102.0, math.Max(20.0, v)) // m/s, capped at ~367 km/h
		default:
			base := 20000.0
			if anchor != 0 {
				base = anchor
			}
			score = base * (0.05 + 3.0*r*r) // long tail of lower scores, a few above
		}
		var speed *float64
		if SpeedBoards[sb] {
			speed = &score
		}
		car, cls, cpi := s.fakeCar(nm, speed, evCars)
		out = append(out, &Row{Xuid: FakeXuid(nm), Tag: trunc(nm, 15), Sb: sb, St: st, Track: track, Car: car, Cls: cls, Cpi: cpi, Score: roundHalfEven3(score),
			Clean: r > 0.3, Stm: false, Tcs: r < 0.5, Abs: true, Autoshift: r < 0.6, T: Now() - 86400*(1+20*r), Fake: true})
	}
	return out
}

// BountyFor is the credits for beating a rival on track (the "REWARDS" box): scales with the route length and with how high the rival ranks
// [guess; the real formula is Turn 10's].
func (s *Store) BountyFor(track int64, position int) int64 {
	var length float64
	if track != -1 && track != 0 && track != 65535 {
		length, _ = s.routeLength(track)
	}
	if length == 0 {
		length = 3000.0
	}
	km := length / 1000.0
	return int64(math.RoundToEven(km*400*(1.0+1.0/float64(max(1, position)))/10) * 10)
}

// fakeCar picks a real car for a fake row (so the page shows its name, not "DLC Car"). For speed boards the car is chosen by performance index
// so the speed is plausible for its class: roughly km/h = 40 + 320 * PI^1.5 (D ~150-190, A ~230-270, S2 ~300+) [inferred from spot checks].
func (s *Store) fakeCar(nm string, speed *float64, pool []cardb.PoolCar) (int64, int64, float64) {
	cars := pool
	if len(cars) == 0 {
		cars = s.Cars
	}
	if len(cars) == 0 {
		return 100 + hashInt(nm, "car")%500, hashInt(nm, "c") % 6, 0.3 + 0.6*hashFrac(nm, "cpi")
	}
	if speed == nil {
		c := cars[hashInt(nm, "car")%int64(len(cars))]
		return c.ID, c.Class, c.PI
	}
	kmh := *speed * 3.6
	target := math.Pow(math.Min(1.0, math.Max(0.0, (kmh-40.0)/320.0)), 2.0/3.0)
	// also cap by the car's listed top speed: those are unlimited-gearing values, ~10% above real stock figures, so a trap reading stays under 85% of it
	var ok []cardb.PoolCar
	for _, c := range cars {
		if c.Top*0.85 >= *speed {
			ok = append(ok, c)
		}
	}
	if len(ok) == 0 {
		ok = cars[max(0, len(cars)-8):]
	}
	near := append([]cardb.PoolCar(nil), ok...)
	sort.SliceStable(near, func(i, j int) bool { return math.Abs(near[i].PI-target) < math.Abs(near[j].PI-target) })
	near = near[:min(8, len(near))]
	c := near[hashInt(nm, "car", int64(math.RoundToEven(*speed)))%int64(len(near))]
	return c.ID, c.Class, c.PI
}

// Rows gives sorted best-per-player rows (real + fake), the requesting player's row, and the counts of real and fake rows.
func (s *Store) Rows(xuid, sb, st, track int64, startAt, maxResults int, pivot int64) (page []*Row, mine *Row, nReal, nFake int, err error) {
	real, err := s.bestPerPlayer(sb, st, track, nil, pivot, &xuid, false)
	if err != nil {
		return nil, nil, 0, 0, err
	}
	var me *Row
	for _, r := range real {
		if r.Xuid == xuid {
			me = r
		}
	}
	var fake []*Row
	if _, progress := ProgressLimits[[2]int64{sb, st}]; !progress {
		anchor := 0.0
		if me != nil {
			anchor = me.Score
		}
		fake = s.fake(sb, st, track, anchor, max(0, FakeRows-len(real)), pivot)
	}
	all := append(append([]*Row{}, real...), fake...)
	desc := !Ascending[st]
	sort.SliceStable(all, func(i, j int) bool {
		if desc {
			return all[i].Score > all[j].Score
		}
		return all[i].Score < all[j].Score
	})
	for i, r := range all {
		r.Position = i + 1
	}
	for _, r := range all {
		if r.Xuid == xuid {
			mine = r
			break
		}
	}
	a := min(max(0, startAt-1), len(all))
	page = all[a:min(len(all), a+max(0, maxResults))]
	return page, mine, len(real), len(fake), nil
}

// RivalFor is the rival to chase: the row of rivalXuid when the player picked one on the board, else the row just above the player's best; with
// no best yet, one near the middle of the board. A real player's row (it has a ghost to race) is preferred over a made-up one. The player's own row
// is the rival when they are #1 or when the client asks for their own xuid. On an empty board it is the world record, or a made-up time when
// there is no record: GetRival has no "not found" flag, and the client spins forever on an all-zero answer.
func (s *Store) RivalFor(xuid, sb, st, track, pivot, rivalXuid int64) (*Row, error) {
	rows, mine, _, _, err := s.Rows(xuid, sb, st, track, 1, 1000, pivot)
	if err != nil {
		return nil, err
	}
	if len(rows) == 0 {
		if r := s.recordRow(sb, st, track, pivot); r != nil {
			return r, nil
		}
		f := s.fake(sb, st, track, 0, 1, pivot)[0]
		f.Position = 1
		return f, nil
	}
	if rivalXuid != 0 {
		for _, r := range rows {
			if r.Xuid == rivalXuid {
				return r, nil
			}
		}
	}
	if mine == nil {
		mid := len(rows) / 2
		if r := nearReal(rows, mid, xuid, len(rows), 0); r != nil { // any real ghost on the board beats a made-up rival
			return r, nil
		}
		return rows[mid], nil
	}
	var above []*Row
	for _, r := range rows {
		if r.Position < mine.Position {
			above = append(above, r)
		}
	}
	if len(above) == 0 {
		return mine, nil
	}
	last := above[len(above)-1]
	if r := nearReal(rows, last.Position-1, xuid, 4, mine.Position); r != nil {
		return r, nil
	}
	return last, nil
}

// RecordTarget is (seconds, class) of the world record an empty time board is raced against (ok=false: none). Festival Routes: the record of the
// board's class. Monthly (always 1 lap): the event's track and class. Showcase / open events: the track's fastest class. A class without a record
// on that track takes the next faster class that has one (never a slower class's time).
func (s *Store) RecordTarget(sb, st, track, pivot int64) (float64, int, bool) {
	if !Ascending[st] {
		return 0, 0, false
	}
	cls := -1
	if ClassPivot[sb] && 0 <= pivot && pivot <= 5 {
		cls = int(pivot)
	} else if EventBoards[sb] && pivot > 0 {
		if ev := s.eventInfo(pivot); ev != nil { // Monthly only; Showcase pivots are the game's own event ids (request track, any class)
			track = ev.Track
			cls = -1
			if ev.Cls != nil {
				cls = int(*ev.Cls)
			}
		}
	}
	best, found := 0, false
	for k := range s.Records {
		if int64(k[0]) != track {
			continue
		}
		c := k[1]
		if cls >= 0 && c < cls {
			continue
		}
		if !found || (cls < 0 && c > best) || (cls >= 0 && c < best) {
			best, found = c, true
		}
	}
	if !found {
		return 0, 0, false
	}
	return s.Records[[2]int{int(track), best}], best, true
}

func (s *Store) recordRow(sb, st, track, pivot int64) *Row {
	secs, cls, ok := s.RecordTarget(sb, st, track, pivot)
	if !ok {
		return nil
	}
	var pool []cardb.PoolCar
	if EventBoards[sb] && pivot > 0 {
		pool = s.eventCars(pivot)
	}
	if len(pool) == 0 {
		pool = s.Cars
	}
	var same []cardb.PoolCar
	for _, c := range pool {
		if c.Class == int64(cls) {
			same = append(same, c)
		}
	}
	if len(same) == 0 {
		same = pool
	}
	car, ccls, cpi := s.fakeCar(TargetTag, nil, same) // a car of the record's class
	return &Row{Xuid: TargetXuid, Tag: TargetTag, Sb: sb, St: st, Track: track, Car: car, Cls: ccls, Cpi: cpi, Score: secs, Clean: true, T: Now(), Position: 1, Fake: true}
}

// nearReal is the closest real (non-fake) row with a ghost within radius rows of index idx, never the player's own row (onlyAbove > 0: only rows
// above that position).
func nearReal(rows []*Row, idx int, xuid int64, radius, onlyAbove int) *Row {
	var best *Row
	bestJ := 0
	for j := max(0, idx-radius); j < min(len(rows), idx+radius+1); j++ {
		r := rows[j]
		if r.Fake || r.Xuid == xuid || r.Ghost == 0 {
			continue
		}
		if onlyAbove > 0 && r.Position >= onlyAbove {
			continue
		}
		if best == nil || abs(j-idx) < abs(bestJ-idx) {
			best, bestJ = r, j
		}
	}
	return best
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}
