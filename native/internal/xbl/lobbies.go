package xbl

import (
	"crypto/rand"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"time"

	"fh2mp/internal/auth"
	"fh2mp/internal/capacity"
	"fh2mp/internal/pyjson"
)

// Launcher seat leases (= server/launcher_lobbies.py). A hold only influences the next ordinary matchmaking request; it never moves a connected
// client. New-lobby requests are materialised using that client's real ticket attributes, so no game session configuration is invented.

// HoldSeconds is how long a seat lease lasts.
const HoldSeconds = 300

// hopperKinds: real FreeRoam_qos, RoadTrip_qos and CarMeet_qos tickets all use GSI ...0102. GSI identifies the image set, not the requested mode.
var hopperKinds = map[string]string{"freeroam_qos": "freeroam", "roadtrip_qos": "roadtrip"}

func randomUUID() string {
	var b [16]byte
	rand.Read(b[:])
	b[6] = b[6]&0x0f | 0x40
	b[8] = b[8]&0x3f | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

// LobbyKind is the kind ("freeroam", "roadtrip") of a lobby key's hopper, "" for any other.
func LobbyKind(key LobbyKey) string { return hopperKinds[strings.ToLower(key.Hopper)] }

// holdKind is the kind of a seat lease in a lobby of this key: LobbyKind, or "carmeet" for a Car Meet lobby. Only an accepted game invite
// leases a Car Meet seat: the game's private Free Roam grows out of a Car Meet, so that is the way into someone's private Free Roam.
func holdKind(key LobbyKey) string {
	if k := LobbyKind(key); k != "" {
		return k
	}
	if strings.HasPrefix(strings.ToLower(key.Hopper), "carmeet") {
		return "carmeet"
	}
	return ""
}

// sameHoldKey: a ticket may use a lease of this key. A Car Meet lease ignores the location: the invitee's game asks for its own Car Meet
// place, and the seat is in the inviter's.
func sameHoldKey(held, ticket LobbyKey) bool {
	if holdKind(held) == "carmeet" {
		return held.Scid == ticket.Scid && strings.EqualFold(held.Hopper, ticket.Hopper) && held.Gsi == ticket.Gsi
	}
	return held == ticket
}

// carMeetLobbyLocked: the name is a lobby the matchmaker made for a Car Meet hopper. The caller holds the lock.
func (s *Server) carMeetLobbyLocked(name string) bool {
	for _, k := range s.lobbyKeys {
		if holdKind(k) != "carmeet" {
			continue
		}
		for _, n := range s.lobbies[k] {
			if strings.EqualFold(n, name) {
				return true
			}
		}
	}
	return false
}

// privateFreeRoamLocked: a Car Meet lobby whose players went to Free Roam (the game server's pop report lists it under !freeroam): the
// game's private Free Roam, invite only (user decision 2026-11-11). The matchmaker no longer fills it and the lobby list does not show
// it. The caller holds the lock.
func (s *Server) privateFreeRoamLocked(name string, pops map[string][]string) bool {
	for _, n := range pops["!freeroam"] {
		if strings.EqualFold(n, name) {
			return s.carMeetLobbyLocked(name)
		}
	}
	return false
}

func (s *Server) pruneHolds(now float64) {
	for x, h := range s.holds {
		if float64(h.ExpiresAt) <= now {
			delete(s.holds, x)
			lobbyLog.Info("reservation expired unused " + kv(f("xuid", x), f("lobby", trunc(h.ID, 8)), f("kind", h.Kind), f("held_s", HoldSeconds)))
		}
	}
}

// seatsLocked are the decimal xuids holding a seat in the named lobby, minus the present ones; the caller holds the lock.
func (s *Server) seatsLocked(name string, present []string, now float64) map[string]bool {
	s.pruneHolds(now)
	skip := map[string]bool{}
	for _, p := range present {
		skip[p] = true
	}
	out := map[string]bool{}
	for x, h := range s.holds {
		if h.ID == name && !skip[x] {
			out[x] = true
		}
	}
	return out
}

// Seats are the decimal xuids holding a seat in the named lobby, minus the present ones.
func (s *Server) Seats(name string, present []string) map[string]bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.seatsLocked(name, present, s.Now())
}

