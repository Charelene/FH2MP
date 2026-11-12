package xbl

import (
	"fmt"
	"slices"
	"strings"
	"testing"

	"fh2mp/internal/pyjson"
)

// Dev 2026-11-12 (win1, win2): Private Free Roam chosen while in a public Free Roam put both players back into the public world. The
// game makes its private game in a QoS session (defaultTemplate_qos) and asks GSMS for a server for it; the game server still had the
// players assigned to the public lobby they were matched into.

const qosTemplate = "defaultTemplate_qos"

func (r *heldSeatRig) qosPath(name string) string {
	return "/serviceconfigs/s/sessionTemplates/" + qosTemplate + "/sessions/" + name
}

func (r *heldSeatRig) putQos(name, body, xuid string) {
	r.t.Helper()
	if st, _ := r.s.PutSession(r.qosPath(name), []byte(body), xuid); st != 200 {
		r.t.Fatalf("PUT %s into %s as %s: status %d", body, name, xuid, st)
	}
}

func (r *heldSeatRig) assignedTo(xuid string) string {
	r.t.Helper()
	want := fmt.Sprintf("assign %s ", hexOf(xuid))
	got := ""
	for _, c := range r.hub.control {
		if len(c) > len(want) && c[:len(want)] == want {
			got = strings.TrimSuffix(c[len(want):], " follow")
		}
	}
	return got
}

func TestPrivateGameFromPublicFreeRoamIsAWorldOfItsOwn(t *testing.T) {
	r := newHeldSeatRig(t)
	public := r.match("2001", true)
	r.s.AnnouncePlayer("2001", public) // what the ticket answer does
	priv := "f411b090-5b1d-414f-8ae5-d3322b679c5a"
	r.putQos(priv, joinBody, "2001")
	if got := r.assignedTo("2001"); got != public {
		t.Fatalf("a plain QoS session moved the player to %q", got)
	}
	if !r.s.AllocateServer(r.qosPath(priv)) {
		t.Fatal("AllocateServer refused the private game's session")
	}
	if got := r.assignedTo("2001"); got != priv {
		t.Fatalf("after the server allocation the creator is assigned to %q, want the private game %s", got, priv)
	}
	// a friend's accepted invite: a seat in it, and writing itself into the session puts the friend in the same world
	r.disconnect("2001")
	r.hub.pops[priv] = []string{hexOf("2001")}
	h, err := r.s.ReserveInvited("2002", priv)
	if err != nil {
		t.Fatalf("an accepted invite could not lease a seat in the private game: %v", err)
	}
	if h.ID != priv || h.Kind != "freeroam" {
		t.Fatalf("lease %+v", h)
	}
	if _, err := r.s.Reserve("2003", priv, ""); err == nil {
		t.Fatal("the launcher's Join leased a seat in a private game")
	}
	r.putQos(priv, joinBody, "2002")
	if got := r.assignedTo("2002"); got != priv {
		t.Fatalf("the invited friend is assigned to %q, want %s", got, priv)
	}
	// not listed, not matched
	rows := r.s.LauncherRows(3002, []any{pyjson.Obj{{K: "id", V: priv}, {K: "kind", V: "freeroam"}, {K: "players", V: int64(1)}}}, r.hub.pops, nil)
	if len(rows) != 0 {
		t.Fatalf("the private game is listed: %v", rows[0].Row)
	}
	if got := r.match("2004", false); got == priv {
		t.Fatal("a Free Roam ticket was matched into the private game")
	}
}

