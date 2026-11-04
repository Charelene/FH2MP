// Package rivals is the Monthly Rivals event catalog (= server/rivals.py). Turn 10 curated the Monthly Rivals events and that service is gone, so
// events are generated: each month (and on every admin re-roll) a seeded random pick of closed circuits, each paired with a random restriction
// theme drawn from the game's own categories. Everything comes from the game catalog (schema `game`, package cardb): circuits and lengths, names,
// restrictions (attributes of the game's <CarRestrictions>).
//
// Event ids are 16-bit on the wire (ForzaTimeTrial.id is a u16) and double as the scoreboard key, so every rotation gets a fresh id range:
// 1000 + (months since 2026-01) * 160 + reroll * 16 + index (reroll < 10, index < 16).
package rivals

import (
	"fmt"
	"math"
	"sort"
	"strings"
	"sync"
	"time"

	"fh2mp/internal/cardb"
	"fh2mp/internal/db"
	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
	"fh2mp/internal/pyrand"
	"fh2mp/internal/rentals"
)

var log = diag.Get("rivals")

// Tunables.
const (
	MinCars            = 4 // a theme must match at least this many cars of the game
	EventsPerRotation  = 12
	IDBase             = 1000
	SerialCacheSeconds = 30 // GetTimeTrials and every board page ask; another process may re-roll, which then shows within this time
)

// ClassLetters name the car classes D..S2.
var ClassLetters = [6]string{"D", "C", "B", "A", "S1", "S2"}

type era struct {
	lo, hi *int64
	name   string
}

func ip(v int64) *int64 { return &v }

var eras = []era{{nil, ip(1969), "Classics"}, {ip(1970), ip(1989), "Retro Icons"}, {ip(1990), ip(1999), "90s Heroes"}, {ip(2000), ip(2009), "2000s Icons"}, {ip(2010), nil, "Modern Machines"}}

// ClassEnforced are the themes whose card class (average of the matching cars) is also enforced.
var ClassEnforced = map[string]bool{"body": true, "make": true, "era": true, "drive": true, "country": true}

// TODs are HHMM, as the career data stores them.
var TODs = []int64{600, 900, 1200, 1500, 1800, 2000, 2300, 300}

var weatherWords = map[string]string{"Clear": "clear", "CloudyA": "cloudy", "CloudyB": "overcast", "CloudyFog": "foggy", "ThreateningFog": "misty"}

// attrColumn maps a CarRestrictions attribute to (v_cars column, SQL operator) [names follow the exe's option table, inferred].
var attrColumn = map[string][2]string{"CarClassId": {"class_id", "="}, "BodyFamilyId": {"body_id", "="}, "MakeId": {"make_id", "="},
	"ModelId": {"model_id", "="}, "YearMin": {"year", ">="}, "YearMax": {"year", "<="}, "DriveTypeId": {"drive_id", "="}, "CountryId": {"country_id", "="}}

// Attrs are CarRestrictions attributes in the order they were added (the order is stored and shown).
type Attrs = pyjson.Obj

func attrsWith(a Attrs, k string, v int64) Attrs {
	out := append(Attrs(nil), a...)
	out.Set(k, v)
	return out
}

func attrsEqual(a, b Attrs) bool {
	if len(a) != len(b) {
		return false
	}
	for _, p := range a {
		v, ok := b.Get(p.K)
		if !ok || v != p.V {
			return false
		}
	}
	return true
}

func attrsWithout(a Attrs, k string) Attrs {
	var out Attrs
	for _, p := range a {
		if p.K != k {
			out = append(out, p)
		}
	}
	return out
}

func whereFor(attrs Attrs) (string, []any) {
	var cond []string
	var args []any
	for _, p := range attrs {
		c := attrColumn[p.K]
		cond = append(cond, c[0]+" "+c[1]+" ?")
		args = append(args, p.V)
	}
	if len(cond) == 0 {
		return "1=1", nil
	}
	return strings.Join(cond, " AND "), args
}

