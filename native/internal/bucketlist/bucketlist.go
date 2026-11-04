// Package bucketlist is the bucket-list co-op (router 12, messages 61-68) (= server/bucketlist.py).
package bucketlist

import (
	"fmt"
	"math"
	"os"
	"strconv"

	"fh2mp/internal/bits"
	"fh2mp/internal/diag"
	"fh2mp/internal/netv2"
)

var log = diag.Get("bucketlist")

// Router constants.
const (
	Router          = 12
	TypePlayerState = 0
	TypeSlotArray   = 1
	IDBits          = 4
	TypeBits        = 2
	VarIDBits       = 3
	KeyIDBits       = 7

	MsgStart       = 61
	MsgEnded       = 62
	MsgSync        = 63
	MsgDesync      = 64
	MsgPartner     = 65
	MsgWaiting     = 66
	MsgStarted     = 67
	MsgStatusEnded = 68

	SlotCount     = 12
	SlotVarIDBits = 6
)

func envFloat(name string, def float64) float64 {
	if v, err := strconv.ParseFloat(os.Getenv(name), 64); err == nil {
		return v
	}
	return def
}

func envInt(name string, def int64) int64 {
	if v, err := strconv.ParseInt(os.Getenv(name), 10, 64); err == nil {
		return v
	}
	return def
}

// Tunables (environment variables, read at start).
var (
	WaitSeconds              = envFloat("FH2MP_BL_WAIT_S", 120)
	StartDelayMs             = envInt("FH2MP_BL_START_DELAY_MS", 3000)
	ReadyState               = envInt("FH2MP_BL_READY_STATE", 2)
	Status                   = envOr("FH2MP_BL_STATUS", "challenge")
	indexWaiter, indexJoiner = indexes()
)

func envOr(name, def string) string {
	if v := os.Getenv(name); v != "" {
		return v
	}
	return def
}

func indexes() (uint64, uint64) {
	switch os.Getenv("FH2MP_BL_INDEX") {
	case "swap":
		return 1, 0
	case "both0":
		return 0, 0
	case "both1":
		return 1, 1
	}
	return 0, 1
}

// PlayerStateVars: id -> (name, width).
var PlayerStateVars = map[uint64]struct {
	Name  string
	Width int
}{0: {"Score", 32}, 1: {"CurrentSlotId", 32}, 2: {"CurrentChallengeId", 32}, 3: {"RequestingPartner", 1}, 4: {"State", 3}, 5: {"ArrivedAtDestination", 1}}

// S32 reinterprets 32 bits as signed.
func S32(v uint64) int64 { return int64(int32(uint32(v))) }

// State is a decoded PlayerState update; values are in wire order.
type State struct {
	Names  []string
	Values map[string]float64 // Score as float, ids as integers, flags as 0/1
}

// DecodePlayerState is decode_player_state.
func DecodePlayerState(size int, payload bits.Int) (*State, error) {
	st := &State{Values: map[string]float64{}}
	pos := 0
	for pos+VarIDBits <= size {
		vid := payload.Field(pos, VarIDBits)
		pos += VarIDBits
		spec, ok := PlayerStateVars[vid]
		if !ok {
			return nil, fmt.Errorf("BucketListPlayerStateSynced has no variable %d", vid)
		}
		if pos+spec.Width > size {
			return nil, fmt.Errorf("truncated BucketListPlayerStateSynced update")
		}
		raw := payload.Field(pos, spec.Width)
		pos += spec.Width
		var v float64
		switch spec.Name {
		case "Score":
			v = float64(math.Float32frombits(uint32(raw)))
		case "CurrentSlotId", "CurrentChallengeId":
			v = float64(S32(raw))
		default:
			v = float64(raw)
		}
		if _, seen := st.Values[spec.Name]; !seen {
			st.Names = append(st.Names, spec.Name)
		}
		st.Values[spec.Name] = v
	}
	return st, nil
}

// Key is an object key as the receiver knows it: its own object (plain id) or another client's (relay key).
type Key struct {
	Own   bool
	ID    uint64
	Owner uint64
}

// WriteKey is write_key.
func WriteKey(w *bits.Writer, k Key) {
	if k.Own {
		w.Bits(0, 1)
		w.Bits(0, 1)
		w.Bits(k.ID, KeyIDBits)
		return
	}
	w.Bits(0, 1)
	w.Bits(1, 1)
	w.Bits(k.ID, KeyIDBits)
	w.Bits(k.Owner, 8)
}

// StartMessage is start_message.
func StartMessage(startMs uint64) netv2.Msg {
	return func(w *bits.Writer) { w.Bits(MsgStart, 7); w.Bits(startMs&0xFFFFFFFF, 32) }
}

// PartnerMessage is partner_message.
func PartnerMessage(key Key, value uint64, found bool, name string) netv2.Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgPartner, 7)
		WriteKey(w, key)
		w.Bits(value, 5)
		if found {
			w.Bits(1, 1)
		} else {
			w.Bits(0, 1)
		}
		r := []rune(name)
		if len(r) > 36 {
			r = r[:36]
		}
		w.String(string(r))
	}
}

