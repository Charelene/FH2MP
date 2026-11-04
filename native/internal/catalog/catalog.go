// Package catalog holds the game data the checks use: car policy (server/car_policy.py), part tables (server/part_ids.py) and the Rivals
// world records (server/rival_records.py), embedded as JSON written by tools/golden/dump_catalog.py, plus the rows of the synced car database
// (server/part_tables.py, server/fh2mp_server.py load_sync_catalog). A Catalog is read-only once built; the game server swaps in a new one
// (Clone + Extend) when the synced database changes.
package catalog

import (
	"embed"
	"encoding/json"
	"fmt"
	"sort"
	"strconv"
	"strings"
)

//go:embed data/*.json
var dataFS embed.FS

// PartNone is the "no part" word.
const (
	PartNone  = 0xFFFFFFFF
	PartSlots = 43
)

// ChoiceRow is a stock car's (class id, performance index).
type ChoiceRow struct {
	Class int
	PI    float64
}

// Tables are the part tables of one catalog.
type Tables struct {
	SlotTable map[int]string
	TableIDs  map[string]map[uint32]struct{}
	AllIDs    map[uint32]struct{}
	OwnerKind map[string]string
	RowOwner  map[string]map[uint32]int64
	Stock     map[string]map[int64]uint32
	Installs  map[string]map[uint32]int64
}

// Catalog is the car policy and part data.
type Catalog struct {
	Blocked   map[int]string
	KnownCars map[int]struct{}
	TopSpeed  map[int]int
	Choices   map[int]ChoiceRow
	Rentals   map[int]struct{}
	Parts     *Tables
	Records   map[[2]int]float64
}

func intKeys[T any](m map[string]T) (map[int]T, error) {
	out := make(map[int]T, len(m))
	for k, v := range m {
		n, err := strconv.Atoi(k)
		if err != nil {
			return nil, err
		}
		out[n] = v
	}
	return out, nil
}

func load[T any](name string) (T, error) {
	var v T
	raw, err := dataFS.ReadFile("data/" + name)
	if err != nil {
		return v, err
	}
	return v, json.Unmarshal(raw, &v)
}

// Load builds the embedded catalog.
func Load() (*Catalog, error) {
	pol, err := load[struct {
		Blocked map[string]string            `json:"BLOCKED_CARS"`
		Known   []int                        `json:"KNOWN_CARS"`
		Top     map[string]int               `json:"TOP_SPEED_KMH"`
		Choices map[string][]json.RawMessage `json:"CAR_CHOICES"`
		Rentals []int                        `json:"RENTAL_CARS"`
	}]("car_policy.json")
	if err != nil {
		return nil, err
	}
	c := &Catalog{KnownCars: map[int]struct{}{}, Rentals: map[int]struct{}{}, Choices: map[int]ChoiceRow{}}
	if c.Blocked, err = intKeys(pol.Blocked); err != nil {
		return nil, err
	}
	if c.TopSpeed, err = intKeys(pol.Top); err != nil {
		return nil, err
	}
	for _, id := range pol.Known {
		c.KnownCars[id] = struct{}{}
	}
	for _, id := range pol.Rentals {
		c.Rentals[id] = struct{}{}
	}
	for k, v := range pol.Choices {
		id, err := strconv.Atoi(k)
		if err != nil || len(v) != 2 {
			return nil, fmt.Errorf("car_policy CAR_CHOICES %q", k)
		}
		var row ChoiceRow
		if err := json.Unmarshal(v[0], &row.Class); err != nil {
			return nil, err
		}
		if err := json.Unmarshal(v[1], &row.PI); err != nil {
			return nil, err
		}
		c.Choices[id] = row
	}
	pt, err := load[struct {
		SlotTable map[string]string            `json:"SLOT_TABLE"`
		TableIDs  map[string][]uint32          `json:"TABLE_IDS"`
		OwnerKind map[string]string            `json:"OWNER_KIND"`
		RowOwner  map[string]map[string]int64  `json:"ROW_OWNER"`
		Stock     map[string]map[string]uint32 `json:"STOCK"`
		Installs  map[string]map[string]int64  `json:"INSTALLS"`
	}]("part_ids.json")
	if err != nil {
		return nil, err
	}
	t := &Tables{TableIDs: map[string]map[uint32]struct{}{}, AllIDs: map[uint32]struct{}{}, OwnerKind: pt.OwnerKind,
		RowOwner: map[string]map[uint32]int64{}, Stock: map[string]map[int64]uint32{}, Installs: map[string]map[uint32]int64{}}
	if t.SlotTable, err = intKeys(pt.SlotTable); err != nil {
		return nil, err
	}
	for name, ids := range pt.TableIDs {
		set := make(map[uint32]struct{}, len(ids))
		for _, id := range ids {
			set[id] = struct{}{}
			t.AllIDs[id] = struct{}{}
		}
		t.TableIDs[name] = set
	}
	for name, m := range pt.RowOwner {
		d := make(map[uint32]int64, len(m))
		for k, v := range m {
			n, _ := strconv.ParseUint(k, 10, 32)
			d[uint32(n)] = v
		}
		t.RowOwner[name] = d
	}
	for name, m := range pt.Stock {
		d := make(map[int64]uint32, len(m))
		for k, v := range m {
			n, _ := strconv.ParseInt(k, 10, 64)
			d[n] = v
		}
		t.Stock[name] = d
	}
	for name, m := range pt.Installs {
		d := make(map[uint32]int64, len(m))
		for k, v := range m {
			n, _ := strconv.ParseUint(k, 10, 32)
			d[uint32(n)] = v
		}
		t.Installs[name] = d
	}
	c.Parts = t
	rec, err := load[struct {
		Records map[string]float64 `json:"RECORDS"`
	}]("rival_records.json")
	if err != nil {
		return nil, err
	}
	c.Records = map[[2]int]float64{}
	for k, v := range rec.Records {
		a, b, _ := strings.Cut(k, ",")
		x, _ := strconv.Atoi(a)
		y, _ := strconv.Atoi(b)
		c.Records[[2]int{x, y}] = v
	}
	return c, nil
}