// TodWord is a natural word for the time of day (HHMM).
func TodWord(tod int64) string {
	h := (tod / 100) % 24
	for _, w := range []struct {
		lo, hi int64
		word   string
	}{{0, 3, "midnight"}, {3, 6, "pre-dawn"}, {6, 9, "morning"}, {9, 11, "late morning"}, {11, 14, "midday"}, {14, 17, "afternoon"}, {17, 20, "evening"}, {20, 22, "night"}, {22, 24, "midnight"}} {
		if w.lo <= h && h < w.hi {
			return w.word
		}
	}
	return "day"
}

// TodSeconds converts HHMM to seconds since midnight: the unit of GameOptions.TimeOfDay (verified in game: 1200 started at ~00:20).
func TodSeconds(tod int64) int64 { return (tod/100)*3600 + (tod%100)*60 }

// Conditions is 'a stormy midday', 'a clear night', 'an overcast day', 'morning' ... or "" (ok=false).
func Conditions(tod *int64, weatherAdj *string) (string, bool) {
	if tod == nil && weatherAdj == nil {
		return "", false
	}
	if tod == nil {
		return *weatherAdj + " skies", true
	}
	word := TodWord(*tod)
	if weatherAdj == nil {
		return word, true
	}
	art := "a"
	if strings.ContainsRune("aeiou", rune((*weatherAdj)[0])) {
		art = "an"
	}
	return art + " " + *weatherAdj + " " + word, true
}

// Names are the catalog's names for tracks, makes, bodies, countries, drive types and cars.
type Names struct {
	Track                                                      map[int64][2]string // id -> (name, short name)
	Make, Body, Country, Drive, Car                            map[int64]string
	trackOrder, makeOrder, bodyOrder, countryOrder, driveOrder []int64
}

// Theme is a candidate restriction.
type Theme struct {
	Kind, Title, Phrase, BasePhrase string
	Attrs                           Attrs
	Cars                            int64
	Cls                             *int64
	Avoid                           bool
}

// Event is one Monthly Rivals event.
type Event struct {
	ID, Track, Laps              int64
	Length                       float64
	Kind                         string
	Attrs                        Attrs
	Cls                          *int64
	Name, TrackName, Description string
	Tod, Weather                 *int64
	Wetness, Precip              *float64
	Phrase                       string
}

// Catalog is the event catalog.
type Catalog struct {
	Cars     *cardb.CarDb
	Database *db.DB // state: meta + rival_events (nil = nothing is stored)
	// Today gives the date that picks the rotation (tests replace it).
	Today func() time.Time

	mu         sync.Mutex
	names      *Names
	themes     []*Theme
	haveThemes bool
	usable     []*Theme
	haveUsable bool
	rentals    map[int64]bool
	haveRent   bool
	cache      *eventCache
	serial     *serialCache
	combos     [][3]any
	haveCombos bool
}

type eventCache struct {
	month, n int64
	events   []*Event
}

type serialCache struct {
	month, n int64
	at       time.Time
}

// New makes a catalog on cars (the game catalog) and the state database.
func New(cars *cardb.CarDb, database *db.DB) *Catalog {
	return &Catalog{Cars: cars, Database: database, Today: time.Now}
}

func (c *Catalog) q(sql string, args ...any) ([]db.Row, error) { return c.Cars.DB.Query(sql, args...) }

// Names reads the catalog names once.
func (c *Catalog) Names() (*Names, error) {
	if c.names != nil {
		return c.names, nil
	}
	n := &Names{Track: map[int64][2]string{}, Make: map[int64]string{}, Body: map[int64]string{}, Country: map[int64]string{}, Drive: map[int64]string{}, Car: map[int64]string{}}
	rows, err := c.q("SELECT id, name, short_name FROM v_tracks")
	if err != nil {
		return nil, err
	}
	for i := range rows {
		r := &rows[i]
		id := r.IntAt(0)
		n.Track[id] = [2]string{r.StrAt(1), r.StrAt(2)}
		n.trackOrder = append(n.trackOrder, id)
	}
	for _, t := range []struct {
		sql   string
		m     map[int64]string
		order *[]int64
	}{{"SELECT id, name FROM v_makes", n.Make, &n.makeOrder}, {"SELECT id, name FROM v_bodies", n.Body, &n.bodyOrder}, {"SELECT id, name FROM v_countries", n.Country, &n.countryOrder},
		{"SELECT id, name FROM v_drive_types", n.Drive, &n.driveOrder}, {"SELECT id, name FROM v_cars", n.Car, new([]int64)}} {
		rows, err := c.q(t.sql)
		if err != nil {
			return nil, err
		}
		for i := range rows {
			id := rows[i].IntAt(0)
			if _, dup := t.m[id]; !dup {
				*t.order = append(*t.order, id)
			}
			t.m[id] = rows[i].StrAt(1)
		}
	}
	c.names = n
	return n, nil
}

