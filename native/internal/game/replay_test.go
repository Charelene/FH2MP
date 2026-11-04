package game

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
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
	Env      map[string]string `json:"env"`
	Roadtrip bool              `json:"roadtrip"`
	Log      []string          `json:"log"`
	Events   []traceEvent      `json:"events"`
	Out      []traceOut        `json:"out"`
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
	// The traces were recorded with the local data files (server/data is not in the public repository): without them the comparison is skipped.
	need := []string{filepath.Join("..", "..", "..", "server", "data", "trafficcars.txt")}
	for _, v := range tr.Env {
		if v == "@gamedata" {
			need = append(need, filepath.Join("..", "..", "..", "server", "data", "gamedata.sqlite"))
		}
	}
	for _, f := range need {
		if _, err := os.Stat(f); err != nil {
			t.Skip("local data file missing:", f)
		}
	}
	t.Setenv("FH2MP_CLOCK_OFFSET_MS", "5000000")
	t.Setenv("FH2MP_WEATHER_START", "1")
	t.Setenv("FH2MP_UDP_AUTH", "off")
	for k, v := range tr.Env {
		if v == "@gamedata" {
			v = filepath.Join("..", "..", "..", "server", "data", "gamedata.sqlite")
		}
		t.Setenv(k, v)
	}
	t.Setenv("FH2MP_TRAFFIC_CARS", filepath.Join("..", "..", "..", "server", "data", "trafficcars.txt"))
	clk := &fakeClock{t: 1000.0}
	sock := &fakeSock{clk: clk}
	cat, err := catalog.Load()
	if err != nil {
		t.Fatal(err)
	}
	var lastCtl string
	var gotLog []string
	logSink := func(chunk string) error {
		for _, ln := range strings.FieldsFunc(chunk, func(r rune) bool { return r == 10 }) {
			if len(ln) > 9 && ln[2] == ':' && ln[5] == ':' && ln[8] == ' ' {
				if strings.HasPrefix(ln[9:], "date ") {
					continue
				}
				gotLog = append(gotLog, ln[9:])
			} else if strings.TrimSpace(ln) != "" {
				gotLog = append(gotLog, ln)
			}
		}
		return nil
	}
	prevReliable := Reliable
	Reliable = tr.Env["FH2MP_RELIABLE"] == "1"
	t.Cleanup(func() { Reliable = prevReliable })
	hub, err := NewHub(HubConfig{Roadtrip: tr.Roadtrip, CtlSend: func(d []byte, _ Addr) { lastCtl = string(d) }, Port: 0, Clock: clk, Rng: pyrand.New(seed), Sock: sock, Out: NewLogOut(logSink), Catalog: cat,
		Sinks: &Sinks{Gamemode: func(int, []uint64) (string, string, error) { return "session-1", "template-1", nil }}})
	if err != nil {
		t.Fatal(err)
	}
	picks := 0
	hub.PickRng = func() Rand { picks++; return pyrand.New(uint64(1000 + picks)) }
	for _, ev := range tr.Events {
		clk.t = ev.T
		switch ev.Kind {
		case "dgram":
			data, _ := hex.DecodeString(ev.Hex)
			hub.Handle(data, toAddr(ev.Addr))
		case "ctl":
			lastCtl = ""
			hub.ControlText(ev.Text, &Addr{"127.0.0.1", 1})
			if lastCtl != ev.Expect {
				t.Errorf("%s: control %q replied %q, want %q", name, ev.Text, lastCtl, ev.Expect)
			}
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
	compareLogs(t, name, tr.Log, gotLog)
	t.Log(fmt.Sprintf("%s: %d inputs, %d datagrams, %d log lines compared", name, len(tr.Events), len(tr.Out), len(tr.Log)))
}

func TestJoinRelayTrace(t *testing.T)  { runTrace(t, "join_relay", 7) }
func TestControlTrace(t *testing.T)    { runTrace(t, "control", 3) }
func TestRaceTrace(t *testing.T)       { runTrace(t, "race", 11) }
func TestPartsTrace(t *testing.T)      { runTrace(t, "parts", 5) }
func TestTicketsTrace(t *testing.T)    { runTrace(t, "tickets", 9) }
func TestCarMeetTrace(t *testing.T)    { runTrace(t, "carmeet", 13) }
func TestTagTrace(t *testing.T)        { runTrace(t, "tag", 17) }
func TestBucketlistTrace(t *testing.T) { runTrace(t, "bucketlist", 19) }
func TestTrafficTrace(t *testing.T)    { runTrace(t, "traffic", 23) }
func TestSpeedTrace(t *testing.T)      { runTrace(t, "speed", 29) }
func TestCapacityTrace(t *testing.T)   { runTrace(t, "capacity", 31) }
func TestBotsTrace(t *testing.T)       { runTrace(t, "bots", 37) }
func TestRentalTrace(t *testing.T)     { runTrace(t, "rental", 41) }
func TestReliableTrace(t *testing.T)   { runTrace(t, "reliable", 43) }

func compareLogs(t *testing.T, name string, want, got []string) {
	t.Helper()
	filter := func(in []string) []string {
		var out []string
		for _, l := range in {
			if strings.HasPrefix(l, "[idle] hub listening") {
				continue
			}
			out = append(out, l)
		}
		return out
	}
	want, got = filter(want), filter(got)
	for i := 0; i < len(want) && i < len(got); i++ {
		if want[i] != got[i] {
			t.Errorf("%s: log line %d differs: want %s got %s", name, i, want[i], got[i])
			return
		}
	}
	if len(want) != len(got) {
		more, extra := want, "missing"
		if len(got) > len(want) {
			more, extra = got, "extra"
		}
		n := minInt(len(want), len(got))
		t.Errorf("%s: %d log lines, want %d; first %s line: %s", name, len(got), len(want), extra, more[n])
	}
}
func TestRoadTripTrace(t *testing.T) { runTrace(t, "roadtrip", 47) }
func TestEdgeTrace(t *testing.T)     { runTrace(t, "edge", 53) }
