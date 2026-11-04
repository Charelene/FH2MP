package game

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"

	"fh2mp/internal/catalog"
	"fh2mp/internal/pyrand"
)

type fakeClock struct{ t float64 }

func (c *fakeClock) Mono() float64 { return c.t }
func (c *fakeClock) Wall() float64 { return 1.7e9 + c.t }

type outRow struct {
	t    float64
	addr Addr
	data []byte
}

type fakeSock struct {
	clk  *fakeClock
	sent []outRow
}

func (s *fakeSock) SendTo(data []byte, addr Addr) error {
	s.sent = append(s.sent, outRow{s.clk.t, addr, append([]byte(nil), data...)})
	return nil
}

type traceEvent struct {
	Name   string  `json:"name"`
	Expect string  `json:"expect"`
	Kind   string  `json:"kind"`
	T      float64 `json:"t"`
	Addr   []any   `json:"addr"`
	Hex    string  `json:"hex"`
	Text   string  `json:"text"`
}

type traceOut struct {
	T    float64 `json:"t"`
	Addr []any   `json:"addr"`
	Hex  string  `json:"hex"`
}

type trace struct {
	Env    map[string]string `json:"env"`
	Events []traceEvent      `json:"events"`
	Out    []traceOut        `json:"out"`
}

func toAddr(a []any) Addr { return Addr{a[0].(string), int(a[1].(float64))} }

func runTrace(t *testing.T, name string, seed uint64) {
	raw, err := os.ReadFile(filepath.Join("..", "..", "testdata", "game_"+name+".json"))
	if err != nil {
		t.Skip("trace not recorded:", err)
	}
	var tr trace
	if err := json.Unmarshal(raw, &tr); err != nil {
		t.Fatal(err)
	}
	t.Setenv("FH2MP_CLOCK_OFFSET_MS", "5000000")
	t.Setenv("FH2MP_WEATHER_START", "1")
	t.Setenv("FH2MP_UDP_AUTH", "off")
	for k, v := range tr.Env {
		t.Setenv(k, v)
	}
	t.Setenv("FH2MP_TRAFFIC_CARS", filepath.Join("..", "..", "..", "server", "data", "trafficcars.txt"))
	clk := &fakeClock{t: 1000.0}
	sock := &fakeSock{clk: clk}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	hub, err := NewHub(HubConfig{Port: 0, Clock: clk, Rng: pyrand.New(seed), Sock: sock, Out: NewLogOut(func(string) error { return nil }), Catalog: cat,
		Sinks: &Sinks{Gamemode: func(int, []uint64) (string, string, error) { return "session-1", "template-1", nil }}})
	if err != nil {
		t.Fatal(err)
	}
	for _, ev := range tr.Events {
		clk.t = ev.T
		switch ev.Kind {
		case "dgram":
			data, _ := hex.DecodeString(ev.Hex)
			hub.Handle(data, toAddr(ev.Addr))
		case "ctl":
			hub.ControlText(ev.Text, nil)
		case "query":
			var got string
			switch ev.Name {
			case "pop":
				got = hub.populationsText()
			case "lobbies":
				got = jsonDumps(hub.lobbyStatus())
			case "positions":
				got = jsonDumps(hub.mapStatus())
			}
			if got != ev.Expect {
				t.Errorf("%s: query %s: want %s got %s", name, ev.Name, ev.Expect, got)
			}
		}
		hub.Step()
	}
	// compare per receiving address, in order
	want := map[Addr][]string{}
	for _, o := range tr.Out {
		a := toAddr(o.Addr)
		want[a] = append(want[a], o.Hex)
	}
	got := map[Addr][]string{}
	for _, o := range sock.sent {
		got[o.addr] = append(got[o.addr], hex.EncodeToString(o.data))
	}
	bad := 0
	for a, w := range want {
		g := got[a]
		n := len(w)
		if len(g) < n {
			n = len(g)
		}
		for i := 0; i < n; i++ {
			if w[i] != g[i] {
				t.Errorf("%s: datagram %d to %v differs\n want %s\n  got %s", name, i, a, w[i], g[i])
				bad++
				break
			}
		}
		if len(g) != len(w) {
			t.Errorf("%s: %v got %d datagrams, want %d", name, a, len(g), len(w))
			bad++
		}
		if bad > 6 {
			return
		}
	}
	for a := range got {
		if _, ok := want[a]; !ok {
			t.Errorf("%s: unexpected datagrams to %v: %d", name, a, len(got[a]))
		}
	}
	t.Log(fmt.Sprintf("%s: %d inputs, %d datagrams compared", name, len(tr.Events), len(tr.Out)))
}

func TestJoinRelayTrace(t *testing.T) { runTrace(t, "join_relay", 7) }
func TestControlTrace(t *testing.T)   { runTrace(t, "control", 3) }
func TestRaceTrace(t *testing.T)      { runTrace(t, "race", 11) }
func TestPartsTrace(t *testing.T)     { runTrace(t, "parts", 5) }
func TestTicketsTrace(t *testing.T)   { runTrace(t, "tickets", 9) }
func TestCarMeetTrace(t *testing.T)   { runTrace(t, "carmeet", 13) }