// Dev 2026-11-12 14:50 (win1, win2): both were matched into one Car Meet and each chose Private Free Roam; both private games were given
// the meet's world, so the second player joined the first uninvited. Each private game is a world of its own.
func TestTwoPrivateGamesFromOneCarMeetAreSeparateWorlds(t *testing.T) {
	r := newHeldSeatRig(t)
	meet := r.carMeet("2001", "5", true)
	r.s.AnnouncePlayer("2001", meet)
	if got := r.carMeet("2002", "5", true); got != meet {
		t.Fatalf("the second player's Car Meet is %s, want the first one's %s", got, meet)
	}
	r.s.AnnouncePlayer("2002", meet)
	a, b := "ffbb4ebb-0726-4ebc-9780-ceaf136507f9", "c9aab327-5db8-475e-a67a-94c857682e98"
	r.putQos(a, joinBody, "2001")
	r.s.AllocateServer(r.qosPath(a))
	r.putQos(b, joinBody, "2002")
	r.s.AllocateServer(r.qosPath(b))
	if got := r.assignedTo("2001"); got != a {
		t.Fatalf("the first player's private game is world %q, want its own %s", got, a)
	}
	if got := r.assignedTo("2002"); got != b {
		t.Fatalf("the second player's private game is world %q, want its own %s", got, b)
	}
	if !slices.Contains(r.hub.control, fmt.Sprintf("assign %s %s follow", hexOf("2002"), b)) {
		t.Fatalf("control datagrams %v", r.hub.control)
	}
}

// The seat an accepted invite leases in a private game is no matchmaking hold: a Free Roam ticket made while it lasts gets a public lobby
// (using it would make a public lobby of the private game's name), the lobby list does not show it, and joining the game uses it up.
func TestAPrivateGamesSeatIsNotAMatchmakingHold(t *testing.T) {
	r := newHeldSeatRig(t)
	priv := "ffbb4ebb-0726-4ebc-9780-ceaf136507f9"
	r.putQos(priv, joinBody, "2001")
	r.s.AllocateServer(r.qosPath(priv))
	r.hub.pops[priv] = []string{hexOf("2001")}
	h, err := r.s.ReserveInvited("2002", priv)
	if err != nil || !h.Private {
		t.Fatalf("lease %+v, error %v", h, err)
	}
	if got := r.match("2002", false); got == priv {
		t.Fatal("a Free Roam ticket was matched into the private game through the invite's lease")
	}
	for _, row := range r.s.LauncherRows(2002, nil, r.hub.pops, nil) {
		if id, _ := row.Row.Get("id"); id == priv {
			t.Fatalf("the lobby list shows the private game's lease: %v", row.Row)
		}
	}
	if r.s.CurrentHold("2002") == nil {
		t.Fatal("the lease was dropped before the player joined")
	}
	r.s.ReserveMember("s", qosTemplate, priv, "2002")
	r.putQos(priv, joinBody, "2002")
	if r.s.CurrentHold("2002") != nil {
		t.Fatal("the lease outlived the join")
	}
}

// Dev 2026-11-12 (win2 into win1's private Free Roam): the invitee's game read the host's session, did not find itself among the members
// and sent a leave at once ("The session you were trying to join no longer exists"). The seat reserves it there first.
func TestAnInvitedFriendIsAReservedMemberOfTheHostsSession(t *testing.T) {
	r := newHeldSeatRig(t)
	public := r.match("2001", true)
	r.s.AnnouncePlayer("2001", public)
	priv := "2d45247e-58be-4269-a57a-338ee0f8c956"
	r.putQos(priv, joinBody, "2001")
	r.s.AllocateServer(r.qosPath(priv))
	if !r.s.ReserveMember("s", qosTemplate, priv, "2002") {
		t.Fatal("no session to reserve the friend in")
	}
	_, doc := r.s.GetSession(r.qosPath(priv), "2002")
	members, _ := doc.Get("members")
	text, _ := pyjson.Dumps(members, pyjson.Opts{})
	if !strings.Contains(text, `"xuid": "2002"`) || !strings.Contains(text, `"active": false`) {
		t.Fatalf("the friend is not a reserved member: %s", text)
	}
	r.putQos(priv, joinBody, "2002") // the game's own join: properties.system.active = true for itself
	if got := r.assignedTo("2002"); got != priv {
		t.Fatalf("the friend is assigned to %q, want the private game %s", got, priv)
	}
	if !r.s.ReserveMember("s", qosTemplate, priv, "2002") {
		t.Fatal("a member already there is reported as no session")
	}
}