// CarFor is (car id, class id, PI, top speed m/s) of a car a restriction allows.
type CarFor = cardb.PoolCar

// CarsFor lists the cars a restriction allows (all cars for no attributes), by PI.
func (c *Catalog) CarsFor(attrs Attrs) ([]CarFor, error) {
	where, args := whereFor(attrs)
	rows, err := c.q("SELECT id, class_id, pi, COALESCE(top_speed_ms, 0) FROM v_cars WHERE "+where+" ORDER BY pi", args...)
	if err != nil {
		return nil, err
	}
	out := make([]CarFor, len(rows))
	for i := range rows {
		out[i] = CarFor{ID: rows[i].IntAt(0), Class: rows[i].IntAt(1), PI: rows[i].FloatAt(2), Top: rows[i].FloatAt(3)}
	}
	return out, nil
}

// Rentals are the car ids of the game's RentalCars table (empty when the catalog has none).
func (c *Catalog) Rentals() map[int64]bool {
	if c.haveRent {
		return c.rentals
	}
	c.haveRent = true
	c.rentals = map[int64]bool{}
	rows, err := c.q("SELECT CarId FROM RentalCars")
	if err != nil {
		log.Warn(fmt.Sprintf("RentalCars could not be read from the catalog, so events are generated without a rental check and may offer nobody a car: %v", err))
		return c.rentals
	}
	for i := range rows {
		c.rentals[rows[i].IntAt(0)] = true
	}
	return c.rentals
}

// UsableThemes are the themes a new event may use: the class can be enforced and the car selector offers a rental to anybody who owns no fitting
// car (2 rentals, or all cars of a smaller theme). Without rentals in the catalog, or when no theme passes, every theme that can be enforced.
func (c *Catalog) UsableThemes() ([]*Theme, error) {
	if c.haveUsable {
		return c.usable, nil
	}
	all, err := c.Themes()
	if err != nil {
		return nil, err
	}
	var themes []*Theme
	for _, t := range all {
		if !t.Avoid {
			themes = append(themes, t)
		}
	}
	rent := c.Rentals()
	if len(rent) > 0 {
		rset := map[int]bool{}
		for k := range rent {
			rset[int(k)] = true
		}
		var ok []*Theme
		for _, t := range themes {
			cars, err := c.CarsFor(t.Attrs)
			if err != nil {
				return nil, err
			}
			ids := make([]int, len(cars))
			for i, x := range cars {
				ids[i] = int(x.ID)
			}
			if rentals.Enough(ids, rset, rentals.RentalMin) {
				ok = append(ok, t)
			}
		}
		if len(ok) == 0 {
			log.Warn(fmt.Sprintf("no theme has %d rentals in the catalog's %d RentalCars rows: all %d themes stay in use and some events will offer a player without a fitting car nothing to drive",
				rentals.RentalMin, len(rent), len(themes)))
		}
		if len(ok) > 0 {
			themes = ok
		}
	}
	c.usable, c.haveUsable = themes, true
	return themes, nil
}

func (c *Catalog) count(attrs Attrs) (int64, *int64, error) {
	where, args := whereFor(attrs)
	r, err := c.Cars.DB.One("SELECT COUNT(*), AVG(class_id) FROM v_cars WHERE "+where, args...)
	if err != nil {
		return 0, nil, err
	}
	var avg *int64
	if !r.IsNullAt(1) {
		v := int64(math.RoundToEven(r.FloatAt(1)))
		avg = &v
	}
	return r.IntAt(0), avg, nil
}

