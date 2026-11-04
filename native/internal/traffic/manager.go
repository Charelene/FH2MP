// Package traffic is the game's networked civilian traffic: manager.go is the port of the game's Traffic::TrafficManager (= server/traffic_manager.py,
// which player holds which car and who owns it), traffic.go the wire and the join sequence (= server/traffic.py).
package traffic

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"

	"fh2mp/internal/diag"
	"fh2mp/internal/omap"
)

var (
	mlog             = diag.Get("traffic")
	mrate            = diag.NewRateLimit(nil, 0)
	exhaustedLogSecs = 30.0
)

// Tunables of the manager.
const (
	MaxCars       = 216
	HardCap       = 18
	BaseCapacity  = 10
	EvictMargin   = 100.0
	RecentMs      = 10000.0
	RecentFactor  = 0.01
	RoundBudget   = 10
	ChangeCost    = 5
	DefaultRadius = 250
)

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func envFloat(name string, def float64) float64 {
	if v, err := strconv.ParseFloat(os.Getenv(name), 64); err == nil {
		return v
	}
	return def
}

// OwnerBonus and ChangeGapMs come from the environment.
var (
	OwnerBonus  = envInt("FH2MP_TRAFFIC_OWNER_BONUS", 20)
	ChangeGapMs = envFloat("FH2MP_TRAFFIC_HOLD_MS", 2000)
)

// Key identifies a player to the manager (the game server uses its peer).
type Key = any

// Port is implemented by keys that have a client port to name them in log lines.
type Port interface{ PortNumber() int }

func name(k Key) any {
	if p, ok := k.(Port); ok {
		return p.PortNumber()
	}
	return k
}

// Pos is a position; nil means unknown.
type Pos = *[3]float64

func dist2(a, b [3]float64) float64 {
	dx, dy, dz := a[0]-b[0], a[1]-b[1], a[2]-b[2]
	return dx*dx + dy*dy + dz*dz
}

// IntSet is a set of small ints iterated in ascending order (CPython's order for small ints).
type IntSet map[int]struct{}

// Add inserts v.
func (s IntSet) Add(v int) { s[v] = struct{}{} }

// Has reports membership.
func (s IntSet) Has(v int) bool { _, ok := s[v]; return ok }

// Sorted lists the members ascending.
func (s IntSet) Sorted() []int {
	out := make([]int, 0, len(s))
	for v := range s {
		out = append(out, v)
	}
	sort.Ints(out)
	return out
}

// Player is a client as the manager sees it.
type Player struct {
	Key      Key
	Universe int
	Pos      Pos
	Radius   int
	Models   map[int]int
	Held     IntSet
	CanTake  IntSet
	Removing IntSet
	Granted  IntSet
	Canceled IntSet
}

func newPlayer(k Key) *Player {
	return &Player{Key: k, Universe: -1, Radius: DefaultRadius, Models: map[int]int{}, Held: IntSet{}, CanTake: IntSet{}, Removing: IntSet{},
		Granted: IntSet{}, Canceled: IntSet{}}
}

// Capacity is 10 + car models in use - cars held.
func (p *Player) Capacity() int {
	n := 0
	for _, c := range p.Models {
		if c > 0 {
			n++
		}
	}
	return BaseCapacity + n - len(p.Held)
}

// Settled: every held car is ready on this client.
func (p *Player) Settled() bool {
	for v := range p.Held {
		if !p.CanTake.Has(v) {
			return false
		}
	}
	return true
}

// Car is one civilian car.
type Car struct {
	VID      int
	Model    int
	Universe int
	Info     any
	Owner    Key
	Pos      Pos
	Holders  []Key
	Changed  float64
	Reserved bool
}

func (c *Car) holds(k Key) bool {
	for _, h := range c.Holders {
		if h == k {
			return true
		}
	}
	return false
}

func (c *Car) addHolder(k Key) {
	if !c.holds(k) {
		c.Holders = append(c.Holders, k)
	}
}

func (c *Car) delHolder(k Key) {
	for i, h := range c.Holders {
		if h == k {
			c.Holders = append(c.Holders[:i:i], c.Holders[i+1:]...)
			return
		}
	}
}

// Out is what goes to a client (the vtable slots of TrafficManager).
type Out interface {
	Add(key Key, car *Car)
	Remove(key Key, vid int)
	Granted(key Key, vid int)
	Canceled(key Key, vid int)
	Withdrawn(key Key)
	Take(key Key, vid int)
	Drop(key Key, vid int)
	Denied(key Key, vid int)
}

