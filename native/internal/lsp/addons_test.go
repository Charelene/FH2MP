package lsp

import (
	"net/http"
	"net/http/httptest"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/xbl"
)

// a launcher API with a car database and its PoliceLightbars copy
type dbLauncher struct{ sha, lightbars string }

func (l *dbLauncher) MinShim() string                       { return "" }
func (l *dbLauncher) VersionAtLeast(have, need string) bool { return true }
func (l *dbLauncher) BodyLimit(string) int64                { return 1 << 20 }
func (l *dbLauncher) GameDBInfo() (string, string, bool)    { return l.sha, "gamedbRC.slt", true }
func (l *dbLauncher) Handle(*Req, string, string, http.Header, []byte, string) (int, any, string) {
	return 404, nil, ""
}
func (l *dbLauncher) GameDBAddon(sha string) string {
	if sha == l.lightbars {
		return "lightbars"
	}
	return ""
}

// A game on the server's database with PoliceLightbars' rows plays as one on the server's database (not with its own car pack), and the game
// server is told it has the add-on; back on the plain database it is told "-". Each once, not with every request.
func TestLightbarsDatabaseTellsTheGameServer(t *testing.T) {
	hub := &logoutHub{}
	var lines []string
	c := &Core{Xbl: xbl.New(hub), Launcher: &dbLauncher{sha: "aaaa", lightbars: "bbbb"}, Out: func(l string) { lines = append(lines, l) }}
	acct := &auth.Account{Xuid: 0x3e9, Gamertag: "Racer"}
	ask := func(have string) *Req {
		t.Helper()
		req := httptest.NewRequest("GET", "/x", nil)
		req.Header.Set("X-FH2MP-GameDb", have)
		r := &Req{}
		if !c.gamedbOK(respWriter{httptest.NewRecorder()}, r, req, acct) {
			t.Fatalf("hash %s refused", have)
		}
		return r
	}
	addons := func() []string {
		var out []string
		for _, m := range hub.control {
			if len(m) > 7 && m[:7] == "addons " {
				out = append(out, m)
			}
		}
		return out
	}
	if r := ask("BBBB"); r.OwnCars {
		t.Fatal("the lightbars copy counted as the player's own car pack")
	}
	ask("bbbb")
	if got := addons(); len(got) != 1 || got[0] != "addons 3e9 lightbars" {
		t.Fatalf("control messages %q, want one \"addons 3e9 lightbars\"", got)
	}
	ask("aaaa")
	if got := addons(); len(got) != 2 || got[1] != "addons 3e9 -" {
		t.Fatalf("control messages %q, want \"addons 3e9 -\" after the plain database", got)
	}
	if r := ask("cccc"); !r.OwnCars {
		t.Fatal("an unknown database did not count as the player's own")
	}
	if got := addons(); len(got) != 2 {
		t.Fatalf("an own database announced add-ons: %q", got)
	}
}