// Themes are the candidate restrictions, each matching >= MinCars cars.
func (c *Catalog) Themes() ([]*Theme, error) {
	if c.haveThemes {
		return c.themes, nil
	}
	n, err := c.Names()
	if err != nil {
		return nil, err
	}
	var out []*Theme
	add := func(kind string, attrs Attrs, title, phrase string, cls *int64) error {
		count, avg, err := c.count(attrs)
		if err != nil {
			return err
		}
		if count >= MinCars || kind == "model" && count >= 1 {
			theme := &Theme{Kind: kind, Attrs: attrs, Title: title, Phrase: phrase, Cars: count, Cls: cls}
			if cls == nil {
				theme.Cls = avg
			}
			if ClassEnforced[kind] && cls == nil {
				// the card shows a class (the average of the matching cars); enforce it too, so the car selector agrees with the card
				counts := make([]int64, len(ClassLetters))
				for k := range counts {
					if counts[k], _, err = c.count(attrsWith(attrs, "CarClassId", int64(k))); err != nil {
						return err
					}
				}
				pick := int64(0)
				if avg != nil && *avg >= 0 && int(*avg) < len(counts) && counts[*avg] >= MinCars {
					pick = *avg
				} else {
					for k := range counts { // the average class, else the most common one (the first of equals)
						if counts[k] > counts[pick] {
							pick = int64(k)
						}
					}
				}
				if counts[pick] >= MinCars {
					theme.Attrs, theme.Cars, theme.Cls = attrsWith(attrs, "CarClassId", pick), counts[pick], ip(pick)
					theme.Phrase, theme.BasePhrase = phrase+", "+ClassLetters[pick]+" class", phrase
				} else {
					theme.Avoid = true // the shown class cannot be enforced: no new events use it; kept so old events still resolve
				}
			}
			out = append(out, theme)
		}
		return nil
	}
	total, _, err := c.count(nil)
	if err != nil {
		return nil, err
	}
	out = append(out, &Theme{Kind: "open", Attrs: Attrs{}, Title: "Open Class Dash", Phrase: "Any car", Cars: total, Cls: ip(4)})
	for k := int64(0); k < 5; k++ {
		if err := add("class", Attrs{{K: "CarClassId", V: k}}, ClassLetters[k]+" Class Dash", ClassLetters[k]+" class cars only", ip(k)); err != nil {
			return nil, err
		}
	}
	for _, b := range n.bodyOrder {
		if name := n.Body[b]; name != "" && strings.ToLower(name) != "none" {
			if err := add("body", Attrs{{K: "BodyFamilyId", V: b}}, name+" Cup", name+" cars only", nil); err != nil {
				return nil, err
			}
		}
	}
	for _, m := range n.makeOrder {
		if name := n.Make[m]; name != "" {
			if err := add("make", Attrs{{K: "MakeId", V: m}}, name+" Showdown", name+" cars only", nil); err != nil {
				return nil, err
			}
		}
	}
	for _, e := range eras {
		var attrs Attrs
		if e.lo != nil {
			attrs = append(attrs, pyjson.Pair{K: "YearMin", V: *e.lo})
		}
		if e.hi != nil {
			attrs = append(attrs, pyjson.Pair{K: "YearMax", V: *e.hi})
		}
		if attrs == nil {
			attrs = Attrs{}
		}
		var phrase string
		switch {
		case e.lo != nil && e.hi != nil && *e.lo != 0 && *e.hi != 0:
			phrase = fmt.Sprintf("Cars from %d to %d", *e.lo, *e.hi)
		case e.hi != nil && *e.hi != 0:
			phrase = fmt.Sprintf("Cars up to %d", *e.hi)
		default:
			phrase = fmt.Sprintf("Cars from %d", *e.lo)
		}
		if err := add("era", attrs, e.name+" Cup", phrase, nil); err != nil {
			return nil, err
		}
	}
	for _, d := range n.driveOrder {
		if name := n.Drive[d]; name != "" {
			if err := add("drive", Attrs{{K: "DriveTypeId", V: d}}, name+" Only Trial", name+" cars only", nil); err != nil {
				return nil, err
			}
		}
	}
	for _, cid := range n.countryOrder {
		if name := n.Country[cid]; name != "" {
			if err := add("country", Attrs{{K: "CountryId", V: cid}}, "Made in "+name+" Cup", "Cars built in "+name, nil); err != nil {
				return nil, err
			}
		}
	}
	rows, err := c.q("SELECT model_id, MIN(id) FROM v_cars GROUP BY model_id")
	if err != nil {
		return nil, err
	}
	for i := range rows {
		name := n.Car[rows[i].IntAt(1)]
		if name != "" {
			title := "One-Make: " + name
			if r := []rune(title); len(r) > 60 {
				title = string(r[:60])
			}
			if err := add("model", Attrs{{K: "ModelId", V: rows[i].IntAt(0)}}, title, name+" only", nil); err != nil {
				return nil, err
			}
		}
	}
	c.themes, c.haveThemes = out, true
	return out, nil
}

