package catalog

import (
	"database/sql"
	"fmt"
	"math"
	"os"
	"sort"
	"strings"

	_ "modernc.org/sqlite" // pure-Go SQLite driver ("sqlite"), read-only use
)

// ownerColumns: owner column -> what it names; CarBody ids are car id x 1000.
var ownerColumns = []struct {
	Col  string
	Kind string
	Div  int64
}{{"Ordinal", "car", 1}, {"CarBodyID", "car", 1000}, {"CarbodyId", "car", 1000}, {"EngineID", "engine", 1}, {"DrivetrainID", "drivetrain", 1},
	{"DrivetrainId", "drivetrain", 1}, {"MotorID", "motor", 1}}

// PartData is read_parts' result: the part tables of a game database.
type PartData struct {
	IDs      map[string][]uint32
	Kind     map[string]string
	Owner    map[string]map[uint32]int64
	Stock    map[string]map[int64]uint32
	Installs map[string]map[uint32]int64
}

// OpenRO opens a SQLite file read-only.
func OpenRO(path string) (*sql.DB, error) {
	return sql.Open("sqlite", "file:"+strings.ReplaceAll(path, "\\", "/")+"?mode=ro")
}

func floorDiv(a, b int64) int64 {
	q := a / b
	if (a%b != 0) && ((a < 0) != (b < 0)) {
		q--
	}
	return q
}

// ReadParts is part_tables.read_parts.
func ReadParts(db *sql.DB) (*PartData, error) {
	pd := &PartData{IDs: map[string][]uint32{}, Kind: map[string]string{}, Owner: map[string]map[uint32]int64{},
		Stock: map[string]map[int64]uint32{}, Installs: map[string]map[uint32]int64{}}
	rows, err := db.Query(`select name from sqlite_master where type='table' and name like 'List_Upgrade%'`)
	if err != nil {
		return nil, err
	}
	var tables []string
	for rows.Next() {
		var t string
		if err := rows.Scan(&t); err != nil {
			rows.Close()
			return nil, err
		}
		tables = append(tables, t)
	}
	rows.Close()
	for _, t := range tables {
		cols, err := columns(db, t)
		if err != nil {
			return nil, err
		}
		key := ""
		if cols["Id"] {
			key = "Id"
		} else if cols["ID"] {
			key = "ID"
		}
		if key == "" {
			continue
		}
		name := strings.TrimPrefix(t, "List_Upgrade")
		r, err := db.Query(fmt.Sprintf(`select "%s" from "%s"`, key, t))
		if err != nil {
			return nil, err
		}
		var ids []uint32
		for r.Next() {
			var v int64
			if err := r.Scan(&v); err != nil {
				r.Close()
				return nil, err
			}
			ids = append(ids, uint32(v))
		}
		r.Close()
		sort.Slice(ids, func(i, j int) bool { return int64(int32(ids[i])) < int64(int32(ids[j])) })
		pd.IDs[name] = ids
		oc := -1
		for i, c := range ownerColumns {
			if cols[c.Col] {
				oc = i
				break
			}
		}
		if oc < 0 {
			continue
		}
		col := ownerColumns[oc]
		pd.Kind[name] = col.Kind
		stockExpr := "Level = 0"
		if cols["IsStock"] {
			stockExpr = "IsStock"
		}
		r, err = db.Query(fmt.Sprintf(`select "%s", "%s", %s from "%s"`, key, col.Col, stockExpr, t))
		if err != nil {
			return nil, err
		}
		type row struct {
			id, owner int64
			stock     bool
		}
		var rs []row
		for r.Next() {
			var id, o int64
			var s sql.NullInt64
			if err := r.Scan(&id, &o, &s); err != nil {
				r.Close()
				return nil, err
			}
			rs = append(rs, row{id, o, s.Int64 != 0})
		}
		r.Close()
		own := map[uint32]int64{}
		for _, x := range rs {
			own[uint32(x.id)] = floorDiv(x.owner, col.Div)
		}
		pd.Owner[name] = own
		sort.Slice(rs, func(i, j int) bool {
			if rs[i].id != rs[j].id {
				return rs[i].id < rs[j].id
			}
			return rs[i].owner < rs[j].owner
		})
		st := map[int64]uint32{}
		for _, x := range rs {
			if x.stock {
				if _, ok := st[floorDiv(x.owner, col.Div)]; !ok {
					st[floorDiv(x.owner, col.Div)] = uint32(x.id)
				}
			}
		}
		pd.Stock[name] = st
	}
	for _, t := range []struct{ Name, Col string }{{"Engine", "EngineID"}, {"Drivetrain", "DrivetrainID"}, {"Motor", "MotorID"}} {
		r, err := db.Query(fmt.Sprintf(`select Id, "%s" from "List_Upgrade%s"`, t.Col, t.Name))
		if err != nil {
			return nil, err
		}
		m := map[uint32]int64{}
		for r.Next() {
			var id, o int64
			if err := r.Scan(&id, &o); err != nil {
				r.Close()
				return nil, err
			}
			m[uint32(id)] = o
		}
		r.Close()
		pd.Installs[t.Name] = m
	}
	return pd, nil
}

