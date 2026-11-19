package lsp

import (
	"slices"
	"testing"

	"fh2mp/internal/pyjson"
)

// The Guide's Players tab lists who is in the player's game: the other members of the world the player is in, nobody from another world,
// nobody when the player is in none.
func TestPlayersHereAreTheOthersOfMyWorld(t *testing.T) {
	lobbies := []any{
		pyjson.Obj{{K: "id", V: "aaaa"}, {K: "memberXuids", V: []any{"90000e961ddae", "90000abd7df93", "9000000000001"}}},
		pyjson.Obj{{K: "id", V: "bbbb"}, {K: "memberXuids", V: []any{"9000000000002"}}},
	}
	got := playersHere(0x90000abd7df93, lobbies)
	if !slices.Equal(got, []int64{0x90000e961ddae, 0x9000000000001}) {
		t.Fatalf("players here: %x", got)
	}
	if got := playersHere(0x9000000000002, lobbies); len(got) != 0 {
		t.Fatalf("a player alone in its world has %x with it", got)
	}
	if got := playersHere(0x9000000000003, lobbies); len(got) != 0 {
		t.Fatalf("a player in no world has %x with it", got)
	}
}