// ---------------------------------------------------------------- rotation

// Serial is (month index since 2026-01, re-roll counter of that month). now nil = today.
func (c *Catalog) Serial(now *time.Time) (int64, int64, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.serialLocked(now)
}

func (c *Catalog) serialLocked(now *time.Time) (int64, int64, error) {
	d := c.Today()
	if now != nil {
		d = *now
	}
	month := int64(d.Year()-2026)*12 + int64(d.Month()) - 1
	clock := time.Now()
	if now == nil && c.serial != nil && c.serial.month == month && clock.Sub(c.serial.at).Seconds() < SerialCacheSeconds {
		return month, c.serial.n, nil
	}
	n := int64(0)
	if c.Database != nil {
		r, err := c.Database.One("SELECT value FROM meta WHERE key='rival_reroll'")
		if err != nil {
			return 0, 0, err
		}
		if r != nil {
			m, cnt, _ := strings.Cut(r.Str("value"), ":")
			var mv, cv int64
			fmt.Sscan(m, &mv)
			fmt.Sscan(cnt, &cv)
			if mv == month {
				n = cv
			}
		}
	}
	n = min(n, 9)
	if now == nil {
		c.serial = &serialCache{month, n, clock}
	}
	return month, n, nil
}

// Rotate is the admin re-roll: a different set right now (stays until the next re-roll or month).
func (c *Catalog) Rotate() ([]*Event, error) {
	c.mu.Lock()
	month, n, err := c.serialLocked(nil)
	if err != nil {
		c.mu.Unlock()
		return nil, err
	}
	if c.Database != nil {
		err = c.Database.Do(func(t *db.Tx) error {
			_, err := t.Exec("INSERT INTO meta (key, value) VALUES ('rival_reroll', ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value", fmt.Sprintf("%d:%d", month, min(n+1, 9)))
			return err
		})
		if err != nil {
			c.mu.Unlock()
			return nil, err
		}
	}
	c.cache, c.serial = nil, nil
	c.mu.Unlock()
	return c.Events()
}

func eventOf(r *db.Row) (*Event, error) {
	pv, err := pyjson.Parse(r.Str("attrs"))
	if err != nil {
		return nil, err
	}
	attrs, _ := pv.(pyjson.Obj)
	e := &Event{ID: r.Int("id"), Track: r.Int("track"), Laps: r.Int("laps"), Length: r.Float("length"), Kind: r.Str("kind"), Attrs: attrs,
		Name: r.Str("name"), TrackName: r.Str("trackname"), Description: r.Str("description")}
	for i := range e.Attrs { // json numbers come back as float64 / int64 depending on the text; the attributes are whole numbers
		if f, ok := e.Attrs[i].V.(float64); ok {
			e.Attrs[i].V = int64(f)
		}
	}
	if !r.IsNull("cls") {
		e.Cls = ip(r.Int("cls"))
	}
	if !r.IsNull("tod") {
		e.Tod = ip(r.Int("tod"))
	}
	if !r.IsNull("weather") {
		e.Weather = ip(r.Int("weather"))
	}
	if !r.IsNull("wetness") {
		v := r.Float("wetness")
		e.Wetness = &v
	}
	if !r.IsNull("precip") {
		v := r.Float("precip")
		e.Precip = &v
	}
	return e, nil
}