func columns(db *sql.DB, table string) (map[string]bool, error) {
	r, err := db.Query(fmt.Sprintf(`pragma table_info("%s")`, table))
	if err != nil {
		return nil, err
	}
	defer r.Close()
	out := map[string]bool{}
	for r.Next() {
		var cid int
		var name, typ string
		var notnull, pk int
		var dflt sql.NullString
		if err := r.Scan(&cid, &name, &typ, &notnull, &dflt, &pk); err != nil {
			return nil, err
		}
		out[name] = true
	}
	return out, nil
}

// CarRow is read_cars' value: media name and stock top speed in km/h (0 = none).
type CarRow struct {
	Name string
	Kmh  int
}

// ReadCars is part_tables.read_cars.
func ReadCars(db *sql.DB) (map[int]CarRow, error) {
	r, err := db.Query(`select Id, MediaName, "TopSpeed-mph" from Data_Car`)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	out := map[int]CarRow{}
	for r.Next() {
		var id int
		var name sql.NullString
		var mph sql.NullFloat64
		if err := r.Scan(&id, &name, &mph); err != nil {
			return nil, err
		}
		kmh := 0
		if mph.Valid && mph.Float64 != 0 {
			kmh = int(math.RoundToEven(mph.Float64 * 1.609344))
		}
		out[id] = CarRow{name.String, kmh}
	}
	return out, nil
}

// ReadCarChoices is part_tables.read_car_choices; a failure is returned so the caller can log it and use an empty map.
func ReadCarChoices(db *sql.DB) (map[int]ChoiceRow, error) {
	r, err := db.Query(`select Id, ClassID, PerformanceIndex from Data_Car`)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	out := map[int]ChoiceRow{}
	for r.Next() {
		var id, cl int
		var pi sql.NullFloat64
		if err := r.Scan(&id, &cl, &pi); err != nil {
			return nil, err
		}
		out[id] = ChoiceRow{cl, math.RoundToEven(pi.Float64*1e6) / 1e6}
	}
	return out, nil
}

// ReadRentals is part_tables.read_rentals.
func ReadRentals(db *sql.DB) (map[int]struct{}, error) {
	r, err := db.Query(`select CarId from RentalCars`)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	out := map[int]struct{}{}
	for r.Next() {
		var id int
		if err := r.Scan(&id); err != nil {
			return nil, err
		}
		out[id] = struct{}{}
	}
	return out, nil
}

// Extend is parts.extend: adds the rows of another game database to the part tables (rows already there keep their owner and stock) and
// returns the number of part ids added.
func (t *Tables) Extend(pd *PartData) int {
	added := 0
	for name, vals := range pd.IDs {
		old := t.TableIDs[name]
		if old == nil {
			old = map[uint32]struct{}{}
			t.TableIDs[name] = old
		}
		for _, v := range vals {
			if _, ok := old[v]; !ok {
				old[v] = struct{}{}
				added++
			}
		}
	}
	t.AllIDs = map[uint32]struct{}{}
	for _, s := range t.TableIDs {
		for v := range s {
			t.AllIDs[v] = struct{}{}
		}
	}
	for name, k := range pd.Kind {
		if _, ok := t.OwnerKind[name]; !ok {
			t.OwnerKind[name] = k
		}
	}
	for name, m := range pd.Owner {
		d := t.RowOwner[name]
		if d == nil {
			d = map[uint32]int64{}
			t.RowOwner[name] = d
		}
		for k, v := range m {
			if _, ok := d[k]; !ok {
				d[k] = v
			}
		}
	}
	for name, m := range pd.Stock {
		d := t.Stock[name]
		if d == nil {
			d = map[int64]uint32{}
			t.Stock[name] = d
		}
		for k, v := range m {
			if _, ok := d[k]; !ok {
				d[k] = v
			}
		}
	}
	for name, m := range pd.Installs {
		d := t.Installs[name]
		if d == nil {
			d = map[uint32]int64{}
			t.Installs[name] = d
		}
		for k, v := range m {
			if _, ok := d[k]; !ok {
				d[k] = v
			}
		}
	}
	return added
}