// WaitingMessage is waiting_message.
func WaitingMessage(key Key, number int64) netv2.Msg {
	return func(w *bits.Writer) { w.Bits(MsgWaiting, 7); WriteKey(w, key); w.Bits(uint64(number)&0xFFFFFFFF, 32) }
}

// StartedMessage is started_message.
func StartedMessage(a, b Key, number int64) netv2.Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgStarted, 7)
		WriteKey(w, a)
		WriteKey(w, b)
		w.Bits(uint64(number)&0xFFFFFFFF, 32)
	}
}

// StatusEndedMessage is status_ended_message.
func StatusEndedMessage(a, b Key, number int64, ok bool) netv2.Msg {
	return func(w *bits.Writer) {
		w.Bits(MsgStatusEnded, 7)
		WriteKey(w, a)
		WriteKey(w, b)
		w.Bits(uint64(number)&0xFFFFFFFF, 32)
		if ok {
			w.Bits(1, 1)
		} else {
			w.Bits(0, 1)
		}
	}
}

// SlotTable is the server's copy of the slot array: [slot, challenge, owner] per entry, zeros = unused.
type SlotTable struct{ Entries [SlotCount][3]int64 }

// Claim gives `slot` an entry (an existing one is taken over): the changed entry indexes, or nil+false when all are in use.
func (t *SlotTable) Claim(slot, challenge, owner int64) ([]int, bool) {
	idx := -1
	for i, e := range t.Entries {
		if e[0] == slot {
			idx = i
			break
		}
	}
	if idx < 0 {
		for i, e := range t.Entries {
			if e[0] == 0 {
				idx = i
				break
			}
		}
		if idx < 0 || slot == 0 {
			why := fmt.Sprintf("all %d entries are in use", SlotCount)
			if slot == 0 {
				why = "slot id 0 means unused"
			}
			log.Warn(fmt.Sprintf("slot array entry not created for owner %d, slot %d, challenge %d: %s", owner, slot, challenge, why))
			return nil, false
		}
	}
	if t.Entries[idx] == [3]int64{slot, challenge, owner} {
		return []int{}, true
	}
	t.Entries[idx] = [3]int64{slot, challenge, owner}
	return []int{idx}, true
}

// Release frees every entry of this owner and returns the changed indexes.
func (t *SlotTable) Release(owner int64) []int {
	var changed []int
	for i, e := range t.Entries {
		if e[2] == owner && e[0] != 0 {
			changed = append(changed, i)
		}
	}
	for _, i := range changed {
		t.Entries[i] = [3]int64{}
	}
	return changed
}

// Used lists the entries in use.
func (t *SlotTable) Used() []int {
	var out []int
	for i, e := range t.Entries {
		if e[0] != 0 {
			out = append(out, i)
		}
	}
	return out
}

// Writer writes the variables of the given entries in id order.
func (t *SlotTable) Writer(indexes []int) netv2.VarsWriter {
	sorted := append([]int(nil), indexes...)
	for i := 1; i < len(sorted); i++ {
		for j := i; j > 0 && sorted[j] < sorted[j-1]; j-- {
			sorted[j], sorted[j-1] = sorted[j-1], sorted[j]
		}
	}
	entries := t.Entries
	return func(w *bits.Writer) {
		for _, i := range sorted {
			for j, v := range entries[i] {
				w.Bits(uint64(3*i+j), SlotVarIDBits)
				w.Bits(uint64(v)&0xFFFFFFFF, 32)
			}
		}
	}
}

// Member is a peer as the co-op logic sees it.
type Member interface {
	OwnerID() int
	// BLState is the last decoded "State" variable of the peer's PlayerState.
	BLState() (float64, bool)
}

// Send is one message for one receiver.
type Send struct {
	To  Member
	Msg netv2.Msg
}

type waitEntry struct {
	slot, challenge int64
	since           float64
}

type pair struct{ slot, challenge int64 }

// Coop is who waits for a partner and who plays with whom, for one world.
type Coop struct {
	KeyOf   func(subject, receiver Member) (Key, bool)
	ClockMs func() uint64
	Seconds func() float64
	waiting map[Member]waitEntry
	Partner map[Member]Member
	Running map[Member]pair
	Started map[Member]bool
	Slots   SlotTable
	Changed []int
}

// NewCoop builds the co-op state of one world.
func NewCoop(keyOf func(subject, receiver Member) (Key, bool), clockMs func() uint64, seconds func() float64) *Coop {
	return &Coop{KeyOf: keyOf, ClockMs: clockMs, Seconds: seconds, waiting: map[Member]waitEntry{}, Partner: map[Member]Member{},
		Running: map[Member]pair{}, Started: map[Member]bool{}}
}

// Waiting reports whether the peer has a pending request.
func (c *Coop) Waiting(p Member) bool { _, ok := c.waiting[p]; return ok }

