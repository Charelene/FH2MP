package lsp

import (
	"testing"

	"fh2mp/internal/dbtest"
)

// Setup gives the Drivatar store the game's car list and the friend graph: without the car list no Drivatar ever got its owner's car, livery or
// tune (the Go port until 2026-11-09), without the friends nobody was ever marked IsFriend.
func TestSetupWiresDrivatarCarsAndFriends(t *testing.T) {
	t.Setenv("FH2MP_ENV", "dev")
	c := New()
	c.Out = func(string) {}
	if err := c.Setup(dbtest.New(t, "fh2mp_cat")); err != nil {
		t.Fatal(err)
	}
	defer c.DB.Close()
	if len(c.Drv.KnownCars) < 100 {
		t.Errorf("the Drivatar store knows %d cars, want the catalog's KNOWN_CARS", len(c.Drv.KnownCars))
	}
	if c.Drv.FriendsOf == nil {
		t.Fatal("the Drivatar store has no friend lookup")
	}
	if err := c.Social.AddFriend(0x10, 0x20); err != nil {
		t.Fatal(err)
	}
	if f, err := c.Drv.FriendsOf(0x10); err != nil || len(f) != 1 || f[0] != 0x20 {
		t.Errorf("FriendsOf(0x10) = %v, %v; want [0x20]", f, err)
	}
}
