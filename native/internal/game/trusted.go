package game

import (
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strconv"

	"fh2mp/internal/diag"
	"fh2mp/internal/pyjson"
)

// Trusted players (operator list, control "trust"/"untrust", query "trusted"): the speed and flight checks log them but never end their
// connection, and Flag never puts them in the cheater pool. The car and part checks still apply: those protect the other players' games.
// The list lives in trusted.json next to cheaters.json ({"<decimal xuid>": <unix seconds when trusted>}) and survives restarts.

func (h *Hub) loadTrusted() {
	if h.TrustFile == "" {
		return
	}
	raw, err := os.ReadFile(h.TrustFile)
	if err != nil {
		if !os.IsNotExist(err) {
			h.TrustError = fmt.Sprintf("%T: %v", err, err)
		}
		return
	}
	var m map[string]float64
	if err := json.Unmarshal(raw, &m); err != nil {
		h.TrustError = fmt.Sprintf("%T: %v", err, err)
		return
	}
	for k, since := range m {
		x, err := strconv.ParseUint(k, 10, 64)
		if err != nil || x == 0 {
			h.TrustError = "ValueError: bad entry " + strconv.Quote(k)
			h.Trusted = map[uint64]float64{}
			return
		}
		h.Trusted[x] = since
	}
}

func (h *Hub) saveTrusted() {
	if h.TrustFile == "" {
		return
	}
	m := make(map[string]float64, len(h.Trusted))
	for x, since := range h.Trusted {
		m[strconv.FormatUint(x, 10)] = since
	}
	text, err := json.Marshal(m) // map keys come out sorted
	if err == nil {
		tmp := h.TrustFile + ".tmp"
		if err = os.WriteFile(tmp, text, 0o644); err == nil {
			err = os.Rename(tmp, h.TrustFile)
		}
	}
	if err != nil {
		h.Idle.LogEvery("trust-save", 60.0, fmt.Sprintf("trusted players file %s not written (%T: %v): %d trusted player(s) are lost on a restart", h.TrustFile, err, err, len(h.Trusted)), diag.Error)
	}
}

// IsTrusted says whether the operator trusted this account. Worlds read it from their workers; it only changes in control messages, which run
// with the workers paused.
func (h *Hub) IsTrusted(xuid uint64) bool {
	if h == nil || xuid == 0 {
		return false
	}
	_, ok := h.Trusted[xuid]
	return ok
}

// trusted is IsTrusted for a world that may have no hub (tests, tools).
func (w *World) trusted(xuid uint64) bool { return w.Hub != nil && w.Hub.IsTrusted(xuid) }

// Trust adds an account to the trusted list and takes it out of the cheater pool.
func (h *Hub) Trust(xuid uint64) {
	_, was := h.Trusted[xuid]
	if !was {
		h.Trusted[xuid] = h.Clk.Wall()
		h.saveTrusted()
	}
	pooled := h.Flagged.Delete(xuid)
	if pooled {
		h.saveFlags()
	}
	switch {
	case !was && pooled:
		h.Log(fmt.Sprintf("control: %#x trusted (speed and flight checks log only, never pooled) and taken out of the cheater pool; %d trusted", xuid, len(h.Trusted)))
	case !was:
		h.Log(fmt.Sprintf("control: %#x trusted (speed and flight checks log only, never pooled); %d trusted", xuid, len(h.Trusted)))
	case pooled:
		h.Log(fmt.Sprintf("control: %#x already trusted, taken out of the cheater pool", xuid))
	}
}

// Untrust removes an account from the trusted list; the checks apply to it again from its next trip.
func (h *Hub) Untrust(xuid uint64) {
	if _, ok := h.Trusted[xuid]; !ok {
		return
	}
	delete(h.Trusted, xuid)
	h.saveTrusted()
	h.Log(fmt.Sprintf("control: %#x no longer trusted (speed and flight checks kick it again); %d trusted", xuid, len(h.Trusted)))
}

// trustStatus is the trusted list for the operator panel, oldest first. The empty "lobbies" list is there for the control client's JSON query
// (see flagStatus).
func (h *Hub) trustStatus() pyjson.Obj {
	xs := make([]uint64, 0, len(h.Trusted))
	for x := range h.Trusted {
		xs = append(xs, x)
	}
	sort.Slice(xs, func(i, j int) bool {
		if h.Trusted[xs[i]] != h.Trusted[xs[j]] {
			return h.Trusted[xs[i]] < h.Trusted[xs[j]]
		}
		return xs[i] < xs[j]
	})
	list := []any{}
	for _, x := range xs {
		list = append(list, pyjson.Obj{{K: "xuid", V: fmt.Sprintf("%x", x)}, {K: "since", V: h.Trusted[x]}})
	}
	return pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "trusted", V: list}}
}