// Request is 63. world = every peer in the same world; nameOf names a peer.
func (c *Coop) Request(peer Member, slot, challenge int64, world []Member, nameOf func(Member) string) ([]Send, string) {
	c.Release(peer)
	c.BreakPair(peer)
	if _, ok := c.KeyOf(peer, peer); !ok {
		return nil, "has no bucket-list player state yet, ignored"
	}
	at := c.Seconds()
	for _, q := range world {
		held, ok := c.waiting[q]
		if q == peer || !ok || held.slot != slot || held.challenge != challenge || at-held.since > WaitSeconds {
			continue
		}
		if _, ok := c.KeyOf(q, peer); !ok {
			continue
		}
		if _, ok := c.KeyOf(peer, q); !ok {
			continue
		}
		delete(c.waiting, q)
		c.Release(q)
		c.Partner[peer], c.Partner[q] = q, peer
		c.Running[peer] = pair{slot, challenge}
		c.Running[q] = pair{slot, challenge}
		var out []Send
		for _, t := range []struct {
			me, other Member
			index     uint64
		}{{q, peer, indexWaiter}, {peer, q, indexJoiner}} {
			k, _ := c.KeyOf(t.other, t.me)
			out = append(out, Send{t.me, PartnerMessage(k, t.index, true, nameOf(t.other))})
		}
		out = append(out, c.status(world, "started", peer, q, slot, challenge, false)...)
		return out, fmt.Sprintf("paired with owner %d (slot %d, challenge %d); the start time follows when both are ready", q.OwnerID(), slot, challenge)
	}
	c.waiting[peer] = waitEntry{slot, challenge, at}
	if ch, ok := c.Slots.Claim(slot, challenge, int64(peer.OwnerID())); ok {
		c.Changed = append(c.Changed, ch...)
	}
	return c.status(world, "waiting", peer, nil, slot, challenge, false), fmt.Sprintf("waiting for a partner (slot %d, challenge %d)", slot, challenge)
}

func (c *Coop) status(world []Member, kind string, a, b Member, slot, challenge int64, ok bool) []Send {
	if Status == "off" {
		return nil
	}
	number := challenge
	if Status == "slot" {
		number = slot
	}
	var out []Send
	for _, q := range world {
		ka, okA := c.KeyOf(a, q)
		var kb Key
		okB := true
		if b != nil {
			kb, okB = c.KeyOf(b, q)
		}
		if !okA || (b != nil && !okB) {
			continue
		}
		switch {
		case kind == "waiting" && q != a:
			out = append(out, Send{q, WaitingMessage(ka, number)})
		case kind == "started":
			out = append(out, Send{q, StartedMessage(ka, kb, number)})
		case kind == "ended":
			out = append(out, Send{q, StatusEndedMessage(ka, kb, number, ok)})
		}
	}
	return out
}

// Cancel is 64: the request (and the slot sync) is cancelled; an existing pairing stays.
func (c *Coop) Cancel(peer Member) bool { return c.Release(peer) }

// Release drops the peer's waiting request and its slot array entries; it reports whether it was waiting.
func (c *Coop) Release(peer Member) bool {
	_, was := c.waiting[peer]
	delete(c.waiting, peer)
	c.Changed = append(c.Changed, c.Slots.Release(int64(peer.OwnerID()))...)
	return was
}

// Leave handles a peer that left and returns the partner it was playing with.
func (c *Coop) Leave(peer Member) Member {
	c.Release(peer)
	return c.BreakPair(peer)
}

// StateChanged: a PlayerState update of a paired peer arrived; when both partners are ready both get the start time once.
func (c *Coop) StateChanged(peer Member) ([]Send, string) {
	other, ok := c.Partner[peer]
	if !ok || c.Started[peer] || c.Started[other] {
		return nil, ""
	}
	ps, ok1 := peer.BLState()
	os_, ok2 := other.BLState()
	if !ok1 || !ok2 || int64(ps) != ReadyState || int64(os_) != ReadyState {
		return nil, ""
	}
	start := (c.ClockMs() + uint64(StartDelayMs)) & 0xFFFFFFFF
	if start == 0 {
		start = 1
	}
	c.Started[peer], c.Started[other] = true, true
	return []Send{{peer, StartMessage(start)}, {other, StartMessage(start)}},
		fmt.Sprintf("both ready with owner %d: start %d", other.OwnerID(), start)
}

// BreakPair ends a pairing and returns the former partner.
func (c *Coop) BreakPair(peer Member) Member {
	other, had := c.Partner[peer]
	delete(c.Partner, peer)
	delete(c.Running, peer)
	delete(c.Started, peer)
	if had && c.Partner[other] == peer {
		delete(c.Partner, other)
		delete(c.Running, other)
		delete(c.Started, other)
	}
	if !had {
		return nil
	}
	return other
}

// Ended is 62: the challenge is over for this client.
func (c *Coop) Ended(peer Member, ok bool, world []Member) ([]Send, Member) {
	sc, running := c.Running[peer]
	c.Release(peer)
	other := c.BreakPair(peer)
	if other == nil || !running {
		return nil, other
	}
	return c.status(world, "ended", peer, other, sc.slot, sc.challenge, ok), other
}
