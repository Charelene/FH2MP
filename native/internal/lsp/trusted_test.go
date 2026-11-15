package lsp

import (
	"fmt"
	"strconv"
	"strings"
	"testing"

	"fh2mp/internal/pyjson"
	"fh2mp/internal/xbl"
)

// trustHub answers the "trusted" query from the trust/untrust messages it was sent, as the game server does.
type trustHub struct {
	control []string
	set     map[string]bool
	down    bool
}

func (h *trustHub) Populations() (map[string][]string, bool) { return map[string][]string{}, true }
func (h *trustHub) Tools(string) string                      { return "" }
func (h *trustHub) Control(text string) {
	h.control = append(h.control, text)
	if verb, x, ok := strings.Cut(text, " "); ok && verb == "trust" {
		h.set[x] = true
	} else if ok && verb == "untrust" {
		delete(h.set, x)
	}
}
func (h *trustHub) Query(cmd string) pyjson.Obj {
	if h.down || cmd != "trusted" {
		return nil
	}
	list := []any{}
	for x := range h.set {
		list = append(list, pyjson.Obj{{K: "xuid", V: x}, {K: "since", V: 1.7e9}})
	}
	return pyjson.Obj{{K: "lobbies", V: []any{}}, {K: "trusted", V: list}}
}

func opsCall(t *testing.T, o *Ops, method, path, body string) (status int, text string) {
	t.Helper()
	defer func() {
		if p := recover(); p != nil {
			e, ok := p.(*OpsError)
			if !ok {
				panic(p)
			}
			status, text = e.Status, e.Code
		}
	}()
	st, obj, _ := o.route(nil, method, path, []byte(body))
	out, err := pyjson.Dumps(obj, pyjson.Opts{})
	if err != nil {
		t.Fatal(err)
	}
	return st, out
}

// POST /ops/v1/trusted/<xuid> sends "trust <hex>", DELETE sends "untrust <hex>", GET lists them with gamertags.
func TestOpsTrustedRoutes(t *testing.T) {
	hub := &trustHub{set: map[string]bool{}}
	c := &Core{Xbl: xbl.New(hub), tags: map[int64]string{}}
	const x = int64(2533278711105714)
	c.SetTag(x, "AVALON")
	o := &Ops{C: c}

	st, out := opsCall(t, o, "POST", fmt.Sprintf("/ops/v1/trusted/%d", x), "")
	if st != 200 || !strings.Contains(out, `"ok": true`) || !strings.Contains(out, `"gamertag": "AVALON"`) || !strings.Contains(out, `"trusted": true`) {
		t.Fatalf("trust: %d %s", st, out)
	}
	if want := "trust " + strconv.FormatUint(uint64(x), 16); hub.control[len(hub.control)-1] != want {
		t.Errorf("game server was told %q, want %q", hub.control, want)
	}
	st, out = opsCall(t, o, "GET", "/ops/v1/trusted", "")
	if st != 200 || !strings.Contains(out, fmt.Sprintf(`"xuid": %d`, x)) || !strings.Contains(out, `"AVALON"`) {
		t.Fatalf("list: %d %s", st, out)
	}
	st, out = opsCall(t, o, "POST", "/ops/v1/trusted", fmt.Sprintf(`{"xuid": %d}`, x))
	if st != 200 || !strings.Contains(out, `"ok": true`) {
		t.Fatalf("trust by body: %d %s", st, out)
	}
	st, out = opsCall(t, o, "DELETE", fmt.Sprintf("/ops/v1/trusted/%d", x), "")
	if st != 200 || !strings.Contains(out, `"ok": true`) || !strings.Contains(out, `"trusted": false`) {
		t.Fatalf("untrust: %d %s", st, out)
	}
	if len(hub.set) != 0 {
		t.Errorf("still trusted on the game server: %v", hub.set)
	}

	if st, code := opsCall(t, o, "POST", "/ops/v1/trusted", `{}`); st != 400 || code != "bad_request" {
		t.Errorf("empty body: %d %s", st, code)
	}
	if st, code := opsCall(t, o, "POST", "/ops/v1/trusted", `{"xuid": -4}`); st != 400 || code != "bad_xuid" {
		t.Errorf("bad xuid: %d %s", st, code)
	}
	hub.down = true
	if st, code := opsCall(t, o, "POST", fmt.Sprintf("/ops/v1/trusted/%d", x), ""); st != 503 || code != "unavailable" {
		t.Errorf("game server down: %d %s", st, code)
	}
	if n := len(hub.control); n != 3 {
		t.Errorf("%d control message(s) sent, want 3 (none while the game server is down): %q", n, hub.control)
	}
}