// Clone copies everything that Extend changes.
func (c *Catalog) Clone() *Catalog {
	n := *c
	n.KnownCars = make(map[int]struct{}, len(c.KnownCars))
	for k := range c.KnownCars {
		n.KnownCars[k] = struct{}{}
	}
	n.Rentals = make(map[int]struct{}, len(c.Rentals))
	for k := range c.Rentals {
		n.Rentals[k] = struct{}{}
	}
	n.TopSpeed = make(map[int]int, len(c.TopSpeed))
	for k, v := range c.TopSpeed {
		n.TopSpeed[k] = v
	}
	n.Choices = make(map[int]ChoiceRow, len(c.Choices))
	for k, v := range c.Choices {
		n.Choices[k] = v
	}
	t := *c.Parts
	t.TableIDs = make(map[string]map[uint32]struct{}, len(c.Parts.TableIDs))
	for name, s := range c.Parts.TableIDs {
		cp := make(map[uint32]struct{}, len(s))
		for k := range s {
			cp[k] = struct{}{}
		}
		t.TableIDs[name] = cp
	}
	t.AllIDs = make(map[uint32]struct{}, len(c.Parts.AllIDs))
	for k := range c.Parts.AllIDs {
		t.AllIDs[k] = struct{}{}
	}
	t.OwnerKind = make(map[string]string, len(c.Parts.OwnerKind))
	for k, v := range c.Parts.OwnerKind {
		t.OwnerKind[k] = v
	}
	t.RowOwner = make(map[string]map[uint32]int64, len(c.Parts.RowOwner))
	for k, m := range c.Parts.RowOwner {
		cp := make(map[uint32]int64, len(m))
		for a, b := range m {
			cp[a] = b
		}
		t.RowOwner[k] = cp
	}
	t.Stock = make(map[string]map[int64]uint32, len(c.Parts.Stock))
	for k, m := range c.Parts.Stock {
		cp := make(map[int64]uint32, len(m))
		for a, b := range m {
			cp[a] = b
		}
		t.Stock[k] = cp
	}
	t.Installs = make(map[string]map[uint32]int64, len(c.Parts.Installs))
	for k, m := range c.Parts.Installs {
		cp := make(map[uint32]int64, len(m))
		for a, b := range m {
			cp[a] = b
		}
		t.Installs[k] = cp
	}
	n.Parts = &t
	return &n
}

// SortedKnown lists the known car ids ascending.
func (c *Catalog) SortedKnown() []int {
	out := make([]int, 0, len(c.KnownCars))
	for k := range c.KnownCars {
		out = append(out, k)
	}
	sort.Ints(out)
	return out
}