type request struct {
	key   Key
	count int
}

// Manager decides who holds and owns what.
type Manager struct {
	out      Out
	Players  *omap.Map[Key, *Player]
	Cars     *omap.Map[int, *Car]
	reserved map[int]Key
	requests map[int][]*request
	universe []int // order universes were first requested in
	placing  *omap.Map[int, Key]
	Refusal  string
	Resent   bool
}

// NewManager builds an empty manager.
func NewManager(out Out) *Manager {
	return &Manager{out: out, Players: omap.New[Key, *Player](), Cars: omap.New[int, *Car](), reserved: map[int]Key{}, requests: map[int][]*request{},
		placing: omap.New[int, Key]()}
}

// Join registers a player.
func (m *Manager) Join(k Key) *Player {
	if p, ok := m.Players.Get(k); ok {
		return p
	}
	p := newPlayer(k)
	m.Players.Set(k, p)
	return p
}

// Leave removes a player silently.
func (m *Manager) Leave(k Key) {
	if p, ok := m.Players.Get(k); ok {
		m.release(p, true)
		m.Players.Delete(k)
	}
}

// Reset: the client threw its traffic away; the tick adds everything again.
func (m *Manager) Reset(k Key) {
	if p, ok := m.Players.Get(k); ok {
		m.release(p, true)
		p.Removing = IntSet{}
	}
}

// SetPosition sets a player's position.
func (m *Manager) SetPosition(k Key, pos [3]float64) {
	if p, ok := m.Players.Get(k); ok {
		p.Pos = &pos
	}
}

// SetRadius sets the radius when positive.
func (m *Manager) SetRadius(k Key, radius int) {
	if radius > 0 {
		if p, ok := m.Players.Get(k); ok {
			p.Radius = radius
		}
	}
}

// SetUniverse: a player leaving a universe loses everything there.
func (m *Manager) SetUniverse(k Key, universe int) {
	p, ok := m.Players.Get(k)
	if !ok {
		return
	}
	if p.Universe != universe {
		if p.Universe != -1 {
			m.release(p, false)
		}
		p.Universe = universe
	}
}

// ModelLoaded notes a model the client has cars of.
func (m *Manager) ModelLoaded(k Key, model int) {
	if p, ok := m.Players.Get(k); ok {
		if _, has := p.Models[model]; !has {
			p.Models[model] = 0
		}
	}
}

// ModelUnloaded forgets a model.
func (m *Manager) ModelUnloaded(k Key, model int) {
	if p, ok := m.Players.Get(k); ok {
		delete(p.Models, model)
	}
}

// Request is OnRequestTraffic: ignored while earlier grants are unanswered.
func (m *Manager) Request(k Key, count int) {
	p, _ := m.Players.Get(k)
	if p == nil {
		return
	}
	if len(p.Granted) > 0 || p.Universe == -1 {
		why := "it is in no universe yet"
		if len(p.Granted) > 0 {
			why = fmt.Sprintf("%d grant(s) not answered yet", len(p.Granted))
		}
		mlog.Debug(fmt.Sprintf("request from %v for %d car(s) ignored: %s", name(k), count, why))
		return
	}
	p.Canceled = IntSet{}
	if _, ok := m.requests[p.Universe]; !ok {
		m.universe = append(m.universe, p.Universe)
	}
	m.requests[p.Universe] = append(m.requests[p.Universe], &request{k, count})
}

// Withdraw is OnWithdrawTrafficRequest.
func (m *Manager) Withdraw(k Key) {
	p, _ := m.Players.Get(k)
	if p == nil {
		return
	}
	for v := range p.Granted {
		delete(m.reserved, v)
		p.Canceled.Add(v)
	}
	p.Granted = IntSet{}
	m.unqueue(k)
	m.out.Withdrawn(k)
}

// WithdrawOne handles a grant the client never answered.
func (m *Manager) WithdrawOne(k Key, vid int) {
	p, _ := m.Players.Get(k)
	if p == nil {
		return
	}
	delete(p.Granted, vid)
	p.Canceled.Add(vid)
	delete(m.reserved, vid)
}