// Events are this rotation's events: loaded from the database, or generated once and stored.
func (c *Catalog) Events() ([]*Event, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	month, n, err := c.serialLocked(nil)
	if err != nil {
		return nil, err
	}
	if c.cache != nil && c.cache.month == month && c.cache.n == n {
		return c.cache.events, nil
	}
	if c.Database != nil {
		rows, err := c.Database.Query("SELECT * FROM rival_events WHERE month=? AND reroll=? ORDER BY idx", month, n)
		if err != nil {
			return nil, err
		}
		if len(rows) > 0 {
			ev := make([]*Event, len(rows))
			for i := range rows {
				if ev[i], err = eventOf(&rows[i]); err != nil {
					return nil, err
				}
			}
			c.cache = &eventCache{month, n, ev}
			return ev, nil
		}
	}
	ev, err := c.Generate(month, n)
	if err != nil {
		return nil, err
	}
	if c.Database != nil && len(ev) > 0 {
		err = c.Database.Do(func(t *db.Tx) error {
			for k, e := range ev {
				attrs, _ := pyjson.Dumps(e.Attrs, pyjson.Opts{})
				if _, err := t.Exec("INSERT OR IGNORE INTO rival_events (id, month, reroll, idx, track, laps, length, kind, attrs, cls, "+
					"name, trackname, description, created, tod, weather, wetness, precip) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
					e.ID, month, n, k, e.Track, e.Laps, e.Length, e.Kind, attrs, ptr(e.Cls), e.Name, e.TrackName, e.Description, float64(time.Now().UnixNano())/1e9,
					ptr(e.Tod), ptr(e.Weather), fptr(e.Wetness), fptr(e.Precip)); err != nil {
					return err
				}
			}
			return nil
		})
		if err != nil {
			return nil, err
		}
	}
	c.cache = &eventCache{month, n, ev}
	return ev, nil
}

func ptr(p *int64) any {
	if p == nil {
		return nil
	}
	return *p
}

func fptr(p *float64) any {
	if p == nil {
		return nil
	}
	return *p
}

// WeatherCombos are the (preset id, environment wetness, precipitation) combos the game's own career events use (known-good).
func (c *Catalog) weatherCombos() ([][3]any, error) {
	if c.haveCombos {
		return c.combos, nil
	}
	rows, err := c.q("SELECT DISTINCT WeatherPreset, WeatherEnvironmentWetness, WeatherPrecipitationLevel FROM AnthemEventRaces " +
		"WHERE CustomWeather=1 AND WeatherPreset IS NOT NULL ORDER BY 1, 2, 3")
	if err != nil {
		return nil, err
	}
	for i := range rows {
		r := &rows[i]
		var w, p any
		if !r.IsNullAt(1) {
			w = r.FloatAt(1)
		}
		if !r.IsNullAt(2) {
			p = r.FloatAt(2)
		}
		c.combos = append(c.combos, [3]any{r.IntAt(0), w, p})
	}
	c.haveCombos = true
	return c.combos, nil
}

// WeatherWord is an adjective for a weather preset ('stormy', 'foggy', 'ominous', ...).
func (c *Catalog) WeatherWord(preset int64, precip *float64) (string, error) {
	r, err := c.Cars.DB.One("SELECT Name FROM WeatherPreset WHERE Id=?", preset)
	if err != nil {
		return "", err
	}
	name := fmt.Sprint(preset)
	if r != nil {
		name = r.StrAt(0)
	}
	if w, ok := weatherWords[name]; ok {
		return w, nil
	}
	if strings.Contains(name, "Tropical") {
		return "tropical stormy", nil
	}
	if strings.Contains(name, "Fog") {
		return "foggy and stormy", nil
	}
	if precip != nil && *precip > 0.3 {
		return "stormy", nil
	}
	return "ominous", nil
}

func (c *Catalog) phrase(kind string, attrs Attrs) (string, bool, error) {
	themes, err := c.Themes()
	if err != nil {
		return "", false, err
	}
	for _, t := range themes {
		if t.Kind == kind && attrsEqual(t.Attrs, attrs) {
			return t.Phrase, true, nil
		}
	}
	plain := attrsWithout(attrs, "CarClassId") // events stored before the class was enforced
	for _, t := range themes {
		if t.Kind == kind && attrsEqual(attrsWithout(t.Attrs, "CarClassId"), plain) {
			if t.BasePhrase != "" {
				return t.BasePhrase, true, nil
			}
			return t.Phrase, true, nil
		}
	}
	return "", false, nil
}

