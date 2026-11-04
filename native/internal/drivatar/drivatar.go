// Package drivatar is the Drivatar data of the LSP (= server/drivatar.py): the Drivatars the game ships (loaded from the catalog table
// game.drivatar_shipped by the caller), the ones players upload (SaveDrivatarInformation) in the `drivatars` table, and a Drivatar built for a player
// profile from a shipped one used as a template. A record is the wire-shaped JSON object (pyjson.Obj, key order kept); the lock flag travels as "_locked".
package drivatar

import (
	"fmt"
	"hash/crc32"
	"math"
	"os"
	"sort"
	"strconv"
	"sync"
	"time"

	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyfmt"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
)

var (
	log  = diag.Get("drivatar")
	rate = diag.NewRateLimit(nil, 0)
)

// Now is the clock (tests replace it).
var Now = func() float64 { return float64(time.Now().UnixNano()) / 1e9 }

// Cache constants: GetDrivatarsForCache passes 120 as count and capacity; a longer reply fails to read and the client keeps no cache at all. The file
// persists on the client, so each skill keeps a floor of shipped records.
const (
	CacheCapacity        = 120
	CacheShippedPerSkill = 12
	MaxInfoValue         = 1e6 // the client does not check these floats after download
	NumSkills            = 5
	DefaultSkill         = 3
	DefaultWristband     = 240000 // about a Lamborghini Huracan's value
)

// CacheLimit is FH2MP_DRIVATAR_CACHE_LIMIT: fewer Drivatars than the client's 120 (0 serves none).
func CacheLimit() int {
	if v := os.Getenv("FH2MP_DRIVATAR_CACHE_LIMIT"); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return min(CacheCapacity, max(0, n))
		}
	}
	return CacheCapacity
}

// Switches read at start (tests change them).
var (
	CachePlayers = os.Getenv("FH2MP_DRIVATAR_CACHE_PLAYERS") != "0" // 0 serves shipped records only, which replaces the client's saved file
	Cars         = os.Getenv("FH2MP_DRIVATAR_CARS") == "1"          // real cars, tunes and liveries on player Drivatars
)

// Record is one Drivatar as the wire shows it.
type Record = pyjson.Obj

func get(r Record, k string) any { v, _ := r.Get(k); return v }

func asInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	case int:
		return int64(x)
	case bool:
		if x {
			return 1
		}
	}
	return 0
}

func asFloat(v any) float64 {
	switch x := v.(type) {
	case float64:
		return x
	case int64:
		return float64(x)
	}
	return 0
}

// copyRecord is json.loads(json.dumps(r)).
func copyRecord(r Record) (Record, error) {
	text, err := pyjson.Dumps(r, pyjson.Opts{})
	if err != nil {
		return nil, err
	}
	v, err := pyjson.Parse(text)
	if err != nil {
		return nil, err
	}
	o, _ := v.(pyjson.Obj)
	return o, nil
}

// SaneInfo: every number in an uploaded Drivatar is finite and in range.
func SaneInfo(v any) bool {
	switch x := v.(type) {
	case pyjson.Obj:
		for _, p := range x {
			if !SaneInfo(p.V) {
				return false
			}
		}
	case []any:
		for _, e := range x {
			if !SaneInfo(e) {
				return false
			}
		}
	case float64:
		return !math.IsNaN(x) && !math.IsInf(x, 0) && math.Abs(x) <= MaxInfoValue
	}
	return true
}

// KeyError is a field name Edit does not know.
type KeyError struct{ Key string }

func (e *KeyError) Error() string { return pyfmt.ReprStr(e.Key) }

// Store is the Drivatar store.
type Store struct {
	DB        *db.DB
	Templates []Record // the shipped Drivatars
	KnownCars map[int]struct{}
	// CarOverride maps a decimal xuid to the car id its Drivatar uses.
	CarOverride map[string]int64

	mu       sync.Mutex
	rng      *pyrand.Random
	tplNames map[int64]string
	haveTpl  bool
}

// New makes a store on the shipped templates.
func New(templates []Record, d *db.DB) *Store {
	return &Store{DB: d, Templates: templates, CarOverride: map[string]int64{}, rng: pyrand.New(uint64(time.Now().UnixNano()))}
}