// CurrentHold is the player's lease (nil = none).
func (s *Server) CurrentHold(xuid string) *Hold {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.pruneHolds(s.Now())
	if h, ok := s.holds[xuid]; ok {
		c := *h
		return &c
	}
	return nil
}

// CancelHold ends the player's lease.
func (s *Server) CancelHold(xuid string) {
	s.mu.Lock()
	h, ok := s.holds[xuid]
	delete(s.holds, xuid)
	s.mu.Unlock()
	if ok {
		lobbyLog.Info("reservation cancelled by the player " + kv(f("xuid", xuid), f("lobby", trunc(h.ID, 8)), f("kind", h.Kind)))
	}
}

func authErr(status int, code, message, xuid, detail string) *auth.Error {
	if message == "" {
		message = code
	}
	e := &auth.Error{Status: status, Code: code, Message: message, Detail: detail}
	if n, err := strconv.ParseInt(xuid, 10, 64); err == nil {
		e.Xuid = n
	}
	return e
}

// Reserve leases a seat in a lobby (by name) or in a new one of a kind ("freeroam", "roadtrip").
func (s *Server) Reserve(xuid, lobby, kind string) (*Hold, error) {
	return s.reserve(xuid, lobby, kind, false)
}

// ReserveInvited leases a seat for a player who accepted a game invite into the named lobby: like Reserve, and a Car Meet lobby (the
// inviter's private Free Roam) is allowed.
func (s *Server) ReserveInvited(xuid, lobby string) (*Hold, error) {
	return s.reserve(xuid, lobby, "", true)
}

func (s *Server) reserve(xuid, lobby, kind string, invited bool) (*Hold, error) {
	if kind != "" && kind != "freeroam" && kind != "roadtrip" {
		return nil, authErr(400, "bad_mode", "Choose Free Roam or Road Trip.", xuid, "kind "+pyRepr(trunc(kind, 20)))
	}
	pops, havePops := s.Populations()
	s.mu.Lock()
	s.pruneHolds(s.Now())
	if !havePops {
		s.mu.Unlock()
		return nil, authErr(503, "unavailable", "The game server did not answer. Try again shortly.", xuid, "population query got no answer")
	}
	flagged := s.FlaggedSet(pops)
	var key *LobbyKey
	var previous *Hold
	var hold *Hold
	var err error
	func() {
		defer s.mu.Unlock()
		n, _ := strconv.ParseInt(xuid, 10, 64)
		if lobby != "" {
			for _, k := range s.lobbyKeys {
				for _, name := range s.lobbies[k] {
					if name == lobby {
						kk := k
						key = &kk
						break
					}
				}
				if key != nil {
					break
				}
			}
			if key == nil && invited && s.privateGameLocked(lobby) != nil {
				// the inviter's private game (a world of its own, not in the lobby index): a seat while it has room
				kind = "freeroam"
				present := []string{}
				already := s.seatsLocked(lobby, nil, s.Now())[xuid]
				for _, x := range pops[lobby] {
					if x == fmt.Sprintf("%x", n) {
						already = true
					}
					if v, e := strconv.ParseInt(x, 16, 64); e == nil {
						present = append(present, strconv.FormatInt(v, 10))
					}
				}
				occupied := len(pops[lobby]) + len(s.seatsLocked(lobby, present, s.Now()))
				if room := capacity.ForKind(kind); occupied >= room && !already {
					err = authErr(409, "lobby_full", "This lobby is full.", xuid, fmt.Sprintf("private game %s holds %d of %d", trunc(lobby, 8), occupied, room))
					return
				}
				previous = s.holds[xuid]
				hold = &Hold{ID: lobby, Kind: kind, ExpiresAt: int64(s.Now()) + HoldSeconds, Private: true}
				s.holds[xuid] = hold
				return
			}
			if key == nil || (LobbyKind(*key) == "" && !(invited && holdKind(*key) == "carmeet")) {
				why := "is not in the lobby index"
				if key != nil {
					why = fmt.Sprintf("is of hopper %s, which is neither FreeRoam nor RoadTrip", pyRepr(key.Hopper))
				}
				code := "lobby_gone"
				err = authErr(409, code, "This lobby is no longer available.", xuid, fmt.Sprintf("lobby %s %s", trunc(lobby, 8), why))
				return
			}
			kind = holdKind(*key)
			rec := s.sessions[[3]string{strings.ToLower(key.Scid), strings.ToLower(TargetTemplate), strings.ToLower(lobby)}]
			if rec == nil {
				err = authErr(409, "lobby_gone", "", xuid, fmt.Sprintf("lobby %s is indexed but its MPSD session is gone", trunc(lobby, 8)))
				return
			}
			occupied := s.lobbyOccupancy(lobby, rec, pops, true)
			if !s.poolFits(lobby, pops, flagged, flagged[n]) {
				err = authErr(409, "lobby_unavailable", "This lobby is unavailable for your player pool.", xuid, fmt.Sprintf("lobby %s: pool does not fit (player flagged=%s)", trunc(lobby, 8), pyBool(flagged[n])))
				return
			}
			already := s.seatsLocked(lobby, nil, s.Now())[xuid]
			for _, x := range pops[lobby] {
				if x == fmt.Sprintf("%x", n) {
					already = true
				}
			}
			room := capacity.ForKind(kind)
			if occupied >= min(maxPlayers(rec, room), room) && !already {
				err = authErr(409, "lobby_full", "This lobby is full.", xuid, fmt.Sprintf("lobby %s holds %d of %d", trunc(lobby, 8), occupied, min(maxPlayers(rec, room), room)))
				return
			}
		} else if kind != "" {
			lobby = s.NewUUID()
		} else {
			err = authErr(400, "bad_request", "", xuid, "neither a lobby nor a kind was given")
			return
		}
		previous = s.holds[xuid]
		hold = &Hold{ID: lobby, Kind: kind, ExpiresAt: int64(s.Now()) + HoldSeconds, Key: key}
		s.holds[xuid] = hold
	}()
	if err != nil {
		return nil, err
	}
	var prev any
	if previous != nil {
		prev = trunc(previous.ID, 8)
	}
	lobbyLog.Info("reservation made " + kv(f("xuid", xuid), f("lobby", trunc(lobby, 8)), f("kind", kind), f("existing", key != nil), f("replaces", prev), f("hold_s", HoldSeconds)))
	c := *hold
	return &c, nil
}

