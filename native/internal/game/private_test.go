package game

import (
	"testing"

	"fh2mp/internal/bits"
	"fh2mp/internal/netv2"
)

// A private game (dev 2026-11-12: "party leader is not being assigned, the invited car could start an event"). The game has its own
// private-session rules, switched on by SessionHostSynced.PrivacyMode = InviteOnly: the leader's crown in the HUD list, and only the leader
// starts events. The world keeps a leader for it: the first player, then whoever the leader names or the longest-present player.

func privateRig(t *testing.T) (*stateRig, *Peer, *Peer) {
	r := newStateRig(t)
	r.w.Private = true
	key := &WorldKey{Type: LobbyTypeFreeRoam, Name: "ffbb4ebb"}
	mk := func(port, owner int, at float64) *Peer {
		p := r.halfOpen(port)
		p.Owner, p.ConnectedAt, p.LobbyType, p.World = owner, at, LobbyTypeFreeRoam, key
		p.Connected, p.Entered, p.State = true, true, PeerEntered
		return p
	}
	host := mk(40001, 1, 100)
	r.w.KeepPrivateLeader("the private game's first player")
	guest := mk(40002, 2, 200)
	r.w.KeepPrivateLeader("the private game's first player")
	return r, host, guest
}

func varsBits(v netv2.VarsWriter) (uint64, int) {
	if v == nil {
		return 0, 0
	}
	var w bits.Writer
	v(&w)
	return w.Value().Uint64(), w.Pos
}

func TestPrivateGameTellsTheGameItIsInviteOnly(t *testing.T) {
	r, host, guest := privateRig(t)
	// the host's game asked for Free Roam: PrivacyMode alone (var 1, 2 bits = 1)
	if v, n := varsBits(r.w.sessionHostCreateVars(host)); n != 6 || v != 1|1<<4 {
		t.Errorf("host: %d bits %#x, want var 1 = 1 in 6 bits", n, v)
	}
	// a guest whose game came in as a Car Meet: LobbyType = Free Roam (var 0, 3 bits), then PrivacyMode
	guest.ClaimedLobbyType = LobbyTypeCarMeet
	if v, n := varsBits(r.w.sessionHostCreateVars(guest)); n != 13 || v != 2<<4|1<<7|1<<11 {
		t.Errorf("guest: %d bits %#x, want LobbyType 2 then PrivacyMode 1 in 13 bits", n, v)
	}
	// a public Free Roam says nothing, as before
	r.w.Private = false
	guest.ClaimedLobbyType = 0
	if r.w.sessionHostCreateVars(guest) != nil {
		t.Error("a public Free Roam world sends session vars at the handshake")
	}
}

func TestPrivateGameKeepsALeader(t *testing.T) {
	r, host, guest := privateRig(t)
	if r.w.Leader != host {
		t.Fatalf("the leader is %v, want the host (the first player)", r.w.Leader)
	}
	// between events and past the setup timeout the host stays the leader
	r.w.EndEvent("test", false)
	r.clk.t += LeaderSetupSeconds + 60
	host.LastRx, guest.LastRx = r.clk.t, r.clk.t // both still send; only the setup timeout is due
	r.w.Tick()
	if r.w.Leader != host {
		t.Fatal("the private game's leader was dropped between events")
	}
	// a guest cannot take over, the leader can pass it on, and back
	r.w.RequestLeaderChange(guest, "relay", host.Owner)
	if r.w.Leader != host {
		t.Fatal("a guest made itself the leader")
	}
	r.w.RequestLeaderChange(host, "relay", guest.Owner)
	if r.w.Leader != guest {
		t.Fatal("the leader could not pass leadership on (Make Session Leader)")
	}
	// the leader leaves: the longest-present player takes over
	r.w.DropPeer(guest, "test")
	if r.w.Leader != host {
		t.Fatalf("after the leader left the leader is %v, want the remaining player", r.w.Leader)
	}
	if !r.logHas("event leader: owner 1 (the leader, owner 2, left)") {
		t.Errorf("no leader line for the takeover in %q", r.log)
	}
}

// A public Free Roam keeps its rules: no standing leader, the event leader is dropped when the event is over.
func TestPublicWorldHasNoStandingLeader(t *testing.T) {
	r, host, _ := privateRig(t)
	r.w.Private = false
	r.w.EndEvent("test", false)
	if r.w.Leader != nil {
		t.Fatal("a public world kept its event leader after the event")
	}
	r.w.KeepPrivateLeader("x")
	if r.w.Leader != nil {
		t.Fatal("a public world was given a standing leader")
	}
	_ = host
}

// The leader of a private game removes a player (message 46, the popup's Kick From Session); nobody else can.
func TestPrivateGameLeaderKicks(t *testing.T) {
	r, host, guest := privateRig(t)
	r.w.KickFromPrivate(guest, host.Xuid)
	if _, still := r.w.Peers.Get(host.Addr); !still {
		t.Fatal("a guest kicked the leader")
	}
	r.w.KickFromPrivate(host, 0x1234)
	if r.w.Peers.Len() != 2 {
		t.Fatal("a kick of an id nobody has removed someone")
	}
	r.w.KickFromPrivate(host, guest.Xuid)
	if _, still := r.w.Peers.Get(guest.Addr); still {
		t.Fatal("the leader's kick left the player in the game")
	}
	if !r.logHas("removed from the private game by its leader, owner 1") {
		t.Errorf("no kick line in %q", r.log)
	}
	// the kicked player's game was told why: CloseConnection with the reason PlayerKick
	sent := sentTo(r, guest.Addr)
	if len(sent) == 0 {
		t.Fatal("the kicked player's game got nothing")
	}
	last := sent[len(sent)-1]
	if last[8] != 1 || last[11]&0x7f != netv2.MsgCloseConnection || (uint16(last[11])|uint16(last[12])<<8)>>7&7 != CloseReasonPlayerKick {
		t.Errorf("the last datagram to the kicked player is not CloseConnection(PlayerKick): %x", last)
	}
}