// Spawned is helper_19579a0, the handler SendTrafficSpawned reaches.
func (m *Manager) Spawned(k Key, vid, model int, info any, now float64) bool {
	p, _ := m.Players.Get(k)
	if p == nil {
		return false
	}
	if p.Canceled.Has(vid) {
		delete(p.Canceled, vid)
		m.Refusal = "the grant was cancelled before the spawn arrived"
		return false
	}
	if !p.Granted.Has(vid) {
		m.Refusal = "no grant is outstanding for that id"
		if m.Cars.Has(vid) {
			m.Refusal += " (a car with that id exists already)"
		}
		return false
	}
	delete(p.Granted, vid)
	delete(m.reserved, vid)
	car := &Car{VID: vid, Model: model, Universe: p.Universe, Info: info, Owner: k, Changed: now}
	m.Cars.Set(vid, car)
	m.hold(p, car)
	p.CanTake.Add(vid)
	if len(p.Held) > HardCap {
		if far, _, ok := m.farthest(p); ok {
			c, _ := m.Cars.Get(far)
			m.dropCar(p, c, false, now)
		}
	}
	m.placing.Set(vid, k)
	return true
}

// Removed is OnTrafficRemoved: only clears that player's pending entry.
func (m *Manager) Removed(k Key, vid int) {
	if p, ok := m.Players.Get(k); ok {
		delete(p.Removing, vid)
	}
}

// Despawn is OnRequestDespawn: the car leaves this player only.
func (m *Manager) Despawn(k Key, vid int, now float64) {
	p, _ := m.Players.Get(k)
	car, _ := m.Cars.Get(vid)
	if p != nil && car != nil && p.Held.Has(vid) {
		m.dropCar(p, car, false, now)
	}
}

// CanTake is OnCanTakeOwnership.
func (m *Manager) CanTake(k Key, vid int) {
	p, _ := m.Players.Get(k)
	if p != nil && m.Cars.Has(vid) && p.Held.Has(vid) {
		p.CanTake.Add(vid)
	}
}

// RequestOwnership is OnRequestOwnership (the retail flag check is gone).
func (m *Manager) RequestOwnership(k Key, vid int, now float64) bool {
	p, _ := m.Players.Get(k)
	car, _ := m.Cars.Get(vid)
	m.Resent = false
	if car != nil && car.Owner == k {
		m.Refusal = "the player owns it already, TakeOwnership sent again"
		m.Resent = true
		m.out.Take(k, vid)
		return false
	}
	if car == nil || p == nil || !p.Held.Has(vid) {
		if car == nil {
			m.Refusal = "no such car"
		} else {
			m.Refusal = "the player does not hold it"
		}
		m.out.Denied(k, vid)
		return false
	}
	if now-car.Changed <= ChangeGapMs {
		m.Refusal = fmt.Sprintf("its owner changed %.0f ms ago, minimum %.0f", now-car.Changed, ChangeGapMs)
		m.out.Denied(k, vid)
		return false
	}
	m.handOver(car, k, now, "requested by the new owner")
	return true
}

// CarPosition records the owner's latest position of a car (an all-zero position is ignored).
func (m *Manager) CarPosition(k Key, vid int, pos [3]float64) {
	car, _ := m.Cars.Get(vid)
	if car != nil && car.Owner == k && (pos[0] != 0 || pos[1] != 0 || pos[2] != 0) {
		car.Pos = &pos
	}
}

// Tick is Update 0x1958340, per universe: holders, ownership, then spawns.
func (m *Manager) Tick(now float64) {
	var universes []int
	seen := map[int]bool{}
	m.Players.Each(func(_ Key, p *Player) {
		if p.Universe != -1 && !seen[p.Universe] {
			seen[p.Universe] = true
			universes = append(universes, p.Universe)
		}
	})
	sort.Ints(universes) // CPython iterates a set of small ints in ascending order
	for _, u := range universes {
		var players []*Player
		m.Players.Each(func(_ Key, p *Player) {
			if p.Universe == u {
				players = append(players, p)
			}
		})
		var cars []*Car
		m.Cars.Each(func(_ int, c *Car) {
			if c.Universe == u {
				cars = append(cars, c)
			}
		})
		m.updateHolders(players, cars, now)
		m.rebalance(players, cars, now)
		m.grant(u)
	}
}