func pyBool(b bool) string {
	if b {
		return "True"
	}
	return "False"
}

// pick returns a compatible held lobby ("" = none); called under the matchmaking lock.
func (s *Server) pick(xuid string, key LobbyKey, attrs *Dict, ref *Dict, pops map[string][]string, havePops bool) (string, error) {
	s.pruneHolds(s.Now())
	h := s.holds[xuid]
	if h == nil || h.Private {
		return "", nil
	}
	if holdKind(key) != h.Kind {
		s.pickLog("pick-kind/"+xuid+"/"+key.Hopper, fmt.Sprintf("reservation not used for this ticket: hopper %s is not the held %s ", pyRepr(key.Hopper), h.Kind)+kv(f("xuid", xuid), f("lobby", trunc(h.ID, 8))))
		return "", nil
	}
	if h.Key != nil && !sameHoldKey(*h.Key, key) {
		s.pickLog(fmt.Sprintf("pick-key/%s/%v", xuid, key), "reservation not used for this ticket: "+kv(f("xuid", xuid), f("lobby", trunc(h.ID, 8)), f("held_template", h.Key.Scid), f("ticket_template", key.Scid),
			f("held_hopper", h.Key.Hopper), f("ticket_hopper", key.Hopper)))
		return "", nil // another island/location must not consume this hold
	}
	if !havePops {
		return "", authErr(503, "unavailable", "Cannot verify the reserved lobby right now.", xuid, "population query got no answer during matchmaking")
	}
	name := h.ID
	if h.Key == nil {
		s.newLobby(key.Scid, attrs, ref, name, key.Hopper)
		if _, ok := s.lobbies[key]; !ok {
			s.lobbyKeys = append(s.lobbyKeys, key)
		}
		s.lobbies[key] = append(s.lobbies[key], name)
		lobbyLog.Info("reservation used: new world made for it " + kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("kind", h.Kind)))
	} else {
		rec := s.lookup(key.Scid, name)
		flags := s.FlaggedSet(pops)
		occupied := 0
		if rec != nil {
			occupied = s.lobbyOccupancy(name, rec, pops, true)
		}
		n, _ := strconv.ParseInt(xuid, 10, 64)
		fits := rec != nil && s.poolFits(name, pops, flags, flags[n])
		if rec == nil || !fits {
			delete(s.holds, xuid)
			why := "the world is not in this player's pool"
			if rec == nil {
				why = "the world's session is gone"
			}
			lobbyLog.Warn("reservation dropped, the player is matched normally: " + kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("why", why), f("flagged", flags[n])))
			return "", nil
		}
		// The held seat is already counted in occupancy.
		room := capacity.ForKind(h.Kind)
		if occupied > min(maxPlayers(rec, room), room) {
			delete(s.holds, xuid)
			lobbyLog.Warn("reservation dropped, the player is matched normally: " + kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("why", "the world filled up since"), f("occupied", occupied), f("max", min(maxPlayers(rec, room), room))))
			return "", nil
		}
		lobbyLog.Info("reservation used: joins the held world " + kv(f("xuid", xuid), f("lobby", trunc(name, 8)), f("kind", h.Kind), f("occupied", occupied)))
	}
	delete(s.holds, xuid)
	return name, nil
}