// SetRng replaces the random source (tests).
func (s *Store) SetRng(r *pyrand.Random) {
	s.mu.Lock()
	s.rng = r
	s.mu.Unlock()
}

// ---- persistence (table `drivatars`: wire-shaped record as JSON plus the columns we select on)

// Count is the number of stored player Drivatars.
func (s *Store) Count() (int64, error) {
	r, err := s.DB.One("SELECT COUNT(*) FROM drivatars")
	if err != nil {
		return 0, err
	}
	return r.IntAt(0), nil
}

// BotName is the name of a shipped Drivatar bot, "" if the id is not one.
func (s *Store) BotName(xuid int64) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.haveTpl {
		s.tplNames = map[int64]string{}
		for _, t := range s.Templates {
			s.tplNames[asInt(get(t, "Xuid"))], _ = get(t, "GamerTag").(string)
		}
		s.haveTpl = true
	}
	return s.tplNames[xuid]
}

// TagForXuid is the name of a bot or a stored player Drivatar, "" for an unknown id: the profile service shows these names in lobbies.
func (s *Store) TagForXuid(xuid int64) (string, error) {
	if n := s.BotName(xuid); n != "" {
		return n, nil
	}
	r, err := s.DB.One("SELECT gamertag FROM drivatars WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return "", err
	}
	return r.Str("gamertag"), nil
}

// Gamertags lists the stored player Drivatars' names in the order they were added.
func (s *Store) Gamertags() ([]string, error) {
	rows, err := s.DB.Query("SELECT gamertag FROM drivatars ORDER BY seq")
	if err != nil {
		return nil, err
	}
	out := make([]string, len(rows))
	for i := range rows {
		out[i] = rows[i].StrAt(0)
	}
	return out, nil
}

func recOf(r *db.Row) (Record, error) {
	v, err := pyjson.Parse(r.Str("record"))
	if err != nil {
		return nil, err
	}
	rec, _ := v.(pyjson.Obj)
	if r.Int("locked") != 0 {
		rec.Set("_locked", true)
	}
	return rec, nil
}

func (s *Store) get(t *db.Tx, xuid int64) (Record, error) {
	r, err := t.One("SELECT record, locked FROM drivatars WHERE xuid=?", xuid)
	if err != nil || r == nil {
		return nil, err
	}
	return recOf(r)
}

// put inserts or replaces a player's Drivatar (the lock flag travels as rec["_locked"]).
func put(t *db.Tx, rec Record) error {
	var stored Record
	locked := int64(0)
	for _, p := range rec {
		if p.K == "_locked" {
			if b, ok := p.V.(bool); ok && b {
				locked = 1
			}
			continue
		}
		stored = append(stored, p)
	}
	text, err := pyjson.Dumps(stored, pyjson.Opts{})
	if err != nil {
		return err
	}
	_, err = t.Exec("INSERT INTO drivatars (xuid, gamertag, skill, wristband, locked, record, updated) VALUES (?,?,?,?,?,?,?) "+
		"ON CONFLICT(xuid) DO UPDATE SET gamertag=excluded.gamertag, skill=excluded.skill, "+
		"wristband=excluded.wristband, locked=excluded.locked, record=excluded.record, updated=excluded.updated",
		asInt(get(stored, "Xuid")), get(stored, "GamerTag"), asInt(get(stored, "UserSkillLevel")), asInt(get(stored, "wristbandLevel")), locked, text, Now())
	return err
}

// ---- records

// Template is the shipped Drivatar closest to skill and wristband (nil pointers = the defaults); nil when there are none.
func (s *Store) Template(skill, wristband *int64) Record {
	if len(s.Templates) == 0 {
		return nil
	}
	sk, wb := int64(DefaultSkill), int64(DefaultWristband)
	if skill != nil {
		sk = *skill
	}
	if wristband != nil {
		wb = *wristband
	}
	abs := func(x int64) int64 {
		if x < 0 {
			return -x
		}
		return x
	}
	best := s.Templates[0]
	bs, bw := abs(asInt(get(best, "UserSkillLevel"))-sk), abs(asInt(get(best, "wristbandLevel"))-wb)
	for _, t := range s.Templates[1:] {
		ts, tw := abs(asInt(get(t, "UserSkillLevel"))-sk), abs(asInt(get(t, "wristbandLevel"))-wb)
		if ts < bs || ts == bs && tw < bw {
			best, bs, bw = t, ts, tw
		}
	}
	return best
}