// EnforceStoredClasses enforces the advertised class on legacy themed events. MIN_CARS only selects new themes: owned cars can be upgraded, so a
// small stock pool must not leave an existing event unrestricted. Preserves the advertised class and event id. Idempotent.
func (c *Catalog) EnforceStoredClasses() (int, error) {
	if c.Database == nil {
		return 0, nil
	}
	changed := 0
	err := c.Database.Do(func(t *db.Tx) error {
		rows, err := t.Query("SELECT * FROM rival_events")
		if err != nil {
			return err
		}
		for i := range rows {
			e, err := eventOf(&rows[i])
			if err != nil {
				return err
			}
			if !ClassEnforced[e.Kind] || hasKey(e.Attrs, "CarClassId") || e.Cls == nil {
				continue
			}
			attrs, _ := pyjson.Dumps(attrsWith(e.Attrs, "CarClassId", *e.Cls), pyjson.Opts{})
			if _, err := t.Exec("UPDATE rival_events SET attrs=?, cls=? WHERE id=?", attrs, *e.Cls, rows[i].Int("id")); err != nil {
				return err
			}
			changed++
		}
		return nil
	})
	c.mu.Lock()
	c.cache = nil
	c.mu.Unlock()
	return changed, err
}

func hasKey(a Attrs, k string) bool { _, ok := a.Get(k); return ok }

// RefreshTexts recomputes every stored event's description from its stored fields (wording fixes reach old rotations; ids, tracks, restrictions
// and conditions never change). Returns the number of rows updated.
func (c *Catalog) RefreshTexts() (int, error) {
	if c.Database == nil {
		return 0, nil
	}
	changed := 0
	err := c.Database.Do(func(t *db.Tx) error {
		rows, err := t.Query("SELECT * FROM rival_events")
		if err != nil {
			return err
		}
		for i := range rows {
			e, err := eventOf(&rows[i])
			if err != nil {
				return err
			}
			text, err := c.Describe(e, "")
			if err != nil {
				return err
			}
			if text != rows[i].Str("description") {
				if _, err := t.Exec("UPDATE rival_events SET description=? WHERE id=?", text, rows[i].Int("id")); err != nil {
					return err
				}
				changed++
			}
		}
		return nil
	})
	c.mu.Lock()
	c.cache = nil
	c.mu.Unlock()
	return changed, err
}

// Describe is the event description text from its stored fields (tname "" = look the track name up).
func (c *Catalog) Describe(e *Event, tname string) (string, error) {
	names, err := c.Names()
	if err != nil {
		return "", err
	}
	full := tname
	if full == "" {
		full = names.Track[e.Track][0]
	}
	if full == "" {
		full = fmt.Sprintf("Circuit %d", e.Track)
	}
	theme := ""
	switch e.Kind {
	case "open":
		theme = "Any car"
	case "class":
		letter := ""
		if e.Cls != nil {
			letter = ClassLetters[*e.Cls]
		}
		theme = letter + " class cars only"
	}
	phrase := e.Phrase
	if phrase == "" {
		p, _, err := c.phrase(e.Kind, e.Attrs)
		if err != nil {
			return "", err
		}
		phrase = p
	}
	if phrase == "" {
		phrase = theme
	}
	var adj *string
	if e.Weather != nil {
		w, err := c.WeatherWord(*e.Weather, e.Precip)
		if err != nil {
			return "", err
		}
		adj = &w
	}
	parts := []string{fmt.Sprintf("One lap of %s (%.1f km).", full, e.Length/1000)}
	if phrase != "" {
		parts = append(parts, phrase+".")
	}
	if cond, ok := Conditions(e.Tod, adj); ok && cond != "" {
		parts = append(parts, "Conditions: "+cond+".")
	}
	parts = append(parts, "Beat the rival's time.")
	text := strings.Join(parts, " ")
	if r := []rune(text); len(r) > 500 {
		text = string(r[:500])
	}
	return text, nil
}