// pickLog: a ticket is polled every few seconds; a reason that keeps holding is written once a minute per player.
func (s *Server) pickLog(key, msg string) {
	if ok, skipped := s.rate.Allow(key, 60.0); ok {
		if skipped > 0 {
			msg += fmt.Sprintf(" (and %d more tickets in the last 60 s)", skipped)
		}
		lobbyLog.Info(msg)
	}
}

var _ = time.Now
var _ = sort.Strings

// LauncherRow is one lobby of the launcher's list: the hub's summary without its member ids, which are returned apart.
type LauncherRow struct {
	Row pyjson.Obj
	IDs []any // memberXuids as the hub sent them (hex text); nil for the synthetic row of a pending reservation
}

// LauncherRows is the matchmaking-locked part of the launcher's lobby list (community.lobbies): the hub's lobbies that fit the caller's pool, with the
// seats held for launchers and the occupancy, then the caller's own pending reservation as a row of its own.
func (s *Server) LauncherRows(me int64, lobbies []any, pops map[string][]string, flags map[int64]bool) []LauncherRow {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.Now()
	s.pruneHolds(now)
	sessions := map[string]*Session{}
	for key, rec := range s.sessions {
		if key[1] == strings.ToLower(TargetTemplate) {
			sessions[key[2]] = rec
		}
	}
	var out []LauncherRow
	for _, lv := range lobbies {
		row, _ := lv.(pyjson.Obj)
		idv, _ := row.Get("id")
		id, _ := idv.(string)
		rec := sessions[strings.ToLower(id)]
		occupied := -1
		if rec != nil {
			occupied = s.lobbyOccupancy(id, rec, pops, true)
		}
		if !s.poolFits(id, pops, flags, flags[me]) {
			continue
		}
		if kind, _ := row.Get("kind"); kind == "freeroam" && (s.carMeetLobbyLocked(id) || s.privateGameLocked(id) != nil) {
			continue // a private Free Roam: invite only, not listed
		}
		var ids []any
		clean := pyjson.Obj{}
		for _, p := range row {
			if p.K == "memberXuids" {
				ids, _ = p.V.([]any)
			} else {
				clean = append(clean, p)
			}
		}
		present := []string{}
		for _, x := range pops[id] {
			if x != "" {
				n, _ := strconv.ParseInt(x, 16, 64)
				present = append(present, strconv.FormatInt(n, 10))
			}
		}
		held := len(s.seatsLocked(id, present, now))
		if occupied < 0 {
			pl, _ := row.Get("players")
			occupied = int(toInt(pl)) + held
		}
		clean.Set("held", int64(held))
		clean.Set("occupied", int64(occupied))
		out = append(out, LauncherRow{Row: clean, IDs: ids})
	}
	if h, ok := s.holds[strconv.FormatInt(me, 10)]; ok && h.Key == nil && !h.Private {
		out = append(out, LauncherRow{Row: pyjson.Obj{{K: "id", V: h.ID}, {K: "kind", V: h.Kind}, {K: "players", V: int64(0)}, {K: "held", V: int64(1)}, {K: "occupied", V: int64(1)},
			{K: "capacity", V: int64(capacity.FreeRoam())}, {K: "status", V: "Waiting for your matchmaking request"}, {K: "members", V: []any{}}}})
	}
	return out
}

func toInt(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case uint64:
		return int64(x)
	case float64:
		return int64(x)
	}
	return 0
}
