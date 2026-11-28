// Package flags is the operator's kill switches: a few named features of the servers that can be switched off without a release.
//
// The switches live in one small JSON file per realm (FH2MP_FEATURES_FILE, one file for the LSP and the game server of a realm):
//
//	{"voice": true, "nos": true, "mt_chat": true, "lobbies": true}
//
// A feature that is not in the file is on, and so is every feature when there is no file or no path: the defaults are what the servers did
// before this package existed. The file is looked at again at most once a second (its modification time and size), so writing it takes
// effect on both servers in about a second, with no restart; deleting it puts every feature back on.
//
// A broken file keeps the last good switches in force, as launcher.json does (the problem is logged once per file version). A key that is not
// one of the features below, or a value that is not true or false, is left out and logged once, and the rest of the file still applies:
// "voice": false next to a typo must still switch voice off.
//
// Every change of the effective switches is logged on one line ("flags: voice off (...)"), so a log shows when and what was switched.
//
// What a switch does when it is off (nothing that is already running is torn down except where it says so):
//
//	voice    the game server stops relaying chat packets between players of a world.
//	nos      NitroSystem's NOS burn is not accepted or shown for anybody (the same as FH2MP_NO_NOS=1).
//	mt_chat  the chat server (MTProto) answers no new connection and closes the ones it has.
//	lobbies  nobody is matched into a lobby or reserves a seat: the matchmaking answer is 503 lobbies_off. Players already in a world stay.
package flags

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"fh2mp/internal/diag"
)

// The features (the keys of the file).
const (
	Voice   = "voice"
	NOS     = "nos"
	MTChat  = "mt_chat"
	Lobbies = "lobbies"
)

// Info describes one feature.
type Info struct{ Name, Does string }

// Known is every feature, in the order the documentation and the status answer give them.
var Known = []Info{
	{Voice, "voice chat relayed between the players of a world (game server)"},
	{NOS, "NitroSystem's NOS burn online (game server)"},
	{MTChat, "the chat server's connections (LSP)"},
	{Lobbies, "matching into lobbies and reserving seats (LSP)"},
}

var log = diag.Get("flags")

const recheck = time.Second

type state struct {
	vals  map[string]bool // only the features the file names
	mtime time.Time
	size  int64
	have  bool // the file existed
}

// Set is one realm's switches.
type Set struct {
	Path string
	Now  func() time.Time // tests

	known map[string]bool
	cur   atomic.Pointer[state]
	next  atomic.Int64 // unix nanoseconds of the next look at the file
	mu    sync.Mutex
	// warned is the file version (mtime and size) whose problems were logged
	warned state
	// bad is the file version that was not valid JSON
	bad state
	// last is the effective switches logged at the previous change
	last map[string]bool
}

// New is the switches read from path ("" = no file: everything on).
func New(path string) *Set {
	s := &Set{Path: path, Now: time.Now, known: map[string]bool{}, last: map[string]bool{}}
	for _, k := range Known {
		s.known[k.Name] = true
		s.last[k.Name] = true
	}
	s.cur.Store(&state{vals: map[string]bool{}})
	return s
}

var (
	defOnce sync.Once
	def     *Set
)

// Default is the realm's switches, read from FH2MP_FEATURES_FILE.
func Default() *Set {
	defOnce.Do(func() { def = New(os.Getenv("FH2MP_FEATURES_FILE")) })
	return def
}

// On says whether a feature is on now. An unknown name is on.
func On(name string) bool { return Default().On(name) }

// On says whether a feature is on now.
func (s *Set) On(name string) bool {
	s.refresh()
	if v, ok := s.cur.Load().vals[name]; ok {
		return v
	}
	return true
}

// Snapshot is every feature's effective switch.
func (s *Set) Snapshot() map[string]bool {
	s.refresh()
	vals := s.cur.Load().vals
	out := make(map[string]bool, len(Known))
	for _, k := range Known {
		v, ok := vals[k.Name]
		out[k.Name] = !ok || v
	}
	return out
}

// Off is the names of the features that are off now, in the order of Known.
func (s *Set) Off() []string {
	snap := s.Snapshot()
	var off []string
	for _, k := range Known {
		if !snap[k.Name] {
			off = append(off, k.Name)
		}
	}
	return off
}

// refresh looks at the file when the last look is more than a second old. It costs one atomic load otherwise, so a per-packet check is fine.
func (s *Set) refresh() {
	if s.Path == "" {
		return
	}
	now := s.Now()
	if now.UnixNano() < s.next.Load() {
		return
	}
	if !s.mu.TryLock() {
		return
	}
	defer s.mu.Unlock()
	s.next.Store(now.Add(recheck).UnixNano())
	s.load()
}

// Reload looks at the file now (tests, and an operator action that has just written it).
func (s *Set) Reload() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.load()
}

