// Package replay is the history the game server keeps of every client's objects and replays to late joiners (= server/replay.py).
package replay

import "fh2mp/internal/bits"

// SeqMod is the 8-bit object sequence modulus (0..254).
const SeqMod = 0xFF

// Entry is one stored object message: (type, type id, sequence, size, payload).
type Entry struct {
	Type, TID, Seq int
	Size           int
	Payload        bits.Int
}

type spec struct {
	idBits int
	widths map[uint64]int
}

// singleVars: router -> variables whose updates are safe to supersede (state, not events).
var singleVars = map[int]spec{1: {8, map[uint64]int{5: 1, 12: 1, 16: 1, 19: 4, 27: 32, 28: 32, 32: 1, 41: 64}}}

// SingleVar returns the variable id when payload (an update of size bits) is exactly one whitelisted variable.
func SingleVar(router, size int, payload bits.Int) (uint64, bool) {
	s, ok := singleVars[router]
	if !ok {
		return 0, false
	}
	vid := payload.Field(0, s.idBits)
	if w, ok := s.widths[vid]; ok && size == s.idBits+w {
		return vid, true
	}
	return 0, false
}

// Compact drops every single-variable update that a later single-variable update of the same variable supersedes.
func Compact(router int, hist []Entry, updateType int) []Entry {
	if _, ok := singleVars[router]; !ok || len(hist) < 3 {
		return append([]Entry(nil), hist...)
	}
	last := map[uint64]int{}
	for i, e := range hist {
		if e.Type == updateType {
			if vid, ok := SingleVar(router, e.Size, e.Payload); ok {
				last[vid] = i
			}
		}
	}
	out := make([]Entry, 0, len(hist))
	for i, e := range hist {
		if e.Type == updateType {
			if vid, ok := SingleVar(router, e.Size, e.Payload); ok && last[vid] != i {
				continue
			}
		}
		out = append(out, e)
	}
	return out
}

// Renumber is the replay of one object for a joiner: sequence numbers count 0, 1, 2 ... (mod 255).
func Renumber(entries []Entry) []Entry {
	out := make([]Entry, len(entries))
	for i, e := range entries {
		e.Seq = i % SeqMod
		out[i] = e
	}
	return out
}

// WireSeq is the sequence number a recipient is sent for the server's n-th message of an object.
func WireSeq(n, shift int) int {
	v := (n - shift) % SeqMod
	if v < 0 {
		v += SeqMod
	}
	return v
}