func canon(r Record) string {
	if r == nil {
		return "null"
	}
	text, _ := pyjson.Dumps(r, pyjson.Opts{SortKeys: true})
	return text
}

func (s *Store) forPlayer(t *db.Tx, xuid int64, gamertag string) (Record, error) {
	rec, err := s.get(t, xuid)
	if err != nil {
		return nil, err
	}
	before := canon(rec)
	if rec == nil {
		tpl := s.Template(nil, nil)
		if tpl == nil {
			return nil, nil
		}
		if rec, err = copyRecord(tpl); err != nil {
			return nil, err
		}
		rec.Set("IsFriend", false)
	}
	if gamertag != "" {
		rec.Set("GamerTag", gamertag)
	}
	rec.Set("Xuid", xuid)
	if car, ok := s.CarOverride[strconv.FormatInt(xuid, 10)]; ok {
		rec.Set("CarId", car)
	}
	if canon(rec) != before {
		if err := db.TouchPlayer(t, xuid, gamertag, nil, nil); err != nil {
			return nil, err
		}
		if err := put(t, rec); err != nil {
			return nil, err
		}
	}
	return rec, nil
}

// ForPlayer is the Drivatar record for a player: their upload when we have one, else a profile-based fake (nil if there are no templates).
func (s *Store) ForPlayer(xuid int64, gamertag string) (Record, error) {
	var rec Record
	err := s.DB.Do(func(t *db.Tx) error {
		var err error
		rec, err = s.forPlayer(t, xuid, gamertag)
		return err
	})
	return rec, err
}

// AddPlayer creates (or replaces) a stored player Drivatar from the nearest shipped template.
func (s *Store) AddPlayer(xuid int64, gamertag string, skill, wristband *int64) (Record, error) {
	tpl := s.Template(skill, wristband)
	if tpl == nil {
		return nil, nil
	}
	rec, err := copyRecord(tpl)
	if err != nil {
		return nil, err
	}
	tag := gamertag
	if r := []rune(tag); len(r) > 15 {
		tag = string(r[:15])
	}
	rec.Set("GamerTag", tag)
	rec.Set("Xuid", xuid)
	rec.Set("IsFriend", false)
	return rec, s.DB.Do(func(t *db.Tx) error {
		if err := db.TouchPlayer(t, xuid, tag, nil, nil); err != nil {
			return err
		}
		return put(t, rec)
	})
}

// Remove deletes a stored Drivatar; true when there was one.
func (s *Store) Remove(xuid int64) (bool, error) {
	var n int64
	err := s.DB.Do(func(t *db.Tx) error {
		r, err := t.Exec("DELETE FROM drivatars WHERE xuid=?", xuid)
		n = r.RowsAffected
		return err
	})
	return n > 0, err
}

// Set is one field to change in Edit.
type Set struct {
	K string
	V float64
}

// Edit hand-tunes a stored Drivatar. The sets map UserInfo field names (e.g. sideswipeMean, or base fields like timeRatioMean) or UserSkillLevel /
// wristbandLevel / CarId / BucketId to values. lock=true keeps the game's own training uploads from overwriting the player's stats; lock=false releases
// it. nil when there is no such Drivatar; an unknown field is a *KeyError and nothing is applied.
func (s *Store) Edit(xuid int64, sets []Set, lock *bool) (Record, error) {
	var rec Record
	err := s.DB.Do(func(t *db.Tx) error {
		var err error
		if rec, err = s.get(t, xuid); err != nil || rec == nil {
			return err
		}
		ui, _ := get(rec, "UserInfo").(pyjson.Obj)
		base, _ := get(ui, "base").(pyjson.Obj)
		for _, st := range sets {
			if v, ok := ui.Get(st.K); ok {
				if _, isObj := v.(pyjson.Obj); !isObj {
					ui.Set(st.K, st.V)
					continue
				}
			}
			if _, ok := base.Get(st.K); ok {
				base.Set(st.K, st.V)
			} else if st.K == "UserSkillLevel" || st.K == "wristbandLevel" || st.K == "CarId" || st.K == "BucketId" {
				rec.Set(st.K, int64(st.V))
			} else {
				return &KeyError{st.K} // rolls the transaction back, nothing half-applied
			}
		}
		if lock != nil {
			rec.Set("_locked", *lock)
		}
		return put(t, rec)
	})
	if err != nil {
		return nil, err
	}
	return rec, nil
}

