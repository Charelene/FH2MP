package lsp

import (
	"strings"
	"testing"

	"fh2mp/internal/pyjson"
	"fh2mp/internal/xbl"
)

type logoutHub struct{ control []string }

func (h *logoutHub) Populations() (map[string][]string, bool) { return map[string][]string{}, true }
func (h *logoutHub) Control(text string)                      { h.control = append(h.control, text) }
func (h *logoutHub) Query(string) pyjson.Obj                  { return nil }
func (h *logoutHub) Tools(string) string                      { return "" }

// A launcher signs out after its game ended. A game that did not leave its lobby session (closed window, crash) stayed an active member there for
// up to 30 minutes and filled the lobby for every game that read the session (prod 2026-11-09, xbl.CountSessionMembers): the logout takes the seat.
func TestLogoutTakesThePlayersLobbySeat(t *testing.T) {
	hub := &logoutHub{}
	c := &Core{Xbl: xbl.New(hub)}
	path := "/serviceconfigs/s/sessionTemplates/" + xbl.TargetTemplate + "/sessions/00000000-0000-4000-8000-000000000001"
	join := []byte(`{"members": {"me": {"properties": {"system": {"active": true}}}}}`)
	for _, x := range []string{"1001", "1002"} {
		if st, _ := c.Xbl.PutSession(path, join, x); st != 200 && st != 201 {
			t.Fatalf("PUT as %s: status %d", x, st)
		}
	}
	members := func() string {
		st, doc := c.Xbl.GetSession(path, "")
		if st != 200 {
			t.Fatalf("GET: status %d", st)
		}
		text, err := pyjson.Dumps(doc, pyjson.Opts{})
		if err != nil {
			t.Fatal(err)
		}
		return text
	}
	if text := members(); !strings.Contains(text, `"1001"`) || !strings.Contains(text, `"1002"`) {
		t.Fatalf("both players should be members: %s", text)
	}
	c.OnLogout(1001, "fh2.sometoken")
	text := members()
	if strings.Contains(text, `"1001"`) {
		t.Fatalf("the signed-out player still holds a seat: %s", text)
	}
	if !strings.Contains(text, `"1002"`) {
		t.Fatalf("the other player lost its seat: %s", text)
	}
	if n := len(hub.control); n == 0 || !strings.HasPrefix(hub.control[n-1], "logout 3e9 ") { // after the two joins' "assign" messages
		t.Fatalf("the game server was told %q, want a logout of 0x3e9 last", hub.control)
	}
}