// conditions draws (time of day HHMM or nil, weather preset or nil, wetness, precipitation). Verified in game (2026-10-18): the stored HHMM is
// converted to seconds for GameOptions.TimeOfDay and the weather attributes are honoured.
func (c *Catalog) conditions(rng *pyrand.Random) (*int64, *int64, *float64, *float64, error) {
	var tod *int64
	if rng.Float64() < 0.7 {
		tod = ip(TODs[rng.Intn(len(TODs))])
	}
	combos, err := c.weatherCombos()
	if err != nil {
		return nil, nil, nil, nil, err
	}
	if len(combos) > 0 && rng.Float64() < 0.5 {
		w := combos[rng.Intn(len(combos))]
		preset := w[0].(int64)
		var wet, pr *float64
		if v, ok := w[1].(float64); ok {
			wet = &v
		}
		if v, ok := w[2].(float64); ok {
			pr = &v
		}
		return tod, &preset, wet, pr, nil
	}
	return tod, nil, nil, nil, nil
}

// Generate makes the events of a rotation (month index, re-roll counter), seeded by both.
func (c *Catalog) Generate(month, n int64) ([]*Event, error) {
	rng := pyrand.NewString(fmt.Sprintf("fh2mp-rivals-%d-%d", month, n))
	names, err := c.Names()
	if err != nil {
		return nil, err
	}
	rows, err := c.q("SELECT id, route, length_m FROM v_tracks WHERE active=1 AND closed=1 AND length_m BETWEEN 1500 AND 3700 ORDER BY id")
	if err != nil {
		return nil, err
	}
	type trk struct {
		id     int64
		length float64
	}
	seen := map[string]bool{}
	var tracks []trk
	for i := range rows {
		route := fmt.Sprint(rows[i].Any("route"))
		if !seen[route] {
			seen[route] = true
			tracks = append(tracks, trk{rows[i].IntAt(0), rows[i].FloatAt(2)})
		}
	}
	pyrand.Shuffle(rng, tracks)
	themes, err := c.UsableThemes()
	if err != nil {
		return nil, err
	}
	kindSet := map[string]bool{}
	for _, t := range themes {
		kindSet[t.Kind] = true
	}
	kinds := make([]string, 0, len(kindSet))
	for k := range kindSet {
		kinds = append(kinds, k)
	}
	sort.Strings(kinds)
	pyrand.Shuffle(rng, kinds)
	var events []*Event
	used := map[*Theme]bool{}
	for k := 0; k < len(tracks) && k < EventsPerRotation; k++ {
		tid, length := tracks[k].id, tracks[k].length
		kind := kinds[k%len(kinds)]
		var pool []*Theme
		for _, t := range themes {
			if t.Kind == kind && !used[t] {
				pool = append(pool, t)
			}
		}
		if len(pool) == 0 {
			pool = themes
		}
		theme := pool[rng.Intn(len(pool))]
		used[theme] = true
		tn := names.Track[tid]
		tname := tn[0]
		if tname == "" {
			tname = fmt.Sprintf("Circuit %d", tid)
		}
		tname = trunc(tname, 60)
		tod, weather, wetness, precip, err := c.conditions(rng)
		if err != nil {
			return nil, err
		}
		short := tn[1]
		if short == "" {
			short = tname
		}
		e := &Event{ID: IDBase + month*160 + n*16 + int64(k), Track: tid, Laps: 1, Length: length, Kind: theme.Kind, Attrs: theme.Attrs, Cls: theme.Cls,
			Name: trunc(theme.Title+": "+short, 120), TrackName: tname, Phrase: theme.Phrase, Tod: tod, Weather: weather, Wetness: wetness, Precip: precip}
		if e.Description, err = c.Describe(e, tname); err != nil {
			return nil, err
		}
		events = append(events, e)
	}
	return events, nil
}

func trunc(s string, n int) string {
	r := []rune(s)
	if len(r) > n {
		return string(r[:n])
	}
	return s
}

// ByID is any stored event (also from earlier rotations, so old boards, ghosts and notifications keep resolving); nil = none.
func (c *Catalog) ByID(id int64) (*Event, error) {
	ev, err := c.Events()
	if err != nil {
		return nil, err
	}
	for _, e := range ev {
		if e.ID == id {
			return e, nil
		}
	}
	if c.Database != nil {
		r, err := c.Database.One("SELECT * FROM rival_events WHERE id=?", id)
		if err != nil || r == nil {
			return nil, err
		}
		return eventOf(r)
	}
	return nil, nil
}