func (s *Set) load() {
	cur := s.cur.Load()
	st, err := os.Stat(s.Path)
	if err != nil {
		if cur.have { // the file is gone: back to the defaults
			s.apply(&state{vals: map[string]bool{}})
		}
		return
	}
	if cur.have && st.ModTime().Equal(cur.mtime) && st.Size() == cur.size {
		return
	}
	if s.bad.have && s.bad.mtime.Equal(st.ModTime()) && s.bad.size == st.Size() { // this version did not parse; it was said once
		return
	}
	raw, err := os.ReadFile(s.Path)
	if err != nil {
		s.warnOnce(st, fmt.Sprintf("%s not read, the last good switches stay in force: %v", s.Path, err))
		return
	}
	vals, problems, err := Parse(raw)
	if err != nil {
		s.bad = state{mtime: st.ModTime(), size: st.Size(), have: true}
		s.warnOnce(st, fmt.Sprintf("%s not reloaded, the last good switches stay in force: %v", s.Path, err))
		return
	}
	if len(problems) > 0 {
		s.warnOnce(st, fmt.Sprintf("%s: %s (the rest of the file applies)", s.Path, strings.Join(problems, "; ")))
	}
	s.apply(&state{vals: vals, mtime: st.ModTime(), size: st.Size(), have: true})
}

func (s *Set) warnOnce(st os.FileInfo, msg string) {
	if s.warned.have && s.warned.mtime.Equal(st.ModTime()) && s.warned.size == st.Size() {
		return
	}
	s.warned = state{mtime: st.ModTime(), size: st.Size(), have: true}
	log.Warn("flags: " + msg)
}

// apply makes a state current and logs what changed.
func (s *Set) apply(n *state) {
	s.cur.Store(n)
	var changes []string
	for _, k := range Known {
		v, ok := n.vals[k.Name]
		eff := !ok || v
		if s.last[k.Name] != eff {
			word := "on"
			if !eff {
				word = "off"
			}
			changes = append(changes, k.Name+" "+word)
			s.last[k.Name] = eff
		}
	}
	if len(changes) > 0 {
		log.Info(fmt.Sprintf("flags: %s (%s)", strings.Join(changes, ", "), s.Path))
	}
}

// Parse reads a switches file. A JSON error is an error (the caller keeps the last good switches); unknown keys and values that are not
// booleans are reported in problems and left out.
func Parse(raw []byte) (vals map[string]bool, problems []string, err error) {
	var obj map[string]json.RawMessage
	if err := json.Unmarshal(raw, &obj); err != nil {
		return nil, nil, fmt.Errorf("not a JSON object: %v", err)
	}
	known := map[string]bool{}
	for _, k := range Known {
		known[k.Name] = true
	}
	vals = map[string]bool{}
	keys := make([]string, 0, len(obj))
	for k := range obj {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	for _, k := range keys {
		if !known[k] {
			problems = append(problems, fmt.Sprintf("unknown feature %q", k))
			continue
		}
		var b bool
		if err := json.Unmarshal(obj[k], &b); err != nil {
			problems = append(problems, fmt.Sprintf("%q must be true or false", k))
			continue
		}
		vals[k] = b
	}
	return vals, problems, nil
}

// Update writes the given switches into the file (the other switches stay as they are) and applies them at once. It fails when there is no
// file path. The write is atomic (a temporary file renamed over the file), so a server never reads half of it.
func (s *Set) Update(changes map[string]bool) error {
	if s.Path == "" {
		return fmt.Errorf("no features file is configured (FH2MP_FEATURES_FILE)")
	}
	for k := range changes {
		if !s.known[k] {
			return fmt.Errorf("unknown feature %q", k)
		}
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	merged := map[string]bool{}
	if raw, err := os.ReadFile(s.Path); err == nil {
		if vals, _, perr := Parse(raw); perr == nil {
			merged = vals
		}
	}
	for k, v := range changes {
		merged[k] = v
	}
	out := make(map[string]bool, len(Known))
	for _, k := range Known { // the whole set, in a fixed order, so the file says what is in force
		v, ok := merged[k.Name]
		out[k.Name] = !ok || v
	}
	b, err := json.MarshalIndent(orderedBools{keys: names(), vals: out}, "", " ")
	if err != nil {
		return err
	}
	dir := filepath.Dir(s.Path)
	tmp, err := os.CreateTemp(dir, ".features-*.tmp")
	if err != nil {
		return err
	}
	defer os.Remove(tmp.Name())
	if _, err := tmp.Write(append(b, '\n')); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Chmod(tmp.Name(), 0o644); err != nil {
		return err
	}
	if err := os.Rename(tmp.Name(), s.Path); err != nil {
		return err
	}
	s.load()
	return nil
}

func names() []string {
	out := make([]string, len(Known))
	for i, k := range Known {
		out[i] = k.Name
	}
	return out
}

// orderedBools marshals as a JSON object with its keys in a fixed order.
type orderedBools struct {
	keys []string
	vals map[string]bool
}

func (o orderedBools) MarshalJSON() ([]byte, error) {
	var b strings.Builder
	b.WriteByte('{')
	for i, k := range o.keys {
		if i > 0 {
			b.WriteByte(',')
		}
		fmt.Fprintf(&b, "%q:%t", k, o.vals[k])
	}
	b.WriteByte('}')
	return []byte(b.String()), nil
}

// SetDefault makes s the realm's switches (tests use it to point the servers at a file of their own) and returns what puts the old ones back.
func SetDefault(s *Set) (restore func()) {
	old := Default()
	defOnce.Do(func() {})
	def = s
	return func() { def = old }
}