// SaveUpload stores what a client uploads (SaveDrivatarInformation). Returns false for a refused upload (not finite / out of range), true when
// stored, and ok=false-with-nil for "not stored" (no template, or locked) as in Python's None.
func (s *Store) SaveUpload(xuid int64, gamertag string, info, curves, segments any) (stored bool, refused bool, err error) {
	if !(SaneInfo(info) && SaneInfo(curves) && SaneInfo(segments)) {
		var bad []string
		for _, c := range []struct {
			n string
			v any
		}{{"info", info}, {"curves", curves}, {"segments", segments}} {
			if !SaneInfo(c.v) {
				bad = append(bad, c.n)
			}
		}
		log.Warn(fmt.Sprintf("Drivatar upload refused: %s hold a value that is not finite or beyond +-%s, xuid=%#x", join(bad, ", "), "1e+06", xuid))
		return false, true, nil
	}
	err = s.DB.Do(func(t *db.Tx) error {
		rec, err := s.forPlayer(t, xuid, gamertag)
		if err != nil {
			return err
		}
		locked, _ := get(rec, "_locked").(bool)
		if rec == nil || locked {
			why := "the stored Drivatar is locked by the operator"
			if rec == nil {
				why = "no shipped template to build a record from"
			}
			log.Info(fmt.Sprintf("Drivatar upload not stored: %s xuid=%#x", why, xuid))
			return nil
		}
		rec.Set("UserInfo", info)
		rec.Set("Curves", curves)
		rec.Set("Segments", segments)
		if err := put(t, rec); err != nil {
			return err
		}
		stored = true
		return nil
	})
	return stored, false, err
}

func join(l []string, sep string) string {
	out := ""
	for i, s := range l {
		if i > 0 {
			out += sep
		}
		out += s
	}
	return out
}

// SetWristband keeps the car-value bucket of the car the player drives now on their stored Drivatar.
func (s *Store) SetWristband(xuid, wristband int64) error {
	return s.DB.Do(func(t *db.Tx) error {
		rec, err := s.get(t, xuid)
		if err != nil || rec == nil || asInt(get(rec, "wristbandLevel")) == wristband {
			return err
		}
		rec.Set("wristbandLevel", wristband)
		return put(t, rec)
	})
}

func (s *Store) wristbands() []int64 {
	seen := map[int64]bool{}
	var w []int64
	for _, t := range s.Templates {
		v := asInt(get(t, "wristbandLevel"))
		if !seen[v] {
			seen[v] = true
			w = append(w, v)
		}
	}
	sort.Slice(w, func(i, j int) bool { return w[i] < w[j] })
	return w
}

// BucketWristband maps a car-value bucket index (as the client sends it) to the shipped Drivatars' wristband value. The shipped set has 25 distinct
// values; the index is their ascending order.
func (s *Store) BucketWristband(bucket int64) int64 {
	w := s.wristbands()
	if len(w) == 0 {
		return DefaultWristband
	}
	return w[max(0, min(bucket, int64(len(w)-1)))]
}

// spreadWristband: the client shows a Drivatar's wristbandLevel as the level on its name tag. Free Roam sends bucket 0, so players' stored values piled
// up on the lowest one and every tag read the same. Such untouched records get a stable value from the shipped set's own spread, picked by their id;
// locked or already different ones are kept.
func (s *Store) spreadWristband(rec Record) Record {
	if len(s.Templates) == 0 {
		return rec
	}
	low := asInt(get(s.Templates[0], "wristbandLevel"))
	for _, t := range s.Templates {
		low = min(low, asInt(get(t, "wristbandLevel")))
	}
	if locked, _ := get(rec, "_locked").(bool); locked || asInt(get(rec, "wristbandLevel")) != low {
		return rec
	}
	w := s.wristbands()
	rec.Set("wristbandLevel", w[int(crc32.ChecksumIEEE([]byte(strconv.FormatInt(asInt(get(rec, "Xuid")), 10)))%uint32(len(w)))])
	return rec
}

