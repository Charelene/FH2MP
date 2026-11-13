package xbl

import (
	"testing"

	"fh2mp/internal/capacity"
)

// Storm Island asks for IslandFreeRoam_qos / IslandRoadTrip_qos and, for its Car Meet, CarMeet_qos with Location 7 (game+0x8c5750,
// car_meets_island.xml). Island and mainland lobbies never share a lease: the game loads the map of the session type it asked for.

func (r *heldSeatRig) islandMatch(xuid string, connect bool) string {
	r.t.Helper()
	name, err := r.s.lobbyFor("s", "IslandFreeRoam_qos", NewDict(), nil, xuid)
	if err != nil {
		r.t.Fatal(err)
	}
	if connect {
		r.disconnect(xuid)
		r.hub.pops[name] = append(r.hub.pops[name], hexOf(xuid))
	}
	return name
}

func TestIslandHoppersHaveTheirKinds(t *testing.T) {
	for hopper, want := range map[string]string{"IslandFreeRoam_qos": "islandfreeroam", "IslandRoadTrip_qos": "islandroadtrip", "FreeRoam_qos": "freeroam"} {
		if got := LobbyKind(LobbyKey{Hopper: hopper}); got != want {
			t.Errorf("LobbyKind(%s) = %q, want %q", hopper, got, want)
		}
	}
	if got := holdKind(LobbyKey{Hopper: "CarMeet_qos", Location: IslandCarMeetLocation}); got != "islandcarmeet" {
		t.Errorf("island Car Meet hold kind %q", got)
	}
	if got := holdKind(LobbyKey{Hopper: "CarMeet_qos", Location: "5"}); got != "carmeet" {
		t.Errorf("mainland Car Meet hold kind %q", got)
	}
	if capacity.ForHopper("IslandFreeRoam_qos") != capacity.FreeRoam() || capacity.ForKind("islandfreeroam") != capacity.FreeRoam() {
		t.Error("island Free Roam does not get the Free Roam capacity")
	}
}

func TestIslandAndMainlandFreeRoamAreSeparateLobbies(t *testing.T) {
	r := newHeldSeatRig(t)
	main := r.match("3001", true)
	isl := r.islandMatch("3002", true)
	if isl == main {
		t.Fatal("an island Free Roam ticket was matched into a mainland lobby")
	}
	if got := r.islandMatch("3003", false); got != isl {
		t.Fatalf("a second island ticket went to %s, want the island lobby %s", got, isl)
	}
	// a launcher Join into the island lobby is used only by an island ticket
	if _, err := r.s.Reserve("3004", isl, ""); err != nil {
		t.Fatal(err)
	}
	if got := r.match("3004", false); got == isl {
		t.Fatal("a mainland ticket used an island seat lease")
	}
	if _, err := r.s.Reserve("3005", isl, ""); err != nil {
		t.Fatal(err)
	}
	if got := r.islandMatch("3005", false); got != isl {
		t.Fatalf("the island ticket went to %s, want its leased lobby %s", got, isl)
	}
	// a new island lobby asked for from the launcher
	h, err := r.s.Reserve("3006", "", "islandfreeroam")
	if err != nil {
		t.Fatalf("an island lobby request was refused: %v", err)
	}
	if got := r.islandMatch("3006", false); got != h.ID {
		t.Fatalf("the island ticket went to %s, want the requested lobby %s", got, h.ID)
	}
}

func TestCarMeetInviteLeaseStaysOnItsSide(t *testing.T) {
	r := newHeldSeatRig(t)
	meet := r.carMeet("4001", IslandCarMeetLocation, true)
	r.hub.pops["!freeroam"] = []string{meet}
	h, err := r.s.ReserveInvited("4002", meet, "")
	if err != nil {
		t.Fatal(err)
	}
	if h.Kind != "islandcarmeet" {
		t.Fatalf("lease kind %q, want islandcarmeet", h.Kind)
	}
	// a mainland Car Meet ticket does not use the island lease: its game has loaded a mainland meet
	if got := r.carMeet("4002", "3", false); got == meet {
		t.Fatal("a mainland Car Meet ticket was put into the island Car Meet")
	}
	if _, err := r.s.ReserveInvited("4003", meet, ""); err != nil {
		t.Fatal(err)
	}
	if got := r.carMeet("4003", IslandCarMeetLocation, false); got != meet {
		t.Fatalf("the island Car Meet ticket went to %s, want the inviter's %s", got, meet)
	}
}
