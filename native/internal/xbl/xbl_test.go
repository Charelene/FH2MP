package xbl

import (
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyjson"
)

type fakeHub struct {
	mu      sync.Mutex
	pops    map[string][]string
	have    bool
	control []string
}

func (h *fakeHub) Populations() (map[string][]string, bool) { return h.pops, h.have }
func (h *fakeHub) Control(text string)                      { h.control = append(h.control, text) }
func (h *fakeHub) Query(string) pyjson.Obj                  { return nil }
func (h *fakeHub) Tools(string) string                      { return "" }

func load(t *testing.T) (ops []any, results []any) {
	f, err := os.Open("../../testdata/xbl.json.gz")
	if err != nil {
		t.Skip(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	b, err := io.ReadAll(zr)
	if err != nil {
		t.Fatal(err)
	}
	v, err := pyjson.Parse(string(b))
	if err != nil {
		t.Fatal(err)
	}
	o := v.(pyjson.Obj)
	a, _ := o.Get("ops")
	r, _ := o.Get("results")
	return a.([]any), r.([]any)
}

func canon(v any) string {
	s, err := pyjson.Dumps(v, pyjson.Opts{SortKeys: true})
	if err != nil {
		panic(err)
	}
	return s
}

func field(o pyjson.Obj, k string) any { v, _ := o.Get(k); return v }

func str(v any) string { s, _ := v.(string); return s }

func holdObj(h *Hold) any {
	if h == nil {
		return nil
	}
	var key any
	if h.Key != nil {
		key = []any{h.Key.Scid, h.Key.Hopper, h.Key.Gsi, h.Key.Location}
	}
	return pyjson.Obj{{K: "id", V: h.ID}, {K: "kind", V: h.Kind}, {K: "expiresAt", V: h.ExpiresAt}, {K: "key", V: key}}
}

func (s *Server) snapshot() pyjson.Obj {
	s.mu.Lock()
	defer s.mu.Unlock()
	keys := make([][3]string, 0, len(s.sessions))
	for k := range s.sessions {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		for n := 0; n < 3; n++ {
			if keys[i][n] != keys[j][n] {
				return keys[i][n] < keys[j][n]
			}
		}
		return false
	})
	sessions := []any{}
	for _, k := range keys {
		sessions = append(sessions, []any{[]any{k[0], k[1], k[2]}, s.sessions[k].obj()})
	}
	tickets := pyjson.Obj{}
	ids := make([]string, 0, len(s.tickets))
	for id := range s.tickets {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		t := s.tickets[id]
		var ref any
		if t.Ref != nil {
			ref = ToJSON(t.Ref)
		}
		o := pyjson.Obj{{K: "scid", V: t.Scid}, {K: "hopper", V: t.Hopper}, {K: "ref", V: ref}, {K: "attrs", V: ToJSON(t.Attrs)}, {K: "preserve", V: ToJSON(t.Preserve)},
			{K: "xuid", V: nilIfEmpty(t.Xuid)}, {K: "at", V: t.At}}
		if t.Lobby != "" {
			o = append(o, pyjson.Pair{K: "lobby", V: t.Lobby})
		}
		tickets = append(tickets, pyjson.Pair{K: id, V: o})
	}
	lobbies := []any{}
	for _, k := range s.lobbyKeys {
		names := []any{}
		for _, n := range s.lobbies[k] {
			names = append(names, n)
		}
		lobbies = append(lobbies, []any{[]any{k.Scid, k.Hopper, k.Gsi, k.Location}, names})
	}
	reserved := pyjson.Obj{}
	rn := make([]string, 0, len(s.reserved))
	for n := range s.reserved {
		rn = append(rn, n)
	}
	sort.Strings(rn)
	for _, n := range rn {
		o := pyjson.Obj{}
		xs := make([]string, 0)
		for x := range s.reserved[n] {
			xs = append(xs, x)
		}
		sort.Strings(xs)
		for _, x := range xs {
			o = append(o, pyjson.Pair{K: x, V: s.reserved[n][x]})
		}
		reserved = append(reserved, pyjson.Pair{K: n, V: o})
	}
	leftO := pyjson.Obj{}
	lx := make([]string, 0)
	for x := range s.left {
		lx = append(lx, x)
	}
	sort.Strings(lx)
	for _, x := range lx {
		leftO = append(leftO, pyjson.Pair{K: x, V: []any{s.left[x].name, s.left[x].at}})
	}
	holds := pyjson.Obj{}
	hx := make([]string, 0)
	for x := range s.holds {
		hx = append(hx, x)
	}
	sort.Strings(hx)
	for _, x := range hx {
		holds = append(holds, pyjson.Pair{K: x, V: holdObj(s.holds[x])})
	}
	return pyjson.Obj{{K: "sessions", V: sessions}, {K: "tickets", V: tickets}, {K: "lobbies", V: lobbies}, {K: "reserved", V: reserved}, {K: "left", V: leftO}, {K: "holds", V: holds}}
}

func TestMatchesPython(t *testing.T) {
	ops, results := load(t)
	hub := &fakeHub{}
	s := New(hub)
	clock, mono := 1_700_000_000.0, 1000.0
	s.Now = func() float64 { return clock }
	s.Monotonic = func() float64 { return mono }
	n := 0
	s.NewUUID = func() string { n++; return fmt.Sprintf("00000000-0000-4000-8000-%012x", n) }
	s.NameLookup = func(x int64) string { return map[int64]string{100: "Alice", 200: "Bob"}[x] }
	defer os.Unsetenv("FH2MP_PUBLIC_HOST")
	defer os.Unsetenv("FH2MP_WORLD_CAPACITY")
	os.Unsetenv("FH2MP_WORLD_CAPACITY")
	os.Unsetenv("FH2MP_PUBLIC_HOST")
	lastTicket := map[string]string{}
	lastLobby := map[string]string{}
	bodyOf := func(o pyjson.Obj) []byte {
		b, has := o.Get("body")
		if !has || b == nil {
			return nil
		}
		if s, ok := b.(string); ok {
			return []byte(s)
		}
		text, _ := pyjson.Dumps(b, pyjson.Opts{})
		return []byte(text)
	}
	xuidOf := func(o pyjson.Obj) string { return str(field(o, "xuid")) }
	statusDoc := func(st int, doc pyjson.Obj) any {
		if doc == nil {
			return []any{int64(st), nil}
		}
		return []any{int64(st), doc}
	}
	for i, ov := range ops {
		o := ov.(pyjson.Obj)
		kind := str(field(o, "op"))
		name := str(field(o, "name"))
		hub.control = nil
		mono += 100
		var got any
		switch kind {
		case "clock":
			clock = field(o, "t").(float64)
			mono += 100
		case "pops":
			v := field(o, "value")
			hub.have = v != nil
			hub.pops = nil
			if v != nil {
				hub.pops = map[string][]string{}
				for _, p := range v.(pyjson.Obj) {
					l := []string{}
					for _, x := range p.V.([]any) {
						l = append(l, x.(string))
					}
					hub.pops[p.K] = l
				}
			}
		case "pops_connected":
			lobby := lastLobby[str(field(o, "of"))]
			x, _ := strconv.ParseInt(xuidOf(o), 10, 64)
			p := map[string][]string{}
			for k, v := range hub.pops {
				p[k] = append([]string(nil), v...)
			}
			p[lobby] = append(p[lobby], fmt.Sprintf("%x", x))
			hub.pops, hub.have = p, true
		case "maxsessions":
			if nn := field(o, "n").(int64); nn != 0 {
				s.MaxSessions = int(nn)
			} else {
				s.MaxSessions = len(s.sessions)
			}
		case "env":
			if v := field(o, "value"); v == nil {
				os.Unsetenv(str(field(o, "name")))
			} else {
				os.Setenv(str(field(o, "name")), v.(string))
			}
		case "snapshot":
			got = s.snapshot()
		case "sweep":
			s.lastSweep = 0
			s.Sweep()
			got = pyjson.Obj{{K: "sessions", V: int64(s.Count())}, {K: "tickets", V: int64(len(s.tickets))}}
		case "put":
			got = statusDoc(s.PutSession(str(field(o, "path")), bodyOf(o), xuidOf(o)))
		case "get":
			got = statusDoc(s.GetSession(str(field(o, "path")), xuidOf(o)))
		case "delete":
			got = int64(s.DeleteSession(str(field(o, "path")), xuidOf(o)))
		case "ticket":
			st, doc := s.CreateTicket(str(field(o, "path")), bodyOf(o), xuidOf(o))
			if doc != nil {
				if id, ok := doc.Get("ticketId"); ok {
					lastTicket[name] = id.(string)
				}
			}
			got = statusDoc(st, doc)
		case "getticket":
			got = statusDoc(s.GetTicket(str(field(o, "path")), xuidOf(o)))
		case "getticket_last":
			st, doc := s.GetTicket(hopperPath("FreeRoam_qos", lastTicket[str(field(o, "of"))]), xuidOf(o))
			if doc != nil {
				if tr, ok := doc.Get("targetSessionRef"); ok {
					lastLobby[name] = str(field(tr.(pyjson.Obj), "name"))
				}
			}
			got = statusDoc(st, doc)
		case "delticket_last":
			got = int64(s.DeleteTicket(hopperPath("FreeRoam_qos", lastTicket[str(field(o, "of"))]), xuidOf(o)))
		case "reserve", "reserve_lobby_of":
			lobby := ""
			if kind == "reserve_lobby_of" {
				lobby = lastLobby[str(field(o, "of"))]
			} else if v := field(o, "lobby"); v != nil {
				lobby = v.(string)
			}
			h, err := s.Reserve(xuidOf(o), lobby, str(field(o, "kind")))
			if err != nil {
				ae := err.(*auth.Error)
				var xs any
				if ae.Xuid != 0 {
					xs = strconv.FormatInt(ae.Xuid, 10)
				}
				got = pyjson.Obj{{K: "error", V: []any{int64(ae.Status), ae.Code, ae.Message, xs, ae.Detail}}}
			} else {
				got = pyjson.Obj{{K: "hold", V: holdObj(h)}}
			}
		case "current":
			got = holdObj(s.CurrentHold(xuidOf(o)))
		case "cancel":
			s.CancelHold(xuidOf(o))
		case "seats_of":
			l := []string{}
			for x := range s.Seats(lastLobby[str(field(o, "of"))], nil) {
				l = append(l, x)
			}
			sort.Strings(l)
			out := []any{}
			for _, x := range l {
				out = append(out, x)
			}
			got = out
		case "drop":
			s.DropPlayer(xuidOf(o))
		case "gamesession":
			var xs []string
			for _, x := range field(o, "xuids").([]any) {
				xs = append(xs, x.(string))
			}
			nm, tpl := s.CreateGameSession(int(field(o, "mode").(int64)), xs)
			got = []any{nm, tpl}
		case "announces":
			s.AnnounceRun(255, "run-1")
			s.AnnounceAuth(255, "10.0.0.5", "abcd")
			s.AnnounceAuth(255, "10.0.0.5", "")
			s.AnnounceAuth(0, "10.0.0.5", "")
			s.AnnounceAuth(255, "", "")
			s.AnnounceLogout(255, "abcd")
			s.AnnounceLogout(255, "")
			s.AnnounceAllow(255)
			s.AnnounceAllow(0)
			s.AnnounceRevoke(255)
		case "servers":
			got = ServersBlock()
		case "tunnel":
			doc, _ := pyjson.Parse(canon(field(o, "doc")))
			got = ForTunnel(doc.(pyjson.Obj))
		case "etag":
			got = ETag(field(o, "doc").(pyjson.Obj))
		case "count":
			got = int64(s.Count())
		default:
			t.Fatalf("op %d: unknown %s", i, kind)
		}
		want := results[i].(pyjson.Obj)
		wr := field(want, "r")
		if canon(got) != canon(wr) {
			g, w := canon(got), canon(wr)
			at := 0
			for at < len(g) && at < len(w) && g[at] == w[at] {
				at++
			}
			from := max(0, at-120)
			t.Fatalf("op %d (%s %s) differs at char %d\n got ...%.400s\nwant ...%.400s", i, kind, name, at, g[from:], w[from:])
		}
		wc := []string{}
		for _, c := range field(want, "control").([]any) {
			wc = append(wc, c.(string))
		}
		if strings.Join(hub.control, "\n") != strings.Join(wc, "\n") {
			t.Fatalf("op %d (%s %s): control datagrams %q, python %q", i, kind, name, hub.control, wc)
		}
	}
}

func hopperPath(h, id string) string {
	return "/serviceconfigs/" + DefaultScid + "/hoppers/" + h + "/tickets/" + id
}