// Pool gives real players' Drivatars first (whatever their skill: there are few of them), then the shipped ones of the requested skill, nearest
// car-value bucket first. skill / nearWristband / excludeXuid nil or 0 = no filter.
func (s *Store) Pool(limit int, skill, nearWristband *int64, excludeXuid int64, excludeXuids []int64, anySkillPlayers bool) ([]Record, error) {
	skipSet := map[int64]bool{}
	skip := []int64{}
	for _, x := range excludeXuids {
		if !skipSet[x] {
			skipSet[x] = true
			skip = append(skip, x)
		}
	}
	sort.Slice(skip, func(i, j int) bool { return skip[i] < skip[j] })
	where, args := "xuid <> ? AND xuid <> ALL(?::bigint[])", []any{excludeXuid, skip}
	if !anySkillPlayers && skill != nil {
		where += " AND skill = ?"
		args = append(args, *skill)
	}
	rows, err := s.DB.Query("SELECT record, locked FROM drivatars WHERE "+where+" ORDER BY seq LIMIT ?", append(args, max(0, limit))...)
	if err != nil {
		return nil, err
	}
	var recs []Record
	for i := range rows {
		rec, err := recOf(&rows[i])
		if err != nil {
			return nil, err
		}
		recs = append(recs, rec)
	}
	var players []Record
	for _, rec := range recs {
		if SaneInfo(get(rec, "UserInfo")) {
			players = append(players, s.spreadWristband(rec))
		}
	}
	noteSkipped(len(recs)-len(players), "pool")
	var tpl []Record
	for _, t := range s.Templates {
		if (skill == nil || asInt(get(t, "UserSkillLevel")) == *skill) && !skipSet[asInt(get(t, "Xuid"))] {
			tpl = append(tpl, t)
		}
	}
	if nearWristband != nil {
		sort.SliceStable(tpl, func(i, j int) bool {
			a, b := asInt(get(tpl[i], "wristbandLevel"))-*nearWristband, asInt(get(tpl[j], "wristbandLevel"))-*nearWristband
			if a < 0 {
				a = -a
			}
			if b < 0 {
				b = -b
			}
			return a < b
		})
	}
	room := max(0, limit-len(players))
	window := tpl
	if nearWristband != nil { // nearest car values, but not always the same ones
		window = tpl[:min(len(tpl), max(room*4, 40))]
	}
	chosen := s.sample(window, min(room, len(window)))
	all := append(append([]Record{}, players...), chosen...)
	if len(all) > limit {
		all = all[:max(0, limit)]
	}
	out, err := s.withCars(all)
	if err != nil {
		return nil, err
	}
	s.mu.Lock()
	pyrand.Shuffle(s.rng, out)
	s.mu.Unlock()
	return out, nil
}

func (s *Store) sample(pop []Record, k int) []Record {
	s.mu.Lock()
	defer s.mu.Unlock()
	return pyrand.Sample(s.rng, pop, k)
}

// ForCache is the global cache: never more than the client holds, a floor of shipped records per skill, then recent players spread across the skills,
// then more shipped ones.
func (s *Store) ForCache(limit int, excludeXuid int64) ([]Record, error) {
	limit = min(limit, CacheLimit())
	var shipped []Record
	for skill := int64(0); skill < NumSkills; skill++ {
		var ofSkill []Record
		for _, t := range s.Templates {
			if asInt(get(t, "UserSkillLevel")) == skill {
				ofSkill = append(ofSkill, t)
			}
		}
		shipped = append(shipped, s.sample(ofSkill, min(CacheShippedPerSkill, len(ofSkill)))...)
	}
	if len(shipped) > limit {
		shipped = shipped[:max(0, limit)]
	}
	room := limit - len(shipped)
	var players []Record
	if CachePlayers && room > 0 {
		rows, err := s.DB.Query("SELECT record, locked FROM drivatars WHERE xuid <> ? ORDER BY updated DESC LIMIT ?", excludeXuid, room*4)
		if err != nil {
			return nil, err
		}
		var recs []Record
		for i := range rows {
			rec, err := recOf(&rows[i])
			if err != nil {
				return nil, err
			}
			if tag, _ := get(rec, "GamerTag").(string); tag != "" && SaneInfo(get(rec, "UserInfo")) {
				recs = append(recs, rec)
			}
		}
		noteSkipped(len(rows)-len(recs), "cache")
		perSkill := make([][]Record, NumSkills)
		for skill := range perSkill {
			for _, r := range recs {
				if asInt(get(r, "UserSkillLevel")) == int64(skill) {
					perSkill[skill] = append(perSkill[skill], r)
				}
			}
		}
		for len(players) < room {
			any := false
			for _, l := range perSkill {
				if len(l) > 0 {
					any = true
				}
			}
			if !any {
				break
			}
			for i, of := range perSkill {
				if len(of) > 0 && len(players) < room {
					players = append(players, s.spreadWristband(of[0]))
					perSkill[i] = of[1:]
				}
			}
		}
	}
	used := map[int64]bool{}
	for _, t := range shipped {
		used[asInt(get(t, "Xuid"))] = true
	}
	var rest []Record
	for _, t := range s.Templates {
		if !used[asInt(get(t, "Xuid"))] {
			rest = append(rest, t)
		}
	}
	fill := s.sample(rest, max(0, min(limit-len(shipped)-len(players), len(rest))))
	return s.withCars(append(append(append([]Record{}, shipped...), players...), fill...))
}