// Syncer re-reads FH2MP_SYNC_GAMEDB whenever the file changes (looked at once per Every seconds).
type Syncer struct {
	Path    string
	Every   float64
	checked float64
	key     [2]int64
	haveKey bool
	missing bool
	started bool
}

// NewSyncer watches path; "" disables it.
func NewSyncer(path string) *Syncer { return &Syncer{Path: path, Every: 30.0} }

// Poll is load_sync_catalog: it returns a new catalog when the file (re)loaded, else nil. now is monotonic seconds.
func (s *Syncer) Poll(now float64, cur *Catalog, hasParts bool, log func(string)) *Catalog {
	if s.Path == "" || (s.started && now-s.checked < s.Every) {
		return nil
	}
	s.started, s.checked = true, now
	st, err := os.Stat(s.Path)
	if err != nil {
		if !s.missing {
			s.missing = true
			log(fmt.Sprintf("car database %s: not readable (%v); car packs on top of the stock database are unknown to the checks until it is back", s.Path, err))
		}
		return nil
	}
	s.missing = false
	key := [2]int64{st.ModTime().UnixNano(), st.Size()}
	if s.haveKey && key == s.key {
		return nil
	}
	s.key, s.haveKey = key, true
	db, err := OpenRO(s.Path)
	if err != nil {
		log(fmt.Sprintf("car database %s: not read (%v); the known cars stay as they were", s.Path, err))
		return nil
	}
	defer db.Close()
	cars, err := ReadCars(db)
	var choices map[int]ChoiceRow
	var rented map[int]struct{}
	var pd *PartData
	if err == nil {
		choices, _ = ReadCarChoices(db) // a failure is logged by the caller in Python and leaves {}
		if choices == nil {
			choices = map[int]ChoiceRow{}
		}
		rented, _ = ReadRentals(db)
		if rented == nil {
			rented = map[int]struct{}{}
		}
	}
	if err == nil && hasParts {
		pd, err = ReadParts(db)
	}
	if err != nil {
		log(fmt.Sprintf("car database %s: not read (%v); the known cars stay as they were", s.Path, err))
		return nil
	}
	n := cur.Clone()
	var fresh []int
	for id := range cars {
		if _, ok := n.KnownCars[id]; !ok {
			fresh = append(fresh, id)
		}
	}
	sort.Ints(fresh)
	for id := range cars {
		n.KnownCars[id] = struct{}{}
	}
	for id, c := range cars {
		if c.Kmh != 0 {
			if _, ok := n.TopSpeed[id]; !ok {
				n.TopSpeed[id] = c.Kmh
			}
		}
	}
	for id, cp := range choices {
		if _, ok := n.Choices[id]; !ok {
			n.Choices[id] = cp
		}
	}
	for id := range rented {
		n.Rentals[id] = struct{}{}
	}
	added := 0
	if pd != nil {
		added = n.Parts.Extend(pd)
	}
	shown := fresh
	if len(shown) > 20 {
		shown = shown[:20]
	}
	log(fmt.Sprintf("car database %s: %d cars (%d new: %s), %d new part ids", s.Path, len(cars), len(fresh), pyList(shown), added))
	return n
}

func pyList(a []int) string {
	parts := make([]string, len(a))
	for i, v := range a {
		parts[i] = fmt.Sprint(v)
	}
	return "[" + strings.Join(parts, ", ") + "]"
}