func (m *Manager) updateHolders(players []*Player, cars []*Car, now float64) {
	for _, p := range players {
		if p.Pos == nil {
			continue
		}
		reach := float64(p.Radius) * float64(p.Radius)
		for _, car := range cars {
			vid := car.VID
			if car.Pos == nil || p.Held.Has(vid) || p.Removing.Has(vid) || !m.Cars.Has(vid) {
				continue
			}
			if _, ok := p.Models[car.Model]; !ok {
				continue
			}
			d := dist2(*car.Pos, *p.Pos)
			if d >= reach {
				continue
			}
			if p.Capacity() < 1 && p.Models[car.Model] != 0 {
				far, farD, ok := m.farthest(p)
				if !ok {
					// Python: self.cars[None] raises KeyError; the world's tick failure handler logs it.
					panic("traffic: no car to evict")
				}
				if -EvictMargin+farD <= d {
					continue
				}
				c, _ := m.Cars.Get(far)
				m.dropCar(p, c, false, now)
			}
			m.hold(p, car)
			m.out.Add(p.Key, car)
		}
	}
}

type score struct {
	score int
	vid   int
	key   Key
}

func (m *Manager) rebalance(players []*Player, cars []*Car, now float64) {
	byKey := map[Key]*Player{}
	var order []Key
	for _, p := range players {
		byKey[p.Key] = p
		order = append(order, p.Key)
	}
	var scores []score
	ownerScore := map[int]int{}
	for _, car := range cars {
		if len(car.Holders) < 2 || now-car.Changed <= ChangeGapMs {
			continue
		}
		for _, key := range car.Holders {
			p := byKey[key]
			if p == nil || p.Pos == nil || car.Pos == nil {
				continue
			}
			sc := int(math.Pow(dist2(*car.Pos, *p.Pos), 0.5))
			if car.Owner == key {
				sc -= OwnerBonus
				age := now - car.Changed
				if age < RecentMs {
					sc -= int((RecentMs - age) * RecentFactor)
				}
			}
			scores = append(scores, score{sc, car.VID, key})
			if car.Owner == key {
				ownerScore[car.VID] = sc
			}
		}
	}
	sort.SliceStable(scores, func(i, j int) bool {
		if scores[i].score != scores[j].score {
			return scores[i].score < scores[j].score
		}
		return scores[i].vid < scores[j].vid
	})
	done := map[int]bool{}
	for len(scores) > 0 {
		budget := map[Key]int{}
		for _, k := range order {
			budget[k] = RoundBudget
		}
		var rest []score
		for _, e := range scores {
			if budget[e.key] < ChangeCost {
				rest = append(rest, e)
			} else if !done[e.vid] {
				car, _ := m.Cars.Get(e.vid)
				if car.Owner != e.key {
					if !byKey[e.key].Settled() {
						continue
					}
					against := "-"
					if s, ok := ownerScore[e.vid]; ok {
						against = strconv.Itoa(s)
					}
					m.handOver(car, e.key, now, fmt.Sprintf("rebalance: score %d against the owner's %s", e.score, against))
				}
				done[e.vid] = true
				budget[e.key] -= ChangeCost
			}
		}
		scores = rest
	}
}

func (m *Manager) grant(universe int) {
	for _, vid := range m.placing.Keys() {
		c, ok := m.Cars.Get(vid)
		if !ok || c.Pos != nil {
			m.placing.Delete(vid)
		}
	}
	queue := m.requests[universe]
	if len(queue) == 0 {
		return
	}
	i := 0
	for i < len(queue) {
		entry := queue[i]
		p, _ := m.Players.Get(entry.key)
		if p == nil || !m.maySpawn(p) {
			i++
			continue
		}
		vid := -1
		for v := 0; v < MaxCars; v++ {
			if !m.Cars.Has(v) {
				if _, r := m.reserved[v]; !r {
					vid = v
					break
				}
			}
		}
		if vid < 0 {
			if ok, skipped := mrate.Allow("exhausted", exhaustedLogSecs); ok {
				waiting := 0
				for _, q := range m.requests {
					waiting += len(q)
				}
				extra := ""
				if skipped > 0 {
					extra = fmt.Sprintf(" (+%d more since the last line)", skipped)
				}
				mlog.Warn(fmt.Sprintf("no free car id: all %d are in use (%d cars, %d granted and not spawned), %d request(s) wait%s",
					MaxCars, m.Cars.Len(), len(m.reserved), waiting, extra))
			}
			m.requests[universe] = queue
			return
		}
		entry.count--
		if entry.count <= 0 {
			queue = append(queue[:i:i], queue[i+1:]...)
		}
		m.reserved[vid] = p.Key
		p.Granted.Add(vid)
		m.out.Granted(p.Key, vid)
	}
	m.requests[universe] = queue
}

