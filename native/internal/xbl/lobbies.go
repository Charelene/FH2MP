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
			if key == nil || LobbyKind(*key) == "" {
				why := "is not in the lobby index"
				if key != nil {
					why = fmt.Sprintf("is of hopper %s, which is neither FreeRoam nor RoadTrip", pyRepr(key.Hopper))
				}
				code := "lobby_gone"
				err = authErr(409, code, "This lobby is no longer available.", xuid, fmt.Sprintf("lobby %s %s", trunc(lobby, 8), why))
				return
			}
			kind = LobbyKind(*key)
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
	if h == nil {
		return "", nil
	}
	if LobbyKind(key) != h.Kind {
		s.pickLog("pick-kind/"+xuid+"/"+key.Hopper, fmt.Sprintf("reservation not used for this ticket: hopper %s is not the held %s ", pyRepr(key.Hopper), h.Kind)+kv(f("xuid", xuid), f("lobby", trunc(h.ID, 8))))
		return "", nil
	}
	if h.Key != nil && *h.Key != key {
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