// noteSkipped: stored Drivatars left out of a reply because their numbers fail SaneInfo (or they have no name): the client reads them without checking.
func noteSkipped(n int, where string) {
	if n > 0 {
		if ok, skipped := rate.Allow("skipped/"+where, 600.0); ok {
			extra := ""
			if skipped > 0 {
				extra = fmt.Sprintf("; %d replies like it since the last line", skipped)
			}
			log.Warn(fmt.Sprintf("%d stored Drivatar(s) left out of the %s reply (no name, or a value not finite or beyond +-1e+06)%s", n, where, extra))
		}
	}
}

// withCars puts a player's Drivatar in that player's own latest car, with their own tune and livery for it. The client checks the car against the
// bucket and, if it does not fit, swaps it and clears both ids, so a misfit costs nothing; a shipped record has no owner and is left alone.
func (s *Store) withCars(recs []Record) ([]Record, error) {
	bots := map[int64]bool{}
	for _, t := range s.Templates {
		bots[asInt(get(t, "Xuid"))] = true
	}
	var mine []Record
	for _, r := range recs {
		if !bots[asInt(get(r, "Xuid"))] {
			mine = append(mine, r)
		}
	}
	if !Cars || len(mine) == 0 {
		return recs, nil
	}
	set := map[int64]bool{}
	xuids := []int64{}
	for _, r := range mine {
		x := asInt(get(r, "Xuid"))
		if !set[x] {
			set[x] = true
			xuids = append(xuids, x)
		}
	}
	sort.Slice(xuids, func(i, j int) bool { return xuids[i] < xuids[j] })
	rows, err := s.DB.Query("SELECT DISTINCT ON (xuid) xuid, car FROM score_submissions WHERE xuid = ANY(?::bigint[]) AND car > 0 ORDER BY xuid, submitted DESC", xuids)
	if err != nil {
		return nil, err
	}
	cars := map[int64]int64{}
	for i := range rows {
		cars[rows[i].Int("xuid")] = rows[i].Int("car")
	}
	rows, err = s.DB.Query("SELECT DISTINCT ON (creator, car_id, kind) creator, car_id, kind, id FROM ugc_versioned WHERE creator = ANY(?::bigint[]) "+
		"AND kind IN ('livery', 'tune') AND refs > 0 ORDER BY creator, car_id, kind, created DESC", xuids)
	if err != nil {
		return nil, err
	}
	type key struct {
		creator, car int64
		kind         string
	}
	content := map[key]int64{}
	for i := range rows {
		content[key{rows[i].Int("creator"), rows[i].Int("car_id"), rows[i].Str("kind")}] = rows[i].Int("id")
	}
	out := make([]Record, 0, len(recs))
	for _, r := range recs {
		x := asInt(get(r, "Xuid"))
		if car, ok := cars[x]; ok {
			if _, known := s.KnownCars[int(car)]; known {
				n := append(Record(nil), r...)
				tune, ok1 := content[key{x, car, "tune"}]
				if !ok1 {
					tune = -1
				}
				livery, ok2 := content[key{x, car, "livery"}]
				if !ok2 {
					livery = -1
				}
				n.Set("CarId", car)
				n.Set("TuneId", tune)
				n.Set("LiveryId", livery)
				r = n
			}
		}
		out = append(out, r)
	}
	return out, nil
}