func (m *Manager) maySpawn(p *Player) bool {
	if !p.Settled() || p.Pos == nil {
		return false
	}
	var others []Key
	seen := map[Key]bool{}
	for _, k := range m.placing.Values() {
		if !seen[k] {
			seen[k] = true
			others = append(others, k)
		}
	}
	vids := make([]int, 0, len(m.reserved))
	for v := range m.reserved {
		vids = append(vids, v)
	}
	sort.Ints(vids)
	for _, v := range vids {
		if k := m.reserved[v]; !seen[k] {
			seen[k] = true
			others = append(others, k)
		}
	}
	for _, key := range others {
		q, _ := m.Players.Get(key)
		if q == nil || q == p || q.Pos == nil {
			continue
		}
		if dist2(*q.Pos, *p.Pos) <= float64(q.Radius)*float64(q.Radius)+float64(p.Radius)*float64(p.Radius) {
			return false
		}
	}
	return true
}

// farthest is RebalanceByDistance: the farthest held car, sparing a player's last car of a model.
func (m *Manager) farthest(p *Player) (int, float64, bool) {
	best, bestD, found := 0, -1.0, false
	for _, vid := range p.Held.Sorted() {
		car, _ := m.Cars.Get(vid)
		if car == nil {
			panic("traffic: held car missing")
		}
		if car.Pos != nil && p.Models[car.Model] > 1 {
			d := dist2(*car.Pos, *p.Pos)
			if bestD < d {
				best, bestD, found = vid, d, true
			}
		}
	}
	return best, bestD, found
}

func (m *Manager) hold(p *Player, car *Car) {
	p.Held.Add(car.VID)
	p.Models[car.Model]++
	car.addHolder(p.Key)
}

func (m *Manager) dropCar(p *Player, car *Car, silent bool, now float64) {
	delete(p.Held, car.VID)
	delete(p.CanTake, car.VID)
	car.delHolder(p.Key)
	if _, ok := p.Models[car.Model]; ok {
		p.Models[car.Model]--
	}
	if !silent {
		p.Removing.Add(car.VID)
		m.out.Remove(p.Key, car.VID)
	}
	if car.Owner == p.Key {
		car.Owner = nil
		if next := m.nextOwner(car); next != nil {
			car.Owner = next
			car.Changed = now
			m.out.Take(next, car.VID)
		}
	}
	if len(car.Holders) == 0 {
		m.Cars.Delete(car.VID)
		m.placing.Delete(car.VID)
	}
}

func (m *Manager) nextOwner(car *Car) Key {
	var holders []*Player
	m.Players.Each(func(_ Key, q *Player) {
		if car.holds(q.Key) {
			holders = append(holders, q)
		}
	})
	for _, q := range holders {
		if q.Settled() {
			return q.Key
		}
	}
	for _, q := range holders {
		if q.CanTake.Has(car.VID) {
			return q.Key
		}
	}
	return nil
}

func (m *Manager) handOver(car *Car, k Key, now float64, why string) {
	mlog.Debug(fmt.Sprintf("car %d owner %v -> %v after %.0f ms (%s), %d holders, owner bonus %d m, hold %.0f ms",
		car.VID, name(car.Owner), name(k), now-car.Changed, why, len(car.Holders), OwnerBonus, ChangeGapMs))
	if car.Owner != nil {
		m.out.Drop(car.Owner, car.VID)
	}
	car.Owner, car.Changed = k, now
	m.out.Take(k, car.VID)
}

func (m *Manager) release(p *Player, silent bool) {
	for _, vid := range p.Granted.Sorted() {
		delete(m.reserved, vid)
		if !silent {
			m.out.Canceled(p.Key, vid)
		}
	}
	p.Granted = IntSet{}
	for _, vid := range p.Held.Sorted() {
		car, _ := m.Cars.Get(vid)
		if car != nil {
			m.dropCar(p, car, silent, car.Changed)
		}
	}
	m.unqueue(p.Key)
	for _, vid := range m.placing.Keys() {
		if k, _ := m.placing.Get(vid); k == p.Key {
			m.placing.Delete(vid)
		}
	}
}

func (m *Manager) unqueue(k Key) {
	for u, q := range m.requests {
		var kept []*request
		for _, e := range q {
			if e.key != k {
				kept = append(kept, e)
			}
		}
		m.requests[u] = kept
	}
}
