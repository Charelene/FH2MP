package xbl

import (
	"testing"

	"fh2mp/internal/pyjson"
)

// Private Free Roam is invite only (user decision 2026-11-11). Dev 2026-11-11 (win1): the Car Meet lobby 6812a683 went to Free Roam and
// the matchmaker kept matching new Car Meet tickets into it ("fullest with room"), from where they could follow into that Free Roam;
// the launcher's lobby list showed it to everyone.

func carMeetAttrs(location string) *Dict {
	d := NewDict()
	d.Set("Location", location)
	return d
}

func (r *heldSeatRig) carMeet(xuid, location string, connect bool) string {
	r.t.Helper()
	name, err := r.s.lobbyFor("s", "CarMeet_qos", carMeetAttrs(location), nil, xuid)
	if err != nil {
		r.t.Fatal(err)
	}
	if connect {
		r.disconnect(xuid)
		r.hub.pops[name] = append(r.hub.pops[name], hexOf(xuid))
	}
	return name
}

func TestCarMeetThatWentToFreeRoamIsNotMatched(t *testing.T) {
	r := newHeldSeatRig(t)
	meet := r.carMeet("2001", "5", true)
	if got := r.carMeet("2002", "5", false); got != meet {
		t.Fatalf("before its Free Roam a Car Meet with room takes new players: got %s, want %s", got, meet)
	}
	r.hub.pops["!freeroam"] = []string{meet} // the meet went to Free Roam
	if got := r.carMeet("2003", "5", false); got == meet {
		t.Fatalf("a stranger was matched into the Car Meet %s whose players are in their private Free Roam", meet)
	}
}

func TestPrivateFreeRoamIsNotListed(t *testing.T) {
	r := newHeldSeatRig(t)
	meet := r.carMeet("2001", "5", true)
	public := r.match("3001", true)
	r.hub.pops["!freeroam"] = []string{meet, public}
	rows := r.s.LauncherRows(3002, []any{
		pyjson.Obj{{K: "id", V: meet}, {K: "kind", V: "freeroam"}, {K: "players", V: int64(1)}},
		pyjson.Obj{{K: "id", V: public}, {K: "kind", V: "freeroam"}, {K: "players", V: int64(1)}},
	}, r.hub.pops, nil)
	if len(rows) != 1 {
		t.Fatalf("lobby list has %d rows, want only the public Free Roam", len(rows))
	}
	if id, _ := rows[0].Row.Get("id"); id != public {
		t.Fatalf("listed %v, want the public Free Roam %s", id, public)
	}
}

func TestOnlyAnInviteLeasesASeatInAPrivateFreeRoam(t *testing.T) {
	r := newHeldSeatRig(t)
	meet := r.carMeet("2001", "5", true)
	r.hub.pops["!freeroam"] = []string{meet}
	if _, err := r.s.Reserve("2009", meet, ""); err == nil {
		t.Fatal("the launcher's Join leased a seat in a private Free Roam")
	}
	h, err := r.s.ReserveInvited("2009", meet)
	if err != nil {
		t.Fatalf("an accepted invite could not lease a seat: %v", err)
	}
	if h.Kind != "carmeet" {
		t.Fatalf("lease kind %q, want carmeet", h.Kind)
	}
	// the invitee's game asks for a Car Meet somewhere else; the lease still takes it to the inviter's
	if got := r.carMeet("2009", "2", false); got != meet {
		t.Fatalf("the invitee's Car Meet ticket went to %s, want the inviter's %s", got, meet)
	}
	// a Free Roam ticket does not use a Car Meet lease
	if _, err := r.s.ReserveInvited("2010", meet); err != nil {
		t.Fatal(err)
	}
	if got := r.match("2010", false); got == meet {
		t.Fatal("a Free Roam ticket used a Car Meet lease")
	}
}
